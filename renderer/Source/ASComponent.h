// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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

#include "Common.h"
#include "Buffer.h"
#include "VertexCollectorFilterType.h"

namespace qray
{

struct ASComponent
{
protected:
    explicit ASComponent(VkDevice device, const char *debugName);

public:
    virtual ~ASComponent() = 0;
    void Destroy();

    ASComponent(const ASComponent &other) = delete;
    ASComponent &operator=(const ASComponent &other) = delete;
    ASComponent &operator=(ASComponent &&other) noexcept = delete;

    void RecreateIfNotValid(
        const VkAccelerationStructureBuildSizesInfoKHR &buildSizes,
        const std::shared_ptr<MemoryAllocator> &allocator);

    VkAccelerationStructureKHR GetAS() const;
    VkDeviceAddress GetASAddress() const;

    bool IsValid(const VkAccelerationStructureBuildSizesInfoKHR &buildSizes) const;

protected:
    virtual void CreateAS(VkDeviceSize size) = 0;
    virtual const char *GetBufferDebugName() const = 0;

private:
    void CreateBuffer(
        const std::shared_ptr<MemoryAllocator> &allocator,
        VkDeviceSize size);

    VkDeviceAddress GetASAddress(VkAccelerationStructureKHR as) const;

protected:
    VkDevice device;

    Buffer buffer;
    VkAccelerationStructureKHR as;

    const char *debugName;
};


struct BLASComponent : public ASComponent
{
public:
    explicit BLASComponent(VkDevice device, VertexCollectorFilterTypeFlags filter);
    VertexCollectorFilterTypeFlags GetFilter() const;

    void SetGeometryCount(uint32_t geomCount);

    bool IsEmpty() const;
    uint32_t GetGeomCount() const;

protected:
    void CreateAS(VkDeviceSize size) override;
    const char *GetBufferDebugName() const override;

private:
    VertexCollectorFilterTypeFlags filter;
    uint32_t geomCount;
};


struct TLASComponent : public ASComponent
{
public:
    explicit TLASComponent(VkDevice device, const char *debugName = nullptr);

protected:
    void CreateAS(VkDeviceSize size) override;
    const char *GetBufferDebugName() const override;
};

}
