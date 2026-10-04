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

#pragma once

#include <vector>

#include "Framebuffers.h"

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace qray
{

class RenderResolutionHelper;

class DLSS
{
public:
    DLSS(VkInstance instance, VkDevice device, VkPhysicalDevice physDevice,
         const char *pAppGuid,
         bool enableDebug);
    ~DLSS();

    DLSS(const DLSS &other) = delete;
    DLSS(DLSS &&other) noexcept = delete;
    DLSS &operator=(const DLSS &other) = delete;
    DLSS &operator=(DLSS &&other) noexcept = delete;

    FramebufferImageIndex Apply(VkCommandBuffer cmd, uint32_t frameIndex,
                                const std::shared_ptr<Framebuffers> &framebuffers,
                                const RenderResolutionHelper &renderResolution,
                                QrFloat2D jitterOffset);

    void GetOptimalSettings(uint32_t userWidth, uint32_t userHeight, QrRenderResolutionMode mode,
                            uint32_t *pOutWidth, uint32_t *pOutHeight, float *pOutSharpness) const;

    bool IsDlssAvailable() const;

    static std::vector<const char *> GetDlssVulkanInstanceExtensions();
    static std::vector<const char *> GetDlssVulkanDeviceExtensions();

private:
    bool TryInit(VkInstance instance, VkDevice device, VkPhysicalDevice physDevice, const char *pAppGuid, bool enableDebug);
    bool CheckSupport() const;
    void DestroyDlssFeature();
    void Destroy();

    bool AreSameDlssFeatureValues(const RenderResolutionHelper &renderResolution) const;
    void SaveDlssFeatureValues(const RenderResolutionHelper &renderResolution);

    bool ValidateDlssFeature(VkCommandBuffer cmd, const RenderResolutionHelper &renderResolution);

private:
    VkDevice            device;

    bool                isInitialized;
    NVSDK_NGX_Parameter *pParams;
    NVSDK_NGX_Handle    *pDlssFeature;

    struct PrevDlssFeatureValues
    {
        uint32_t renderWidth;
        uint32_t renderHeight;
        uint32_t upscaledWidth;
        uint32_t upscaledHeight;
    } prevDlssFeatureValues;
};

}
