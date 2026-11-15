/*
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// cl_predweapons.c -- immediate feedback for the player's own weapon
//
// The server decides when a weapon fires and tells us afterwards: the muzzle
// flash, the sound, the gun animation and the railgun trail all arrive one
// round trip after the trigger was pulled. This file runs the same weapon
// logic as the standard game code on the client, ahead of the server by that
// round trip, and shows those effects at once. When the server's own version
// of the shot arrives it is recognised and not shown a second time.
//
// Nothing about the shot itself changes: the server still fires when the
// command reaches it, and still decides what was hit.
//
// The local weapon state is a copy of what the server will have when the
// command we send right now arrives ("command time"). It is advanced once
// per server frame and by each attack command, and it is compared with every
// server frame: the frame the server reports must be the frame we had for
// that server frame. When it is not, the local state is rebuilt from the
// server's.

#include "client.h"

cvar_t	*cl_predict_weapons;
cvar_t	*cl_predict_weapons_debug;
cvar_t	*cl_predict_weapons_delay;

extern cvar_t	*cl_predict_players_maxping;
extern cvar_t	*cl_predict_players_offset;

extern cvar_t	*hand;
extern cvar_t	*cl_predict;

#define	WP_HISTORY		32
#define	WP_EVENTS		16
#define	WP_CONFIRM_TIME	500		// ms past the round trip a confirmation may take
#define	WP_MAX_MISSES	3		// unconfirmed shots in a row before giving up for a while
#define	WP_SUSPEND_TIME	5000
#define	WP_FRAME_TIME	100		// ms the gun model takes to move to a new frame

typedef enum
{
	WS_READY,
	WS_ACTIVATING,
	WS_DROPPING,
	WS_FIRING
} wpstate_t;

typedef enum
{
	WK_BLASTER,
	WK_SHOTGUN,
	WK_SSHOTGUN,
	WK_MACHINEGUN,
	WK_CHAINGUN,
	WK_GRENADELAUNCHER,
	WK_ROCKETLAUNCHER,
	WK_HYPERBLASTER,
	WK_RAILGUN,
	WK_BFG
} wpkind_t;

// the frame numbers are those of the standard view weapon models, as
// used by the standard game code (Weapon_Generic)
typedef struct
{
	const char	*model;
	wpkind_t	kind;
	int			activateLast;
	int			fireLast;
	int			idleLast;
	int			quantity;		// ammo needed to start firing
	int			fireFrames[20];
} wpdef_t;

static const wpdef_t wp_defs[] =
{
	{"models/weapons/v_blast/tris.md2",		WK_BLASTER,			4, 8, 52, 0,	{5, 0}},
	{"models/weapons/v_shotg/tris.md2",		WK_SHOTGUN,			7, 18, 36, 1,	{8, 9, 0}},
	{"models/weapons/v_shotg2/tris.md2",	WK_SSHOTGUN,		6, 17, 57, 2,	{7, 0}},
	{"models/weapons/v_machn/tris.md2",		WK_MACHINEGUN,		3, 5, 45, 1,	{4, 5, 0}},
	{"models/weapons/v_chain/tris.md2",		WK_CHAINGUN,		4, 31, 61, 1,	{5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 0}},
	{"models/weapons/v_launch/tris.md2",	WK_GRENADELAUNCHER,	5, 16, 59, 1,	{6, 0}},
	{"models/weapons/v_rocket/tris.md2",	WK_ROCKETLAUNCHER,	4, 12, 50, 1,	{5, 0}},
	{"models/weapons/v_hyperb/tris.md2",	WK_HYPERBLASTER,	5, 20, 49, 1,	{6, 7, 8, 9, 10, 11, 0}},
	{"models/weapons/v_rail/tris.md2",		WK_RAILGUN,			3, 18, 56, 1,	{4, 0}},
	{"models/weapons/v_bfg/tris.md2",		WK_BFG,				8, 32, 55, 50,	{9, 17, 0}},
	{NULL}
};

// the weapon as the server will have it when a command sent now arrives
static struct
{
	qboolean		valid;
	const wpdef_t	*def;
	int				gunframe;
	wpstate_t		state;
	qboolean		held;		// attack button in the last command
	qboolean		latched;	// attack pressed and not yet acted on
	qboolean		thunk;		// already thought since the last server frame
	int				tick;		// server frame this state belongs to
} wp;

static struct
{
	int		tick;
	int		gunframe;
} wp_history[WP_HISTORY];

// shots shown ahead of the server, waiting to be confirmed by it
typedef struct
{
	qboolean	open;
	int			weapon;		// MZ_*
	int			time;
	int			cost;		// ammo
	int			showTime;	// when to show it; 0 once shown
} wpevent_t;

static wpevent_t	wp_events[WP_EVENTS];
static int			wp_eventCount;

static qboolean		wp_silent;			// rebuilding state: no effects
static int			wp_silenced;		// the last confirmed shot had a silencer
static int			wp_misses;
static int			wp_suspendTime;		// no predicted shots before this
static int			wp_blockTime;		// same, after a weapon change was asked for
static float		wp_rtt;

static int			wp_railTime;
static vec3_t		wp_railStart;
static int			wp_railDrawTime;	// a trail waiting to be drawn, 0 if none
static vec3_t		wp_railEnd;

// the gun model
static struct
{
	qboolean	active;
	int			from, to;
	int			start;
	int			holdTime;
} wp_gun;

// changes of the weapon frame that are still to be shown
#define	WP_GUNQUEUE		32

static struct
{
	int			time;
	int			frame;
	qboolean	firing;
} wp_gunQueue[WP_GUNQUEUE];

static int			wp_gunHead, wp_gunTail;
static int			wp_gunQueuedFrame;
static qboolean		wp_gunQueuedFiring;
static qboolean		wp_gunFiring;

/*
=================
CL_WP_Init
=================
*/
void CL_WP_Init (void)
{
	cl_predict_weapons = Cvar_Get ("cl_predict_weapons", "1", CVAR_ARCHIVE);
	cl_predict_weapons_debug = Cvar_Get ("cl_predict_weapons_debug", "0", 0);
	cl_predict_weapons_delay = Cvar_Get ("cl_predict_weapons_delay", "-1", CVAR_ARCHIVE);
}

/*
=================
CL_WP_Reset
=================
*/
void CL_WP_Reset (void)
{
	memset (&wp, 0, sizeof(wp));
	memset (wp_history, 0, sizeof(wp_history));
	memset (wp_events, 0, sizeof(wp_events));
	memset (&wp_gun, 0, sizeof(wp_gun));

	wp_eventCount = 0;
	wp_silent = false;
	wp_silenced = 0;
	wp_misses = 0;
	wp_suspendTime = 0;
	wp_blockTime = 0;
	wp_rtt = 0;
	wp_railTime = 0;
	wp_railDrawTime = 0;
	wp_gunHead = wp_gunTail = 0;
	wp_gunQueuedFrame = -1;
	wp_gunQueuedFiring = false;
	wp_gunFiring = false;
}

/*
=================
WP_Allowed

Is there a weapon in our hands that we know how to follow?
=================
*/
static qboolean WP_Allowed (void)
{
	const player_state_t	*ps;

	// part of player prediction: only in use while that is
	if (!cl_predict_weapons->intvalue || !CL_PP_Active ())
		return false;

	if (cls.state != ca_active || cl.attractloop || !cl.frame.valid)
		return false;

	ps = &cl.frame.playerstate;

	// dead, spectating, following someone, intermission
	if (ps->pmove.pm_type != PM_NORMAL)
		return false;

	if (!ps->gunindex)
		return false;

	return true;
}

/*
=================
WP_CanShow

May new shots be shown ahead of the server right now?
=================
*/
static qboolean WP_CanShow (void)
{
	if (wp_silent)
		return false;

	if (cls.realtime < wp_suspendTime || cls.realtime < wp_blockTime)
		return false;

	return true;
}

static const wpdef_t *WP_FindDef (int gunindex)
{
	const wpdef_t	*d;
	const char		*name;

	if (gunindex <= 0 || gunindex >= MAX_MODELS)
		return NULL;

	name = cl.configstrings[CS_MODELS + gunindex];

	for (d = wp_defs; d->model; d++)
	{
		if (!Q_stricmp ((char *)name, (char *)d->model))
			return d;
	}

	return NULL;
}

/*
=================
WP_Ammo

Ammo for the current weapon as the server will have it: what it last told
us, less the shots it has not confirmed yet.
=================
*/
static int WP_Ammo (void)
{
	int		i, ammo;

	if (!wp.def->quantity)
		return 9999;

	ammo = cl.frame.playerstate.stats[STAT_AMMO];

	for (i = 0; i < WP_EVENTS; i++)
	{
		if (wp_events[i].open)
			ammo -= wp_events[i].cost;
	}

	return ammo;
}

/*
=================
WP_Delay

How long to hold back what is shown of a shot, in ms.

Other players are drawn ahead of the server's picture by the part of the
ping that is compensated. The shot is decided when it reaches the server,
and the players on screen are where they were at that moment only after
the part of the ping that is not compensated has passed. A shot shown
then is seen against the players it was really fired at; shown at once,
it would be seen against players who still have time to move away.
=================
*/
static int WP_Delay (void)
{
	float	ping, comp;

	if (cl_predict_weapons_delay->value >= 0)
		return (int)cl_predict_weapons_delay->value;

	ping = wp_rtt + cl_predict_players_offset->value;
	if (ping < 0)
		ping = 0;

	comp = cl_predict_players_maxping->value;
	if (comp < 0)
		comp = 0;

	if (ping <= comp)
		return 0;

	return (int)(ping - comp);
}

/*
=================
WP_Rail

The trail of a railgun shot fired right now
=================
*/
static void WP_Rail (void)
{
	vec3_t	forward, right, start, end;
	float	side;
	int		delay;

	wp_railTime = 0;

	// needs our own predicted position
	if (!cl_predict->intvalue || (cl.frame.playerstate.pmove.pm_flags & PMF_NO_PREDICTION))
		return;

	AngleVectors (cl.predicted_angles, forward, right, NULL);

	if (hand->intvalue == 1)
		side = -7;
	else if (hand->intvalue == 2)
		side = 0;
	else
		side = 7;

	// the muzzle: 7 to the side of and 8 below the eyes
	VectorMA (cl.predicted_origin, side, right, start);
	start[2] += ((cl.frame.playerstate.pmove.pm_flags & PMF_DUCKED) ? -2 : 22) - 8;

	VectorMA (start, 8192, forward, end);
	CL_PP_ShotTrace (start, end, end);

	wp_railTime = cls.realtime;
	FastVectorCopy (start, wp_railStart);
	FastVectorCopy (end, wp_railEnd);

	delay = WP_Delay ();
	if (delay < 1)
	{
		CL_RailTrailEffect (start, end);
		wp_railDrawTime = 0;
	}
	else
	{
		wp_railDrawTime = cls.realtime + delay;
	}
}

/*
=================
WP_Shot

The weapon fires: show it, and remember that the server's report of this
shot is still to come.
=================
*/
static void WP_Shot (int weapon, int cost)
{
	wpevent_t	*ev;
	int			delay;

	if (!WP_CanShow ())
		return;

	ev = &wp_events[wp_eventCount++ % WP_EVENTS];
	ev->open = true;
	ev->weapon = weapon;
	ev->time = cls.realtime;
	ev->cost = cost;

	// the trail is where the shot goes now, whenever it is drawn
	if (weapon == MZ_RAILGUN)
		WP_Rail ();

	delay = WP_Delay ();

	if (cl_predict_weapons_debug->intvalue)
		Com_Printf ("WP: shot %d at frame %d, ping %.0f ms, shown after %d ms\n", LOG_CLIENT, weapon, wp.tick, wp_rtt, delay);

	if (delay < 1)
	{
		ev->showTime = 0;
		CL_MuzzleFlash (cl.playernum + 1, weapon, wp_silenced);
	}
	else
	{
		ev->showTime = cls.realtime + delay;
	}
}

/*
=================
WP_Fire

The weapon is on one of its firing frames. Follows the fire functions of
the standard game code for what happens to the frame, and when a shot
leaves the gun.
=================
*/
static void WP_Fire (void)
{
	int		ammo, shots;

	ammo = WP_Ammo ();

	switch (wp.def->kind)
	{
	case WK_BLASTER:
		WP_Shot (MZ_BLASTER, 0);
		wp.gunframe++;
		break;

	case WK_SHOTGUN:
		if (wp.gunframe == 9)
		{
			wp.gunframe++;
			break;
		}
		WP_Shot (MZ_SHOTGUN, 1);
		wp.gunframe++;
		break;

	case WK_SSHOTGUN:
		WP_Shot (MZ_SSHOTGUN, 2);
		wp.gunframe++;
		break;

	case WK_MACHINEGUN:
		if (!wp.held)
		{
			wp.gunframe++;
			break;
		}

		if (wp.gunframe == 5)
			wp.gunframe = 4;
		else
			wp.gunframe = 5;

		if (ammo < 1)
		{
			wp.gunframe = 6;
			break;
		}

		WP_Shot (MZ_MACHINEGUN, 1);
		break;

	case WK_CHAINGUN:
		if (wp.gunframe == 14 && !wp.held)
		{
			wp.gunframe = 32;
			break;
		}
		else if (wp.gunframe == 21 && wp.held && ammo)
		{
			wp.gunframe = 15;
		}
		else
		{
			wp.gunframe++;
		}

		if (wp.gunframe <= 9)
			shots = 1;
		else if (wp.gunframe <= 14)
			shots = wp.held ? 2 : 1;
		else
			shots = 3;

		if (ammo < shots)
			shots = ammo;

		if (shots < 1)
			break;

		WP_Shot (MZ_CHAINGUN1 + shots - 1, shots);
		break;

	case WK_GRENADELAUNCHER:
		WP_Shot (MZ_GRENADE, 1);
		wp.gunframe++;
		break;

	case WK_ROCKETLAUNCHER:
		WP_Shot (MZ_ROCKET, 1);
		wp.gunframe++;
		break;

	case WK_HYPERBLASTER:
		if (!wp.held)
		{
			wp.gunframe++;
			break;
		}

		if (ammo > 0)
		{
			WP_Shot (MZ_HYPERBLASTER, 1);
			ammo--;
		}

		wp.gunframe++;
		if (wp.gunframe == 12 && ammo > 0)
			wp.gunframe = 6;
		break;

	case WK_RAILGUN:
		WP_Shot (MZ_RAILGUN, 1);
		wp.gunframe++;
		break;

	case WK_BFG:
		// frame 9 is the charging sound, the ball leaves on frame 17
		if (wp.gunframe == 9)
			WP_Shot (MZ_BFG, 0);
		wp.gunframe++;
		break;
	}
}

/*
=================
WP_Think

One step of the weapon, as Weapon_Generic in the standard game code
=================
*/
static void WP_Think (void)
{
	const wpdef_t	*d = wp.def;
	int				idleFirst = d->fireLast + 1;
	int				n;

	if (wp.state == WS_DROPPING)
	{
		// the server changes weapons at the end of this; wait for it
		wp.gunframe++;
		return;
	}

	if (wp.state == WS_ACTIVATING)
	{
		if (wp.gunframe >= d->activateLast)
		{
			wp.state = WS_READY;
			wp.gunframe = idleFirst;
			return;
		}

		wp.gunframe++;
		return;
	}

	if (wp.state == WS_READY)
	{
		if (wp.latched || wp.held)
		{
			wp.latched = false;

			if (WP_Ammo () >= d->quantity)
			{
				wp.gunframe = d->activateLast + 1;
				wp.state = WS_FIRING;
			}
		}
		else
		{
			if (wp.gunframe >= d->idleLast)
				wp.gunframe = idleFirst;
			else
				wp.gunframe++;
			return;
		}
	}

	if (wp.state == WS_FIRING)
	{
		for (n = 0; d->fireFrames[n]; n++)
		{
			if (wp.gunframe == d->fireFrames[n])
			{
				WP_Fire ();
				break;
			}
		}

		if (!d->fireFrames[n])
			wp.gunframe++;

		if (wp.gunframe == idleFirst + 1)
			wp.state = WS_READY;
	}
}

/*
=================
WP_UpdateGun

Start moving the gun model whenever the weapon frame has changed
=================
*/
static void WP_UpdateGun (void)
{
	qboolean	firing = (wp.state == WS_FIRING);
	int			time, last;

	if (wp.gunframe == wp_gunQueuedFrame && firing == wp_gunQueuedFiring)
		return;

	wp_gunQueuedFrame = wp.gunframe;
	wp_gunQueuedFiring = firing;

	time = cls.realtime + WP_Delay ();

	// keep them in order should the delay have shrunk
	if (wp_gunHead != wp_gunTail)
	{
		last = wp_gunQueue[(wp_gunHead - 1) & (WP_GUNQUEUE-1)].time;
		if (time < last)
			time = last;
	}

	if (wp_gunHead - wp_gunTail >= WP_GUNQUEUE)
		wp_gunTail++;

	wp_gunQueue[wp_gunHead & (WP_GUNQUEUE-1)].time = time;
	wp_gunQueue[wp_gunHead & (WP_GUNQUEUE-1)].frame = wp.gunframe;
	wp_gunQueue[wp_gunHead & (WP_GUNQUEUE-1)].firing = firing;
	wp_gunHead++;
}

/*
=================
WP_Advance

Bring the weapon up to command time: one think for every server frame
that will have run before a command sent now arrives.
=================
*/
static void WP_Advance (void)
{
	double	serverTime;
	int		tick, interval;

	if (!wp.valid)
		return;

	if (!CL_PP_CommandTime (&serverTime, &wp_rtt))
	{
		wp.valid = false;
		return;
	}

	interval = 1000 / cl.settings[SVSET_FPS];
	if (interval < 1)
		interval = 100;

	tick = cl.frame.serverframe + (int)floor ((serverTime - (double)cl.frame.servertime) / interval);

	if (tick - wp.tick > WP_HISTORY - 2)
	{
		// lost touch with the server; start again from its next frame
		wp.valid = false;
		return;
	}

	while (wp.tick < tick)
	{
		wp.tick++;

		if (wp.thunk)
			wp.thunk = false;
		else
			WP_Think ();

		wp_history[wp.tick & (WP_HISTORY-1)].tick = wp.tick;
		wp_history[wp.tick & (WP_HISTORY-1)].gunframe = wp.gunframe;
	}

	WP_UpdateGun ();
}

/*
=================
WP_SameFrame

Do the server's weapon frame and ours for the same server frame agree?
The idle animation has random pauses, so any two idle frames agree.
=================
*/
static qboolean WP_SameFrame (const wpdef_t *d, int a, int b)
{
	if (a == b)
		return true;

	if (a > d->fireLast && a <= d->idleLast && b > d->fireLast && b <= d->idleLast)
		return true;

	return false;
}

/*
=================
WP_Resync

Take the weapon as the server has it in the latest frame and bring it
forward to command time without showing anything.
=================
*/
static void WP_Resync (const wpdef_t *d)
{
	int		g = cl.frame.playerstate.gunframe;

	if (cl_predict_weapons_debug->intvalue && wp.valid)
		Com_Printf ("WP: resync at frame %d, server gun frame %d, ours %d\n", LOG_CLIENT, cl.frame.serverframe,
			g, wp_history[cl.frame.serverframe & (WP_HISTORY-1)].gunframe);

	if (wp.def != d)
		wp.latched = false;

	wp.def = d;
	wp.gunframe = g;
	wp.thunk = false;
	wp.tick = cl.frame.serverframe;

	// the first idle frame is the last step of firing
	if (g <= d->activateLast)
		wp.state = WS_ACTIVATING;
	else if (g <= d->fireLast + 1)
		wp.state = WS_FIRING;
	else if (g <= d->idleLast)
		wp.state = WS_READY;
	else
		wp.state = WS_DROPPING;

	wp_history[wp.tick & (WP_HISTORY-1)].tick = wp.tick;
	wp_history[wp.tick & (WP_HISTORY-1)].gunframe = g;

	wp.valid = true;

	wp_silent = true;
	WP_Advance ();
	wp_silent = false;
}

/*
=================
CL_WP_NewFrame

A server frame has arrived: check our weapon against the server's
=================
*/
void CL_WP_NewFrame (void)
{
	const wpdef_t	*d;
	int				i, f;
	wpevent_t		*ev;

	// shots the server never confirmed
	for (i = 0; i < WP_EVENTS; i++)
	{
		ev = &wp_events[i];

		if (ev->open && cls.realtime - ev->time > (int)wp_rtt + WP_CONFIRM_TIME)
		{
			ev->open = false;

			if (cl_predict_weapons_debug->intvalue)
				Com_Printf ("WP: shot %d was never confirmed by the server\n", LOG_CLIENT, ev->weapon);

			if (++wp_misses >= WP_MAX_MISSES)
			{
				// this server's weapons do not behave the way we expect
				wp_misses = 0;
				wp_suspendTime = cls.realtime + WP_SUSPEND_TIME;
			}
		}
	}

	if (!WP_Allowed ())
	{
		wp.valid = false;
		return;
	}

	d = WP_FindDef (cl.frame.playerstate.gunindex);
	if (!d)
	{
		wp.valid = false;
		return;
	}

	f = cl.frame.serverframe;

	if (!wp.valid || wp.def != d || wp.tick < f ||
		wp_history[f & (WP_HISTORY-1)].tick != f ||
		!WP_SameFrame (d, wp_history[f & (WP_HISTORY-1)].gunframe, cl.frame.playerstate.gunframe))
	{
		WP_Resync (d);
	}
}

/*
=================
CL_WP_Input

Called for every command that is sent to the server, with the state of
the attack button in it.
=================
*/
void CL_WP_Input (qboolean attack)
{
	if (!wp.valid || !WP_Allowed ())
	{
		wp.held = attack;
		return;
	}

	WP_Advance ();
	if (!wp.valid)
	{
		wp.held = attack;
		return;
	}

	if (attack && !wp.held)
		wp.latched = true;

	wp.held = attack;

	// the server lets the weapon think as soon as a fresh press arrives,
	// which is what makes a shot leave without waiting for a server frame
	if (wp.latched && !wp.thunk)
	{
		wp.thunk = true;
		WP_Think ();
		WP_UpdateGun ();
	}
}

/*
=================
CL_WP_RenderFrame
=================
*/
void CL_WP_RenderFrame (void)
{
	int			i;
	wpevent_t	*ev;

	// shots that were held back
	for (i = 0; i < WP_EVENTS; i++)
	{
		ev = &wp_events[i];

		if (ev->showTime && cls.realtime >= ev->showTime)
		{
			ev->showTime = 0;
			CL_MuzzleFlash (cl.playernum + 1, ev->weapon, wp_silenced);
		}
	}

	if (wp_railDrawTime && cls.realtime >= wp_railDrawTime)
	{
		wp_railDrawTime = 0;
		CL_RailTrailEffect (wp_railStart, wp_railEnd);
	}

	if (!wp.valid || !WP_Allowed ())
		return;

	WP_Advance ();
}

/*
=================
CL_WP_GunFrame

The frames the gun model should be drawn between while the weapon is
firing. False when the frames from the server should be used.
=================
*/
qboolean CL_WP_GunFrame (int *frame, int *oldframe, float *backlerp)
{
	const wpdef_t	*d;
	int				g;
	float			f;

	if (!wp.valid || !WP_Allowed () || cls.realtime < wp_suspendTime || cls.realtime < wp_blockTime)
	{
		wp_gun.active = false;
		wp_gunFiring = false;
		wp_gunTail = wp_gunHead;
		return false;
	}

	d = wp.def;

	// frame changes that have become due
	while (wp_gunTail != wp_gunHead && wp_gunQueue[wp_gunTail & (WP_GUNQUEUE-1)].time <= cls.realtime)
	{
		int		i = wp_gunTail & (WP_GUNQUEUE-1);

		if (wp_gunQueue[i].frame != wp_gun.to || !wp_gun.active)
		{
			wp_gun.from = wp_gun.active ? wp_gun.to : cl.frame.playerstate.gunframe;
			wp_gun.to = wp_gunQueue[i].frame;
			wp_gun.start = wp_gunQueue[i].time;
		}

		wp_gunFiring = wp_gunQueue[i].firing;
		if (wp_gunFiring)
			wp_gun.active = true;

		wp_gunTail++;
	}

	if (wp_gunFiring)
	{
		wp_gun.active = true;
		wp_gun.holdTime = cls.realtime + (int)wp_rtt + 300;
	}
	else if (wp_gun.active)
	{
		// the animation is over for us; keep the gun still while the
		// server's copy of it is still running, or it would play the end twice
		g = cl.frame.playerstate.gunframe;

		if (cls.realtime > wp_gun.holdTime || g <= d->activateLast || g > d->fireLast)
			wp_gun.active = false;
	}

	if (!wp_gun.active)
		return false;

	f = (float)(cls.realtime - wp_gun.start) / WP_FRAME_TIME;
	if (f < 0)
		f = 0;
	else if (f > 1)
		f = 1;

	*frame = wp_gun.to;
	*oldframe = wp_gun.from;
	*backlerp = 1.0f - f;
	return true;
}

/*
=================
CL_WP_ConfirmShot

The server reports a shot of ours. True if it was already shown.
=================
*/
qboolean CL_WP_ConfirmShot (int weapon, int silenced)
{
	int			i, best;
	wpevent_t	*ev;

	wp_silenced = silenced;

	best = -1;
	for (i = 0; i < WP_EVENTS; i++)
	{
		ev = &wp_events[i];

		if (!ev->open)
			continue;

		// the chaingun reports one, two or three bullets
		if (ev->weapon != weapon &&
			!(ev->weapon >= MZ_CHAINGUN1 && ev->weapon <= MZ_CHAINGUN3 && weapon >= MZ_CHAINGUN1 && weapon <= MZ_CHAINGUN3))
			continue;

		if (best == -1 || ev->time < wp_events[best].time)
			best = i;
	}

	if (best == -1)
	{
		if (cl_predict_weapons_debug->intvalue && cl_predict_weapons->intvalue)
			Com_Printf ("WP: shot %d from the server was not expected\n", LOG_CLIENT, weapon);
		return false;
	}

	if (cl_predict_weapons_debug->intvalue)
		Com_Printf ("WP: shot %d confirmed after %d ms\n", LOG_CLIENT, weapon, cls.realtime - wp_events[best].time);

	wp_events[best].open = false;
	wp_misses = 0;

	// the server was quicker than our own delayed version: show the server's
	if (wp_events[best].showTime)
	{
		wp_events[best].showTime = 0;
		return false;
	}

	return true;
}

/*
=================
CL_WP_ConfirmRail

The server sends a railgun trail. True if it is the one we already drew.
=================
*/
qboolean CL_WP_ConfirmRail (vec3_t start)
{
	vec3_t	d;

	if (!wp_railTime)
		return false;

	if (cls.realtime - wp_railTime > (int)wp_rtt + WP_CONFIRM_TIME)
	{
		wp_railTime = 0;
		return false;
	}

	VectorSubtract (start, wp_railStart, d);
	if (VectorLength (d) > 96)
		return false;

	// the server was quicker than our own delayed trail: show the server's
	if (wp_railDrawTime)
	{
		wp_railDrawTime = 0;
		wp_railTime = 0;
		return false;
	}

	wp_railTime = 0;
	return true;
}

/*
=================
CL_WP_Command

A command is on its way to the server. After one that changes weapons
the server will not fire the weapon we still see in our hands, so no
shots are shown until its answer has had time to arrive.
=================
*/
void CL_WP_Command (const char *cmd)
{
	if (!Q_stricmp ((char *)cmd, "use") || !Q_stricmp ((char *)cmd, "weapnext") ||
		!Q_stricmp ((char *)cmd, "weapprev") || !Q_stricmp ((char *)cmd, "weaplast") ||
		!Q_stricmp ((char *)cmd, "invuse") || !Q_stricmp ((char *)cmd, "drop") ||
		!Q_stricmp ((char *)cmd, "invdrop"))
	{
		wp_blockTime = cls.realtime + (int)wp_rtt + 300;
	}
}
