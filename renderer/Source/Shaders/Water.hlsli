#ifndef WATER_HLSLI_
#define WATER_HLSLI_

float3 getWaterNormal(const RayCone rayCone, const float3 rayDir, const float3 normalGeom, const float3 position, bool wasPortal)
{
    const float3x3 basis = getONB(normalGeom);
    const float2 baseUV = float2(dot(position, getColumn(basis, 0)), dot(position, getColumn(basis, 1)));


    float verticality = 1.0 - abs(dot(normalGeom, globalUniform.worldUpVector.xyz));

    float2 flowSpeedVertical = 10 * float2(dot(getColumn(basis, 0), globalUniform.worldUpVector.xyz),
                                           dot(getColumn(basis, 1), globalUniform.worldUpVector.xyz));

    float2 flowSpeedHorizontal = (float2)1.0;


    const float uvScale = 0.05 / globalUniform.waterTextureAreaScale;
    float2 speed0 = uvScale * lerp(flowSpeedHorizontal, flowSpeedVertical, verticality) * globalUniform.waterWaveSpeed;
    float2 speed1 = -0.9 * speed0 * lerp(1.0, -0.1, verticality);


    float derivU = globalUniform.waterTextureDerivativesMultiplier * 0.5 * uvScale * getWaterDerivU(rayCone, rayDir, normalGeom);

    if (wasPortal)
    {
        derivU *= 0.1;
    }


    float2 uv0 = uvScale * baseUV + globalUniform.time * speed0;
    float3 n0 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv0, derivU).xyz;
    n0.xy = n0.xy * 2.0 - (float2)1.0;


    float2 uv1 = 0.8 * uvScale * baseUV + globalUniform.time * speed1;
    float3 n1 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv1, derivU).xyz;
    n1.xy = n1.xy * 2.0 - (float2)1.0;


    float2 uv2 = 0.1 * (uvScale * baseUV + speed0 * sin(globalUniform.time * 0.5));
    float3 n2 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv2, derivU).xyz;
    n2.xy = n2.xy * 2.0 - (float2)1.0;


    const float strength = globalUniform.waterWaveStrength;

    const float3 n = normalize(float3(0, 0, 1) + strength * (0.25 * n0 + 0.2 * n1 + 0.1 * n2));
    return mul(basis, n);
}


#endif
