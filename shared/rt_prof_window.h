#ifndef RT_PROF_WINDOW_H
#define RT_PROF_WINDOW_H

#include <assert.h>

typedef struct
{
    unsigned pending;
    unsigned frames;
    unsigned publications;
    int active;
    int frame_open;
    double started;
} rt_prof_window_t;

typedef struct
{
    void *context;
    void (*lock)(void *context);
    void (*unlock)(void *context);
} rt_prof_window_sync_t;

typedef void (*rt_prof_window_action_t)(void *context);
typedef void (*rt_prof_window_publish_t)(void *context, unsigned frames, double elapsed, unsigned publication);

static inline void RT_ProfWindowBeginFrame(rt_prof_window_t *window, const rt_prof_window_sync_t *sync,
    int enabled, int capturing, double now, rt_prof_window_action_t reset, void *context)
{
    sync->lock(sync->context);
    assert(window->pending == 0);
    if (window->active != enabled)
    {
        reset(context);
        window->frames = 0;
        window->started = now;
        window->active = enabled;
    }
    window->frame_open = capturing;
    sync->unlock(sync->context);
}

static inline void RT_ProfWindowEndFrame(rt_prof_window_t *window, const rt_prof_window_sync_t *sync)
{
    sync->lock(sync->context);
    if (window->frame_open)
    {
        ++window->frames;
        window->frame_open = 0;
    }
    sync->unlock(sync->context);
}

static inline void RT_ProfWindowSubmitEnd(rt_prof_window_t *window, const rt_prof_window_sync_t *sync)
{
    sync->lock(sync->context);
    ++window->pending;
    sync->unlock(sync->context);
}

static inline void RT_ProfWindowRecordEnd(rt_prof_window_t *window, const rt_prof_window_sync_t *sync,
    rt_prof_window_action_t record, void *context)
{
    sync->lock(sync->context);
    assert(window->pending > 0);
    record(context);
    --window->pending;
    sync->unlock(sync->context);
}

static inline int RT_ProfWindowTryPublish(rt_prof_window_t *window, const rt_prof_window_sync_t *sync,
    double now, double interval, rt_prof_window_publish_t publish, rt_prof_window_action_t clear, void *context)
{
    sync->lock(sync->context);
    const double elapsed = now - window->started;
    if (!window->active || window->frame_open || window->pending != 0 || window->frames == 0 || elapsed < interval)
    {
        sync->unlock(sync->context);
        return 0;
    }
    publish(context, window->frames, elapsed, ++window->publications);
    clear(context);
    window->frames = 0;
    window->started = now;
    sync->unlock(sync->context);
    return 1;
}

#endif
