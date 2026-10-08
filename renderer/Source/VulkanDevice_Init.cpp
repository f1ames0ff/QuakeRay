// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "VulkanDevice.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "QrException.h"
#include "Const.h"
#include "Generated/ShaderCommonC.h"
#include "LibraryConfig.h"
#include "RHI/NvrhiContext.h"
#include "RHI/NvrhiFrameSkeleton.h"
#include "RHI/NvrhiRequirements.h"
#include "RHI/RhiAccelStructs.h"
#include "RHI/RhiBloomPass.h"
#include "RHI/RhiDecalPass.h"
#include "RHI/RhiFsrPass.h"
#include "RHI/RhiPostEffectPass.h"
#include "RHI/RhiProceduralSkyPass.h"
#include "RHI/RhiCloudsPass.h"
#include "RHI/RhiRasterOverlayPass.h"
#include "RHI/RhiRasterSkyPass.h"
#include "RHI/RhiRtComposePass.h"
#include "RHI/RhiPipeline.h"
#include "RHI/RhiRtDirectPass.h"
#include "RHI/RhiRtGodRaysPass.h"
#include "RHI/RhiRtIndirectPass.h"
#include "RHI/RhiRtPrimaryPass.h"
#include "RHI/RhiRtReflRefrPass.h"
#include "RHI/RhiShadowMapPass.h"
#include "RHI/RhiFrameContext.h"
#include "RHI/RhiSkyPass.h"
#include "RHI/RhiTextureSource.h"
#include "RHI/RhiTextureTable.h"
#include "RHI/RhiUiPass.h"

using namespace qray;

VulkanDevice::VulkanDevice( const QrInstanceCreateInfo* info )
    : instance( VK_NULL_HANDLE )
    , device( VK_NULL_HANDLE )
    , surface( VK_NULL_HANDLE )
    , currentFrameState()
    , frameId( 1 )
    , waitForOutOfFrameFence( false )
    , libconfig( LibraryConfig::Read( info->pConfigPath ) )
    , debugMessenger( VK_NULL_HANDLE )
    , userPrint{ std::make_unique< UserPrint >( info->pfnPrint, info->pUserPrintData ) }
    , userFileLoad{ std::make_shared< UserFileLoad >(
          info->pfnOpenFile, info->pfnCloseFile, info->pUserLoadFileData ) }
    , rayCullBackFacingTriangles( info->rayCullBackFacingTriangles )
    , allowGeometryWithSkyFlag( info->allowGeometryWithSkyFlag )
    , rasterizedVertexColorGamma( info->rasterizedVertexColorGamma != 0 )
    , previousFrameTime( -1.0 / 60.0 )
    , currentFrameTime( 0 )
{
    ValidateCreateInfo( info );

    rhi::setShaderFileLoader( userFileLoad );

    CreateInstance( *info );

    surface = GetSurfaceFromUser( instance, *info );

    physDevice = std::make_shared< PhysicalDevice >( instance );
    queues     = std::make_shared< Queues >( physDevice->Get(), surface );

    CreateDevice();

    CreateSyncPrimitives();

    queues->SetDevice( device );

    CreateNvrhiDevice();

    {
        const uint32_t maxTextureCount =
            std::clamp(info->maxTextureCount, TEXTURE_COUNT_MIN, TEXTURE_COUNT_MAX);

        rhiFrameContext = std::make_shared<rhi::RhiFrameContext>();

        if (!rhiFrameContext->Create(nvrhi->GetDevice(), MAX_FRAMES_IN_FLIGHT))
        {
            rhiFrameContext.reset();
            Print("Warning: RHI: failed to create the frame context, the RHI renderer is unavailable");
        }
        else
        {
            rhiTextureTable = std::make_shared<rhi::RhiTextureTable>();

            if (!rhiTextureTable->Create(nvrhi->GetDevice(), maxTextureCount, rhiFrameContext.get()))
            {
                rhiTextureTable.reset();
                Print("Warning: RHI: failed to create the texture table, the RHI renderer will not see engine textures");
            }
        }
    }

    memAllocator        = std::make_shared<MemoryAllocator>(instance, device, physDevice);

    cmdManager          = std::make_shared<CommandBufferManager>(device, queues);

    uniform             = std::make_shared<GlobalUniform>(device, memAllocator);

    swapchain           = std::make_shared<Swapchain>(device, surface, physDevice->Get(), cmdManager, presentWait2Enabled);

    worldSamplerManager     = std::make_shared<SamplerManager>(device, 8, info->textureSamplerForceMinificationFilterLinear,
                                                               rhiTextureTable.get());
    genericSamplerManager   = std::make_shared<SamplerManager>(device, 0, info->textureSamplerForceMinificationFilterLinear);

    framebuffers        = std::make_shared<Framebuffers>(
        device,
        memAllocator,
        cmdManager,
        *info );

    blueNoise           = std::make_shared<BlueNoise>(
        device,
        info->pBlueNoiseFilePath,
        memAllocator,
        cmdManager,
        userFileLoad);

    textureManager      = std::make_shared<TextureManager>(
        device,
        memAllocator,
        worldSamplerManager,
        cmdManager,
        userFileLoad,
        *info,
        libconfig);

    textureManager->SetRhiTextureTable(rhiTextureTable.get());

    cubemapManager      = std::make_shared<CubemapManager>(
        device,
        memAllocator,
        genericSamplerManager,
        cmdManager,
        userFileLoad,
        *info,
        libconfig);

    worldLights         = std::make_shared<WorldLights>();
    clusterLightLists   = std::make_shared<ClusterLightLists>();

    shaderManager       = std::make_shared<ShaderManager>(
        device,
        info->pShaderFolderPath,
        userFileLoad);

    scene               = std::make_shared<Scene>(
        device,
        physDevice,
        memAllocator,
        cmdManager,
        textureManager,
        uniform,
        shaderManager);

    tonemapping         = std::make_shared<Tonemapping>(
        device,
        framebuffers,
        shaderManager,
        uniform,
        memAllocator);

    rasterizedDataCollector   = std::make_shared<RasterizedDataCollector>(
        device,
        memAllocator,
        textureManager,
        info->rasterizedMaxVertexCount,
        info->rasterizedMaxIndexCount);

    decalManager        = std::make_shared<DecalManager>(
        device,
        memAllocator,
        shaderManager,
        uniform,
        framebuffers,
        textureManager);

    portalList          = std::make_shared<PortalList>(
        device,
        memAllocator);

    rayStats            = std::make_shared<RayStats>(device, memAllocator);

    amdFsr              = std::make_shared<FidelityFX::FSR>(
        device,
        physDevice->Get(),
        userPrint.get());

    nvDlss              = std::make_shared<DLSS>(
        instance,
        device,
        physDevice->Get(),
        info->pAppGUID,
        libconfig.dlssValidation);

    shaderManager->Subscribe(decalManager);
    shaderManager->Subscribe(tonemapping);
    shaderManager->Subscribe(scene->GetVertexPreprocessing());

    framebuffers->Subscribe(decalManager);
    framebuffers->Subscribe(amdFsr);

    {
        if (nvrhi != nullptr)
        {
            rhiAccelStructs = std::make_shared<rhi::RhiAccelStructs>();
            if (!rhiAccelStructs->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                         scene->GetASManager().get(),
                                         [this](const char *pMessage) { Print(pMessage); }))
            {
                rhiAccelStructs.reset();
                Print("Warning: RHI: the acceleration structures are unavailable, the frame skeleton will be unavailable");
            }

            {
                rhiCloudsPass = std::make_shared<RhiCloudsPass>();
                if (!rhiCloudsPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                           info->pShaderFolderPath,
                                           [this](const char *pMessage) { Print(pMessage); }))
                {
                    rhiCloudsPass.reset();
                    Print("Warning: RHI: the cloud layer pass is unavailable");
                }

                rhiProceduralSkyPass = std::make_shared<RhiProceduralSkyPass>();
                if (!rhiProceduralSkyPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                                  info->pShaderFolderPath,
                                                  rhiCloudsPass != nullptr ? rhiCloudsPass->GetLayerTexture() : nullptr,
                                                  rhiCloudsPass != nullptr ? rhiCloudsPass->GetLayerSampler() : nullptr,
                                                  [this](const char *pMessage) { Print(pMessage); }))
                {
                    rhiProceduralSkyPass.reset();
                    Print("Warning: RHI: the procedural sky pass is unavailable, the RT passes keep the placeholder cubemaps");
                }

                if (rhiProceduralSkyPass != nullptr && rhiProceduralSkyPass->IsCreated())
                {
                    rhiRasterSkyPass = std::make_shared<RhiRasterSkyPass>();
                    if (!rhiRasterSkyPass->Create(nvrhi->GetDevice(), rhiTextureTable.get(),
                                                  info->pShaderFolderPath,
                                                  rhiProceduralSkyPass->GetCubemapTexture(),
                                                  [this](const char *pMessage) { Print(pMessage); }))
                    {
                        rhiRasterSkyPass.reset();
                        Print("Warning: RHI: the raster sky pass is unavailable, the raster sky cube is not written");
                    }
                }

                rhiFsrPass = std::make_shared<RhiFsrPass>();
                if (!rhiFsrPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(), amdFsr.get(),
                                        [this](const char *pMessage) { Print(pMessage); }))
                {
                    rhiFsrPass.reset();
                    Print("Warning: RHI: the FSR pass is unavailable, the TAAU upscaler is kept");
                }

                rhiPostEffectPass = std::make_shared<RhiPostEffectPass>();
                if (!rhiPostEffectPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                               info->pShaderFolderPath,
                                               info->effectWipeIsUsed != 0,
                                               [this](const char *pMessage) { Print(pMessage); }))
                {
                    rhiPostEffectPass.reset();
                    Print("Warning: RHI: the post-effect pass is unavailable, the frame is drawn without the post-upscale effects");
                }

                rhiDecalPass = std::make_shared<RhiDecalPass>();
                if (!rhiDecalPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                          rhiTextureTable.get(), info->pShaderFolderPath,
                                          [this](const char *pMessage) { Print(pMessage); }))
                {
                    rhiDecalPass.reset();
                    Print("Warning: RHI: the decal pass is unavailable, decals are skipped");
                }

                rhiRasterOverlayPass = std::make_shared<RhiRasterOverlayPass>();
                if (!rhiRasterOverlayPass->Create(nvrhi->GetDevice(), rhiTextureTable.get(),
                                                  rhiFrameContext.get(), info->pShaderFolderPath,
                                                  [this](const char *pMessage) { Print(pMessage); }))
                {
                    rhiRasterOverlayPass.reset();
                    Print("Warning: RHI: the raster overlay pass is unavailable, the frame is drawn without it");
                }

                rhiRtPrimaryPass = std::make_shared<RhiRtPrimaryPass>();
                if (!rhiRtPrimaryPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                              rhiTextureTable.get(), rayStats.get(), info->pShaderFolderPath,
                                              [this](const char *pMessage) { Print(pMessage); }))
                {
                    rhiRtPrimaryPass.reset();
                    Print("Warning: RHI: the primary ray-tracing pass is unavailable, the frame skeleton will be unavailable");
                }

                if (rhiRtPrimaryPass != nullptr)
                {
                    rhiRtDirectPass = std::make_shared<RhiRtDirectPass>();
                    if (!rhiRtDirectPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                                 rhiTextureTable.get(), scene->GetLightManager().get(),
                                                 rhiRtPrimaryPass.get(), info->pShaderFolderPath,
                                                 [this](const char *pMessage) { Print(pMessage); }))
                    {
                        rhiRtDirectPass.reset();
                        Print("Warning: RHI: the direct ray-tracing pass is unavailable, the frame skeleton will be unavailable");
                    }
                }

                if (rhiRasterOverlayPass != nullptr && rhiRtDirectPass != nullptr)
                {
                    rhiRasterOverlayPass->SetSmokeLightLayout(rhiRtDirectPass->GetLightLayout());
                }

                rhiShadowMapPass = std::make_shared<RhiShadowMapPass>();
                if (!rhiShadowMapPass->Create(nvrhi->GetDevice(), info->pShaderFolderPath,
                                              [this](const char *pMessage) { Print(pMessage); }))
                {
                    rhiShadowMapPass.reset();
                    Print("Warning: RHI: the shadow-map pass is unavailable, the god rays will be skipped");
                }

                if (rhiRtDirectPass != nullptr)
                {
                    const nvrhi::TextureHandle blueNoiseTexture = rhi::wrapEngineTextureArray(
                        nvrhi->GetDevice(),
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(blueNoise->GetImage())),
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(blueNoise->GetImageView())),
                        static_cast<uint32_t>(blueNoise->GetFormat()),
                        blueNoise->GetExtent().width, blueNoise->GetExtent().height,
                        blueNoise->GetLayerCount(), 1,
                        "RHI blue noise (indirect set 5)");

                    if (blueNoiseTexture == nullptr)
                    {
                        Print("Warning: RHI: failed to wrap the blue-noise texture, the indirect ray-tracing pass is unavailable");
                    }
                    else
                    {
                        rhiRtIndirectPass = std::make_shared<RhiRtIndirectPass>();
                        if (!rhiRtIndirectPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                                       rhiTextureTable.get(), rhiRtPrimaryPass.get(),
                                                       rhiRtDirectPass.get(), info->pShaderFolderPath,
                                                       [this](const char *pMessage) { Print(pMessage); }))
                        {
                            rhiRtIndirectPass.reset();
                            Print("Warning: RHI: the indirect ray-tracing pass is unavailable, the frame skeleton will be unavailable");
                        }
                        else
                        {
                            rhiRtIndirectPass->SetBlueNoiseTexture(blueNoiseTexture);

                            rhiRtGodRaysPass = std::make_shared<RhiRtGodRaysPass>();
                            if (!rhiRtGodRaysPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                                          info->pShaderFolderPath,
                                                          [this](const char *pMessage) { Print(pMessage); }))
                            {
                                rhiRtGodRaysPass.reset();
                                Print("Warning: RHI: the god-rays pass is unavailable, the frame is drawn without shafts");
                            }
                            else
                            {
                                rhiRtGodRaysPass->SetBlueNoiseTexture(blueNoiseTexture);
                                if (rhiShadowMapPass != nullptr && rhiShadowMapPass->IsCreated())
                                {
                                    rhiRtGodRaysPass->SetShadowMap(rhiShadowMapPass->GetTexture(),
                                                                   rhiShadowMapPass->GetSampler());
                                }
                            }
                        }
                    }
                }

                if (rhiRtPrimaryPass != nullptr)
                {
                    rhiRtReflRefrPass = std::make_shared<RhiRtReflRefrPass>();
                    if (!rhiRtReflRefrPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                                   rhiTextureTable.get(), rhiRtPrimaryPass.get(),
                                                   rhiRtDirectPass.get(),
                                                   info->pShaderFolderPath,
                                                   [this](const char *pMessage) { Print(pMessage); }))
                    {
                        rhiRtReflRefrPass.reset();
                        Print("Warning: RHI: the reflect/refract pass is unavailable, the frame is drawn without reflections");
                    }
                }

                if (rhiProceduralSkyPass != nullptr && rhiProceduralSkyPass->IsCreated())
                {
                    if (rhiRtPrimaryPass != nullptr)
                    {
                        rhiRtPrimaryPass->SetRenderCubemap(rhiProceduralSkyPass->GetCubemapTexture(),
                                                           rhiProceduralSkyPass->GetCubemapSampler());
                    }
                    if (rhiRtIndirectPass != nullptr)
                    {
                        rhiRtIndirectPass->SetRenderCubemaps(rhiProceduralSkyPass->GetCubemapTexture(),
                                                             rhiProceduralSkyPass->GetEnvironmentTexture(),
                                                             rhiProceduralSkyPass->GetCubemapSampler());
                    }
                    if (rhiRtReflRefrPass != nullptr)
                    {
                        rhiRtReflRefrPass->SetRenderCubemaps(rhiProceduralSkyPass->GetCubemapTexture(),
                                                             rhiProceduralSkyPass->GetEnvironmentTexture(),
                                                             rhiProceduralSkyPass->GetCubemapSampler());
                    }
                }

                {
                    rhiRtComposePass = std::make_shared<RhiRtComposePass>();
                    if (!rhiRtComposePass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                                  tonemapping.get(),
                                                  info->pShaderFolderPath,
                                                  [this](const char *pMessage) { Print(pMessage); }))
                    {
                        rhiRtComposePass.reset();
                        Print("Warning: RHI: the compose pass is unavailable, the diagnostic present is kept");
                    }
                }

                rhiBloomPass = std::make_shared<RhiBloomPass>();
                if (!rhiBloomPass->Create(nvrhi->GetDevice(), rhiFrameContext.get(),
                                          tonemapping.get(), info->pShaderFolderPath,
                                          [this](const char *pMessage) { Print(pMessage); }))
                {
                    rhiBloomPass.reset();
                    Print("Warning: RHI: the bloom pass is unavailable, the frame is drawn without bloom");
                }

                if (rhiRtComposePass != nullptr)
                {
                    rhiRtComposePass->SetBloomPass(rhiBloomPass.get());
                }

                rhiUiPass = std::make_shared<RhiUiPass>();
                if (!rhiUiPass->Create(nvrhi->GetDevice(), rhiTextureTable.get(),
                                       rhiFrameContext.get(), info->pShaderFolderPath,
                                       [this](const char *pMessage) { Print(pMessage); }))
                {
                    rhiUiPass.reset();
                    Print("Warning: RHI: the 2D UI pass is unavailable, the frame is drawn without the UI");
                }
            }

            const NvrhiFrameSkeleton::FrameMode frameMode = NvrhiFrameSkeleton::FrameMode::Traced;

            nvrhiFrameSkeleton = std::make_shared<NvrhiFrameSkeleton>(
                nvrhi->GetDevice(),
                swapchain.get(),
                info->pShaderFolderPath,
                rhiTextureTable.get(),
                rhiFrameContext.get(),
                rhiAccelStructs.get(),
                rhiRtPrimaryPass.get(),
                rhiRtDirectPass.get(),
                rhiRtIndirectPass.get(),
                rhiRtComposePass.get(),
                rhiRtReflRefrPass.get(),
                rhiProceduralSkyPass.get(),
                rhiCloudsPass.get(),
                rhiRasterSkyPass.get(),
                rhiRasterOverlayPass.get(),
                rhiDecalPass.get(),
                rhiFsrPass.get(),
                rhiPostEffectPass.get(),
                rhiBloomPass.get(),
                rhiShadowMapPass.get(),
                rhiRtGodRaysPass.get(),
                rhiUiPass.get(),
                frameMode,
                [this](const char *pMessage) { Print(pMessage); });

            swapchain->Subscribe(nvrhiFrameSkeleton);

            if (RhiSkyPass *skyPass = nvrhiFrameSkeleton->GetSkyPass())
            {
                const RasterizedDataCollector &collector = *rasterizedDataCollector;

                nvrhi::BufferDesc vertexBufferDesc;
                vertexBufferDesc.byteSize =
                    static_cast<uint64_t>(std::max(info->rasterizedMaxVertexCount, 64u)) * sizeof(QrVertex);
                vertexBufferDesc.isVertexBuffer = true;
                vertexBufferDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
                vertexBufferDesc.keepInitialState = true;
                vertexBufferDesc.debugName = "Rasterizer vertex buffer (RHI)";

                nvrhi::BufferDesc indexBufferDesc;
                indexBufferDesc.byteSize =
                    static_cast<uint64_t>(std::max(info->rasterizedMaxIndexCount, 64u)) * sizeof(uint32_t);
                indexBufferDesc.isIndexBuffer = true;
                indexBufferDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
                indexBufferDesc.keepInitialState = true;
                indexBufferDesc.debugName = "Rasterizer index buffer (RHI)";

                nvrhi::BufferHandle vertexBuffer = nvrhi->GetDevice()->createHandleForNativeBuffer(
                    nvrhi::ObjectTypes::VK_Buffer,
                    nvrhi::Object(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(collector.GetVertexBuffer()))),
                    vertexBufferDesc);
                nvrhi::BufferHandle indexBuffer = nvrhi->GetDevice()->createHandleForNativeBuffer(
                    nvrhi::ObjectTypes::VK_Buffer,
                    nvrhi::Object(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(collector.GetIndexBuffer()))),
                    indexBufferDesc);

                if (vertexBuffer == nullptr || indexBuffer == nullptr)
                {
                    Print("Warning: RHI: failed to wrap the rasterized geometry buffers, the sky is skipped");
                }
                else
                {
                    skyPass->SetGeometryBuffers(vertexBuffer, indexBuffer);

                    if (rhiRasterSkyPass != nullptr && rhiRasterSkyPass->IsCreated())
                    {
                        rhiRasterSkyPass->SetGeometryBuffers(vertexBuffer, indexBuffer);
                    }
                }
            }
        }
    }
}

VulkanDevice::~VulkanDevice()
{
    vkDeviceWaitIdle(device);

    rhi::setShaderFileLoader( nullptr );

    nvrhiFrameSkeleton.reset();

    rhiRtComposePass.reset();
    rhiBloomPass.reset();
    rhiRtGodRaysPass.reset();
    rhiShadowMapPass.reset();
    rhiUiPass.reset();
    rhiRtIndirectPass.reset();
    rhiRtDirectPass.reset();
    rhiRtReflRefrPass.reset();
    rhiRtPrimaryPass.reset();
    rhiRasterOverlayPass.reset();
    rhiDecalPass.reset();
    rhiFsrPass.reset();
    rhiPostEffectPass.reset();
    rhiRasterSkyPass.reset();
    rhiProceduralSkyPass.reset();
    rhiCloudsPass.reset();
    rhiAccelStructs.reset();

    rhiTextureTable.reset();

    rhiFrameContext.reset();

    nvrhi.reset();

    physDevice.reset();
    queues.reset();
    swapchain.reset();
    cmdManager.reset();
    framebuffers.reset();
    tonemapping.reset();
    amdFsr.reset();
    nvDlss.reset();
    uniform.reset();
    scene.reset();
    shaderManager.reset();
    rasterizedDataCollector.reset();
    decalManager.reset();
    portalList.reset();
    worldSamplerManager.reset();
    genericSamplerManager.reset();
    blueNoise.reset();
    textureManager.reset();
    cubemapManager.reset();

    rayStats.reset();

    memAllocator.reset();

    vkDestroySurfaceKHR(instance, surface, nullptr);
    DestroySyncPrimitives();

    DestroyDevice();
    DestroyInstance();
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void *pUserData)
{
    if (pUserData == nullptr)
    {
        return VK_FALSE;
    }

    if (pCallbackData->messageIdNumber == 2044605652)
    {
        return VK_FALSE;
    }

    const char *msg;

    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
    {
        msg = "Vulkan::VERBOSE::[%d][%s]\n%s\n\n";
    }
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
    {
        msg = "Vulkan::INFO::[%d][%s]\n%s\n\n";
    }
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        msg = "Vulkan::WARNING::[%d][%s]\n%s\n\n";
    }
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        msg = "Vulkan::ERROR::[%d][%s]\n%s\n\n";
    }
    else
    {
        msg = "Vulkan::[%d][%s]\n%s\n\n";
    }

    char buf[1024];
    snprintf(buf, sizeof(buf) / sizeof(buf[0]), msg, pCallbackData->messageIdNumber, pCallbackData->pMessageIdName, pCallbackData->pMessage);

    auto *userPrint = static_cast<UserPrint*>(pUserData);
    userPrint->Print(buf);

    return VK_FALSE;
}

void VulkanDevice::CreateInstance(const QrInstanceCreateInfo &info)
{
    std::vector<const char *> layerNames;

    if (libconfig.vulkanValidation)
    {
        layerNames.push_back("VK_LAYER_KHRONOS_validation");
    }

    if (libconfig.fpsMonitor)
    {
        layerNames.push_back("VK_LAYER_LUNARG_monitor");
    }

    std::vector<VkExtensionProperties> supportedInstanceExtensions;
    uint32_t supportedExtensionsCount;

    if (vkEnumerateInstanceExtensionProperties(nullptr, &supportedExtensionsCount, nullptr) == VK_SUCCESS)
    {
        supportedInstanceExtensions.resize(supportedExtensionsCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &supportedExtensionsCount, supportedInstanceExtensions.data());
    }

    const bool surfaceCapabilities2Supported = std::any_of(supportedInstanceExtensions.cbegin(), supportedInstanceExtensions.cend(),
        [](const VkExtensionProperties& ext)
        {
            return !std::strcmp(ext.extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        });

    std::vector<const char *> extensions =
    {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

    if (surfaceCapabilities2Supported)
    {
        extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    }

    if (libconfig.vulkanValidation)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    }

    for (const char *n : DLSS::GetDlssVulkanInstanceExtensions())
    {
        const bool isSupported = std::any_of(supportedInstanceExtensions.cbegin(), supportedInstanceExtensions.cend(),
            [&](const VkExtensionProperties& ext)
            {
                return !std::strcmp(ext.extensionName, n);
            }
        );

        if (!isSupported)
        {
            continue;
        }

        extensions.push_back(n);
    }

    enabledInstanceExtensions.clear();
    for (const char *n : extensions)
    {
        enabledInstanceExtensions.push_back(n);
    }

    VkApplicationInfo appInfo = {};
    appInfo.apiVersion = VK_API_VERSION_1_3;
    appInfo.pApplicationName = info.pAppName;

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.ppEnabledLayerNames = layerNames.data();
    instanceInfo.enabledLayerCount = layerNames.size();
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    instanceInfo.enabledExtensionCount = extensions.size();

    VkResult r = vkCreateInstance(&instanceInfo, nullptr, &instance);
    VK_CHECKERROR(r);

    if (surfaceCapabilities2Supported)
    {
        InitInstanceExtensionFunctions_SurfaceCapabilities2(instance);
    }

    if (libconfig.vulkanValidation)
    {
        InitInstanceExtensionFunctions_DebugUtils(instance);

        if (userPrint)
        {
            VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo = {};
            debugMessengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugMessengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugMessengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            debugMessengerInfo.pfnUserCallback = DebugMessengerCallback;
            debugMessengerInfo.pUserData = static_cast<void *>(userPrint.get());

            r = svkCreateDebugUtilsMessengerEXT(instance, &debugMessengerInfo, nullptr, &debugMessenger);
            VK_CHECKERROR(r);
        }
    }
}

void VulkanDevice::CreateDevice()
{
    VkPhysicalDeviceFeatures features = {};
    features.robustBufferAccess = 1;
    features.fullDrawIndexUint32 = 1;
    features.imageCubeArray = 1;
    features.independentBlend = 1;
    features.geometryShader = 0;
    features.tessellationShader = 0;
    features.sampleRateShading = 0;
    features.dualSrcBlend = 0;
    features.logicOp = 1;
    features.multiDrawIndirect = 1;
    features.drawIndirectFirstInstance = 1;
    features.depthClamp = 1;
    features.depthBiasClamp = 1;
    features.fillModeNonSolid = 0;
    features.depthBounds = 1;
    features.wideLines = 0;
    features.largePoints = 0;
    features.alphaToOne = 0;
    features.multiViewport = 1;
    features.samplerAnisotropy = 1;
    features.textureCompressionETC2 = 0;
    features.textureCompressionASTC_LDR = 0;
    features.textureCompressionBC = 0;
    features.occlusionQueryPrecise = 0;
    features.pipelineStatisticsQuery = 1;
    features.vertexPipelineStoresAndAtomics = 1;
    features.fragmentStoresAndAtomics = 1;
    features.shaderTessellationAndGeometryPointSize = 1;
    features.shaderImageGatherExtended = 1;
    features.shaderStorageImageExtendedFormats = 1;
    features.shaderStorageImageMultisample = 1;
    features.shaderStorageImageReadWithoutFormat = 1;
    features.shaderStorageImageWriteWithoutFormat = 1;
    features.shaderUniformBufferArrayDynamicIndexing = 1;
    features.shaderSampledImageArrayDynamicIndexing = 1;
    features.shaderStorageBufferArrayDynamicIndexing = 1;
    features.shaderStorageImageArrayDynamicIndexing = 1;
    features.shaderClipDistance = 1;
    features.shaderCullDistance = 1;
    features.shaderFloat64 = 1;
    features.shaderInt64 = 1;
    features.shaderInt16 = 1;
    features.shaderResourceResidency = 1;
    features.shaderResourceMinLod = 1;
    features.sparseBinding = 0;
    features.sparseResidencyBuffer = 0;
    features.sparseResidencyImage2D = 0;
    features.sparseResidencyImage3D = 0;
    features.sparseResidency2Samples = 0;
    features.sparseResidency4Samples = 0;
    features.sparseResidency8Samples = 0;
    features.sparseResidency16Samples = 0;
    features.sparseResidencyAliased = 0;
    features.variableMultisampleRate = 0;
    features.inheritedQueries = 1;

    VkPhysicalDeviceVulkan12Features vulkan12Features = {};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12Features.samplerMirrorClampToEdge = 1;
    vulkan12Features.runtimeDescriptorArray = 1;
    vulkan12Features.shaderSampledImageArrayNonUniformIndexing = 1;
    vulkan12Features.shaderStorageBufferArrayNonUniformIndexing = 1;
    vulkan12Features.bufferDeviceAddress = 1;
    vulkan12Features.shaderFloat16 = 1;
    vulkan12Features.drawIndirectCount = 1;

    const NvrhiRequirements nvrhiRequirements = QueryNvrhiRequirements(physDevice->Get());
    const std::vector<std::string> unsupportedNvrhiFeatures = nvrhiRequirements.GetUnsupported();

    if (!nvrhiRequirements.IsCriticalSupported())
    {
        std::string message = "RHI: the device does not support the required features:";
        for (const std::string &name : unsupportedNvrhiFeatures)
        {
            message += " ";
            message += name;
        }

        throw QrException(QR_GRAPHICS_API_ERROR, message);
    }

    ApplyNvrhiRequirements(nvrhiRequirements, vulkan12Features);

    if (!unsupportedNvrhiFeatures.empty())
    {
        std::string message = "RHI: features not supported by the device:";
        for (const std::string &name : unsupportedNvrhiFeatures)
        {
            message += " ";
            message += name;
        }
        message += "\n";

        Print(message.c_str());
    }

    VkPhysicalDeviceVulkan13Features vulkan13Features = {};
    vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13Features.pNext = nullptr;
    vulkan13Features.computeFullSubgroups = 1;
    vulkan13Features.subgroupSizeControl = 1;
    vulkan13Features.dynamicRendering = 1;
    vulkan13Features.synchronization2 = 1;

    vulkan12Features.pNext = &vulkan13Features;

    VkPhysicalDeviceMultiviewFeatures multiviewFeatures = {};
    multiviewFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    multiviewFeatures.pNext = &vulkan12Features;
    multiviewFeatures.multiview = 1;

    VkPhysicalDevice16BitStorageFeatures storage16 = {};
    storage16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    storage16.pNext = &multiviewFeatures;
    storage16.storageBuffer16BitAccess = 1;

    std::vector<VkExtensionProperties> supportedDeviceExtensions;
    uint32_t supportedExtensionsCount;

    if (vkEnumerateDeviceExtensionProperties(physDevice->Get(), nullptr, &supportedExtensionsCount, nullptr) == VK_SUCCESS)
    {
        supportedDeviceExtensions.resize(supportedExtensionsCount);
        vkEnumerateDeviceExtensionProperties(physDevice->Get(), nullptr, &supportedExtensionsCount, supportedDeviceExtensions.data());
    }

    const bool rayQuerySupported = std::any_of(supportedDeviceExtensions.cbegin(), supportedDeviceExtensions.cend(),
        [](const VkExtensionProperties& ext)
        {
            return !std::strcmp(ext.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME);
        });

    const bool presentId2ExtensionSupported = std::any_of(supportedDeviceExtensions.cbegin(), supportedDeviceExtensions.cend(),
        [](const VkExtensionProperties& ext)
        {
            return !std::strcmp(ext.extensionName, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
        });

    const bool presentWait2ExtensionSupported = std::any_of(supportedDeviceExtensions.cbegin(), supportedDeviceExtensions.cend(),
        [](const VkExtensionProperties& ext)
        {
            return !std::strcmp(ext.extensionName, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
        });

    VkPhysicalDevicePresentId2FeaturesKHR presentId2Features = {};
    presentId2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;

    VkPhysicalDevicePresentWait2FeaturesKHR presentWait2Features = {};
    presentWait2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR;
    presentWait2Features.pNext = &presentId2Features;

    bool presentWait2Supported = false;
    if (presentId2ExtensionSupported && presentWait2ExtensionSupported)
    {
        VkPhysicalDeviceFeatures2 presentWaitFeatures2 = {};
        presentWaitFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        presentWaitFeatures2.pNext = &presentWait2Features;
        vkGetPhysicalDeviceFeatures2(physDevice->Get(), &presentWaitFeatures2);

        presentWait2Supported = presentId2Features.presentId2 && presentWait2Features.presentWait2 &&
                                sVkGetPhysicalDeviceSurfaceCapabilities2KHR != nullptr;
    }

    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = {};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQueryFeatures.pNext = &storage16;
    rayQueryFeatures.rayQuery = rayQuerySupported ? 1 : 0;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures = {};
    rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtPipelineFeatures.pNext = rayQuerySupported ? static_cast<void *>(&rayQueryFeatures) : static_cast<void *>(&storage16);
    rtPipelineFeatures.rayTracingPipeline = 1;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures = {};
    asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.pNext = &rtPipelineFeatures;
    asFeatures.accelerationStructure = 1;

    VkPhysicalDeviceFeatures2 physicalDeviceFeatures2 = {};
    physicalDeviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    physicalDeviceFeatures2.pNext = &asFeatures;
    physicalDeviceFeatures2.features = features;

    if (presentWait2Supported)
    {
        presentId2Features.pNext = physicalDeviceFeatures2.pNext;
        presentWait2Features.pNext = &presentId2Features;
        physicalDeviceFeatures2.pNext = &presentWait2Features;
    }

    std::vector<const char *> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    };

    for (const char *n : DLSS::GetDlssVulkanDeviceExtensions())
    {
        const bool isSupported = std::any_of(supportedDeviceExtensions.cbegin(), supportedDeviceExtensions.cend(),
            [&](const VkExtensionProperties& ext)
            {
                return !std::strcmp(ext.extensionName, n);
            }
        );

        if (!isSupported)
        {
            continue;
        }

        deviceExtensions.push_back(n);
    }

    if (rayQuerySupported)
    {
        deviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }

    if (presentWait2Supported)
    {
        deviceExtensions.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
        deviceExtensions.push_back(VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
    }

    enabledDeviceExtensions.clear();
    for (const char *n : deviceExtensions)
    {
        enabledDeviceExtensions.push_back(n);
    }

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queues->GetDeviceQueueCreateInfos(queueCreateInfos);

    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());;
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceCreateInfo.pEnabledFeatures = nullptr;
    deviceCreateInfo.pNext = &physicalDeviceFeatures2;
    deviceCreateInfo.enabledExtensionCount = (uint32_t) deviceExtensions.size();
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VkResult r = vkCreateDevice(physDevice->Get(), &deviceCreateInfo, nullptr, &device);
    VK_CHECKERROR(r);

    InitDeviceExtensionFunctions(device);

    presentWait2Enabled = presentWait2Supported && InitDeviceExtensionFunctions_PresentWait2(device);

    if (libconfig.vulkanValidation)
    {
        InitDeviceExtensionFunctions_DebugUtils(device);
    }
}

void VulkanDevice::CreateNvrhiDevice()
{
    const NvrhiRequirements requirements = QueryNvrhiRequirements(physDevice->Get());

    std::vector<const char *> instanceExtensions;
    instanceExtensions.reserve(enabledInstanceExtensions.size());
    for (const std::string &name : enabledInstanceExtensions)
    {
        instanceExtensions.push_back(name.c_str());
    }

    std::vector<const char *> deviceExtensions;
    deviceExtensions.reserve(enabledDeviceExtensions.size());
    for (const std::string &name : enabledDeviceExtensions)
    {
        deviceExtensions.push_back(name.c_str());
    }

    NvrhiDeviceInfo info = {};
    info.instance = instance;
    info.physicalDevice = physDevice->Get();
    info.device = device;

    info.graphicsQueue = queues->GetGraphics();
    info.graphicsQueueIndex = queues->GetIndexGraphics();
    info.computeQueue = queues->GetCompute();
    info.computeQueueIndex = queues->GetIndexCompute();
    info.transferQueue = queues->GetTransfer();
    info.transferQueueIndex = queues->GetIndexTransfer();

    info.instanceExtensions = instanceExtensions.data();
    info.instanceExtensionCount = instanceExtensions.size();
    info.deviceExtensions = deviceExtensions.data();
    info.deviceExtensionCount = deviceExtensions.size();

    info.bufferDeviceAddressSupported = true;
    info.uniformBufferUpdateAfterBindSupported = requirements.descriptorBindingUniformBufferUpdateAfterBind;

    nvrhi = std::make_unique<NvrhiContext>();

    std::string errorMessage;
    if (!nvrhi->Init(info, [this](const char *pMessage) { Print(pMessage); }, errorMessage))
    {
        nvrhi.reset();
        throw QrException(QR_GRAPHICS_API_ERROR, errorMessage);
    }

    nvrhi->LogCapabilities();
}

void VulkanDevice::CreateSyncPrimitives()
{
    VkResult r;

    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkFenceCreateInfo nonSignaledFenceInfo = {};
    nonSignaledFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        r = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]);
        VK_CHECKERROR(r);
        r = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &inFrameSemaphores[i]);
        VK_CHECKERROR(r);

        r = vkCreateFence(device, &fenceInfo, nullptr, &frameFences[i]);
        VK_CHECKERROR(r);
        r = vkCreateFence(device, &nonSignaledFenceInfo, nullptr, &outOfFrameFences[i]);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, imageAvailableSemaphores[i], VK_OBJECT_TYPE_SEMAPHORE, "Image available semaphore");
        SET_DEBUG_NAME(device, inFrameSemaphores[i], VK_OBJECT_TYPE_SEMAPHORE, "In-frame semaphore");
        SET_DEBUG_NAME(device, frameFences[i], VK_OBJECT_TYPE_FENCE, "Frame fence");
        SET_DEBUG_NAME(device, outOfFrameFences[i], VK_OBJECT_TYPE_FENCE, "Out of frame fence");
    }
}

VkSurfaceKHR VulkanDevice::GetSurfaceFromUser(VkInstance instance, const QrInstanceCreateInfo &info)
{
    VkSurfaceKHR surface;
    VkResult r;

    if (info.pWin32SurfaceInfo != nullptr)
    {
        VkWin32SurfaceCreateInfoKHR win32Info = {};
        win32Info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        win32Info.hinstance = info.pWin32SurfaceInfo->hinstance;
        win32Info.hwnd = info.pWin32SurfaceInfo->hwnd;

        r = vkCreateWin32SurfaceKHR(instance, &win32Info, nullptr, &surface);
        VK_CHECKERROR(r);

        return surface;
    }

    throw QrException(QR_WRONG_ARGUMENT, "Surface info wasn't specified");
}

void VulkanDevice::DestroyInstance()
{
    if (debugMessenger != VK_NULL_HANDLE)
    {
        svkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
    }

    vkDestroyInstance(instance, nullptr);
}

void VulkanDevice::DestroyDevice()
{
    vkDestroyDevice(device, nullptr);
}

void VulkanDevice::DestroySyncPrimitives()
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
        vkDestroySemaphore(device, inFrameSemaphores[i], nullptr);

        vkDestroyFence(device, frameFences[i], nullptr);
        vkDestroyFence(device, outOfFrameFences[i], nullptr);
    }
}

void VulkanDevice::ValidateCreateInfo(const QrInstanceCreateInfo *pInfo)
{
    using namespace std::string_literals;

    if (pInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    {
        if (pInfo->pWin32SurfaceInfo == nullptr)
        {
            throw QrException(QR_WRONG_ARGUMENT, "The Win32 surface info must not be null");
        }
    }

    if (pInfo->rasterizedSkyCubemapSize == 0)
    {
        throw QrException(QR_WRONG_ARGUMENT, "rasterizedSkyCubemapSize must be non-zero");
    }

    if (pInfo->primaryRaysMaxAlbedoLayers > MATERIALS_MAX_LAYER_COUNT)
    {
        throw QrException(QR_WRONG_ARGUMENT, "primaryRaysMaxAlbedoLayers must be <="s + std::to_string(MATERIALS_MAX_LAYER_COUNT));
    }

    if (pInfo->indirectIlluminationMaxAlbedoLayers > MATERIALS_MAX_LAYER_COUNT)
    {
        throw QrException(QR_WRONG_ARGUMENT, "indirectIlluminationMaxAlbedoLayers must be <="s + std::to_string(MATERIALS_MAX_LAYER_COUNT));
    }
}
