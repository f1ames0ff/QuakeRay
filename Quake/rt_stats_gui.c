#include "quakedef.h"
#include "glquake.h"
#include "qr_gui.h"

#define RT_STATS_HISTORY 60

#define RT_STATS_COLOR_TOTAL  0xFFFF4D0Du
#define RT_STATS_COLOR_DETAIL 0xFFD3D3D3u

typedef struct
{
	float samples[RT_STATS_HISTORY];
	int   count;
} rt_stats_series_t;

static rt_stats_series_t rt_stats_series_gpu_frame;
static rt_stats_series_t rt_stats_series_gpu_pass[QR_GPU_PASS_COUNT];
static rt_stats_series_t rt_stats_series_cpu_frame;
static rt_stats_series_t rt_stats_series_cpu_main;
static rt_stats_series_t rt_stats_series_cpu_wait;
static rt_stats_series_t rt_stats_series_cpu_draw;
static rt_stats_series_t rt_stats_series_cpu_slot[RT_PROF_COUNT];

static rt_stats_snapshot_t rt_stats_overlay_snap;
static qboolean             rt_stats_overlay_sampled;
static double               rt_stats_overlay_next;
static unsigned             rt_stats_overlay_panels;

static void RT_StatsSeriesReset (rt_stats_series_t *series)
{
	series->count = 0;
}

static void RT_StatsSeriesResetAll (void)
{
	int i;

	RT_StatsSeriesReset (&rt_stats_series_gpu_frame);
	for (i = 0; i < QR_GPU_PASS_COUNT; i++)
		RT_StatsSeriesReset (&rt_stats_series_gpu_pass[i]);

	RT_StatsSeriesReset (&rt_stats_series_cpu_frame);
	RT_StatsSeriesReset (&rt_stats_series_cpu_main);
	RT_StatsSeriesReset (&rt_stats_series_cpu_wait);
	RT_StatsSeriesReset (&rt_stats_series_cpu_draw);
	for (i = 0; i < RT_PROF_COUNT; i++)
		RT_StatsSeriesReset (&rt_stats_series_cpu_slot[i]);
}

static void RT_StatsSeriesPush (rt_stats_series_t *series, float value)
{
	if (series->count < RT_STATS_HISTORY)
	{
		series->samples[series->count++] = value;
		return;
	}

	memmove (series->samples, series->samples + 1, sizeof (series->samples[0]) * (RT_STATS_HISTORY - 1));
	series->samples[RT_STATS_HISTORY - 1] = value;
}

static void RT_StatsOverlaySample (void)
{
	const rt_stats_snapshot_t *snap;
	int                        i;

	RT_StatsCapture (&rt_stats_overlay_snap);
	rt_stats_overlay_sampled = true;
	snap = &rt_stats_overlay_snap;

	if (snap->haveGpu && snap->gpu.gpuTimingValid)
	{
		RT_StatsSeriesPush (&rt_stats_series_gpu_frame, snap->gpu.gpuFrameMs);
		for (i = 0; i < QR_GPU_PASS_COUNT; i++)
			RT_StatsSeriesPush (&rt_stats_series_gpu_pass[i], snap->gpu.gpuPassMs[i]);
	}

	if (snap->haveProfile)
	{
		const rt_prof_report_t *rep = &snap->profile;

		RT_StatsSeriesPush (&rt_stats_series_cpu_frame, rep->frameMs);
		RT_StatsSeriesPush (&rt_stats_series_cpu_main, rep->frameMs - rep->waitMs);
		RT_StatsSeriesPush (&rt_stats_series_cpu_wait, rep->waitMs);
		RT_StatsSeriesPush (&rt_stats_series_cpu_draw, rep->ms[RT_PROF_DRAWFRAME]);

		for (i = 0; i < RT_PROF_COUNT; i++)
			RT_StatsSeriesPush (&rt_stats_series_cpu_slot[i], rep->ms[i]);
	}

	RT_StatsRecordSample (snap);
}

static void RT_StatsOverlayMs (const char *label, float ms, const rt_stats_series_t *series, uint32_t color)
{
	char st[32];

	if (ms >= 100.0f)
		q_snprintf (st, sizeof (st), "%.0f ms", ms);
	else if (ms >= 10.0f)
		q_snprintf (st, sizeof (st), "%.1f ms", ms);
	else
		q_snprintf (st, sizeof (st), "%.2f ms", ms);

	QR_GUI_OverlayRow (label, st, series ? series->samples : NULL, series ? series->count : 0, color);
}

static void RT_StatsOverlayCount (const char *label, unsigned value)
{
	char st[32];

	q_snprintf (st, sizeof (st), "%u", value);
	QR_GUI_OverlayRow (label, st, NULL, 0, RT_STATS_COLOR_TOTAL);
}

static void RT_StatsOverlayRays (const rt_stats_snapshot_t *snap)
{
	RT_StatsOverlayCount ("RAYS", snap->gpu.raysTotal);
	RT_StatsOverlayCount ("PRIMARY", snap->gpu.raysPerCategory[0]);
	RT_StatsOverlayCount ("REFL/REFR", snap->gpu.raysPerCategory[1]);
	RT_StatsOverlayCount ("INDIRECT", snap->gpu.raysPerCategory[2]);
	RT_StatsOverlayCount ("SHADOW DIR", snap->gpu.raysPerCategory[3]);
	RT_StatsOverlayCount ("SHADOW IND", snap->gpu.raysPerCategory[4]);
	RT_StatsOverlayCount ("CALLS", snap->gpu.apiCalls);
}

static void RT_StatsOverlayGpu (const rt_stats_snapshot_t *snap)
{
	int i;

	if (!snap->haveGpu)
	{
		QR_GUI_OverlayNote ("the renderer reports no frame stats");
		return;
	}

	if (!snap->gpu.gpuTimingValid)
	{
		QR_GUI_OverlayNote ("pass timings not collected");
		return;
	}

	RT_StatsOverlayMs ("TOTAL", snap->gpu.gpuFrameMs, &rt_stats_series_gpu_frame, RT_STATS_COLOR_TOTAL);

	for (i = 0; i < QR_GPU_PASS_COUNT; i++)
		RT_StatsOverlayMs (qrGetGpuPassName (i), snap->gpu.gpuPassMs[i], &rt_stats_series_gpu_pass[i], RT_STATS_COLOR_DETAIL);
}

static void RT_StatsOverlayCpu (const rt_stats_snapshot_t *snap)
{
	const rt_prof_report_t *rep = &snap->profile;
	char                    st[64];
	int                     i;

	if (!snap->haveProfile)
	{
		QR_GUI_OverlayNote ("waiting for the first window");
		return;
	}

	RT_StatsOverlayMs ("FRAME", rep->frameMs, &rt_stats_series_cpu_frame, RT_STATS_COLOR_TOTAL);
	RT_StatsOverlayMs ("MAIN", rep->frameMs - rep->waitMs, &rt_stats_series_cpu_main, RT_STATS_COLOR_TOTAL);
	RT_StatsOverlayMs ("WAIT", rep->waitMs, &rt_stats_series_cpu_wait, RT_STATS_COLOR_TOTAL);
	RT_StatsOverlayMs ("qrDrawFrame", rep->ms[RT_PROF_DRAWFRAME], &rt_stats_series_cpu_draw, RT_STATS_COLOR_TOTAL);

	for (i = 0; i < RT_PROF_COUNT; i++)
	{
		if (i == RT_PROF_FRAME || i == RT_PROF_WAIT || i == RT_PROF_DRAWFRAME)
			continue;

		RT_StatsOverlayMs (RT_ProfSlotName (i), rep->ms[i], &rt_stats_series_cpu_slot[i], RT_STATS_COLOR_DETAIL);
	}

	const int cacheFrames = rep->clusterCacheHits + rep->clusterCacheMisses;

	if (cacheFrames > 0)
	{
		q_snprintf (st, sizeof (st), "clust cache %3i%% of %i", (100 * rep->clusterCacheHits) / cacheFrames, cacheFrames);
		QR_GUI_OverlayNote (st);
	}

	q_snprintf (st, sizeof (st), "clust miss set %i move %i other %i", rep->clusterMissSet, rep->clusterMissMove,
	            rep->clusterMissOther);
	QR_GUI_OverlayNote (st);

	q_snprintf (st, sizeof (st), "clust grants %i denied %i gated %i", rep->clusterGrants, rep->clusterDenied, rep->clusterGated);
	QR_GUI_OverlayNote (st);

	q_snprintf (st, sizeof (st), "clust lights %i add %i drop %i", rep->clusterLights, rep->clusterAttempts,
	            rep->clusterDropped);
	QR_GUI_OverlayNote (st);
}

void RT_StatsGuiReset (void)
{
	rt_stats_overlay_panels = 0;
	rt_stats_overlay_sampled = false;
	RT_StatsSeriesResetAll ();
}

void RT_StatsDrawGui (void)
{
	const unsigned             panels = CVAR_TO_UINT32 (rt_stats_panels);
	const rt_stats_snapshot_t *snap = &rt_stats_overlay_snap;
	const qboolean             rays = (panels & (1u << (RT_STATS_RAYS - 1))) != 0;
	const qboolean             passes = (panels & (1u << (RT_STATS_PASSES - 1))) != 0;
	const qboolean             profile = (panels & (1u << (RT_STATS_PROFILE - 1))) != 0;
	char                       title[64];
	double                     now;
	double                     interval;

	if (!QR_GUI_Ready ())
		return;

	interval = CLAMP (0.05, CVAR_TO_FLOAT (rt_stats_interval), 0.2);

	if (!(interval >= 0.05 && interval <= 0.2))
		interval = 0.2;

	now = Sys_DoubleTime ();

	if (panels != rt_stats_overlay_panels)
	{
		rt_stats_overlay_panels = panels;
		rt_stats_overlay_sampled = false;
		RT_StatsSeriesResetAll ();
	}

	if (!panels && !RT_StatsRecording ())
		return;

	if (!rt_stats_overlay_sampled || now >= rt_stats_overlay_next)
	{
		RT_StatsOverlaySample ();
		rt_stats_overlay_next = now + interval;
	}

	if (!panels)
		return;

	title[0] = 0;
	if (snap->haveGpu)
		q_snprintf (title, sizeof (title), "%.1f FPS", snap->gpu.fpsX10 / 10.0f);
	else if (snap->haveProfile)
		q_snprintf (title, sizeof (title), "%.1f FPS", snap->profile.fps);

	QR_GUI_OverlayBegin ("##rt_stats", 8.0f, 8.0f, 0.62f, title[0] ? title : NULL);

	if (profile)
	{
		QR_GUI_OverlaySection ("CPU");
		RT_StatsOverlayCpu (snap);
	}

	if (passes)
	{
		QR_GUI_OverlaySection ("GPU");
		RT_StatsOverlayGpu (snap);
	}

	if (rays)
	{
		QR_GUI_OverlaySection ("RAYS");
		if (snap->haveGpu)
			RT_StatsOverlayRays (snap);
		else
			QR_GUI_OverlayNote ("the renderer reports no frame stats");
	}

	QR_GUI_OverlayEnd ();
}
