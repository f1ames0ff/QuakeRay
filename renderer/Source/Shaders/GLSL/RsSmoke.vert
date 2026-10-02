// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>

#version 460

#extension GL_EXT_ray_query : require

layout (location = 0) in vec3 position;
layout (location = 1) in vec4 color;
layout (location = 2) in vec2 texCoord;
layout (location = 3) in vec3 puffParams;
layout (location = 4) in vec2 puffLook;
layout (location = 5) in uint puffCluster;

layout (location = 0) out vec4 outColor;
layout (location = 1) out vec2 outCorner;
layout (location = 2) out vec3 outWorldPos;
layout (location = 3) out vec3 outParams;
layout (location = 4) out vec2 outLook;
layout (location = 5) out vec3 outLit;

#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_TEXTURES       0
#define DESC_SET_LIGHT_SOURCES  6
#include "ShaderCommonGLSLFunc.h"
#include "Random.h"
#include "Smoke.h"
#include "Light.h"
#include "Q2ClusterLights.h"
#include "SmokeLight.h"

layout(push_constant) uniform RasterizerVert_BT
{
    layout(offset = 0) mat4 viewProj;
} rasterizerVertInfo;

void main()
{
    const vec3 right = globalUniform.invView[0].xyz;
    const vec3 up    = globalUniform.invView[1].xyz;

    const vec3 worldPos = position + (right * texCoord.x + up * texCoord.y) * puffParams.y;

    outColor    = color;
    outCorner   = texCoord;
    outWorldPos = worldPos;
    outParams   = puffParams;
    outLook     = puffLook;
    outLit      = smokeLightAt(worldPos, puffCluster, puffParams.z);

    gl_Position = rasterizerVertInfo.viewProj * vec4(worldPos, 1.0);
}
