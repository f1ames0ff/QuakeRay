#ifndef RT_FRAME_POLICY_H
#define RT_FRAME_POLICY_H

static inline int RT_ShouldRenderUiOnly(int hasWorld, int fullySignedOn)
{
    return !hasWorld || !fullySignedOn;
}

static inline int RT_ShouldSkipConsoleDraw(int menuActive, int consoleForced, float menuOpacity)
{
    return menuActive && consoleForced && menuOpacity >= 1.0f;
}

static inline int RT_EndTaskResultMergesNow(unsigned pendingSerial, unsigned resultSerial)
{
    return pendingSerial != 0 && pendingSerial == resultSerial;
}

static inline int RT_EndFrameConsumesEarlyResult(unsigned frameSerial, unsigned finishedSerial)
{
    return frameSerial != 0 && frameSerial == finishedSerial;
}

static inline int RT_CacheGenerationStale(unsigned threadGeneration, unsigned globalGeneration)
{
    return threadGeneration != globalGeneration;
}

static inline int RT_ReportTakeRun(unsigned *active)
{
    if (!*active)
        return 0;

    *active = 0;
    return 1;
}

#endif
