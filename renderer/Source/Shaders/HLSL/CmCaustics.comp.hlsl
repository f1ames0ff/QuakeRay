#define DESC_SET_TLAS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_VERTEX_DATA 2
#define DESC_SET_TEXTURES 3
#define DESC_SET_CAUSTICS 4
#define MATERIAL_MAX_ALBEDO_LAYERS 1

#include "ShaderCommonHLSLFunc.hlsli"
#include "Random.hlsli"
#include "VertexData.hlsli"
#include "RayCone.hlsli"
#include "Media.hlsli"
#include "Water.hlsli"

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
[[vk::binding(1, DESC_SET_CAUSTICS)]] RWStructuredBuffer<ShPhoton> causticsPhotons;
[[vk::binding(BINDING_ACCELERATION_STRUCTURE_MAIN, DESC_SET_TLAS)]] RaytracingAccelerationStructure topLevelAS;

#define CAUSTICS_RAY_MAX_LENGTH 100000.0
#define CAUSTICS_RAY_EPS 0.1
#define CAUSTICS_SECONDARY_RAY_EPS 0.1
#define CAUSTICS_SKY_SKIP_COUNT 4

static const uint CAUSTICS_INVALID_INDEX = 0xFFFFFFFFu;

struct CausticsHit
{
    ShTriangle shTriangle;
    float3     position;
    float3     normal;
    uint       instanceCustomIndex;
    uint       instanceIndex;
    uint       geometryIndex;
    uint       primitiveIndex;
    float      rayT;
};

bool causticsTrace(const float3 origin, const float3 direction, const uint cullMask, const float tMin,
                   out CausticsHit hit)
{
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;

    RayDesc rayDesc;
    rayDesc.Origin    = origin;
    rayDesc.Direction = direction;
    rayDesc.TMin      = tMin;
    rayDesc.TMax      = CAUSTICS_RAY_MAX_LENGTH;

    query.TraceRayInline(topLevelAS, RAY_FLAG_NONE, cullMask, rayDesc);

    while (query.Proceed())
    {
    }

    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return false;
    }

    hit.instanceIndex       = query.CommittedInstanceIndex();
    hit.instanceCustomIndex = query.CommittedInstanceID();
    hit.geometryIndex       = query.CommittedGeometryIndex();
    hit.primitiveIndex      = query.CommittedPrimitiveIndex();
    hit.rayT                = query.CommittedRayT();

    hit.shTriangle = getTriangle((int)hit.instanceIndex, (int)hit.instanceCustomIndex,
                                 (int)hit.geometryIndex, (int)hit.primitiveIndex);

    const float2 inBary = query.CommittedTriangleBarycentrics();
    const float3 bary   = float3(1.0 - inBary.x - inBary.y, inBary.x, inBary.y);

    hit.position = mul(hit.shTriangle.positions, bary);
    hit.normal   = normalize(mul(hit.shTriangle.normals, bary));

    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const CausticsParams_BT params = causticsParams[0];
    const uint resolution = params.gridSize.x;

    const uint2 cell = dispatchThreadID.xy;
    if (cell.x >= resolution || cell.y >= resolution)
    {
        return;
    }

    const uint photonIndex = cell.y * resolution + cell.x;

    ShPhoton photon;
    photon.position = (float4)0.0;
    photon.normal   = (float4)0.0;
    photon.flux     = (float4)0.0;

    const float texelSize = params.gridMinAndTexel.z;
    const float2 worldXZ  = params.gridMinAndTexel.xy + (float2(cell) + (float2)0.5) * texelSize;

    const float3 sunDirection = params.sunDirection.xyz;
    const float3 rayDirection = -sunDirection;

    const float3 rayOrigin = float3(worldXZ.x, params.gridMinAndTexel.w, worldXZ.y);

    CausticsHit waterHit;
    bool waterHitFound = false;

    for (uint skip = 0; skip < CAUSTICS_SKY_SKIP_COUNT; skip++)
    {
        CausticsHit hit;
        if (!causticsTrace(rayOrigin, rayDirection, globalUniform.rayCullMaskWorld | INSTANCE_MASK_REFRACT,
                           skip == 0 ? 0.0 : waterHit.rayT + CAUSTICS_RAY_EPS, hit))
        {
            break;
        }

        waterHit = hit;

        if ((hit.instanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_SKY) != 0)
        {
            continue;
        }

        waterHitFound = true;
        break;
    }

    if (waterHitFound)
    {
        if ((waterHit.shTriangle.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_WATER) != 0)
        {
            float3 waterNormal = waterHit.normal;
            if (dot(waterNormal, rayDirection) > 0.0)
            {
                waterNormal = -waterNormal;
            }

            RayCone rayCone;
            rayCone.width       = texelSize;
            rayCone.spreadAngle = 0.0;

            const float3 waveNormal = getWaterNormal(rayCone, rayDirection, waterNormal, waterHit.position, false);

            const float3 refractedDirection =
                refract(rayDirection, waveNormal, 1.0 / getIndexOfRefraction(MEDIA_TYPE_WATER));

            if (dot(refractedDirection, refractedDirection) > 0.0)
            {
                CausticsHit receiverHit;
                if (causticsTrace(waterHit.position + refractedDirection * CAUSTICS_SECONDARY_RAY_EPS,
                                  refractedDirection, globalUniform.rayCullMaskWorld, 0.0, receiverHit) &&
                    (receiverHit.instanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_SKY) == 0)
                {
                    float3 receiverNormal = receiverHit.normal;
                    if (dot(receiverNormal, refractedDirection) > 0.0)
                    {
                        receiverNormal = -receiverNormal;
                    }

                    const float fresnel = 0.1 + 0.9 * pow(1.0 - abs(dot(rayDirection, waveNormal)), 5.0);

                    photon.position = float4(receiverHit.position, 0.0);
                    photon.normal   = float4(receiverNormal, 0.0);
                    photon.flux     = float4(params.sunColor.rgb * params.sunDirection.w * (1.0 - fresnel), 0.0);
                }
            }
        }
    }

    causticsPhotons[photonIndex] = photon;
}
