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

#include <algorithm>
#include <cassert>
#include <cmath>

#include "DLSS.h"
#include "QrException.h"
#include "ResolutionState.h"

namespace qray
{

class RenderResolutionHelper
{
public:
    RenderResolutionHelper() = default;
    ~RenderResolutionHelper() = default;

    RenderResolutionHelper(const RenderResolutionHelper &other) = delete;
    RenderResolutionHelper(RenderResolutionHelper &&other) noexcept = delete;
    RenderResolutionHelper &operator=(const RenderResolutionHelper &other) = delete;
    RenderResolutionHelper &operator=(RenderResolutionHelper &&other) noexcept = delete;

    void Setup( const QrDrawFrameRenderResolutionParams* pParams,
                uint32_t                                 windowWidth,
                uint32_t                                 windowHeight,
                const std::shared_ptr< DLSS >&           dlss )
    {
        renderWidth  = windowWidth;
        renderHeight = windowHeight;

        upscaledWidth  = windowWidth;
        upscaledHeight = windowHeight;

        dlssSharpness = 0;

        if (pParams == nullptr)
        {
            upscaleTechnique = QR_RENDER_UPSCALE_TECHNIQUE_LINEAR;
            sharpenTechnique = QR_RENDER_SHARPEN_TECHNIQUE_NONE;
            resolutionMode = QR_RENDER_RESOLUTION_MODE_CUSTOM;

            return;
        }

        upscaleTechnique = pParams->upscaleTechnique;
        sharpenTechnique = pParams->sharpenTechnique;
        resolutionMode   = pParams->resolutionMode;

        ValidateUpscaleTechnique( upscaleTechnique );
        ValidateSharpenTechnique( sharpenTechnique );
        ValidateResolutionMode( resolutionMode );

        if (upscaleTechnique == QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3)
        {
            SetupAmdFsr( pParams, windowWidth, windowHeight );
        }
        else if (upscaleTechnique == QR_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS)
        {
            SetupNvDlss( pParams, windowWidth, windowHeight, dlss );
        }
        else if (resolutionMode == QR_RENDER_RESOLUTION_MODE_CUSTOM)
        {
            renderWidth  = pParams->customRenderSize.width;
            renderHeight = pParams->customRenderSize.height;
        }
    }

    float GetMipLodBias(float nativeBias = 0.0f) const
    {
        if (!IsUpscaleEnabled())
        {
            return nativeBias;
        }

        const float ratio = (float)Width() / (float)UpscaledWidth();
        return nativeBias + std::log2(std::max(0.01f, ratio)) - 1.0f;
    }

    uint32_t Width()            const { return renderWidth + renderWidth % 2; }
    uint32_t Height()           const { return renderHeight; }

    uint32_t UpscaledWidth()    const { return upscaledWidth; }
    uint32_t UpscaledHeight()   const { return upscaledHeight; }

    bool IsAmdFsr3Enabled()     const { return upscaleTechnique == QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3; }
    bool IsNvDlssEnabled()      const { return upscaleTechnique == QR_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS; }
    bool IsUpscaleEnabled()     const { return IsAmdFsr3Enabled() || IsNvDlssEnabled(); }

    QrRenderUpscaleTechnique GetUpscaleTechnique() const { return upscaleTechnique; }

    float GetAmdFsrSharpness()  const { return 1.0f; }
    float GetNvDlssSharpness()  const { return dlssSharpness; }

    bool IsCASInsideFSR3()      const { return upscaleTechnique == QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3 && sharpenTechnique == QR_RENDER_SHARPEN_TECHNIQUE_AMD_CAS; }

    bool IsDedicatedSharpeningEnabled() const
    {
        return !IsCASInsideFSR3() &&
               sharpenTechnique != QR_RENDER_SHARPEN_TECHNIQUE_NONE;
    }

    QrRenderSharpenTechnique GetSharpeningTechnique() const { return sharpenTechnique; }
    float                    GetSharpeningIntensity() const { return 1.0f; }

    VkFilter GetBlitFilter() const { return upscaleTechnique == QR_RENDER_UPSCALE_TECHNIQUE_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }

    ResolutionState GetResolutionState() const
    {
        assert(Width() % 2 == 0);
        return ResolutionState{ Width(), Height(), UpscaledWidth(), UpscaledHeight() };
    }

private:
    static void ValidateUpscaleTechnique(QrRenderUpscaleTechnique technique)
    {
        switch (technique)
        {
            case QR_RENDER_UPSCALE_TECHNIQUE_NEAREST:
            case QR_RENDER_UPSCALE_TECHNIQUE_LINEAR:
            case QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3:
            case QR_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS:
                return;
            default:
                throw QrException(QR_WRONG_ARGUMENT, "QrDrawFrameRenderResolutionParams::upscaleTechnique is incorrect");
        }
    }

    static void ValidateSharpenTechnique(QrRenderSharpenTechnique technique)
    {
        switch (technique)
        {
            case QR_RENDER_SHARPEN_TECHNIQUE_NONE:
            case QR_RENDER_SHARPEN_TECHNIQUE_NAIVE:
            case QR_RENDER_SHARPEN_TECHNIQUE_AMD_CAS:
                return;
            default:
                throw QrException(QR_WRONG_ARGUMENT, "QrDrawFrameRenderResolutionParams::sharpenTechnique is incorrect");
        }
    }

    static void ValidateResolutionMode(QrRenderResolutionMode mode)
    {
        switch (mode)
        {
            case QR_RENDER_RESOLUTION_MODE_CUSTOM:
            case QR_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE:
            case QR_RENDER_RESOLUTION_MODE_PERFORMANCE:
            case QR_RENDER_RESOLUTION_MODE_BALANCED:
            case QR_RENDER_RESOLUTION_MODE_QUALITY:
            case QR_RENDER_RESOLUTION_MODE_ULTRA_QUALITY:
            case QR_RENDER_RESOLUTION_MODE_NATIVE_AA:
                return;
            default:
                throw QrException(QR_WRONG_ARGUMENT, "QrDrawFrameRenderResolutionParams::resolutionMode is incorrect");
        }
    }

    void SetupAmdFsr( const QrDrawFrameRenderResolutionParams* pParams,
                      uint32_t                                 windowWidth,
                      uint32_t                                 windowHeight )
    {
        if (resolutionMode == QR_RENDER_RESOLUTION_MODE_ULTRA_QUALITY)
        {
            resolutionMode = QR_RENDER_RESOLUTION_MODE_QUALITY;
            assert(0 && "Ultra quality should not be used with FSR");
        }

        if (resolutionMode == QR_RENDER_RESOLUTION_MODE_NATIVE_AA)
        {
            renderWidth  = windowWidth;
            renderHeight = windowHeight;
            return;
        }

        if (resolutionMode == QR_RENDER_RESOLUTION_MODE_CUSTOM)
        {
            renderWidth  = pParams->customRenderSize.width;
            renderHeight = pParams->customRenderSize.height;
            return;
        }

        float divisor = 1.0f;

        switch (resolutionMode)
        {
        case QR_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE: divisor = 3.0f; break;
        case QR_RENDER_RESOLUTION_MODE_PERFORMANCE:       divisor = 2.0f; break;
        case QR_RENDER_RESOLUTION_MODE_BALANCED:          divisor = 1.7f; break;
        case QR_RENDER_RESOLUTION_MODE_QUALITY:           divisor = 1.5f; break;
        default: assert(0); break;
        }

        renderWidth  = static_cast< uint32_t >( static_cast< float >( windowWidth ) / divisor );
        renderHeight = static_cast< uint32_t >( static_cast< float >( windowHeight ) / divisor );
    }

    void SetupNvDlss( const QrDrawFrameRenderResolutionParams* pParams,
                      uint32_t                                 windowWidth,
                      uint32_t                                 windowHeight,
                      const std::shared_ptr< DLSS >&           dlss )
    {
        if (resolutionMode == QR_RENDER_RESOLUTION_MODE_CUSTOM)
        {
            renderWidth  = pParams->customRenderSize.width;
            renderHeight = pParams->customRenderSize.height;
            return;
        }

        dlss->GetOptimalSettings( windowWidth,
                                  windowHeight,
                                  resolutionMode,
                                  &renderWidth,
                                  &renderHeight,
                                  &dlssSharpness );

        if (renderWidth == 0 || renderHeight == 0)
        {
            renderWidth  = windowWidth;
            renderHeight = windowHeight;
        }
    }

    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;

    uint32_t upscaledWidth = 0;
    uint32_t upscaledHeight = 0;

    QrRenderUpscaleTechnique    upscaleTechnique = QR_RENDER_UPSCALE_TECHNIQUE_LINEAR;
    QrRenderSharpenTechnique    sharpenTechnique = QR_RENDER_SHARPEN_TECHNIQUE_NONE;
    QrRenderResolutionMode      resolutionMode   = QR_RENDER_RESOLUTION_MODE_CUSTOM;

    float dlssSharpness = 0.0f;
};

}
