#ifndef SKY_MOTION_HLSLI
#define SKY_MOTION_HLSLI

float2 getMotionForInfinitePoint(const float3 rayDir)
{
    float4 cur = mul(globalUniform.projection, mul(globalUniform.view, float4(rayDir, 0.0)));
    float4 prev = mul(globalUniform.projectionPrev, mul(globalUniform.viewPrev, float4(rayDir, 0.0)));
    return (prev.xy / prev.w - cur.xy / cur.w) * 0.5;
}

float2 getMotionForCloudLayer(const float3 rayDir, const float2 motionInfinite)
{
    const float4 layer = globalUniform.cloudLayerMotion;
    if (globalUniform.skyType != SKY_TYPE_PROCEDURAL || layer.w <= 0.0 ||
        layer.y <= 0.0 || rayDir.z <= 1.0e-3)
    {
        return motionInfinite;
    }
    float distance = (layer.y + 0.3 * layer.z) / rayDir.z;
    float2 wind = globalUniform.timeDelta * layer.x * float2(30.0, 12.0);
    float3 eyeDelta = globalUniform.cameraPosition.xyz - globalUniform.cameraPositionPrev.xyz;
    float3 dirPrev = normalize(rayDir * distance + float3(wind, 0.0) + eyeDelta);
    float4 cur = mul(globalUniform.projection, mul(globalUniform.view, float4(rayDir, 0.0)));
    float4 prev = mul(globalUniform.projectionPrev, mul(globalUniform.viewPrev, float4(dirPrev, 0.0)));
    return (prev.xy / prev.w - cur.xy / cur.w) * 0.5;
}

#endif
