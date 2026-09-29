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

#pragma once

#include "Framebuffers.h"
#include "UserFunction.h"

using ffxContext = void*;

namespace qray
{
    class RenderResolutionHelper;

    namespace FidelityFX
    {
        class FSR : public IFramebuffersDependency
        {
        public:
            FSR(VkDevice device, VkPhysicalDevice physDevice, UserPrint* pUserPrint);
            ~FSR() override;

            FSR(const FSR& other) = delete;
            FSR(FSR&& other) noexcept = delete;
            FSR& operator=(const FSR& other) = delete;
            FSR& operator=(FSR&& other) noexcept = delete;

            void SetUpscaleVersion(QrRenderUpscaleTechnique technique);
            void OnFramebuffersSizeChange(const ResolutionState& resolutionState) override;

            FramebufferImageIndex Apply(
                VkCommandBuffer cmd,
                uint32_t frameIndex,
                const std::shared_ptr<Framebuffers>& framebuffers,
                const RenderResolutionHelper& renderResolution,
                QrFloat2D jitterOffset,
                float timeDelta,
                float nearPlane,
                float farPlane,
                float fovVerticalRad,
                bool resetAccumulation);

            static QrFloat2D GetJitter(const ResolutionState& resolutionState, uint32_t frameId);
            static bool IsUpscaleVersionAvailable(QrRenderUpscaleTechnique technique);

        private:
            void RecreateContext();
            void DestroyContext();
            static uint64_t FindVersionId();
            VkDevice m_device;
            VkPhysicalDevice m_physDevice;
            UserPrint* m_pUserPrint;
            ffxContext m_context;
            QrRenderUpscaleTechnique m_requestedTechnique;
            uint32_t m_renderWidth;
            uint32_t m_renderHeight;
            uint32_t m_displayWidth;
            uint32_t m_displayHeight;
            bool m_hasSize;
            static inline ffxContext s_contextForJitter = nullptr;
        };
    }
}
