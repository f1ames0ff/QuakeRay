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

#include "Matrix.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <utility>

using namespace vkpt;

namespace
{
    constexpr float PI = 3.141592653589793238462643383279f;

    float Dot3(const float *a, const float *b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    float Minor2(const float *m, int row0, int row1, int col0, int col1)
    {
        return m[row0 * 4 + col0] * m[row1 * 4 + col1] -
               m[row0 * 4 + col1] * m[row1 * 4 + col0];
    }

    float Minor3(const float *m, int skipRow, int skipCol)
    {
        int rows[3];
        int cols[3];
        int rowCount = 0;
        int colCount = 0;

        for (int i = 0; i < 4; i++)
        {
            if (i != skipRow)
            {
                rows[rowCount++] = i;
            }

            if (i != skipCol)
            {
                cols[colCount++] = i;
            }
        }

        return m[rows[0] * 4 + cols[0]] * Minor2(m, rows[1], rows[2], cols[1], cols[2]) -
               m[rows[0] * 4 + cols[1]] * Minor2(m, rows[1], rows[2], cols[0], cols[2]) +
               m[rows[0] * 4 + cols[2]] * Minor2(m, rows[1], rows[2], cols[0], cols[1]);
    }
}

void Matrix::Inverse(float *inversed, const float *m)
{
    float cofactors[16];

    for (int row = 0; row < 4; row++)
    {
        for (int col = 0; col < 4; col++)
        {
            const float minor = Minor3(m, row, col);
            const float sign = ((row + col) % 2 == 0) ? 1.0f : -1.0f;

            cofactors[col * 4 + row] = sign * minor;
        }
    }

    const float det =
        m[0] * cofactors[0] +
        m[1] * cofactors[4] +
        m[2] * cofactors[8] +
        m[3] * cofactors[12];

    const float invDet = 1.0f / det;

    for (int i = 0; i < 16; i++)
    {
        inversed[i] = cofactors[i] * invDet;
    }
}

void Matrix::Transpose( float* transposed, const float* m )
{
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            transposed[ i * 4 + j ] = m[ j * 4 + i ];
        }
    }
}

void Matrix::Transpose(float t[4][4])
{
    std::swap(t[0][1], t[1][0]);
    std::swap(t[0][2], t[2][0]);
    std::swap(t[0][3], t[3][0]);

    std::swap(t[1][2], t[2][1]);
    std::swap(t[1][3], t[3][1]);

    std::swap(t[2][3], t[3][2]);
}

void Matrix::Multiply(float *result, const float *a, const float *b)
{
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            result[i * 4 + j] =
                a[i * 4 + 0] * b[0 * 4 + j] +
                a[i * 4 + 1] * b[1 * 4 + j] +
                a[i * 4 + 2] * b[2 * 4 + j] +
                a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
}

void Matrix::ToMat4(float *result, const RgTransform &m)
{
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            result[i * 4 + j] = m.matrix[i][j];
        }

        result[i * 4 + 3] = m.matrix[i][3];
    }

    result[12] = 0.0f;
    result[13] = 0.0f;
    result[14] = 0.0f;
    result[15] = 1.0f;
}

void Matrix::ToMat4Transposed(float *result, const RgTransform &m)
{
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            result[j * 4 + i] = m.matrix[i][j];
        }

        result[12 + i] = m.matrix[i][3];
    }

    result[3] = 0.0f;
    result[7] = 0.0f;
    result[11] = 0.0f;
    result[15] = 1.0f;
}

void Matrix::GetViewMatrix(float *result, const float *pos, float pitch, float yaw, float roll)
{
    const float sinYaw = std::sin(yaw);
    const float cosYaw = std::cos(yaw);
    const float sinPitch = std::sin(pitch);
    const float cosPitch = std::cos(pitch);
    const float sinRoll = std::sin(roll);
    const float cosRoll = std::cos(roll);

    const float rotation[3][3] =
    {
        {
            cosYaw * cosRoll + sinPitch * sinYaw * sinRoll,
            cosPitch * sinRoll,
            sinPitch * cosYaw * sinRoll - sinYaw * cosRoll,
        },
        {
            sinPitch * sinYaw * cosRoll - cosYaw * sinRoll,
            cosPitch * cosRoll,
            sinPitch * cosYaw * cosRoll + sinYaw * sinRoll,
        },
        {
            cosPitch * sinYaw,
            -sinPitch,
            cosPitch * cosYaw,
        },
    };

    const float invTranslation[3] = { -pos[0], -pos[1], -pos[2] };

    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            result[i * 4 + j] = rotation[j][i];
        }

        result[12 + i] = Dot3(rotation[i], invTranslation);
    }

    result[3] = 0.0f;
    result[7] = 0.0f;
    result[11] = 0.0f;
    result[15] = 1.0f;
}

void Matrix::GetCubemapViewProjMat(float *result, uint32_t sideIndex, const float *position, float zNear, float zFar)
{
    constexpr float FACE_ANGLES[6][2] =
    {
        { 0.0f,       PI / 2.0f },
        { 0.0f,      -PI / 2.0f },
        { -PI / 2.0f, 0.0f      },
        { PI / 2.0f,  0.0f      },
        { 0.0f,       0.0f      },
        { 0.0f,       PI        },
    };

    assert(sideIndex < 6);

    if (sideIndex >= 6)
    {
        return;
    }

    float view[16];
    GetViewMatrix(view, position, FACE_ANGLES[sideIndex][0], FACE_ANGLES[sideIndex][1], 0.0f);

    const float tanHalfFovy = 1.0f;
    const float aspect = 1.0f;

    float proj[16] = {};
    proj[0] = 1.0f / (aspect * tanHalfFovy);
    proj[5] = 1.0f / tanHalfFovy;
    proj[10] = zFar / (zFar - zNear);
    proj[11] = 1.0f;
    proj[14] = -(zFar * zNear) / (zFar - zNear);
    proj[5] *= -1.0f;

    Multiply(result, view, proj);
}

void Matrix::SetNewViewerPosition(float *result, const float *viewMatrix, const float *newPosition)
{
    memcpy(result, viewMatrix, sizeof(float) * 16);

    const float invTranslation[3] = { -newPosition[0], -newPosition[1], -newPosition[2] };

    const float columnI[3] = { viewMatrix[0 * 4 + 0], viewMatrix[1 * 4 + 0], viewMatrix[2 * 4 + 0] };
    const float columnJ[3] = { viewMatrix[0 * 4 + 1], viewMatrix[1 * 4 + 1], viewMatrix[2 * 4 + 1] };
    const float columnK[3] = { viewMatrix[0 * 4 + 2], viewMatrix[1 * 4 + 2], viewMatrix[2 * 4 + 2] };

    result[3 * 4 + 0] = Dot3(columnI, invTranslation);
    result[3 * 4 + 1] = Dot3(columnJ, invTranslation);
    result[3 * 4 + 2] = Dot3(columnK, invTranslation);
}

void Matrix::MakeProjectionMatrix( float *matrix, float aspect, float fovYRad, float zNear, float zFar )
{
    const float tanHalfFovY = tanf( fovYRad * 0.5f );

    memset( matrix, 0, 16 * sizeof( float ) );

    matrix[ 0 * 4 + 0 ] = 1.0f / ( aspect * tanHalfFovY );

    matrix[ 1 * 4 + 1 ] = -1.0f / tanHalfFovY;

    matrix[ 2 * 4 + 2 ] = zFar / ( zNear - zFar );
    matrix[ 2 * 4 + 3 ] = -1.0f;

    matrix[ 3 * 4 + 2 ] = ( zFar * zNear ) / ( zNear - zFar );
}
