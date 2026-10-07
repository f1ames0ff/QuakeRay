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

#ifndef RAY_CLEARANCE_H_
#define RAY_CLEARANCE_H_

#define RAY_CLEARANCE_BIAS 0.01

/* How far the ray runs before it meets the first surface it would otherwise
   hide behind, no farther than maxDistance: the virtual point past that
   surface would start the next ray inside it and tear the junction. */
float traceClearance(vec3 origin, vec3 direction, float maxDistance, uint cullMask)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query,
                          topLevelAS,
                          getAdditionalRayFlags() | gl_RayFlagsOpaqueEXT,
                          cullMask,
                          origin,
                          0.001,
                          direction,
                          maxDistance);

    while (rayQueryProceedEXT(query))
    {
    }

    return rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT
        ? max(rayQueryGetIntersectionTEXT(query, true) - RAY_CLEARANCE_BIAS, 0.0)
        : maxDistance;
}

#endif
