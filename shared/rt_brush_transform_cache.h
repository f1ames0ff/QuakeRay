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

#pragma once

#include <string.h>

typedef struct
{
    const void *entity;
    float origin[3];
    float angles[3];
    float matrix[12];
    int valid;
} rt_brush_transform_cache_t;

static inline int RT_BrushTransformCacheGet(const rt_brush_transform_cache_t *cache, const void *entity,
                                          const float origin[3], const float angles[3], void *matrix)
{
    if (!cache->valid || cache->entity != entity ||
        memcmp(cache->origin, origin, sizeof(cache->origin)) != 0 ||
        memcmp(cache->angles, angles, sizeof(cache->angles)) != 0)
        return 0;
    memcpy(matrix, cache->matrix, sizeof(cache->matrix));
    return 1;
}

static inline void RT_BrushTransformCacheStore(rt_brush_transform_cache_t *cache, const void *entity,
                                             const float origin[3], const float angles[3], const void *matrix)
{
    cache->entity = entity;
    memcpy(cache->origin, origin, sizeof(cache->origin));
    memcpy(cache->angles, angles, sizeof(cache->angles));
    memcpy(cache->matrix, matrix, sizeof(cache->matrix));
    cache->valid = 1;
}
