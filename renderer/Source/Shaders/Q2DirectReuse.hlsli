#ifndef Q2_DIRECT_REUSE_HLSLI_
#define Q2_DIRECT_REUSE_HLSLI_

#define Q2_REUSE_RNG_SELECT 48u
#define Q2_REUSE_RNG_POINT 56u
#define Q2_REUSE_RNG_MEMBER 64u
#define Q2_REUSE_RNG_MERGE 72u
#define Q2_REUSE_RNG_NEIGHBOR 80u

#define Q2_REUSE_TARGET_EPS 1e-6
#define Q2_REUSE_TARGET_MAX 60000.0
#define Q2_REUSE_M_CAP 20u
#define Q2_REUSE_M_CAP_MIN 1u
#define Q2_REUSE_NEIGHBOR_RADIUS 30.0

#define Q2_REUSE_FLAG_VALID 1u

struct Q2ReuseReservoir
{
    uint   lightIndex;
    uint   m;
    float2 pointRnd;
    float  memberRnd;
    float  targetOwn;
    float  ucw;
    uint   fingerprint;
    uint   candidateHash;
    uint   cluster;
    uint   slot;
    uint   flags;
};

uint q2ReusePack16(const float v)
{
    return (uint)round(clamp(v, 0.0, 1.0) * 65535.0);
}

float q2ReuseUnpack16(const uint v)
{
    return (float)(v & 0xFFFFu) * (1.0 / 65535.0);
}

uint q2ReuseFingerprint(const ShLightEncoded l)
{
    uint h = wellonsLowBias32(l.lightType ^ 0x9E3779B9u);
    h = wellonsLowBias32(h ^ asuint(l.data_0.x));
    h = wellonsLowBias32(h ^ asuint(l.data_0.y));
    h = wellonsLowBias32(h ^ asuint(l.data_0.z));

    if (l.lightType == LIGHT_TYPE_SPHERE || l.lightType == LIGHT_TYPE_SPOT)
    {
        h = wellonsLowBias32(h ^ asuint(l.data_0.w));
        h = wellonsLowBias32(h ^ asuint(l.data_1.x));
        h = wellonsLowBias32(h ^ asuint(l.data_1.y));
        h = wellonsLowBias32(h ^ asuint(l.data_1.z));
        h = wellonsLowBias32(h ^ asuint(l.data_2.x));
        h = wellonsLowBias32(h ^ asuint(l.data_2.y));
    }
    else if (l.lightType == LIGHT_TYPE_TRIANGLE)
    {
        h = wellonsLowBias32(h ^ asuint(l.data_0.w));
        h = wellonsLowBias32(h ^ asuint(l.data_1.x));
        h = wellonsLowBias32(h ^ asuint(l.data_1.y));
        h = wellonsLowBias32(h ^ asuint(l.data_1.z));
        h = wellonsLowBias32(h ^ asuint(l.data_1.w));
        h = wellonsLowBias32(h ^ asuint(l.data_2.x));
        h = wellonsLowBias32(h ^ asuint(l.data_2.y));
        h = wellonsLowBias32(h ^ asuint(l.data_2.z));
        h = wellonsLowBias32(h ^ asuint(l.data_2.w));
    }
    else if (l.lightType == LIGHT_TYPE_TEXTURED_AREA)
    {
        h = wellonsLowBias32(h ^ asuint(l.data_0.w));
        h = wellonsLowBias32(h ^ asuint(l.data_1.x));
        h = wellonsLowBias32(h ^ asuint(l.data_1.y));
        h = wellonsLowBias32(h ^ asuint(l.data_1.z));
        h = wellonsLowBias32(h ^ asuint(l.data_2.x));
        h = wellonsLowBias32(h ^ asuint(l.data_2.y));
        h = wellonsLowBias32(h ^ asuint(l.data_2.z));
        h = wellonsLowBias32(h ^ asuint(l.data_2.w));
        h = wellonsLowBias32(h ^ asuint(l.data_3.x));
        h = wellonsLowBias32(h ^ asuint(l.data_3.y));
        h = wellonsLowBias32(h ^ asuint(l.data_3.z));
        h = wellonsLowBias32(h ^ asuint(l.data_3.w));
        h = wellonsLowBias32(h ^ asuint(l.data_4.x));
        h = wellonsLowBias32(h ^ asuint(l.data_4.y));
        h = wellonsLowBias32(h ^ asuint(l.data_4.z));
        h = wellonsLowBias32(h ^ asuint(l.data_4.w));
        h = wellonsLowBias32(h ^ asuint(l.data_5.x));
        h = wellonsLowBias32(h ^ asuint(l.data_5.y));
        h = wellonsLowBias32(h ^ asuint(l.data_5.z));
        h = wellonsLowBias32(h ^ asuint(l.data_5.w));
        h = wellonsLowBias32(h ^ asuint(l.data_6.x));
        h = wellonsLowBias32(h ^ asuint(l.data_6.y));
        h = wellonsLowBias32(h ^ asuint(l.data_6.z));
        h = wellonsLowBias32(h ^ asuint(l.data_6.w));
        h = wellonsLowBias32(h ^ asuint(l.data_7.x));
        h = wellonsLowBias32(h ^ asuint(l.data_7.y));
        h = wellonsLowBias32(h ^ asuint(l.data_7.z));
        h = wellonsLowBias32(h ^ asuint(l.data_7.w));
    }
    else if (l.lightType == LIGHT_TYPE_DTAL_GROUP)
    {
        h = wellonsLowBias32(h ^ asuint(l.data_0.w));
        h = wellonsLowBias32(h ^ asuint(l.data_1.y));
        h = wellonsLowBias32(h ^ asuint(l.data_1.w));
        h = wellonsLowBias32(h ^ asuint(l.data_2.x));
        h = wellonsLowBias32(h ^ asuint(l.data_2.y));
        h = wellonsLowBias32(h ^ asuint(l.data_2.z));
        h = wellonsLowBias32(h ^ asuint(l.data_7.x));
        h = wellonsLowBias32(h ^ asuint(l.data_7.y));
        h = wellonsLowBias32(h ^ asuint(l.data_7.z));
    }

    h = wellonsLowBias32(h ^ asuint(l.coneCosInner));
    h = wellonsLowBias32(h ^ asuint(l.coneCosOuter));
    h = wellonsLowBias32(h ^ asuint(l.projector));

    return h & 0xFFFFu;
}

uint q2ReuseSampleHash(const uint lightIndex, const float2 pointRnd, const float memberRnd)
{
    uint h = wellonsLowBias32(lightIndex ^ 0x85EBCA6Bu);
    h = wellonsLowBias32(h ^ asuint(pointRnd.x));
    h = wellonsLowBias32(h ^ asuint(pointRnd.y));
    h = wellonsLowBias32(h ^ asuint(memberRnd));
    return h & 0xFFFFu;
}

float q2ReuseCapUcw(const float ucw, const uint cfg)
{
    const uint e = (cfg >> 24u) & 0xFFu;
    return (e > 0u) ? min(ucw, exp2((float)e)) : ucw;
}

uint2 q2ReuseEncodeDir(const float3 n)
{
    const float l1 = max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    float2 p = n.xy / l1;

    if (n.z < 0.0)
    {
        p = (1.0 - abs(p.yx)) * sign(p);
    }

    return uint2(q2ReusePack16(p.x * 0.5 + 0.5), q2ReusePack16(p.y * 0.5 + 0.5));
}

float3 q2ReuseDecodeDir(const uint2 e)
{
    const float2 f = float2(q2ReuseUnpack16(e.x), q2ReuseUnpack16(e.y)) * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));

    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    }

    return normalize(n);
}

LightSample q2ReuseSampleStored(const uint lightIndex, const float3 surfPosition, const float2 pointRnd, const float memberRnd, out float memberPdf)
{
    const ShLightEncoded encoded = lightSources[lightIndex];
    memberPdf = 1.0;

    if (encoded.lightType == LIGHT_TYPE_SPHERE)
    {
        const SphereLight sl = decodeAsSphereLight(encoded);
        const float3 lightNormal = q2ReuseDecodeDir(uint2(q2ReusePack16(pointRnd.x), q2ReusePack16(pointRnd.y)));

        LightSample r;
        r.position = sl.center + lightNormal * sl.radius;
        r.color = sl.color;
        r.dw = calcSolidAngleForSphere(sl.radius, length(sl.center - surfPosition));
        return r;
    }

    return sampleLightNee(encoded, surfPosition, pointRnd, float2(memberRnd, 0.0), memberPdf);
}

bool q2ReuseIsReceiverDependent(const ShLightEncoded l)
{
    if (l.projector > 0.5 &&
        (l.lightType == LIGHT_TYPE_TEXTURED_AREA || l.lightType == LIGHT_TYPE_DTAL_GROUP))
    {
        return true;
    }

    return false;
}

void q2ReuseStore(const int2 pix, const Q2ReuseReservoir r, const uint stateChanged)
{
    framebufQ2ReuseReservoir[pix] = uint4(
        (r.lightIndex & 0xFFFFu) | ((r.m & 0xFFFFu) << 16u),
        q2ReusePack16(r.pointRnd.x) | (q2ReusePack16(r.pointRnd.y) << 16u),
        q2ReusePack16(r.memberRnd) | (f32tof16(r.targetOwn) << 16u),
        asuint(r.ucw));

    framebufQ2ReuseReservoirMeta[pix] = uint4(
        (r.fingerprint & 0xFFFFu) | ((r.candidateHash & 0xFFFFu) << 16u),
        (r.cluster & 0xFFFFu) | ((r.slot & 0xFFu) << 16u) | ((r.flags & 0xFFu) << 24u),
        stateChanged,
        0u);
}

bool q2ReuseLoadPrev(const int2 pix, out Q2ReuseReservoir r)
{
    const uint4 a = framebufQ2ReuseReservoir_Prev_Sampled.Load(int3(pix, 0));
    const uint4 b = framebufQ2ReuseReservoirMeta_Prev_Sampled.Load(int3(pix, 0));

    r.lightIndex = a.x & 0xFFFFu;
    r.m = (a.x >> 16u) & 0xFFFFu;
    r.pointRnd = float2(q2ReuseUnpack16(a.y), q2ReuseUnpack16(a.y >> 16u));
    r.memberRnd = q2ReuseUnpack16(a.z);
    r.targetOwn = f16tof32(a.z >> 16u);
    r.ucw = asfloat(a.w);
    r.fingerprint = b.x & 0xFFFFu;
    r.candidateHash = (b.x >> 16u) & 0xFFFFu;
    r.cluster = b.y & 0xFFFFu;
    r.slot = (b.y >> 16u) & 0xFFu;
    r.flags = (b.y >> 24u) & 0xFFu;

    return (r.flags & Q2_REUSE_FLAG_VALID) != 0u &&
           ((r.flags >> 2u) & 0x3Fu) == ((globalUniform.frameId + 63u) & 0x3Fu) &&
           r.lightIndex != (uint)LIGHT_INDEX_NONE &&
           r.m > 0u &&
           r.ucw > 0.0 && r.ucw < 1.0e9 && !isnan(r.ucw) && !isinf(r.ucw) &&
           r.targetOwn > 0.0 && !isnan(r.targetOwn) && !isinf(r.targetOwn);
}

#endif
