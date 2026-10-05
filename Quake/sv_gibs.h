#ifndef SV_GIBS_H
#define SV_GIBS_H

#include "quakedef.h"

void SV_Gibs_Init (void);
void SV_Gibs_OnCall (dfunction_t *callee, dfunction_t *caller);
void SV_Gibs_OnSpawn (edict_t *ed);
void SV_Gibs_Apply (edict_t *ent);
void SV_Gibs_OnNewMap (void);

#endif /* SV_GIBS_H */
