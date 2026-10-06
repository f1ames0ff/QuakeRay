#include "quakedef.h"
#include "sv_gibs.h"

cvar_t gibs_damage_vector = { "gibs_damage_vector", "1", CVAR_ARCHIVE };

#define SV_GIB_MARKS      64
#define SV_GIB_DAMAGE_TTL 0.25f
#define SV_GIB_MARK_TTL   1.0f
#define SV_GIB_DIR_BLEND  0.55f
#define SV_GIB_JITTER     0.3f

typedef struct
{
	edict_t *ed;
	vec3_t   dir;
	float    damage;
	float    time;
} sv_gib_mark_t;

static sv_gib_mark_t sv_gib_marks[SV_GIB_MARKS];
static int           sv_gib_mark_count;

static qcvm_t      *sv_gib_vm;
static void        *sv_gib_progs;
static dfunction_t *sv_gib_fn_damage;
static dfunction_t *sv_gib_fn_throwgib;
static dfunction_t *sv_gib_fn_throwhead;
static dfunction_t *sv_gib_fn_beam;
static dfunction_t *sv_gib_fn_beamd;

static qboolean sv_gib_spawn_pending;
static qboolean sv_gib_damage_valid;
static vec3_t   sv_gib_damage_dir;
static float    sv_gib_damage_amount;
static float    sv_gib_damage_time;

static void SV_Gibs_MarkClear (sv_gib_mark_t *mark)
{
	if (mark->ed != NULL)
	{
		mark->ed = NULL;
		sv_gib_mark_count--;
	}
}

static void SV_Gibs_FindFunctions (void)
{
	int i;

	sv_gib_vm = qcvm;
	sv_gib_progs = qcvm->progs;

	sv_gib_fn_damage = ED_FindFunction ("T_Damage");
	sv_gib_fn_throwgib = ED_FindFunction ("ThrowGib");
	sv_gib_fn_throwhead = ED_FindFunction ("ThrowHead");
	sv_gib_fn_beam = ED_FindFunction ("LightningDamage");
	sv_gib_fn_beamd = ED_FindFunction ("T_BeamDamage");

	sv_gib_spawn_pending = false;
	sv_gib_damage_valid = false;

	for (i = 0; i < SV_GIB_MARKS; i++)
		sv_gib_marks[i].ed = NULL;

	sv_gib_mark_count = 0;
}

static void SV_Gibs_CaptureDamage (dfunction_t *caller)
{
	edict_t *world = EDICT_NUM (0);
	edict_t *targ = PROG_TO_EDICT (((int *)qcvm->globals)[OFS_PARM0]);
	edict_t *inflictor = PROG_TO_EDICT (((int *)qcvm->globals)[OFS_PARM0 + 3]);
	edict_t *attacker = PROG_TO_EDICT (((int *)qcvm->globals)[OFS_PARM0 + 6]);
	vec3_t   from, dir;

	if (caller == sv_gib_fn_beam || caller == sv_gib_fn_beamd)
		return;

	if (targ == NULL || targ == world)
		return;

	if (inflictor != NULL && inflictor != world)
		VectorCopy (inflictor->v.origin, from);
	else if (attacker != NULL && attacker != world)
		VectorCopy (attacker->v.origin, from);
	else
		return;

	VectorSubtract (targ->v.origin, from, dir);

	if (VectorLength (dir) < 1.0f)
		return;

	VectorNormalize (dir);
	VectorCopy (dir, sv_gib_damage_dir);
	sv_gib_damage_amount = ((float *)qcvm->globals)[OFS_PARM0 + 9];
	sv_gib_damage_valid = true;
	sv_gib_damage_time = qcvm->time;
}

static void SV_Gibs_Mark (edict_t *ed)
{
	int i;

	if (ed == NULL || ed == EDICT_NUM (0))
		return;
	if (!CVAR_TO_BOOL (gibs_damage_vector) || !sv_gib_damage_valid)
		return;
	if (qcvm->time - sv_gib_damage_time > SV_GIB_DAMAGE_TTL)
		return;

	for (i = 0; i < SV_GIB_MARKS; i++)
	{
		if (sv_gib_marks[i].ed == NULL)
		{
			sv_gib_marks[i].ed = ed;
			VectorCopy (sv_gib_damage_dir, sv_gib_marks[i].dir);
			sv_gib_marks[i].damage = sv_gib_damage_amount;
			sv_gib_marks[i].time = qcvm->time;
			sv_gib_mark_count++;
			return;
		}
	}

	i = 0;
	VectorCopy (sv_gib_damage_dir, sv_gib_marks[i].dir);
	sv_gib_marks[i].damage = sv_gib_damage_amount;
	sv_gib_marks[i].time = qcvm->time;
	sv_gib_marks[i].ed = ed;
}

void SV_Gibs_OnCall (dfunction_t *callee, dfunction_t *caller)
{
	if (qcvm != &sv.qcvm)
		return;

	if (sv_gib_vm != qcvm || sv_gib_progs != qcvm->progs)
		SV_Gibs_FindFunctions ();

	if (callee == sv_gib_fn_throwgib || callee == sv_gib_fn_throwhead)
	{
		sv_gib_spawn_pending = true;

		if (callee == sv_gib_fn_throwhead && pr_global_struct != NULL)
			SV_Gibs_Mark (PROG_TO_EDICT (pr_global_struct->self));
		return;
	}

	if (callee == sv_gib_fn_damage && CVAR_TO_BOOL (gibs_damage_vector))
		SV_Gibs_CaptureDamage (caller);
}

void SV_Gibs_OnSpawn (edict_t *ed)
{
	if (!sv_gib_spawn_pending)
		return;

	sv_gib_spawn_pending = false;

	SV_Gibs_Mark (ed);
}

void SV_Gibs_Apply (edict_t *ent)
{
	int i;

	if (sv_gib_mark_count <= 0)
		return;

	for (i = 0; i < SV_GIB_MARKS; i++)
	{
		sv_gib_mark_t *mark = &sv_gib_marks[i];
		float          speed;
		vec3_t         qcdir, dir;

		if (mark->ed != ent)
			continue;

		SV_Gibs_MarkClear (mark);

		if (!CVAR_TO_BOOL (gibs_damage_vector))
			return;
		if (qcvm->time - mark->time > SV_GIB_MARK_TTL)
			return;

		{
			const char *mdl = ent->v.model ? PR_GetString (ent->v.model) : "";

			if (!strstr (mdl, "gib") && !strstr (mdl, "h_") && !strstr (mdl, "head"))
				return;
		}

		speed = VectorLength (ent->v.velocity);
		if (speed < 120.0f)
			speed = CLAMP (40.0f, mark->damage * 3.0f, 600.0f);
		if (speed < 1.0f)
			return;

		VectorCopy (ent->v.velocity, qcdir);
		VectorNormalize (qcdir);

		VectorScale (mark->dir, SV_GIB_DIR_BLEND, dir);
		VectorMA (dir, 1.0f - SV_GIB_DIR_BLEND, qcdir, dir);

		dir[0] += ((rand () % 2001) / 1000.0f - 1.0f) * SV_GIB_JITTER;
		dir[1] += ((rand () % 2001) / 1000.0f - 1.0f) * SV_GIB_JITTER;
		dir[2] += ((rand () % 2001) / 1000.0f - 1.0f) * SV_GIB_JITTER;

		if (VectorLength (dir) < 0.01f)
			return;

		VectorNormalize (dir);
		VectorScale (dir, speed * (0.85f + (rand () % 301) / 1000.0f), ent->v.velocity);
		return;
	}
}

void SV_Gibs_OnNewMap (void)
{
	int i;

	sv_gib_spawn_pending = false;
	sv_gib_damage_valid = false;

	for (i = 0; i < SV_GIB_MARKS; i++)
		sv_gib_marks[i].ed = NULL;

	sv_gib_mark_count = 0;
}

void SV_Gibs_Init (void)
{
	Cvar_RegisterVariable (&gibs_damage_vector);
}
