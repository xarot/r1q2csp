//
// cl_csp.c - Client-Side Prediction (Ghost) for high-latency Quake2 play
//
// Phase 1: Accuracy baseline — stateless prediction from authoritative
// server timestamps. Phase 2 decaying visual offset enabled by csp_smooth.
//
// Prediction formula:
//   predicted = snapshot_origin + velocity * horizon
//   horizon   = snapshot_age + (ack_rtt_ms * csp_mult / 2.0 / 1000.0) + csp_horizon_bias
//   velocity  = (origin_now - origin_prev) / ((servertime_now - servertime_prev) / 1000.0)
//
// Phase 2 smoothing (csp_smooth 1):
//   On new snapshot: smooth_offset = old_prediction - new_prediction
//   Each render frame: out_origin = prediction + smooth_offset * exp(-t / tau)
//   Diagnostics/raw prediction remain unaffected.
//
// See step0point5_report.md for timing architecture verification.
//

#include "client.h"
#include "cl_csp.h"
#include <math.h>

//
// Per-entity snapshot history — stores 2 consecutive authoritative snapshots
// for velocity computation and prediction.
//
#define CSP_MAX_ENTITIES 128

#define CSP_SMOOTH_TAU_DEFAULT 80  // default smoothing time constant in ms

//
// Reset reasons — why a velocity/smoothing reseed occurred.
// Used for diagnostics; an UNKNOWN value is preferred over inventing a
// specific cause when the true reason cannot be established.
//
typedef enum {
	CSP_RESET_NONE = 0,
	CSP_RESET_ENTITY_ABSENT,      // entity missing from the immediately prior frame
	CSP_RESET_SERVERFRAME_SKIPPED,// serverframe gap (dropped intermediate frame)
	CSP_RESET_TELEPORT_DISP,      // displacement exceeded plausible velocity
	CSP_RESET_INVALID_DT,         // non-positive server-time delta
	CSP_RESET_VELOCITY_CLEARED,   // velocity cleared by another path
	CSP_RESET_UNKNOWN             // reseeded, cause not established
} csp_reset_reason_t;

typedef struct {
	// Previous snapshot (for velocity calc)
	int prev_servertime;        // cl.frame.servertime of previous snapshot
	int prev_serverframe;       // cl.frame.serverframe of previous snapshot
	float prev_origin[3];       // entity origin at previous snapshot

	// Current snapshot (latest authoritative)
	int cur_servertime;         // cl.frame.servertime of current snapshot
	int cur_serverframe;        // cl.frame.serverframe of current snapshot
	float cur_origin[3];        // entity origin at current snapshot

	// Local receive timestamp (wall-clock ms at CL_ParseFrame time)
	int cur_receive_ms;         // Sys_Milliseconds when current snapshot was received

	// Current velocity (computed from prev/cur servertime delta)
	float velocity[3];

	// State flags
	qboolean initialized;       // have we received at least one snapshot
	qboolean has_velocity;      // have we computed a valid velocity (need 2 snapshots)
	int seen_count;             // number of snapshots received (for diagnostics)

	// Phase 2 smoothing state (disabled when csp_smooth == 0)
	float smooth_offset[3];     // decaying visual offset vector
	float smooth_offset_time;   // Sys_Milliseconds when offset was captured
	qboolean smooth_active;     // decay is in progress for this entity

	// Actual last displayed ghost position (for smoothing continuity).
	// Captured at render time from the position actually sent to the
	// renderer — NOT a reconstruction from cur_origin + velocity*horizon.
	float last_rendered_ghost[3];
	int   last_rendered_time;   // cls.realtime when last ghost was rendered
	qboolean last_rendered_valid;
	int   smooth_captured_for_frame;  // serverframe for which offset was captured

	// Snapshot continuity tracking (Req 1).
	// Presence and drop-counter deltas let us classify missing frames
	// accurately instead of assuming a single generic reason.
	int   prev_present;         // entity present in previous snapshot (0/1)
	int   cur_present;          // entity present in current snapshot (0/1)
	int   prev_dropped;         // cls.netchan.dropped at previous snapshot
	int   cur_dropped;          // cls.netchan.dropped at current snapshot

	// Last reset reason (diagnostics)
	csp_reset_reason_t last_reset_reason;
} csp_entity_state_t;

static csp_entity_state_t csp_entities[CSP_MAX_ENTITIES];

//
// Acknowledged-command RTT tracking — captured from cl_scrn.c ping measurement
// We sample the raw RTT each frame and maintain a light EMA for stability.
//
static float csp_ack_rtt_ms = 0.0f;       // smoothed RTT estimate in ms
static qboolean csp_has_rtt = false;     // have we received an RTT sample

// Extern declarations for client state
extern client_state_t cl;
extern client_static_t cls;

//
// Phase 1 is the default and only mode. Phase 2 smoothing is gated by
// csp_smooth (default 0 = disabled) and must not affect underlying
// prediction or diagnostics.
//
// Debug logging state
static FILE *csp_debug_fp = NULL;
static int csp_debug_frame = 0;

/*
====================
CL_CSP_ValueChanged

Called when csp_enable cvar changes.
====================
*/
void CL_CSP_ValueChanged (struct cvar_s *self, char *oldValue, char *newValue)
{
	if (csp_enable->intvalue < 0 || csp_enable->intvalue > 2)
	{
		Cvar_SetValue ("csp_enable", 1);
		Com_Printf ("CSP mode reset to 1 (ghost both)\n", LOG_CLIENT);
	}
}

/*
====================
CL_CSP_ClearState

Reset all entity state (e.g. on level change, disconnect).
====================
*/
void CL_CSP_ClearState (void)
{
	int i;

	csp_ack_rtt_ms = 0.0f;
	csp_has_rtt = false;

	if (csp_debug_fp)
	{
		fclose (csp_debug_fp);
		csp_debug_fp = NULL;
	}
	csp_debug_frame = 0;
	for (i = 0; i < CSP_MAX_ENTITIES; i++)
	{
		csp_entities[i].initialized = false;
		csp_entities[i].has_velocity = false;
		csp_entities[i].seen_count = 0;
		csp_entities[i].prev_servertime = 0;
		csp_entities[i].cur_servertime = 0;
		csp_entities[i].prev_serverframe = 0;
		csp_entities[i].cur_serverframe = 0;
		csp_entities[i].cur_receive_ms = 0;
		VectorClear (csp_entities[i].prev_origin);
		VectorClear (csp_entities[i].cur_origin);
		VectorClear (csp_entities[i].velocity);
		VectorClear (csp_entities[i].smooth_offset);
		csp_entities[i].smooth_offset_time = 0;
		csp_entities[i].smooth_active = false;
		VectorClear (csp_entities[i].last_rendered_ghost);
		csp_entities[i].last_rendered_time = 0;
		csp_entities[i].last_rendered_valid = false;
		csp_entities[i].smooth_captured_for_frame = 0;
		csp_entities[i].prev_present = 0;
		csp_entities[i].cur_present = 0;
		csp_entities[i].prev_dropped = 0;
		csp_entities[i].cur_dropped = 0;
		csp_entities[i].last_reset_reason = CSP_RESET_NONE;
	}
}

/*
====================
CL_CSP_ResetSmoothing

Clear the decaying visual smoothing state for an entity.

Called from every reseed path (entity absent, serverframe skipped,
teleport displacement, invalid dt, velocity cleared) and from
CL_CSP_ClearState. This ensures a stale smoothing offset from a
previous movement segment is never carried across a discontinuity.

The raw prediction state (velocity, origin) is NOT touched here —
only the visual smoothing layer is reset.
====================
*/
static void CL_CSP_ResetSmoothing (csp_entity_state_t *st)
{
	VectorClear (st->smooth_offset);
	st->smooth_offset_time = 0;
	st->smooth_active = false;
}


//
// Limits and constants
//
#define CSP_MAX_SNAP_AGE      250.0f    // ms — max snapshot age before clamp
#define CSP_MAX_DT            500       // ms — max server-time delta between snapshots
#define CSP_MAX_DISP_VEL      1500.0f   // units/s — max plausible velocity for reseed
#define CSP_MAX_JUMP          3000.0f   // units/s — max velocity magnitude (clamp)
#define CSP_RTT_EMA_ALPHA     0.1f     // smoothing alpha for RTT tracker
#define CSP_SNAP_INIT         2        // min snapshots before velocity is valid

//
// Per-frame timestamp capture — set by CL_CSP_ParseFrameNotify()
//
static int csp_snapshot_receive_ms = 0;
static int csp_snapshot_serverframe = 0;

//
// Extern cvar declarations — registered in cl_main.c
//
extern cvar_t *csp_enable;
extern cvar_t *csp_latency_ms;
extern cvar_t *csp_mult;
extern cvar_t *csp_horizon_ms;
extern cvar_t *csp_horizon_bias;
extern cvar_t *csp_debug;
extern cvar_t *csp_debug_file;

// ====================================================================
// RTT Tracking
// ====================================================================

/*
====================
CL_CSP_UpdateRTT

Feed a raw RTT measurement (in milliseconds) from cl_scrn.c.
Maintains a light EMA for stability.

Called each time an acknowledged-command RTT sample is available.
rtt_ms = cls.realtime - cl.cmd_time[incoming_acknowledged]
====================
*/
void CL_CSP_UpdateRTT (float rtt_ms)
{
	// Clamp to safe range
	if (rtt_ms < 5.0f) rtt_ms = 5.0f;
	if (rtt_ms > 2000.0f) rtt_ms = 2000.0f;

	if (!csp_has_rtt)
	{
		csp_ack_rtt_ms = rtt_ms;
		csp_has_rtt = true;
	}
	else
	{
		csp_ack_rtt_ms = (1.0f - CSP_RTT_EMA_ALPHA) * csp_ack_rtt_ms
		              + CSP_RTT_EMA_ALPHA * rtt_ms;
	}
}

// ====================================================================
// Snapshot Timestamp Capture
// ====================================================================

/*
====================
CL_CSP_ParseFrameNotify

Called at the top of CL_ParseFrame (in cl_ents.c) to record the local
wall-clock receive timestamp for the incoming snapshot.

This associates the receive time with the corresponding serverframe
and servertime for the duration of the frame parse.
====================
*/
void CL_CSP_ParseFrameNotify (void)
{
	// Guard: only track timestamps when we have an active game state.
	// During map loading (especially with downloaded maps like OpenFFA),
	// frame data may arrive before client state is fully initialized.
	if (cls.state != ca_active)
		return;

	csp_snapshot_receive_ms = cls.realtime;
	csp_snapshot_serverframe = cl.frame.serverframe;

	// Auto-open debug file at level 2
	if (csp_debug->intvalue >= 2 && !csp_debug_fp)
	{
		csp_debug_fp = fopen (csp_debug_file->string, "w");
		if (csp_debug_fp)
		{
			csp_debug_frame = 0;
			fprintf (csp_debug_fp,
				"frame,"
				"local_receive_ms,serverframe,servertime,"
				"ent_num,"
				"snap_origin_x,snap_origin_y,snap_origin_z,"
				"prev_origin_x,prev_origin_y,prev_origin_z,"
				"velocity_x,velocity_y,velocity_z,"
				"dt_ms,snapshot_age_ms,"
				"rtt_ms,rtt_source,mult,horizon_bias,"
				"horizon_ms,"
				"predicted_x,predicted_y,predicted_z,"
				"rendered_x,rendered_y,rendered_z,"
				"diff_x,diff_y,diff_z,diff_mag,"
				"valid,reseeded,reseed_reason,snap_age_clamped\n");
			fflush (csp_debug_fp);
		}
	}
}

// ====================================================================
// Velocity Update (Idempotent per snapshot)
// ====================================================================

/*
====================
CL_CSP_UpdateEntityVelocity

Update the entity's velocity estimate from authoritative snapshots.

velocity = (cur_origin - prev_origin) / ((cur_servertime - prev_servertime) / 1000.0)
Uses server timestamps, NOT client wall-clock arrival intervals.

Idempotent: called once per entity per frame — early returns if already
updated for this serverframe.
====================
*/
static void CL_CSP_UpdateEntityVelocity (centity_t *cent, int ent_num)
{
	csp_entity_state_t *st = &csp_entities[ent_num];
	int dt;
	float dx, dy, dz;

	// Guard: don't update if frame data is not ready
	if (cl.frame.servertime == 0 && cl.frame.serverframe == 0)
		return;

	// New snapshot detection via serverframe — idempotent
	if (st->cur_serverframe == cl.frame.serverframe)
		return;  // already updated this frame

	// Record snapshot continuity (Req 1): presence + drop counter deltas.
	// These are recorded BEFORE the shift so prev_* reflects the prior
	// snapshot's state.
	st->prev_present = st->cur_present;
	st->cur_present  = 1;
	st->prev_dropped = st->cur_dropped;
	st->cur_dropped  = cls.netchan.dropped;

	// Shift current -> prev for velocity calc (only if we have data)
	if (st->initialized)
	{
		st->prev_servertime = st->cur_servertime;
		st->prev_serverframe = st->cur_serverframe;
		VectorCopy (st->cur_origin, st->prev_origin);
	}
	else
	{
		// First sight — init prev/cur from entity
		st->initialized = true;
		st->seen_count = 1;
		VectorCopy (cent->current.origin, st->cur_origin);
		VectorCopy (cent->current.origin, st->prev_origin);
		st->cur_servertime = cl.frame.servertime;
		st->prev_servertime = cl.frame.servertime;
		st->cur_serverframe = cl.frame.serverframe;
		st->prev_serverframe = cl.frame.serverframe;
		st->cur_receive_ms = csp_snapshot_receive_ms;
		st->has_velocity = false;
		VectorClear (st->velocity);
		return;
	}

	// Calculate dt from authoritative server timestamps
	dt = cl.frame.servertime - st->prev_servertime;

	// Invalid time delta — reset velocity
	if (dt <= 0)
	{
		st->has_velocity = false;
		VectorClear (st->velocity);
		CL_CSP_ResetSmoothing (st);
		st->last_reset_reason = CSP_RESET_INVALID_DT;
		st->cur_receive_ms = csp_snapshot_receive_ms;
		st->cur_servertime = cl.frame.servertime;
		st->cur_serverframe = cl.frame.serverframe;
		VectorCopy (cent->current.origin, st->cur_origin);
		st->seen_count++;
		return;
	}

	// Too large gap — likely respawn/disconnect
	if (dt > CSP_MAX_DT)
	{
		st->has_velocity = false;
		VectorClear (st->velocity);
		CL_CSP_ResetSmoothing (st);
		st->last_reset_reason = CSP_RESET_SERVERFRAME_SKIPPED;
		VectorCopy (st->cur_origin, st->prev_origin);
		st->prev_servertime = st->cur_servertime;
		st->prev_serverframe = st->cur_serverframe;
		st->cur_receive_ms = csp_snapshot_receive_ms;
		st->cur_servertime = cl.frame.servertime;
		st->cur_serverframe = cl.frame.serverframe;
		VectorCopy (cent->current.origin, st->cur_origin);
		st->seen_count++;
		return;
	}

	// Calculate displacement: new snapshot origin minus current stored origin
	// (st->cur_origin holds the PREVIOUS snapshot's position at this point)
	// Use cent->current.origin (current entity state from latest snapshot)
	dx = cent->current.origin[0] - st->cur_origin[0];
	dy = cent->current.origin[1] - st->cur_origin[1];
	dz = cent->current.origin[2] - st->cur_origin[2];

	{
		float disp = sqrtf (dx*dx + dy*dy + dz*dz);
		float max_disp = CSP_MAX_DISP_VEL * ((float)dt / 1000.0f);

		if (disp > max_disp)
		{
			// Likely teleport — zero velocity, reseed prev at current
			st->has_velocity = false;
			VectorClear (st->velocity);
			CL_CSP_ResetSmoothing (st);
			st->last_reset_reason = CSP_RESET_TELEPORT_DISP;
			// Reseed: set prev = current (so next delta starts fresh)
			VectorCopy (st->cur_origin, st->prev_origin);
			st->prev_servertime = st->cur_servertime;
			st->prev_serverframe = st->cur_serverframe;
			st->cur_receive_ms = csp_snapshot_receive_ms;
			st->cur_servertime = cl.frame.servertime;
			st->cur_serverframe = cl.frame.serverframe;
			VectorCopy (cent->current.origin, st->cur_origin);
			st->seen_count++;
			return;
		}
	}

	// Calculate velocity from authoritative timestamps
	{
		float dt_sec = (float)dt / 1000.0f;

		st->velocity[0] = dx / dt_sec;
		st->velocity[1] = dy / dt_sec;
		// Vertical velocity: clamp to reasonable max to avoid flying ghosts
		st->velocity[2] = dz / dt_sec;
		st->has_velocity = true;
	}

	// Clamp velocity magnitude (safety against extreme jumps)
	{
		float vlen = sqrtf (
			st->velocity[0]*st->velocity[0] +
			st->velocity[1]*st->velocity[1] +
			st->velocity[2]*st->velocity[2]
		);
		if (vlen > CSP_MAX_JUMP)
		{
			float scale = CSP_MAX_JUMP / vlen;
			st->velocity[0] *= scale;
			st->velocity[1] *= scale;
			st->velocity[2] *= scale;
		}
	}

	// Update current snapshot data from entity
	VectorCopy (cent->current.origin, st->cur_origin);
	st->cur_servertime = cl.frame.servertime;
	st->cur_serverframe = cl.frame.serverframe;
	st->cur_receive_ms = csp_snapshot_receive_ms;
	st->seen_count++;
	st->last_reset_reason = CSP_RESET_NONE;  // successful update
}

/*
====================
CL_CSP_ResetReasonString

Return a human-readable string for a reset reason enum value.
Used in debug logging and diagnostics.
====================
*/
static const char *CL_CSP_ResetReasonString (csp_reset_reason_t reason)
{
	switch (reason)
	{
		case CSP_RESET_NONE:                return "none";
		case CSP_RESET_ENTITY_ABSENT:       return "entity_absent";
		case CSP_RESET_SERVERFRAME_SKIPPED: return "serverframe_skipped";
		case CSP_RESET_TELEPORT_DISP:       return "teleport_disp";
		case CSP_RESET_INVALID_DT:          return "invalid_dt";
		case CSP_RESET_VELOCITY_CLEARED:    return "velocity_cleared";
		case CSP_RESET_UNKNOWN:             return "unknown";
		default:                            return "unknown";
	}
}

// ====================================================================
// RTT Access
// ====================================================================

/*
====================
CL_CSP_GetRTTMs

Returns the current RTT estimate in milliseconds. Priority order:
  1. csp_latency_ms if > 0 (user override, explicit RTT in ms)
  2. Smoothed ack_rtt_ms from RTT tracker (from CL_CSP_UpdateRTT)
  3. Fixed 150 ms fallback
  
IMPORTANT: Do NOT use (cl.time - cl.frame.servertime) as a latency proxy.
Step 0 investigation confirmed this value is cl.interpolation_amount *
cl.frametime (roughly 100ms fixed), NOT actual network latency.
====================
*/
static float CL_CSP_GetRTTMs (void)
{
	if (csp_latency_ms->intvalue > 0)
		return (float)csp_latency_ms->intvalue;

	if (csp_has_rtt && (csp_ack_rtt_ms > 0.0f))
		return csp_ack_rtt_ms;

	return 150.0f;  // fixed fallback for unconnected/early-game
}

// ====================================================================
// Horizon Calculation
// ====================================================================

/*
====================
CL_CSP_CalculateHorizon

Compute the prediction horizon in milliseconds.

horizon = snapshot_age + (RTT * csp_mult / 2.0) + csp_horizon_bias

Under symmetric network assumption:
- snapshot_age includes downstream travel time already
- RTT/2 estimates upstream delay to server
- Commands execute immediately on arrival (no server scheduling delay)

If csp_horizon_ms > 0: returns that fixed value instead.
====================
*/
static float CL_CSP_CalculateHorizon (csp_entity_state_t *st, qboolean *age_clamped)
{
	float snapshot_age_ms;
	float rtt_ms;

	*age_clamped = false;

	if (csp_horizon_ms->value > 0.0f)
		return csp_horizon_ms->value;

	snapshot_age_ms = (float)cls.realtime - (float)st->cur_receive_ms;

	// Negative snapshot age can happen if clock jumps — clamp to 0
	if (snapshot_age_ms < 0.0f)
		snapshot_age_ms = 0.0f;

	// Clamp — frequent activation indicates networking issues
	if (snapshot_age_ms > CSP_MAX_SNAP_AGE)
	{
		*age_clamped = true;
		snapshot_age_ms = CSP_MAX_SNAP_AGE;
	}

	rtt_ms = CL_CSP_GetRTTMs ();

	return snapshot_age_ms
		+ rtt_ms * csp_mult->value / 2.0f
		+ csp_horizon_bias->value;
}

// ====================================================================
// Debug Logging
// ====================================================================

/*
====================
CL_CSP_DebugLog

Log one row of prediction CSV per entity per snapshot.
Only fires when csp_debug >= 1. Console prints at level 1, file CSV at level 2.
====================
*/
static void CL_CSP_DebugLog (
	int ent_num, vec3_t predicted, vec3_t rendered,
	csp_entity_state_t *st, float rtt_ms, float horizon_ms,
	qboolean reseeded, const char *reseed_reason, qboolean age_clamped)
{
	float dx, dy, dz, diff_mag;

	dx = predicted[0] - rendered[0];
	dy = predicted[1] - rendered[1];
	dz = predicted[2] - rendered[2];
	diff_mag = sqrtf (dx*dx + dy*dy + dz*dz);

	// CSV to file at level 2
	if (csp_debug->intvalue >= 2 && csp_debug_fp)
	{
		csp_debug_frame++;

		fprintf (csp_debug_fp,
			"%d,"           // frame
			"%d,%d,%d,"     // local_receive_ms, serverframe, servertime
			"%d,"           // ent_num
			"%.1f,%.1f,%.1f,"   // snap_origin
			"%.1f,%.1f,%.1f,"   // prev_origin
			"%.2f,%.2f,%.2f,"   // velocity
			"%.1f,%.1f,"  // dt_ms, snapshot_age_ms
			"%.1f,%s,%.2f,%.1f,"   // rtt_ms, rtt_source, mult, horizon_bias
			"%.1f,"        // horizon_ms
			"%.1f,%.1f,%.1f,"  // predicted
			"%.1f,%.1f,%.1f,"  // rendered
			"%.1f,%.1f,%.1f,%.1f,"  // diff x,y,z,mag
			"%d,%d,%s,%d\n",  // valid, reseeded, reseed_reason, age_clamped
			csp_debug_frame,
			csp_snapshot_receive_ms, cl.frame.serverframe, cl.frame.servertime,
			ent_num,
			st->cur_origin[0], st->cur_origin[1], st->cur_origin[2],
			st->prev_origin[0], st->prev_origin[1], st->prev_origin[2],
			st->velocity[0], st->velocity[1], st->velocity[2],
			(float)(cl.frame.servertime - st->prev_servertime),
			(float)cls.realtime - (float)st->cur_receive_ms,
			rtt_ms,
			csp_latency_ms->intvalue > 0 ? "override" : "acked",
			csp_mult->value, csp_horizon_bias->value,
			horizon_ms,
			predicted[0], predicted[1], predicted[2],
			rendered[0], rendered[1], rendered[2],
			dx, dy, dz, diff_mag,
			st->initialized && st->has_velocity,
			reseeded ? 1 : 0,
			reseed_reason ? reseed_reason : "",
			age_clamped ? 1 : 0
		);
		fflush (csp_debug_fp);
	}

	// Console log at level 1
	if (csp_debug->intvalue == 1)
	{
		Com_Printf ("CSP[%d]: snap=(%.0f,%.0f,%.0f) vel=(%.1f,%.1f,%.1f) "
			"horiz=%.1fms pred=(%.0f,%.0f,%.0f) diff=%.1f %s\n",
			LOG_CLIENT,
			ent_num,
			st->cur_origin[0], st->cur_origin[1], st->cur_origin[2],
			st->velocity[0], st->velocity[1], st->velocity[2],
			horizon_ms,
			predicted[0], predicted[1], predicted[2],
			diff_mag,
			reseeded ? "RESET" : (age_clamped ? "CLAMP" : "ok")
		);
	}
}

// ====================================================================
// Prediction
// ====================================================================

/*
====================
CL_CSP_PredictEntity

Stateless prediction from the latest authoritative snapshot.

Pipeline:
  1. Bounds check entity number
  2. Update entity velocity state (idempotent per snapshot)
  3. Calculate horizon from snapshot age + RTT/2 + bias
  4. predicted = cur_origin + velocity * (horizon / 1000.0)

Idempotent: multiple calls per frame (body, weapon, linked) return same
result because snapshot state is updated only once per serverframe.

Parameters:
  cent       - entity being predicted
  ent_num    - entity number (0..127, CSP_MAX_ENTITIES)
  out_origin - predicted origin output
  out_angles - predicted angles output
====================
*/
void CL_CSP_PredictEntity (centity_t *cent, int ent_num, vec3_t out_origin, vec3_t out_angles)
{
	csp_entity_state_t *st;
	int i;
	float horizon_ms;
	qboolean reseeded = false, age_clamped = false;
	const char *reseed_reason = "";

	if (!cent)
	{
		VectorClear (out_origin);
		VectorClear (out_angles);
		return;
	}

	// Bounds check — valid indices are 0..127
	if (ent_num < 0 || ent_num >= CSP_MAX_ENTITIES)
	{
		VectorCopy (cent->current.origin, out_origin);
		for (i = 0; i < 3; i++)
			out_angles[i] = cent->current.angles[i];
		return;
	}

	// Guard: only predict when frame state is ready.
	// During map loading, frame data may be zeroed or invalid.
	if (cl.frame.servertime == 0 && cl.frame.serverframe == 0)
	{
		VectorCopy (cent->current.origin, out_origin);
		for (i = 0; i < 3; i++)
			out_angles[i] = cent->current.angles[i];
		return;
	}

	st = &csp_entities[ent_num];

	// Check if entity was present in the current or previous server frame.
	// If it was absent, reseed to clear stale velocity — the entity may have
	// respawned or teleported while we had no snapshots.
	if (st->initialized && st->cur_serverframe != cl.frame.serverframe - 1
		&& st->cur_serverframe != cl.frame.serverframe)
	{
		st->has_velocity = false;
		VectorClear (st->velocity);
		CL_CSP_ResetSmoothing (st);
		st->last_reset_reason = CSP_RESET_ENTITY_ABSENT;
		st->prev_servertime = st->cur_servertime;
		st->prev_serverframe = st->cur_serverframe;
		VectorCopy (st->cur_origin, st->prev_origin);
		reseeded = true;
		reseed_reason = "entity_not_in_recent_frame";
	}

	// Update entity state and velocity (idempotent)
	CL_CSP_UpdateEntityVelocity (cent, ent_num);

	// Check if velocity was cleared (reseed event)
	if (!st->has_velocity && st->seen_count > CSP_SNAP_INIT)
	{
		if (!reseeded)
		{
			reseeded = true;
			reseed_reason = "velocity_cleared";
			CL_CSP_ResetSmoothing (st);
			st->last_reset_reason = CSP_RESET_VELOCITY_CLEARED;
		}
	}

	// Calculate predicted position
	if (st->has_velocity)
	{
		horizon_ms = CL_CSP_CalculateHorizon (st, &age_clamped);

		out_origin[0] = st->cur_origin[0] + st->velocity[0] * (horizon_ms / 1000.0f);
		out_origin[1] = st->cur_origin[1] + st->velocity[1] * (horizon_ms / 1000.0f);
		out_origin[2] = st->cur_origin[2] + st->velocity[2] * (horizon_ms / 1000.0f);
	}
	else
	{
		// No valid velocity — use current snapshot origin (no prediction)
		VectorCopy (st->cur_origin, out_origin);

		// Calculate horizon for diagnostics even without velocity
		horizon_ms = CL_CSP_CalculateHorizon (st, &age_clamped);
	}

	// Phase 2 smoothing — apply decaying visual offset to predictions.
	//
	// On a new snapshot (first render for this serverframe):
	//   offset = last_rendered_ghost - new_predicted
	//   This captures the actual visual jump from where the ghost was
	//   last displayed to where it is now predicted.
	//
	// Each subsequent render within the same snapshot:
	//   contribution = offset * exp(-elapsed_ms / tau)
	//
	// The raw diagnostic prediction (out_origin before smoothing) is
	// unaffected — only the displayed position shifts.
	//
	// Req 2: uses the ACTUAL last-rendered ghost position as the
	// reference, NOT a reconstruction from cur_origin + velocity*horizon.
	if (csp_smooth->value && st->has_velocity)
	{
		// Capture the prediction jump on the first render for this serverframe
		if (st->smooth_captured_for_frame != cl.frame.serverframe
			&& st->last_rendered_valid)
		{
			// offset = where we last displayed - where we now predict
			VectorSubtract (st->last_rendered_ghost, out_origin, st->smooth_offset);
			st->smooth_offset_time = (float)cls.realtime;
			st->smooth_active = true;
			st->smooth_captured_for_frame = cl.frame.serverframe;
		}

		if (st->smooth_active)
		{
			int tau = (int)csp_smooth_tau->value;
			if (tau < 1) tau = CSP_SMOOTH_TAU_DEFAULT;
			float elapsed = (float)(cls.realtime - st->smooth_offset_time);
			float decay = expf(-elapsed / (float)tau);
			if (decay < 0.001f)
			{
				// Offset has effectively decayed away
				st->smooth_active = false;
			}
			else
			{
				out_origin[0] += st->smooth_offset[0] * decay;
				out_origin[1] += st->smooth_offset[1] * decay;
				out_origin[2] += st->smooth_offset[2] * decay;
			}
		}

		// Record the actual displayed position for next frame's smoothing
		// reference. This is the position that will be sent to the renderer.
		VectorCopy (out_origin, st->last_rendered_ghost);
		st->last_rendered_time = cls.realtime;
		st->last_rendered_valid = true;
	}

	// Angles — use normal client interpolation
	for (i = 0; i < 3; i++)
	{
		float a1 = cent->current.angles[i];
		float a2 = cent->prev.angles[i];
		out_angles[i] = LerpAngle (a2, a1, cl.lerpfrac);
	}

	// Debug logging (once per snapshot per entity)
	if (csp_debug->intvalue > 0)
	{
		CL_CSP_DebugLog (
			ent_num, out_origin, cent->current.origin,
			st, CL_CSP_GetRTTMs(), horizon_ms,
			reseeded, reseed_reason, age_clamped
		);
	}
}

// ====================================================================
// Rendering Decision
// ====================================================================

/*
====================
CL_CSP_ShouldRenderGhost

Return whether a ghost should be rendered for the given entity.
Checks CSP enable state, entity validity, bounds, and game state.
====================
*/
qboolean CL_CSP_ShouldRenderGhost (centity_t *cent, int ent_num)
{
	csp_entity_state_t *st;
	int max_entities_for_player;

	// CSP must be enabled
	if (csp_enable->intvalue <= 0)
		return false;

	// Must be in active game state
	if (cls.state != ca_active)
		return false;

	// Guard against negative entity numbers
	if (ent_num < 0)
		return false;

	// Bounds check — entity index must be within allocated state
	if (ent_num >= CSP_MAX_ENTITIES)
		return false;

	// Only predict player entities, not map entities, items, projectiles, etc.
	// Player entities start at index 1 (0 is worldspawn) and go up to
	// cl.maxclients + num_player_edicts. Guard against cl.maxclients being
	// 0 during map loading or before first frame is received.
	if (cl.maxclients == 0)
		return false;

	// Check if this is a valid player entity
	// Player entities typically start at entity index 1 (0 is worldspawn)
	// Max player entity index depends on maxclients
	max_entities_for_player = (int)cl.maxclients + 1;
	if (ent_num >= max_entities_for_player)
		return false;

	st = &csp_entities[ent_num];

	// Always update velocity for this snapshot. This breaks the chicken-and-egg
	// problem: ShouldRenderGhost previously required st->initialized to be true,
	// but initialized is only set during UpdateEntityVelocity which was normally
	// called from PredictEntity AFTER ShouldRenderGhost returned true.
	// By updating here we ensure entity state advances on every render frame.
	// UpdateEntityVelocity is idempotent per snapshot (early-returns if already
	// updated for this serverframe).
	if (cl.frame.servertime != 0 || cl.frame.serverframe != 0)
		CL_CSP_UpdateEntityVelocity (cent, ent_num);
	else
		return false;

	// Only render ghost after we have velocity data (initialized + seen at
	// least twice so we've computed a velocity delta).
	if (!st->initialized || !st->has_velocity)
		return false;

	return true;
}
