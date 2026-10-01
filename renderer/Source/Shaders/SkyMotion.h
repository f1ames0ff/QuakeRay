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
        (layer.y > 0.0 && rayDir.z <= 1.0e-3))
    {
        return motionInfinite;
    }
    vec3 dirPrev;
    if (layer.y <= 0.0)
    {
        dirPrev = normalize(rayDir + globalUniform.timeDelta * layer.x / 3.0 * vec3(1.0, 0.4, 0.0));
    }
    else
    {
        float distance = (layer.y + 0.3 * layer.z) / rayDir.z;
        vec2 wind = globalUniform.timeDelta * layer.x * vec2(30.0, 12.0);
        vec3 eyeDelta = globalUniform.cameraPosition.xyz - globalUniform.cameraPositionPrev.xyz;
        dirPrev = normalize(rayDir * distance + vec3(wind, 0.0) + eyeDelta);
    }
    vec4 cur = globalUniform.projection * (globalUniform.view * vec4(rayDir, 0.0));
    vec4 prev = globalUniform.projectionPrev * (globalUniform.viewPrev * vec4(dirPrev, 0.0));
    return (prev.xy / prev.w - cur.xy / cur.w) * 0.5;
}

#endif
