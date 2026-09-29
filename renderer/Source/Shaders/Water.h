#ifndef WATER_H_
#define WATER_H_

vec3 getWaterNormal(const RayCone rayCone, const vec3 rayDir, const vec3 normalGeom, const vec3 position, bool wasPortal)
{
    const mat3 basis = getONB(normalGeom);
    const vec2 baseUV = vec2(dot(position, basis[0]), dot(position, basis[1])); 


    // how much vertical flow to apply
    float verticality = 1.0 - abs(dot(normalGeom, globalUniform.worldUpVector.xyz));

    // project basis[0] and basis[1] on up vector
    vec2 flowSpeedVertical = 10 * vec2(dot(basis[0], globalUniform.worldUpVector.xyz), 
                                       dot(basis[1], globalUniform.worldUpVector.xyz));

    vec2 flowSpeedHorizontal = vec2(1.0);


    const float uvScale = 0.05 / globalUniform.waterTextureAreaScale;
    vec2 speed0 = uvScale * mix(flowSpeedHorizontal, flowSpeedVertical, verticality) * globalUniform.waterWaveSpeed;
    vec2 speed1 = -0.9 * speed0 * mix(1.0, -0.1, verticality);


    // for texture sampling
    float derivU = globalUniform.waterTextureDerivativesMultiplier * 0.5 * uvScale * getWaterDerivU(rayCone, rayDir, normalGeom);

    // make water sharper if visible through the portal
    if (wasPortal)
    {
        derivU *= 0.1;
    }


    vec2 uv0 = uvScale * baseUV + globalUniform.time * speed0;
    vec3 n0 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv0, derivU).xyz;
    n0.xy = n0.xy * 2.0 - vec2(1.0);


    vec2 uv1 = 0.8 * uvScale * baseUV + globalUniform.time * speed1;
    vec3 n1 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv1, derivU).xyz;
    n1.xy = n1.xy * 2.0 - vec2(1.0);


    vec2 uv2 = 0.1 * (uvScale * baseUV + speed0 * sin(globalUniform.time * 0.5));
    vec3 n2 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv2, derivU).xyz;
    n2.xy = n2.xy * 2.0 - vec2(1.0);


    const float strength = globalUniform.waterWaveStrength;

    const vec3 n = normalize(vec3(0, 0, 1) + strength * (0.25 * n0 + 0.2 * n1 + 0.1 * n2));
    return basis * n;   
}

#endif
