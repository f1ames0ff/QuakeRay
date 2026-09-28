static const float CLOUD_SHADOW_FADE = 0.04;

bool cloudShadowUV(float3 worldPos, float3 sunDir, float4 mapPlacement, out float2 uv, out float blend)
{
    uv = float2(0.0, 0.0);
    blend = 0.0;

    if (mapPlacement.x <= 0.5 || sunDir.z <= 1.0e-3)
    {
        return false;
    }

    float2 flatPos = worldPos.xy - sunDir.xy * (worldPos.z / sunDir.z);
    float2 mapped = (flatPos - mapPlacement.yz) / max(mapPlacement.w, 1.0);

    float2 fade = smoothstep(float2(0.0, 0.0), float2(CLOUD_SHADOW_FADE, CLOUD_SHADOW_FADE), mapped) *
                  (float2(1.0, 1.0) - smoothstep(float2(1.0 - CLOUD_SHADOW_FADE, 1.0 - CLOUD_SHADOW_FADE), float2(1.0, 1.0), mapped));
    blend = fade.x * fade.y;
    if (blend <= 0.0)
    {
        return false;
    }

    uv = clamp(mapped, float2(0.0, 0.0), float2(1.0, 1.0));
    return true;
}

float cloudShadowTauFromUV(Texture3D<float> shadowVolume, SamplerState shadowVolume_Sampler, float2 uv, float height)
{
    uint width, heightTexels, depth;
    shadowVolume.GetDimensions(width, heightTexels, depth);

    float slices = (float)depth;
    float z = clamp(height, 0.0, 1.0) * (slices - 1.0) / slices + 0.5 / slices;

    return shadowVolume.SampleLevel(shadowVolume_Sampler, float3(uv, z), 0.0).r;
}

float cloudShadowTau(Texture3D<float> shadowVolume, SamplerState shadowVolume_Sampler, float3 worldPos, float3 sunDir, float4 mapPlacement, float height, out float blend)
{
    float2 uv;

    if (!cloudShadowUV(worldPos, sunDir, mapPlacement, uv, blend))
    {
        blend = 0.0;

        return -1.0;
    }

    return cloudShadowTauFromUV(shadowVolume, shadowVolume_Sampler, uv, height);
}

float cloudShadowTauNear(Texture3D<float> shadowVolume, SamplerState shadowVolume_Sampler, float3 worldPos, float3 sunDir, float4 mapPlacement, float height)
{
    if (mapPlacement.x <= 0.5 || sunDir.z <= 1.0e-3)
    {
        return 0.0;
    }

    float2 flatPos = worldPos.xy - sunDir.xy * (worldPos.z / sunDir.z);
    float2 mapped = (flatPos - mapPlacement.yz) / max(mapPlacement.w, 1.0);

    return cloudShadowTauFromUV(shadowVolume, shadowVolume_Sampler, clamp(mapped, float2(0.0, 0.0), float2(1.0, 1.0)), height);
}

float cloudShadowTransmittance(Texture3D<float> shadowVolume, SamplerState shadowVolume_Sampler, float3 worldPos, float3 sunDir, float4 mapPlacement, float height)
{
    float2 uv;
    float blend;

    if (!cloudShadowUV(worldPos, sunDir, mapPlacement, uv, blend))
    {
        return 1.0;
    }

    return lerp(1.0, exp(-cloudShadowTauFromUV(shadowVolume, shadowVolume_Sampler, uv, height)), blend);
}
