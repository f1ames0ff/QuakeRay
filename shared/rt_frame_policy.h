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
