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

#include "qray/qray.h"

namespace qray
{
    namespace Matrix
    {
        void Multiply( float* result, const float* a, const float* b );
        void Inverse( float* inversed, const float* m );
        void Transpose( float* transposed, const float* m );
        void Transpose( float t[ 4 ][ 4 ] );

        void ToMat4( float* result, const QrTransform& m );
        void ToMat4Transposed( float* result, const QrTransform& m );

        void GetViewMatrix( float* result, const float* pos, float pitch, float yaw, float roll );
        void GetCubemapViewProjMat(
            float* result, uint32_t sideIndex, const float* position, float zNear, float zFar );
        // Set new position for viewer in (column-major) view matrix.
        void SetNewViewerPosition( float*       result,
                                   const float* viewMatrix,
                                   const float* newPosition );

        void MakeProjectionMatrix(
            float* matrix, float aspect, float fovYRad, float zNear, float zFar );
    }
}
