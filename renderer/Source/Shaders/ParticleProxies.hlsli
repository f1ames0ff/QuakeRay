// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>

// The traced half of the particle-through-glass feature, for the Q2 reflect/refract raygen only
// (the resource declaration is what pins the set-5 layout of that pipeline): one inline ray query
// along the segment the raygen already traced, the intersection of the frame's stand-ins (the
// classic sprite's ray-facing triangle and the FTE effects' exact world-space triangles) and the
// shading of the raster copy, folded into `q2Transparent` with the operator the raster copy's
// blend state implies. The raster sprite stays the primary visibility source; the raster discard
// (RsParticle.frag, RsWorld.frag) hides the copy that lies behind a pane.

#ifndef PARTICLE_PROXIES_HLSLI_
#define PARTICLE_PROXIES_HLSLI_

#if defined(Q2_REFL_REFR_SHADER)

// `topLevelAS` is RaygenCommon.hlsli's set-0 declaration, which every include site of this header
// has already pulled in; SmokeLight.hlsli must not declare its own.
#define SMOKE_LIGHT_EXTERNAL_TLAS

#include "Smoke.hlsli"
#include "Q2ClusterLights.hlsli"
#include "SmokeLight.hlsli"

// Like DESC_SET_RANDOM in RaygenPrimary.hlsli this is the position itself: the reflect/refract
// pipeline's fifth layout is this module's one-item proxy layout.
#define DESC_SET_PARTICLE_PROXIES 5

// The tenth layout carries the engine's tonemapping array, for the unlit stand-ins' exposure.
#define DESC_SET_PARTICLE_TONEMAPPING 10

// The kinds and blend operators of ParticleProxies.h, mirrored field for field.
#define PARTICLE_PROXY_KIND_BILLBOARD 0u
#define PARTICLE_PROXY_KIND_TRIANGLE 1u
#define PARTICLE_BLEND_ALPHA_OVER 0u
#define PARTICLE_BLEND_PREMUL 1u
#define PARTICLE_BLEND_ADD_ALPHA 2u
#define PARTICLE_BLEND_ADD_COLOR 3u
#define PARTICLE_BLEND_MUL_INV_ALPHA 4u

struct ShParticleProxy
{
    float3 center;
    float  radius;
    float  legRight;
    float  legUp;
    uint   packedColor;
    uint   textureIndex;
    uint   cluster;
    float  direct;
    float  gain;
    float  lightFloor;
    float3 v0;
    uint   blendOp;
    float3 edge1;
    uint   kind;
    float3 edge2;
    uint   colorB;
    float2 uv0;
    float2 uv1;
    float2 uv2;
    uint   colorC;
    uint   unused;
};

[[vk::binding(0, DESC_SET_PARTICLE_PROXIES)]] StructuredBuffer<ShParticleProxy> particleProxies;

[[vk::binding(0, DESC_SET_PARTICLE_TONEMAPPING)]] StructuredBuffer<ShTonemapping> particleTonemapping;

struct ShParticleHit
{
    float3 color;
    float  opacity;
    uint   blendOp;
    bool   valid;
};

float3 unpackParticleColor(const uint packed)
{
    const uint3 bytes = uint3(packed & 0xFFu, (packed >> 8u) & 0xFFu, (packed >> 16u) & 0xFFu);
    return float3(bytes) / 255.0;
}

// The sprite the record describes is the raster triangle r_part.c emits: its centroid, a right
// isoceles triangle with the two legs (UV corners (0,0), (1,0) and (0,1) at the leg ends). The
// reconstruction turns it to face the ray: the plane is the one through the centroid whose normal
// is the direction, the two legs lie in a basis built from the direction and a world-up hint, and
// the local triangle coordinates are the same thirds of the legs the capture stored - so a sprite
// seen through a pane or a mirror shows its front, whatever direction the ray came from.
bool intersectParticleBillboard(const ShParticleProxy proxy, const float3 origin, const float3 dir,
                                const float tMin, const float tMax, out float tHit, out float2 uv)
{
    tHit = 0.0;
    uv = (float2)0.0;

    if (dot(dir, dir) < 1e-8)
    {
        return false;
    }

    const float3 d = normalize(dir);
    const float planeT = dot(proxy.center - origin, d);

    if (planeT < tMin || planeT > tMax)
    {
        return false;
    }

    const float3 upHint = abs(d.z) < 0.99 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 legBasisR = normalize(cross(d, upHint));
    const float3 legBasisU = cross(legBasisR, d);

    const float3 rel = origin + planeT * d - proxy.center;
    const float2 local = float2(dot(rel, legBasisR) / max(proxy.legRight, 1e-4),
                                dot(rel, legBasisU) / max(proxy.legUp, 1e-4));

    // The centroid sits a third of each leg from the right-angle corner: the triangle is the
    // (0,0)-(1,0)-(0,1) affine image of the local basis, so the UVs are the affine coordinates.
    const float a = local.x + (1.0 / 3.0);
    const float b = local.y + (1.0 / 3.0);

    if (a < 0.0 || b < 0.0 || a + b > 1.0)
    {
        return false;
    }

    tHit = planeT;
    uv = float2(a, b);
    return true;
}

// The FTE effects are exact world-space triangles, so the ray meets the stored geometry itself -
// the same shape the raster draw shows, seen from the ray's own direction. The material is
// two-sided (the raster overlay draws with culling off), and the barycentrics carry the per-vertex
// UVs and palette colours.
bool intersectParticleTriangle(const ShParticleProxy proxy, const float3 origin, const float3 dir,
                               const float tMin, const float tMax, out float tHit, out float3 bary)
{
    tHit = 0.0;
    bary = (float3)0.0;

    const float3 h = cross(dir, proxy.edge2);
    const float det = dot(proxy.edge1, h);

    if (abs(det) < 1e-8)
    {
        return false;
    }

    const float invDet = 1.0 / det;
    const float3 s = origin - proxy.v0;
    const float u = dot(s, h) * invDet;

    if (u < 0.0 || u > 1.0)
    {
        return false;
    }

    const float3 q = cross(s, proxy.edge1);
    const float v = dot(dir, q) * invDet;

    if (v < 0.0 || u + v > 1.0)
    {
        return false;
    }

    const float t = dot(proxy.edge2, q) * invDet;

    if (t < tMin || t > tMax)
    {
        return false;
    }

    tHit = t;
    bary = float3(1.0 - u - v, u, v);
    return true;
}

float3 particleProxyAmbient(const int2 cbPix)
{
    const int2 lfPix = int2(clamp(cbPix / Q2_GRAD_DWN, (int2)0,
                                  int2(globalUniform.renderWidth, globalUniform.renderHeight) / Q2_GRAD_DWN -
                                      (int2)1));

    Q2SH lf;
    lf.shY = framebufQ2AtrousPingLF_SH_Sampled.Load(int3(lfPix, 0));
    lf.CoCg = framebufQ2AtrousPingLF_COCG_Sampled.Load(int3(lfPix, 0)).xy;
    lf.shY /= Q2_STORAGE_SCALE_LF;
    lf.CoCg /= Q2_STORAGE_SCALE_LF;

    return q2SHToIrradiance(lf, (float3)0.0) * SMOKE_AMBIENT_GAIN;
}

// The one inline query of the pixel: the same shadow-ray flags-and-commit shape SmokeLight.hlsli
// uses, over the particle instance the TLAS carries behind INSTANCE_MASK_PARTICLE - no scene ray's
// cull mask includes the bit, so only this query ever reaches the proxy BLAS. Committing the
// accepted candidate and letting traversal continue keeps the nearest stand-in of the segment; the
// mask is the caller's gate, so a frame without proxies performs no traversal at all.
ShParticleHit traceParticleProxy(const float3 origin, const float3 dir, const float tMin,
                                 const float tMax, const int2 cbPix)
{
    ShParticleHit result;
    result.color = (float3)0.0;
    result.opacity = 0.0;
    result.blendOp = PARTICLE_BLEND_ALPHA_OVER;
    result.valid = false;

    RayQuery<RAY_FLAG_NONE> query;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = tMin;
    ray.TMax = tMax;

    query.TraceRayInline(topLevelAS, RAY_FLAG_NONE, INSTANCE_MASK_PARTICLE, ray);

    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE)
        {
            continue;
        }

        const ShParticleProxy candidate = particleProxies[query.CandidatePrimitiveIndex()];

        float tHit;
        if (candidate.kind == PARTICLE_PROXY_KIND_TRIANGLE)
        {
            float3 bary;
            if (intersectParticleTriangle(candidate, origin, dir, tMin, tMax, tHit, bary))
            {
                query.CommitProceduralPrimitiveHit(tHit);
            }
        }
        else
        {
            float2 uv;
            if (intersectParticleBillboard(candidate, origin, dir, tMin, tMax, tHit, uv))
            {
                query.CommitProceduralPrimitiveHit(tHit);
            }
        }
    }

    if (query.CommittedStatus() != COMMITTED_PROCEDURAL_PRIMITIVE_HIT)
    {
        return result;
    }

    const ShParticleProxy proxy = particleProxies[query.CommittedPrimitiveIndex()];
    const float t = query.CommittedRayT();

    float2 uv;
    float3 vertexColor;

    if (proxy.kind == PARTICLE_PROXY_KIND_TRIANGLE)
    {
        float3 bary;
        float tHit;
        if (!intersectParticleTriangle(proxy, origin, dir, tMin, tMax, tHit, bary))
        {
            return result;
        }

        uv = proxy.uv0 * bary.x + proxy.uv1 * bary.y + proxy.uv2 * bary.z;
        vertexColor = unpackParticleColor(proxy.packedColor) * bary.x +
                      unpackParticleColor(proxy.colorB) * bary.y +
                      unpackParticleColor(proxy.colorC) * bary.z;
    }
    else
    {
        float tHit;
        if (!intersectParticleBillboard(proxy, origin, dir, tMin, tMax, tHit, uv))
        {
            return result;
        }

        vertexColor = unpackParticleColor(proxy.packedColor);
    }

    // The raster fragments' shading: a lit sprite takes the sun and the cluster light plus the
    // screen-space ambient the LF ping carries, an unlit one reproduces the world fragment's flat
    // palette-and-texture formula with its exposure factor.
    const float4 texel = getTextureSampleLod(proxy.textureIndex, uv, 0.0);

    float3 rgb;
    if (proxy.gain < 0.0)
    {
        const float3 gamma = pow(vertexColor, (float3)2.2);
        const float lumAverage = max(0.0, particleTonemapping[0].avgLuminance);
        const float exposure = lumAverage > 0.0 ? 1.0 / (9.6 * lumAverage) : 0.0;
        rgb = gamma * texel.rgb * exposure;
    }
    else
    {
        const float3 lighting = smokeLightAt(origin + t * normalize(dir), proxy.cluster, 0.0,
                                             proxy.direct, 0.0);
        const float3 factor = proxy.gain * max(lighting + particleProxyAmbient(cbPix), (float3)proxy.lightFloor);
        rgb = texel.rgb * vertexColor * factor;
    }

    result.color = rgb;
    result.opacity = texel.a;
    result.blendOp = proxy.blendOp;
    result.valid = true;
    return result;
}

// The raster copy's blend operator folded into the composite's premultiplied "over"
// (CmQ2Adapter.comp.hlsl: `final = q2Transparent.rgb + lit * (1 - q2Transparent.a)`), with the
// pane's transmission applied to the contribution once.
void q2BlendParticleProxies(const float3 origin, const float3 dir, const float tMax,
                            const float3 throughput, const int2 cbPix)
{
    if (globalUniform.glassParticles == 0u)
    {
        return;
    }

    const ShParticleHit hit = traceParticleProxy(origin, dir, 0.01, tMax, cbPix);

    if (!hit.valid)
    {
        return;
    }

    float3 contribution;
    float coverage;

    if (hit.blendOp == PARTICLE_BLEND_ADD_ALPHA)
    {
        contribution = hit.color * hit.opacity;
        coverage = 0.0;
    }
    else if (hit.blendOp == PARTICLE_BLEND_ADD_COLOR)
    {
        contribution = hit.color * hit.color;
        coverage = 0.0;
    }
    else if (hit.blendOp == PARTICLE_BLEND_MUL_INV_ALPHA)
    {
        contribution = (float3)0.0;
        coverage = hit.opacity;
    }
    else if (hit.blendOp == PARTICLE_BLEND_PREMUL)
    {
        contribution = hit.color;
        coverage = hit.opacity;
    }
    else
    {
        contribution = hit.color * hit.opacity;
        coverage = hit.opacity;
    }

    /* The particle lands in its own layer at plain pixels, not in the pane's half-field signal:
       that signal is reconstructed with a 50/50 mix of the two checkerboard fields, which mixed a
       moving particle in and out and made it flicker. CmCheckerboard composites the layer over
       FINAL (gated by the glass mask it already reads), so the "over" still occludes the
       background, and the raster overlay draws the raster particles after it - the same temporal
       treatment. Each pixel belongs to one thread, so the read-modify-write is ordered by
       construction; the refl/refr main zeroes the layer before its loop. */
    const int2 layerPix = getRegularPixFromCheckerboardPix(cbPix);
    framebufQ2ParticleLayer[layerPix] =
        q2AlphaBlendPremultiplied(float4(contribution * throughput, coverage),
                                  framebufQ2ParticleLayer[layerPix]);
}

#endif // Q2_REFL_REFR_SHADER

#endif // PARTICLE_PROXIES_HLSLI_
