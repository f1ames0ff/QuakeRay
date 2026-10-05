#ifndef OBSERVER_H
#define OBSERVER_H

#include "quakedef.h"

void     Observer_Register (void);
void     Observer_Unregister (void);

qboolean Observer_Active (void);
qboolean Observer_Frozen (void);

void     Observer_Stop (void);
void     Observer_OnNewMap (void);

void     Observer_UpdateView (void);
void     Observer_MouseMove (float dmy);
void     Observer_Zoom (qboolean on);
void     Observer_Wheel (int y);
qboolean Observer_KeyEvent (int key, qboolean down);

#endif /* OBSERVER_H */
