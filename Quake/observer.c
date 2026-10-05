#include "quakedef.h"
#include "glquake.h"
#include "observer.h"
#include "photocam.h"
#include "qr_editor.h"
#include "client.h"

#define OBSERVER_DEG2RAD(a) ((a) * M_PI_DIV_180)
#define OBSERVER_RAD2DEG(a) ((a) / M_PI_DIV_180)

#define OBSERVER_TRACE_DIST    8192.0f
#define OBSERVER_NO_HIT_DIST   512.0f
#define OBSERVER_MIN_RADIUS    16.0f
#define OBSERVER_MAX_RADIUS    8192.0f
#define OBSERVER_MAX_ELEV      85.0f
#define OBSERVER_SPEED_DEFAULT 45.0f
#define OBSERVER_SPEED_STEP    15.0f
#define OBSERVER_SPEED_MAX     180.0f
#define OBSERVER_ZOOM_RATE     0.005f

typedef struct
{
	qboolean active;
	qboolean zooming;
	qboolean sv_paused_prev;
	vec3_t   pivot;
	float    radius;
	float    azimuth;   // degrees around the pivot
	float    elevation; // degrees above the pivot
	float    speed;     // degrees per second, negative turns the other way
} observer_t;

static observer_t observer;

/*
=============
Observer_PlaceCamera

Puts the camera on the orbit and aims it back at the pivot.
=============
*/
static void Observer_PlaceCamera (void)
{
	float  ce = cos (OBSERVER_DEG2RAD (observer.elevation));
	float  se = sin (OBSERVER_DEG2RAD (observer.elevation));
	float  ca = cos (OBSERVER_DEG2RAD (observer.azimuth));
	float  sa = sin (OBSERVER_DEG2RAD (observer.azimuth));
	vec3_t origin, dir, angles;

	origin[0] = observer.pivot[0] + observer.radius * ce * ca;
	origin[1] = observer.pivot[1] + observer.radius * ce * sa;
	origin[2] = observer.pivot[2] + observer.radius * se;

	VectorSubtract (observer.pivot, origin, dir);
	VectorAngles (dir, NULL, angles);

	VectorCopy (origin, r_refdef.vieworg);
	VectorCopy (angles, r_refdef.viewangles);
}

static void Observer_Start (void)
{
	vec3_t  fwd, right, up, end, impact, offset;
	float   ratio;
	trace_t trace;
	qcvm_t *oldvm;

	if (observer.active)
		return;

	if (QR_Editor_Active ())
	{
		Con_Printf ("camera_observer: the qr editor is active\n");
		return;
	}
	if (PhotoCam_Active ())
	{
		Con_Printf ("camera_observer: photocam is active\n");
		return;
	}
	if (cls.state != ca_connected || !cl.worldmodel)
	{
		Con_Printf ("camera_observer: a level must be loaded first\n");
		return;
	}
	if (!sv.active || svs.maxclients > 1 || cls.demoplayback)
	{
		Con_Printf ("camera_observer: single player only (the world has to be frozen)\n");
		return;
	}

	// The pivot is where the aim meets the world -- a monster included; the
	// camera keeps the distance it had to that spot and looks at it while
	// turning around it. SV_Move clips through the server VM's edicts, and the
	// command arrives with no VM active, so borrow the server one for the trace.
	AngleVectors (cl.viewangles, fwd, right, up);
	VectorMA (r_refdef.vieworg, OBSERVER_TRACE_DIST, fwd, end);

	oldvm = qcvm;
	PR_SwitchQCVM (NULL);
	PR_SwitchQCVM (&sv.qcvm);

	trace = SV_Move (r_refdef.vieworg, vec3_origin, vec3_origin, end, MOVE_NORMAL, sv_player);

	PR_SwitchQCVM (NULL);
	PR_SwitchQCVM (oldvm);

	if (trace.fraction >= 1.0f)
		VectorMA (r_refdef.vieworg, OBSERVER_NO_HIT_DIST, fwd, impact);
	else
		VectorCopy (trace.endpos, impact);

	VectorCopy (impact, observer.pivot);
	VectorSubtract (r_refdef.vieworg, observer.pivot, offset);
	observer.radius = VectorLength (offset);
	if (observer.radius < OBSERVER_MIN_RADIUS)
	{
		// the aim met something right at the eye: keep the pivot on the aim
		// axis, one minimum radius ahead
		VectorMA (r_refdef.vieworg, OBSERVER_MIN_RADIUS, fwd, observer.pivot);
		observer.radius = OBSERVER_MIN_RADIUS;
		VectorScale (fwd, -OBSERVER_MIN_RADIUS, offset);
	}

	observer.azimuth = OBSERVER_RAD2DEG (atan2 (offset[1], offset[0]));

	ratio = offset[2] / observer.radius;
	observer.elevation = CLAMP (-OBSERVER_MAX_ELEV, OBSERVER_RAD2DEG (asin (CLAMP (-1.0f, ratio, 1.0f))), OBSERVER_MAX_ELEV);
	observer.speed = OBSERVER_SPEED_DEFAULT;

	observer.active = true;
	observer.sv_paused_prev = sv.paused;
	sv.paused = true;

	Con_Printf ("camera_observer: on (wheel: rotation speed; mouse Y: vertical angle; RMB + mouse Y: zoom; ESC or camera_observer to leave)\n");
}

void Observer_Stop (void)
{
	if (!observer.active)
		return;

	observer.active = false;
	observer.zooming = false;
	sv.paused = observer.sv_paused_prev;

	Con_Printf ("camera_observer: off\n");
}

static void Observer_f (void)
{
	if (observer.active)
		Observer_Stop ();
	else
		Observer_Start ();
}

qboolean Observer_Active (void)
{
	return observer.active;
}

qboolean Observer_Frozen (void)
{
	return observer.active;
}

void Observer_UpdateView (void)
{
	if (!observer.active)
		return;

	observer.azimuth += observer.speed * (float)host_frametime;
	if (observer.azimuth > 180.0f)
		observer.azimuth -= 360.0f;
	else if (observer.azimuth < -180.0f)
		observer.azimuth += 360.0f;

	Observer_PlaceCamera ();
}

void Observer_MouseMove (float dmy)
{
	if (!observer.active)
		return;

	// RMB held: the vertical movement brings the camera closer to the pivot or
	// moves it away; otherwise it lifts or lowers the orbit.
	if (observer.zooming)
	{
		observer.radius *= (float)exp (OBSERVER_ZOOM_RATE * dmy);
		observer.radius = CLAMP (OBSERVER_MIN_RADIUS, observer.radius, OBSERVER_MAX_RADIUS);
		return;
	}

	observer.elevation -= m_pitch.value * dmy;
	if (observer.elevation > OBSERVER_MAX_ELEV)
		observer.elevation = OBSERVER_MAX_ELEV;
	else if (observer.elevation < -OBSERVER_MAX_ELEV)
		observer.elevation = -OBSERVER_MAX_ELEV;
}

void Observer_Zoom (qboolean on)
{
	if (!observer.active)
		return;

	observer.zooming = on;
}

void Observer_Wheel (int y)
{
	if (!observer.active || y == 0)
		return;

	observer.speed += OBSERVER_SPEED_STEP * (float)y;
	observer.speed = CLAMP (-OBSERVER_SPEED_MAX, observer.speed, OBSERVER_SPEED_MAX);

	Con_Printf ("camera_observer: speed %.0f deg/s\n", observer.speed);
}

qboolean Observer_KeyEvent (int key, qboolean down)
{
	if (!observer.active || key_dest != key_game)
		return false;

	if (key == K_ESCAPE && down)
	{
		Observer_Stop ();
		return true;
	}

	return false;
}

void Observer_OnNewMap (void)
{
	if (!observer.active)
		return;

	observer.active = false;
	observer.zooming = false;

	Con_Printf ("camera_observer: off (the map changed)\n");
}

static cmd_function_t *observer_cmd;

void Observer_Register (void)
{
	if (observer_cmd != NULL)
		return;

	observer_cmd = Cmd_AddCommand2 ("camera_observer", Observer_f, src_command);
}

void Observer_Unregister (void)
{
	Observer_Stop ();

	if (observer_cmd != NULL)
	{
		Cmd_RemoveCommand (observer_cmd);
		observer_cmd = NULL;
	}
}
