#define DESC_SET_TLAS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_VERTEX_DATA 2
#define DESC_SET_TEXTURES 3
#define DESC_SET_CAUSTICS 4
#define MATERIAL_MAX_ALBEDO_LAYERS 1

#include "ShaderCommonHLSLFunc.hlsli"
#include "BRDF.hlsli"
#include "Random.hlsli"
#include "VertexData.hlsli"
#include "RayCone.hlsli"
#include "Media.hlsli"
#include "Water.hlsli"
#include "Caustics.hlsli"

[[vk::binding(0, DESC_SET_CAUSTICS)]] StructuredBuffer<CausticsParams_BT> causticsParams;
[[vk::binding(1, DESC_SET_CAUSTICS)]] RWStructuredBuffer<uint4> causticsCells;
[[vk::binding(BINDING_ACCELERATION_STRUCTURE_MAIN, DESC_SET_TLAS)]] RaytracingAccelerationStructure topLevelAS;

#define CAUSTICS_RAY_MAX_LENGTH 100000.0
#define CAUSTICS_RAY_EPS 0.1
#define CAUSTICS_SECONDARY_RAY_EPS 0.1
#define CAUSTICS_SKY_SKIP_COUNT 4
#define CAUSTICS_CELL_ADDEND_MAX 4294901760.0

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
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;

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

uint causticsSaturatingAdd(const float irradiance, const uint current)
{
    const uint addend = (uint)min(max(irradiance, 0.0) * CAUSTICS_FLUX_SCALE, CAUSTICS_CELL_ADDEND_MAX);
    return min(addend, 0xFFFFFFFFu - current);
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

    const float texelSize = params.gridMinAndTexel.z;
    const float2 worldXY  = params.gridMinAndTexel.xy + (float2(cell) + (float2)0.5) * texelSize;

    const float3 sunDirection = params.sunDirection.xyz;
    const float3 rayDirection = -sunDirection;

    const float3 rayOrigin = float3(worldXY.x, worldXY.y, params.gridMinAndTexel.w);

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

    if (!waterHitFound)
    {
        return;
    }

    if ((waterHit.shTriangle.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_WATER) == 0)
    {
        return;
    }

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

    if (dot(refractedDirection, refractedDirection) <= 0.0)
    {
        return;
    }

    CausticsHit receiverHit;
    if (!causticsTrace(waterHit.position + refractedDirection * CAUSTICS_SECONDARY_RAY_EPS,
                       refractedDirection, globalUniform.rayCullMaskWorld, 0.0, receiverHit))
    {
        return;
    }

    if ((receiverHit.instanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_SKY) != 0)
    {
        return;
    }

    const int2 receiverCell =
        (int2)floor((receiverHit.position.xy - params.gridMinAndTexel.xy) / texelSize);

    if (receiverCell.x < 0 || receiverCell.y < 0 ||
        receiverCell.x >= (int)resolution || receiverCell.y >= (int)resolution)
    {
        return;
    }

    const float cellArea = texelSize * texelSize;
    const float fresnel = getFresnelSchlick(1.0, getIndexOfRefraction(MEDIA_TYPE_WATER),
                                            rayDirection, waveNormal);
    const float cosTheta = max(0.0, -rayDirection.z);
    const float pathLength = length(receiverHit.position - waterHit.position);

    const float3 power = params.sunColor.rgb * cosTheta * cellArea *
                         (1.0 - fresnel) * getMediaTransmittance(MEDIA_TYPE_WATER, pathLength);

    const uint cellIndex = (uint)receiverCell.y * resolution + (uint)receiverCell.x;
    const float3 irradiance = power / cellArea;

    InterlockedAdd(causticsCells[cellIndex].x,
                   causticsSaturatingAdd(irradiance.r, causticsCells[cellIndex].x));
    InterlockedAdd(causticsCells[cellIndex].y,
                   causticsSaturatingAdd(irradiance.g, causticsCells[cellIndex].y));
    InterlockedAdd(causticsCells[cellIndex].z,
                   causticsSaturatingAdd(irradiance.b, causticsCells[cellIndex].z));
    InterlockedAdd(causticsCells[cellIndex].w, 1u);
}
