// Copyright (c) 2026 QuakeRay contributors
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

#ifndef BRDF_HLSLI_
#define BRDF_HLSLI_
#include "Random.hlsli"

float roughnessSquaredToSpecPower(in float alpha) {
    return max(0.01, 2.0f / (square(alpha) + 1e-4) - 2.0f);
}


float evalBRDFLambertian(float subsurfaceAlbedo)
{
    return subsurfaceAlbedo / M_PI;
}

float3 sampleLambertian(const float3 n, float u1, float u2, out float oneOverPdf)
{
    return sampleOrientedHemisphere(n, u1, u2, oneOverPdf);
}



#define BRDF_MIN_SPECULAR_COLOR 0.04

float3 getSpecularColor(const float3 albedo, float metallic)
{
    float3 minSpec = (float3)BRDF_MIN_SPECULAR_COLOR;
    return lerp(minSpec, albedo, metallic);
}

#define AO_ALBEDO_THRESHOLD 0.02

float getMaterialAmbient(const float3 albedo)
{
    float l = getLuminance(albedo);

    return l > AO_ALBEDO_THRESHOLD ?
        1.0 :
        1.0 - square((l - AO_ALBEDO_THRESHOLD) / AO_ALBEDO_THRESHOLD);
}

float3 demodulateSpecular(const float3 contrib, const float3 surfSpecularColor)
{
    return contrib / max((float3)0.01, surfSpecularColor);
}

float3 getFresnelSchlick(float nl, const float3 specularColor)
{
    return specularColor + ((float3)1.0 - specularColor) * pow(1 - max(nl, 0), 5);
}

float getFresnelSchlick(float n1, float n2, const float3 V, const float3 N)
{
    float R0 = (n1 - n2) / (n1 + n2);
    R0 *= R0;

    return lerp(R0, 1.0, pow(1.0 - abs(dot(N, V)), 5.0));
}

// I points into the surface (dot(N, I) < 0), N is oriented against I, n1 is the medium the ray comes from and n2 the medium it enters.
float getInterfaceTransmission(float n1, float n2, const float3 I, const float3 N)
{
    const float3 T = refract(I, N, n1 / n2);
    if (dot(T, T) <= 0.0)
    {
        return 0.0;
    }

    return clamp(1.0 - getFresnelSchlick(n1, n2, I, N), 0.0, 1.0);
}

float D_GGX( float nm, float alpha )
{
#if SHIPPING_HACK
#ifdef FORCE_EVALBRDF_GGX_LOOSE
    alpha = max( 0.2, alpha );
#endif
#endif

    const float alphaSq = square( alpha );

    nm = max( 0.0, nm );
    return alphaSq / M_PI / square( nm * nm * ( alphaSq - 1 ) + 1 );
}

float G1_GGX( float ns, float alpha )
{
    return 2 * ns * safePositiveRcp( ns * ( 2 - alpha ) + alpha );
}

#define MIN_GGX_ROUGHNESS 0.005

float3 evalBRDFSmithGGX(const float3 n, const float3 v, const float3 l, float alpha, const float3 specularColor)
{
    alpha = max(alpha, MIN_GGX_ROUGHNESS);

    const float nl = max(dot(n, l), 0);

    if (nl <= 0)
    {
        return (float3)0.0;
    }

    const float3  h = normalize( v + l );
    const float3 F = getFresnelSchlick(
        clamp(dot(v, h), 0.0, 1.0),
        specularColor
        );
    const float D = D_GGX( dot( n, h ), alpha );

    float G2Modif;
    {
        const float nv = max(dot(n, v), 0);

        G2Modif = 0.5 / lerp(2 * nl * nv, nl + nv, alpha);
    }

    return F * G2Modif * D;
}



float3 sampleGGXVNDF(const float3 v, float alpha, float u1, float u2, out float oneOverPdf)
{
    alpha = max( alpha, MIN_GGX_ROUGHNESS );

    u1 *= 0.98;
    u2 *= 0.98;

    float3 Vh = normalize(float3(alpha * v.x, alpha * v.y, v.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    const float3 T1 = lensq > 0 ? float3(-Vh.y, Vh.x, 0) * rsqrt(lensq) : float3(1, 0, 0);
    const float3 T2 = cross(Vh, T1);

    float r = sqrt(u1);
    float phi = 2.0 * M_PI * u2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;

    const float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;

    const float3 Ne = normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.02, Nh.z)));

    {
        const float nm = Ne.z;
        const float D = D_GGX( nm, alpha );

        const float nv = v.z;
        const float G1 = G1_GGX( nv, alpha );

        oneOverPdf = v.z * safePositiveRcp(G1 * max(0, dot(v, Ne)) * D);
    }

    return Ne;
}

float3 sampleSmithGGX(const float3 n, const float3 v, float alpha, float u1, float u2, out float oneOverPdf)
{
    const float3x3 basis = getONB(n);

    const float3 ve = mul(transpose(basis), v);

    const float3 m = sampleGGXVNDF(ve, alpha, u1, u2, oneOverPdf);

    const float3 l = reflect( -ve, m );
    oneOverPdf *= 4 * dot( ve, m );

    return mul(basis, l);
}

float evalSpecularBouncePdf(const float3 n, const float3 v, float alpha, const float3 l)
{
    const float nv = dot(n, v);
    const float nl = dot(n, l);

    if (nv <= 0.0 || nl <= 0.0)
    {
        return 0.0;
    }

    alpha = max(alpha, MIN_GGX_ROUGHNESS);

    const float3 m = normalize(v + l);
    const float nm = dot(n, m);

    if (nm <= 0.0)
    {
        return 0.0;
    }

    const float D = D_GGX(nm, alpha);
    const float G1 = G1_GGX(nv, alpha);

    return G1 * D / (4.0 * nv);
}

#endif
