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
#include <limits>
#include <optional>
#include <type_traits>

#include "Common.h"
#include "qray/qray.h"

namespace qray
{
    enum NullifyTokenType
    {
    };
    inline constexpr NullifyTokenType NullifyToken = {};

    template<size_t Size>
    struct FloatStorage
    {
        FloatStorage()  = default;
        ~FloatStorage() = default;

        explicit FloatStorage( NullifyTokenType ) : data{} {}
        explicit FloatStorage( const float* ptr ) { std::copy_n( ptr, Size, data ); }

        FloatStorage( const FloatStorage& other )     = default;
        FloatStorage( FloatStorage&& other ) noexcept = default;
        FloatStorage& operator=( const FloatStorage& other ) = default;
        FloatStorage& operator=( FloatStorage&& other ) noexcept = default;

        [[nodiscard]] const float* Get() const { return data; }
        float*                     Get() { return data; }

        float data[ Size ];
    };

    using Float16D = FloatStorage< 16 >;
    using Float4D = FloatStorage< 4 >;

    #define IfNotNull( ptr, ifnotnull ) \
        ( ( ptr ) != nullptr ? std::optional( ( ifnotnull ) ) : std::nullopt )

namespace Utils
{
    void BarrierImage(
        VkCommandBuffer cmd, VkImage image,
        VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
        const VkImageSubresourceRange &subresourceRange);

    void BarrierImage(
        VkCommandBuffer cmd, VkImage image,
        VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        const VkImageSubresourceRange &subresourceRange);

    void BarrierImage(
        VkCommandBuffer cmd, VkImage image,
        VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask);

    void BarrierImage(
        VkCommandBuffer cmd, VkImage image,
        VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
        VkImageLayout oldLayout, VkImageLayout newLayout);

    void ASBuildMemoryBarrier(
        VkCommandBuffer cmd
    );

    void WaitForFence(VkDevice device, VkFence fence);
    void ResetFence(VkDevice device, VkFence fence);
    void WaitAndResetFence(VkDevice device, VkFence fence);
    void WaitAndResetFences(VkDevice device, VkFence fence_A, VkFence fence_B);

    template<typename T> T    Align( const T& v, const T& alignment );
    template<typename T> bool IsPow2( const T& v );

    bool AreViewportsSame(const VkViewport &a, const VkViewport &b);

    bool IsAlmostZero( const float v[ 3 ] );
    bool IsAlmostZero(const QrFloat3D &v);
    bool IsAlmostZero(const QrMatrix3D &m);
    float Dot(const float a[3], const float b[3]);
    float Length(const float v[3]);
    void Normalize(float inout[3]);
    void Negate(float inout[3]);
    void Nullify(float inout[3]);
    void Cross(const float a[3], const float b[3], float r[3]);
    QrFloat3D GetUnnormalizedNormal(const QrFloat3D positions[3]);
    bool GetNormalAndArea(const QrFloat3D positions[3], QrFloat3D &normal, float &area);
    void SetMatrix3ToGLSLMat4(float dst[16], const QrMatrix3D &src);

    uint32_t GetPreviousByModulo(uint32_t value, uint32_t count);

    uint32_t GetWorkGroupCount(float size, uint32_t groupSize);
    uint32_t GetWorkGroupCount(uint32_t size, uint32_t groupSize);

    template< typename T1, typename T2 >
    requires( std::is_integral_v< T1 >&& std::is_integral_v< T2 > )
    uint32_t GetWorkGroupCountT( T1 size, T2 groupSize );
};

template<typename T>
constexpr T clamp(const T &v, const T &v_min, const T &v_max)
{
    assert(v_min <= v_max);
    return std::min(v_max, std::max(v_min, v));
}

template <typename T>
bool Utils::IsPow2(const T& v)
{
    static_assert(std::is_integral_v<T>);
    return (v != 0) && ((v & (v - 1)) == 0);
}

template <typename T>
T Utils::Align(const T& v, const T& alignment)
{
    static_assert(std::is_integral_v<T>);
    assert(IsPow2(alignment));

    return (v + alignment - 1) & ~(alignment - 1);
}

template< typename T1, typename T2 > requires( std::is_integral_v< T1 >&& std::is_integral_v< T2 > )
uint32_t Utils::GetWorkGroupCountT( T1 size, T2 groupSize )
{
    assert( size <= std::numeric_limits< uint32_t >::max() );
    assert( groupSize <= std::numeric_limits< uint32_t >::max() );

    return GetWorkGroupCount( static_cast< uint32_t >( size ),
                              static_cast< uint32_t >( groupSize ) );
}

}
