#ifndef Q2_DIRECT_REUSE_HLSLI_
#define Q2_DIRECT_REUSE_HLSLI_

#define Q2_REUSE_RNG_SELECT 48u
#define Q2_REUSE_RNG_POINT 56u
#define Q2_REUSE_RNG_MEMBER 64u
#define Q2_REUSE_RNG_MERGE 72u

#define Q2_REUSE_TARGET_EPS 1e-6
#define Q2_REUSE_M_CAP 20u

#define Q2_REUSE_FLAG_VALID 1u
#define Q2_REUSE_FLAG_TAIL 2u

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
    return (uint)(clamp(v, 0.0, 1.0) * 65535.0);
}

float q2ReuseUnpack16(const uint v)
{
    return (float)(v & 0xFFFFu) * (1.0 / 65535.0);
}

uint q2ReuseFingerprint(const ShLightEncoded l)
{
    uint h = wellonsLowBias32(asuint(l.data_0.x) ^ 0x9E3779B9u);
    h = wellonsLowBias32(h ^ asuint(l.data_0.y));
    h = wellonsLowBias32(h ^ asuint(l.data_0.z));
    h = wellonsLowBias32(h ^ asuint(l.data_0.w));
    h = wellonsLowBias32(h ^ asuint(l.data_1.x));
    h = wellonsLowBias32(h ^ asuint(l.data_1.y));
    h = wellonsLowBias32(h ^ asuint(l.data_1.z));
    h = wellonsLowBias32(h ^ asuint(l.data_1.w));
    h = wellonsLowBias32(h ^ asuint(l.data_2.x));
    h = wellonsLowBias32(h ^ asuint(l.data_2.y));
    h = wellonsLowBias32(h ^ l.lightType);
    return h & 0xFFFFu;
}

float q2ReuseTargetAt(const Surface surf, const ShLightEncoded encoded, const float2 pointRnd, const float2 memberRnd)
{
    float memberPdf;
    const LightSample l = sampleLightNee(encoded, surf.position, pointRnd, memberRnd, memberPdf);

    float3 d, s;
    shade(surf, l, 1.0, d, s);

    return max(getLuminance(d + s), Q2_REUSE_TARGET_EPS);
}

void q2ReuseStore(const int2 pix, const Q2ReuseReservoir r)
{
    framebufQ2ReuseReservoir[pix] = uint4(
        (r.lightIndex & 0xFFFFu) | ((r.m & 0xFFFFu) << 16u),
        q2ReusePack16(r.pointRnd.x) | (q2ReusePack16(r.pointRnd.y) << 16u),
        q2ReusePack16(r.memberRnd) | (f32tof16(r.targetOwn) << 16u),
        asuint(r.ucw));

    framebufQ2ReuseReservoirMeta[pix] = uint4(
        (r.fingerprint & 0xFFFFu) | ((r.candidateHash & 0xFFFFu) << 16u),
        (r.cluster & 0xFFFFu) | ((r.slot & 0xFFu) << 16u) | ((r.flags & 0xFFu) << 24u),
        0u, 0u);
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
           r.lightIndex != (uint)LIGHT_INDEX_NONE &&
           r.m > 0u &&
           r.ucw > 0.0 && r.ucw < 1.0e9 && !isnan(r.ucw) && !isinf(r.ucw) &&
           r.targetOwn > 0.0 && !isnan(r.targetOwn) && !isinf(r.targetOwn);
}

#endif
