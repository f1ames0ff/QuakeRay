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

#ifndef RAY_CLEARANCE_HLSLI_
#define RAY_CLEARANCE_HLSLI_

#define RAY_CLEARANCE_BIAS 0.01

/* How far the ray runs before it meets the first surface it would otherwise
   hide behind, no farther than maxDistance: the virtual point past that
   surface would start the next ray inside it and tear the junction. */
float traceClearance(float3 origin, float3 direction, float maxDistance, uint cullMask)
{
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;

    RayDesc rayDesc;
    rayDesc.Origin = origin;
    rayDesc.Direction = direction;
    rayDesc.TMin = 0.001;
    rayDesc.TMax = maxDistance;

    query.TraceRayInline(topLevelAS, getAdditionalRayFlags(), cullMask, rayDesc);

    while (query.Proceed())
    {
    }

    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT
        ? max(query.CommittedRayT() - RAY_CLEARANCE_BIAS, 0.0)
        : maxDistance;
}

#endif
