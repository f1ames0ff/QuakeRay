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

#define VERTEX_BUFFER_WRITEABLE
#define DESC_SET_GLOBAL_UNIFORM 0
#define DESC_SET_VERTEX_DATA 1
#include "ShaderCommonHLSLFunc.hlsli"

[[vk::constant_id(0)]] const uint preprocessMode = VERT_PREPROC_MODE_ONLY_DYNAMIC;

[[vk::push_constant]] ConstantBuffer<ShVertPreprocessing> push;

[numthreads(COMPUTE_VERT_PREPROC_GROUP_SIZE_X, 1, 1)]
void main(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID)
{
    uint tlasInstanceIndex = groupID.x;
    bool isDynamic = (push.tlasInstanceIsDynamicBits[tlasInstanceIndex / 32] & (1u << tlasInstanceIndex)) != 0;


    if (isDynamic)
    {
        #define VERTEX_PREPROCESS_PARTIAL_DYNAMIC
        #include "VertexPreprocessPartial.hlsli"
    }

    else if (preprocessMode == VERT_PREPROC_MODE_ALL)
    {
        #define VERTEX_PREPROCESS_PARTIAL_STATIC_ALL
        #include "VertexPreprocessPartial.hlsli"
    }
}
