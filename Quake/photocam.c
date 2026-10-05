#include "quakedef.h"
#include "glquake.h"
#include "photocam.h"
#include "observer.h"
#include "qr_editor.h"
#include "screen.h"

extern kbutton_t in_forward, in_back, in_moveleft, in_moveright, in_up, in_down, in_jump;
extern qboolean  keydown[MAX_KEYS];

typedef struct
{
	qboolean active;
	qboolean live;
	qboolean sv_paused_prev;
	vec3_t   cam_origin;
	vec3_t   player_viewangles;
} photocam_t;

static photocam_t photocam;

static void PhotoCam_MoveCamera (void)
{
	vec3_t fwd, right, up;
	float  fm, sm, um, speed;
	int    i;

	AngleVectors (cl.viewangles, fwd, right, up);

	fm = (float)((in_forward.state & 1) - (in_back.state & 1));
	sm = (float)((in_moveright.state & 1) - (in_moveleft.state & 1));
	um = (float)(((in_up.state | in_jump.state) & 1) - (in_down.state & 1));

	speed = 450.0f * (float)host_frametime;
	if (keydown[K_SHIFT])
		speed *= 3.0f;

	for (i = 0; i < 3; i++)
		photocam.cam_origin[i] += (fwd[i] * fm + right[i] * sm + up[i] * um) * speed;
}

void PhotoCam_Stop (void)
{
	if (!photocam.active)
		return;

	if (photocam.live)
		sv.paused = true;

	photocam.active = false;
	photocam.live = false;

	VectorCopy (photocam.player_viewangles, cl.viewangles);

	if (sv.paused)
		sv.paused = photocam.sv_paused_prev;

	Con_Printf ("photocam: off\n");
}

static void PhotoCam_Start (void)
{
	if (photocam.active)
		return;

	if (QR_Editor_Active ())
	{
		Con_Printf ("photocam: the qr editor is active\n");
		return;
	}
	if (Observer_Active ())
	{
		Con_Printf ("photocam: the camera observer is active\n");
		return;
	}
	if (cls.state != ca_connected || !cl.worldmodel)
	{
		Con_Printf ("photocam: a level must be loaded first\n");
		return;
	}
	if (!sv.active || svs.maxclients > 1 || cls.demoplayback)
	{
		Con_Printf ("photocam: single player only (the world has to be frozen)\n");
		return;
	}

	photocam.active = true;
	photocam.live = false;
	photocam.sv_paused_prev = sv.paused;
	sv.paused = true;

	VectorCopy (r_refdef.vieworg, photocam.cam_origin);
	VectorCopy (cl.viewangles, photocam.player_viewangles);

	Con_Printf ("photocam: on (WASD + mouse, SPACE/CTRL up/down, SHIFT fast; LMB screenshot; hold RMB to run the world; ESC or photocam to leave)\n");
}

static void PhotoCam_f (void)
{
	if (photocam.active)
		PhotoCam_Stop ();
	else
		PhotoCam_Start ();
}

qboolean PhotoCam_Active (void)
{
	return photocam.active;
}

qboolean PhotoCam_Frozen (void)
{
	return photocam.active && !photocam.live;
}

void PhotoCam_SetLive (qboolean live)
{
	if (!photocam.active || photocam.live == live)
		return;

	photocam.live = live;

	if (live)
	{
		if (sv.active && svs.clients != NULL && svs.clients[0].edict != NULL)
		{
			memset (&svs.clients[0].cmd, 0, sizeof (svs.clients[0].cmd));

			svs.clients[0].edict->v.velocity[0] = 0.0f;
			svs.clients[0].edict->v.velocity[1] = 0.0f;
			svs.clients[0].edict->v.velocity[2] = 0.0f;
		}

		sv.paused = false;
	}
	else
	{
		sv.paused = true;
	}
}

void PhotoCam_UpdateView (void)
{
	if (!photocam.active)
		return;

	PhotoCam_MoveCamera ();

	VectorCopy (photocam.cam_origin, r_refdef.vieworg);
	VectorCopy (cl.viewangles, r_refdef.viewangles);
}

qboolean PhotoCam_KeyEvent (int key, qboolean down)
{
	if (!photocam.active || key_dest != key_game)
		return false;

	if (key == K_ESCAPE && down)
	{
		PhotoCam_Stop ();
		return true;
	}

	return false;
}

void PhotoCam_Screenshot (void)
{
	SCR_ScreenShot_f ();
}

void PhotoCam_OnNewMap (void)
{
	if (!photocam.active)
		return;

	photocam.active = false;
	photocam.live = false;

	Con_Printf ("photocam: off (the map changed)\n");
}

void PhotoCam_Init (void)
{
	Cmd_AddCommand ("photocam", PhotoCam_f);
}
