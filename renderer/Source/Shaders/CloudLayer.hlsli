static const float CLOUD_VERTICAL_STRETCH = 2.0;
static const float CLOUD_FREQUENCY = 2.0;
static const int   CLOUD_OCTAVES = 3;
static const int   CLOUD_DETAIL_OCTAVES = 2;
static const float CLOUD_DETAIL_FREQUENCY = 4.5;
static const float CLOUD_NOISE_MEAN = 0.5;
static const float CLOUD_NOISE_SIGMA = 0.12;
static const float CLOUD_SHAPE_POWER = 1.6;
static const float CLOUD_BASE_RAKE = 0.15;
static const float CLOUD_EXTINCTION = 8.0;
static const float CLOUD_LIGHT_CONE = 0.15;
static const int   CLOUD_SUN_STEPS_MAX = 64;
static const int   CLOUD_SHADOW_STEPS = 32;
static const float CLOUD_SEQUENCE_STEP = 0.6180339887;
static const float CLOUD_SEED_SCALE = 256.0;
static const float CLOUD_REFERENCE_ALTITUDE = 1400.0;

struct CloudLayer
{
    float altitude;
    float thickness;
    float coverage;
    float density;
    float detail;
    float time;
    float speed;
};

float3 cloudSunSample(float3 p, float3 sunDir, float dist, float stepSize)
{
    float3 helper = abs(sunDir.z) < 0.99 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(helper, sunDir));
    float3 bitangent = cross(sunDir, tangent);

    float angle = dist / max(stepSize, 1.0e-3) * 2.39996323;
    float radius = CLOUD_LIGHT_CONE * dist * 0.5;

    return p + sunDir * dist + (cos(angle) * tangent + sin(angle) * bitangent) * radius;
}

float hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float valueNoise3(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = hash13(i);
    float n100 = hash13(i + float3(1.0, 0.0, 0.0));
    float n010 = hash13(i + float3(0.0, 1.0, 0.0));
    float n110 = hash13(i + float3(1.0, 1.0, 0.0));
    float n001 = hash13(i + float3(0.0, 0.0, 1.0));
    float n101 = hash13(i + float3(1.0, 0.0, 1.0));
    float n011 = hash13(i + float3(0.0, 1.0, 1.0));
    float n111 = hash13(i + float3(1.0, 1.0, 1.0));

    return lerp(
        lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
        lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y),
        f.z);
}

float cloudNoise(float3 p, int octaves)
{
    float v = 0.0;
    float amp = 0.5;
    float norm = 0.0;

    for (int i = 0; i < octaves; i++)
    {
        v += amp * valueNoise3(p);
        norm += amp;
        p = p * 2.03 + float3(1.7, 9.2, 3.9);
        amp *= 0.5;
    }

    return v / norm;
}

float cloudShape(float3 p, int octaves)
{
    float n = cloudNoise(p, octaves);
    float z = (n - CLOUD_NOISE_MEAN) / CLOUD_NOISE_SIGMA;
    return clamp(0.5 + 0.5 * z / sqrt(1.0 + z * z), 0.0, 1.0);
}

float cloudHeightProfile(float h)
{
    const float base = 0.12;
    const float top = 0.55;
    return smoothstep(0.0, base, h) * (1.0 - smoothstep(top, 1.0, h));
}

float cloudDensity(CloudLayer layer, float3 p, bool detail)
{
    float h = (p.z - layer.altitude) / layer.thickness;
    if (h <= 0.0 || h >= 1.0)
    {
        return 0.0;
    }

    float2 wind = float2(layer.time * layer.speed * 30.0, layer.time * layer.speed * 12.0);
    float frequency = CLOUD_FREQUENCY / layer.thickness;

    float shape = cloudShape(float3(p.xy + wind, p.z / CLOUD_VERTICAL_STRETCH) * frequency, CLOUD_OCTAVES);

    float profile = cloudHeightProfile(h + shape * CLOUD_BASE_RAKE);
    if (profile <= 0.0)
    {
        return 0.0;
    }

    float d = (shape - layer.coverage) / max(1.0 - layer.coverage, 1.0e-3);
    d = pow(clamp(d, 0.0, 1.0), CLOUD_SHAPE_POWER);
    if (d <= 0.0)
    {
        return 0.0;
    }

    if (detail)
    {
        float3 q = float3(p.xy + wind, p.z * 0.75 / CLOUD_VERTICAL_STRETCH) * frequency * CLOUD_DETAIL_FREQUENCY;
        float erosion = layer.detail * cloudShape(q, CLOUD_DETAIL_OCTAVES);
        d = clamp((d - erosion) / max(1.0 - erosion, 1.0e-3), 0.0, 1.0);
    }

    return d * profile;
}

float cloudOpticalDepth(CloudLayer layer, float column)
{
    return column * layer.density * CLOUD_EXTINCTION / layer.thickness;
}

float cloudSunDepth(CloudLayer layer, float3 p, float3 sunDir, int steps, int slices)
{
    steps = clamp(steps, 1, CLOUD_SUN_STEPS_MAX);
    slices = max(slices, 2);

    float height = clamp((p.z - layer.altitude) / layer.thickness, 0.0, 1.0);

    float dt = layer.thickness / max(sunDir.z, 1.0e-3) / (float)steps;
    float3 base = p + sunDir * ((layer.altitude - p.z) / max(sunDir.z, 1.0e-3));

    float slice = height * (float)(slices - 1);
    float low = floor(slice) / (float)(slices - 1);
    float high = min(low + 1.0 / (float)(slices - 1), 1.0);
    float through = slice - floor(slice);

    float columnLow = 0.0;
    float columnHigh = 0.0;

    for (int i = 0; i < steps; i++)
    {
        float column = cloudDensity(layer, cloudSunSample(base, sunDir, ((float)i + 0.5) * dt, dt), false) * dt;
        float step = (float)(i + 1);
        columnLow  += column * clamp(step - low * (float)steps, 0.0, 1.0);
        columnHigh += column * clamp(step - high * (float)steps, 0.0, 1.0);
    }

    return cloudOpticalDepth(layer, lerp(columnLow, columnHigh, through));
}
