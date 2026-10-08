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
