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

cvar_t	*cl_predict_players;
cvar_t	*cl_predict_players_maxping;
cvar_t	*cl_predict_players_smooth;
cvar_t	*cl_predict_players_offset;
cvar_t	*cl_predict_players_debug;

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
static int				pp_lastServertime;
static int				pp_debugTime;

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
	pp_lastServertime = 0;
}

/*
================
CL_PP_Init
================
*/
void CL_PP_Init (void)
{
	cl_predict_players = Cvar_Get ("cl_predict_players", "1", CVAR_ARCHIVE);
	cl_predict_players_maxping = Cvar_Get ("cl_predict_players_maxping", "300", CVAR_ARCHIVE);
	cl_predict_players_smooth = Cvar_Get ("cl_predict_players_smooth", "70", CVAR_ARCHIVE);
	cl_predict_players_offset = Cvar_Get ("cl_predict_players_offset", "0", CVAR_ARCHIVE);
	cl_predict_players_debug = Cvar_Get ("cl_predict_players_debug", "0", 0);

	CL_PP_Reset ();
}

static qboolean PP_Enabled (void)
{
	if (!cl_predict_players->intvalue || !cl_predict->intvalue)
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

	if (cl_predict_players_debug->intvalue == entnum || (cl_predict_players_debug->intvalue == -1 && cls.realtime - pp_debugTime >= 500))
	{
		pp_debugTime = cls.realtime;
		Com_Printf ("pp %d: rtt %.0f lead %.0f ms vel (%.0f %.0f %.0f) flags %d ahead %.1f err %.1f\n", LOG_CLIENT,
			entnum, pp_rtt, lead * 1000.0f,
			p->snap.velocity[0], p->snap.velocity[1], p->snap.velocity[2], p->snap.flags,
			(float)sqrt ((raw[0]-in[0])*(raw[0]-in[0]) + (raw[1]-in[1])*(raw[1]-in[1]) + (raw[2]-in[2])*(raw[2]-in[2])),
			VectorLength (p->error));
	}

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
