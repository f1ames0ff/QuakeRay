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

[[vk::binding(0, DESC_SET_CAUSTICS)]] StructuredBuffer<CausticsParams_BT> causticsParams;
[[vk::binding(1, DESC_SET_CAUSTICS)]] StructuredBuffer<uint4> causticsCells;
[[vk::binding(2, DESC_SET_CAUSTICS)]] StructuredBuffer<uint> causticsCellDepth;

#define CAUSTICS_DEBUG_MARKER 0.5
#define CAUSTICS_FLUX_SCALE 256.0
#define CAUSTICS_DEPTH_SCALE 16.0
#define CAUSTICS_DEPTH_BIAS 32768.0

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

    const int sep = getCheckerboardSeparatorX();
    const int2 cbPix = getCheckerboardPix(pix);
    const int2 refrPix = (cbPix.x >= sep) ? cbPix : int2(cbPix.x + sep, cbPix.y);

    const float4 surfacePosition = framebufSurfacePosition_Sampled.Load(int3(refrPix, 0));
    const float3 surfacePositionWorld = surfacePosition.xyz;
    const float2 gridCoords =
        (surfacePositionWorld.xy - params.gridMinAndTexel.xy) / params.gridMinAndTexel.z;

    const int2 cell = (int2)floor(gridCoords);
    const bool inGrid =
        cell.x >= 0 && cell.y >= 0 && cell.x + 1 < (int)resolution && cell.y + 1 < (int)resolution;

    if (debugMode == 2)
    {
        framebufFinal[pix] += float4(inGrid ? CAUSTICS_DEBUG_MARKER : 0.0, 0.0, 0.0, 0.0);
    }

    if (!inGrid)
    {
        return;
    }

    const uint cell00 = (uint)cell.y * resolution + (uint)cell.x;
    const uint4 c00 = causticsCells[cell00];
    const uint4 c10 = causticsCells[cell00 + 1];
    const uint4 c01 = causticsCells[cell00 + resolution];
    const uint4 c11 = causticsCells[cell00 + resolution + 1];

    const uint d00 = causticsCellDepth[cell00];
    const uint d10 = causticsCellDepth[cell00 + 1];
    const uint d01 = causticsCellDepth[cell00 + resolution];
    const uint d11 = causticsCellDepth[cell00 + resolution + 1];

    const float2 f = frac(gridCoords);
    const float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                           (1.0 - f.x) * f.y, f.x * f.y);

    const float4 counts = float4(c00.w, c10.w, c01.w, c11.w);
    const float4 validW = w * step(0.5, counts);

    const float3 flux =
        ((float3)c00.xyz * validW.x + (float3)c10.xyz * validW.y +
         (float3)c01.xyz * validW.z + (float3)c11.xyz * validW.w) / CAUSTICS_FLUX_SCALE;

    const float count = counts.x * validW.x + counts.y * validW.y +
                        counts.z * validW.z + counts.w * validW.w;

    if (debugMode == 3)
    {
        const float3 preview = flux * 0.25;
        framebufFinal[pix] += float4(preview, 0.0);
        return;
    }

    if (debugMode == 1)
    {
        framebufFinal[pix] += float4(count > 0.0 ? CAUSTICS_DEBUG_MARKER : 0.0,
                                     count > 0.0 ? CAUSTICS_DEBUG_MARKER : 0.0,
                                     count > 0.0 ? CAUSTICS_DEBUG_MARKER : 0.0, 0.0);
        return;
    }

    if (count <= 0.0)
    {
        if (debugMode == 2)
        {
            framebufFinal[pix] += float4(0.0, 0.0, 0.0, 0.0);
        }
        return;
    }

    const float zSum = (float)d00 * validW.x + (float)d10 * validW.y +
                       (float)d01 * validW.z + (float)d11 * validW.w;
    const float cellMeanZ = (zSum / count) / CAUSTICS_DEPTH_SCALE - CAUSTICS_DEPTH_BIAS;

    const float zTolerance = max(params.gridMinAndTexel.z * 4.0, 16.0);
    const float coverage = saturate(1.0 - abs(cellMeanZ - surfacePositionWorld.z) / zTolerance);

    if (debugMode == 2)
    {
        framebufFinal[pix] += float4(0.0, CAUSTICS_DEBUG_MARKER,
                                     coverage > 0.0 ? CAUSTICS_DEBUG_MARKER : 0.0, 0.0);
        return;
    }

    if (coverage <= 0.0)
    {
        return;
    }

    const float3 albedo = framebufAlbedo_Sampled.Load(int3(getRegularPixFromCheckerboardPix(refrPix), 0)).rgb;

    framebufFinal[pix] += float4(flux * coverage * albedo, 0.0);
}
