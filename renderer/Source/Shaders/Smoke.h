// Copyright (c) 2026 QuakeRay contributors

#ifndef SMOKE_H_
#define SMOKE_H_

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

#define SMOKE_WIND_DIR      vec3(0.45, 0.20, 0.87)

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

float smokePhase(const vec3 toLight, const vec3 toViewer)
{
    const float g  = SMOKE_PHASE_G;
    const float g2 = g * g;

    const float cosTheta = dot(toLight, toViewer);
    const float denom    = 1.0 + g2 - 2.0 * g * cosTheta;

    const float norm = (1.0 - g2) / pow(1.0 + g2, 1.5);

    return ((1.0 - g2) / (denom * sqrt(max(denom, 1e-4)))) / norm;
}

float smokeHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.10, 0.20, 0.30));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float smokeValueNoise(vec3 x)
{
    const vec3 i = floor(x);
    const vec3 f = fract(x);
    const vec3 u = f * f * (3.0 - 2.0 * f);

    return mix(mix(mix(smokeHash(i + vec3(0.0, 0.0, 0.0)), smokeHash(i + vec3(1.0, 0.0, 0.0)), u.x),
                   mix(smokeHash(i + vec3(0.0, 1.0, 0.0)), smokeHash(i + vec3(1.0, 1.0, 0.0)), u.x), u.y),
               mix(mix(smokeHash(i + vec3(0.0, 0.0, 1.0)), smokeHash(i + vec3(1.0, 0.0, 1.0)), u.x),
                   mix(smokeHash(i + vec3(0.0, 1.0, 1.0)), smokeHash(i + vec3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

float smokeFbm(vec3 p)
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

SmokeField smokeSampleField(vec3 worldPos, float seed, float time, const SmokeLook look)
{
    const vec3 phase = vec3(seed * SMOKE_FEATURE_SEED, seed * SMOKE_FEATURE_SEED * 0.6, seed * SMOKE_FEATURE_SEED * 0.8);
    const vec3 drift = SMOKE_WIND_DIR * (look.windSpeed * time);
    const vec3 q     = worldPos + phase + drift;

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

#endif // SMOKE_H_
