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

#endif
