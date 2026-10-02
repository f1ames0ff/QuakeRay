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

#include "ASComponent.h"

namespace qray
{
namespace
{
    VkAccelerationStructureKHR CreateAccelerationStructure(
        VkDevice device, VkAccelerationStructureTypeKHR type, VkBuffer buffer,
        VkDeviceSize size, const char *debugName)
    {
        VkAccelerationStructureCreateInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        info.type = type;
        info.size = size;
        info.buffer = buffer;

        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        VkResult r = svkCreateAccelerationStructureKHR(device, &info, nullptr, &as);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, as, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, debugName);
        return as;
    }
}
}

qray::ASComponent::ASComponent(VkDevice _device, const char *_debugName)
:
    device(_device),
    as(VK_NULL_HANDLE),
    debugName(_debugName)
{
}

qray::BLASComponent::BLASComponent(VkDevice _device, VertexCollectorFilterTypeFlags _filter)
:
    ASComponent(_device, VertexCollectorFilterTypeFlags_GetNameForBLAS(_filter)),
    filter(_filter),
    geomCount(0)
{
}

qray::TLASComponent::TLASComponent(VkDevice _device, const char *_debugName)
:
    ASComponent(_device, _debugName)
{
}

qray::ASComponent::~ASComponent()
{
    Destroy();
}

void qray::ASComponent::CreateBuffer(const std::shared_ptr<MemoryAllocator> &allocator, VkDeviceSize size)
{
    assert(!buffer.IsInitted());

    buffer.Init(
        allocator, size,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        GetBufferDebugName());
}

void qray::ASComponent::Destroy()
{
    assert(device != VK_NULL_HANDLE);

    buffer.Destroy();

    if (as != VK_NULL_HANDLE)
    {
        svkDestroyAccelerationStructureKHR(device, as, nullptr);
        as = VK_NULL_HANDLE;
    }
}

void qray::ASComponent::RecreateIfNotValid(
    const VkAccelerationStructureBuildSizesInfoKHR &buildSizes,
    const std::shared_ptr<MemoryAllocator> &allocator)
{
    if (IsValid(buildSizes))
    {
        return;
    }

    Destroy();

    CreateBuffer(allocator, buildSizes.accelerationStructureSize);
    CreateAS(buildSizes.accelerationStructureSize);
}

void qray::BLASComponent::CreateAS(VkDeviceSize size)
{
    assert(device != VK_NULL_HANDLE);

    as = CreateAccelerationStructure(
        device, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        buffer.GetBuffer(), size, debugName);
}

void qray::TLASComponent::CreateAS(VkDeviceSize size)
{
    assert(device != VK_NULL_HANDLE);

    as = CreateAccelerationStructure(
        device, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        buffer.GetBuffer(), size, debugName);
}

bool qray::ASComponent::IsValid(const VkAccelerationStructureBuildSizesInfoKHR &buildSizes) const
{
    return buffer.IsInitted() && buffer.GetSize() >= buildSizes.accelerationStructureSize;
}

VkAccelerationStructureKHR qray::ASComponent::GetAS() const
{
    return as;
}

VkDeviceAddress qray::ASComponent::GetASAddress() const
{
    assert(buffer.IsInitted());
    return GetASAddress(as);
}

VkDeviceAddress qray::ASComponent::GetASAddress(VkAccelerationStructureKHR _as) const
{
    assert(device != VK_NULL_HANDLE);
    assert(_as != VK_NULL_HANDLE);

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = _as;

    return svkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);
}

const char *qray::BLASComponent::GetBufferDebugName() const
{
    return "BLAS buffer";
}

const char *qray::TLASComponent::GetBufferDebugName() const
{
    return "TLAS buffer";
}

qray::VertexCollectorFilterTypeFlags qray::BLASComponent::GetFilter() const
{
    return filter;
}

void qray::BLASComponent::SetGeometryCount(uint32_t _geomCount)
{
    geomCount = _geomCount;
}

bool qray::BLASComponent::IsEmpty() const
{
    return geomCount == 0;
}

uint32_t qray::BLASComponent::GetGeomCount() const
{
    return geomCount;
}
