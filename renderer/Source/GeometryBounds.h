#pragma once

#include "qray/qray.h"

#include <algorithm>
#include <cmath>

#if defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define QR_GEOMETRY_BOUNDS_SSE2
#endif

namespace qray
{

inline void AccumulateGeometryBoundsScalar(const QrGeometryUploadInfo &info, bool &initialized,
                                          float minimum[3], float maximum[3])
{
    for (uint32_t i = 0; i < info.vertexCount; ++i)
    {
        const float *source = info.pVertices[i].position;
        float point[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            point[axis] = info.transform.matrix[axis][0] * source[0] +
                          info.transform.matrix[axis][1] * source[1] +
                          info.transform.matrix[axis][2] * source[2] +
                          info.transform.matrix[axis][3];
        }
        if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2]) ||
            std::abs(point[0]) > 1.0e7f || std::abs(point[1]) > 1.0e7f || std::abs(point[2]) > 1.0e7f)
        {
            continue;
        }
        if (!initialized)
        {
            initialized = true;
            std::copy(point, point + 3, minimum);
            std::copy(point, point + 3, maximum);
        }
        else
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                minimum[axis] = std::min(minimum[axis], point[axis]);
                maximum[axis] = std::max(maximum[axis], point[axis]);
            }
        }
    }
}

inline void AccumulateGeometryBounds(const QrGeometryUploadInfo &info, bool &initialized,
                                    float minimum[3], float maximum[3])
{
#if defined(QR_GEOMETRY_BOUNDS_SSE2)
    if (info.vertexCount == 0)
    {
        return;
    }

    const auto &matrix = info.transform.matrix;
    const __m128 column0 = _mm_setr_ps(matrix[0][0], matrix[1][0], matrix[2][0], matrix[0][0]);
    const __m128 column1 = _mm_setr_ps(matrix[0][1], matrix[1][1], matrix[2][1], matrix[0][1]);
    const __m128 column2 = _mm_setr_ps(matrix[0][2], matrix[1][2], matrix[2][2], matrix[0][2]);
    const __m128 translation = _mm_setr_ps(matrix[0][3], matrix[1][3], matrix[2][3], matrix[0][3]);
    const __m128 limit = _mm_set1_ps(1.0e7f);
    const __m128 sign = _mm_set1_ps(-0.0f);
    const __m128i exponentMask = _mm_set1_epi32(0x7f800000);
    __m128 lower = initialized ? _mm_setr_ps(minimum[0], minimum[1], minimum[2], 0.0f) : _mm_setzero_ps();
    __m128 upper = initialized ? _mm_setr_ps(maximum[0], maximum[1], maximum[2], 0.0f) : _mm_setzero_ps();

    for (uint32_t i = 0; i < info.vertexCount; ++i)
    {
        const float *source = info.pVertices[i].position;
        const __m128 point = _mm_add_ps(
            _mm_add_ps(_mm_add_ps(_mm_mul_ps(column0, _mm_set1_ps(source[0])),
                                  _mm_mul_ps(column1, _mm_set1_ps(source[1]))),
                       _mm_mul_ps(column2, _mm_set1_ps(source[2]))),
            translation);
        const __m128i exponent = _mm_and_si128(_mm_castps_si128(point), exponentMask);
        if ((_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(exponent, exponentMask))) & 7) != 0)
        {
            continue;
        }
        if ((_mm_movemask_ps(_mm_cmple_ps(_mm_andnot_ps(sign, point), limit)) & 7) != 7)
        {
            continue;
        }
        if (!initialized)
        {
            initialized = true;
            lower = upper = point;
        }
        else
        {
            const __m128 lowerMask = _mm_cmplt_ps(point, lower);
            const __m128 upperMask = _mm_cmpgt_ps(point, upper);
            lower = _mm_or_ps(_mm_and_ps(lowerMask, point), _mm_andnot_ps(lowerMask, lower));
            upper = _mm_or_ps(_mm_and_ps(upperMask, point), _mm_andnot_ps(upperMask, upper));
        }
    }

    if (initialized)
    {
        minimum[0] = _mm_cvtss_f32(lower);
        minimum[1] = _mm_cvtss_f32(_mm_shuffle_ps(lower, lower, _MM_SHUFFLE(1, 1, 1, 1)));
        minimum[2] = _mm_cvtss_f32(_mm_shuffle_ps(lower, lower, _MM_SHUFFLE(2, 2, 2, 2)));
        maximum[0] = _mm_cvtss_f32(upper);
        maximum[1] = _mm_cvtss_f32(_mm_shuffle_ps(upper, upper, _MM_SHUFFLE(1, 1, 1, 1)));
        maximum[2] = _mm_cvtss_f32(_mm_shuffle_ps(upper, upper, _MM_SHUFFLE(2, 2, 2, 2)));
    }
#else
    AccumulateGeometryBoundsScalar(info, initialized, minimum, maximum);
#endif
}

}

#if defined(QR_GEOMETRY_BOUNDS_SSE2)
#undef QR_GEOMETRY_BOUNDS_SSE2
#endif
