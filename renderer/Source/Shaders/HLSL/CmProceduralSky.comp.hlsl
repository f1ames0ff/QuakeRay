// Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
// Copyright (c) 2026 QuakeRay contributors
//
// This file is a port of shader/physical_sky.comp from Quake 2 RTX (https://github.com/NVIDIA/Q2RTX),
// which is distributed under the terms of the GNU General Public License
// version 2.  It has been adapted to the renderer interface of this project.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
// Procedural sky: a flat sky colour (rt_sky_color) with procedural clouds over
// it (rt_sky_clouds_color). The single-scattering atmosphere this file was
// ported with is gone -- it is what kept the sky's own colour out of the sky,
// because what it painted was the physical blue mixed with that colour -- so
// the sky is now exactly the colour it is set to, and the sun disc is added on
// top of it, independent of both.
// The per-face camera bases are passed from the CPU side and match the convention
// of the rasterized sky cubemap path (Matrix::GetCubemapViewProjMat).
//
// HLSL counterpart of CmProceduralSky.comp. The golden includes nothing, declares its own
// resources and is the only user of them, so this file does the same and the two halves of the
// pair carry the same set 0: binding 0 and 2 are the two rgba16f cube storage images, binding 1
// is the std140 block of parameters, binding 3 is the cloud layer the composite samples and
// binding 4 is its sampler.
//
// Spellings that had to change:
//   * layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in -> the
//     [numthreads(16, 16, 1)] attribute of the entry point, and gl_GlobalInvocationID ->
//     SV_DispatchThreadID, which the golden already converts with ivec3(...) / ivec2(...), so the
//     casts stay and become int3(dispatchThreadID) / int2(dispatchThreadID.xy)
//   * layout(std140, set = 0, binding = 1) uniform Params { ... } params -> the struct Params_BT
//     with the same seven members in the same order and [[vk::binding(1, 0)]]
//     ConstantBuffer<Params_BT> params. The golden's vec4 faceBasis[18] stays a float4[18]; a
//     ConstantBuffer uses the same std140 rules as the GLSL block, so every Offset and the
//     ArrayStride stay where the golden put them
//   * layout(rgba16f, set = 0, binding = 0/2) uniform imageCube -> RWTexture2DArray<float4> with
//     [[vk::image_format("rgba16f")]], because HLSL has no writable cube texture type: dxc
//     rejects RWTextureCube as an unknown template and lands RWTexture2DArray on
//     OpTypeImage 2D with Arrayed 1. The pair checker therefore reports exactly one difference,
//     on both descriptors: GLSL image:Cube:0:0:0:2:Rgba16f against HLSL image:2D:0:1:0:2:Rgba16f.
//     The host binds VK_IMAGE_VIEW_TYPE_CUBE views to these two storage images today
//     (RenderCubemap.cpp CreateAttch), so the two descriptors need a 2D array view of the same
//     six layers for this half -- which is also the only view type D3D12 allows for a cube UAV
//     TODO(refactor): the cube view plus the 2D array view of one image is a port shim, not a
//     design. It goes away with the cubemap-write path in the NVRHI rewrite (A2/A5), which should
//     pick one view model for both backends instead of the two the port needs today.
//   * imageStore(img, pix, v) -> img[pix] = v, imageSize(img).xy -> GetDimensions, and the ivec3
//     store position is the HLSL (x, y, layer) coordinate, so the face ipos.z addresses the same
//     layer as in the golden
//   * vecN(s) of one scalar -> (floatN)s, vec3(a, b, c) -> float3(a, b, c), xyz swizzles and the
//     logical operators keep their spelling
//   * mix -> lerp, fract -> frac (the x - floor(x) intrinsic, same as in Utils.hlsli)
//   * no arithmetic expression was re-spelled: the cloud noise, the sun disc and the flat sky
//     colour keep the golden's operand orders and its clamp/smoothstep/lerp arguments

struct Params_BT
{
    float4 faceBasis[18]; // 6 faces * (right, up, forward)
    float4 sunDirection;  // xyz = normalized direction TOWARD the sun, w = how much sun the sky shows (0 = no sun, so no disc either)
    float4 skyColor;      // xyz = the colour of the sky itself (rt_sky_color), w = 1 at the flat level of rt_sky_clouds_quality (the volumetric composite reads no layer then)
    float4 skyParams;     // x = multiplier over the whole sky (rt_sky, rt_sky_brightness, rt_brightness), y = cloud opacity (rt_sky_clouds_alpha), z = sun disc intensity, w = sun disc display radius (radians)
    float4 cloudColor;    // xyz = cloud colour (rt_sky_clouds_color), w = cloud time (seconds)
    float4 cloudParams;   // x = cloud coverage, y = cloud contour sharpness (rt_sky_clouds_density), z = drift speed, w = clouds enabled
    float4 sunDiscColor;  // xyz = color of the sun disc (rt_sky_sun_color), w unused
};

[[vk::binding(1, 0)]] ConstantBuffer<Params_BT> params;

[[vk::binding(0, 0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> cubemapOut;
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> envCubemapOut;
[[vk::binding(3, 0)]] TextureCube<float4> cloudCubemap;
[[vk::binding(4, 0)]] SamplerState cloudCubemap_Sampler;

static const float CLOUD_LAYER_TEXEL = 1.0 / 1024.0;
static const float CLOUD_READ_SPREAD = 1.0;
static const float SUN_DISC_CLOUD_HIDE = 4.0;

// --- procedural clouds (textureless value noise fBm) ---
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
    float n100 = hash13(i + float3(1, 0, 0));
    float n010 = hash13(i + float3(0, 1, 0));
    float n110 = hash13(i + float3(1, 1, 0));
    float n001 = hash13(i + float3(0, 0, 1));
    float n101 = hash13(i + float3(1, 0, 1));
    float n011 = hash13(i + float3(0, 1, 1));
    float n111 = hash13(i + float3(1, 1, 1));

    return lerp(
        lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
        lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y),
        f.z);
}

float fbm3(float3 p)
{
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++)
    {
        v += amp * valueNoise3(p);
        p = p * 2.03;
        amp *= 0.5;
    }
    return v;
}

float cloudMask(float3 dir, float time, float speed)
{
    // sample the noise on the sky dome; drift slowly with time
    float3 p = dir * 3.0 + float3(time * speed, time * speed * 0.4, 0.0);
    float n = fbm3(p);
    return n;
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int3 ipos = int3(dispatchThreadID);

    uint sizeX, sizeY, sizeZ;
    cubemapOut.GetDimensions(sizeX, sizeY, sizeZ);
    int2 size = int2(sizeX, sizeY);

    if (ipos.x >= size.x || ipos.y >= size.y || ipos.z >= 6)
        return;

    int face = ipos.z;
    float3 right   = params.faceBasis[face * 3 + 0].xyz;
    float3 up      = params.faceBasis[face * 3 + 1].xyz;
    float3 forward = params.faceBasis[face * 3 + 2].xyz;

    float2 ndc = (float2(ipos.xy) + (float2)0.5) / (float2)size * 2.0 - 1.0;
    float3 dir = normalize(ndc.x * right + (-ndc.y) * up + forward);

    float3 sunDir       = normalize(params.sunDirection.xyz);
    float3 skyColor     = max(params.skyColor.xyz, (float3)0.0);
    float sunAmount    = params.sunDirection.w;      // 0 when the host has no sun (rt_sky_sun 0)
    float sunAngRad    = max(params.skyParams.w, 0.0); // display sun disc radius
    float multiplier   = params.skyParams.x;
    float cloudOpacity = clamp(params.skyParams.y, 0.0, 1.0);
    float sunIntensity = params.skyParams.z;

    float flatClouds = params.skyColor.w;
    bool cloudsOn = params.cloudParams.w > 0.5 && cloudOpacity > 0.0;

    float4 cloud = float4(0.0, 0.0, 0.0, 1.0);
    if (cloudsOn && flatClouds <= 0.5)
    {
        float2 texel = float2(CLOUD_LAYER_TEXEL, CLOUD_LAYER_TEXEL);
        float2 spread = texel * CLOUD_READ_SPREAD;
        float4 centre = cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir, 0.0);
        float4 edges = cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir - right * spread.x, 0.0) +
                       cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir + right * spread.x, 0.0) +
                       cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir - up * spread.y, 0.0) +
                       cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir + up * spread.y, 0.0);
        float4 corners = cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir - right * spread.x - up * spread.y, 0.0) +
                         cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir - right * spread.x + up * spread.y, 0.0) +
                         cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir + right * spread.x - up * spread.y, 0.0) +
                         cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir + right * spread.x + up * spread.y, 0.0);
        cloud = (centre * 4.0 + edges * 2.0 + corners) / 16.0;
    }

    if (cloudsOn && flatClouds > 0.5)
    {
        float n = cloudMask(dir, params.cloudColor.w, params.cloudParams.z);
        float edge = lerp(0.7, 0.05, clamp(params.cloudParams.y, 0.0, 1.0));
        float mask = smoothstep(params.cloudParams.x, params.cloudParams.x + edge, n);
        skyColor = lerp(skyColor, params.cloudColor.xyz, clamp(mask * cloudOpacity, 0.0, 1.0));
    }

    float cosAng = cos(sunAngRad);
    float disc = smoothstep(cosAng, 1.0, dot(dir, sunDir)) * sunAmount;

    float layerTransmittance = cloudOpacity > 0.0 ? clamp(1.0 - (1.0 - cloud.a) / cloudOpacity, 0.0, 1.0) : 1.0;
    float discTransmittance = lerp(1.0, pow(layerTransmittance, SUN_DISC_CLOUD_HIDE), cloudOpacity);

    float discVisible = clamp(disc * discTransmittance, 0.0, 1.0);
    float cloudShare = 1.0 - smoothstep(0.02, 0.3, discVisible);

    float3 sky = skyColor;
    float3 visible = sky * cloud.a + params.sunDiscColor.xyz * sunIntensity * disc * discTransmittance + cloud.rgb;
    cubemapOut[ipos] = float4(visible * multiplier, cloudShare);

    float3 env = sky * cloud.a + cloud.rgb;
    envCubemapOut[ipos] = float4(env * multiplier, 1.0);
}
