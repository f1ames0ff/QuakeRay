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

#include "Utils.h"

#include <cmath>

using namespace qray;

namespace
{
    constexpr float ALMOST_ZERO_THRESHOLD = 0.01f;

    VkImageSubresourceRange MakeColorSubresourceRange()
    {
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        return range;
    }
}

void Utils::BarrierImage(
    VkCommandBuffer cmd, VkImage image, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
    VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask, const VkImageSubresourceRange &subresourceRange)
{
    VkImageMemoryBarrier imageBarrier = {};
    imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.srcAccessMask = srcAccessMask;
    imageBarrier.dstAccessMask = dstAccessMask;
    imageBarrier.oldLayout = oldLayout;
    imageBarrier.newLayout = newLayout;
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = image;
    imageBarrier.subresourceRange = subresourceRange;

    vkCmdPipelineBarrier(
        cmd,
        srcStageMask, dstStageMask, 0,
        0, nullptr,
        0, nullptr,
        1, &imageBarrier);
}

void Utils::BarrierImage(
    VkCommandBuffer cmd, VkImage image, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
    VkImageLayout oldLayout, VkImageLayout newLayout, const VkImageSubresourceRange &subresourceRange)
{
    BarrierImage(
        cmd, image, srcAccessMask, dstAccessMask, oldLayout, newLayout,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, subresourceRange);
}

void Utils::BarrierImage(
    VkCommandBuffer cmd, VkImage image, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
    VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask)
{
    BarrierImage(
        cmd, image, srcAccessMask, dstAccessMask, oldLayout, newLayout,
        srcStageMask, dstStageMask, MakeColorSubresourceRange());
}

void Utils::BarrierImage(
    VkCommandBuffer cmd, VkImage image, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
    VkImageLayout oldLayout, VkImageLayout newLayout)
{
    BarrierImage(
        cmd, image, srcAccessMask, dstAccessMask, oldLayout, newLayout,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        MakeColorSubresourceRange());
}

void Utils::ASBuildMemoryBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0,
        1, &barrier,
        0, nullptr,
        0, nullptr);
}

void Utils::WaitForFence(VkDevice device, VkFence fence)
{
    VK_CHECKERROR(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
}

void Utils::ResetFence(VkDevice device, VkFence fence)
{
    VK_CHECKERROR(vkResetFences(device, 1, &fence));
}

void Utils::WaitAndResetFence(VkDevice device, VkFence fence)
{
    VK_CHECKERROR(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    VK_CHECKERROR(vkResetFences(device, 1, &fence));
}

void qray::Utils::WaitAndResetFences(VkDevice device, VkFence fence_A, VkFence fence_B)
{
    VkFence fences[2];
    uint32_t count = 0;

    if (fence_A != VK_NULL_HANDLE)
    {
        fences[count++] = fence_A;
    }

    if (fence_B != VK_NULL_HANDLE)
    {
        fences[count++] = fence_B;
    }

    VK_CHECKERROR(vkWaitForFences(device, count, fences, VK_TRUE, UINT64_MAX));
    VK_CHECKERROR(vkResetFences(device, count, fences));
}

bool Utils::AreViewportsSame(const VkViewport &a, const VkViewport &b)
{
    constexpr float eps = 0.1f;
    constexpr float depthEps = 0.001f;

    return
        std::abs(a.x        - b.x)          < eps &&
        std::abs(a.y        - b.y)          < eps &&
        std::abs(a.width    - b.width)      < eps &&
        std::abs(a.height   - b.height)     < eps &&
        std::abs(a.minDepth - b.minDepth)   < depthEps &&
        std::abs(a.maxDepth - b.maxDepth)   < depthEps;
}

bool Utils::IsAlmostZero( const float v[ 3 ] )
{
    return
        std::abs( v[ 0 ] ) +
        std::abs( v[ 1 ] ) +
        std::abs( v[ 2 ] ) < ALMOST_ZERO_THRESHOLD;
}

bool Utils::IsAlmostZero( const QrFloat3D& v )
{
    return IsAlmostZero( v.data );
}

bool qray::Utils::IsAlmostZero(const QrMatrix3D &m)
{
    float s = 0;

    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            s += std::abs(m.matrix[i][j]);
        }
    }

    return s < ALMOST_ZERO_THRESHOLD;
}

float qray::Utils::Dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

float qray::Utils::Length(const float v[3])
{
    return sqrtf(Dot(v, v));
}

void qray::Utils::Normalize(float inout[3])
{
    float len = Length(inout);

    if (len > 0.01f)
    {
        inout[0] /= len;
        inout[1] /= len;
        inout[2] /= len;
    }
    else
    {
        assert(0);
        inout[0] = inout[1] = inout[2] = 0.0f;
    }
}

void Utils::Negate( float inout[3] )
{
    inout[ 0 ] *= -1;
    inout[ 1 ] *= -1;
    inout[ 2 ] *= -1;
}

void Utils::Nullify( float inout[3] )
{
    inout[ 0 ] = 0;
    inout[ 1 ] = 0;
    inout[ 2 ] = 0;
}

void qray::Utils::Cross(const float a[3], const float b[3], float r[3])
{
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

QrFloat3D qray::Utils::GetUnnormalizedNormal(const QrFloat3D positions[3])
{
    const float *a = positions[0].data;
    const float *b = positions[1].data;
    const float *c = positions[2].data;

    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };

    QrFloat3D n = {};
    Cross(e1, e2, n.data);

    return n;
}

bool qray::Utils::GetNormalAndArea(const QrFloat3D positions[3], QrFloat3D &normal, float &area)
{
    normal = GetUnnormalizedNormal(positions);

    float len = Length(normal.data);
    normal.data[0] /= len;
    normal.data[1] /= len;
    normal.data[2] /= len;

    area = len * 0.5f;
    return area > 0.01f;
}

void qray::Utils::SetMatrix3ToGLSLMat4(float dst[16], const QrMatrix3D &src)
{
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            if (i < 3 && j < 3)
            {
                dst[i * 4 + j] = src.matrix[j][i];
            }
            else
            {
                dst[i * 4 + j] = i == j ? 1.0f : 0.0f;
            }
        }
    }
}

uint32_t qray::Utils::GetPreviousByModulo(uint32_t value, uint32_t count)
{
    assert(count > 0);
    return (value + (count - 1)) % count;
}

uint32_t qray::Utils::GetWorkGroupCount(float size, uint32_t groupSize)
{
    return GetWorkGroupCount(static_cast<uint32_t>(std::ceil(size)), groupSize);
}

uint32_t qray::Utils::GetWorkGroupCount(uint32_t size, uint32_t groupSize)
{
    if (groupSize == 0)
    {
        assert(0);
        return 0;
    }

    const uint32_t count = (size + (groupSize - 1)) / groupSize;

    return count == 0 ? 1 : count;
}
