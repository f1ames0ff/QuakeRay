// Copyright (c) 2026 QuakeRay contributors
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

#include "FSR.h"

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include "RenderResolutionHelper.h"
#include "QrException.h"

#include <Windows.h>
#include <cstdio>
#include <vector>

namespace
{
    void FsrMessageCallback(uint32_t type, const wchar_t* msg)
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "[FSR] type=%u: %S\n", type, msg);
        OutputDebugStringA(buf);
    }

    FfxApiSurfaceFormat MapVkFormat(VkFormat fmt)
    {
        struct FormatMapping
        {
            VkFormat vkFormat;
            FfxApiSurfaceFormat ffxFormat;
        };

        static const FormatMapping mappings[] =
        {
            { VK_FORMAT_R16G16B16A16_SFLOAT,        FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT },
            { VK_FORMAT_R32G32B32A32_SFLOAT,        FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT },
            { VK_FORMAT_R32G32_SFLOAT,              FFX_API_SURFACE_FORMAT_R32G32_FLOAT },
            { VK_FORMAT_R32_SFLOAT,                 FFX_API_SURFACE_FORMAT_R32_FLOAT },
            { VK_FORMAT_R16G16_SFLOAT,              FFX_API_SURFACE_FORMAT_R16G16_FLOAT },
            { VK_FORMAT_R16_SFLOAT,                 FFX_API_SURFACE_FORMAT_R16_FLOAT },
            { VK_FORMAT_R8G8B8A8_UNORM,             FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM },
            { VK_FORMAT_B8G8R8A8_UNORM,             FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM },
            { VK_FORMAT_B10G11R11_UFLOAT_PACK32,    FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT },
            { VK_FORMAT_A2B10G10R10_UNORM_PACK32,   FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM },
            { VK_FORMAT_R16G16_UINT,                FFX_API_SURFACE_FORMAT_R16G16_UINT },
        };

        for (const FormatMapping &mapping : mappings)
        {
            if (mapping.vkFormat == fmt)
            {
                return mapping.ffxFormat;
            }
        }

        return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }

    constexpr qray::FramebufferImageIndex OUTPUT_IMAGE_INDEX = qray::FB_IMAGE_INDEX_UPSCALED_PONG;

    FfxApiResource ToFfxApiResource(
        qray::FramebufferImageIndex fbImage, uint32_t frameIndex,
        const qray::Framebuffers& framebuffers,
        const qray::ResolutionState& resolutionState)
    {
        auto [image, view, format, sz] = framebuffers.GetImageHandles(fbImage, frameIndex, resolutionState);
        const bool isOutput = fbImage == OUTPUT_IMAGE_INDEX;

        FfxApiResource res = {};
        res.resource = (void*)image;
        res.description.type     = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        res.description.format   = MapVkFormat(format);
        res.description.width    = sz.width;
        res.description.height   = sz.height;
        res.description.depth    = 1;
        res.description.mipCount = 1;
        res.description.flags    = FFX_API_RESOURCE_FLAGS_NONE;
        res.description.usage    = isOutput ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
        res.state                = isOutput ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        return res;
    }

    template <size_t N>
    void InsertBarriers(
        VkCommandBuffer cmd, uint32_t frameIndex,
        qray::Framebuffers& framebuffers,
        const qray::FramebufferImageIndex (&inputsAndOutput)[N],
        bool isBackwards)
    {
        assert(std::find(std::begin(inputsAndOutput), std::end(inputsAndOutput), OUTPUT_IMAGE_INDEX) != std::end(inputsAndOutput));

        VkImageMemoryBarrier2 barriers[N];
        for (size_t i = 0; i < N; i++)
        {
            const bool isOutput = inputsAndOutput[i] == OUTPUT_IMAGE_INDEX;

            VkImageMemoryBarrier2& b = barriers[i];
            b = {};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = isOutput ? VK_ACCESS_2_SHADER_WRITE_BIT : VK_ACCESS_2_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = isOutput ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = framebuffers.GetImage(inputsAndOutput[i], frameIndex);
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.baseMipLevel = 0;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.baseArrayLayer = 0;
            b.subresourceRange.layerCount = 1;

            if (isBackwards)
            {
                std::swap(b.srcStageMask, b.dstStageMask);
                std::swap(b.srcAccessMask, b.dstAccessMask);
                std::swap(b.oldLayout, b.newLayout);
                std::swap(b.srcQueueFamilyIndex, b.dstQueueFamilyIndex);
            }
        }

        VkDependencyInfoKHR depInfo = {};
        depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
        depInfo.imageMemoryBarrierCount = static_cast<uint32_t>(N);
        depInfo.pImageMemoryBarriers = barriers;

        qray::svkCmdPipelineBarrier2KHR(cmd, &depInfo);
    }
}

qray::FidelityFX::FSR::FSR(VkDevice _device, VkPhysicalDevice _physDevice, UserPrint* pUserPrint)
    : m_device(_device)
    , m_physDevice(_physDevice)
    , m_pUserPrint(pUserPrint)
    , m_context(nullptr)
    , m_requestedTechnique(QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3)
    , m_renderWidth(0)
    , m_renderHeight(0)
    , m_displayWidth(0)
    , m_displayHeight(0)
    , m_hasSize(false)
{
}

qray::FidelityFX::FSR::~FSR()
{
    DestroyContext();
}

void qray::FidelityFX::FSR::SetUpscaleVersion(QrRenderUpscaleTechnique technique)
{
    const bool isFsrRequested = technique == QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3;

    if (!isFsrRequested)
    {
        m_requestedTechnique = technique;

        if (m_context)
        {
            vkDeviceWaitIdle(m_device);
            DestroyContext();
        }

        return;
    }

    if (technique == m_requestedTechnique && m_context)
    {
        return;
    }

    vkDeviceWaitIdle(m_device);

    m_requestedTechnique = technique;
    RecreateContext();
}

void qray::FidelityFX::FSR::OnFramebuffersSizeChange(const ResolutionState& resolutionState)
{
    m_renderWidth  = resolutionState.renderWidth;
    m_renderHeight = resolutionState.renderHeight;
    m_displayWidth  = resolutionState.upscaledWidth;
    m_displayHeight = resolutionState.upscaledHeight;
    m_hasSize = true;

    if (m_requestedTechnique == QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3)
    {
        RecreateContext();
    }
    else
    {
        DestroyContext();
    }
}

uint64_t qray::FidelityFX::FSR::FindVersionId()
{
    ffxQueryDescGetVersions q = {};
    q.header.type       = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType    = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device            = nullptr;
    uint64_t count      = 0;
    q.outputCount       = &count;

    if (ffxQuery(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0)
    {
        return 0;
    }

    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds   = ids.data();
    q.versionNames = names.data();

    if (ffxQuery(nullptr, &q.header) != FFX_API_RETURN_OK)
    {
        return 0;
    }

    for (uint64_t i = 0; i < count; i++)
    {
        const char* name = names[i] ? names[i] : "";

        if (name[0] == '3')
        {
            return ids[i];
        }
    }

    return 0;
}

bool qray::FidelityFX::FSR::IsUpscaleVersionAvailable(QrRenderUpscaleTechnique technique)
{
    switch (technique)
    {
        case QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3:
            return FindVersionId() != 0;
        default:
            return false;
    }
}

void qray::FidelityFX::FSR::RecreateContext()
{
    DestroyContext();

    if (!m_hasSize)
    {
        return;
    }

    uint64_t versionId = FindVersionId();

    if (versionId == 0)
    {
        const char* msg = "FSR: no FSR provider found in the FidelityFX DLL";
        OutputDebugStringA(msg);
        if (m_pUserPrint)
        {
            m_pUserPrint->Print(msg);
        }
        return;
    }

    ffxOverrideVersion overrideDesc = {};
    overrideDesc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    overrideDesc.versionId   = versionId;

    ffxCreateBackendVKDesc backendDesc = {};
    backendDesc.header.type      = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backendDesc.vkDevice         = m_device;
    backendDesc.vkPhysicalDevice = m_physDevice;
    backendDesc.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescUpscale upscaleDesc = {};
    upscaleDesc.header.type   = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscaleDesc.header.pNext  = &overrideDesc.header;
    overrideDesc.header.pNext = &backendDesc.header;
    upscaleDesc.flags         = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    upscaleDesc.maxRenderSize  = { m_renderWidth, m_renderHeight };
    upscaleDesc.maxUpscaleSize = { m_displayWidth, m_displayHeight };
    upscaleDesc.fpMessage      = FsrMessageCallback;

    ffxReturnCode_t r = ffxCreateContext(&m_context, &upscaleDesc.header, nullptr);
    if (r != FFX_API_RETURN_OK)
    {
        m_context = nullptr;
        throw QrException(QR_GRAPHICS_API_ERROR, "Failed to create FSR context");
    }

    s_contextForJitter = m_context;

    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    ffxQuery(&m_context, &pv.header);

    char buf[256];
    snprintf(buf, sizeof(buf), "FSR: requested FSR 3.1, provider \"%s\" (id=0x%llx)",
        pv.versionName ? pv.versionName : "unknown",
        (unsigned long long)pv.versionId);
    OutputDebugStringA(buf);
    if (m_pUserPrint)
    {
        m_pUserPrint->Print(buf);
    }
}

void qray::FidelityFX::FSR::DestroyContext()
{
    if (m_context)
    {
        ffxDestroyContext(&m_context, nullptr);
        m_context = nullptr;
    }

    s_contextForJitter = nullptr;
}

qray::FramebufferImageIndex qray::FidelityFX::FSR::Apply(
    VkCommandBuffer cmd, uint32_t frameIndex,
    const std::shared_ptr<Framebuffers>& framebuffers,
    const RenderResolutionHelper& renderResolution,
    QrFloat2D jitterOffset,
    float timeDelta,
    float nearPlane, float farPlane, float fovVerticalRad,
    bool resetAccumulation)
{
    if (!m_context)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            OutputDebugStringA("[FSR] Apply SKIPPED - no context\n");
        }
        return FB_IMAGE_INDEX_FINAL;
    }

    using FI = qray::FramebufferImageIndex;

    FI rs[] =
    {
        FI::FB_IMAGE_INDEX_FINAL,
        FI::FB_IMAGE_INDEX_DEPTH_NDC,
        FI::FB_IMAGE_INDEX_MOTION_DLSS,
        OUTPUT_IMAGE_INDEX
    };
    InsertBarriers(cmd, frameIndex, *framebuffers, rs, false);

    const qray::ResolutionState& resolutionState = renderResolution.GetResolutionState();

    ffxDispatchDescUpscale info = {};
    info.header.type       = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    info.commandList       = cmd;
    info.color             = ToFfxApiResource(FI::FB_IMAGE_INDEX_FINAL,       frameIndex, *framebuffers, resolutionState);
    info.depth             = ToFfxApiResource(FI::FB_IMAGE_INDEX_DEPTH_NDC,   frameIndex, *framebuffers, resolutionState);
    info.motionVectors     = ToFfxApiResource(FI::FB_IMAGE_INDEX_MOTION_DLSS, frameIndex, *framebuffers, resolutionState);
    info.exposure          = {};
    info.reactive          = {};
    info.transparencyAndComposition = {};
    info.output            = ToFfxApiResource(OUTPUT_IMAGE_INDEX, frameIndex, *framebuffers, resolutionState);
    info.jitterOffset.x    = jitterOffset.data[0];
    info.jitterOffset.y    = jitterOffset.data[1];
    info.motionVectorScale.x = static_cast<float>(resolutionState.renderWidth);
    info.motionVectorScale.y = static_cast<float>(resolutionState.renderHeight);
    info.renderSize.width  = resolutionState.renderWidth;
    info.renderSize.height = resolutionState.renderHeight;
    info.upscaleSize.width  = m_displayWidth;
    info.upscaleSize.height = m_displayHeight;
    info.enableSharpening  = false;
    info.sharpness         = 0.0f;
    info.frameTimeDelta    = timeDelta * 1000.0f;
    info.preExposure       = 1.0f;
    info.reset             = resetAccumulation;
    info.cameraNear        = nearPlane;
    info.cameraFar         = farPlane;
    info.cameraFovAngleVertical = fovVerticalRad;
    info.viewSpaceToMetersFactor = 1.0f;
    info.flags             = 0;

    ffxReturnCode_t r = ffxDispatch(&m_context, &info.header);
    if (r != FFX_API_RETURN_OK)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "[FSR] ffxDispatch FAILED: r=%u\n", (unsigned)r);
        OutputDebugStringA(buf);
        return FB_IMAGE_INDEX_FINAL;
    }

    InsertBarriers(cmd, frameIndex, *framebuffers, rs, true);

    return OUTPUT_IMAGE_INDEX;
}

QrFloat2D qray::FidelityFX::FSR::GetJitter(const ResolutionState& resolutionState, uint32_t frameId)
{
    if (!s_contextForJitter)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            OutputDebugStringA("[FSR] GetJitter SKIPPED - no context\n");
        }
        return { 0, 0 };
    }

    ffxQueryDescUpscaleGetJitterPhaseCount phaseCountDesc = {};
    phaseCountDesc.header.type     = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    phaseCountDesc.renderWidth     = resolutionState.renderWidth;
    phaseCountDesc.displayWidth    = resolutionState.upscaledWidth;
    int32_t phaseCount = 0;
    phaseCountDesc.pOutPhaseCount  = &phaseCount;
    ffxQuery(&s_contextForJitter, &phaseCountDesc.header);

    if (phaseCount <= 0)
    {
        return { 0, 0 };
    }

    QrFloat2D jitter = {};
    ffxQueryDescUpscaleGetJitterOffset offsetDesc = {};
    offsetDesc.header.type  = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    offsetDesc.index        = static_cast<int32_t>(frameId % phaseCount);
    offsetDesc.phaseCount   = phaseCount;
    offsetDesc.pOutX        = &jitter.data[0];
    offsetDesc.pOutY        = &jitter.data[1];
    ffxQuery(&s_contextForJitter, &offsetDesc.header);

    return jitter;
}
