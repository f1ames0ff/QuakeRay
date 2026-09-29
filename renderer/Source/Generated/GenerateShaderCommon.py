# Copyright (c) 2026 QuakeRay contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

import os
import re
import subprocess
import sys
from math import log2


FLOAT32 = 0
INT32   = 1
UINT32  = 2

UNORM8  = 3
UINT8   = 4
UINT16  = 5
FLOAT16 = 6
PACKED_11 = 128
PACKED_E5 = 129

CHANNELS_R    = 0
CHANNELS_RG   = 1
CHANNELS_RGB  = 2
CHANNELS_RGBA = 3


C_TYPES = {
    FLOAT32: "float",
    INT32:   "int32_t",
    UINT32:  "uint32_t",
}

GLSL_TYPES = {
    FLOAT32: "float",
    INT32:   "int",
    UINT32:  "uint",
    (FLOAT32, 2): "vec2",
    (FLOAT32, 3): "vec3",
    (FLOAT32, 4): "vec4",
    (INT32,   2): "ivec2",
    (INT32,   3): "ivec3",
    (INT32,   4): "ivec4",
    (UINT32,  2): "uvec2",
    (UINT32,  3): "uvec3",
    (UINT32,  4): "uvec4",
    (FLOAT32, 22): "mat2",
    (FLOAT32, 23): "mat2x3",
    (FLOAT32, 32): "mat3x2",
    (FLOAT32, 33): "mat3",
    (FLOAT32, 34): "mat3x4",
    (FLOAT32, 43): "mat4x3",
    (FLOAT32, 44): "mat4",
}


TYPE_BYTES = {
    FLOAT32: 4,
    INT32:   4,
    UINT32:  4,
    (FLOAT32,  2): 8,
    (FLOAT32,  3): 12,
    (FLOAT32,  4): 16,
    (INT32,    2): 8,
    (INT32,    3): 12,
    (INT32,    4): 16,
    (UINT32,   2): 8,
    (UINT32,   3): 12,
    (UINT32,   4): 16,
    (FLOAT32, 22): 16,
    (FLOAT32, 23): 24,
    (FLOAT32, 32): 24,
    (FLOAT32, 33): 36,
    (FLOAT32, 34): 48,
    (FLOAT32, 43): 48,
    (FLOAT32, 44): 64,
}


VULKAN_FORMATS = {
    (UNORM8,   CHANNELS_R):       "VK_FORMAT_R8_UNORM",
    (UNORM8,   CHANNELS_RG):      "VK_FORMAT_R8G8_UNORM",
    (UNORM8,   CHANNELS_RGBA):    "VK_FORMAT_R8G8B8A8_UNORM",

    (UINT8,    CHANNELS_R):       "VK_FORMAT_R8_UINT",
    (UINT8,    CHANNELS_RG):      "VK_FORMAT_R8G8_UINT",
    (UINT8,    CHANNELS_RGBA):    "VK_FORMAT_R8G8B8A8_UINT",

    (UINT16,   CHANNELS_R):       "VK_FORMAT_R16_UINT",
    (UINT16,   CHANNELS_RG):      "VK_FORMAT_R16G16_UINT",
    (UINT16,   CHANNELS_RGBA):    "VK_FORMAT_R16G16B16A16_UINT",

    (UINT32,   CHANNELS_R):       "VK_FORMAT_R32_UINT",
    (UINT32,   CHANNELS_RG):      "VK_FORMAT_R32G32_UINT",
    (UINT32,   CHANNELS_RGBA):    "VK_FORMAT_R32G32B32A32_UINT",

    (FLOAT16,  CHANNELS_R):       "VK_FORMAT_R16_SFLOAT",
    (FLOAT16,  CHANNELS_RG):      "VK_FORMAT_R16G16_SFLOAT",
    (FLOAT16,  CHANNELS_RGBA):    "VK_FORMAT_R16G16B16A16_SFLOAT",

    (FLOAT32,  CHANNELS_R):       "VK_FORMAT_R32_SFLOAT",
    (FLOAT32,  CHANNELS_RG):      "VK_FORMAT_R32G32_SFLOAT",
    (FLOAT32,  CHANNELS_RGBA):    "VK_FORMAT_R32G32B32A32_SFLOAT",

    (PACKED_11, CHANNELS_RGB):    "VK_FORMAT_B10G11R11_UFLOAT_PACK32",
    (PACKED_E5, CHANNELS_RGB):    "VK_FORMAT_R32_UINT",
}

GLSL_FORMATS = {
    (UNORM8,   CHANNELS_R):       "r8",
    (UNORM8,   CHANNELS_RG):      "rg8",
    (UNORM8,   CHANNELS_RGBA):    "rgba8",

    (UINT8,    CHANNELS_R):       "r8ui",
    (UINT8,    CHANNELS_RG):      "rg8ui",
    (UINT8,    CHANNELS_RGBA):    "rgba8ui",

    (UINT16,   CHANNELS_R):       "r16ui",
    (UINT16,   CHANNELS_RG):      "rg16ui",
    (UINT16,   CHANNELS_RGBA):    "rgba16ui",

    (UINT32,   CHANNELS_R):       "r32ui",
    (UINT32,   CHANNELS_RG):      "rg32ui",
    (UINT32,   CHANNELS_RGBA):    "rgba32ui",

    (FLOAT16,  CHANNELS_R):       "r16f",
    (FLOAT16,  CHANNELS_RG):      "rg16f",
    (FLOAT16,  CHANNELS_RGBA):    "rgba16f",

    (FLOAT32,  CHANNELS_R):       "r32f",
    (FLOAT32,  CHANNELS_RG):      "rg32f",
    (FLOAT32,  CHANNELS_RGBA):    "rgba32f",

    (PACKED_11, CHANNELS_RGB):    "r11f_g11f_b10f",
    (PACKED_E5, CHANNELS_RGB):    "r32ui",
}

GLSL_STORAGE_TYPES = {
    FLOAT32:  "image2D",
    INT32:    "iimage2D",
    UINT32:   "uimage2D",
    UNORM8:   "image2D",
    UINT8:    "uimage2D",
    UINT16:   "uimage2D",
    FLOAT16:  "image2D",
    PACKED_11: "image2D",
    PACKED_E5: "uimage2D",
}

GLSL_SAMPLED_TYPES = {
    FLOAT32:  "texture2D",
    INT32:    "itexture2D",
    UINT32:   "utexture2D",
    UNORM8:   "texture2D",
    UINT8:    "utexture2D",
    UINT16:   "utexture2D",
    FLOAT16:  "texture2D",
    PACKED_11: "texture2D",
    PACKED_E5: "utexture2D",
}

GLSL_SAMPLER_TYPE = "sampler"


HLSL_TYPES = {
    FLOAT32: "float",
    INT32:   "int",
    UINT32:  "uint",
    (FLOAT32, 2): "float2",
    (FLOAT32, 3): "float3",
    (FLOAT32, 4): "float4",
    (INT32,   2): "int2",
    (INT32,   3): "int3",
    (INT32,   4): "int4",
    (UINT32,  2): "uint2",
    (UINT32,  3): "uint3",
    (UINT32,  4): "uint4",
    (FLOAT32, 22): "float2x2",
    (FLOAT32, 23): "float3x2",
    (FLOAT32, 32): "float2x3",
    (FLOAT32, 33): "float3x3",
    (FLOAT32, 34): "float4x3",
    (FLOAT32, 43): "float3x4",
    (FLOAT32, 44): "float4x4",
}

HLSL_FORMAT_OVERRIDES = {
    (PACKED_11, CHANNELS_RGB): "r11g11b10f",
}

HLSL_FORMATS = dict(GLSL_FORMATS)
HLSL_FORMATS.update(HLSL_FORMAT_OVERRIDES)

HLSL_STORAGE_TYPES = {
    FLOAT32:  "RWTexture2D<float4>",
    INT32:    "RWTexture2D<int4>",
    UINT32:   "RWTexture2D<uint4>",
    UNORM8:   "RWTexture2D<float4>",
    UINT8:    "RWTexture2D<uint4>",
    UINT16:   "RWTexture2D<uint4>",
    FLOAT16:  "RWTexture2D<float4>",
    PACKED_11: "RWTexture2D<float4>",
    PACKED_E5: "RWTexture2D<uint4>",
}

HLSL_SAMPLED_TYPES = {
    FLOAT32:  "Texture2D<float4>",
    INT32:    "Texture2D<int4>",
    UINT32:   "Texture2D<uint4>",
    UNORM8:   "Texture2D<float4>",
    UINT8:    "Texture2D<uint4>",
    UINT16:   "Texture2D<uint4>",
    FLOAT16:  "Texture2D<float4>",
    PACKED_11: "Texture2D<float4>",
    PACKED_E5: "Texture2D<uint4>",
}

HLSL_SAMPLER_TYPE = "SamplerState"


DESCRIPTOR_SYNTAX = {
    "glsl": {
        "format":      lambda base_format, channels: GLSL_FORMATS[(base_format, channels)],
        "storageType": lambda base_format: GLSL_STORAGE_TYPES[base_format],
        "sampledType": lambda base_format: GLSL_SAMPLED_TYPES[base_format],
        "samplerType": lambda base_format: GLSL_SAMPLER_TYPE,
        "storage":     lambda binding, image_format, type_name, name:
            "layout(set = %s, binding = %d, %s) uniform %s %s;" % (FRAMEBUF_DESC_SET_NAME, binding, image_format, type_name, name),
        "sampled":     lambda binding, image_format, type_name, name:
            "layout(set = %s, binding = %d) uniform %s %s;" % (FRAMEBUF_DESC_SET_NAME, binding, type_name, name),
        "sampler":     lambda binding, image_format, type_name, name:
            "layout(set = %s, binding = %d) uniform %s %s;" % (FRAMEBUF_DESC_SET_NAME, binding, type_name, name),
    },
    "hlsl": {
        "format":      lambda base_format, channels: HLSL_FORMATS[(base_format, channels)],
        "storageType": lambda base_format: HLSL_STORAGE_TYPES[base_format],
        "sampledType": lambda base_format: HLSL_SAMPLED_TYPES[base_format],
        "samplerType": lambda base_format: HLSL_SAMPLER_TYPE,
        "storage":     lambda binding, image_format, type_name, name:
            "[[vk::binding(%d, %s), vk::image_format(\"%s\")]] %s %s;" % (binding, FRAMEBUF_DESC_SET_NAME, image_format, type_name, name),
        "sampled":     lambda binding, image_format, type_name, name:
            "[[vk::binding(%d, %s)]] %s %s;" % (binding, FRAMEBUF_DESC_SET_NAME, type_name, name),
        "sampler":     lambda binding, image_format, type_name, name:
            "[[vk::binding(%d, %s)]] %s %s;" % (binding, FRAMEBUF_DESC_SET_NAME, type_name, name),
    },
}


MULTIDIMENSIONAL_ARRAYS_IN_C = False
RESOLVE_LATER = "CONST VALUE MUST BE EVALUATED"



GRADIENT_ESTIMATION_ENABLED = False
FRAMEBUF_IGNORE_ATTACHMENTS_DEFINE = "FRAMEBUF_IGNORE_ATTACHMENTS"

MAX_FOG_VOLUMES = 8

CONST = {
    "MAX_STATIC_VERTEX_COUNT"               : 1 << 20,
    "MAX_DYNAMIC_VERTEX_COUNT"              : 1 << 21,
    "MAX_INDEXED_PRIMITIVE_COUNT"           : 1 << 20,

    "MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT"     : 1 << 12,
    "MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW" : RESOLVE_LATER,
    "MAX_GEOMETRY_PRIMITIVE_COUNT"          : RESOLVE_LATER,
    "MAX_GEOMETRY_PRIMITIVE_COUNT_POW"      : RESOLVE_LATER,
    "LOWER_BOTTOM_LEVEL_GEOMETRIES_COUNT"   : 1 << 8,

    "MAX_TOP_LEVEL_INSTANCE_COUNT"          : 45,

    "BINDING_VERTEX_BUFFER_STATIC"              : 0,
    "BINDING_VERTEX_BUFFER_DYNAMIC"             : 1,
    "BINDING_INDEX_BUFFER_STATIC"               : 2,
    "BINDING_INDEX_BUFFER_DYNAMIC"              : 3,
    "BINDING_GEOMETRY_INSTANCES"                : 4,
    "BINDING_GEOMETRY_INSTANCES_MATCH_PREV"     : 5,
    "BINDING_PREV_POSITIONS_BUFFER_DYNAMIC"     : 6,
    "BINDING_PREV_INDEX_BUFFER_DYNAMIC"         : 7,
    "BINDING_GLOBAL_UNIFORM"                    : 0,
    "BINDING_ACCELERATION_STRUCTURE_MAIN"       : 0,
    "BINDING_TEXTURES"                          : 0,
    "BINDING_TEXTURES_SAMPLER"                  : 1,
    "BINDING_CUBEMAPS"                          : 0,
    "BINDING_CUBEMAPS_SAMPLER"                  : 1,
    "BINDING_RENDER_CUBEMAP"                    : 0,
    "BINDING_RENDER_CUBEMAP_SAMPLER"            : 2,
    "BINDING_RENDER_CUBEMAP_ENV"                : 1,
    "BINDING_RENDER_CUBEMAP_ENV_SAMPLER"        : 3,
    "BINDING_BLUE_NOISE"                        : 0,
    "BINDING_LUM_HISTOGRAM"                     : 0,
    "BINDING_LIGHT_SOURCES"                     : 0,
    "BINDING_LIGHT_SOURCES_PREV"                : 1,
    "BINDING_LIGHT_SOURCES_INDEX_PREV_TO_CUR"   : 2,
    "BINDING_LIGHT_SOURCES_INDEX_CUR_TO_PREV"   : 3,
    "BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_OFFSETS" : 4,
    "BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_LIGHTS"  : 5,
    "BINDING_LIGHT_SOURCES_Q2_LIGHT_STATS"        : 6,
    "BINDING_LIGHT_SOURCES_TAL_CDF"               : 7,
    "BINDING_LIGHT_SOURCES_Q2_CLUSTER_SKY_VIS"    : 8,
    "BINDING_LENS_FLARES_CULLING_INPUT"         : 0,
    "BINDING_LENS_FLARES_DRAW_CMDS"             : 1,
    "BINDING_DRAW_LENS_FLARES_INSTANCES"        : 0,
    "BINDING_DECAL_INSTANCES"                   : 0,
    "BINDING_PORTAL_INSTANCES"                  : 0,
    "BINDING_LPM_PARAMS"                        : 0,
    "BINDING_VOLUMETRIC_STORAGE"                : 0,
    "BINDING_VOLUMETRIC_SAMPLED"                : 1,
    "BINDING_VOLUMETRIC_SAMPLER"                : 2,
    "BINDING_VOLUMETRIC_SAMPLED_PREV"           : 3,
    "BINDING_VOLUMETRIC_SAMPLER_PREV"           : 4,
    "BINDING_VOLUMETRIC_ILLUMINATION"           : 5,
    "BINDING_VOLUMETRIC_ILLUMINATION_SAMPLED"   : 6,
    "BINDING_VOLUMETRIC_ILLUMINATION_SAMPLER"   : 7,

    "INSTANCE_CUSTOM_INDEX_FLAG_DYNAMIC"                : "1 << 0",
    "INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON"           : "1 << 1",
    "INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON_VIEWER"    : "1 << 2",
    "INSTANCE_CUSTOM_INDEX_FLAG_SKY"                    : "1 << 3",

    "INSTANCE_MASK_WORLD_0"                 : 1 << 0,
    "INSTANCE_MASK_WORLD_1"                 : 1 << 1,
    "INSTANCE_MASK_WORLD_2"                 : 1 << 2,
    "INSTANCE_MASK_RESERVED_0"              : 1 << 3,
    "INSTANCE_MASK_RESERVED_1"              : 1 << 4,
    "INSTANCE_MASK_REFRACT"                 : 1 << 5,
    "INSTANCE_MASK_FIRST_PERSON"            : 1 << 6,
    "INSTANCE_MASK_FIRST_PERSON_VIEWER"     : 1 << 7,

    "PAYLOAD_INDEX_DEFAULT"                 : 0,
    "PAYLOAD_INDEX_SHADOW"                  : 1,

    "SBT_INDEX_RAYGEN_PRIMARY"              : 0,
    "SBT_INDEX_RAYGEN_REFL_REFR"            : 1,
    "SBT_INDEX_RAYGEN_DIRECT"               : 2,
    "SBT_INDEX_RAYGEN_Q2_REFL_REFR"         : 3,
    "SBT_INDEX_RAYGEN_Q2_INDIRECT"          : 4,
    "SBT_INDEX_MISS_DEFAULT"                : 0,
    "SBT_INDEX_MISS_SHADOW"                 : 1,
    "SBT_INDEX_HITGROUP_FULLY_OPAQUE"       : 0,
    "SBT_INDEX_HITGROUP_ALPHA_TESTED"       : 1,

    "MATERIAL_ALBEDO_ALPHA_INDEX"                   : 0,
    "MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX"    : 1,
    "MATERIAL_NORMAL_INDEX"                         : 2,

    "MATERIAL_NO_TEXTURE"                   : 0,

    "MATERIAL_BLENDING_FLAG_OPAQUE"         : "1 << 0",
    "MATERIAL_BLENDING_FLAG_ALPHA"          : "1 << 1",
    "MATERIAL_BLENDING_FLAG_ADD"            : "1 << 2",
    "MATERIAL_BLENDING_FLAG_SHADE"          : "1 << 3",
    "MATERIAL_BLENDING_FLAG_BIT_COUNT"      : 4,
    "MATERIAL_BLENDING_MASK_FIRST_LAYER"    : RESOLVE_LATER,
    "MATERIAL_BLENDING_MASK_SECOND_LAYER"   : RESOLVE_LATER,
    "MATERIAL_BLENDING_MASK_THIRD_LAYER"    : RESOLVE_LATER,
    "GEOM_INST_FLAG_TURB_WARP"              : "1 << 13",
    "GEOM_INST_FLAG_ALPHA_TRANSMISSION"     : "1 << 14",
    "GEOM_INST_FLAG_RESERVED_2"             : "1 << 15",
    "GEOM_INST_FLAG_RESERVED_3"             : "1 << 16",
    "GEOM_INST_FLAG_RESERVED_4"             : "1 << 17",
    "GEOM_INST_FLAG_MEDIA_TYPE_ACID"        : "1 << 18",
    "GEOM_INST_FLAG_EXACT_NORMALS"          : "1 << 19",
    "GEOM_INST_FLAG_IGNORE_REFRACT_AFTER"   : "1 << 20",
    "GEOM_INST_FLAG_REFL_REFR_ALBEDO_MULT"  : "1 << 21",
    "GEOM_INST_FLAG_REFL_REFR_ALBEDO_ADD"   : "1 << 22",
    "GEOM_INST_FLAG_NO_MEDIA_CHANGE"        : "1 << 23",
    "GEOM_INST_FLAG_REFRACT"                : "1 << 24",
    "GEOM_INST_FLAG_REFLECT"                : "1 << 25",
    "GEOM_INST_FLAG_PORTAL"                 : "1 << 26",
    "GEOM_INST_FLAG_MEDIA_TYPE_WATER"       : "1 << 27",
    "GEOM_INST_FLAG_MEDIA_TYPE_GLASS"       : "1 << 28",
    "GEOM_INST_FLAG_GENERATE_NORMALS"       : "1 << 29",
    "GEOM_INST_FLAG_INVERTED_NORMALS"       : "1 << 30",
    "GEOM_INST_FLAG_IS_MOVABLE"             : "1 << 31",

    "SKY_TYPE_COLOR"                        : 0,
    "SKY_TYPE_CUBEMAP"                      : 1,
    "SKY_TYPE_RASTERIZED_GEOMETRY"          : 2,
    "SKY_TYPE_PROCEDURAL"                   : 3,

    "BLUE_NOISE_TEXTURE_COUNT"              : 128,
    "BLUE_NOISE_TEXTURE_SIZE"               : 128,
    "BLUE_NOISE_TEXTURE_SIZE_POW"           : RESOLVE_LATER,

    "COMPUTE_COMPOSE_GROUP_SIZE_X"          : 16,
    "COMPUTE_COMPOSE_GROUP_SIZE_Y"          : 16,

    "COMPUTE_BLOOM_UPSAMPLE_GROUP_SIZE_X"   : 16,
    "COMPUTE_BLOOM_UPSAMPLE_GROUP_SIZE_Y"   : 16,
    "COMPUTE_BLOOM_DOWNSAMPLE_GROUP_SIZE_X" : 16,
    "COMPUTE_BLOOM_DOWNSAMPLE_GROUP_SIZE_Y" : 16,
    "COMPUTE_BLOOM_APPLY_GROUP_SIZE_X"      : 16,
    "COMPUTE_BLOOM_APPLY_GROUP_SIZE_Y"      : 16,
    "COMPUTE_BLOOM_STEP_COUNT"              : 5,

    "COMPUTE_EFFECT_GROUP_SIZE_X"           : 16,
    "COMPUTE_EFFECT_GROUP_SIZE_Y"           : 16,

    "COMPUTE_LUM_HISTOGRAM_GROUP_SIZE_X"    : 16,
    "COMPUTE_LUM_HISTOGRAM_GROUP_SIZE_Y"    : 16,
    "COMPUTE_LUM_HISTOGRAM_BIN_COUNT"       : 128,

    "COMPUTE_VERT_PREPROC_GROUP_SIZE_X"     : 256,
    "VERT_PREPROC_MODE_ONLY_DYNAMIC"        : 0,
    "VERT_PREPROC_MODE_DYNAMIC_AND_MOVABLE" : 1,
    "VERT_PREPROC_MODE_ALL"                 : 2,

    "GRADIENT_ESTIMATION_ENABLED"           : int(GRADIENT_ESTIMATION_ENABLED),
    "COMPUTE_GRADIENT_ATROUS_GROUP_SIZE_X"  : 16,
    "COMPUTE_ANTIFIREFLY_GROUP_SIZE_X"      : 16,
    "COMPUTE_SVGF_TEMPORAL_GROUP_SIZE_X"    : 16,
    "COMPUTE_SVGF_VARIANCE_GROUP_SIZE_X"    : 16,
    "COMPUTE_SVGF_ATROUS_GROUP_SIZE_X"      : 16,
    "MAX_FOG_VOLUMES"                       : MAX_FOG_VOLUMES,
    "COMPUTE_SVGF_ATROUS_ITERATION_COUNT"   : 4,

    "COMPUTE_ASVGF_STRATA_SIZE"                         : 3,
    "COMPUTE_ASVGF_GRADIENT_ATROUS_ITERATION_COUNT"     : 4,

    "COMPUTE_INDIRECT_DRAW_FLARES_GROUP_SIZE_X"         : 256,
    "LENS_FLARES_MAX_DRAW_CMD_COUNT"                    : 512,

    "DEBUG_SHOW_FLAG_MOTION_VECTORS"        : "1 << 0",
    "DEBUG_SHOW_FLAG_GRADIENTS"             : "1 << 1",
    "DEBUG_SHOW_FLAG_UNFILTERED_DIFFUSE"    : "1 << 2",
    "DEBUG_SHOW_FLAG_UNFILTERED_SPECULAR"   : "1 << 3",
    "DEBUG_SHOW_FLAG_UNFILTERED_INDIRECT"   : "1 << 4",
    "DEBUG_SHOW_FLAG_ONLY_DIRECT_DIFFUSE"   : "1 << 5",
    "DEBUG_SHOW_FLAG_ONLY_SPECULAR"         : "1 << 6",
    "DEBUG_SHOW_FLAG_ONLY_INDIRECT_DIFFUSE" : "1 << 7",
    "DEBUG_SHOW_FLAG_ALBEDO_WHITE"          : "1 << 8",
    "DEBUG_SHOW_FLAG_GOD_RAYS"              : "1 << 9",
    "DEBUG_SHOW_FLAG_RAY_STATS"             : "1 << 10",
    "DEBUG_SHOW_FLAG_LUMA"                  : "1 << 11",

    "RAY_STATS_CATEGORY_COUNT"              : 5,

    "MAX_RAY_LENGTH"                        : "10000.0",

    "MEDIA_TYPE_VACUUM"                     : 0,
    "MEDIA_TYPE_WATER"                      : 1,
    "MEDIA_TYPE_GLASS"                      : 2,
    "MEDIA_TYPE_ACID"                       : 3,
    "MEDIA_TYPE_COUNT"                      : 4,

    "GEOM_INST_NO_TRIANGLE_INFO"            : "UINT32_MAX",

    "LIGHT_TYPE_NONE"                       : 0,
    "LIGHT_TYPE_DIRECTIONAL"                : 1,
    "LIGHT_TYPE_SPHERE"                     : 2,
    "LIGHT_TYPE_TRIANGLE"                   : 3,
    "LIGHT_TYPE_SPOT"                       : 4,
    "LIGHT_TYPE_TEXTURED_AREA"              : 5,

    "LIGHT_ARRAY_DIRECTIONAL_LIGHT_OFFSET"  : 0,
    "LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET"     : 1,

    "LIGHT_INDEX_NONE"                      : ((1 << 15) - 1),

    "TAL_CDF_LUT_ENTRIES"                   : 256,
    "TAL_CDF_EMPTY_ENTRY"                   : "0xFFFFFFFFu",
    "TAL_CDF_GRID_MAX_SIZE"                 : 256,

    "Q2_MAX_CLUSTERS"                       : 8192,
    "Q2_LIGHT_LIST_MAX_PER_CELL"            : 128,
    "Q2_LIGHT_LIST_STATS_SIDES"             : 6,
    "Q2_LIGHT_LIST_STATS_BUFFERS"           : 3,
    "COMPUTE_Q2_LIGHT_LIST_GROUP_SIZE_X"    : 64,

    "PORTAL_INDEX_NONE"                     : 63,
    "PORTAL_MAX_COUNT"                      : 63,

    "PACKED_INDIRECT_SAMPLE_SIZE_IN_WORDS"    : 6,
    "PACKED_INDIRECT_RESERVOIR_SIZE_IN_WORDS" : 8,

    "VOLUMETRIC_SIZE_X"                     : 160,
    "VOLUMETRIC_SIZE_Y"                     : 88,
    "VOLUMETRIC_SIZE_Z"                     : 64,
    "COMPUTE_VOLUMETRIC_GROUP_SIZE_X"       : 16,
    "COMPUTE_VOLUMETRIC_GROUP_SIZE_Y"       : 16,

    "VOLUME_ENABLE_NONE"                    : 0,
    "VOLUME_ENABLE_SIMPLE"                  : 1,
    "VOLUME_ENABLE_VOLUMETRIC"              : 2,
}

CONST_GLSL_ONLY = {
    "SURFACE_POSITION_INCORRECT"            : 10000000.0,
}


def round_up(value, alignment):
    return ((value + alignment - 1) // alignment) * alignment


def round_up4(value):
    return ((value + 3) >> 2) << 2


def resolve_derived_constants():
    assert CONST["MATERIAL_BLENDING_FLAG_BIT_COUNT"] * 3 <= 12

    blend_bits = CONST["MATERIAL_BLENDING_FLAG_BIT_COUNT"]
    blend_mask = (1 << blend_bits) - 1

    CONST["MATERIAL_BLENDING_MASK_FIRST_LAYER"]  = blend_mask << (blend_bits * 0)
    CONST["MATERIAL_BLENDING_MASK_SECOND_LAYER"] = blend_mask << (blend_bits * 1)
    CONST["MATERIAL_BLENDING_MASK_THIRD_LAYER"]  = blend_mask << (blend_bits * 2)

    CONST["MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW"] = int(log2(CONST["MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT"]))
    CONST["MAX_GEOMETRY_PRIMITIVE_COUNT_POW"]      = 32 - CONST["MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW"]
    CONST["MAX_GEOMETRY_PRIMITIVE_COUNT"]          = 1 << CONST["MAX_GEOMETRY_PRIMITIVE_COUNT_POW"]
    CONST["BLUE_NOISE_TEXTURE_SIZE_POW"]           = int(log2(CONST["BLUE_NOISE_TEXTURE_SIZE"]))

    assert len([None for _, value in CONST.items() if value == RESOLVE_LATER]) == 0, "Every derived constant must be calculated"


VERTEX_MEMBERS = [
    (FLOAT32, 4, "position",      1),
    (FLOAT32, 4, "normal",        1),
    (FLOAT32, 2, "texCoord",      1),
    (FLOAT32, 2, "texCoordLayer1", 1),
    (FLOAT32, 2, "texCoordLayer2", 1),
    (UINT32,  1, "packedColor",   1),
    (UINT32,  1, "cluster",       1),
    (UINT32,  1, "lightStyles",   1),
]

GLOBAL_UNIFORM_MEMBERS = [
    (FLOAT32, 44, "view",                         1),
    (FLOAT32, 44, "invView",                      1),
    (FLOAT32, 44, "viewPrev",                     1),
    (FLOAT32, 44, "projection",                   1),
    (FLOAT32, 44, "invProjection",                1),
    (FLOAT32, 44, "projectionPrev",               1),

    (FLOAT32, 44, "volumeViewProj",               1),
    (FLOAT32, 44, "volumeViewProjInv",            1),
    (FLOAT32, 44, "volumeViewProj_Prev",          1),
    (FLOAT32, 44, "volumeViewProjInv_Prev",       1),

    (FLOAT32, 1, "cellWorldSize",                 1),
    (FLOAT32, 1, "renderWidth",                   1),

    (UINT32, 1, "__pad0",                         1),
    (UINT32, 1, "__pad1",                         1),

    (FLOAT32, 1, "renderHeight",                  1),
    (UINT32, 1, "frameId",                        1),
    (FLOAT32, 1, "timeDelta",                     1),
    (FLOAT32, 1, "minLogLuminance",               1),

    (FLOAT32, 1, "maxLogLuminance",               1),
    (FLOAT32, 1, "luminanceWhitePoint",           1),
    (UINT32, 1, "stopEyeAdaptation",              1),
    (UINT32, 1, "directionalLightExists",         1),

    (FLOAT32, 1, "polyLightSpotlightFactor",      1),
    (UINT32, 1, "skyType",                        1),
    (FLOAT32, 1, "skyColorMultiplier",            1),
    (UINT32, 1, "skyCubemapIndex",                1),

    (FLOAT32, 4, "skyColorDefault",               1),

    (FLOAT32, 4, "cameraPosition",                1),
    (FLOAT32, 4, "cameraPositionPrev",            1),

    (UINT32, 1, "debugShowFlags",                 1),
    (UINT32, 1, "indirSecondBounce",              1),
    (UINT32, 1, "lightCount",                     1),
    (UINT32, 1, "lightCountPrev",                 1),

    (FLOAT32, 1, "emissionMapBoost",              1),
    (FLOAT32, 1, "emissionMaxScreenColor",        1),
    (FLOAT32, 1, "normalMapStrength",             1),
    (FLOAT32, 1, "skyColorSaturation",            1),

    (FLOAT32, 1, "emissionSharpMask",             1),
    (FLOAT32, 1, "talSelfLitOffset",              1),
    (UINT32, 1, "emissionBlendMode",              1),
    (FLOAT32, 1, "emissionBlendStrength",         1),

    (FLOAT32, 1, "skyAmbientLod",                 1),
    (FLOAT32, 1, "rayLength",                     1),
    (UINT32, 1, "rayCullBackFaces",               1),
    (UINT32, 1, "rayCullMaskWorld",               1),

    (FLOAT32, 1, "bloomIntensity",                1),
    (FLOAT32, 1, "bloomThreshold",                1),
    (FLOAT32, 1, "bloomEmissionMultiplier",       1),
    (UINT32, 1, "reflectRefractMaxDepth",         1),

    (UINT32, 1, "cameraMediaType",                1),
    (FLOAT32, 1, "indexOfRefractionWater",        1),
    (FLOAT32, 1, "indexOfRefractionGlass",        1),
    (FLOAT32, 1, "waterTextureDerivativesMultiplier", 1),

    (UINT32, 1, "volumeEnableType",               1),
    (FLOAT32, 1, "volumeScattering",              1),
    (UINT32, 1, "forceNoWaterRefraction",         1),
    (UINT32, 1, "waterNormalTextureIndex",        1),

    (UINT32, 1, "noBackfaceReflForNoMediaChange", 1),
    (FLOAT32, 1, "time",                          1),
    (FLOAT32, 1, "waterWaveSpeed",                1),
    (FLOAT32, 1, "waterWaveStrength",             1),

    (FLOAT32, 4, "waterColorAndDensity",          1),
    (FLOAT32, 4, "acidColorAndDensity",           1),

    (FLOAT32, 1, "cameraRayConeSpreadAngle",      1),
    (FLOAT32, 1, "waterTextureAreaScale",         1),
    (UINT32, 1, "squareInputRoughness",           1),
    (FLOAT32, 1, "upscaledRenderWidth",           1),

    (FLOAT32, 4, "worldUpVector",                 1),

    (FLOAT32, 1, "upscaledRenderHeight",          1),
    (FLOAT32, 1, "jitterX",                       1),
    (FLOAT32, 1, "jitterY",                       1),
    (FLOAT32, 1, "primaryRayMinDist",             1),

    (UINT32, 1, "rayCullMaskWorld_Shadow",        1),
    (UINT32, 1, "lensFlareCullingInputCount",     1),
    (UINT32, 1, "applyViewProjToLensFlares",      1),
    (UINT32, 1, "twirlPortalNormal",              1),

    (UINT32, 1, "lightIndexIgnoreFPVShadows",     1),
    (FLOAT32, 1, "gradientMultDiffuse",           1),
    (FLOAT32, 1, "gradientMultIndirect",          1),
    (FLOAT32, 1, "gradientMultSpecular",          1),

    (FLOAT32, 1, "minRoughness",                  1),
    (FLOAT32, 1, "volumeCameraNear",              1),
    (FLOAT32, 1, "volumeCameraFar",               1),
    (UINT32, 1, "antiFireflyEnabled",             1),

    (FLOAT32, 4, "volumeAmbient",                 1),
    (FLOAT32, 4, "volumeSourceColor",             1),
    (FLOAT32, 4, "volumeDirToSource",             1),

    (FLOAT32, 1, "volumeSourceAsymmetry",         1),
    (UINT32, 1, "coreQ2RTX",                      1),
    (UINT32, 1, "q2DepthGradMode",                1),
    (FLOAT32, 1, "skyNee",                        1),

    (UINT32, 1, "q2LightStatsMode",               1),
    (UINT32, 1, "reflRefrEarlyOut",               1),
    (UINT32, 1, "neeLightSamples",                1),
    (FLOAT32, 1, "turbWarpStrength",              1),

    (INT32, 4, "instanceGeomInfoOffset",       round_up4(CONST["MAX_TOP_LEVEL_INSTANCE_COUNT"]) // 4),
    (INT32, 4, "instanceGeomInfoOffsetPrev",   round_up4(CONST["MAX_TOP_LEVEL_INSTANCE_COUNT"]) // 4),
    (INT32, 4, "instanceGeomCount",            round_up4(CONST["MAX_TOP_LEVEL_INSTANCE_COUNT"]) // 4),
    (FLOAT32, 44, "viewProjCubemap",              6),
    (FLOAT32, 44, "skyCubemapRotationTransform",  1),

    (FLOAT32, 4, "fogMins",                  MAX_FOG_VOLUMES),
    (UINT32, 1, "fogIsActive",               MAX_FOG_VOLUMES),
    (FLOAT32, 4, "fogMaxs",                  MAX_FOG_VOLUMES),
    (FLOAT32, 4, "fogColor",                 MAX_FOG_VOLUMES),
    (FLOAT32, 4, "fogDensity",               MAX_FOG_VOLUMES),

    (FLOAT32, 4, "lightStyleScales",         16),

    (FLOAT32, 4, "giBounceRays",              1),

    (FLOAT32, 4, "fltEnable",                1),

    (FLOAT32, 4, "fixedAlbedo",              1),

    (FLOAT32, 4, "sunBounce",                1),

    (FLOAT32, 4, "levelFogColorDensity",     1),

    (FLOAT32, 4, "levelFogSkyBlend",         1),

    # Global-light RIS mode (host cvars rt_restir / rt_restir_candidates):
    # .x = 1 samples the direct lights from the global light array instead of
    # the per-cluster lists, .y = candidates drawn per NEE light sample.
    (UINT32,  4, "restirParams",             1),
]

GEOM_INSTANCE_MEMBERS = [
    (FLOAT32, 44, "model",                1),
    (FLOAT32, 44, "prevModel",            1),
    (FLOAT32, 4, "materialColors",       3),
    (UINT32, 1, "materials0A",          1),
    (UINT32, 1, "materials0B",          1),
    (UINT32, 1, "materials0C",          1),
    (UINT32, 1, "materials1A",          1),
    (UINT32, 1, "materials1B",          1),
    (UINT32, 1, "portalIndex",          1),
    (UINT32, 1, "materials2A",          1),
    (UINT32, 1, "materials2B",          1),
    (UINT32, 1, "_unused0",             1),
    (UINT32, 1, "flags",                1),
    (UINT32, 1, "baseVertexIndex",      1),
    (UINT32, 1, "baseIndexIndex",       1),
    (UINT32, 1, "prevBaseVertexIndex",  1),
    (UINT32, 1, "prevBaseIndexIndex",   1),
    (UINT32, 1, "vertexCount",          1),
    (UINT32, 1, "indexCount",           1),
    (FLOAT32, 1, "defaultRoughness",     1),
    (FLOAT32, 1, "defaultMetallicity",   1),
    (FLOAT32, 1, "defaultEmission",      1),
    (UINT32, 1, "_unused1",   1),
]

LIGHT_ENCODED_MEMBERS = [
    (FLOAT32, 3, "color",                1),
    (UINT32, 1, "lightType",            1),

    (FLOAT32, 4, "data_0",               1),
    (FLOAT32, 4, "data_1",               1),
    (FLOAT32, 4, "data_2",               1),
    (FLOAT32, 4, "data_3",               1),
    (FLOAT32, 4, "data_4",               1),
    (FLOAT32, 4, "data_5",               1),
    (FLOAT32, 4, "data_6",               1),
    (FLOAT32, 4, "data_7",               1),
    (FLOAT32, 1, "coneCosInner",         1),
    (FLOAT32, 1, "coneCosOuter",         1),
    (FLOAT32, 1, "projector",            1),
]

TONEMAPPING_MEMBERS = [
    (FLOAT32, 1, "tmExposureBias",           1),
    (FLOAT32, 1, "tmExposureSpeedDown",      1),
    (FLOAT32, 1, "tmExposureSpeedUp",        1),
    (FLOAT32, 1, "tmLowPercentile",          1),
    (FLOAT32, 1, "tmHighPercentile",         1),
    (FLOAT32, 1, "tmMinLuminance",           1),
    (FLOAT32, 1, "tmMaxLuminance",           1),
    (FLOAT32, 1, "tmNoiseBlend",             1),
    (FLOAT32, 1, "tmNoiseStops",             1),
    (FLOAT32, 1, "tmDynRangeStops",          1),
    (FLOAT32, 1, "tmReinhard",               1),
    (FLOAT32, 1, "tmKneeStart",              1),
    (FLOAT32, 1, "tmWhitePoint",             1),
    (FLOAT32, 1, "tmSlopeBlurSigma",         1),
    (FLOAT32, 1, "frameTime",                1),
    (UINT32, 1, "resetCurve",               1),
    (FLOAT32, 1, "kneeW",                    1),
    (FLOAT32, 1, "kneeA",                    1),
    (FLOAT32, 1, "kneeB",                    1),
    (UINT32, 1, "histogram",                CONST["COMPUTE_LUM_HISTOGRAM_BIN_COUNT"]),
    (FLOAT32, 1, "curve",                    CONST["COMPUTE_LUM_HISTOGRAM_BIN_COUNT"]),
    (FLOAT32, 1, "normalized",               CONST["COMPUTE_LUM_HISTOGRAM_BIN_COUNT"]),
    (FLOAT32, 1, "adaptedLuminance",         1),
    (FLOAT32, 1, "avgLuminance",             1),
]

VERT_PREPROC_PUSH_MEMBERS = [
    (UINT32, 1, "tlasInstanceCount",            1),
    (UINT32, 1, "tlasInstanceIsDynamicBits",    round_up(CONST["MAX_TOP_LEVEL_INSTANCE_COUNT"], 32) // 32),
]

INDIRECT_DRAW_CMD_MEMBERS = [
    (UINT32, 1, "indexCount",           1),
    (UINT32, 1, "instanceCount",        1),
    (UINT32, 1, "firstIndex",           1),
    (INT32,  1, "vertexOffset",         1),
    (UINT32, 1, "firstInstance",        1),
    (FLOAT32, 1, "positionToCheck_X",    1),
    (FLOAT32, 1, "positionToCheck_Y",    1),
    (FLOAT32, 1, "positionToCheck_Z",    1),
]

LENS_FLARES_INSTANCE_MEMBERS = [
    (UINT32, 1, "textureIndex",         1),
]

DECAL_INSTANCE_MEMBERS = [
    (FLOAT32, 44, "transform",                1),
    (UINT32, 1, "textureAlbedoAlpha",       1),
    (UINT32, 1, "textureRougnessMetallic",  1),
    (UINT32, 1, "textureNormals",           1),
]

PORTAL_INSTANCE_MEMBERS = [
    (FLOAT32, 4, "inPosition",               1),
    (FLOAT32, 4, "outPosition",              1),
    (FLOAT32, 4, "outDirection",             1),
    (FLOAT32, 4, "outUp",                    1),
]

ALIGN_NONE   = 0
ALIGN_STD430 = 1
ALIGN_STD140 = 2

BREAK_NONE    = 0
BREAK_COMPLEX = 1
BREAK_C_ONLY  = 2

STRUCTS = {
    "ShVertex":                 (VERTEX_MEMBERS,              False, ALIGN_STD430, BREAK_NONE),
    "ShGlobalUniform":          (GLOBAL_UNIFORM_MEMBERS,      False, ALIGN_STD140, BREAK_C_ONLY),
    "ShGeometryInstance":       (GEOM_INSTANCE_MEMBERS,       False, ALIGN_STD430, BREAK_NONE),
    "ShTonemapping":            (TONEMAPPING_MEMBERS,         False, ALIGN_NONE,   BREAK_NONE),
    "ShLightEncoded":           (LIGHT_ENCODED_MEMBERS,       False, ALIGN_STD430, BREAK_NONE),
    "ShVertPreprocessing":      (VERT_PREPROC_PUSH_MEMBERS,   False, ALIGN_NONE,   BREAK_NONE),
    "ShIndirectDrawCommand":    (INDIRECT_DRAW_CMD_MEMBERS,   False, ALIGN_STD430, BREAK_NONE),
    "ShLensFlareInstance":      (LENS_FLARES_INSTANCE_MEMBERS, False, ALIGN_NONE,  BREAK_NONE),
    "ShDecalInstance":          (DECAL_INSTANCE_MEMBERS,      False, ALIGN_STD430, BREAK_NONE),
    "ShPortalInstance":         (PORTAL_INSTANCE_MEMBERS,     False, ALIGN_STD140, BREAK_NONE),
}


FRAMEBUF_DESC_SET_NAME              = "DESC_SET_FRAMEBUFFERS"
FRAMEBUF_BASE_BINDING               = 0
FRAMEBUF_PREFIX                     = "framebuf"
FRAMEBUF_SAMPLED_POSTFIX            = "_Sampled"
FRAMEBUF_SAMPLER_POSTFIX            = "_Sampler"
FRAMEBUF_DEBUG_NAME_PREFIX          = "Framebuf "
FRAMEBUF_STORE_PREV_POSTFIX         = "_Prev"
FRAMEBUF_SAMPLER_INVALID_BINDING    = "FB_SAMPLER_INVALID_BINDING"

FRAMEBUF_FLAGS_STORE_PREV           = 1 << 0
FRAMEBUF_FLAGS_NO_SAMPLER           = 1 << 1
FRAMEBUF_FLAGS_IS_ATTACHMENT        = 1 << 2
FRAMEBUF_FLAGS_FORCE_SIZE_1_2       = 1 << 3
FRAMEBUF_FLAGS_FORCE_SIZE_1_3       = 1 << 4
FRAMEBUF_FLAGS_FORCE_SIZE_1_4       = 1 << 5
FRAMEBUF_FLAGS_FORCE_SIZE_1_8       = 1 << 6
FRAMEBUF_FLAGS_FORCE_SIZE_1_16      = 1 << 7
FRAMEBUF_FLAGS_FORCE_SIZE_1_32      = 1 << 8
FRAMEBUF_FLAGS_BILINEAR_SAMPLER     = 1 << 9
FRAMEBUF_FLAGS_UPSCALED_SIZE        = 1 << 10
FRAMEBUF_FLAGS_SINGLE_PIXEL_SIZE    = 1 << 11
FRAMEBUF_FLAGS_USAGE_TRANSFER       = 1 << 12

FRAMEBUF_FLAGS_ENUM = {
    "FRAMEBUF_FLAGS_IS_ATTACHMENT"      : FRAMEBUF_FLAGS_IS_ATTACHMENT,
    "FRAMEBUF_FLAGS_FORCE_SIZE_1_2"     : FRAMEBUF_FLAGS_FORCE_SIZE_1_2,
    "FRAMEBUF_FLAGS_FORCE_SIZE_1_3"     : FRAMEBUF_FLAGS_FORCE_SIZE_1_3,
    "FRAMEBUF_FLAGS_FORCE_SIZE_1_4"     : FRAMEBUF_FLAGS_FORCE_SIZE_1_4,
    "FRAMEBUF_FLAGS_FORCE_SIZE_1_8"     : FRAMEBUF_FLAGS_FORCE_SIZE_1_8,
    "FRAMEBUF_FLAGS_FORCE_SIZE_1_16"    : FRAMEBUF_FLAGS_FORCE_SIZE_1_16,
    "FRAMEBUF_FLAGS_FORCE_SIZE_1_32"    : FRAMEBUF_FLAGS_FORCE_SIZE_1_32,
    "FRAMEBUF_FLAGS_BILINEAR_SAMPLER"   : FRAMEBUF_FLAGS_BILINEAR_SAMPLER,
    "FRAMEBUF_FLAGS_UPSCALED_SIZE"      : FRAMEBUF_FLAGS_UPSCALED_SIZE,
    "FRAMEBUF_FLAGS_SINGLE_PIXEL_SIZE"  : FRAMEBUF_FLAGS_SINGLE_PIXEL_SIZE,
    "FRAMEBUF_FLAGS_USAGE_TRANSFER"     : FRAMEBUF_FLAGS_USAGE_TRANSFER,
}

FRAMEBUFFERS = {
    "Albedo"                            : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_IS_ATTACHMENT | FRAMEBUF_FLAGS_STORE_PREV),
    "IsSky"                             : (UINT8,     CHANNELS_R,    0),
    "Normal"                            : (UINT32,    CHANNELS_R,    FRAMEBUF_FLAGS_STORE_PREV),
    "NormalGeometry"                    : (UINT32,    CHANNELS_R,    FRAMEBUF_FLAGS_STORE_PREV),
    "MetallicRoughness"                 : (UNORM8,    CHANNELS_RG,   FRAMEBUF_FLAGS_STORE_PREV),
    "DepthWorld"                        : (FLOAT16,   CHANNELS_R,    FRAMEBUF_FLAGS_STORE_PREV),
    "DepthGrad"                         : (FLOAT16,   CHANNELS_R,    0),
    "DepthNdc"                          : (FLOAT32,   CHANNELS_R,    0),
    "Motion"                            : (FLOAT16,   CHANNELS_RGBA, 0),
    "UnfilteredDirect"                  : (PACKED_E5, CHANNELS_RGB,  0),
    "UnfilteredSpecular"                : (PACKED_E5, CHANNELS_RGB,  0),
    "UnfilteredIndirectSH_R"            : (FLOAT16,   CHANNELS_RGBA, 0),
    "UnfilteredIndirectSH_G"            : (FLOAT16,   CHANNELS_RGBA, 0),
    "UnfilteredIndirectSH_B"            : (FLOAT16,   CHANNELS_RGBA, 0),
    "SurfacePosition"                   : (FLOAT32,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),
    "VisibilityBuffer"                  : (FLOAT32,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),
    "ViewDirection"                     : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),
    "PrimaryToReflRefr"                 : (UINT32,    CHANNELS_RGBA, 0),
    "Throughput"                        : (FLOAT16,   CHANNELS_RGBA, 0),
    "PreFinal"                          : (PACKED_11, CHANNELS_RGB,  0),
    "Final"                             : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_IS_ATTACHMENT),

    "UpscaledPing"                      : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_IS_ATTACHMENT | FRAMEBUF_FLAGS_UPSCALED_SIZE | FRAMEBUF_FLAGS_USAGE_TRANSFER),
    "UpscaledPong"                      : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_IS_ATTACHMENT | FRAMEBUF_FLAGS_UPSCALED_SIZE | FRAMEBUF_FLAGS_USAGE_TRANSFER),

    "MotionDlss"                        : (FLOAT16,   CHANNELS_RG,   0),

    "AccumHistoryLength"                : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),

    "DiffTemporary"                     : (PACKED_E5, CHANNELS_RGB,  0),
    "DiffAccumColor"                    : (PACKED_E5, CHANNELS_RGB,  FRAMEBUF_FLAGS_STORE_PREV),
    "DiffAccumMoments"                  : (FLOAT16,   CHANNELS_RG,   FRAMEBUF_FLAGS_STORE_PREV),
    "DiffColorHistory"                  : (FLOAT16,   CHANNELS_RGBA, 0),
    "DiffPingColorAndVariance"          : (FLOAT16,   CHANNELS_RGBA, 0),
    "DiffPongColorAndVariance"          : (FLOAT16,   CHANNELS_RGBA, 0),

    "SpecAccumColor"                    : (PACKED_E5, CHANNELS_RGB,  FRAMEBUF_FLAGS_STORE_PREV),
    "SpecPingColor"                     : (PACKED_E5, CHANNELS_RGB,  0),
    "SpecPongColor"                     : (PACKED_E5, CHANNELS_RGB,  0),

    "IndirAccumSH_R"                    : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),
    "IndirAccumSH_G"                    : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),
    "IndirAccumSH_B"                    : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),
    "IndirPingSH_R"                     : (FLOAT16,   CHANNELS_RGBA, 0),
    "IndirPingSH_G"                     : (FLOAT16,   CHANNELS_RGBA, 0),
    "IndirPingSH_B"                     : (FLOAT16,   CHANNELS_RGBA, 0),
    "IndirPongSH_R"                     : (FLOAT16,   CHANNELS_RGBA, 0),
    "IndirPongSH_G"                     : (FLOAT16,   CHANNELS_RGBA, 0),
    "IndirPongSH_B"                     : (FLOAT16,   CHANNELS_RGBA, 0),

    "AtrousFilteredVariance"            : (FLOAT16,   CHANNELS_R,    0),

    "AcidFogRT"                         : (PACKED_11, CHANNELS_RGB,  0),
    "AcidFog"                           : (PACKED_11, CHANNELS_RGB,  0),

    "ScreenEmisRT"                      : (PACKED_11, CHANNELS_RGB,  0),
    "ScreenEmission"                    : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_IS_ATTACHMENT),
    "GodRays"                           : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_FORCE_SIZE_1_2),
    "GodRaysFiltered"                   : (PACKED_11, CHANNELS_RGB,  0),
    "BloomInput"                        : (PACKED_11, CHANNELS_RGB,  0),
    "Bloom_Mip1"                        : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_FORCE_SIZE_1_2  | FRAMEBUF_FLAGS_BILINEAR_SAMPLER),
    "Bloom_Mip2"                        : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_FORCE_SIZE_1_4  | FRAMEBUF_FLAGS_BILINEAR_SAMPLER),
    "Bloom_Mip3"                        : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_FORCE_SIZE_1_8  | FRAMEBUF_FLAGS_BILINEAR_SAMPLER),
    "Bloom_Mip4"                        : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_FORCE_SIZE_1_16 | FRAMEBUF_FLAGS_BILINEAR_SAMPLER),
    "Bloom_Mip5"                        : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_FORCE_SIZE_1_32 | FRAMEBUF_FLAGS_BILINEAR_SAMPLER),
    "Bloom_Result"                      : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_BILINEAR_SAMPLER),

    "WipeEffectSource"                  : (PACKED_11, CHANNELS_RGB,  FRAMEBUF_FLAGS_UPSCALED_SIZE | FRAMEBUF_FLAGS_USAGE_TRANSFER),
}

if GRADIENT_ESTIMATION_ENABLED:
    FRAMEBUFFERS.update({
        "GradientInputs"                : (FLOAT16,   CHANNELS_RG,   FRAMEBUF_FLAGS_STORE_PREV),
        "DISPingGradient"               : (UNORM8,    CHANNELS_RGBA, FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "DISPongGradient"               : (UNORM8,    CHANNELS_RGBA, FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "DISGradientHistory"            : (UNORM8,    CHANNELS_RGBA, FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "GradientPrevPix"               : (UINT8,     CHANNELS_R,    FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
    })

Q2_CORE_ENABLED = True

if Q2_CORE_ENABLED:
    FRAMEBUFFERS.update({
        "Q2ColorLF_SH"                  : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),
        "Q2ColorLF_COCG"                : (FLOAT16,   CHANNELS_RG,   FRAMEBUF_FLAGS_STORE_PREV),
        "Q2ColorHF"                     : (UINT32,    CHANNELS_R,    FRAMEBUF_FLAGS_STORE_PREV),
        "Q2ColorSpec"                   : (UINT32,    CHANNELS_R,    FRAMEBUF_FLAGS_STORE_PREV),

        "Q2ViewDepth"                   : (FLOAT32,   CHANNELS_R,    FRAMEBUF_FLAGS_STORE_PREV),
        "Q2BaseColor"                   : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),
        "Q2Metallic"                    : (UNORM8,    CHANNELS_RG,   FRAMEBUF_FLAGS_STORE_PREV),
        "Q2BounceThroughput"            : (FLOAT16,   CHANNELS_RGBA, 0),
        "Q2Transparent"                 : (FLOAT16,   CHANNELS_RGBA, 0),
        "Q2GodRaysThroughputDist"       : (FLOAT16,   CHANNELS_RGBA, 0),
        "Q2FogAccum"                    : (FLOAT16,   CHANNELS_RGBA, 0),

        "Q2HistColorLF_SH"              : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),
        "Q2HistColorLF_COCG"            : (FLOAT16,   CHANNELS_RG,   FRAMEBUF_FLAGS_STORE_PREV),
        "Q2HistColorHF"                 : (UINT32,    CHANNELS_R,    FRAMEBUF_FLAGS_STORE_PREV),
        "Q2HistMomentsHF"               : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),
        "Q2FilteredSpec"                : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_STORE_PREV),

        "Q2AtrousPingLF_SH"             : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "Q2AtrousPongLF_SH"             : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "Q2AtrousPingLF_COCG"           : (FLOAT16,   CHANNELS_RG,   FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "Q2AtrousPongLF_COCG"           : (FLOAT16,   CHANNELS_RG,   FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "Q2AtrousPingHF"                : (UINT32,    CHANNELS_R,    0),
        "Q2AtrousPongHF"                : (UINT32,    CHANNELS_R,    0),
        "Q2AtrousPingSpec"              : (UINT32,    CHANNELS_R,    0),
        "Q2AtrousPongSpec"              : (UINT32,    CHANNELS_R,    0),
        "Q2AtrousPingMoments"           : (FLOAT16,   CHANNELS_RG,   0),
        "Q2AtrousPongMoments"           : (FLOAT16,   CHANNELS_RG,   0),

        "Q2GradLFPing"                  : (FLOAT16,   CHANNELS_RG,   FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "Q2GradLFPong"                  : (FLOAT16,   CHANNELS_RG,   FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "Q2GradHFSpecPing"              : (FLOAT16,   CHANNELS_RG,   FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "Q2GradHFSpecPong"              : (FLOAT16,   CHANNELS_RG,   FRAMEBUF_FLAGS_FORCE_SIZE_1_3),
        "Q2GradSmplPos"                 : (UINT32,    CHANNELS_R,    FRAMEBUF_FLAGS_FORCE_SIZE_1_3 | FRAMEBUF_FLAGS_STORE_PREV),

        "Q2Color"                       : (FLOAT16,   CHANNELS_RGBA, 0),

        "Q2TaaOutput"                   : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_UPSCALED_SIZE),
        "Q2TaaHistory"                  : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_UPSCALED_SIZE | FRAMEBUF_FLAGS_STORE_PREV),
        "Q2RngSeed"                     : (UINT32,    CHANNELS_R,    FRAMEBUF_FLAGS_STORE_PREV),

        "Q2Cluster"                     : (UINT32,    CHANNELS_R,    0),
    })

FRAMEBUFFERS.update({
    "Caustics"                          : (FLOAT16,   CHANNELS_RGBA, FRAMEBUF_FLAGS_IS_ATTACHMENT),
})


def emit_constants(constants):
    lines = ["#define %s (%s)" % (name, value) for name, value in constants.items()]
    return "\n".join(lines) + "\n\n"


def member_size_bytes(base_type, dim, count):
    if dim == 1:
        return TYPE_BYTES[base_type] * count
    return TYPE_BYTES[(base_type, dim)] * count


def member_declaration(base_type, dim, member_name, count, type_names, alignment, break_mode):
    if count == 1:
        if dim == 1:
            return "%s %s" % (type_names[base_type], member_name)
        if (base_type, dim) in type_names:
            return "%s %s" % (type_names[(base_type, dim)], member_name)
        if dim <= 4:
            return "%s %s[%d]" % (type_names[base_type], member_name, dim)
        if MULTIDIMENSIONAL_ARRAYS_IN_C:
            return "%s %s[%d][%d]" % (type_names[base_type], member_name, dim // 10, dim % 10)
        return "%s %s[%d]" % (type_names[base_type], member_name, (dim // 10) * (dim % 10))

    if break_mode == BREAK_COMPLEX or (type_names is C_TYPES and break_mode == BREAK_C_ONLY):
        if dim <= 4 and alignment == ALIGN_STD140:
            array_size = round_up4(count * 4)
        else:
            array_size = round_up4(count * int(TYPE_BYTES[(base_type, dim)] / 4))
        return "%s %s[%d]" % (type_names[base_type], member_name, array_size)

    if dim == 1:
        return "%s %s[%d]" % (type_names[base_type], member_name, count)
    if (base_type, dim) in type_names:
        return "%s %s[%d]" % (type_names[(base_type, dim)], member_name, count)
    return "%s %s[%d][%d]" % (type_names[base_type], member_name, count, dim)


def pad_members(type_names, count):
    return "\n".join("    " + type_names[UINT32] + " __pad%d;" % index for index in range(count))


def emit_struct(struct_name, members, type_names, alignment, break_mode):
    if alignment == ALIGN_STD140 and type_names is C_TYPES:
        print("Struct \"" + struct_name + "\" is using std140, alignment must be set manually.")

    lines = ["struct " + struct_name, "{"]
    size = 0

    for base_type, dim, member_name, count in members:
        assert count > 0
        lines.append("    " + member_declaration(base_type, dim, member_name, count, type_names, alignment, break_mode) + ";")
        if alignment == ALIGN_STD430:
            size += member_size_bytes(base_type, dim, count)

    if alignment == ALIGN_STD430 and size % 16 != 0:
        if (size % 16) % 4 != 0:
            raise Exception("Size of struct %s is not 4-byte aligned!" % struct_name)
        lines.append(pad_members(type_names, (round_up(size, 16) - size) // 4))

    lines.append("};")
    return "\n".join(lines) + "\n"


def emit_structs(type_names):
    return "\n".join(
        emit_struct(name, members, type_names, alignment, break_mode)
        for name, (members, only_for_glsl, alignment, break_mode) in STRUCTS.items()
        if not (only_for_glsl and type_names is C_TYPES)
    ) + "\n"


def emit_glsl_pack_unpack(name, with_prev):
    image_store = ("void imageStore%s(const ivec2 pix, const vec3 unpacked) "
                   "{ imageStore(%s, pix, uvec4(encodeE5B9G9R9(unpacked))); }")
    texel_fetch = ("vec3 texelFetch%s(const ivec2 pix)"
                   "{ return decodeE5B9G9R9(texelFetch(%s, pix, 0).r); }")

    text = image_store % (name, FRAMEBUF_PREFIX + name) + "\n"
    text += texel_fetch % (name, FRAMEBUF_PREFIX + name + FRAMEBUF_SAMPLED_POSTFIX) + "\n"
    if with_prev:
        text += texel_fetch % (name + FRAMEBUF_STORE_PREV_POSTFIX,
                               FRAMEBUF_PREFIX + name + FRAMEBUF_STORE_PREV_POSTFIX + FRAMEBUF_SAMPLED_POSTFIX) + "\n"
    return text


def emit_hlsl_pack_unpack(name, with_prev):
    image_store = ("void imageStore%s(const int2 pix, const float3 unpacked) "
                   "{ %s[pix] = (uint4)encodeE5B9G9R9(unpacked); }")
    texel_fetch = ("float3 texelFetch%s(const int2 pix)"
                   "{ return decodeE5B9G9R9(%s.Load(int3(pix, 0)).r); }")

    text = image_store % (name, FRAMEBUF_PREFIX + name) + "\n"
    text += texel_fetch % (name, FRAMEBUF_PREFIX + name + FRAMEBUF_SAMPLED_POSTFIX) + "\n"
    if with_prev:
        text += texel_fetch % (name + FRAMEBUF_STORE_PREV_POSTFIX,
                               FRAMEBUF_PREFIX + name + FRAMEBUF_STORE_PREV_POSTFIX + FRAMEBUF_SAMPLED_POSTFIX) + "\n"
    return text


def emit_guarded_declaration(syntax, kind, name, base_format, channels, flags, binding):
    declaration = syntax[kind](
        binding, syntax["format"](base_format, channels),
        syntax[kind + "Type"](base_format), name)

    if flags & FRAMEBUF_FLAGS_IS_ATTACHMENT:
        return "#ifndef " + FRAMEBUF_IGNORE_ATTACHMENTS_DEFINE + "\n" + declaration + "\n#endif"

    return declaration


def emit_descriptor_group(syntax, kind, first_binding):
    postfix = {"storage": "", "sampled": FRAMEBUF_SAMPLED_POSTFIX, "sampler": FRAMEBUF_SAMPLER_POSTFIX}[kind]

    entries = []
    binding = first_binding

    for name, (base_format, channels, flags) in FRAMEBUFFERS.items():
        if kind == "sampler" and flags & FRAMEBUF_FLAGS_NO_SAMPLER:
            binding += 2 if flags & FRAMEBUF_FLAGS_STORE_PREV else 1
            continue

        guarded = flags & FRAMEBUF_FLAGS_IS_ATTACHMENT
        entry = ""

        if guarded:
            entry += "#ifndef " + FRAMEBUF_IGNORE_ATTACHMENTS_DEFINE + "\n"

        entry += syntax[kind](
            binding, syntax["format"](base_format, channels),
            syntax[kind + "Type"](base_format), FRAMEBUF_PREFIX + name + postfix)
        binding += 1

        if flags & FRAMEBUF_FLAGS_STORE_PREV:
            entry += "\n" + emit_guarded_declaration(
                syntax, kind, FRAMEBUF_PREFIX + name + FRAMEBUF_STORE_PREV_POSTFIX + postfix,
                base_format, channels, flags & ~FRAMEBUF_FLAGS_STORE_PREV, binding)
            binding += 1

        if guarded:
            entry += "\n#endif"

        entries.append(entry)

    return "\n".join(entries), binding - first_binding


def emit_framebuffer_declarations(syntax, pack_unpack):
    framebuffers, framebuffer_count = emit_descriptor_group(syntax, "storage", FRAMEBUF_BASE_BINDING)
    sampled, _ = emit_descriptor_group(syntax, "sampled", FRAMEBUF_BASE_BINDING + framebuffer_count)
    samplers, _ = emit_descriptor_group(syntax, "sampler", FRAMEBUF_BASE_BINDING + 2 * framebuffer_count)

    return "#ifdef " + FRAMEBUF_DESC_SET_NAME \
        \
        + "\n\n// framebuffer indices\n" \
        \
        + "\n".join(
        "#define FB_IMAGE_INDEX_%s %d" % (name, index) for (name, index) in framebuffer_enum_entries()
        ) \
        \
        + "\n\n// framebuffers\n" \
        \
        + framebuffers \
        \
        + "\n\n// sampled framebuffers\n" \
        + sampled \
        \
        + "\n\n// samplers\n" \
        + samplers \
        \
        + "\n\n// pack/unpack formats\n" \
        + "\n".join(
            pack_unpack(name, flags & FRAMEBUF_FLAGS_STORE_PREV)
            for name, (base_format, channels, flags) in FRAMEBUFFERS.items()
            if base_format == PACKED_E5 and not (flags & FRAMEBUF_FLAGS_NO_SAMPLER)
        ) \
        \
        + "\n\n#endif\n"


def emit_glsl_framebuffer_declarations():
    return emit_framebuffer_declarations(DESCRIPTOR_SYNTAX["glsl"], emit_glsl_pack_unpack)


def emit_hlsl_framebuffer_declarations():
    return emit_framebuffer_declarations(DESCRIPTOR_SYNTAX["hlsl"], emit_hlsl_pack_unpack)


def collapse_repeated(characters, character_to_remove="_"):
    result = ""
    for index in range(len(characters)):
        if index == 0 or characters[index] != characters[index - 1] or characters[index] != character_to_remove:
            result += characters[index]
    return result


def enum_name(name):
    return collapse_repeated("_".join(filter(None, re.split("([A-Z][^A-Z]*)", name))).upper())


def framebuffer_enum_entries():
    names = []
    for name, (_, _, flags) in FRAMEBUFFERS.items():
        names.append(name)
        if flags & FRAMEBUF_FLAGS_STORE_PREV:
            names.append(name + FRAMEBUF_STORE_PREV_POSTFIX)

    return [(enum_name(names[index]), index) for index in range(len(names))]


def emit_framebuffer_constants():
    binding_constant = "#define " + FRAMEBUF_SAMPLER_INVALID_BINDING + " 0xFFFFFFFF\n\n"

    image_indices = "enum FramebufferImageIndex\n{\n" + "\n".join(
        "    FB_IMAGE_INDEX_%s = %d," % (name, index) for (name, index) in framebuffer_enum_entries()
    ) + "\n};\n\n"

    image_flags = "enum FramebufferImageFlagBits\n{\n" + "\n".join(
        "    FB_IMAGE_FLAGS_%s = %d," % (name, value)
        for (name, value) in FRAMEBUF_FLAGS_ENUM.items()
    ) + "\n};\ntypedef uint32_t FramebufferImageFlags;\n\n"

    return binding_constant + image_indices + image_flags


def public_flag_expression(flags):
    expression = " | ".join(
        "qray::FB_IMAGE_FLAGS_" + name
        for (name, value) in FRAMEBUF_FLAGS_ENUM.items()
        if value & flags
    )

    return expression if expression else "0"


def emit_vulkan_framebuffer_declarations():
    return ("extern const uint32_t ShFramebuffers_Count;\n"
            "extern const VkFormat ShFramebuffers_Formats[];\n"
            "extern const FramebufferImageFlags ShFramebuffers_Flags[];\n"
            "extern const uint32_t ShFramebuffers_Bindings[];\n"
            "extern const uint32_t ShFramebuffers_BindingsSwapped[];\n"
            "extern const uint32_t ShFramebuffers_Sampled_Bindings[];\n"
            "extern const uint32_t ShFramebuffers_Sampled_BindingsSwapped[];\n"
            "extern const uint32_t ShFramebuffers_Sampler_Bindings[];\n"
            "extern const uint32_t ShFramebuffers_Sampler_BindingsSwapped[];\n"
            "extern const char *const ShFramebuffers_DebugNames[];\n\n")


def emit_bindings(first_binding, with_sampler):
    tab = "    "
    bindings = ""
    bindings_swapped = ""
    count = 0

    for name, (base_format, channels, flags) in FRAMEBUFFERS.items():
        if with_sampler and flags & FRAMEBUF_FLAGS_NO_SAMPLER:
            current = next_binding = FRAMEBUF_SAMPLER_INVALID_BINDING
        else:
            current = str(first_binding + count)
            next_binding = str(first_binding + count + 1)

        if not flags & FRAMEBUF_FLAGS_STORE_PREV:
            bindings += tab + current + ",\n"
            bindings_swapped += tab + current + ",\n"
        else:
            bindings += tab + current + ",\n"
            bindings += tab + next_binding + ",\n"
            bindings_swapped += tab + next_binding + ",\n"
            bindings_swapped += tab + current + ",\n"
            count += 1

        count += 1

    return bindings, bindings_swapped


def emit_vulkan_framebuffer_definitions():
    template = ("const uint32_t qray::ShFramebuffers_Count = %d;\n\n"
                "const VkFormat qray::ShFramebuffers_Formats[] = \n{\n%s};\n\n"
                "const qray::FramebufferImageFlags qray::ShFramebuffers_Flags[] = \n{\n%s};\n\n"
                "const uint32_t qray::ShFramebuffers_Bindings[] = \n{\n%s};\n\n"
                "const uint32_t qray::ShFramebuffers_BindingsSwapped[] = \n{\n%s};\n\n"
                "const uint32_t qray::ShFramebuffers_Sampled_Bindings[] = \n{\n%s};\n\n"
                "const uint32_t qray::ShFramebuffers_Sampled_BindingsSwapped[] = \n{\n%s};\n\n"
                "const uint32_t qray::ShFramebuffers_Sampler_Bindings[] = \n{\n%s};\n\n"
                "const uint32_t qray::ShFramebuffers_Sampler_BindingsSwapped[] = \n{\n%s};\n\n"
                "const char *const qray::ShFramebuffers_DebugNames[] = \n{\n%s};\n\n")

    tab = "    "
    formats = ""
    public_flags = ""
    names = ""
    count = 0

    for name, (base_format, channels, flags) in FRAMEBUFFERS.items():
        formats += tab + VULKAN_FORMATS[(base_format, channels)] + ",\n"
        names += tab + "\"" + FRAMEBUF_DEBUG_NAME_PREFIX + name + "\",\n"
        public_flags += tab + public_flag_expression(flags) + ",\n"

        if flags & FRAMEBUF_FLAGS_STORE_PREV:
            formats += tab + VULKAN_FORMATS[(base_format, channels)] + ",\n"
            names += tab + "\"" + FRAMEBUF_DEBUG_NAME_PREFIX + name + FRAMEBUF_STORE_PREV_POSTFIX + "\",\n"
            public_flags += tab + public_flag_expression(flags) + ",\n"
            count += 1

        count += 1

    bindings, bindings_swapped = emit_bindings(FRAMEBUF_BASE_BINDING, False)
    sampled_bindings, sampled_bindings_swapped = emit_bindings(FRAMEBUF_BASE_BINDING + count, False)
    sampler_bindings, sampler_bindings_swapped = emit_bindings(FRAMEBUF_BASE_BINDING + 2 * count, True)

    return template % (count, formats, public_flags, bindings, bindings_swapped,
                       sampled_bindings, sampled_bindings_swapped,
                       sampler_bindings, sampler_bindings_swapped, names)


FILE_HEADER = "// This file was generated by GenerateShaderCommon.py\n\n"


def write_c_files(common_header_file, framebuffer_header_file, framebuffer_source_file):
    common_header_file.write(FILE_HEADER)
    common_header_file.write("#pragma once\n\n")
    common_header_file.write("namespace qray\n{\n\n")
    common_header_file.write("#include <stdint.h>\n\n")
    common_header_file.write(emit_constants(CONST))
    common_header_file.write(emit_structs(C_TYPES))
    common_header_file.write("}")

    framebuffer_header_file.write(FILE_HEADER)
    framebuffer_header_file.write("#pragma once\n\n")
    framebuffer_header_file.write("#include \"../Common.h\"\n\n")
    framebuffer_header_file.write("namespace qray\n{\n\n")
    framebuffer_header_file.write(emit_framebuffer_constants())
    framebuffer_header_file.write(emit_vulkan_framebuffer_declarations())
    framebuffer_header_file.write("}")

    framebuffer_source_file.write(FILE_HEADER)
    framebuffer_source_file.write("#include \"%s\"\n\n" % os.path.basename(framebuffer_header_file.name))
    framebuffer_source_file.write(emit_vulkan_framebuffer_definitions())


def write_glsl_header(file):
    file.write(FILE_HEADER)
    file.write(emit_constants(CONST))
    file.write(emit_constants(CONST_GLSL_ONLY))
    file.write(emit_structs(GLSL_TYPES))
    file.write(emit_glsl_framebuffer_declarations())


def write_hlsl_header(file):
    file.write(FILE_HEADER)
    file.write("#pragma once\n\n")
    file.write(emit_constants(CONST))
    file.write(emit_constants(CONST_GLSL_ONLY))
    file.write(emit_structs(HLSL_TYPES))
    file.write(emit_hlsl_framebuffer_declarations())


def regenerate_probe():
    shaders_folder = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "Shaders")
    script_name = "GenerateShaderCommonProbe.py"

    if not os.path.isfile(os.path.join(shaders_folder, script_name)):
        return

    print("Regenerating the ShaderCommon probe pair...")
    subprocess.check_call([sys.executable, script_name], cwd=shaders_folder)


def main():
    base_path = ""

    for index in range(len(sys.argv)):
        if "--help" == sys.argv[index] or "-help" == sys.argv[index]:
            print("--path     : specify path to target folder in the next argument")
            return
        if "--path" == sys.argv[index]:
            if index + 1 < len(sys.argv):
                base_path = sys.argv[index + 1]
                if not os.path.exists(base_path):
                    print("Folder with path \"" + base_path + "\" doesn't exist.")
                    return
            else:
                print("--path expects folder path in the next argument.")
                return

    if base_path and not base_path.endswith(("/", "\\")):
        base_path += os.sep

    resolve_derived_constants()

    with open(base_path + "ShaderCommonC.h", "w") as common_header_file:
        with open(base_path + "ShaderCommonCFramebuf.h", "w") as framebuffer_header_file:
            with open(base_path + "ShaderCommonCFramebuf.cpp", "w") as framebuffer_source_file:
                write_c_files(common_header_file, framebuffer_header_file, framebuffer_source_file)

    with open(base_path + "ShaderCommonGLSL.h", "w") as glsl_header_file:
        write_glsl_header(glsl_header_file)

    with open(base_path + "ShaderCommonHLSL.hlsli", "w") as hlsl_header_file:
        write_hlsl_header(hlsl_header_file)

    regenerate_probe()


if __name__ == "__main__":
    main()
