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

#include "qr_gpu_context.h"

#include <qray/gpu_context.h>

#include <nvrhi/nvrhi.h>

extern "C" QrResult QR_GpuCreateInstance(const QrInstanceCreateInfo *pInfo, QrInstance *pResult)
{
    if (pInfo == nullptr || pResult == nullptr)
        return QR_WRONG_ARGUMENT;

    const nvrhi::Feature requiredFeatures[] = {
        nvrhi::Feature::RayTracingPipeline,
        nvrhi::Feature::RayTracingAccelStruct,
        nvrhi::Feature::RayQuery,
    };

    qray::QrayGpuRequirements requirements = {};
    requirements.header = {
        sizeof(qray::QrayGpuRequirements),
        qray::kGpuContextContractVersion,
    };
    requirements.minVulkanApiVersion = VK_API_VERSION_1_3;
    requirements.minimumFramesInFlight = 1;
    requirements.requiredNvrhiFeatures = { requiredFeatures, 3 };

    return qray::qrCreateInstanceWithGpuRequirements(pInfo, &requirements, pResult);
}

extern "C" QrResult QR_GpuValidateSharedContext(QrInstance instance)
{
    qray::QrayGpuContext context = {};
    context.header = {
        sizeof(qray::QrayGpuContext),
        qray::kGpuContextContractVersion,
    };

    QrResult result = qray::qrGetGpuContext(instance, &context);
    if (result != QR_SUCCESS)
        return result;

    if (context.pDevice == nullptr || context.pVulkanDevice == nullptr ||
        context.nativeVulkan.instance == VK_NULL_HANDLE ||
        context.nativeVulkan.physicalDevice == VK_NULL_HANDLE ||
        context.nativeVulkan.device == VK_NULL_HANDLE ||
        context.nativeVulkan.graphicsQueue == VK_NULL_HANDLE ||
        context.capabilities.graphicsQueueCount == 0 ||
        (context.capabilities.flags & qray::QRAY_GPU_CAP_SHARED_NVRHI_DEVICE) == 0)
    {
        return QR_FEATURE_UNAVAILABLE;
    }

    nvrhi::BufferDesc probeDesc;
    probeDesc.byteSize = 256;
    probeDesc.initialState = nvrhi::ResourceStates::CopyDest;
    probeDesc.debugName = "QuakeRay shared GPU context probe";
    nvrhi::BufferHandle probe = context.pDevice->createBuffer(probeDesc);
    return probe != nullptr ? QR_SUCCESS : QR_GRAPHICS_API_ERROR;
}
