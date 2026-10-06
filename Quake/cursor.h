#ifndef _QUAKE_CURSOR_H
#define _QUAKE_CURSOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Cursor_Init (void);
void Cursor_Shutdown (void);
void Cursor_SetStandard (int standard);

int Cursor_GetGuiCursor (int64_t *texture, int *size, int *hotX, int *hotY);

#ifdef __cplusplus
}
#endif

#endif /* _QUAKE_CURSOR_H */
