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

#ifndef SKY_MOTION_H
#define SKY_MOTION_H

vec2 getMotionForInfinitePoint(const vec3 rayDir)
{
    vec4 cur = globalUniform.projection * (globalUniform.view * vec4(rayDir, 0.0));
    vec4 prev = globalUniform.projectionPrev * (globalUniform.viewPrev * vec4(rayDir, 0.0));
    return (prev.xy / prev.w - cur.xy / cur.w) * 0.5;
}

vec2 getMotionForCloudLayer(const vec3 rayDir, const vec2 motionInfinite)
{
    const vec4 layer = globalUniform.cloudLayerMotion;
    if (globalUniform.skyType != SKY_TYPE_PROCEDURAL || layer.w <= 0.0 ||
        layer.y <= 0.0 || rayDir.z <= 1.0e-3)
    {
        return motionInfinite;
    }
    float distance = (layer.y + 0.3 * layer.z) / rayDir.z;
    vec2 wind = globalUniform.timeDelta * layer.x * vec2(30.0, 12.0);
    vec3 eyeDelta = globalUniform.cameraPosition.xyz - globalUniform.cameraPositionPrev.xyz;
    vec3 dirPrev = normalize(rayDir * distance + vec3(wind, 0.0) + eyeDelta);
    vec4 cur = globalUniform.projection * (globalUniform.view * vec4(rayDir, 0.0));
    vec4 prev = globalUniform.projectionPrev * (globalUniform.viewPrev * vec4(dirPrev, 0.0));
    return (prev.xy / prev.w - cur.xy / cur.w) * 0.5;
}

#endif
