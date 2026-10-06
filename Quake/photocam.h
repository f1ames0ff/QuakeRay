#ifndef PHOTOCAM_H
#define PHOTOCAM_H

#include "quakedef.h"

void PhotoCam_Init (void);

qboolean PhotoCam_Active (void);
qboolean PhotoCam_Frozen (void);
void     PhotoCam_SetLive (qboolean live);

void     PhotoCam_UpdateView (void);
qboolean PhotoCam_KeyEvent (int key, qboolean down);
void     PhotoCam_Screenshot (void);
void     PhotoCam_Stop (void);
void     PhotoCam_OnNewMap (void);

#endif /* PHOTOCAM_H */
