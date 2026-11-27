// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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

// Q2RTX-style .mat material definitions (phase 4.5).
// Ported from Q2RTX material.c (GPL v2) and adapted to the vkquake host.

#include "quakedef.h"
#include "rt_material.h"
#include "rt_pkz.h"

#include <errno.h>
#include <math.h>

#ifndef _WIN32
#include <dirent.h>
#endif

#include <yaml.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4505)
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STBI_MALLOC(sz)		Mem_Alloc (sz)
#define STBI_REALLOC(p, newsz) Mem_Realloc (p, newsz)
#define STBI_FREE(p)		Mem_Free (p)
#include "stb_image.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define RT_MAT_MAX_MATERIALS 2048
#define RT_MAT_MAX_GLOBAL   RT_MAT_CAP_GLOBAL
#define RT_MAT_MAX_MAP      RT_MAT_CAP_MAP
#define RT_MAT_FEATHER_MAX  128.0f
#define RT_MAT_PARALLAX_MAX 0.25f

static rt_material_t rt_global_materials[RT_MAT_MAX_GLOBAL];
static int rt_global_count = 0;
static rt_material_t rt_map_materials[RT_MAT_MAX_MAP];
static int rt_map_count = 0;
static qboolean rt_initialized = false;
static cmd_function_t *rt_mat_cmd = NULL;
static char rt_current_map[MAX_QPATH];

static cvar_t rt_materials = { "rt_materials", "1", CVAR_ARCHIVE };
cvar_t rt_mat_debug = { "rt_mat_debug", "0", 0 };

static byte *rt_load_file(const char *name, int *outLen)
{
    unsigned int path_id;
    byte *fs = COM_LoadFile(name, &path_id);
    if (fs)
    {
        if (outLen)
        {
            *outLen = com_filesize;
        }
        return fs;
    }

    if (outLen)
    {
        *outLen = 0;
    }
    return NULL;
}

static void rt_load_file_free(byte *data)
{
    Mem_Free(data);
}

#define RT_TGA_MAX_DIM 16384

static byte *rt_decode_tga(const byte *buf, int len, int *outW, int *outH)
{
    if (!buf || len < 18)
    {
        return NULL;
    }

    const size_t ulen = (size_t)len;
    const byte   id_length = buf[0];
    const byte   colormap_type = buf[1];
    const byte   image_type = buf[2];
    const int    width = (int)buf[12] | ((int)buf[13] << 8);
    const int    height = (int)buf[14] | ((int)buf[15] << 8);
    const byte   pixel_size = buf[16];
    const byte   attributes = buf[17];

    if (colormap_type != 0)
    {
        return NULL;
    }
    if (image_type != 2 && image_type != 10)
    {
        return NULL;
    }
    if (pixel_size != 24 && pixel_size != 32)
    {
        return NULL;
    }
    if (width <= 0 || height <= 0 || width > RT_TGA_MAX_DIM || height > RT_TGA_MAX_DIM)
    {
        return NULL;
    }

    size_t pos = 18u + (size_t)id_length;
    if (pos > ulen)
    {
        return NULL;
    }

    const size_t bpp = (size_t)(pixel_size / 8);
    const size_t uw = (size_t)width;
    const size_t uh = (size_t)height;
    if (uh > SIZE_MAX / uw)
    {
        return NULL;
    }
    const size_t numPixels = uw * uh;
    if (numPixels > SIZE_MAX / 4u)
    {
        return NULL;
    }

    // Reject truncated files before allocating the output buffer.
    if (image_type == 2)
    {
        if (numPixels > (ulen - pos) / bpp)
        {
            return NULL;
        }
    }
    else
    {
        // Each RLE packet consumes at least 1 + bpp bytes and encodes at most 128 pixels.
        const size_t minPackets = (numPixels + 127u) / 128u;
        if (minPackets > (ulen - pos) / (1u + bpp))
        {
            return NULL;
        }
    }

    const qboolean upside_down = !(attributes & 0x20);
    const size_t   rowStride = uw * 4u;
    byte          *out = (byte *)Mem_Alloc(numPixels * 4u);
    if (!out)
    {
        return NULL;
    }

    for (int row = height - 1; row >= 0; row--)
    {
        const size_t realrow = (size_t)(upside_down ? row : (height - 1 - row));
        byte        *pixbuf = out + realrow * rowStride;

        for (int column = 0; column < width; column++)
        {
            if (image_type == 10)
            {
                if (pos >= ulen)
                {
                    Mem_Free(out);
                    return NULL;
                }
                const byte packet = buf[pos++];
                const int  count = (int)(packet & 0x7f) + 1;
                if (packet & 0x80)
                {
                    if (bpp > ulen - pos || count > width - column)
                    {
                        Mem_Free(out);
                        return NULL;
                    }
                    const byte r = buf[pos + 2];
                    const byte g = buf[pos + 1];
                    const byte b = buf[pos + 0];
                    const byte a = (bpp == 4u) ? buf[pos + 3] : 255;
                    pos += bpp;
                    for (int k = 0; k < count; k++)
                    {
                        *pixbuf++ = r; *pixbuf++ = g; *pixbuf++ = b; *pixbuf++ = a;
                    }
                    column += count - 1;
                }
                else
                {
                    if (count > width - column || (size_t)count > (ulen - pos) / bpp)
                    {
                        Mem_Free(out);
                        return NULL;
                    }
                    for (int k = 0; k < count; k++)
                    {
                        *pixbuf++ = buf[pos + 2];
                        *pixbuf++ = buf[pos + 1];
                        *pixbuf++ = buf[pos + 0];
                        *pixbuf++ = (bpp == 4u) ? buf[pos + 3] : 255;
                        pos += bpp;
                    }
                    column += count - 1;
                }
            }
            else
            {
                if (bpp > ulen - pos)
                {
                    Mem_Free(out);
                    return NULL;
                }
                *pixbuf++ = buf[pos + 2];
                *pixbuf++ = buf[pos + 1];
                *pixbuf++ = buf[pos + 0];
                *pixbuf++ = (bpp == 4u) ? buf[pos + 3] : 255;
                pos += bpp;
            }
        }
    }

    if (outW) *outW = width;
    if (outH) *outH = height;
    return out;
}

static void rt_mat_normalize_tex_path(const char *base, char *norm, size_t norm_size)
{
    const char *src = base;
    char       *dst = norm;

    while (*src == ' ' || *src == '\t')
        src++;
    if (src[0] == '.' && (src[1] == '/' || src[1] == '\\'))
        src += 2;
    while (*src == '/' || *src == '\\')
        src++;
    while (*src && dst < norm + norm_size - 1)
    {
        const char c = *src++;
        *dst++ = (c == '\\') ? '/' : c;
    }
    *dst = '\0';
}

static const char *rt_mat_tex_slot_path(const rt_material_t *mat, int which)
{
    switch (which)
    {
        case RT_MAT_TEX_BASE:     return mat->filename_base;
        case RT_MAT_TEX_NORMALS:  return mat->filename_normals;
        case RT_MAT_TEX_EMISSIVE: return mat->filename_emissive;
        case RT_MAT_TEX_OVERLAY:  return mat->filename_overlay;
        case RT_MAT_TEX_GLOSS:    return mat->filename_gloss;
        case RT_MAT_TEX_SSS:      return mat->filename_sss;
        case RT_MAT_TEX_HEIGHT:   return mat->filename_height;
        default:                  return NULL;
    }
}

byte *RT_MAT_LoadTexture(const rt_material_t *mat, int which, int *outWidth, int *outHeight)
{
    const char *base = rt_mat_tex_slot_path(mat, which);
    if (!base || !base[0])
    {
        return NULL;
    }

    /* The texture keys are paths under the game directories: accept Windows
       separators and a leading ./ or /, so a hand-edited yaml resolves like a
       path typed in the editor's field. */
    char norm[MAX_QPATH];
    rt_mat_normalize_tex_path(base, norm, sizeof(norm));
    base = norm;

    const char *dot = strrchr(base, '.');
    const qboolean hasExt = (dot != NULL && dot[1] != '\0');

    if (hasExt)
    {
        int len = 0;
        byte *file = rt_load_file(base, &len);
        if (!file)
        {
            if (outWidth) *outWidth = 0;
            if (outHeight) *outHeight = 0;
            return NULL;
        }
        byte *decoded = NULL;
        if (!q_strcasecmp(dot, ".tga"))
        {
            decoded = rt_decode_tga(file, len, outWidth, outHeight);
        }
        else
        {
            int w = 0, h = 0, c = 0;
            decoded = stbi_load_from_memory(file, len, &w, &h, &c, 4);
            if (decoded)
            {
                if (outWidth) *outWidth = w;
                if (outHeight) *outHeight = h;
            }
        }
        rt_load_file_free(file);
        if (!decoded)
        {
            if (outWidth) *outWidth = 0;
            if (outHeight) *outHeight = 0;
        }
        return decoded;
    }

    static const char *exts[] = { "tga", "jpg", "jpeg", "png" };
    for (int e = 0; e < (int)(sizeof(exts) / sizeof(exts[0])); e++)
    {
        char name[MAX_QPATH];
        q_snprintf(name, sizeof(name), "%s.%s", base, exts[e]);

        int len = 0;
        byte *file = rt_load_file(name, &len);
        if (!file)
        {
            continue;
        }

        if (e == 0)
        {
            byte *decoded = rt_decode_tga(file, len, outWidth, outHeight);
            rt_load_file_free(file);
            if (decoded)
            {
                return decoded;
            }
        }
        else
        {
            int w = 0, h = 0, comp = 0;
            byte *decoded = stbi_load_from_memory(file, len, &w, &h, &comp, 4);
            rt_load_file_free(file);
            if (decoded)
            {
                if (outWidth) *outWidth = w;
                if (outHeight) *outHeight = h;
                return decoded;
            }
        }
    }

    if (outWidth) *outWidth = 0;
    if (outHeight) *outHeight = 0;
    return NULL;
}

// ---------------------------------------------------------------------------
// .mat parser (ported from Q2RTX material.c load_material_file)
// ---------------------------------------------------------------------------

void RT_MAT_InitSubsurfaceDefaults(rt_material_t *mat)
{
    mat->sss_strength = 0.0f;
    mat->has_sss_strength = false;
    mat->sss_color[0] = 1.0f;
    mat->sss_color[1] = 0.35f;
    mat->sss_color[2] = 0.2f;
    mat->sss_radius = 0.02f;
    mat->sss_block_count = 0;
    mat->has_sss_blocks = false;
    mat->sss_threshold = 0.02f;
    mat->sss_feather = 0.0f;
    mat->sss_enabled = true;
    mat->emissive_enabled = true;
    mat->gloss_enabled = true;
    mat->parallax_scale = 0.0f;
    for (int i = 0; i < RT_MAT_MAX_SSS_BLOCKS; i++)
        mat->sss_blocks[i] = (rt_sss_block_t){0};
}

static void rt_mat_reset(rt_material_t *mat)
{
    memset(mat, 0, sizeof(*mat));
    mat->bump_scale = 1.0f;
    mat->metalness_factor = 0.0f;
    mat->emissive_factor = 1.0f;
    mat->emissive_blend = -1;
    mat->emissive_focus_soft = -1.0f;
    mat->base_factor = 1.0f;
    mat->transparency = 1.0f;
    mat->glass_ior = 0.0f;
    mat->glass_thickness = 2.0f;
    mat->glass_color[0] = 1.0f;
    mat->glass_color[1] = 1.0f;
    mat->glass_color[2] = 1.0f;
    mat->texture_scale_u = 1.0f;
    mat->texture_scale_v = 1.0f;
    mat->light_brightness = 1.0f;
    mat->light_shafts_power = 1.0f;
    mat->light_shafts_distance = 0.0f;
    mat->light_shafts_falloff = 1.0f;
    mat->light_styles = false;
    mat->color_emissive_threshold = 0.02f;
    RT_MAT_InitSubsurfaceDefaults(mat);
}

static qboolean rt_mat_parse_bool(const char *value)
{
    if (!q_strcasecmp(value, "true") || !q_strcasecmp(value, "yes") || !q_strcasecmp(value, "on"))
        return true;
    if (!q_strcasecmp(value, "false") || !q_strcasecmp(value, "no") || !q_strcasecmp(value, "off"))
        return false;
    return atoi(value) != 0;
}

static qboolean rt_mat_parse_hex_color(const char *value, vec3_t out)
{
    int i, c;

    if (!value || strlen(value) != 6)
        return false;

    for (i = 0; i < 3; i++)
    {
        int hi = value[i * 2];
        int lo = value[i * 2 + 1];

        if (hi >= '0' && hi <= '9')       hi -= '0';
        else if (hi >= 'a' && hi <= 'f')  hi = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F')  hi = hi - 'A' + 10;
        else return false;

        if (lo >= '0' && lo <= '9')       lo -= '0';
        else if (lo >= 'a' && lo <= 'f')  lo = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F')  lo = lo - 'A' + 10;
        else return false;

        c = (hi << 4) | lo;
        out[i] = (float)c / 255.0f;
    }
    return true;
}

/* Appends an empty block and returns it (NULL when the list is full): the
   mapping form of a color entry fills the fields itself. */
static rt_emissive_t *rt_mat_append_emissive_block(rt_material_t *mat)
{
    rt_emissive_t *block;

    if (mat->color_emissive_count >= RT_MAT_MAX_EMISSIVE_COLORS)
    {
        return NULL;
    }

    block = &mat->color_emissive[mat->color_emissive_count];
    memset(block, 0, sizeof(*block));
    block->color[0] = block->color[1] = block->color[2] = 1.0f;
    block->threshold = mat->color_emissive_threshold;
    block->feather = mat->color_emissive_feather;
    block->factor = mat->emissive_factor;
    block->blend = mat->emissive_blend;
    mat->color_emissive_count++;
    mat->has_color_emissive = true;
    return block;
}

// Appends one color to the material's color_emissive list ("ff0000", "#ff0000"
// and " ff0000 " all read the same). The list is what a texture atlas with
// several differently colored emissive regions is authored with. Returns the
// new block or NULL when the list is full or the color does not parse.
static rt_emissive_t *rt_mat_add_emissive_color(rt_material_t *mat, const char *value)
{
    vec3_t c;
    char   buf[64];
    size_t len;
    rt_emissive_t *block;

    if (!value)
    {
        return NULL;
    }

    while (*value == ' ' || *value == '\t')
    {
        value++;
    }
    if (*value == '#')
    {
        value++;
    }

    len = strlen(value);
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t' || value[len - 1] == '\r'))
    {
        len--;
    }
    if (len == 0 || len >= sizeof(buf))
    {
        return NULL;
    }

    memcpy(buf, value, len);
    buf[len] = 0;

    if (!rt_mat_parse_hex_color(buf, c))
    {
        Con_DWarning("RT mat: material '%s': luma '%s' is not rrggbb; ignored\n",
                     mat->name, buf);
        return NULL;
    }

    block = rt_mat_append_emissive_block(mat);
    if (block)
    {
        VectorCopy(c, block->color);
    }
    return block;
}

static rt_sss_block_t *rt_mat_append_sss_block(rt_material_t *mat)
{
    rt_sss_block_t *block;

    if (mat->sss_block_count >= RT_MAT_MAX_SSS_BLOCKS)
    {
        return NULL;
    }

    block = &mat->sss_blocks[mat->sss_block_count];
    *block = (rt_sss_block_t){0};
    block->color[0] = 1.0f;
    block->color[1] = 0.35f;
    block->color[2] = 0.2f;
    block->threshold = mat->sss_threshold;
    block->feather = mat->sss_feather;
    block->factor = 1.0f;
    mat->sss_block_count++;
    mat->has_sss_blocks = true;
    return block;
}

static rt_sss_block_t *rt_mat_add_sss_color(rt_material_t *mat, const char *value)
{
    vec3_t          c;
    char            buf[64];
    size_t          len;
    rt_sss_block_t *block;

    if (!value)
        return NULL;

    while (*value == ' ' || *value == '\t')
        value++;
    if (*value == '#')
        value++;

    len = strlen(value);
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t' || value[len - 1] == '\r'))
        len--;
    if (len == 0 || len >= sizeof(buf))
        return NULL;

    q_strlcpy(buf, value, len + 1);

    if (!rt_mat_parse_hex_color(buf, c))
    {
        Con_DWarning("RT mat: material '%s': sss '%s' is not rrggbb; ignored\n",
                     mat->name, buf);
        return NULL;
    }

    block = rt_mat_append_sss_block(mat);
    if (block)
        VectorCopy(c, block->color);
    return block;
}

static void rt_mat_add_sss_color_list(rt_material_t *mat, const char *value)
{
    char  buf[1024];
    char *p, *next;

    q_strlcpy(buf, value, sizeof(buf));
    for (p = buf; p && *p; p = next)
    {
        char *end;

        next = strchr(p, ',');
        if (next)
            *next++ = '\0';
        end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (*p)
            rt_mat_add_sss_color(mat, p);
    }
}

// The blend modes by name, so the file reads like what the shader does:
// "normal" is the coverage blend, "screen" adds the emission on top, "overlay"
// and "hard light" are the two halves of the overlay formula (the base and the
// emission as the driver), "color dodge" divides by the inverse.
const char *RT_MAT_EmissiveBlendName(int blend)
{
    switch (blend)
    {
    case -1: return "cvar";
    case 0:  return "off";
    case 1:  return "normal";
    case 2:  return "screen";
    case 3:  return "overlay";
    case 4:  return "hard light";
    case 5:  return "color dodge";
    default: return "cvar";
    }
}

// A blend mode by number or by any of its aliases; -2 when it is neither.
static int rt_mat_parse_emissive_blend(const char *value)
{
    char *end = NULL;
    long  v;

    if (!value)
        return -2;

    while (*value == ' ' || *value == '\t')
        value++;
    if (!*value)
        return -2;

    if (!q_strcasecmp(value, "cvar"))                                      return -1;
    if (!q_strcasecmp(value, "off") || !q_strcasecmp(value, "none"))       return 0;
    if (!q_strcasecmp(value, "normal") || !q_strcasecmp(value, "coverage")) return 1;
    if (!q_strcasecmp(value, "screen") || !q_strcasecmp(value, "add") ||
        !q_strcasecmp(value, "additive"))                                  return 2;
    if (!q_strcasecmp(value, "overlay"))                                   return 3;
    if (!q_strcasecmp(value, "hard light") || !q_strcasecmp(value, "hard_light") ||
        !q_strcasecmp(value, "hardlight"))                                 return 4;
    if (!q_strcasecmp(value, "color dodge") || !q_strcasecmp(value, "color_dodge") ||
        !q_strcasecmp(value, "colordodge") ||
        !q_strcasecmp(value, "dodge") || !q_strcasecmp(value, "divide"))   return 5;

    errno = 0;
    v = strtol(value, &end, 10);
    if (errno != 0 || end == value)
    {
        return -2;
    }
    while (*end == ' ' || *end == '\t')
    {
        end++;
    }
    if (*end != '\0' || v < -1 || v > RT_MAT_EMIS_BLEND_MAX)
    {
        return -2;
    }
    return (int)v;
}

static float rt_mat_clamp_unit_float(const char *mat_name, const char *key, const char *value)
{
    const float raw = (float)atof(value);
    float       v   = raw;

    if (!isfinite(v) || v < 0.0f)
        v = 0.0f;
    else if (v > 1.0f)
        v = 1.0f;

    if (!isfinite(raw) || v != raw)
        Con_DWarning("RT mat: material '%s': %s '%s' is not in 0..1; using %.3g\n", mat_name, key, value, v);
    return v;
}

static void rt_mat_set_pbr_or_warn(rt_material_t *mat, const char *key, const char *value)
{
    if (!q_strcasecmp(key, "texture_height"))
        q_strlcpy(mat->filename_height, value, sizeof(mat->filename_height));
    else if (!q_strcasecmp(key, "parallax_scale"))
    {
        const float raw = (float)atof(value);
        float       v   = (!isfinite(raw) || raw < 0.0f) ? 0.0f : raw;

        if (v > RT_MAT_PARALLAX_MAX)
            v = RT_MAT_PARALLAX_MAX;
        if (!isfinite(raw) || v != raw)
            Con_DWarning("RT mat: material '%s': parallax_scale '%s' is not in 0..0.25; using %.3g\n",
                         mat->name, value, v);
        mat->parallax_scale = v;
    }
    else if (!q_strcasecmp(key, "emissive_enabled"))
        mat->emissive_enabled = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "gloss_enabled"))
        mat->gloss_enabled = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "sss_enabled"))
        mat->sss_enabled = rt_mat_parse_bool(value);
    else
        Con_DWarning("RT mat: unknown attribute '%s' in material '%s'\n", key, mat->name);
}

static void rt_mat_set_sss_or_warn(rt_material_t *mat, const char *key, const char *value)
{
    if (!q_strcasecmp(key, "texture_sss"))
        q_strlcpy(mat->filename_sss, value, sizeof(mat->filename_sss));
    else if (!q_strcasecmp(key, "sss_strength"))
    {
        mat->sss_strength = rt_mat_clamp_unit_float(mat->name, "sss_strength", value);
        mat->has_sss_strength = true;
    }
    else if (!q_strcasecmp(key, "sss_radius"))
        mat->sss_radius = rt_mat_clamp_unit_float(mat->name, "sss_radius", value);
    else if (!q_strcasecmp(key, "sss_color"))
    {
        if (!rt_mat_parse_hex_color(value, mat->sss_color))
            Con_DWarning("RT mat: material '%s': sss_color '%s' is not rrggbb; ignored\n", mat->name, value);
    }
    else if (!q_strcasecmp(key, "sss_threshold"))
        mat->sss_threshold = CLAMP(0.0f, (float)atof(value), 1.0f);
    else if (!q_strcasecmp(key, "sss_feather"))
        mat->sss_feather = (float)(int)(CLAMP(0.0f, (float)atof(value), RT_MAT_FEATHER_MAX) + 0.5f);
    else if (!q_strcasecmp(key, "sss"))
        rt_mat_add_sss_color_list(mat, value);
    else
        rt_mat_set_pbr_or_warn(mat, key, value);
}

static void rt_mat_set_light_shaft_or_warn(rt_material_t *mat, const char *key, const char *value)
{
    if (!q_strcasecmp(key, "light_shafts"))
        mat->light_shafts = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "light_shafts_power"))
        mat->light_shafts_power = CLAMP(0.0f, (float)atof(value), 16.0f);
    else if (!q_strcasecmp(key, "light_shafts_distance"))
        mat->light_shafts_distance = CLAMP(0.0f, (float)atof(value), 25.0f);
    else if (!q_strcasecmp(key, "light_shafts_falloff"))
        mat->light_shafts_falloff = CLAMP(0.0f, (float)atof(value), 4.0f);
    else
        rt_mat_set_sss_or_warn(mat, key, value);
}

static void rt_mat_set_attribute(rt_material_t *mat, const char *key, const char *value)
{
    if (!q_strcasecmp(key, "bump_scale"))
        mat->bump_scale = (float)atof(value);
    else if (!q_strcasecmp(key, "roughness_override"))
    {
        mat->roughness_override = (float)atof(value);
        mat->has_roughness_override = true;
    }
    else if (!q_strcasecmp(key, "metalness_factor"))
    {
        mat->metalness_factor = (float)atof(value);
        mat->has_metalness_factor = true;
    }
    else if (!q_strcasecmp(key, "metalness_from_normal_alpha"))
    {
        mat->metalness_from_normal_alpha = rt_mat_parse_bool(value);
    }
    else if (!q_strcasecmp(key, "emissive_factor"))
        mat->emissive_factor = (float)atof(value);
    else if (!q_strcasecmp(key, "base_factor"))
        mat->base_factor = (float)atof(value);
    else if (!q_strcasecmp(key, "luma_blend") || !q_strcasecmp(key, "emissive_blend"))
    {
        const int v = rt_mat_parse_emissive_blend(value);

        if (v == -2)
        {
            Con_DWarning("RT mat: material '%s': luma_blend '%s' is not a mode; ignored\n",
                         mat->name, value);
        }
        else
        {
            mat->emissive_blend = v;
        }
    }
    else if (!q_strcasecmp(key, "light_focus") || !q_strcasecmp(key, "emissive_focus"))
    {
        const float v = (float)atof(value);

        if (v >= 0.0f && v <= 90.0f)
            mat->emissive_focus = v;
        else
            Con_DWarning("RT mat: material '%s': light_focus %.1f is not in 0..90 degrees; ignored\n",
                         mat->name, v);
    }
    else if (!q_strcasecmp(key, "light_focus_soft") || !q_strcasecmp(key, "emissive_focus_soft"))
    {
        const float v = (float)atof(value);

        if (v >= 0.0f && v <= 90.0f)
            mat->emissive_focus_soft = v;
        else
            Con_DWarning("RT mat: material '%s': light_focus_soft %.1f is not in 0..90 degrees; ignored\n",
                         mat->name, v);
    }
    else if (!q_strcasecmp(key, "light_projector") || !q_strcasecmp(key, "emissive_projector"))
        mat->emissive_projector = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "is_light"))
        mat->is_light = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "light_styles"))
        mat->light_styles = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "luma") || !q_strcasecmp(key, "color_emissive"))
    {
        // one or more colors, comma separated: "ff0000,00ff00"
        char  buf[1024];
        char *p, *next;

        q_strlcpy(buf, value, sizeof(buf));
        for (p = buf; p && *p; p = next)
        {
            char *end;

            next = strchr(p, ',');
            if (next)
            {
                *next++ = '\0';
            }
            end = p + strlen(p);
            while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
            {
                *--end = '\0';
            }
            if (*p)
            {
                rt_mat_add_emissive_color(mat, p);
            }
        }
    }
    else if (!q_strcasecmp(key, "luma_threshold") || !q_strcasecmp(key, "color_emissive_threshold"))
        mat->color_emissive_threshold = (float)atof(value);
    else if (!q_strcasecmp(key, "luma_feather") || !q_strcasecmp(key, "color_emissive_feather"))
    {
        float f = (float)atof(value);

        if (f < 0.0f)
        {
            f = 0.0f;
        }
        if (f > RT_MAT_FEATHER_MAX)
        {
            f = RT_MAT_FEATHER_MAX;
        }
        mat->color_emissive_feather = (float)(int)(CLAMP(0.0f, f, RT_MAT_FEATHER_MAX) + 0.5f);
    }
    else if (!q_strcasecmp(key, "light_color"))
        mat->has_light_color = rt_mat_parse_hex_color(value, mat->light_color);
    else if (!q_strcasecmp(key, "light_brightness"))
        mat->light_brightness = (float)atof(value);
    else if (!q_strcasecmp(key, "light_upoffset"))
        mat->light_upoffset = (float)atof(value);
    else if (!q_strcasecmp(key, "mirror"))
        mat->mirror = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "exact_normals"))
        mat->exact_normals = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "force_rasterize"))
        mat->force_rasterize = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "alpha_test"))
        mat->alpha_test = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "material_glass"))
        mat->material_glass = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "material_water"))
        mat->material_water = rt_mat_parse_bool(value);
    else if (!q_strcasecmp(key, "transparency"))
    {
        const float raw = (float)atof(value);
        float       v   = raw;

        if (isnan(v))
            v = 1.0f;
        else if (v < 0.0f)
            v = 0.0f;
        else if (v > 1.0f)
            v = 1.0f;

        if (v != raw)
            Con_DWarning("RT mat: material '%s': transparency '%s' is not in 0..1; using %.3g\n", mat->name, value, v);

        mat->transparency = v;
    }
    else if (!q_strcasecmp(key, "glass_ior") || !q_strcasecmp(key, "refractive_index"))
    {
        const float raw = (float)atof(value);
        float       v   = raw;

        if (isnan(v) || v < 0.0f)
            v = 0.0f;
        else if (v > 0.0f && v < 1.0f)
            v = 1.0f;
        else if (v > 5.0f)
            v = 5.0f;

        if (v != raw)
            Con_DWarning("RT mat: material '%s': glass_ior '%s' is not in 0..5; using %.3g\n", mat->name, value, v);

        mat->glass_ior = v;
    }
    else if (!q_strcasecmp(key, "glass_thickness") || !q_strcasecmp(key, "thickness"))
    {
        const float raw = (float)atof(value);
        float       v   = raw;

        if (isnan(v) || v < 0.0f)
            v = 0.0f;
        else if (v > 64.0f)
            v = 64.0f;

        if (v != raw)
            Con_DWarning("RT mat: material '%s': glass_thickness '%s' is not in 0..64; using %.3g\n", mat->name, value, v);

        mat->glass_thickness = v;
    }
    else if (!q_strcasecmp(key, "glass_color"))
    {
        if (!rt_mat_parse_hex_color(value, mat->glass_color))
            Con_DWarning("RT mat: material '%s': glass_color '%s' is not rrggbb; using white\n", mat->name, value);
    }
    else if (!q_strcasecmp(key, "texture_base"))
        q_strlcpy(mat->filename_base, value, sizeof(mat->filename_base));
    else if (!q_strcasecmp(key, "texture_normals"))
        q_strlcpy(mat->filename_normals, value, sizeof(mat->filename_normals));
    else if (!q_strcasecmp(key, "texture_emissive"))
        q_strlcpy(mat->filename_emissive, value, sizeof(mat->filename_emissive));
    else if (!q_strcasecmp(key, "texture_overlay"))
        q_strlcpy(mat->filename_overlay, value, sizeof(mat->filename_overlay));
    else if (!q_strcasecmp(key, "texture_gloss"))
        q_strlcpy(mat->filename_gloss, value, sizeof(mat->filename_gloss));
    else if (!q_strcasecmp(key, "texture_scale_u"))
    {
        float v = (float)atof(value);
        float raw = v;
        v = CLAMP(0.05f, v, 16.0f);
        if (v != raw)
            Con_DWarning("RT mat: material '%s': texture_scale_u '%s' is not in 0.05..16; using %.3g\n", mat->name, value, v);
        mat->texture_scale_u = v;
    }
    else if (!q_strcasecmp(key, "texture_scale_v"))
    {
        float v = (float)atof(value);
        float raw = v;
        v = CLAMP(0.05f, v, 16.0f);
        if (v != raw)
            Con_DWarning("RT mat: material '%s': texture_scale_v '%s' is not in 0.05..16; using %.3g\n", mat->name, value, v);
        mat->texture_scale_v = v;
    }
    else if (!q_strcasecmp(key, "texture_offset_u"))
    {
        float v = (float)atof(value);
        float raw = v;
        v = CLAMP(-8.0f, v, 8.0f);
        if (v != raw)
            Con_DWarning("RT mat: material '%s': texture_offset_u '%s' is not in -8..8; using %.3g\n", mat->name, value, v);
        mat->texture_offset_u = v;
    }
    else if (!q_strcasecmp(key, "texture_offset_v"))
    {
        float v = (float)atof(value);
        float raw = v;
        v = CLAMP(-8.0f, v, 8.0f);
        if (v != raw)
            Con_DWarning("RT mat: material '%s': texture_offset_v '%s' is not in -8..8; using %.3g\n", mat->name, value, v);
        mat->texture_offset_v = v;
    }
    else
        rt_mat_set_light_shaft_or_warn(mat, key, value);
}

static void rt_mat_yaml_scalar(const yaml_node_t *node, char *out, size_t outsize)
{
    if (!node || !node->data.scalar.value || outsize == 0)
    {
        if (outsize > 0)
            out[0] = 0;
        return;
    }

    size_t n = node->data.scalar.length;
    if (n >= outsize)
        n = outsize - 1;
    memcpy(out, node->data.scalar.value, n);
    out[n] = 0;
}

static int rt_mat_parse_polygon_uv(const char *s, float poly_uv[RT_MAT_EMIS_POLY_MAX][2])
{
    int count = 0;

    while (*s && count < RT_MAT_EMIS_POLY_MAX)
    {
        char *end;
        float u = (float)strtod(s, &end);
        float v;

        if (end == s)
            break;
        s = end;
        while (*s == ' ' || *s == '\t' || *s == ',')
            s++;
        v = (float)strtod(s, &end);
        if (end == s)
            break;
        s = end;
        poly_uv[count][0] = CLAMP(0.0f, u, 1.0f);
        poly_uv[count][1] = CLAMP(0.0f, v, 1.0f);
        count++;
        while (*s == ' ' || *s == '\t')
            s++;
    }
    return count >= 3 ? count : 0;
}

static void *rt_mat_append_emissive_block_void(rt_material_t *dest)
{
    return rt_mat_append_emissive_block(dest);
}

static void *rt_mat_append_sss_block_void(rt_material_t *dest)
{
    return rt_mat_append_sss_block(dest);
}

static void rt_mat_add_emissive_color_void(rt_material_t *dest, const char *value)
{
    (void)rt_mat_add_emissive_color(dest, value);
}

static void rt_mat_add_sss_color_void(rt_material_t *dest, const char *value)
{
    (void)rt_mat_add_sss_color(dest, value);
}

typedef struct
{
    void *(*append_block)(rt_material_t *dest);
    void (*parse_pair)(rt_material_t *dest, void *block, const char *kb, const char *vb);
    void (*add_scalar)(rt_material_t *dest, const char *valbuf);
} rt_mat_block_seq_ops_t;

static void rt_mat_parse_mapping_pairs(yaml_document_t *doc, yaml_node_t *sitem,
                                       rt_material_t *dest, void *block,
                                       const rt_mat_block_seq_ops_t *ops)
{
    for (yaml_node_pair_t *bp = sitem->data.mapping.pairs.start;
         bp < sitem->data.mapping.pairs.top; bp++)
    {
        yaml_node_t *bk = yaml_document_get_node(doc, bp->key);
        yaml_node_t *bv = yaml_document_get_node(doc, bp->value);
        char kb[64], vb[512];

        if (!bk || !bv || bk->type != YAML_SCALAR_NODE || bv->type != YAML_SCALAR_NODE)
            continue;
        rt_mat_yaml_scalar(bk, kb, sizeof(kb));
        rt_mat_yaml_scalar(bv, vb, sizeof(vb));
        ops->parse_pair(dest, block, kb, vb);
    }
}

static void rt_mat_parse_block_sequence(yaml_document_t *doc, yaml_node_t *mv,
                                        rt_material_t *dest,
                                        const rt_mat_block_seq_ops_t *ops)
{
    for (yaml_node_item_t *sit = mv->data.sequence.items.start;
         sit < mv->data.sequence.items.top; sit++)
    {
        yaml_node_t *sitem = yaml_document_get_node(doc, *sit);

        if (!sitem)
            continue;

        if (sitem->type == YAML_MAPPING_NODE)
        {
            void *block = ops->append_block(dest);
            if (block)
                rt_mat_parse_mapping_pairs(doc, sitem, dest, block, ops);
        }
        else if (sitem->type == YAML_SCALAR_NODE)
        {
            char valbuf[1024];

            rt_mat_yaml_scalar(sitem, valbuf, sizeof(valbuf));
            ops->add_scalar(dest, valbuf);
        }
    }
}

static void rt_mat_parse_emissive_block_pair(rt_material_t *dest, void *block_ptr,
                                             const char *kb, const char *vb)
{
    rt_emissive_t *block = (rt_emissive_t *)block_ptr;

    if (!q_strcasecmp(kb, "color"))
    {
        const char *hex = (vb[0] == '#') ? vb + 1 : vb;

        if (!rt_mat_parse_hex_color(hex, block->color))
            Con_DWarning("RT mat: material '%s': color '%s' is not rrggbb; white is used\n",
                         dest->name, vb);
    }
    else if (!q_strcasecmp(kb, "polygon"))
        block->poly_count = rt_mat_parse_polygon_uv(vb, block->poly_uv);
    else if (!q_strcasecmp(kb, "threshold"))
    {
        block->threshold = (float)atof(vb);
        block->has_threshold = true;
    }
    else if (!q_strcasecmp(kb, "feather"))
    {
        block->feather = CLAMP(0.0f, (float)(int)((float)atof(vb) + 0.5f), RT_MAT_FEATHER_MAX);
        block->has_feather = true;
    }
    else if (!q_strcasecmp(kb, "emissive_factor") || !q_strcasecmp(kb, "factor"))
    {
        block->factor = (float)atof(vb);
        block->has_factor = true;
    }
    else if (!q_strcasecmp(kb, "blend"))
    {
        const int v = rt_mat_parse_emissive_blend(vb);

        if (v == -2)
            Con_DWarning("RT mat: material '%s': blend '%s' is not a mode; the material's is used\n",
                         dest->name, vb);
        else
        {
            block->blend = v;
            block->has_blend = true;
        }
    }
}

static void rt_mat_parse_sss_block_pair(rt_material_t *dest, void *block_ptr,
                                        const char *kb, const char *vb)
{
    rt_sss_block_t *block = (rt_sss_block_t *)block_ptr;

    if (!q_strcasecmp(kb, "color"))
    {
        const char *hex = (vb[0] == '#') ? vb + 1 : vb;

        if (!rt_mat_parse_hex_color(hex, block->color))
            Con_DWarning("RT mat: material '%s': sss color '%s' is not rrggbb\n", dest->name, vb);
    }
    else if (!q_strcasecmp(kb, "polygon"))
        block->poly_count = rt_mat_parse_polygon_uv(vb, block->poly_uv);
    else if (!q_strcasecmp(kb, "threshold"))
    {
        block->threshold = CLAMP(0.0f, (float)atof(vb), 1.0f);
        block->has_threshold = true;
    }
    else if (!q_strcasecmp(kb, "feather"))
    {
        block->feather = CLAMP(0.0f, (float)(int)((float)atof(vb) + 0.5f), RT_MAT_FEATHER_MAX);
        block->has_feather = true;
    }
    else if (!q_strcasecmp(kb, "factor"))
    {
        block->factor = CLAMP(0.0f, (float)atof(vb), 2.0f);
        block->has_factor = true;
    }
}

static void rt_mat_parse_emissive_sequence(yaml_document_t *doc, yaml_node_t *mv, rt_material_t *dest)
{
    static const rt_mat_block_seq_ops_t ops = {
        rt_mat_append_emissive_block_void,
        rt_mat_parse_emissive_block_pair,
        rt_mat_add_emissive_color_void,
    };
    rt_mat_parse_block_sequence(doc, mv, dest, &ops);
}

static void rt_mat_parse_sss_sequence(yaml_document_t *doc, yaml_node_t *mv, rt_material_t *dest)
{
    static const rt_mat_block_seq_ops_t ops = {
        rt_mat_append_sss_block_void,
        rt_mat_parse_sss_block_pair,
        rt_mat_add_sss_color_void,
    };
    rt_mat_parse_block_sequence(doc, mv, dest, &ops);
}

static void rt_mat_finalize_blocks(rt_material_t *dest)
{
    for (int c = 0; c < dest->color_emissive_count; c++)
    {
        rt_emissive_t *block = &dest->color_emissive[c];

        if (!block->has_threshold)
            block->threshold = dest->color_emissive_threshold;
        if (!block->has_feather)
            block->feather = dest->color_emissive_feather;
        if (!block->has_factor)
            block->factor = dest->emissive_factor;
        if (!block->has_blend)
            block->blend = dest->emissive_blend;
        block->has_threshold = block->has_feather = true;
        block->has_factor = block->has_blend = true;
    }

    for (int c = 0; c < dest->sss_block_count; c++)
    {
        rt_sss_block_t *block = &dest->sss_blocks[c];

        if (!block->has_threshold)
            block->threshold = dest->sss_threshold;
        if (!block->has_feather)
            block->feather = dest->sss_feather;
        if (!block->has_factor)
            block->factor = 1.0f;
        block->has_threshold = block->has_feather = block->has_factor = true;
    }

    if (!dest->has_sss_strength &&
        (dest->filename_sss[0] != '\0' || (dest->has_sss_blocks && dest->sss_block_count > 0)))
    {
        dest->sss_strength = 1.0f;
    }
}

static int rt_mat_parse_yaml(const char *filebuf, int len, const char *file_name, rt_material_t *dest, int max_items)
{
    if (!filebuf || len == 0)
    {
        return 0;
    }

    yaml_parser_t parser;
    yaml_document_t document;
    int count = 0;

    if (!yaml_parser_initialize(&parser))
    {
        return 0;
    }

    yaml_parser_set_input_string(&parser, (const unsigned char *)filebuf, (size_t)len);

    if (yaml_parser_load(&parser, &document))
    {
        yaml_node_t *root = yaml_document_get_root_node(&document);
        if (root && root->type == YAML_MAPPING_NODE)
        {
            for (yaml_node_pair_t *rp = root->data.mapping.pairs.start;
                 rp < root->data.mapping.pairs.top; rp++)
            {
                yaml_node_t *rk = yaml_document_get_node(&document, rp->key);
                if (!rk || rk->type != YAML_SCALAR_NODE || !rk->data.scalar.value)
                    continue;
                if (!((rk->data.scalar.length == 14 && !memcmp(rk->data.scalar.value, "qray_materials", 14)) ||
                      (rk->data.scalar.length == 9 && !memcmp(rk->data.scalar.value, "materials", 9))))
                    continue;

                yaml_node_t *seq = yaml_document_get_node(&document, rp->value);
                if (!seq || seq->type != YAML_SEQUENCE_NODE)
                    continue;

                for (yaml_node_item_t *it = seq->data.sequence.items.start;
                     it < seq->data.sequence.items.top && count < max_items; it++)
                {
                    yaml_node_t *item = yaml_document_get_node(&document, *it);
                    if (!item || item->type != YAML_MAPPING_NODE)
                        continue;

                    rt_mat_reset(dest);
                    dest->valid = true;

                    int have_name = 0;
                    for (yaml_node_pair_t *mp = item->data.mapping.pairs.start;
                         mp < item->data.mapping.pairs.top; mp++)
                    {
                        yaml_node_t *mk = yaml_document_get_node(&document, mp->key);
                        yaml_node_t *mv = yaml_document_get_node(&document, mp->value);
                        if (!mk || !mv || mk->type != YAML_SCALAR_NODE)
                            continue;

                        char keybuf[128];
                        rt_mat_yaml_scalar(mk, keybuf, sizeof(keybuf));

                        if (mv->type == YAML_SEQUENCE_NODE)
                        {
                            if (!q_strcasecmp(keybuf, "luma") || !q_strcasecmp(keybuf, "color_emissive"))
                                rt_mat_parse_emissive_sequence(&document, mv, dest);
                            else if (!q_strcasecmp(keybuf, "sss"))
                                rt_mat_parse_sss_sequence(&document, mv, dest);
                            continue;
                        }
                        if (mv->type != YAML_SCALAR_NODE)
                            continue;

                        char valbuf[1024];
                        rt_mat_yaml_scalar(mv, valbuf, sizeof(valbuf));

                        if (!q_strcasecmp(keybuf, "name"))
                        {
                            q_strlcpy(dest->name, valbuf, sizeof(dest->name));
                            q_strlwr(dest->name);
                            have_name = 1;
                        }
                        else
                        {
                            rt_mat_set_attribute(dest, keybuf, valbuf);
                        }
                    }

                    rt_mat_finalize_blocks(dest);

                    if (have_name)
                    {
                        q_strlcpy(dest->source_file, file_name, sizeof(dest->source_file));
                        dest++;
                        count++;
                    }
                }
            }
        }
        yaml_document_delete(&document);
    }
    else
    {
        Con_DWarning("RT mat: failed to parse YAML '%s': %s\n",
                     file_name, parser.problem ? parser.problem : "unknown error");
    }

    yaml_parser_delete(&parser);
    return count;
}

static qboolean rt_mat_is_editor_temp_name(const char *path)
{
    const char *base;
    const char *slash;
    const char *bslash;

    if (!path)
        return false;

    slash = strrchr(path, '/');
    bslash = strrchr(path, '\\');
    base = path;
    if (slash && slash + 1 > base)
        base = slash + 1;
    if (bslash && bslash + 1 > base)
        base = bslash + 1;

    return (!q_strcasecmp(base, "materials.editor.yaml") ||
            !q_strcasecmp(base, "backup_materials.yaml") ||
            !q_strcasecmp(base, "qray.materials.editor.yaml") ||
            !q_strcasecmp(base, "qray.backup_materials.yaml")) ? true : false;
}

static int rt_mat_load_abs_file(const char *path, rt_material_t *dest, int max_items)
{
    FILE  *f = fopen(path, "rb");
    long   len;
    char  *buf;
    size_t got;
    int    count;

    if (!f)
        return 0;

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 64 * 1024 * 1024)
    {
        fclose(f);
        return 0;
    }

    buf = (char *)Mem_Alloc((size_t)len + 1);
    got = fread(buf, 1, (size_t)len, f);
    fclose(f);

    count = rt_mat_parse_yaml(buf, (got > (size_t)len) ? (int)len : (int)got, path, dest, max_items);
    Mem_Free(buf);
    return count;
}

static void rt_mat_load_dir(const char *dir, int (*cb)(const char *name, void *ctx), void *ctx)
{
	char pattern[MAX_OSPATH];

#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h;

	q_snprintf(pattern, sizeof(pattern), "%s/materials/*.yaml", dir);
	h = FindFirstFileA(pattern, &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			char path[MAX_OSPATH];

			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				continue;
			}
			// the editor's own files: the session file is not a materials file
			// until it is saved, and the backup never is
			if (rt_mat_is_editor_temp_name(fd.cFileName))
			{
				continue;
			}
			q_snprintf(path, sizeof(path), "%s/materials/%s", dir, fd.cFileName);
			if (cb(path, ctx))
			{
				break;
			}
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
#else
	DIR *d;
	struct dirent *e;

	q_snprintf(pattern, sizeof(pattern), "%s/materials", dir);
	d = opendir(pattern);
	if (d)
	{
		while ((e = readdir(d)) != NULL)
		{
			char path[MAX_OSPATH];
			size_t n = strlen(e->d_name);

			if (n < 5 || q_strcasecmp(e->d_name + n - 5, ".yaml") != 0)
			{
				continue;
			}
			if (rt_mat_is_editor_temp_name(e->d_name))
			{
				continue;
			}
			q_snprintf(path, sizeof(path), "%s/materials/%s", dir, e->d_name);
			if (cb(path, ctx))
			{
				break;
			}
		}
		closedir(d);
	}
#endif

	q_snprintf(pattern, sizeof(pattern), "%s/materials.yaml", dir);
	if (Sys_FileTime(pattern) != -1)
	{
		cb(pattern, ctx);
	}

	q_snprintf(pattern, sizeof(pattern), "%s/qray.materials.yaml", dir);
	if (Sys_FileTime(pattern) != -1)
	{
		cb(pattern, ctx);
	}
}

typedef struct {
	rt_material_t *dest;
	int *count;
	int max;
} rt_mat_load_ctx_t;

static void rt_mat_merge_loaded(rt_mat_load_ctx_t *ctx, int before, int loaded, const char *label)
{
    int n, kept = 0;

    for (n = 0; n < loaded; n++)
    {
        rt_material_t *loaded_mat = ctx->dest + before + n;
        int            old = -1, i;

        for (i = 0; i < before + kept; i++)
        {
            if (!q_strcasecmp(ctx->dest[i].name, loaded_mat->name))
            {
                old = i;
                break;
            }
        }

        if (old >= 0)
        {
            ctx->dest[old] = *loaded_mat;
        }
        else
        {
            if (kept != n)
            {
                ctx->dest[before + kept] = *loaded_mat;
            }
            kept++;
        }
    }
    *ctx->count = before + kept;

    if (loaded > 0)
    {
        Con_Printf("RT: loaded %d materials from %s\n", loaded, label);
    }
}

static int rt_mat_load_buffer(const char *buf, int len, const char *label, rt_mat_load_ctx_t *ctx)
{
    const int before = *ctx->count;
    int       loaded;

    if (before >= ctx->max)
    {
        return 1;
    }
    if (!buf || len <= 0)
    {
        return 0;
    }

    loaded = rt_mat_parse_yaml(buf, len, label, ctx->dest + before, ctx->max - before);
    rt_mat_merge_loaded(ctx, before, loaded, label);
    return (*ctx->count >= ctx->max) ? 1 : 0;
}

static int rt_mat_load_cb(const char *name, void *vctx)
{
    rt_mat_load_ctx_t *ctx = (rt_mat_load_ctx_t *)vctx;
    const int          before = *ctx->count;
    int                loaded;

    if (before >= ctx->max)
    {
        return 1;
    }

    loaded = rt_mat_load_abs_file(name, ctx->dest + before, ctx->max - before);
    rt_mat_merge_loaded(ctx, before, loaded, name);
    return 0;
}

// Collects com_searchpaths into lowest-to-highest precedence order: tail-to-head
// across roots/mods, with each directory's pak/pkz archives ordered before its
// loose directory so loose overrides win over shipped archives in the same root.
static searchpath_t **rt_mat_build_searchpath_order(int *outCount)
{
    searchpath_t  *s;
    searchpath_t **rev;
    searchpath_t **prec;
    int            total = 0;
    int            idx = 0;
    int            i = 0;
    int            out = 0;

    for (s = com_searchpaths; s; s = s->next)
    {
        total++;
    }
    if (total == 0)
    {
        *outCount = 0;
        return NULL;
    }

    rev = (searchpath_t **)Mem_Alloc((size_t)total * sizeof(*rev));
    prec = (searchpath_t **)Mem_Alloc((size_t)total * sizeof(*prec));

    for (s = com_searchpaths; s; s = s->next)
    {
        rev[total - 1 - idx++] = s;
    }

    while (i < total)
    {
        if (!rev[i]->pack && !rev[i]->rt_pkz)
        {
            int j = i + 1;
            int k;

            while (j < total && (rev[j]->pack || rev[j]->rt_pkz))
            {
                j++;
            }
            for (k = i + 1; k < j; k++)
            {
                prec[out++] = rev[k];
            }
            prec[out++] = rev[i];
            i = j;
        }
        else
        {
            prec[out++] = rev[i++];
        }
    }

    Mem_Free(rev);
    *outCount = out;
    return prec;
}

static char *rt_mat_read_pack_file(const pack_t *pak, const char *filename, int *outLen)
{
    int i;

    if (!pak || !filename || !filename[0])
    {
        return NULL;
    }

    for (i = 0; i < pak->numfiles; i++)
    {
        FILE  *f;
        char  *buf;
        size_t got;
        int    filelen;

        if (q_strcasecmp(pak->files[i].name, filename) != 0)
        {
            continue;
        }
        filelen = pak->files[i].filelen;
        if (filelen <= 0 || filelen > 64 * 1024 * 1024)
        {
            return NULL;
        }
        f = fopen(pak->filename, "rb");
        if (!f)
        {
            return NULL;
        }
        if (fseek(f, pak->files[i].filepos, SEEK_SET) != 0)
        {
            fclose(f);
            return NULL;
        }
        buf = (char *)Mem_Alloc((size_t)filelen + 1);
        got = fread(buf, 1, (size_t)filelen, f);
        fclose(f);
        if (outLen)
        {
            *outLen = (got > (size_t)filelen) ? filelen : (int)got;
        }
        return buf;
    }

    return NULL;
}

typedef struct {
    const void        *archive;
    rt_mat_load_ctx_t *mat_ctx;
} rt_mat_pkz_ctx_t;

static int rt_mat_load_pkz_file(const void *archive, const char *filename, rt_mat_load_ctx_t *ctx)
{
    int   len = 0;
    byte *buf;
    int   stop;

    if (*ctx->count >= ctx->max)
    {
        return 1;
    }

    buf = RT_PKZ_LoadFile(archive, filename, &len);
    if (!buf)
    {
        return 0;
    }

    stop = rt_mat_load_buffer((const char *)buf, len, filename, ctx);
    Mem_Free(buf);
    return stop;
}

static int rt_mat_load_pkz_cb(const char *name, void *vctx)
{
    rt_mat_pkz_ctx_t *pctx = (rt_mat_pkz_ctx_t *)vctx;

    if (rt_mat_is_editor_temp_name(name))
    {
        return 0;
    }
    return rt_mat_load_pkz_file(pctx->archive, name, pctx->mat_ctx);
}

static void rt_mat_load_pkz(const void *archive, rt_mat_load_ctx_t *ctx)
{
    rt_mat_pkz_ctx_t pctx = { archive, ctx };

    RT_PKZ_ListArchiveFiles(archive, "materials/", ".yaml", rt_mat_load_pkz_cb, &pctx);
    rt_mat_load_pkz_file(archive, "materials.yaml", ctx);
    rt_mat_load_pkz_file(archive, "qray.materials.yaml", ctx);
}

static void rt_mat_load_pack_file(const pack_t *pak, const char *filename, rt_mat_load_ctx_t *ctx)
{
    int   len = 0;
    char *buf;

    if (*ctx->count >= ctx->max)
    {
        return;
    }

    buf = rt_mat_read_pack_file(pak, filename, &len);
    if (!buf)
    {
        return;
    }

    rt_mat_load_buffer(buf, len, filename, ctx);
    Mem_Free(buf);
}

static void rt_mat_load_pack(const pack_t *pak, rt_mat_load_ctx_t *ctx)
{
    int i;

    if (!pak)
    {
        return;
    }

    for (i = 0; i < pak->numfiles && *ctx->count < ctx->max; i++)
    {
        const char *name = pak->files[i].name;
        size_t      len = strlen(name);

        if (len < 15 || q_strncasecmp(name, "materials/", 10) != 0 ||
            q_strcasecmp(name + len - 5, ".yaml") != 0)
        {
            continue;
        }
        if (rt_mat_is_editor_temp_name(name))
        {
            continue;
        }
        rt_mat_load_pack_file(pak, name, ctx);
    }

    rt_mat_load_pack_file(pak, "materials.yaml", ctx);
    rt_mat_load_pack_file(pak, "qray.materials.yaml", ctx);
}

static void rt_mat_load_global(rt_mat_load_ctx_t *ctx)
{
    searchpath_t **prec;
    int            count = 0;
    int            i;

    prec = rt_mat_build_searchpath_order(&count);
    for (i = 0; i < count && *ctx->count < ctx->max; i++)
    {
        searchpath_t *s = prec[i];

        if (s->rt_pkz)
        {
            rt_mat_load_pkz(s->rt_pkz, ctx);
        }
        else if (s->pack)
        {
            rt_mat_load_pack(s->pack, ctx);
        }
        else if (s->filename[0] && (!com_gamedir[0] || !COM_PathMatches(s->filename, com_gamedir)))
        {
            rt_mat_load_dir(s->filename, rt_mat_load_cb, ctx);
        }
    }
    if (prec)
    {
        Mem_Free(prec);
    }

    if (com_gamedir[0])
    {
        rt_mat_load_dir(com_gamedir, rt_mat_load_cb, ctx);
    }
}

void RT_MAT_Init(void)
{
    if (rt_initialized)
    {
        return;
    }
    rt_initialized = true;

    Cvar_RegisterVariable(&rt_materials);
    Cvar_RegisterVariable(&rt_mat_debug);

    rt_global_count = 0;
    rt_map_count = 0;

    rt_mat_load_ctx_t ctx = { rt_global_materials, &rt_global_count, RT_MAT_MAX_GLOBAL };
    rt_mat_load_global(&ctx);

    rt_mat_cmd = Cmd_AddCommand2("rt_mat", RT_MAT_Cmd, src_command);
}

void RT_MAT_Shutdown(void)
{
    if (!rt_initialized)
    {
        return;
    }
    rt_initialized = false;

    if (rt_mat_cmd)
    {
        Cmd_RemoveCommand(rt_mat_cmd);
        rt_mat_cmd = NULL;
    }
    rt_global_count = 0;
    rt_map_count = 0;
    RT_PKZ_Shutdown();
}

static void rt_mat_apply_gamedir_overrides(const char *path)
{
    rt_material_t *tmp;
    int            loaded;
    int            n, i;

    if (Sys_FileTime(path) == -1)
    {
        return;
    }

    tmp = (rt_material_t *)Mem_Alloc((size_t)RT_MAT_MAX_GLOBAL * sizeof(*tmp));
    loaded = rt_mat_load_abs_file(path, tmp, RT_MAT_MAX_GLOBAL);
    if (loaded <= 0)
    {
        Mem_Free(tmp);
        return;
    }

    // Refresh rt_global_materials with the latest saved com_gamedir overrides
    // across map changes, and patch matching names already present in
    // rt_map_materials so user overrides win over materials/<map>.yaml without
    // duplicating unrelated global materials into rt_map_materials.
    for (n = 0; n < loaded; n++)
    {
        const rt_material_t *m = &tmp[n];
        int                  old = -1;

        for (i = 0; i < rt_global_count; i++)
        {
            if (!q_strcasecmp(rt_global_materials[i].name, m->name))
            {
                old = i;
                break;
            }
        }
        if (old >= 0)
        {
            rt_global_materials[old] = *m;
        }
        else if (rt_global_count < RT_MAT_MAX_GLOBAL)
        {
            rt_global_materials[rt_global_count++] = *m;
        }

        for (i = 0; i < rt_map_count; i++)
        {
            if (!q_strcasecmp(rt_map_materials[i].name, m->name))
            {
                rt_map_materials[i] = *m;
                break;
            }
        }
    }

    Con_Printf("RT: loaded %d materials from %s\n", loaded, path);
    Mem_Free(tmp);
}

void RT_MAT_ChangeMap(const char *mapname)
{
    rt_mat_load_ctx_t ctx = { rt_map_materials, &rt_map_count, RT_MAT_MAX_MAP };

    if (!rt_initialized)
    {
        return;
    }

    q_strlcpy(rt_current_map, mapname ? mapname : "", sizeof(rt_current_map));
    rt_map_count = 0;

    if (mapname && mapname[0])
    {
        char           rel[MAX_QPATH];
        searchpath_t **prec;
        int            count = 0;
        int            i;

        q_snprintf(rel, sizeof(rel), "materials/%s.yaml", mapname);

        prec = rt_mat_build_searchpath_order(&count);
        for (i = 0; i < count && *ctx.count < ctx.max; i++)
        {
            searchpath_t *s = prec[i];

            if (s->rt_pkz)
            {
                rt_mat_load_pkz_file(s->rt_pkz, rel, &ctx);
            }
            else if (s->pack)
            {
                rt_mat_load_pack_file(s->pack, rel, &ctx);
            }
            else if (s->filename[0] && (!com_gamedir[0] || !COM_PathMatches(s->filename, com_gamedir)))
            {
                char path[MAX_OSPATH];
                q_snprintf(path, sizeof(path), "%s/%s", s->filename, rel);
                if (Sys_FileTime(path) != -1)
                {
                    rt_mat_load_cb(path, &ctx);
                }
            }
        }
        if (prec)
        {
            Mem_Free(prec);
        }

        if (com_gamedir[0])
        {
            char ownmap[MAX_OSPATH];
            q_snprintf(ownmap, sizeof(ownmap), "%s/%s", com_gamedir, rel);
            if (Sys_FileTime(ownmap) != -1)
            {
                rt_mat_load_cb(ownmap, &ctx);
            }
        }
    }

    if (com_gamedir[0])
    {
        char own[MAX_OSPATH];
        q_snprintf(own, sizeof(own), "%s/materials.yaml", com_gamedir);
        rt_mat_apply_gamedir_overrides(own);
        q_snprintf(own, sizeof(own), "%s/qray.materials.yaml", com_gamedir);
        rt_mat_apply_gamedir_overrides(own);
    }
}

void RT_MAT_Reload(void)
{
    if (!rt_initialized)
    {
        return;
    }

    rt_global_count = 0;
    rt_map_count = 0;

    rt_mat_load_ctx_t ctx = { rt_global_materials, &rt_global_count, RT_MAT_MAX_GLOBAL };
    rt_mat_load_global(&ctx);

    if (rt_current_map[0])
    {
        RT_MAT_ChangeMap(rt_current_map);
    }
}

static void rt_mat_normalize_name(const char *name, char *out, size_t outsize)
{
    q_strlcpy(out, name, outsize);

    char *bsp = strstr(out, ".bsp");
    if (bsp)
    {
        char *colon = strchr(bsp, ':');
        if (colon)
        {
            char texname[MAX_QPATH];
            q_strlcpy(texname, colon + 1, sizeof(texname));
            if (texname[0] == '*')
            {
                texname[0] = '#';
            }
            q_snprintf(out, outsize, "textures/%s", texname);
        }
    }

    q_strlwr(out);
}

void RT_MAT_NormalizeName(const char *name, char *out, size_t outsize)
{
    rt_mat_normalize_name(name, out, outsize);
}

// "textures/+3_med25" -> 3, "progs/flame.mdl:frame2" -> 2 (a model or sprite
// skin frame), anything else -> -1
int RT_MAT_FrameDigit(const char *name)
{
    if (!q_strncasecmp(name, "textures/+", 10) && name[10] >= '0' && name[10] <= '9')
    {
        int         value = 0;
        const char *p = name + 10;

        while (*p >= '0' && *p <= '9' && value <= 9999)
        {
            value = value * 10 + (*p - '0');
            p++;
        }
        return value;
    }

    {
        const char *p = strstr(name, ":frame");

        if (p && p[6] >= '0' && p[6] <= '9')
        {
            int value = 0;

            p += 6;
            while (*p >= '0' && *p <= '9' && value <= 9999)
            {
                value = value * 10 + (*p - '0');
                p++;
            }
            if (*p == '\0' || *p == '_')
            {
                return value;
            }
        }
    }
    return -1;
}

void RT_MAT_GroupBaseOf(const char *name, char *out, size_t outsize)
{
    if (!q_strncasecmp(name, "textures/+", 10) && name[10] >= '0' && name[10] <= '9')
    {
        const char *p = name + 10;

        while (*p >= '0' && *p <= '9')
        {
            p++;
        }
        q_snprintf(out, outsize, "textures/%s", p);
        return;
    }

    {
        const char *p = strstr(name, ":frame");

        if (p && p[6] >= '0' && p[6] <= '9')
        {
            const char *q = p + 6;

            while (*q >= '0' && *q <= '9')
            {
                q++;
            }
            if (*q == '\0' || *q == '_')
            {
                size_t n = (size_t)(p - name);

                if (n >= outsize)
                {
                    n = outsize - 1;
                }
                memcpy(out, name, n);
                out[n] = '\0';
                return;
            }
        }
    }

    q_strlcpy(out, name, outsize);
}

static rt_material_t *rt_mat_find_in(const char *name, rt_material_t *first, int count)
{
    char n[MAX_QPATH];
    rt_mat_normalize_name(name, n, sizeof(n));
    char *dot = strrchr(n, '.');
    if (dot && !strchr(dot, ':'))
    {
        *dot = 0;
    }

    for (int i = 0; i < count; i++)
    {
        if (first[i].valid)
        {
            if (!strcmp(first[i].name, n))
            {
                if (CVAR_TO_BOOL (rt_mat_debug))
                {
                    Con_Printf("RT: material lookup '%s' -> '%s' FOUND (emissive=%s gloss=%s)\n",
                               name, n,
                               first[i].filename_emissive[0] ? first[i].filename_emissive : "-",
                               first[i].filename_gloss[0] ? first[i].filename_gloss : "-");
                }
                return &first[i];
            }
        }
    }
    if (CVAR_TO_BOOL (rt_mat_debug))
    {
        Con_Printf("RT: material lookup '%s' -> '%s' NOT FOUND\n", name, n);
    }
    return NULL;
}

rt_material_t *RT_MAT_Find(const char *name)
{
    if (!rt_initialized || !RT_MAT_Enabled())
    {
        return NULL;
    }
    if (!name || !name[0])
    {
        return NULL;
    }

    rt_material_t *m = rt_mat_find_in(name, rt_map_materials, rt_map_count);
    if (m)
    {
        return m;
    }
    return rt_mat_find_in(name, rt_global_materials, rt_global_count);
}

rt_material_t *RT_MAT_GetList(int which, int *outCount)
{
    if (which == RT_MAT_LIST_MAP)
    {
        if (outCount)
            *outCount = rt_map_count;
        return rt_map_materials;
    }

    if (outCount)
        *outCount = rt_global_count;
    return rt_global_materials;
}

int RT_MAT_AppendGlobal(const rt_material_t *mat)
{
    if (!rt_initialized || rt_global_count >= RT_MAT_MAX_GLOBAL)
        return -1;

    rt_global_materials[rt_global_count] = *mat;
    return rt_global_count++;
}

void RT_MAT_SetListCounts(int globalCount, int mapCount)
{
    if (globalCount < 0)
        globalCount = 0;
    if (mapCount < 0)
        mapCount = 0;
    if (globalCount > RT_MAT_MAX_GLOBAL)
        globalCount = RT_MAT_MAX_GLOBAL;
    if (mapCount > RT_MAT_MAX_MAP)
        mapCount = RT_MAT_MAX_MAP;

    /* dropped entries must not stay findable through a stale valid flag */
    for (int i = globalCount; i < rt_global_count && i < RT_MAT_MAX_GLOBAL; i++)
        rt_global_materials[i].valid = false;
    for (int i = mapCount; i < rt_map_count && i < RT_MAT_MAX_MAP; i++)
        rt_map_materials[i].valid = false;

    rt_global_count = globalCount;
    rt_map_count = mapCount;
}

const char *RT_MAT_CurrentMap(void)
{
    return rt_current_map;
}

qboolean RT_MAT_Enabled(void)
{
    return rt_materials.value != 0;
}

void RT_MAT_Cmd(void)
{
    if (Cmd_Argc() == 1)
    {
        Con_Printf("rt_materials is %s; %d global, %d map materials\n",
                   RT_MAT_Enabled() ? "ON" : "OFF", rt_global_count, rt_map_count);
        return;
    }

    rt_material_t *m = RT_MAT_Find(Cmd_Argv(1));
    if (!m)
    {
        Con_Printf("no material for '%s'\n", Cmd_Argv(1));
        return;
    }

    Con_Printf("material '%s': base=%s normals=%s emissive=%s gloss=%s "
               "bump=%.2f rough=%.2f metal=%.2f emiss=%.2f base=%.2f emis_blend=%d "
               "focus=%.1f soft=%.1f projector=%d\n",
               m->name,
               m->filename_base[0] ? m->filename_base : "-",
               m->filename_normals[0] ? m->filename_normals : "-",
               m->filename_emissive[0] ? m->filename_emissive : "-",
               m->filename_gloss[0] ? m->filename_gloss : "-",
               m->bump_scale, m->roughness_override,
               m->metalness_factor, m->emissive_factor, m->base_factor,
               m->emissive_blend,
               m->emissive_focus, m->emissive_focus_soft,
               m->emissive_projector ? 1 : 0);
}
