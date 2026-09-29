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

#ifndef BRDF_H_
#define BRDF_H_

#include "Random.h"


float roughnessSquaredToSpecPower(in float alpha) {
    return max(0.01, 2.0f / (square(alpha) + 1e-4) - 2.0f);
}


float evalBRDFLambertian(float subsurfaceAlbedo)
{
    return subsurfaceAlbedo / M_PI;
}

vec3 sampleLambertian(const vec3 n, float u1, float u2, out float oneOverPdf)
{
    return sampleOrientedHemisphere(n, u1, u2, oneOverPdf);
}



#define BRDF_MIN_SPECULAR_COLOR 0.04

vec3 getSpecularColor(const vec3 albedo, float metallic)
{
    vec3 minSpec = vec3(BRDF_MIN_SPECULAR_COLOR);
    return mix(minSpec, albedo, metallic);
}

#define AO_ALBEDO_THRESHOLD 0.02

float getMaterialAmbient(const vec3 albedo)
{
    float l = getLuminance(albedo);

    return l > AO_ALBEDO_THRESHOLD ?
        1.0 :
        1.0 - square((l - AO_ALBEDO_THRESHOLD) / AO_ALBEDO_THRESHOLD);
}

vec3 demodulateSpecular(const vec3 contrib, const vec3 surfSpecularColor)
{
    return contrib / max(vec3(0.01), surfSpecularColor);
}

vec3 getFresnelSchlick(float nl, const vec3 specularColor)
{
    return specularColor + (vec3(1.0) - specularColor) * pow(1 - max(nl, 0), 5);
}

float getFresnelSchlick(float n1, float n2, const vec3 V, const vec3 N)
{
    float R0 = (n1 - n2) / (n1 + n2);
    R0 *= R0;

    return mix(R0, 1.0, pow(1.0 - abs(dot(N, V)), 5.0));
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

vec3 evalBRDFSmithGGX(const vec3 n, const vec3 v, const vec3 l, float alpha, const vec3 specularColor)
{
    alpha = max(alpha, MIN_GGX_ROUGHNESS);

    const float nl = max(dot(n, l), 0);

    if (nl <= 0)
    {
        return vec3(0.0);
    }

    const vec3  h = normalize( v + l );
    const vec3 F = getFresnelSchlick(
        clamp(dot(v, h), 0.0, 1.0),
        specularColor
        );
    const float D = D_GGX( dot( n, h ), alpha );

    float G2Modif;
    {
        const float nv = max(dot(n, v), 0);

        G2Modif = 0.5 / mix(2 * nl * nv, nl + nv, alpha);
    }

    return F * G2Modif * D;
}



vec3 sampleGGXVNDF(const vec3 v, float alpha, float u1, float u2, out float oneOverPdf)
{
    alpha = max( alpha, MIN_GGX_ROUGHNESS );

    u1 *= 0.98;
    u2 *= 0.98;

    vec3 Vh = normalize(vec3(alpha * v.x, alpha * v.y, v.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    const vec3 T1 = lensq > 0 ? vec3(-Vh.y, Vh.x, 0) * inversesqrt(lensq) : vec3(1,0,0);
    const vec3 T2 = cross(Vh, T1);

    float r = sqrt(u1);
    float phi = 2.0 * M_PI * u2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;

    const vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;

    const vec3 Ne = normalize(vec3(alpha * Nh.x, alpha * Nh.y, max(0.02, Nh.z)));

    {
        const float nm = Ne.z;
        const float D = D_GGX( nm, alpha );

        const float nv = v.z;
        const float G1 = G1_GGX( nv, alpha );

        oneOverPdf = v.z * safePositiveRcp(G1 * max(0, dot(v, Ne)) * D);
    }

    return Ne;
}

vec3 sampleSmithGGX(const vec3 n, const vec3 v, float alpha, float u1, float u2, out float oneOverPdf)
{
    const mat3 basis = getONB(n);

    const vec3 ve = transpose(basis) * v;

    const vec3 m = sampleGGXVNDF(ve, alpha, u1, u2, oneOverPdf);

    const vec3 l = reflect( -ve, m );
    oneOverPdf *= 4 * dot( ve, m );

    return basis * l;
}

float evalSpecularBouncePdf(const vec3 n, const vec3 v, float alpha, const vec3 l)
{
    const float nv = dot(n, v);
    const float nl = dot(n, l);

    if (nv <= 0.0 || nl <= 0.0)
    {
        return 0.0;
    }

    alpha = max(alpha, MIN_GGX_ROUGHNESS);

    const vec3 m = normalize(v + l);
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
