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

#ifndef RT_DTAL_DEBUG_H
#define RT_DTAL_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

#define RT_DTAL_DEBUG_FLOATS_PER_ARROW 8

int  RT_DtalDebugBuildArrows (float *out, int max_arrows, int fb_w, int fb_h);
void RT_DtalDebugDrawGui (int mode, unsigned int frame_id, float dt, int fb_x, int fb_y, int fb_w, int fb_h, int drawable_h);

#ifdef __cplusplus
}
#endif

#endif
