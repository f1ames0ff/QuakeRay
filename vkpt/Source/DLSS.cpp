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

#include "DLSS.h"

#include <regex>

#include "vkpt/vkpt.h"
#include "CmdLabel.h"
#include "RenderResolutionHelper.h"

#ifdef RG_USE_NVIDIA_DLSS

#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers.h>

#if __linux__
#include <unistd.h>
#include <linux/limits.h>
#endif

static void PrintCallback(const char *message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent)
{
    printf("DLSS (sourceComponent = %d): %s \n", sourceComponent, message);
}

static std::wstring GetFolderPath()
{
#if defined(_WIN32)
    wchar_t appPath[MAX_PATH];
    GetModuleFileNameW(NULL, appPath, MAX_PATH);
#elif defined(__linux__)
    wchar_t appPath[PATH_MAX];
    char appPath_c[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", appPath_c, PATH_MAX);
    std::mbstowcs(appPath, appPath_c, PATH_MAX);
#endif

    std::wstring curFolderPath = appPath;
    auto p = curFolderPath.find_last_of(L"\\/");
    return curFolderPath.substr(0, p);
}

vkpt::DLSS::DLSS(
    VkInstance _instance,
    VkDevice _device,
    VkPhysicalDevice _physDevice,
    const char *_pAppGuid,
    bool _enableDebug)
    : device(_device)
    , isInitialized(false)
    , pParams(nullptr)
    , pDlssFeature(nullptr)
    , prevDlssFeatureValues{}
{
    isInitialized = TryInit(_instance, _device, _physDevice, _pAppGuid, _enableDebug);

    if (!CheckSupport())
    {
        Destroy();
    }
}

bool vkpt::DLSS::TryInit(VkInstance instance, VkDevice device, VkPhysicalDevice physDevice, const char *pAppGuid, bool enableDebug)
{
    std::wstring dllPath = GetFolderPath() + (enableDebug ? L"/dev/" : L"/rel/");

#ifdef NV_WINDOWS
    wchar_t *dllPath_c = (wchar_t *)dllPath.c_str();
#else
    char dllPath_c_buf[PATH_MAX];
    char *dllPath_c = &dllPath_c_buf[0];
    std::wcstombs(dllPath_c, dllPath.c_str(), PATH_MAX);
#endif

    NVSDK_NGX_PathListInfo pathsInfo = {};
    pathsInfo.Path = &dllPath_c;
    pathsInfo.Length = 1;

    NGSDK_NGX_LoggingInfo debugLogInfo = {};
    debugLogInfo.LoggingCallback = &PrintCallback;
    debugLogInfo.MinimumLoggingLevel = NVSDK_NGX_Logging_Level::NVSDK_NGX_LOGGING_LEVEL_ON;

    NGSDK_NGX_LoggingInfo releaseLogInfo = {};

    NVSDK_NGX_FeatureCommonInfo commonInfo = {};
    commonInfo.PathListInfo = pathsInfo;
    commonInfo.LoggingInfo = enableDebug ? debugLogInfo : releaseLogInfo;

    const std::regex guidRegex("^[{]?[0-9a-fA-F]{8}-([0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}[}]?$");

    if (pAppGuid == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Application GUID wasn't provided. Generate and specify it to use DLSS.");
    }

    if (!std::regex_match(pAppGuid, guidRegex))
    {
        throw RgException(RG_WRONG_ARGUMENT, "Provided application GUID is not GUID. Generate and specify correct GUID to use DLSS.");
    }

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        pAppGuid,
        NVSDK_NGX_EngineType::NVSDK_NGX_ENGINE_TYPE_CUSTOM, RG_RTGL_VERSION_API, L"DLSSTemp/", instance, physDevice, device, &commonInfo);

    if (NVSDK_NGX_FAILED(r))
    {
        return false;
    }

    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&pParams);
    if (NVSDK_NGX_FAILED(r))
    {
        NVSDK_NGX_VULKAN_Shutdown();
        pParams = nullptr;

        return false;
    }

    return true;
}

bool vkpt::DLSS::CheckSupport() const
{
    if (!isInitialized || pParams == nullptr)
    {
        return false;
    }

    int needsUpdatedDriver = 0;
    unsigned int minDriverVersionMajor = 0;
    unsigned int minDriverVersionMinor = 0;

    NVSDK_NGX_Result r_upd = pParams->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver);
    NVSDK_NGX_Result r_mjr = pParams->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minDriverVersionMajor);
    NVSDK_NGX_Result r_mnr = pParams->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minDriverVersionMinor);

    if (NVSDK_NGX_SUCCEED(r_upd) && NVSDK_NGX_SUCCEED(r_mjr) && NVSDK_NGX_SUCCEED(r_mnr))
    {
        if (needsUpdatedDriver)
        {
            return false;
        }
    }

    int isDlssSupported = 0;
    NVSDK_NGX_Result featureInitResult;

    NVSDK_NGX_Result r = pParams->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &isDlssSupported);
    if (NVSDK_NGX_FAILED(r) || !isDlssSupported)
    {
        NVSDK_NGX_Parameter_GetI(pParams, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, (int *)&featureInitResult);
        return false;
    }

    return true;
}

vkpt::DLSS::~DLSS()
{
    Destroy();
}

void vkpt::DLSS::DestroyDlssFeature()
{
    assert(pDlssFeature != nullptr);

    vkDeviceWaitIdle(device);

    NVSDK_NGX_VULKAN_ReleaseFeature(pDlssFeature);
    pDlssFeature = nullptr;
}

void vkpt::DLSS::Destroy()
{
    if (!isInitialized)
    {
        return;
    }

    vkDeviceWaitIdle(device);

    if (pDlssFeature != nullptr)
    {
        DestroyDlssFeature();
    }

    NVSDK_NGX_VULKAN_DestroyParameters(pParams);
    NVSDK_NGX_VULKAN_Shutdown();

    pParams = nullptr;
    isInitialized = false;
}

bool vkpt::DLSS::IsDlssAvailable() const
{
    return isInitialized && pParams != nullptr;
}

static NVSDK_NGX_PerfQuality_Value ToNGXPerfQuality(RgRenderResolutionMode mode)
{
    switch (mode)
    {
        case RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        case RG_RENDER_RESOLUTION_MODE_PERFORMANCE:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case RG_RENDER_RESOLUTION_MODE_BALANCED:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_Balanced;
        case RG_RENDER_RESOLUTION_MODE_QUALITY:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case RG_RENDER_RESOLUTION_MODE_ULTRA_QUALITY:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_UltraQuality;
        default:
            assert(0);
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_Balanced;
    }
}

bool vkpt::DLSS::AreSameDlssFeatureValues(const RenderResolutionHelper &renderResolution) const
{
    return
        prevDlssFeatureValues.renderWidth    == renderResolution.Width() &&
        prevDlssFeatureValues.renderHeight   == renderResolution.Height() &&
        prevDlssFeatureValues.upscaledWidth  == renderResolution.UpscaledWidth() &&
        prevDlssFeatureValues.upscaledHeight == renderResolution.UpscaledHeight();
}

void vkpt::DLSS::SaveDlssFeatureValues(const RenderResolutionHelper &renderResolution)
{
    prevDlssFeatureValues.renderWidth = renderResolution.Width();
    prevDlssFeatureValues.renderHeight = renderResolution.Height();
    prevDlssFeatureValues.upscaledWidth = renderResolution.UpscaledWidth();
    prevDlssFeatureValues.upscaledHeight = renderResolution.UpscaledHeight();
}

bool vkpt::DLSS::ValidateDlssFeature(VkCommandBuffer cmd, const RenderResolutionHelper &renderResolution)
{
    if (!isInitialized || pParams == nullptr)
    {
        return false;
    }

    if (AreSameDlssFeatureValues(renderResolution))
    {
        return true;
    }

    SaveDlssFeatureValues(renderResolution);

    if (pDlssFeature != nullptr)
    {
        DestroyDlssFeature();
    }

    NVSDK_NGX_DLSS_Create_Params dlssParams = {};
    dlssParams.Feature.InWidth = renderResolution.Width();
    dlssParams.Feature.InHeight = renderResolution.Height();
    dlssParams.Feature.InTargetWidth = renderResolution.UpscaledWidth();
    dlssParams.Feature.InTargetHeight = renderResolution.UpscaledHeight();

    int &dlssCreateFeatureFlags = dlssParams.InFeatureCreateFlags;
    dlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    dlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;

    uint32_t creationNodeMask = 1;
    uint32_t visibilityNodeMask = 1;

    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(cmd, creationNodeMask, visibilityNodeMask,
                                                    &pDlssFeature, pParams, &dlssParams);
    if (NVSDK_NGX_FAILED(r))
    {
        pDlssFeature = nullptr;
        return false;
    }

    return true;
}

static NVSDK_NGX_Resource_VK ToNGXResource(const std::shared_ptr<vkpt::Framebuffers> &framebuffers, uint32_t frameIndex,
                                           vkpt::FramebufferImageIndex imageIndex, NVSDK_NGX_Dimensions size, bool withWriteAccess = false)
{
    auto [image, view, format] = framebuffers->GetImageHandles(imageIndex, frameIndex);

    VkImageSubresourceRange subresourceRange = {};
    subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresourceRange.baseMipLevel = 0;
    subresourceRange.levelCount = 1;
    subresourceRange.baseArrayLayer = 0;
    subresourceRange.layerCount = 1;

    return NVSDK_NGX_Create_ImageView_Resource_VK(view, image, subresourceRange, format, size.Width, size.Height, withWriteAccess);
}

vkpt::FramebufferImageIndex vkpt::DLSS::Apply(VkCommandBuffer cmd, uint32_t frameIndex,
                                              const std::shared_ptr<Framebuffers> &framebuffers,
                                              const RenderResolutionHelper &renderResolution,
                                              RgFloat2D jitterOffset)
{
    if (!IsDlssAvailable())
    {
        throw RgException(RG_WRONG_ARGUMENT, "Nvidia DLSS is not supported (or DLSS dynamic library files are not found). Check availability before usage.");
    }

    ValidateDlssFeature(cmd, renderResolution);

    if (pDlssFeature == nullptr)
    {
        throw RgException(RG_GRAPHICS_API_ERROR, "Internal error of Nvidia DLSS: NGX_VULKAN_CREATE_DLSS_EXT has failed.");
    }

    const FramebufferImageIndex outputImage = FB_IMAGE_INDEX_UPSCALED_PONG;

    CmdLabel label(cmd, "DLSS");

    FramebufferImageIndex fs[] =
    {
        FB_IMAGE_INDEX_FINAL,
        FB_IMAGE_INDEX_MOTION_DLSS,
        FB_IMAGE_INDEX_DEPTH_NDC,
    };
    framebuffers->BarrierMultiple(cmd, frameIndex, fs, Framebuffers::BarrierType::Storage);

    int resetAccumulation = 0;
    NVSDK_NGX_Coordinates sourceOffset = { 0, 0 };
    NVSDK_NGX_Dimensions sourceSize = { renderResolution.Width(),         renderResolution.Height() };
    NVSDK_NGX_Dimensions targetSize = { renderResolution.UpscaledWidth(), renderResolution.UpscaledHeight() };

    NVSDK_NGX_Resource_VK unresolvedColorResource = ToNGXResource(framebuffers, frameIndex, FB_IMAGE_INDEX_FINAL,       sourceSize);
    NVSDK_NGX_Resource_VK resolvedColorResource   = ToNGXResource(framebuffers, frameIndex, outputImage,                 targetSize, true);
    NVSDK_NGX_Resource_VK motionVectorsResource   = ToNGXResource(framebuffers, frameIndex, FB_IMAGE_INDEX_MOTION_DLSS, sourceSize);
    NVSDK_NGX_Resource_VK depthResource           = ToNGXResource(framebuffers, frameIndex, FB_IMAGE_INDEX_DEPTH_NDC,   sourceSize);

    NVSDK_NGX_VK_DLSS_Eval_Params evalParams = {};
    evalParams.Feature.pInColor = &unresolvedColorResource;
    evalParams.Feature.pInOutput = &resolvedColorResource;
    evalParams.pInDepth = &depthResource;
    evalParams.pInMotionVectors = &motionVectorsResource;
    evalParams.InJitterOffsetX = jitterOffset.data[0] * (-1);
    evalParams.InJitterOffsetY = jitterOffset.data[1] * (-1);
    evalParams.Feature.InSharpness = renderResolution.GetNvDlssSharpness();
    evalParams.InReset = resetAccumulation;
    evalParams.InMVScaleX = static_cast<float>(sourceSize.Width);
    evalParams.InMVScaleY = static_cast<float>(sourceSize.Height);
    evalParams.InColorSubrectBase = sourceOffset;
    evalParams.InDepthSubrectBase = sourceOffset;
    evalParams.InTranslucencySubrectBase = sourceOffset;
    evalParams.InMVSubrectBase = sourceOffset;
    evalParams.InRenderSubrectDimensions = sourceSize;

    NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, pDlssFeature, pParams, &evalParams);

    return outputImage;
}

void vkpt::DLSS::GetOptimalSettings(uint32_t userWidth, uint32_t userHeight, RgRenderResolutionMode mode,
                                    uint32_t *pOutWidth, uint32_t *pOutHeight, float *pOutSharpness) const
{
    *pOutWidth = userWidth;
    *pOutHeight = userHeight;
    *pOutSharpness = 0.0f;

    if (!isInitialized || pParams == nullptr)
    {
        return;
    }

    uint32_t minWidth, minHeight, maxWidth, maxHeight;
    NGX_DLSS_GET_OPTIMAL_SETTINGS(pParams,
                                  userWidth, userHeight, ToNGXPerfQuality(mode),
                                  pOutWidth, pOutHeight,
                                  &maxWidth, &maxHeight, &minWidth, &minHeight,
                                  pOutSharpness);
}

std::vector<const char *> vkpt::DLSS::GetDlssVulkanInstanceExtensions()
{
    uint32_t instanceExtCount;
    const char **ppInstanceExts;
    uint32_t deviceExtCount;
    const char **ppDeviceExts;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(&instanceExtCount, &ppInstanceExts, &deviceExtCount, &ppDeviceExts);
    assert(NVSDK_NGX_SUCCEED(r));

    std::vector<const char *> v;

    for (uint32_t i = 0; i < instanceExtCount; i++)
    {
        v.push_back(ppInstanceExts[i]);
    }

    return v;
}

std::vector<const char *> vkpt::DLSS::GetDlssVulkanDeviceExtensions()
{
    uint32_t instanceExtCount;
    const char **ppInstanceExts;
    uint32_t deviceExtCount;
    const char **ppDeviceExts;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(&instanceExtCount, &ppInstanceExts, &deviceExtCount, &ppDeviceExts);
    assert(NVSDK_NGX_SUCCEED(r));

    std::vector<const char *> v;

    for (uint32_t i = 0; i < deviceExtCount; i++)
    {
        if (strcmp(ppDeviceExts[i], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0)
        {
            continue;
        }

        v.push_back(ppDeviceExts[i]);
    }

    return v;
}

#else

vkpt::DLSS::DLSS(VkInstance _instance, VkDevice _device, VkPhysicalDevice _physDevice, const char *pAppGuid, bool _enableDebug)
    : device(_device)
    , isInitialized(false)
    , pParams(nullptr)
    , pDlssFeature(nullptr)
    , prevDlssFeatureValues{}
{
}

vkpt::DLSS::~DLSS()
{
}

vkpt::FramebufferImageIndex vkpt::DLSS::Apply(VkCommandBuffer cmd, uint32_t frameIndex, const std::shared_ptr<Framebuffers> &framebuffers, const RenderResolutionHelper &renderResolution, RgFloat2D jitterOffset)
{
    throw RgException(RG_WRONG_ARGUMENT, "vkpt was built without DLSS support. Enable RG_WITH_NVIDIA_DLSS CMake option.");
}

void vkpt::DLSS::GetOptimalSettings(uint32_t userWidth, uint32_t userHeight, RgRenderResolutionMode mode, uint32_t *pOutWidth, uint32_t *pOutHeight, float *pOutSharpness) const
{
    throw RgException(RG_WRONG_ARGUMENT, "vkpt was built without DLSS support. Enable RG_WITH_NVIDIA_DLSS CMake option.");
}

bool vkpt::DLSS::IsDlssAvailable() const
{
    return false;
}

std::vector<const char *> vkpt::DLSS::GetDlssVulkanInstanceExtensions()
{
    return {};
}

std::vector<const char *> vkpt::DLSS::GetDlssVulkanDeviceExtensions()
{
    return {};
}

bool vkpt::DLSS::TryInit(VkInstance instance, VkDevice device, VkPhysicalDevice physDevice, const char *pAppGuid, bool enableDebug)
{
    return false;
}

bool vkpt::DLSS::CheckSupport() const
{
    return false;
}

void vkpt::DLSS::Destroy()
{
}

void vkpt::DLSS::DestroyDlssFeature()
{
}

bool vkpt::DLSS::AreSameDlssFeatureValues(const RenderResolutionHelper &renderResolution) const
{
    return false;
}

void vkpt::DLSS::SaveDlssFeatureValues(const RenderResolutionHelper &renderResolution)
{
}

bool vkpt::DLSS::ValidateDlssFeature(VkCommandBuffer cmd, const RenderResolutionHelper &renderResolution)
{
    return false;
}

#endif // RG_USE_NVIDIA_DLSS
