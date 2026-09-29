#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_CAUSTICS 2

#include "ShaderCommonHLSLFunc.hlsli"

struct CausticsParams_BT
{
    float4 sunDirection;
    float4 sunColor;
    float4 gridMinAndTexel;
    uint4  gridSize;
};

struct ShPhoton
{
    float4 position;
    float4 normal;
    float4 flux;
};

[[vk::binding(0, DESC_SET_CAUSTICS)]] StructuredBuffer<CausticsParams_BT> causticsParams;
[[vk::binding(1, DESC_SET_CAUSTICS)]] StructuredBuffer<ShPhoton> causticsPhotons;

#define CAUSTICS_DEBUG_MARKER 0.5

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= globalUniform.renderWidth || dispatchThreadID.y >= globalUniform.renderHeight)
    {
        return;
    }

    const int2 pix = int2(dispatchThreadID.xy);

    const CausticsParams_BT params = causticsParams[0];
    const uint resolution = params.gridSize.x;
    const uint debugMode = params.gridSize.y;
    if (resolution == 0)
    {
        return;
    }

    const float4 surfacePosition = framebufSurfacePosition_Sampled.Load(int3(getCheckerboardPix(pix), 0));
    const float3 surfacePositionWorld = surfacePosition.xyz;
    const float2 gridCoords =
        (surfacePositionWorld.xy - params.gridMinAndTexel.xy) / params.gridMinAndTexel.z;

    const int2 cell = (int2)floor(gridCoords);
    const bool inGrid =
        cell.x >= 0 && cell.y >= 0 && cell.x + 2 <= (int)resolution && cell.y + 2 <= (int)resolution;

    if (debugMode == 2)
    {
        framebufFinal[pix] += float4(inGrid ? CAUSTICS_DEBUG_MARKER : 0.0, 0.0, 0.0, 0.0);
        if (!inGrid)
        {
            return;
        }
    }
    else if (!inGrid)
    {
        return;
    }

    const uint photon00 = (uint)cell.y * resolution + (uint)cell.x;
    const ShPhoton p00 = causticsPhotons[photon00];
    const ShPhoton p10 = causticsPhotons[photon00 + 1];
    const ShPhoton p01 = causticsPhotons[photon00 + resolution];
    const ShPhoton p11 = causticsPhotons[photon00 + resolution + 1];

    const float2 f = frac(gridCoords);
    const float4 weights = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                                  (1.0 - f.x) * f.y, f.x * f.y);

    const float3 flux = p00.flux.rgb * weights.x + p10.flux.rgb * weights.y +
                        p01.flux.rgb * weights.z + p11.flux.rgb * weights.w;

    const float stageMax = max(max(p00.flux.w, p10.flux.w), max(p01.flux.w, p11.flux.w));
    const bool hasFlux = dot(flux, flux) > 0.0;

    if (debugMode == 3)
    {
        framebufFinal[pix] += float4(stageMax * 0.2, stageMax * 0.2, stageMax * 0.2, 0.0);
        return;
    }

    if (debugMode == 2)
    {
        framebufFinal[pix] += float4(0.0, stageMax >= 4.0 ? CAUSTICS_DEBUG_MARKER : 0.0,
                                     stageMax >= 2.0 ? CAUSTICS_DEBUG_MARKER : 0.0, 0.0);
        if (stageMax < 4.0)
        {
            return;
        }
    }
    else if (!hasFlux)
    {
        return;
    }

    if (debugMode == 1)
    {
        framebufFinal[pix] += float4(CAUSTICS_DEBUG_MARKER, CAUSTICS_DEBUG_MARKER, CAUSTICS_DEBUG_MARKER, 0.0);
        return;
    }

    const float3 receiverPosition = p00.position.xyz * weights.x + p10.position.xyz * weights.y +
                                    p01.position.xyz * weights.z + p11.position.xyz * weights.w;

    const float3 edgeX = p10.position.xyz - p00.position.xyz;
    const float3 edgeY = p01.position.xyz - p00.position.xyz;
    const float footprintArea = max(length(cross(edgeX, edgeY)), 0.01);

    const float footprintRadius =
        max(max(length(edgeX), length(edgeY)), params.gridMinAndTexel.z);
    const float coverage =
        saturate(1.0 - length(receiverPosition - surfacePositionWorld) / footprintRadius * 2.0);

    if (coverage <= 0.0)
    {
        return;
    }

    const float3 albedo = framebufAlbedo_Sampled.Load(int3(pix, 0)).rgb;

    const float3 caustics = flux / footprintArea * coverage * albedo;

    framebufFinal[pix] += float4(caustics, 0.0);
}
