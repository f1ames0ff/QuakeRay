// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>

// HLSL counterpart of Smoke.h. The whole header is pure arithmetic: it declares no descriptor, no
// texture and no matrix, and it includes nothing itself.
//
// Spellings that had to change:
//   * vec2/vec3 -> float2/float3. The one constructor that could look like a scalar one,
//     SMOKE_WIND_DIR's vec3(0.45, 0.20, 0.87), carries three distinct components and keeps them
//     in the same order, spelled float3(0.45, 0.20, 0.87).
//   * mix -> lerp and fract -> frac, the same substitutions Smoke's value noise needs and every
//     other port of the base makes.
//   * the #ifndef SMOKE_H_ / #define SMOKE_H_ guard -> #ifndef SMOKE_HLSLI_ / #define SMOKE_HLSLI_,
//     the mechanism of the golden with the extension of this base.
//
// What did not change: the member sets and order of SmokeLook and SmokeField, every SMOKE_*
// constant and its value, the phase function with its pow(1.0 + g2, 1.5) normalization and its
// max(denom, 1e-4) guard, the hash constants and the two fract applications, the value noise with
// its smoothstep weights u = f * f * (3.0 - 2.0 * f), the three fbm octaves with their amplitude
// halving and the 2.03 frequency step, the / 0.875 of the sum, and the field sampling with its
// per-channel phase multipliers 1.0 / 0.6 / 0.8, the wind drift, the shape/medium/detail scales,
// the 0.62 / 0.26 / 0.12 mix and the displacement weights SMOKE_DISPLACE_SHAPE/MEDIUM.

#ifndef SMOKE_HLSLI_
#define SMOKE_HLSLI_

struct SmokeLook
{
    float shapeScale;
    float mediumScale;
    float detailScale;
    float windSpeed;
    float displace;
    float breakup;
    float edgePower;
    float edgeGain;
};

#define SMOKE_WIND_DIR      float3(0.45, 0.20, 0.87)

#define SMOKE_FEATURE_SEED    0.35

#define SMOKE_DISPLACE_SHAPE   0.75
#define SMOKE_DISPLACE_MEDIUM  0.50

#define SMOKE_ERODE_LOW     0.38
#define SMOKE_ERODE_HIGH    0.74

#define SMOKE_EDGE_FIELD    0.6

#define SMOKE_DEPTH_FADE    24.0

#define SMOKE_LIFE_POWER    1.0

#define SMOKE_LIGHT_STRENGTH 0.35
#define SMOKE_AMBIENT_GAIN   0.35
#define SMOKE_MIN_LIGHT      0.01
#define SMOKE_PHASE_G        0.35

float smokePhase(const float3 toLight, const float3 toViewer)
{
    const float g  = SMOKE_PHASE_G;
    const float g2 = g * g;

    const float cosTheta = dot(toLight, toViewer);
    const float denom    = 1.0 + g2 - 2.0 * g * cosTheta;

    const float norm = (1.0 - g2) / pow(1.0 + g2, 1.5);

    return ((1.0 - g2) / (denom * sqrt(max(denom, 1e-4)))) / norm;
}

float smokeHash(float3 p)
{
    p = frac(p * 0.3183099 + float3(0.10, 0.20, 0.30));
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float smokeValueNoise(float3 x)
{
    const float3 i = floor(x);
    const float3 f = frac(x);
    const float3 u = f * f * (3.0 - 2.0 * f);

    return lerp(lerp(lerp(smokeHash(i + float3(0.0, 0.0, 0.0)), smokeHash(i + float3(1.0, 0.0, 0.0)), u.x),
                     lerp(smokeHash(i + float3(0.0, 1.0, 0.0)), smokeHash(i + float3(1.0, 1.0, 0.0)), u.x), u.y),
                lerp(lerp(smokeHash(i + float3(0.0, 0.0, 1.0)), smokeHash(i + float3(1.0, 0.0, 1.0)), u.x),
                     lerp(smokeHash(i + float3(0.0, 1.0, 1.0)), smokeHash(i + float3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

float smokeFbm(float3 p)
{
    float sum = 0.0;
    float amplitude = 0.5;

    for (int i = 0; i < 3; i++)
    {
        sum += smokeValueNoise(p) * amplitude;
        p *= 2.03;
        amplitude *= 0.5;
    }

    return sum / 0.875;
}

struct SmokeField
{
    float shape;
    float medium;
    float detail;
    float mixed;
};

SmokeField smokeSampleField(float3 worldPos, float seed, float time, const SmokeLook look)
{
    const float3 phase = float3(seed * SMOKE_FEATURE_SEED, seed * SMOKE_FEATURE_SEED * 0.6, seed * SMOKE_FEATURE_SEED * 0.8);
    const float3 drift = SMOKE_WIND_DIR * (look.windSpeed * time);
    const float3 q     = worldPos + phase + drift;

    SmokeField f;
    f.shape  = smokeFbm(q * look.shapeScale);
    f.medium = smokeValueNoise(q * look.mediumScale + 13.7);
    f.detail = smokeValueNoise(q * look.detailScale + 41.3);

    f.mixed = f.shape * 0.62 + f.medium * 0.26 + f.detail * 0.12;
    return f;
}

float smokeFieldDisplacement(SmokeField f, const SmokeLook look)
{
    const float shape  = (f.shape - 0.5) * 2.0 * SMOKE_DISPLACE_SHAPE;
    const float medium = (f.medium - 0.5) * 2.0 * SMOKE_DISPLACE_MEDIUM;

    return (shape + medium) * look.displace;
}

#endif // SMOKE_HLSLI_
