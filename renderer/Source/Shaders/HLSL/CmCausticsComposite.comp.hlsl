#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_CAUSTICS 2

#include "ShaderCommonHLSLFunc.hlsli"
#include "Caustics.hlsli"

[[vk::binding(0, DESC_SET_CAUSTICS)]] StructuredBuffer<CausticsParams_BT> causticsParams;
[[vk::binding(1, DESC_SET_CAUSTICS)]] StructuredBuffer<uint4> causticsCells;

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
    if (resolution == 0 || debugMode == CAUSTICS_DEBUG_OFF)
    {
        return;
    }

    const int sep = getCheckerboardSeparatorX();
    const int2 cbPix = getCheckerboardPix(pix);
    const int2 refrPix = (cbPix.x >= sep) ? cbPix : int2(cbPix.x + sep, cbPix.y);

    if (debugMode == CAUSTICS_DEBUG_MEDIA_REFR || debugMode == CAUSTICS_DEBUG_MEDIA_OWN)
    {
        const int2 mediaPix = (debugMode == CAUSTICS_DEBUG_MEDIA_REFR) ? refrPix : cbPix;
        const float encodedMedia = framebufMetallicRoughness_Sampled.Load(int3(mediaPix, 0)).z;
        const uint media = (uint)round(encodedMedia * 3.0);

        float3 mediaColor = float3(0.05, 0.05, 0.05);
        if (media == MEDIA_TYPE_WATER)
        {
            mediaColor = float3(0.0, 0.0, 1.0);
        }
        else if (media == MEDIA_TYPE_GLASS)
        {
            mediaColor = float3(0.0, 1.0, 0.0);
        }
        else if (media == MEDIA_TYPE_ACID)
        {
            mediaColor = float3(1.0, 0.0, 1.0);
        }

        framebufFinal[pix] = float4(mediaColor, 1.0);
        return;
    }

    const float4 surfacePosition = framebufSurfacePosition_Sampled.Load(int3(refrPix, 0));
    const float3 surfacePositionWorld = surfacePosition.xyz;
    const float2 gridCoords =
        (surfacePositionWorld.xy - params.gridMinAndTexel.xy) / params.gridMinAndTexel.z;

    const bool inDomain = gridCoords.x >= 0.0 && gridCoords.y >= 0.0 &&
                          gridCoords.x < (float)resolution && gridCoords.y < (float)resolution;

    const int lastCell = max((int)resolution - 2, 0);
    const int lastCenter = max((int)resolution - 1, 0);
    const float2 cellCenterCoords = clamp(gridCoords - 0.5, 0.0, (float)lastCenter);
    const int2 baseCell = clamp((int2)floor(cellCenterCoords), int2(0, 0), int2(lastCell, lastCell));
    const float2 f = saturate(cellCenterCoords - (float2)baseCell);

    uint4 c00;
    uint4 c10;
    uint4 c01;
    uint4 c11;

    if (resolution == 1)
    {
        c00 = causticsCells[0];
        c10 = c00;
        c01 = c00;
        c11 = c00;
    }
    else
    {
        const uint baseIndex = (uint)baseCell.y * resolution + (uint)baseCell.x;
        c00 = causticsCells[baseIndex];
        c10 = causticsCells[baseIndex + 1];
        c01 = causticsCells[baseIndex + resolution];
        c11 = causticsCells[baseIndex + resolution + 1];
    }

    const float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                            (1.0 - f.x) * f.y, f.x * f.y);

    const float3 cellFlux = ((float3)c00.xyz * w.x + (float3)c10.xyz * w.y +
                             (float3)c01.xyz * w.z + (float3)c11.xyz * w.w) / CAUSTICS_FLUX_SCALE;
    const float count = (float)c00.w * w.x + (float)c10.w * w.y +
                        (float)c01.w * w.z + (float)c11.w * w.w;

    if (debugMode == CAUSTICS_DEBUG_PREVIEW)
    {
        framebufFinal[pix] = float4(saturate(cellFlux), 1.0);
        return;
    }

    if (debugMode == CAUSTICS_DEBUG_PHOTONS)
    {
        const float present = count > 0.0 ? CAUSTICS_DEBUG_MARKER : 0.0;
        framebufFinal[pix] = float4(present, present, present, 1.0);
        return;
    }

    if (debugMode == CAUSTICS_DEBUG_GRID)
    {
        const float3 gridColor = inDomain
            ? float3(CAUSTICS_DEBUG_MARKER, count > 0.0 ? CAUSTICS_DEBUG_MARKER : 0.0, 0.0)
            : float3(0.0, 0.0, 0.0);
        framebufFinal[pix] = float4(gridColor, 1.0);
        return;
    }
}
