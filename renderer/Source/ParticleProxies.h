#pragma once

#include <cstdint>

namespace qray
{

// One traced stand-in of a rasterized particle effect the reflect/refract rays can hit. The raster
// path stays the primary visibility source; this record describes the same sprite (the classic
// three-vertex billboard of Quake/r_part.c) or the same triangle (an FTE effect of
// Quake/r_part_fte.c, de-indexed on the CPU) so an inline ray query can intersect it and shade it
// with the formula its raster copy uses. The layout has to match ShParticleProxy in
// ParticleProxies.hlsli.
inline constexpr uint32_t MAX_PARTICLE_PROXY_COUNT = 45056;
inline constexpr uint32_t MAX_FTE_TRIANGLE_COUNT = 40960;
// A single draw over this count is skipped outright instead of competing for the frame's budget:
// a draw that hovers at the frame bound would otherwise flip between captured and not captured
// from frame to frame, which reads as the effect flickering between refracted and raster. The FTE
// system batches every particle of one texture and blend into a single indexed draw, and Arcane
// Dimensions' effects reach thousands of triangles in one batch, so the bound has to cover a
// whole batch rather than a handful of sprites.
inline constexpr uint32_t MAX_FTE_TRIANGLES_PER_DRAW = 32768;

// The proxy kinds, mirrored by the shader's `PARTICLE_PROXY_KIND_*`.
inline constexpr uint32_t PARTICLE_PROXY_KIND_BILLBOARD = 0;
inline constexpr uint32_t PARTICLE_PROXY_KIND_TRIANGLE = 1;

// The blend operators the traced copy can reproduce exactly in the premultiplied "over" the
// composite uses (CmQ2Adapter.comp.hlsl: `final = q2Transparent.rgb + lit * (1 - q2Transparent.a)`):
//   ALPHA_OVER  (SRC_ALPHA, ONE_MINUS_SRC_ALPHA)  - contribution c*a, coverage a
//   PREMUL      (ONE, ONE_MINUS_SRC_ALPHA)        - contribution c,   coverage a
//   ADD_ALPHA   (SRC_ALPHA, ONE)                  - contribution c*a, no coverage
//   ADD_COLOR   (SRC_COLOR, ONE)                  - contribution c*c, no coverage
//   MUL_INV_ALPHA (ZERO, ONE_MINUS_SRC_ALPHA)     - no contribution, coverage a (the background is
//                                                   scaled by 1-a)
// The remaining FTE modes (SRC_COLOR/ONE_MINUS_SRC_COLOR, ZERO/ONE_MINUS_SRC_COLOR,
// SRC_ALPHA/ONE_MINUS_SRC_COLOR) scale the background by a per-channel factor and cannot fold into
// the scalar-alpha composite; their draws stay raster-only (without a proxy, so without the
// discard) and are documented as such.
inline constexpr uint32_t PARTICLE_BLEND_ALPHA_OVER = 0;
inline constexpr uint32_t PARTICLE_BLEND_PREMUL = 1;
inline constexpr uint32_t PARTICLE_BLEND_ADD_ALPHA = 2;
inline constexpr uint32_t PARTICLE_BLEND_ADD_COLOR = 3;
inline constexpr uint32_t PARTICLE_BLEND_MUL_INV_ALPHA = 4;

struct ParticleProxy
{
    // The classic sprite (kind BILLBOARD): the centroid of the raster triangle, its sphere bound
    // and its two leg lengths. The FTE triangle (kind TRIANGLE): the same three fields hold the
    // bounding sphere of the triangle, unused legs and the three vertex colours.
    float center[3];
    float radius;
    float legRight;
    float legUp;
    uint32_t packedColor;
    uint32_t textureIndex;
    uint32_t cluster;
    float direct;
    float gain;
    float lightFloor;

    // The triangle's exact geometry (kind TRIANGLE): v0 plus the two edges from it, with a UV per
    // vertex. A sprite leaves these zero.
    float v0[3];
    uint32_t blendOp;
    float edge1[3];
    uint32_t kind;
    float edge2[3];
    uint32_t unused2;
    float uv0[2];
    float uv1[2];
    float uv2[2];
    uint32_t unused3;
    uint32_t unused4;
};

static_assert(sizeof(ParticleProxy) == 128, "ParticleProxy must match ShParticleProxy");
static_assert(offsetof(ParticleProxy, v0) == 48, "");
static_assert(offsetof(ParticleProxy, blendOp) == 60, "");
static_assert(offsetof(ParticleProxy, edge1) == 64, "");
static_assert(offsetof(ParticleProxy, kind) == 76, "");
static_assert(offsetof(ParticleProxy, edge2) == 80, "");
static_assert(offsetof(ParticleProxy, uv0) == 96, "");
static_assert(offsetof(ParticleProxy, uv1) == 104, "");
static_assert(offsetof(ParticleProxy, uv2) == 112, "");

}
