/*
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

// cl_predplayers.c -- client side prediction of OTHER players
//
// The server does no lag compensation: a shot is resolved against wherever
// the other players are at the moment our usercmd arrives. Stock Quake 2 draws
// other players interpolated between the last two snapshots, so what you see
// is (interpolation delay + round trip) old and you have to lead by your ping.
//
// Here every other player is drawn where the server will have them when a
// command sent right now arrives:
//
//   lead = (time since the latest snapshot was taken) + (round trip time)
//
// The server only sends origins, so velocity is derived from consecutive
// snapshots and the player is moved forward with a small physics model
// (gravity, wall sliding, stair stepping, riding movers) clipped against the
// world and brush models. Whenever a new snapshot changes the forecast, the
// difference is blended out over a short time instead of popping.
//
// Everything is driven by cls.realtime at render time and by data from the
// server frames, so it behaves the same with cl_async 0 and 1.

#include "client.h"
#include <time.h>

cvar_t	*cl_predict_players;
cvar_t	*cl_predict_players_maxping;
cvar_t	*cl_predict_players_smooth;
cvar_t	*cl_predict_players_offset;
cvar_t	*cl_predict_players_debug;
cvar_t	*cl_predict_players_log;

extern cvar_t	*hand;

static FILE	*pp_logFile;

// the accuracy meter runs for the console readout and for the shot log
#define	PP_METERING	(cl_predict_players_debug->intvalue == -2 || pp_logFile)

static void PP_LogRecord (int entnum, vec3_t shown, vec3_t raw, vec3_t lerp);
static void PP_LogFrame (void);
static float PP_Lead (int servertime);
static qboolean PP_Enabled (void);

#define	PP_STEPSIZE			18
#define	PP_MIN_STEP_NORMAL	0.7f
#define	PP_SIM_STEP			0.025f		// seconds per simulation step
#define	PP_JUMP_SPEED		270.0f
#define	PP_RUN_SPEED		300.0f
#define	PP_MAX_SPEED		3000.0f		// faster than this is a teleport
#define	PP_MAX_MOVER_SPEED	1500.0f
#define	PP_MAX_ERROR		256.0f		// bigger corrections just snap
#define	PP_RTT_SAMPLES		16
#define	PP_MAX_BMODELS		512

// prediction is there to make up for a high ping, not to give players with a
// good connection something extra: below this round trip time it stays off
#define	PP_MIN_PING			100.0f
#define	PP_MIN_PING_ON		105.0f		// needed to switch on; avoids flicker at the limit

#define	PPF_GROUND			1			// standing on something
#define	PPF_NOGRAVITY		2			// swimming, on a ladder, hooked, flying...

typedef struct
{
	vec3_t		origin;
	vec3_t		velocity;		// world velocity, includes the mover's
	vec3_t		moverVelocity;	// velocity of the brush model being ridden
	vec3_t		mins, maxs;
	int			flags;
	int			servertime;
} ppsnap_t;

#define	PP_SAMPLES			64

typedef struct
{
	int			targetTime;		// server time the drawing was meant for, 0 = unused
	vec3_t		shown;
	vec3_t		raw;
} ppsample_t;

typedef struct
{
	// raw tracking of the entity as the server reports it
	qboolean	tracked;
	int			serverframe;
	vec3_t		lastOrigin;
	qboolean	wasAirborne;
	float		lastRawVz;
	float		speed;			// filtered horizontal speed

	// forecast state
	ppsnap_t	snap, oldsnap;
	qboolean	hasOld;
	qboolean	newSnap;
	qboolean	reset;

	// what is on screen
	vec3_t		error;
	int			errorTime;
	vec3_t		shown;
	int			shownTime;
	qboolean	hasShown;
	qboolean	fallback;

	// accuracy meter: what was drawn, and for which server time
	ppsample_t	samples[PP_SAMPLES];
	int			sampleHead;
	int			sampleTime;
} ppstate_t;

static ppstate_t	pp_players[MAX_CLIENTS];

// brush models in the current frame, for clipping
static entity_state_t	*pp_bmodels[PP_MAX_BMODELS];
static int				pp_numBmodels;

// own tracking of brush model motion (centity_t prev is not reliable for new ents)
static int				pp_entFrame[MAX_EDICTS];
static vec3_t			pp_entOrigin[MAX_EDICTS];
static vec3_t			pp_entVelocity[MAX_EDICTS];

// timing
static qboolean			pp_clockValid;
static double			pp_clockOffset;		// realtime - servertime, smoothed
static int				pp_rttSamples[PP_RTT_SAMPLES];
static int				pp_rttCount;
static float			pp_rtt;				// ms
static qboolean			pp_pingOk;			// ping is high enough for prediction
static int				pp_lastServertime;
static int				pp_debugTime;

// accuracy meter totals (cl_predict_players_debug -2)
static int				pp_accCount;
static float			pp_accShownAlong, pp_accShownMs, pp_accShownSq;
static float			pp_accRawAlong, pp_accRawMs, pp_accRawSq;
static int				pp_accTime;

/*
================
PP_MeasureAccuracy

The server has just told us where a player was during the last interval.
Compare that with what we had on screen for those moments.
last + delta is the path the player actually took.
================
*/
static void PP_MeasureAccuracy (ppstate_t *p, vec3_t last, vec3_t delta, int servertime)
{
	int			i, interval;
	float		speed, frac, len, along;
	vec3_t		dir, truth, e;
	ppsample_t	*sm;

	interval = 1000 / cl.settings[SVSET_FPS];

	len = (float)sqrt (delta[0] * delta[0] + delta[1] * delta[1]);
	speed = len * 1000.0f / interval;

	for (i = 0; i < PP_SAMPLES; i++)
	{
		sm = &p->samples[i];

		if (!sm->targetTime || sm->targetTime > servertime)
			continue;

		if (sm->targetTime > servertime - interval && speed > 100.0f)
		{
			frac = (float)(sm->targetTime - (servertime - interval)) / interval;
			VectorMA (last, frac, delta, truth);

			dir[0] = delta[0] / len;
			dir[1] = delta[1] / len;

			VectorSubtract (sm->shown, truth, e);
			along = e[0] * dir[0] + e[1] * dir[1];
			pp_accShownAlong += along;
			pp_accShownMs += along / speed * 1000.0f;
			pp_accShownSq += DotProduct (e, e);

			VectorSubtract (sm->raw, truth, e);
			along = e[0] * dir[0] + e[1] * dir[1];
			pp_accRawAlong += along;
			pp_accRawMs += along / speed * 1000.0f;
			pp_accRawSq += DotProduct (e, e);

			pp_accCount++;
		}

		sm->targetTime = 0;
	}
}

static void PP_ReportAccuracy (void)
{
	float	n;

	if (cls.realtime - pp_accTime < 1000)
		return;

	pp_accTime = cls.realtime;

	if (pp_accCount >= 10)
	{
		n = (float)pp_accCount;

		if (pp_logFile)
			fprintf (pp_logFile, "ACC t=%d n=%d drawn_along=%+.1f drawn_ms=%+.0f drawn_off=%.1f raw_along=%+.1f raw_ms=%+.0f raw_off=%.1f rtt=%.0f predict=%d smooth=%.0f offset=%.0f\n",
				cls.realtime, pp_accCount, pp_accShownAlong / n, pp_accShownMs / n, (float)sqrt (pp_accShownSq / n),
				pp_accRawAlong / n, pp_accRawMs / n, (float)sqrt (pp_accRawSq / n), pp_rtt,
				cl_predict_players->intvalue, cl_predict_players_smooth->value, cl_predict_players_offset->value);

		if (cl_predict_players_debug->intvalue == -2)
			Com_Printf ("pp accuracy: drawn %+.1f units (%+.0f ms) off %.1f | unsmoothed %+.1f units (%+.0f ms) off %.1f | rtt %.0f\n", LOG_CLIENT,
			pp_accShownAlong / n, pp_accShownMs / n, (float)sqrt (pp_accShownSq / n),
			pp_accRawAlong / n, pp_accRawMs / n, (float)sqrt (pp_accRawSq / n),
			pp_rtt);
	}

	pp_accCount = 0;
	pp_accShownAlong = pp_accShownMs = pp_accShownSq = 0;
	pp_accRawAlong = pp_accRawMs = pp_accRawSq = 0;
}

/*
================
CL_PP_Reset
================
*/
void CL_PP_Reset (void)
{
	memset (pp_players, 0, sizeof(pp_players));
	memset (pp_entFrame, 0, sizeof(pp_entFrame));
	memset (pp_entVelocity, 0, sizeof(pp_entVelocity));
	pp_numBmodels = 0;
	pp_clockValid = false;
	pp_rttCount = 0;
	pp_rtt = 0;
	pp_pingOk = false;
	pp_lastServertime = 0;
}

/*
================
CL_PP_Init
================
*/
void CL_PP_Init (void)
{
	cl_predict_players = Cvar_Get ("cl_predict_players", "0", CVAR_ARCHIVE);
	cl_predict_players_maxping = Cvar_Get ("cl_predict_players_maxping", "50", CVAR_ARCHIVE);
	cl_predict_players_smooth = Cvar_Get ("cl_predict_players_smooth", "30", CVAR_ARCHIVE);
	cl_predict_players_offset = Cvar_Get ("cl_predict_players_offset", "0", CVAR_ARCHIVE);
	cl_predict_players_debug = Cvar_Get ("cl_predict_players_debug", "0", 0);
	cl_predict_players_log = Cvar_Get ("cl_predict_players_log", "0", 0);

	CL_PP_Reset ();
}

static qboolean PP_Enabled (void)
{
	if (!cl_predict_players->intvalue || !cl_predict->intvalue)
		return false;

	if (!pp_pingOk)
		return false;

	if (cls.state != ca_active || cl.attractloop || cl.maxclients <= 1)
		return false;

	// chase cams and the like: our own view is not predicted either
	if (cl.frame.playerstate.pmove.pm_flags & PMF_NO_PREDICTION)
		return false;

	return true;
}

/*
================
PP_Trace

World and brush models only. Other players and items are ignored so that
forecasts do not snag on each other.
================
*/
static trace_t PP_Trace (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, qboolean worldOnly)
{
	trace_t			t, trace;
	int				i;
	entity_state_t	*ent;
	cmodel_t		*cmodel;

	t = CM_BoxTrace (start, end, mins, maxs, 0, MASK_PLAYERSOLID);
	t.ent = (t.fraction < 1.0f || t.startsolid) ? (struct edict_s *)1 : NULL;

	if (worldOnly)
		return t;

	for (i = 0; i < pp_numBmodels; i++)
	{
		if (t.allsolid)
			break;

		ent = pp_bmodels[i];
		cmodel = cl.model_clip[ent->modelindex];
		if (!cmodel)
			continue;

		trace = CM_TransformedBoxTrace (start, end, mins, maxs, cmodel->headnode,
			MASK_PLAYERSOLID, ent->origin, ent->angles);

		if (trace.allsolid || trace.startsolid || trace.fraction < t.fraction)
		{
			qboolean	wasSolid = t.startsolid;

			trace.ent = (struct edict_s *)ent;
			t = trace;
			if (wasSolid)
				t.startsolid = true;
		}
	}

	return t;
}

static void PP_ClipVelocity (vec3_t in, vec3_t normal, vec3_t out)
{
	float	backoff;
	int		i;

	backoff = DotProduct (in, normal) * 1.01f;

	for (i = 0; i < 3; i++)
	{
		out[i] = in[i] - normal[i] * backoff;
		if (out[i] > -0.1f && out[i] < 0.1f)
			out[i] = 0;
	}
}

/*
================
PP_SlideMove

Moves pos along vel for the given time, sliding along whatever it hits.
vel is updated to the clipped velocity.
================
*/
static void PP_SlideMove (vec3_t pos, vec3_t vel, float time, vec3_t mins, vec3_t maxs)
{
	vec3_t		planes[5];
	vec3_t		primal, end, dir;
	int			numplanes = 0;
	int			bump, i, j;
	float		d;
	trace_t		tr;

	FastVectorCopy (*vel, primal);

	for (bump = 0; bump < 4; bump++)
	{
		VectorMA (pos, time, vel, end);

		tr = PP_Trace (pos, mins, maxs, end, false);

		if (tr.allsolid)
		{
			// stuck in something, stay put
			VectorClear (vel);
			return;
		}

		if (tr.fraction > 0)
		{
			FastVectorCopy (tr.endpos, *pos);
			numplanes = 0;
		}

		if (tr.fraction == 1.0f)
			break;

		time -= time * tr.fraction;

		if (numplanes >= 5)
		{
			VectorClear (vel);
			break;
		}

		FastVectorCopy (tr.plane.normal, planes[numplanes]);
		numplanes++;

		// find a velocity that does not push into any of the planes
		for (i = 0; i < numplanes; i++)
		{
			PP_ClipVelocity (vel, planes[i], vel);

			for (j = 0; j < numplanes; j++)
			{
				if (j != i && DotProduct (vel, planes[j]) < 0)
					break;
			}

			if (j == numplanes)
				break;
		}

		if (i == numplanes)
		{
			// wedged, go along the crease if there is one
			if (numplanes != 2)
			{
				VectorClear (vel);
				break;
			}

			CrossProduct (planes[0], planes[1], dir);
			d = DotProduct (dir, vel);
			VectorScale (dir, d, vel);
		}

		// never turn around against the original direction
		if (DotProduct (vel, primal) <= 0)
		{
			VectorClear (vel);
			break;
		}
	}
}

/*
================
PP_StepSlideMove

Slide move that can also climb steps up to PP_STEPSIZE.
================
*/
static void PP_StepSlideMove (vec3_t pos, vec3_t vel, float time, vec3_t mins, vec3_t maxs)
{
	vec3_t		start_o, start_v;
	vec3_t		flat_o, flat_v;
	vec3_t		up, down;
	float		flat_dist, step_dist, raised;
	trace_t		tr;

	FastVectorCopy (*pos, start_o);
	FastVectorCopy (*vel, start_v);

	// plain move
	PP_SlideMove (pos, vel, time, mins, maxs);

	FastVectorCopy (*pos, flat_o);
	FastVectorCopy (*vel, flat_v);

	// raise, move, lower
	FastVectorCopy (start_o, up);
	up[2] += PP_STEPSIZE;

	tr = PP_Trace (start_o, mins, maxs, up, false);
	if (tr.allsolid || tr.startsolid)
		return;

	raised = tr.endpos[2] - start_o[2];
	if (raised <= 0)
		return;

	FastVectorCopy (tr.endpos, *pos);
	FastVectorCopy (start_v, *vel);

	PP_SlideMove (pos, vel, time, mins, maxs);

	FastVectorCopy (*pos, down);
	down[2] -= raised;

	tr = PP_Trace (pos, mins, maxs, down, false);
	if (!tr.allsolid)
		FastVectorCopy (tr.endpos, *pos);

	flat_dist = (flat_o[0] - start_o[0]) * (flat_o[0] - start_o[0]) + (flat_o[1] - start_o[1]) * (flat_o[1] - start_o[1]);
	step_dist = (pos[0] - start_o[0]) * (pos[0] - start_o[0]) + (pos[1] - start_o[1]) * (pos[1] - start_o[1]);

	if (flat_dist >= step_dist || tr.fraction == 1.0f || tr.plane.normal[2] < PP_MIN_STEP_NORMAL)
	{
		// stepping did not help, or it would leave us on a wall / in the air
		FastVectorCopy (flat_o, *pos);
		FastVectorCopy (flat_v, *vel);
		return;
	}

	// keep the vertical speed of the plain move
	vel[2] = flat_v[2];
}

/*
================
PP_Simulate

Moves a snapshot forward by lead seconds.
================
*/
static void PP_Simulate (ppsnap_t *s, float lead, vec3_t out)
{
	vec3_t		pos, vel, v, end;
	vec3_t		mins, maxs;
	qboolean	ground, nograv;
	float		t, dt, gravity, newvz, movevz;
	trace_t		tr;

	FastVectorCopy (s->origin, pos);

	if (lead <= 0)
	{
		FastVectorCopy (pos, *out);
		return;
	}

	FastVectorCopy (s->mins, mins);
	FastVectorCopy (s->maxs, maxs);

	// move relative to whatever we are riding, the ride is added at the end
	VectorSubtract (s->velocity, s->moverVelocity, vel);

	ground = (s->flags & PPF_GROUND) ? true : false;
	nograv = (s->flags & PPF_NOGRAVITY) ? true : false;
	gravity = (float)cl.frame.playerstate.pmove.gravity;

	if (ground)
		vel[2] = 0;

	t = lead;
	while (t > 0)
	{
		dt = t;
		if (dt > PP_SIM_STEP)
			dt = PP_SIM_STEP;
		t -= dt;

		if (ground)
		{
			if (!vel[0] && !vel[1])
				break;		// standing still, nothing will change

			v[0] = vel[0];
			v[1] = vel[1];
			v[2] = 0;

			PP_StepSlideMove (pos, v, dt, mins, maxs);

			// follow the floor down steps and slopes
			FastVectorCopy (pos, end);
			end[2] -= PP_STEPSIZE;

			tr = PP_Trace (pos, mins, maxs, end, false);
			if (tr.allsolid)
				break;

			if (tr.fraction < 1.0f && tr.plane.normal[2] >= PP_MIN_STEP_NORMAL)
			{
				FastVectorCopy (tr.endpos, pos);
			}
			else
			{
				// ran off a ledge
				ground = false;
				vel[2] = 0;
			}
		}
		else
		{
			if (nograv)
			{
				movevz = vel[2];
			}
			else
			{
				newvz = vel[2] - gravity * dt;
				movevz = (vel[2] + newvz) * 0.5f;
				vel[2] = newvz;
			}

			v[0] = vel[0];
			v[1] = vel[1];
			v[2] = movevz;

			if (!v[0] && !v[1] && !v[2])
				continue;

			PP_StepSlideMove (pos, v, dt, mins, maxs);

			vel[0] = v[0];
			vel[1] = v[1];
			if (v[2] != movevz)
				vel[2] = v[2];		// hit a floor or a ceiling

			if (!nograv && vel[2] <= 0)
			{
				// landed?
				FastVectorCopy (pos, end);
				end[2] -= 0.25f;

				tr = PP_Trace (pos, mins, maxs, end, false);
				if (tr.allsolid)
					break;

				if (tr.fraction < 1.0f && tr.plane.normal[2] >= PP_MIN_STEP_NORMAL)
				{
					ground = true;
					vel[2] = 0;
				}
			}
		}
	}

	// carried along by the lift / train we stand on
	if (s->moverVelocity[0] || s->moverVelocity[1] || s->moverVelocity[2])
	{
		VectorMA (pos, lead, s->moverVelocity, end);

		tr = PP_Trace (pos, mins, maxs, end, true);
		if (!tr.allsolid && !tr.startsolid)
			FastVectorCopy (tr.endpos, pos);
	}

	FastVectorCopy (pos, *out);
}

/*
================
PP_Lead

How far ahead of the given snapshot time to forecast, in seconds.
================
*/
static float PP_Lead (int servertime)
{
	double	age;
	float	interval, rtt, maxrtt;

	if (!pp_clockValid)
		return 0;

	interval = 1000.0f / cl.settings[SVSET_FPS];

	// estimated server time right now, minus the snapshot time
	age = ((double)cls.realtime - pp_clockOffset) - (double)servertime;

	// when packets stop arriving do not keep running forever
	if (age < 0)
		age = 0;
	else if (age > interval * 2.5)
		age = interval * 2.5;

	rtt = pp_rtt + cl_predict_players_offset->value;
	maxrtt = cl_predict_players_maxping->value;

	if (rtt > maxrtt)
		rtt = maxrtt;
	if (rtt < 0)
		rtt = 0;

	return (float)((age + rtt) * 0.001);
}

static void PP_DecayError (ppstate_t *p)
{
	int		dt;
	float	tau, f;

	dt = cls.realtime - p->errorTime;
	p->errorTime = cls.realtime;

	if (dt <= 0)
		return;

	tau = cl_predict_players_smooth->value;
	if (tau < 1 || dt > 1000)
	{
		VectorClear (p->error);
		return;
	}

	f = (float)exp (-(double)dt / tau);
	VectorScale (p->error, f, p->error);

	if (DotProduct (p->error, p->error) < 0.01f)
		VectorClear (p->error);
}

/*
================
PP_UpdateTiming

Called for every valid server frame.
================
*/
static void PP_UpdateTiming (void)
{
	double	d;
	int		sample, i, best;

	// server clock relative to ours. packets arrive with jitter, so follow slowly.
	d = (double)cls.realtime - (double)cl.frame.servertime;

	if (!pp_clockValid || cl.frame.servertime < pp_lastServertime || fabs (d - pp_clockOffset) > 250)
	{
		pp_clockOffset = d;
		pp_clockValid = true;
	}
	else
	{
		pp_clockOffset += (d - pp_clockOffset) * 0.1;
	}

	pp_lastServertime = cl.frame.servertime;

	// round trip: when did we send the command this frame acknowledges?
	// the server only acks on its own frames, which adds up to one of our
	// packet intervals. taking the minimum of recent samples removes that.
	sample = cls.realtime - cl.cmd_time[cls.netchan.incoming_acknowledged & (CMD_BACKUP-1)];
	if (sample >= 0 && sample < 2000)
	{
		pp_rttSamples[pp_rttCount % PP_RTT_SAMPLES] = sample;
		pp_rttCount++;

		best = sample;
		for (i = 0; i < PP_RTT_SAMPLES && i < pp_rttCount; i++)
		{
			if (pp_rttSamples[i] < best)
				best = pp_rttSamples[i];
		}

		if (pp_rttCount == 1)
			pp_rtt = (float)best;
		else
			pp_rtt += ((float)best - pp_rtt) * 0.2f;
	}

	if (!pp_rttCount || pp_rtt < PP_MIN_PING)
		pp_pingOk = false;
	else if (pp_rtt >= PP_MIN_PING_ON)
		pp_pingOk = true;
}

/*
================
PP_TrackBmodels

Remembers the brush models of this frame for clipping and works out how
fast each of them is moving.
================
*/
static void PP_TrackBmodels (void)
{
	int				i, df;
	entity_state_t	*s;
	float			scale;
	vec3_t			delta;

	pp_numBmodels = 0;

	for (i = 0; i < cl.frame.num_entities; i++)
	{
		s = &cl_parse_entities[(cl.frame.parse_entities + i) & (MAX_PARSE_ENTITIES-1)];

		if (s->solid != 31)
			continue;

		if (pp_numBmodels < PP_MAX_BMODELS)
			pp_bmodels[pp_numBmodels++] = s;

		df = cl.frame.serverframe - pp_entFrame[s->number];

		VectorClear (pp_entVelocity[s->number]);

		if (pp_entFrame[s->number] && df >= 1 && df <= 3)
		{
			scale = (float)cl.settings[SVSET_FPS] / (float)df;
			VectorSubtract (s->origin, pp_entOrigin[s->number], delta);
			VectorScale (delta, scale, delta);

			if (DotProduct (delta, delta) < PP_MAX_MOVER_SPEED * PP_MAX_MOVER_SPEED)
				FastVectorCopy (delta, pp_entVelocity[s->number]);
		}

		pp_entFrame[s->number] = cl.frame.serverframe;
		FastVectorCopy (s->origin, pp_entOrigin[s->number]);
	}
}

static qboolean PP_IsMover (trace_t *tr)
{
	entity_state_t	*ent;

	if (!tr->ent || tr->ent == (struct edict_s *)1)
		return false;

	ent = (entity_state_t *)tr->ent;

	return (pp_entVelocity[ent->number][0] || pp_entVelocity[ent->number][1] || pp_entVelocity[ent->number][2]);
}

/*
================
PP_TrackPlayer

Builds a new forecast base for one player from the frame just received.
================
*/
static void PP_TrackPlayer (entity_state_t *s)
{
	ppstate_t	*p;
	ppsnap_t	ns;
	int			x, zd, zu, df;
	float		dtsec, gravity, rawvz, dz, e, speed, use;
	vec3_t		delta, end;
	qboolean	fresh, airborne;
	trace_t		tr;

	p = &pp_players[s->number - 1];

	// decode the bounding box
	if (cls.protocolVersion >= MINOR_VERSION_R1Q2_32BIT_SOLID)
	{
		x = (s->solid & 255);
		zd = ((s->solid>>8) & 255);
		zu = ((s->solid>>16) & 65535) - 32768;
	}
	else
	{
		x = 8*(s->solid & 31);
		zd = 8*((s->solid>>5) & 31);
		zu = 8*((s->solid>>10) & 63) - 32;
	}

	memset (&ns, 0, sizeof(ns));

	ns.mins[0] = ns.mins[1] = -(float)x;
	ns.maxs[0] = ns.maxs[1] = (float)x;
	ns.mins[2] = -(float)zd;
	ns.maxs[2] = (float)zu;

	FastVectorCopy (s->origin, ns.origin);
	ns.servertime = cl.frame.servertime;

	gravity = (float)cl.frame.playerstate.pmove.gravity;

	// velocity from the last time we saw this player
	df = cl.frame.serverframe - p->serverframe;
	dtsec = df / (float)cl.settings[SVSET_FPS];
	fresh = true;
	VectorClear (delta);

	if (p->tracked && df >= 1 && df <= 3 &&
		s->event != EV_PLAYER_TELEPORT && s->event != EV_OTHER_TELEPORT)
	{
		VectorSubtract (ns.origin, p->lastOrigin, delta);
		VectorScale (delta, 1.0f / dtsec, ns.velocity);

		if (DotProduct (ns.velocity, ns.velocity) < PP_MAX_SPEED * PP_MAX_SPEED)
			fresh = false;
		else
			VectorClear (ns.velocity);
	}

	rawvz = ns.velocity[2];

	if (PP_METERING)
	{
		if (!fresh && df == 1)
			PP_MeasureAccuracy (p, p->lastOrigin, delta, cl.frame.servertime);
		else
			memset (p->samples, 0, sizeof(p->samples));
	}

	// what are they standing on?
	FastVectorCopy (ns.origin, end);
	end[2] -= 0.25f;

	tr = PP_Trace (ns.origin, ns.mins, ns.maxs, end, false);

	if (tr.allsolid || tr.startsolid)
	{
		// inside something (a door closing on them, bad data...), do not guess
		ns.flags |= PPF_GROUND;
		VectorClear (ns.velocity);
	}
	else if (tr.fraction < 1.0f && tr.plane.normal[2] >= PP_MIN_STEP_NORMAL)
	{
		if (PP_IsMover (&tr))
		{
			ns.flags |= PPF_GROUND;
			FastVectorCopy (pp_entVelocity[((entity_state_t *)tr.ent)->number], ns.moverVelocity);
		}
		else if (rawvz <= 180)
		{
			ns.flags |= PPF_GROUND;
		}
	}
	else
	{
		// a lift moving down leaves a small gap under its riders
		end[2] = ns.origin[2] - 4.0f;

		tr = PP_Trace (ns.origin, ns.mins, ns.maxs, end, false);
		if (!tr.allsolid && tr.fraction < 1.0f && tr.plane.normal[2] >= PP_MIN_STEP_NORMAL && PP_IsMover (&tr))
		{
			ns.flags |= PPF_GROUND;
			FastVectorCopy (pp_entVelocity[((entity_state_t *)tr.ent)->number], ns.moverVelocity);
		}
	}

	airborne = !(ns.flags & PPF_GROUND);

	// The server moves a player once for every packet it gets from them, so
	// the distance covered between two snapshots depends on how many of their
	// packets happened to land in that interval. The direction of the delta
	// is reliable, the speed is not: it flickers by a packet's worth every
	// snapshot, and multiplied by the lead that makes the forecast shake.
	// So take the direction from the newest delta and the speed from a
	// filter, which is only overruled when the speed really changes.
	speed = (float)sqrt (ns.velocity[0] * ns.velocity[0] + ns.velocity[1] * ns.velocity[1]);

	if (fresh || speed < 10.0f)
	{
		if (speed < 10.0f)
			ns.velocity[0] = ns.velocity[1] = 0;

		p->speed = fresh ? 0 : speed;
	}
	else
	{
		if (speed < p->speed * 0.6f || speed > p->speed * 1.6f)
			p->speed = speed;		// started, stopped, got launched
		else
			p->speed += (speed - p->speed) * 0.35f;

		use = p->speed;

		// running on the ground is always at full run speed
		if (!airborne && use > 250.0f && use < 360.0f &&
			!ns.moverVelocity[0] && !ns.moverVelocity[1] && !ns.moverVelocity[2])
			use = PP_RUN_SPEED;

		ns.velocity[0] *= use / speed;
		ns.velocity[1] *= use / speed;
	}

	if (airborne)
	{
		if (gravity <= 0 || (CM_PointContents (ns.origin, 0) & MASK_WATER))
		{
			ns.flags |= PPF_NOGRAVITY;
		}
		else if (fresh)
		{
			// nothing known yet
		}
		else if (delta[2] == 0)
		{
			// held up by something we do not clip against (another player, an
			// item, a monster)
			ns.flags |= PPF_NOGRAVITY;
		}
		else if (p->wasAirborne)
		{
			// two airborne snapshots in a row: is gravity really pulling?
			// ladders, grapples and jetpacks keep their vertical speed.
			if (rawvz - p->lastRawVz > -0.5f * gravity * dtsec)
			{
				ns.flags |= PPF_NOGRAVITY;
			}
			else
			{
				// the delta is the average speed over the interval, we want
				// the speed at the end of it
				ns.velocity[2] = rawvz - gravity * dtsec * 0.5f;
			}
		}
		else
		{
			// left the ground during the last interval
			dz = delta[2];
			e = PP_JUMP_SPEED * PP_JUMP_SPEED - 2.0f * gravity * dz;

			if (dz > 0 && e > 0)
				ns.velocity[2] = (float)sqrt (e);					// a jump
			else if (dz <= 0)
				ns.velocity[2] = -(float)sqrt (-2.0f * gravity * dz);	// walked off an edge
			else
				ns.velocity[2] = rawvz - gravity * dtsec * 0.5f;		// launched
		}
	}

	// shuffle
	if (!fresh && p->tracked && df == 1)
	{
		p->oldsnap = p->snap;
		p->hasOld = true;
	}
	else
	{
		p->hasOld = false;
	}

	if (fresh)
		p->reset = true;

	p->snap = ns;
	p->newSnap = true;

	p->tracked = true;
	p->serverframe = cl.frame.serverframe;
	FastVectorCopy (ns.origin, p->lastOrigin);
	p->wasAirborne = airborne;
	p->lastRawVz = rawvz;

	// the raw material for trying out other forecast models afterwards
	if (pp_logFile)
	{
		fprintf (pp_logFile, "TRK f=%d t=%d e=%d o=%.3f %.3f %.3f a=%.1f %.1f anim=%d fl=%d v=%.0f %.0f %.0f\n",
			cl.frame.serverframe, cls.realtime, s->number,
			s->origin[0], s->origin[1], s->origin[2], s->angles[0], s->angles[1], s->frame,
			ns.flags, ns.velocity[0], ns.velocity[1], ns.velocity[2]);
	}
}

/*
================
CL_PP_NewFrame

A valid server frame has just been parsed.
================
*/
void CL_PP_NewFrame (void)
{
	int				i;
	entity_state_t	*s;

	if (cls.state != ca_active || cl.attractloop)
		return;

	PP_LogFrame ();

	if (PP_METERING)
		PP_ReportAccuracy ();

	PP_UpdateTiming ();
	PP_TrackBmodels ();

	for (i = 0; i < cl.frame.num_entities; i++)
	{
		s = &cl_parse_entities[(cl.frame.parse_entities + i) & (MAX_PARSE_ENTITIES-1)];

		if (s->number < 1 || s->number > cl.maxclients || s->number > MAX_CLIENTS)
			continue;

		if (s->number == cl.playernum + 1)
			continue;

		// corpses and invisible players are not targets
		if (!s->solid || s->solid == 31 || !s->modelindex)
			continue;

		PP_TrackPlayer (s);
	}
}

/*
================
CL_PP_GetOrigin

Called while adding a player entity to the scene. lerped is the stock
interpolated origin. Returns true and fills out if the entity should be
drawn somewhere else.
================
*/
qboolean CL_PP_GetOrigin (int entnum, vec3_t lerped, vec3_t out)
{
	ppstate_t	*p;
	vec3_t		raw, oldraw, target, in;
	float		lead;
	trace_t		tr;

	if (entnum < 1 || entnum > MAX_CLIENTS)
		return false;

	p = &pp_players[entnum - 1];

	if (!PP_Enabled ())
	{
		p->hasShown = false;
		PP_LogRecord (entnum, lerped, lerped, lerped);
		return false;
	}

	FastVectorCopy (*lerped, in);

	if (!p->tracked || p->serverframe != cl.frame.serverframe)
	{
		// not a live target in this frame (just died, usually). ease from the
		// forecast back to the stock position instead of popping.
		if (!p->hasShown || cls.realtime - p->shownTime > 500)
		{
			p->hasShown = false;
			return false;
		}

		if (!p->fallback)
		{
			p->fallback = true;
			VectorSubtract (p->shown, in, p->error);
			p->errorTime = cls.realtime;

			if (DotProduct (p->error, p->error) > PP_MAX_ERROR * PP_MAX_ERROR)
			{
				p->hasShown = false;
				return false;
			}
		}
		else
		{
			PP_DecayError (p);
		}

		VectorAdd (in, p->error, p->shown);
		p->shownTime = cls.realtime;
		PP_LogRecord (entnum, p->shown, in, in);
		FastVectorCopy (p->shown, *out);
		return true;
	}

	lead = PP_Lead (p->snap.servertime);
	PP_Simulate (&p->snap, lead, raw);

	if (p->newSnap)
	{
		// the forecast has a new base. whatever it moved by compared to what
		// is on screen becomes an error that is blended away.
		p->newSnap = false;

		if (p->reset || !p->hasShown || cls.realtime - p->shownTime > 500)
		{
			VectorClear (p->error);
		}
		else if (p->hasOld && !p->fallback)
		{
			// where the old base would put them right now
			PP_Simulate (&p->oldsnap, PP_Lead (p->oldsnap.servertime), oldraw);
			PP_DecayError (p);
			VectorAdd (oldraw, p->error, oldraw);
			VectorSubtract (oldraw, raw, p->error);
		}
		else
		{
			VectorSubtract (p->shown, raw, p->error);
		}

		if (DotProduct (p->error, p->error) > PP_MAX_ERROR * PP_MAX_ERROR)
			VectorClear (p->error);

		p->errorTime = cls.realtime;
		p->reset = false;
		p->fallback = false;
	}
	else
	{
		PP_DecayError (p);
	}

	if (p->error[0] || p->error[1] || p->error[2])
	{
		// the blend must not drag the model through a wall
		VectorAdd (raw, p->error, target);

		tr = PP_Trace (raw, p->snap.mins, p->snap.maxs, target, false);
		if (tr.startsolid || tr.allsolid)
		{
			VectorClear (p->error);
			FastVectorCopy (raw, p->shown);
		}
		else
		{
			FastVectorCopy (tr.endpos, p->shown);
			VectorSubtract (p->shown, raw, p->error);
		}
	}
	else
	{
		FastVectorCopy (raw, p->shown);
	}

	p->shownTime = cls.realtime;
	p->hasShown = true;

	if (PP_METERING && cls.realtime - p->sampleTime >= 10)
	{
		ppsample_t	*sm = &p->samples[p->sampleHead++ % PP_SAMPLES];

		p->sampleTime = cls.realtime;
		sm->targetTime = p->snap.servertime + (int)(lead * 1000.0f);
		FastVectorCopy (p->shown, sm->shown);
		FastVectorCopy (raw, sm->raw);
	}

	if (cl_predict_players_debug->intvalue == entnum || (cl_predict_players_debug->intvalue == -1 && cls.realtime - pp_debugTime >= 500))
	{
		pp_debugTime = cls.realtime;
		Com_Printf ("pp %d: rtt %.0f lead %.0f ms vel (%.0f %.0f %.0f) flags %d ahead %.1f err %.1f\n", LOG_CLIENT,
			entnum, pp_rtt, lead * 1000.0f,
			p->snap.velocity[0], p->snap.velocity[1], p->snap.velocity[2], p->snap.flags,
			(float)sqrt ((raw[0]-in[0])*(raw[0]-in[0]) + (raw[1]-in[1])*(raw[1]-in[1]) + (raw[2]-in[2])*(raw[2]-in[2])),
			VectorLength (p->error));
	}

	PP_LogRecord (entnum, p->shown, raw, in);

	FastVectorCopy (p->shown, *out);
	return true;
}

/*
================
CL_PP_GetShownOrigin

For sound spatialization: where the player was last drawn.
================
*/
qboolean CL_PP_GetShownOrigin (int entnum, vec3_t out)
{
	ppstate_t	*p;

	if (entnum < 1 || entnum > MAX_CLIENTS || entnum > cl.maxclients)
		return false;

	p = &pp_players[entnum - 1];

	if (!p->hasShown || cls.realtime - p->shownTime > 200 || !PP_Enabled ())
		return false;

	FastVectorCopy (p->shown, *out);
	return true;
}

/*
==========================================================================

SHOT LOG

cl_predict_players_log 1 writes a text file with one block per shot we
fire, for working out afterwards why shots hit or missed:

  - where we were aiming and what was on screen at that moment
  - where the server says everybody really was when the shot was resolved
  - whether the crosshair was on the drawn model, and whether the same
    shot would have touched the real player
  - blood / armor sparks the server reported on that player in that frame

A shot is known from the muzzle flash the server sends back for our own
entity. That message travels with the server frame in which the weapon
fired, so the player origins of that frame are where the targets were.

==========================================================================
*/

#define	PP_LOG_VIEWS		256
#define	PP_LOG_IMPACTS		128
#define	PP_LOG_TARGETS		32

typedef struct
{
	int			time;
	vec3_t		shown, raw, lerp;
} ppview_t;

typedef struct
{
	int			time;
	vec3_t		origin, angles;
} ppeye_t;

typedef struct
{
	int			serverframe;
	int			type;
	vec3_t		pos;
} ppimpact_t;

typedef struct
{
	int			ent;
	qboolean	hasView;
	vec3_t		truth, shown, raw, lerp, velocity;
	vec3_t		mins, maxs;
	int			flags;
} pptarget_t;

typedef struct
{
	qboolean	pending;
	int			serverframe, realtime, weapon, cmdnum, cmdage;
	qboolean	click, hasEye;
	vec3_t		eye, eyeAngles;					// what we rendered from
	vec3_t		srvEye, srvMuzzle, srvAngles;	// what the server fired from
	float		lead;
	int			numTargets;
	pptarget_t	targets[PP_LOG_TARGETS];
} ppshot_t;

static ppview_t		pp_views[MAX_CLIENTS][PP_LOG_VIEWS];
static int			pp_viewHead[MAX_CLIENTS];
static int			pp_viewTime[MAX_CLIENTS];
static ppeye_t		pp_eyes[PP_LOG_VIEWS];
static int			pp_eyeHead, pp_eyeTime;
static ppimpact_t	pp_impacts[PP_LOG_IMPACTS];
static int			pp_impactHead;
static ppshot_t		pp_shot;
static int			pp_ackNow, pp_ackPrev;

static void PP_LogRecord (int entnum, vec3_t shown, vec3_t raw, vec3_t lerp)
{
	ppview_t	*v;
	int			idx = entnum - 1;

	if (!pp_logFile)
		return;

	// a few ms apart is plenty and keeps a second of history
	if (pp_viewTime[idx] && (unsigned)(cls.realtime - pp_viewTime[idx]) < 4)
		return;

	pp_viewTime[idx] = cls.realtime;

	v = &pp_views[idx][pp_viewHead[idx]++ % PP_LOG_VIEWS];
	v->time = cls.realtime;
	FastVectorCopy (*shown, v->shown);
	FastVectorCopy (*raw, v->raw);
	FastVectorCopy (*lerp, v->lerp);
}

/*
================
CL_PP_RenderFrame

Called once per rendered frame after the view has been calculated.
================
*/
void CL_PP_RenderFrame (void)
{
	ppeye_t	*e;

	if (!pp_logFile)
		return;

	if (pp_eyeTime && (unsigned)(cls.realtime - pp_eyeTime) < 4)
		return;

	pp_eyeTime = cls.realtime;

	e = &pp_eyes[pp_eyeHead++ % PP_LOG_VIEWS];
	e->time = cls.realtime;
	FastVectorCopy (cl.refdef.vieworg, e->origin);
	FastVectorCopy (cl.refdef.viewangles, e->angles);
}

// newest entry that is not newer than time
static ppview_t *PP_LogFindView (int idx, int time)
{
	int			i;
	ppview_t	*v, *best = NULL;

	for (i = 0; i < PP_LOG_VIEWS; i++)
	{
		v = &pp_views[idx][i];
		if (!v->time || v->time > time || time - v->time > 150)
			continue;
		if (!best || v->time > best->time)
			best = v;
	}

	return best;
}

static ppeye_t *PP_LogFindEye (int time)
{
	int		i;
	ppeye_t	*e, *best = NULL;

	for (i = 0; i < PP_LOG_VIEWS; i++)
	{
		e = &pp_eyes[i];
		if (!e->time || e->time > time || time - e->time > 150)
			continue;
		if (!best || e->time > best->time)
			best = e;
	}

	return best;
}

static qboolean PP_RayHitsBox (vec3_t org, vec3_t dir, vec3_t origin, vec3_t mins, vec3_t maxs)
{
	int		i;
	float	tmin = 0, tmax = 8192, lo, hi, t1, t2, t;

	for (i = 0; i < 3; i++)
	{
		lo = origin[i] + mins[i] - org[i];
		hi = origin[i] + maxs[i] - org[i];

		if (dir[i] > -0.00001f && dir[i] < 0.00001f)
		{
			if (lo > 0 || hi < 0)
				return false;
			continue;
		}

		t1 = lo / dir[i];
		t2 = hi / dir[i];
		if (t1 > t2)
		{
			t = t1; t1 = t2; t2 = t;
		}

		if (t1 > tmin)
			tmin = t1;
		if (t2 < tmax)
			tmax = t2;

		if (tmin > tmax)
			return false;
	}

	return true;
}

// where a point is relative to a ray: distance along it, and how far right / up of it
static void PP_RayOffset (vec3_t org, vec3_t angles, vec3_t point, float *dist, float *right, float *up)
{
	vec3_t	f, r, u, d;

	AngleVectors (angles, f, r, u);
	VectorSubtract (point, org, d);

	*dist = DotProduct (d, f);
	*right = DotProduct (d, r);
	*up = DotProduct (d, u);
}

static void PP_LogOpen (void)
{
	char		path[MAX_OSPATH];
	time_t		now;
	struct tm	*t;

	time (&now);
	t = localtime (&now);

	Com_sprintf (path, sizeof(path), "%s/pplog/pp_%04d%02d%02d_%02d%02d%02d.log", FS_Gamedir (),
		t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);

	FS_CreatePath (path);

	pp_logFile = fopen (path, "w");
	if (!pp_logFile)
	{
		Com_Printf ("cl_predict_players_log: couldn't write %s\n", LOG_CLIENT, path);
		Cvar_Set ("cl_predict_players_log", "0");
		return;
	}

	memset (pp_views, 0, sizeof(pp_views));
	memset (pp_eyes, 0, sizeof(pp_eyes));
	memset (pp_impacts, 0, sizeof(pp_impacts));
	memset (&pp_shot, 0, sizeof(pp_shot));

	fprintf (pp_logFile,
		"# player prediction shot log\n"
		"# All positions are world units, x y z. A player is 32 wide and 56 tall.\n"
		"#\n"
		"# SHOT    one per shot the server confirmed (muzzle flash of our own entity)\n"
		"#   mz        muzzle flash id (0 blaster, 1 machinegun, 2 shotgun, 3-5 chaingun, 6 railgun,\n"
		"#             7 rocket, 8 grenade launcher, 13 super shotgun, 14 hyperblaster)\n"
		"#   click     1 = fired by a fresh button press, 0 = button was held\n"
		"#   cmdage    ms between sending the command that fired and hearing about the shot\n"
		"#   predict   cl_predict_players at the time, with rtt / lead / smooth / offset used\n"
		"#   eye/ang   what the screen was rendered from when that command was made\n"
		"#   srv_*     what the server fired from: eye, muzzle (hand offset), angles\n"
		"# TGT     one per other player in the server frame of the shot, nearest to the crosshair first\n"
		"#   true      where the server had them when the shot was resolved. Exact for click=0;\n"
		"#             for click=1 it is their position up to one server frame after the shot.\n"
		"#   drawn     where they were on screen;  raw = forecast before smoothing;  lerp = stock Quake 2 position\n"
		"#   aim_drawn crosshair relative to the middle of the drawn model: right, up (units), HIT if inside it\n"
		"#   aim_true  server shot line relative to the middle of the real player, HIT if it passes through\n"
		"#   err       drawn minus true: total, and along their direction of travel in units and ms\n"
		"#             (negative = drawn behind where they really were)\n"
		"#   lerp_err  the same for the stock position, for comparison\n"
		"#   blood/sparks  impacts the server reported inside the real player box in that frame\n"
		"# VERDICT for the target nearest the crosshair\n"
		"#   ON_TARGET          crosshair on the model and the shot line through the real player\n"
		"#   PREDICTION_MISS    crosshair on the model, real player was somewhere else\n"
		"#   AIM_MISS           crosshair not on the model, and not on the real player either\n"
		"#   OFF_MODEL_BUT_HIT  crosshair not on the model, but the shot line went through the real player\n"
		"#   confirmed=1        blood or sparks were reported on that player\n"
		"# ACC     once a second: average error of everything drawn (see cl_predict_players_debug -2)\n"
		"# TRK     every server frame, every visible player: frame, our clock, entity, origin,\n"
		"#         angles (pitch yaw), animation frame, ground/no-gravity flags, velocity used\n"
		"#\n");
	fflush (pp_logFile);

	Com_Printf ("Logging shots to %s\n", LOG_CLIENT, path);
}

static void PP_LogDescribe (pptarget_t *t, ppshot_t *sh, qboolean *aimDrawn, qboolean *aimTrue, float *angle)
{
	vec3_t	f, center;
	float	dist, right, up;

	*aimDrawn = false;
	*angle = 9999;

	center[0] = t->truth[0];
	center[1] = t->truth[1];
	center[2] = t->truth[2] + (t->mins[2] + t->maxs[2]) * 0.5f;

	AngleVectors (sh->srvAngles, f, NULL, NULL);
	*aimTrue = PP_RayHitsBox (sh->srvMuzzle, f, t->truth, t->mins, t->maxs);

	if (t->hasView && sh->hasEye)
	{
		AngleVectors (sh->eyeAngles, f, NULL, NULL);
		*aimDrawn = PP_RayHitsBox (sh->eye, f, t->shown, t->mins, t->maxs);

		center[0] = t->shown[0];
		center[1] = t->shown[1];
		center[2] = t->shown[2] + (t->mins[2] + t->maxs[2]) * 0.5f;
		PP_RayOffset (sh->eye, sh->eyeAngles, center, &dist, &right, &up);
	}
	else
	{
		PP_RayOffset (sh->srvEye, sh->srvAngles, center, &dist, &right, &up);
	}

	// how far off the crosshair, as an angle, for picking the intended target
	if (dist > 1)
		*angle = (float)sqrt (right * right + up * up) / dist;
}

static void PP_LogFlushShot (void)
{
	ppshot_t	*sh = &pp_shot;
	pptarget_t	*t;
	int			i, j, k, best, order[PP_LOG_TARGETS];
	int			blood[PP_LOG_TARGETS], sparks[PP_LOG_TARGETS];
	qboolean	aimDrawn[PP_LOG_TARGETS], aimTrue[PP_LOG_TARGETS];
	float		angle[PP_LOG_TARGETS];
	float		dist, right, up, speed, along, len;
	vec3_t		center, e, dir;
	ppimpact_t	*im;
	const char	*verdict;

	if (!sh->pending)
		return;

	sh->pending = false;

	if (!pp_logFile)
		return;

	fprintf (pp_logFile, "SHOT t=%d frame=%d mz=%d click=%d cmd=%d cmdage=%d predict=%d rtt=%.0f lead=%.0f smooth=%.0f offset=%.0f async=%d eye=%.1f %.1f %.1f ang=%.2f %.2f srv_eye=%.1f %.1f %.1f srv_muzzle=%.1f %.1f %.1f srv_ang=%.2f %.2f\n",
		sh->realtime, sh->serverframe, sh->weapon, sh->click, sh->cmdnum, sh->cmdage,
		PP_Enabled () ? 1 : 0, pp_rtt, sh->lead * 1000.0f,
		cl_predict_players_smooth->value, cl_predict_players_offset->value, cl_async->intvalue,
		sh->eye[0], sh->eye[1], sh->eye[2], sh->eyeAngles[0], sh->eyeAngles[1],
		sh->srvEye[0], sh->srvEye[1], sh->srvEye[2],
		sh->srvMuzzle[0], sh->srvMuzzle[1], sh->srvMuzzle[2],
		sh->srvAngles[0], sh->srvAngles[1]);

	for (i = 0; i < sh->numTargets; i++)
	{
		t = &sh->targets[i];
		order[i] = i;
		blood[i] = sparks[i] = 0;

		PP_LogDescribe (t, sh, &aimDrawn[i], &aimTrue[i], &angle[i]);

		// impacts of that server frame inside (or just outside) the real box
		for (j = 0; j < PP_LOG_IMPACTS; j++)
		{
			im = &pp_impacts[j];
			if (im->serverframe != sh->serverframe)
				continue;

			for (k = 0; k < 3; k++)
			{
				if (im->pos[k] < t->truth[k] + t->mins[k] - 12 || im->pos[k] > t->truth[k] + t->maxs[k] + 12)
					break;
			}

			if (k != 3)
				continue;

			if (im->type == TE_BLOOD || im->type == TE_MOREBLOOD || im->type == TE_GREENBLOOD)
				blood[i]++;
			else if (im->type == TE_SPARKS || im->type == TE_BULLET_SPARKS || im->type == TE_SCREEN_SPARKS || im->type == TE_SHIELD_SPARKS)
				sparks[i]++;
		}
	}

	// nearest to the crosshair first
	for (i = 0; i < sh->numTargets; i++)
	{
		for (j = i + 1; j < sh->numTargets; j++)
		{
			if (angle[order[j]] < angle[order[i]])
			{
				k = order[i]; order[i] = order[j]; order[j] = k;
			}
		}
	}

	for (i = 0; i < sh->numTargets; i++)
	{
		k = order[i];
		t = &sh->targets[k];

		speed = (float)sqrt (t->velocity[0] * t->velocity[0] + t->velocity[1] * t->velocity[1]);
		dir[0] = dir[1] = 0;
		if (speed > 1)
		{
			dir[0] = t->velocity[0] / speed;
			dir[1] = t->velocity[1] / speed;
		}

		center[0] = t->truth[0];
		center[1] = t->truth[1];
		center[2] = t->truth[2] + (t->mins[2] + t->maxs[2]) * 0.5f;
		PP_RayOffset (sh->srvMuzzle, sh->srvAngles, center, &dist, &right, &up);

		fprintf (pp_logFile, " TGT ent=%d name=\"%s\" range=%.0f true=%.1f %.1f %.1f vel=%.0f %.0f %.0f flags=%d aim_true=%+.1f %+.1f %s blood=%d sparks=%d",
			t->ent, cl.clientinfo[t->ent - 1].name, dist,
			t->truth[0], t->truth[1], t->truth[2],
			t->velocity[0], t->velocity[1], t->velocity[2], t->flags,
			-right, -up, aimTrue[k] ? "HIT" : "MISS", blood[k], sparks[k]);

		if (t->hasView)
		{
			center[0] = t->shown[0];
			center[1] = t->shown[1];
			center[2] = t->shown[2] + (t->mins[2] + t->maxs[2]) * 0.5f;
			PP_RayOffset (sh->eye, sh->eyeAngles, center, &dist, &right, &up);

			VectorSubtract (t->shown, t->truth, e);
			len = VectorLength (e);
			along = e[0] * dir[0] + e[1] * dir[1];

			fprintf (pp_logFile, " drawn=%.1f %.1f %.1f raw=%.1f %.1f %.1f lerp=%.1f %.1f %.1f aim_drawn=%+.1f %+.1f %s err=%.1f along=%+.1f ms=%+.0f",
				t->shown[0], t->shown[1], t->shown[2],
				t->raw[0], t->raw[1], t->raw[2],
				t->lerp[0], t->lerp[1], t->lerp[2],
				-right, -up, aimDrawn[k] ? "HIT" : "MISS",
				len, along, speed > 50 ? along / speed * 1000.0f : 0.0f);

			VectorSubtract (t->lerp, t->truth, e);
			len = VectorLength (e);
			along = e[0] * dir[0] + e[1] * dir[1];

			fprintf (pp_logFile, " lerp_err=%.1f lerp_along=%+.1f lerp_ms=%+.0f",
				len, along, speed > 50 ? along / speed * 1000.0f : 0.0f);
		}
		else
		{
			fprintf (pp_logFile, " drawn=none");
		}

		fprintf (pp_logFile, "\n");
	}

	if (!sh->numTargets || angle[order[0]] > 0.35f)
	{
		fprintf (pp_logFile, " VERDICT NO_TARGET\n");
	}
	else
	{
		best = order[0];

		if (aimDrawn[best])
			verdict = aimTrue[best] ? "ON_TARGET" : "PREDICTION_MISS";
		else
			verdict = aimTrue[best] ? "OFF_MODEL_BUT_HIT" : "AIM_MISS";

		fprintf (pp_logFile, " VERDICT %s ent=%d confirmed=%d\n", verdict, sh->targets[best].ent,
			(blood[best] || sparks[best]) ? 1 : 0);
	}

	fflush (pp_logFile);
}

/*
================
CL_PP_LogImpact

A bullet impact effect was received: blood, armor sparks, wall hits.
================
*/
void CL_PP_LogImpact (int type, vec3_t pos)
{
	ppimpact_t	*im;

	if (!pp_logFile)
		return;

	im = &pp_impacts[pp_impactHead++ % PP_LOG_IMPACTS];
	im->serverframe = cl.frame.serverframe;
	im->type = type;
	FastVectorCopy (*pos, im->pos);
}

/*
================
CL_PP_LogShot

The server says our own weapon fired in the frame we just received.
================
*/
void CL_PP_LogShot (int weapon)
{
	ppshot_t		*sh = &pp_shot;
	pptarget_t		*t;
	player_state_t	*ps;
	usercmd_t		*cmd;
	entity_state_t	*s;
	ppview_t		*v;
	ppeye_t			*eye;
	vec3_t			f, r;
	int				i, c, x, zd, zu, cmdtime, side;

	if (!pp_logFile || cls.state != ca_active || cl.attractloop)
		return;

	PP_LogFlushShot ();

	memset (sh, 0, sizeof(*sh));
	sh->pending = true;
	sh->serverframe = cl.frame.serverframe;
	sh->realtime = cls.realtime;
	sh->weapon = weapon;

	// which of our commands fired it? the server ran everything up to the
	// acknowledged one before this frame. a held button fires after the last
	// of them, a fresh press fires on the command that brought it.
	sh->cmdnum = pp_ackNow;

	if (pp_ackNow - pp_ackPrev > 0 && pp_ackNow - pp_ackPrev < CMD_BACKUP - 1)
	{
		for (c = pp_ackPrev + 1; c <= pp_ackNow; c++)
		{
			if ((cl.cmds[c & (CMD_BACKUP-1)].buttons & BUTTON_ATTACK) &&
				!(cl.cmds[(c - 1) & (CMD_BACKUP-1)].buttons & BUTTON_ATTACK))
			{
				sh->cmdnum = c;
				sh->click = true;
				break;
			}
		}
	}

	cmd = &cl.cmds[sh->cmdnum & (CMD_BACKUP-1)];
	cmdtime = cl.cmd_time[sh->cmdnum & (CMD_BACKUP-1)];
	sh->cmdage = cls.realtime - cmdtime;

	// the server side of the shot
	ps = &cl.frame.playerstate;

	for (i = 0; i < 3; i++)
	{
		sh->srvEye[i] = ps->pmove.origin[i] * 0.125f + ps->viewoffset[i];
		sh->srvAngles[i] = SHORT2ANGLE (cmd->angles[i]) + SHORT2ANGLE (ps->pmove.delta_angles[i]);
	}

	// bullets leave from the gun hand, 8 to the side and 8 below the eye
	side = 1;
	if (hand->intvalue == 1)
		side = -1;
	else if (hand->intvalue == 2)
		side = 0;

	AngleVectors (sh->srvAngles, f, r, NULL);
	VectorMA (sh->srvEye, 8.0f * side, r, sh->srvMuzzle);
	sh->srvMuzzle[2] -= 8;

	// our side of it
	eye = PP_LogFindEye (cmdtime);
	if (eye)
	{
		sh->hasEye = true;
		FastVectorCopy (eye->origin, sh->eye);
		FastVectorCopy (eye->angles, sh->eyeAngles);
	}

	sh->lead = PP_Lead (cl.frame.servertime);

	for (i = 0; i < cl.frame.num_entities && sh->numTargets < PP_LOG_TARGETS; i++)
	{
		s = &cl_parse_entities[(cl.frame.parse_entities + i) & (MAX_PARSE_ENTITIES-1)];

		if (s->number < 1 || s->number > cl.maxclients || s->number > MAX_CLIENTS)
			continue;

		if (s->number == cl.playernum + 1 || !s->solid || s->solid == 31 || !s->modelindex)
			continue;

		t = &sh->targets[sh->numTargets++];
		t->ent = s->number;
		FastVectorCopy (s->origin, t->truth);

		if (cls.protocolVersion >= MINOR_VERSION_R1Q2_32BIT_SOLID)
		{
			x = (s->solid & 255);
			zd = ((s->solid>>8) & 255);
			zu = ((s->solid>>16) & 65535) - 32768;
		}
		else
		{
			x = 8*(s->solid & 31);
			zd = 8*((s->solid>>5) & 31);
			zu = 8*((s->solid>>10) & 63) - 32;
		}

		t->mins[0] = t->mins[1] = -(float)x;
		t->maxs[0] = t->maxs[1] = (float)x;
		t->mins[2] = -(float)zd;
		t->maxs[2] = (float)zu;

		if (pp_players[s->number - 1].tracked && pp_players[s->number - 1].serverframe == cl.frame.serverframe)
		{
			FastVectorCopy (pp_players[s->number - 1].snap.velocity, t->velocity);
			t->flags = pp_players[s->number - 1].snap.flags;
		}

		v = PP_LogFindView (s->number - 1, cmdtime);
		if (v)
		{
			t->hasView = true;
			FastVectorCopy (v->shown, t->shown);
			FastVectorCopy (v->raw, t->raw);
			FastVectorCopy (v->lerp, t->lerp);
		}
	}
}

/*
================
PP_LogFrame

Called for every valid server frame, before anything else in it is parsed.
================
*/
static void PP_LogFrame (void)
{
	if (cl_predict_players_log->intvalue && !pp_logFile)
		PP_LogOpen ();

	// the shot of the previous frame has all its impacts by now
	PP_LogFlushShot ();

	if (!cl_predict_players_log->intvalue && pp_logFile)
	{
		fclose (pp_logFile);
		pp_logFile = NULL;
		Com_Printf ("Shot log closed.\n", LOG_CLIENT);
	}

	pp_ackPrev = pp_ackNow;
	pp_ackNow = cls.netchan.incoming_acknowledged;
}

/*
=================
CL_PP_CommandTime

The server time (ms) at which a command sent right now will be run, and
the round trip time that estimate is based on. False until both are known.
=================
*/
qboolean CL_PP_CommandTime (double *serverTime, float *rtt)
{
	if (!pp_clockValid || !pp_rttCount)
		return false;

	*serverTime = ((double)cls.realtime - pp_clockOffset) + (double)pp_rtt;
	*rtt = pp_rtt;
	return true;
}

/*
=================
CL_PP_ShotTrace

Where a shot from start towards end is stopped by the map: the world and
its doors, lifts and other brush models. Players do not stop it.
=================
*/
void CL_PP_ShotTrace (vec3_t start, vec3_t end, vec3_t out)
{
	trace_t			t, trace;
	int				i;
	entity_state_t	*ent;
	cmodel_t		*cmodel;
	const int		mask = CONTENTS_SOLID|CONTENTS_WINDOW;

	t = CM_BoxTrace (start, end, vec3_origin, vec3_origin, 0, mask);

	for (i = 0; i < pp_numBmodels; i++)
	{
		ent = pp_bmodels[i];
		cmodel = cl.model_clip[ent->modelindex];
		if (!cmodel)
			continue;

		trace = CM_TransformedBoxTrace (start, end, vec3_origin, vec3_origin, cmodel->headnode,
			mask, ent->origin, ent->angles);

		if (trace.fraction < t.fraction)
			t = trace;
	}

	FastVectorCopy (t.endpos, *out);
}

/*
=================
CL_PP_Active

Is prediction of other players in use right now? (switched on, in a
multiplayer game, and the ping is high enough)
=================
*/
qboolean CL_PP_Active (void)
{
	return PP_Enabled ();
}
