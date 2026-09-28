#ifndef RT_DTAL_DEBUG_H
#define RT_DTAL_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

#define RT_DTAL_DEBUG_FLOATS_PER_ARROW 7

int  RT_DtalDebugBuildArrows (float *out, int max_arrows, int fb_w, int fb_h);
void RT_DtalDebugDrawGui (int mode, unsigned int frame_id, float dt, int fb_x, int fb_y, int fb_w, int fb_h, int drawable_h);

#ifdef __cplusplus
}
#endif

#endif
