struct CmSkyCloudsParams_BT
{
    float4 faceBasis[18];
    float4 sunDirection;
    float4 skyColor;
    float4 skyParams;
    float4 cloudColor;
    float4 cloudParams;
    float4 sunDiscColor;
    float4 cloudLayer;
    float4 cloudMarch;
    float4 cloudAnchor;
    float4 cloudShadowPlacement;
};

[[vk::binding(3, 0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> cloudCubemapOut;
[[vk::binding(5, 0)]] Texture3D<float> cloudShadowMap;
[[vk::binding(6, 0)]] SamplerState cloudShadowMap_Sampler;
[[vk::binding(1, 0)]] ConstantBuffer<CmSkyCloudsParams_BT> params;

#include "CloudLayer.hlsli"
#include "CloudShadowMap.hlsli"

static const float CLOUD_POWDER = 8.0;
static const float CLOUD_STEP_GROWTH = 1.045;
static const float CLOUD_MAX_MARCH_SPAN = 12.0;

float henyeyGreenstein(float mu, float g)
{
    float g2 = g * g;
    float d = max(1.0 + g2 - 2.0 * g * mu, 1.0e-4);
    return (1.0 - g2) / (d * sqrt(d));
}

float cloudPhase(float mu, float g)
{
    return lerp(henyeyGreenstein(mu, g), henyeyGreenstein(mu, -g * 0.35), 0.5);
}

float cloudMultiScatter(float mu, float sunDepth, float g)
{
    float sum = 0.0;
    float energy = 0.5;

    for (int order = 1; order <= 3; order++)
    {
        float o = (float)order;
        float gOrder = g * (1.0 - 0.3 * o);
        float phase = lerp(henyeyGreenstein(mu, gOrder), henyeyGreenstein(mu, -gOrder), 0.5);
        sum += energy * phase * exp(-sunDepth / (1.0 + o));
        energy *= 0.5;
    }

    return sum;
}

float cloudSunTauFromMap(CloudLayer layer, float3 p, float3 sunDir, out float blend)
{
    const float4 placement = params.cloudShadowPlacement;
    const float3 worldPos = float3(p.xy, params.cloudAnchor.z + p.z);
    const float height = clamp((p.z - layer.altitude) / layer.thickness, 0.0, 1.0);

    return cloudShadowTau(cloudShadowMap, cloudShadowMap_Sampler, worldPos, sunDir, placement, height, blend);
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int3 ipos = (int3)dispatchThreadID;

    uint sizeX, sizeY, elements;
    cloudCubemapOut.GetDimensions(sizeX, sizeY, elements);
    const int2 size = int2(sizeX, sizeY);

    if (ipos.x >= size.x || ipos.y >= size.y || ipos.z >= 6)
    {
        return;
    }

    int face = ipos.z;
    float3 right   = params.faceBasis[face * 3 + 0].xyz;
    float3 up      = params.faceBasis[face * 3 + 1].xyz;
    float3 forward = params.faceBasis[face * 3 + 2].xyz;

    float2 ndc = ((float2)ipos.xy + 0.5) / (float2)size * 2.0 - 1.0;
    float3 dir = normalize(ndc.x * right + (-ndc.y) * up + forward);

    float opacity  = clamp(params.skyParams.y, 0.0, 1.0);
    float sunLight = params.sunDirection.w;

    CloudLayer layer;
    layer.altitude  = max(params.cloudLayer.x, 1.0);
    layer.thickness = max(params.cloudLayer.y, 1.0);
    layer.coverage  = clamp(params.cloudParams.x, 0.0, 0.99);
    layer.density   = max(params.cloudParams.y, 0.0);
    layer.detail    = clamp(params.cloudMarch.z, 0.0, 0.9);
    layer.speed     = params.cloudParams.z;
    layer.time      = params.cloudColor.w;

    int viewSteps = clamp((int)(params.cloudMarch.x + 0.5), 8, 256);
    float phaseG  = clamp(params.cloudMarch.w, 0.0, 0.95);

    uint volumeX, volumeY, volumeZ;
    cloudShadowMap.GetDimensions(volumeX, volumeY, volumeZ);
    int sunSlices = (int)volumeZ;

    float3 albedo = max(params.cloudColor.xyz, float3(0.0, 0.0, 0.0));

    float3 sunColor = params.sunDiscColor.xyz * max(params.cloudLayer.z, 0.0) * sunLight;
    float3 skyLight = max(params.skyColor.xyz, float3(0.0, 0.0, 0.0)) * max(params.cloudLayer.w, 0.0);

    float3 inscatter = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;

    bool lit = sunColor.r + sunColor.g + sunColor.b > 0.0 || skyLight.r + skyLight.g + skyLight.b > 0.0;

    const float baseSpan = layer.altitude / max(dir.z, 1.0e-3);

    if (dir.z > 0.0 && lit && opacity > 0.0 && params.cloudParams.w > 0.5 &&
        layer.coverage < 1.0 && layer.density > 0.0)
    {
        float tIn = layer.altitude / dir.z;
        float tOut = min((layer.altitude + layer.thickness) / dir.z,
                         tIn + layer.thickness * CLOUD_MAX_MARCH_SPAN);

        float span = (pow(CLOUD_STEP_GROWTH, (float)viewSteps) - 1.0) / (CLOUD_STEP_GROWTH - 1.0);
        float dt = (tOut - tIn) / span;

        float2 seedPos = params.cloudAnchor.xy + dir.xy * baseSpan;
        const float seedScale = CLOUD_SEED_SCALE * (layer.altitude / CLOUD_REFERENCE_ALTITUDE);
        float jitter = valueNoise3(float3(seedPos, 0.0) / seedScale);
        float t = tIn;
        float step = dt;

        float3 sunDir = normalize(params.sunDirection.xyz);
        float mu = dot(dir, sunDir);

        for (int i = 0; i < viewSteps; i++)
        {
            float offset = frac(jitter + (float)i * CLOUD_SEQUENCE_STEP);
            float ts = t + step * offset;

            float3 p = float3(dir.x * ts + params.cloudAnchor.x,
                              dir.y * ts + params.cloudAnchor.y,
                              dir.z * ts);
            float density = cloudDensity(layer, p, true);

            if (density > 1.0e-3)
            {
                float sigma = cloudOpticalDepth(layer, density);

                float sunTau = 0.0;
                float sunVisibility = 1.0;
                if (sunLight > 0.5)
                {
                    float blend;
                    float mapTau = cloudSunTauFromMap(layer, p, sunDir, blend);
                    float marchTau = -1.0;
                    if (blend < 1.0)
                    {
                        marchTau = cloudOpticalDepth(layer, cloudSunDepth(layer, p, sunDir, CLOUD_SHADOW_STEPS, sunSlices));
                    }
                    sunTau = (marchTau >= 0.0) ? lerp(marchTau, max(mapTau, 0.0), blend) : mapTau;
                    sunVisibility = exp(-sunTau);
                }

                float powder = 1.0 - exp(-density * CLOUD_POWDER);
                float rim = lerp(1.0, powder, clamp(-mu, 0.0, 1.0));

                float h = (p.z - layer.altitude) / layer.thickness;
                float solid = density / max(cloudHeightProfile(h), 1.0e-3);
                float skyShield = exp(-solid * layer.density * CLOUD_EXTINCTION * (1.0 - h));

                float3 light = sunColor * (sunVisibility * cloudPhase(mu, phaseG) * rim +
                                           cloudMultiScatter(mu, sunTau, phaseG)) +
                               skyLight * skyShield;

                float stepTransmittance = exp(-sigma * step);
                inscatter += transmittance * light * (1.0 - stepTransmittance);
                transmittance *= stepTransmittance;

                if (transmittance < 0.01)
                {
                    break;
                }
            }

            t += step;
            step *= CLOUD_STEP_GROWTH;
        }
    }

    cloudCubemapOut[int3(ipos.xy, face)] = float4(inscatter * albedo * opacity, lerp(1.0, transmittance, opacity));
}
