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

	// Snapshot-log dedupe: serverframe for which the snapshot CSV row was
	// already emitted this frame. Ensures exactly one snapshot row per
	// entity per authoritative snapshot even though PredictEntity runs at
	// render frequency.
	int   snap_logged_serverframe;

	// Raw (pre-smoothing) predicted position — stored during PredictEntity
	// so the render CSV can report both raw and smoothed values.
	float raw_predicted[3];
} csp_entity_state_t;

static csp_entity_state_t csp_entities[CSP_MAX_ENTITIES];

//
// Acknowledged-command RTT tracking — captured from cl_scrn.c ping measurement
// We sample the raw RTT each frame and maintain a light EMA for stability.
//
static float csp_ack_rtt_ms = 0.0f;       // smoothed RTT estimate in ms
static qboolean csp_has_rtt = false;     // have we received an RTT sample

//
// RTT sampling metadata (Req 3) — raw and smoothed kept separate.
//
typedef enum {
	CSP_RTT_SOURCE_OVERRIDE = 0,  // csp_latency_ms > 0 (user override)
	CSP_RTT_SOURCE_MEASURED,      // from acknowledged-command RTT
	CSP_RTT_SOURCE_FALLBACK       // 150 ms fixed fallback
} csp_rtt_source_t;

static int   csp_last_acked_seq = -1;    // last ack sequence we sampled (dedupe)
static float csp_raw_rtt_ms = 0.0f;      // last raw RTT sample (unsmoothed)
static int   csp_rtt_sample_time = 0;    // cls.realtime when last sample was taken
static int   csp_rtt_acked_seq = 0;      // ack sequence of last sample
static csp_rtt_source_t csp_rtt_source = CSP_RTT_SOURCE_FALLBACK;

// Extern declarations for client state
extern client_state_t cl;
extern client_static_t cls;

//
// Phase 1 is the default and only mode. Phase 2 smoothing is gated by
// csp_smooth (default 0 = disabled) and must not affect underlying
// prediction or diagnostics.
//
// Debug logging state — two independent CSV streams (Req 3 / Phase C):
//   csp_debug_fp    : snapshot CSV (present snapshots via update path;
//                       absence transition logged once; see CL_CSP_LogSnapshot)
//   csp_render_fp   : render CSV   (one row per entity per rendered frame)
static FILE *csp_debug_fp = NULL;
static FILE *csp_render_fp = NULL;
static int csp_debug_frame = 0;
static int csp_render_frame = 0;

//
// Per-frame timestamp capture — set by CL_CSP_ParseFrameNotify()
//
static int csp_snapshot_receive_ms = 0;
static int csp_snapshot_serverframe = 0;
static int csp_snapshot_processed_frame = 0;  // Phase E2: idempotency guard

//
// Phase E3/E4: authoritative-frame indicator.
// Set to true ONLY after a real svc_frame has been successfully reconstructed
// AND its packet entities have been parsed (CL_CSP_FrameValidated, called from
// CL_ParseFrame after CL_ParsePacketEntities). This decouples snapshot
// membership processing from CL_ParseServerMessage()'s return value, which is
// false for svc_playerupdate (delta) messages even when gotFrame is true.
//
// Phase E4: the flag is no longer set in CL_CSP_ParseFrameNotify (which runs at
// the top of CL_ParseFrame, before delta validation). It is now set only on the
// validated-frame path, so invalid delta bases and svc_playerupdate-only
// messages can never trigger CL_CSP_ProcessSnapshot().
//
static qboolean csp_authoritative_frame_parsed = false;

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
	csp_last_acked_seq = -1;
	csp_raw_rtt_ms = 0.0f;
	csp_rtt_sample_time = 0;
	csp_rtt_acked_seq = 0;
	csp_rtt_source = CSP_RTT_SOURCE_FALLBACK;

	// Phase E3: reset per-frame snapshot lifecycle metadata so that a
	// reconnect / map change cannot carry a stale idempotency guard or a
	// stale authoritative-frame indicator into the next session.
	csp_snapshot_receive_ms = 0;
	csp_snapshot_serverframe = 0;
	csp_snapshot_processed_frame = 0;
	csp_authoritative_frame_parsed = false;

	if (csp_debug_fp)
	{
		fclose (csp_debug_fp);
		csp_debug_fp = NULL;
	}
	if (csp_render_fp)
	{
		fclose (csp_render_fp);
		csp_render_fp = NULL;
	}
	csp_debug_frame = 0;
	csp_render_frame = 0;
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
		csp_entities[i].snap_logged_serverframe = 0;
		VectorClear (csp_entities[i].raw_predicted);
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
	st->last_rendered_valid = false;
	st->smooth_captured_for_frame = 0;
}

/*
====================
CL_CSP_SmoothChanged

csp_smooth changed callback: when the cvar transitions (enable/disable or
value change), reset smoothing state for all tracked entities so that a
stale offset captured before the transition is never applied.
====================
*/
void CL_CSP_SmoothChanged (cvar_t *self, char *oldValue, char *newValue)
{
	int i;
	for (i = 0; i < CSP_MAX_ENTITIES; i++)
	{
		if (csp_entities[i].initialized)
			CL_CSP_ResetSmoothing (&csp_entities[i]);
	}
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

/*
====================
CL_CSP_SampleRTT

Decoupled RTT sampling (Req 3).
Called unconditionally from cl_main.c after CL_ParseServerMessage,
independent of netgraph visualization state.

Samples RTT only when a NEW acknowledgement is observed (deduped by
ack sequence number). Keeps raw and smoothed RTT separate.
====================
*/
void CL_CSP_SampleRTT (void)
{
	// If user override is active, no sampling needed
	if (csp_latency_ms && csp_latency_ms->value > 0.0f)
	{
		csp_rtt_source = CSP_RTT_SOURCE_OVERRIDE;
		return;
	}

	// Get current acknowledged sequence
	int ack_seq = cls.netchan.incoming_acknowledged;

	// Dedupe: only sample when a new ack is observed
	if (ack_seq == csp_last_acked_seq)
		return;

	csp_last_acked_seq = ack_seq;

	// Compute raw RTT: time since the acknowledged command was sent
	int in = ack_seq & (CMD_BACKUP - 1);
	int ping = cls.realtime - cl.cmd_time[in];

	// Sanity check: ping should be positive and reasonable
	if (ping < 0 || ping > 5000)
		return;

	// Store raw sample metadata
	csp_raw_rtt_ms = (float)ping;
	csp_rtt_sample_time = cls.realtime;
	csp_rtt_acked_seq = ack_seq;
	csp_rtt_source = CSP_RTT_SOURCE_MEASURED;

	// Update smoothed EMA (same logic as before)
	CL_CSP_UpdateRTT ((float)ping);
}

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

	// Phase E4: the authoritative-frame indicator is NO LONGER set here.
	// This function runs at the top of CL_ParseFrame, before delta-frame
	// validation and packet-entity parsing. Setting the flag here would
	// incorrectly mark invalid delta bases as processed. The flag is now
	// set exclusively by CL_CSP_FrameValidated(), which is called after
	// CL_ParsePacketEntities only when the frame passed validation.

	// Auto-open debug files at level 2 — two independent CSV streams (Phase C)
	if (csp_debug->intvalue >= 2)
	{
		// --- Snapshot CSV ---
		// Logging semantics (Phase E3): present snapshots are logged via the
		// entity update path (one row per entity per snapshot); a
		// present-to-absent transition is logged once by
		// CL_CSP_ProcessSnapshot; continued absence produces no repeated row;
		// reappearance is logged once as a fresh baseline. This is NOT one
		// row for every entity in every snapshot.
		if (!csp_debug_fp)
		{
			csp_debug_fp = fopen (csp_debug_file->string, "w");
			if (csp_debug_fp)
			{
				csp_debug_frame = 0;
				fprintf (csp_debug_fp,
					"row_id,"
					"local_receive_ms,serverframe,servertime,"
					"ent_num,"
					"prev_present,cur_present,"
					"prev_serverframe,cur_serverframe,serverframe_delta,"
					"prev_dropped,cur_dropped,drop_delta,"
					"snap_origin_x,snap_origin_y,snap_origin_z,"
					"prev_origin_x,prev_origin_y,prev_origin_z,"
					"velocity_x,velocity_y,velocity_z,"
					"dt_ms,snapshot_age_ms,"
					"rtt_ms,rtt_source,mult,horizon_bias,horizon_ms,"
					"raw_rtt_ms,smoothed_rtt_ms,rtt_sample_time,rtt_sample_age_ms,acked_cmd_seq,"
					"csp_latency_ms,csp_horizon_ms,csp_smooth,csp_smooth_tau,"
					"raw_predicted_x,raw_predicted_y,raw_predicted_z,"
					"has_velocity,reseeded,reseed_reason,snap_age_clamped,"
					"prev_anim_frame,cur_anim_frame,"
					"prev_event,cur_event,"
					"modelindex,solid,effects,renderfx,"
					"prev_angles_x,prev_angles_y,prev_angles_z,"
					"cur_angles_x,cur_angles_y,cur_angles_z\n");
				fflush (csp_debug_fp);
			}
		}

		// --- Render CSV (one row per entity per rendered frame) ---
		if (!csp_render_fp)
		{
			char render_path[512];
			// Derive render filename: csp_debug.log -> csp_render.log
			const char *base = csp_debug_file->string;
			const char *dot = strrchr (base, '.');
			if (dot)
			{
				int len = (int)(dot - base);
				if (len > 500) len = 500;
				memcpy (render_path, base, len);
				strcpy (render_path + len, "_render.log");
			}
			else
			{
				int len = (int)strlen (base);
				if (len > 500) len = 500;
				memcpy (render_path, base, len);
				strcpy (render_path + len, "_render.log");
			}

			csp_render_fp = fopen (render_path, "w");
			if (csp_render_fp)
			{
				csp_render_frame = 0;
				fprintf (csp_render_fp,
					"row_id,"
					"render_ms,serverframe,servertime,"
					"ent_num,"
					"engine_interp_x,engine_interp_y,engine_interp_z,"
					"raw_predicted_x,raw_predicted_y,raw_predicted_z,"
					"smoothed_ghost_x,smoothed_ghost_y,smoothed_ghost_z,"
					"smooth_active,smooth_offset_x,smooth_offset_y,smooth_offset_z,"
					"smooth_elapsed_ms,smooth_decay,"
					"original_rendered,"
					"ghost_mode\n");
				fflush (csp_render_fp);
			}
		}
	}
}

/*
====================
CL_CSP_FrameValidated

Phase E4: called from CL_ParseFrame (cl_ents.c) ONLY after the frame has
passed delta validation AND CL_ParsePacketEntities has completed. This is
the single point where csp_authoritative_frame_parsed is set to true.

This ensures:
- Invalid delta bases (old->valid == false, serverframe mismatch,
  parse_entities too old) never trigger CL_CSP_ProcessSnapshot().
- svc_playerupdate-only messages never trigger membership processing
  (they never reach CL_ParseFrame at all).
- Valid uncompressed and valid delta frames each trigger exactly one
  membership-processing pass (enforced by the idempotency guard in
  CL_CSP_ProcessSnapshot).
====================
*/
void CL_CSP_FrameValidated (void)
{
	if (cls.state != ca_active)
		return;

	csp_authoritative_frame_parsed = true;
}

// ====================================================================
// Velocity Update (Idempotent per snapshot)
// ====================================================================

// Forward declarations (defined later in this file)
static float CL_CSP_GetRTTMs (void);
static float CL_CSP_CalculateHorizon (csp_entity_state_t *st, qboolean *age_clamped);
static void CL_CSP_LogSnapshot (
	int ent_num, csp_entity_state_t *st,
	float rtt_ms, float horizon_ms,
	qboolean reseeded, const char *reseed_reason, qboolean age_clamped);

/*
====================
CL_CSP_ProcessSnapshot

Phase E2/E3: Snapshot membership lifecycle — called from cl_main.c after
CL_ParseServerMessage() returns, when cl.frame and all packet entities
are fully populated.

Iterates over ALL tracked entity slots (not just those in the render loop)
and detects entities that are ABSENT from the current authoritative snapshot.
For each absent entity:
  - Clears velocity and smoothing state
  - Records the absence transition (prev_present/cur_present)
  - Logs the snapshot row exactly once (deduped by snap_logged_serverframe)

This decouples absence detection from the render path. Previously, the
entity-absent reseed path in CL_CSP_UpdateEntityVelocity could only execute
if the entity reached the render loop — which it never does when absent.

Phase E3 — authoritative-frame gating:
  This function is now called UNCONDITIONALLY from cl_main.c (not inside
  `if (CL_ParseServerMessage())`). CL_ParseServerMessage() returns false for
  svc_playerupdate (delta) messages even when gotFrame is true, so gating on
  its return value would skip membership processing for any message that
  contains both svc_frame and svc_playerupdate. Instead, this function
  self-guards on csp_authoritative_frame_parsed, which is set to true ONLY by
  CL_CSP_ParseFrameNotify() (called exclusively from CL_ParseFrame, i.e. only
  for a real svc_frame) and consumed here. A svc_playerupdate-only message
  never sets this flag, so it cannot trigger a false entity-absence
  transition.

Idempotent: guarded by csp_snapshot_processed_frame to prevent double
processing within the same serverframe.

Logging semantics (intentional, do NOT change):
  - Normal present snapshots: logged through the entity update path
    (CL_CSP_UpdateEntityVelocity), one row per entity per snapshot.
  - Present-to-absent transition: logged ONCE by this function (the
    prev_present==1 branch below).
  - Continued absence: no repeated row (prev_present is 0, so the log
    branch is skipped).
  - Reappearance: logged once as a fresh baseline via the update path.
  This is NOT "one row for every entity in every snapshot" — absence is
  logged as a single transition event, not per absent frame.
====================
*/
void CL_CSP_ProcessSnapshot (void)
{
	int i;
	int max_entities;

	// Phase E3: only process when a real authoritative svc_frame was parsed
	// in the current message. This flag is set by CL_CSP_ParseFrameNotify
	// (called only from CL_ParseFrame, i.e. only for svc_frame) and consumed
	// here. A svc_playerupdate (delta) message never sets this flag, so it
	// cannot trigger a false entity-absence transition.
	if (!csp_authoritative_frame_parsed)
		return;
	csp_authoritative_frame_parsed = false;  // consume

	// Guard: only process when we have valid frame data
	if (cl.frame.servertime == 0 && cl.frame.serverframe == 0)
		return;

	// Idempotency: only process once per serverframe
	if (csp_snapshot_processed_frame == cl.frame.serverframe)
		return;
	csp_snapshot_processed_frame = cl.frame.serverframe;

	// Only process when CSP is enabled and we're in active game state
	if (csp_enable->intvalue <= 0)
		return;
	if (cls.state != ca_active)
		return;

	// Determine the range of player entity slots to check.
	// Player entities start at index 1 (0 is worldspawn) and go up to
	// cl.maxclients. Guard against cl.maxclients being 0 during map loading.
	if (cl.maxclients == 0)
		return;
	max_entities = (int)cl.maxclients + 1;
	if (max_entities > CSP_MAX_ENTITIES)
		max_entities = CSP_MAX_ENTITIES;

	for (i = 1; i < max_entities; i++)
	{
		csp_entity_state_t *st = &csp_entities[i];

		// Only process entities that have been initialized (seen at least once)
		if (!st->initialized)
			continue;

		// Check actual snapshot membership: cl_entities[i].serverframe is set
		// by CL_SetEntState during parsing ONLY if the entity was present in
		// the current snapshot's delta stream. If it doesn't match the current
		// frame, the entity was ABSENT from this authoritative snapshot.
		qboolean entity_present = (cl_entities[i].serverframe == cl.frame.serverframe);

		// Record snapshot continuity: presence + drop counter deltas
		st->prev_present = st->cur_present;
		st->cur_present  = entity_present ? 1 : 0;
		st->prev_dropped = st->cur_dropped;
		st->cur_dropped  = cls.netchan.dropped;

		// Entity ABSENT from current snapshot — perform reseed
		if (!entity_present)
		{
			st->has_velocity = false;
			VectorClear (st->velocity);
			CL_CSP_ResetSmoothing (st);
			st->last_reset_reason = CSP_RESET_ENTITY_ABSENT;

			// Log the absence TRANSITION exactly once: only when the entity
			// was present in the previous snapshot (prev_present was 1 before
			// the shift above). Subsequent absent frames do not re-log.
			if (st->prev_present == 1)
			{
				qboolean age_clamped = false;
				float horizon_ms = CL_CSP_CalculateHorizon (st, &age_clamped);
				VectorCopy (st->cur_origin, st->raw_predicted);
				if (csp_debug->intvalue > 0)
					CL_CSP_LogSnapshot (i, st, CL_CSP_GetRTTMs(), horizon_ms,
						true, "entity_not_in_snapshot", age_clamped);
			}
		}
	}
}

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
	qboolean reseeded = false;
	const char *reseed_reason = "";
	qboolean age_clamped = false;
	float horizon_ms = 0.0f;

	// Guard: don't update if frame data is not ready
	if (cl.frame.servertime == 0 && cl.frame.serverframe == 0)
		return;

	// New snapshot detection via serverframe — idempotent
	if (st->cur_serverframe == cl.frame.serverframe)
		return;  // already updated this frame

	// Phase E2: Entity presence is now handled by CL_CSP_ProcessSnapshot
	// (called from cl_main.c after parsing). This function only runs for
	// entities PRESENT in the current snapshot (they reached the render loop).
	//
	// Reappearance detection: if the entity was absent in the previous
	// snapshot (prev_present == 0, set by CL_CSP_ProcessSnapshot) and is now
	// present, establish a fresh baseline. Do NOT compute velocity across
	// the missing interval using the old position.
	// seen_count > 1 ensures this is a genuine reappearance, not the first
	// frame after initialization (where prev_present is 0 by default).
	if (st->initialized && st->prev_present == 0 && st->seen_count > 1)
	{
		// Entity reappeared after absence — fresh baseline
		st->prev_servertime = cl.frame.servertime;
		st->prev_serverframe = cl.frame.serverframe;
		VectorCopy (cent->current.origin, st->prev_origin);
		st->cur_servertime = cl.frame.servertime;
		st->cur_serverframe = cl.frame.serverframe;
		VectorCopy (cent->current.origin, st->cur_origin);
		st->cur_receive_ms = csp_snapshot_receive_ms;
		st->has_velocity = false;
		VectorClear (st->velocity);
		CL_CSP_ResetSmoothing (st);
		st->last_reset_reason = CSP_RESET_ENTITY_ABSENT;
		st->seen_count++;
		reseeded = true;
		reseed_reason = "entity_reappeared_fresh_baseline";

		// Log the reappearance transition
		horizon_ms = CL_CSP_CalculateHorizon (st, &age_clamped);
		VectorCopy (st->cur_origin, st->raw_predicted);
		if (csp_debug->intvalue > 0)
			CL_CSP_LogSnapshot (ent_num, st, CL_CSP_GetRTTMs(), horizon_ms,
				reseeded, reseed_reason, age_clamped);
		return;
	}

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

		// Section C: Log first-sight snapshot
		horizon_ms = CL_CSP_CalculateHorizon (st, &age_clamped);
		VectorCopy (st->cur_origin, st->raw_predicted);
		if (csp_debug->intvalue > 0)
			CL_CSP_LogSnapshot (ent_num, st, CL_CSP_GetRTTMs(), horizon_ms,
				false, "", age_clamped);
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
		reseeded = true;
		reseed_reason = "invalid_dt";

		// Section C: Log reseed state
		horizon_ms = CL_CSP_CalculateHorizon (st, &age_clamped);
		VectorCopy (st->cur_origin, st->raw_predicted);
		if (csp_debug->intvalue > 0)
			CL_CSP_LogSnapshot (ent_num, st, CL_CSP_GetRTTMs(), horizon_ms,
				reseeded, reseed_reason, age_clamped);
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
		reseeded = true;
		reseed_reason = "serverframe_skipped";

		// Section C: Log reseed state
		horizon_ms = CL_CSP_CalculateHorizon (st, &age_clamped);
		VectorCopy (st->cur_origin, st->raw_predicted);
		if (csp_debug->intvalue > 0)
			CL_CSP_LogSnapshot (ent_num, st, CL_CSP_GetRTTMs(), horizon_ms,
				reseeded, reseed_reason, age_clamped);
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
			reseeded = true;
			reseed_reason = "teleport_disp";

			// Section C: Log reseed state
			horizon_ms = CL_CSP_CalculateHorizon (st, &age_clamped);
			VectorCopy (st->cur_origin, st->raw_predicted);
			if (csp_debug->intvalue > 0)
				CL_CSP_LogSnapshot (ent_num, st, CL_CSP_GetRTTMs(), horizon_ms,
					reseeded, reseed_reason, age_clamped);
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

	// Section C: Log successful snapshot update
	horizon_ms = CL_CSP_CalculateHorizon (st, &age_clamped);
	if (st->has_velocity)
	{
		st->raw_predicted[0] = st->cur_origin[0] + st->velocity[0] * (horizon_ms / 1000.0f);
		st->raw_predicted[1] = st->cur_origin[1] + st->velocity[1] * (horizon_ms / 1000.0f);
		st->raw_predicted[2] = st->cur_origin[2] + st->velocity[2] * (horizon_ms / 1000.0f);
	}
	else
	{
		VectorCopy (st->cur_origin, st->raw_predicted);
	}
	if (csp_debug->intvalue > 0)
		CL_CSP_LogSnapshot (ent_num, st, CL_CSP_GetRTTMs(), horizon_ms,
			reseeded, reseed_reason, age_clamped);
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
	{
		csp_rtt_source = CSP_RTT_SOURCE_OVERRIDE;
		return (float)csp_latency_ms->intvalue;
	}

	if (csp_has_rtt && (csp_ack_rtt_ms > 0.0f))
	{
		csp_rtt_source = CSP_RTT_SOURCE_MEASURED;
		return csp_ack_rtt_ms;
	}

	csp_rtt_source = CSP_RTT_SOURCE_FALLBACK;
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

Timing model (symmetric network assumption):
- snapshot_age = time since local receipt (cls.realtime - cur_receive_ms).
  This is how stale the snapshot is from the client's perspective.
  It does NOT include downstream travel time — that already elapsed
  before cur_receive_ms was recorded.
- RTT/2 estimates one-way network delay. The server state at servertime
  was computed before the snapshot was sent, so the position is
  effectively RTT/2 behind the server's "now".
- Commands execute immediately on arrival (no server scheduling delay).
- Together, snapshot_age + RTT/2 estimates how far in the past the
  snapshot's position is relative to the server's current state.

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
// Snapshot and Render Logging (Phase C — two-stream CSV)
// ====================================================================

/*
====================
CL_CSP_LogSnapshot

Log one row to the snapshot CSV. Deduped by snap_logged_serverframe so
that multiple calls for the same entity in the same serverframe produce
exactly one row.

Logging semantics (intentional, Phase E3 clarification):
  - Normal present snapshots: logged through the entity update path
    (CL_CSP_UpdateEntityVelocity), one row per entity per snapshot.
  - Present-to-absent transition: logged ONCE by CL_CSP_ProcessSnapshot
    (the prev_present==1 branch).
  - Continued absence: no repeated row (prev_present is 0, so the log
    branch in ProcessSnapshot is skipped).
  - Reappearance: logged once as a fresh baseline via the update path.

This is NOT "one row for every entity in every snapshot" — absence is
logged as a single transition event, not per absent frame.

Includes all Req 1 (continuity) and Req 4 (movement/animation) fields.
The "valid" column is removed — has_velocity is the meaningful indicator.
RTT source uses the csp_rtt_source enum (override/measured/fallback).
====================
*/
static void CL_CSP_LogSnapshot (
	int ent_num, csp_entity_state_t *st,
	float rtt_ms, float horizon_ms,
	qboolean reseeded, const char *reseed_reason, qboolean age_clamped)
{
	centity_t *cent;
	const char *rtt_source_str;

	// Dedupe: only log once per entity per authoritative snapshot
	if (st->snap_logged_serverframe == cl.frame.serverframe)
		return;
	st->snap_logged_serverframe = cl.frame.serverframe;

	// Console log at level 1
	if (csp_debug->intvalue == 1)
	{
		Com_Printf ("CSP[%d]: snap=(%.0f,%.0f,%.0f) vel=(%.1f,%.1f,%.1f) "
			"horiz=%.1fms pred=(%.0f,%.0f,%.0f) %s\n",
			LOG_CLIENT,
			ent_num,
			st->cur_origin[0], st->cur_origin[1], st->cur_origin[2],
			st->velocity[0], st->velocity[1], st->velocity[2],
			horizon_ms,
			st->raw_predicted[0], st->raw_predicted[1], st->raw_predicted[2],
			reseeded ? "RESET" : (age_clamped ? "CLAMP" : "ok")
		);
	}

	// CSV to file at level 2
	if (csp_debug->intvalue < 2 || !csp_debug_fp)
		return;

	csp_debug_frame++;

	// RTT source string from enum
	switch (csp_rtt_source)
	{
	case CSP_RTT_SOURCE_OVERRIDE: rtt_source_str = "override"; break;
	case CSP_RTT_SOURCE_MEASURED: rtt_source_str = "measured"; break;
	default:                      rtt_source_str = "fallback"; break;
	}

	// Get centity for Req 4 fields (animation frame, event, model, angles)
	if (ent_num < 0 || ent_num >= MAX_EDICTS)
		cent = NULL;
	else
		cent = &cl_entities[ent_num];

	fprintf (csp_debug_fp,
		"%d,"           // row_id
		"%d,%d,%d,"     // local_receive_ms, serverframe, servertime
		"%d,"           // ent_num
		"%d,%d,"        // prev_present, cur_present
		"%d,%d,%d,"     // prev_serverframe, cur_serverframe, serverframe_delta
		"%d,%d,%d,"     // prev_dropped, cur_dropped, drop_delta
		"%.1f,%.1f,%.1f,"   // snap_origin
		"%.1f,%.1f,%.1f,"   // prev_origin
		"%.2f,%.2f,%.2f,"   // velocity
		"%.1f,%.1f,"  // dt_ms, snapshot_age_ms
		"%.1f,%s,%.2f,%.1f,%.1f,"  // rtt_ms, rtt_source, mult, horizon_bias, horizon_ms
		"%.2f,%.2f,%d,%d,%d,"  // raw_rtt_ms, smoothed_rtt_ms, rtt_sample_time, rtt_sample_age_ms, acked_cmd_seq
		"%.1f,%.1f,%.1f,%.1f,"  // csp_latency_ms, csp_horizon_ms, csp_smooth, csp_smooth_tau
		"%.1f,%.1f,%.1f,"  // raw_predicted
		"%d,%d,%s,%d,"  // has_velocity, reseeded, reseed_reason, age_clamped
		"%d,%d,"        // prev_anim_frame, cur_anim_frame
		"%d,%d,"        // prev_event, cur_event
		"%d,%d,%d,%d,"  // modelindex, solid, effects, renderfx
		"%.1f,%.1f,%.1f,"  // prev_angles
		"%.1f,%.1f,%.1f\n" // cur_angles
		,
		csp_debug_frame,
		csp_snapshot_receive_ms, cl.frame.serverframe, cl.frame.servertime,
		ent_num,
		st->prev_present, st->cur_present,
		st->prev_serverframe, st->cur_serverframe,
		st->cur_serverframe - st->prev_serverframe,
		st->prev_dropped, st->cur_dropped,
		st->cur_dropped - st->prev_dropped,
		st->cur_origin[0], st->cur_origin[1], st->cur_origin[2],
		st->prev_origin[0], st->prev_origin[1], st->prev_origin[2],
		st->velocity[0], st->velocity[1], st->velocity[2],
		(float)(cl.frame.servertime - st->prev_servertime),
		(float)cls.realtime - (float)st->cur_receive_ms,
		rtt_ms, rtt_source_str,
		csp_mult->value, csp_horizon_bias->value,
		horizon_ms,
		csp_raw_rtt_ms, csp_ack_rtt_ms,
		csp_rtt_sample_time,
		(int)(cls.realtime - csp_rtt_sample_time),
		csp_rtt_acked_seq,
		csp_latency_ms->value, csp_horizon_ms->value,
		csp_smooth->value, csp_smooth_tau->value,
		st->raw_predicted[0], st->raw_predicted[1], st->raw_predicted[2],
		st->has_velocity ? 1 : 0,
		reseeded ? 1 : 0,
		reseed_reason ? reseed_reason : "",
		age_clamped ? 1 : 0,
		cent ? cent->prev.frame : 0,
		cent ? cent->current.frame : 0,
		cent ? cent->prev.event : 0,
		cent ? cent->current.event : 0,
		cent ? cent->current.modelindex : 0,
		cent ? cent->current.solid : 0,
		cent ? cent->current.effects : 0,
		cent ? cent->current.renderfx : 0,
		cent ? cent->prev.angles[0] : 0,
		cent ? cent->prev.angles[1] : 0,
		cent ? cent->prev.angles[2] : 0,
		cent ? cent->current.angles[0] : 0,
		cent ? cent->current.angles[1] : 0,
		cent ? cent->current.angles[2] : 0
	);
	fflush (csp_debug_fp);
}

/*
====================
CL_CSP_LogRender

Log one row to the render CSV per entity per rendered frame.
Called from cl_ents.c ghost rendering block where both the
engine-interpolated position and the smoothed ghost position are available.

Captures:
  - engine_interpolated_origin: the position the engine would render
    (prev + lerpfrac * (current - prev))
  - raw_predicted: pre-smoothing prediction (from entity state)
  - smoothed_ghost: the actual displayed ghost position
  - smoothing state: active, offset, elapsed, decay
  - original_rendered: whether the original entity was also rendered
    (GHOST_BOTH = 1, GHOST_ONLY = 0)
====================
*/
void CL_CSP_LogRender (
	int ent_num, vec3_t engine_interpolated_origin,
	vec3_t smoothed_ghost_origin, qboolean original_entity_rendered,
	int ghost_mode)
{
	csp_entity_state_t *st;
	float elapsed_ms, decay;

	if (csp_debug->intvalue < 2 || !csp_render_fp)
		return;

	if (ent_num < 0 || ent_num >= CSP_MAX_ENTITIES)
		return;

	st = &csp_entities[ent_num];

	csp_render_frame++;

	elapsed_ms = (float)cls.realtime - (float)st->smooth_offset_time;
	if (elapsed_ms < 0.0f)
		elapsed_ms = 0.0f;
	decay = st->smooth_active ? expf (-elapsed_ms / csp_smooth_tau->value) : 0.0f;

	fprintf (csp_render_fp,
		"%d,"           // row_id
		"%d,%d,%d,"     // render_ms, serverframe, servertime
		"%d,"           // ent_num
		"%.1f,%.1f,%.1f,"  // engine_interpolated
		"%.1f,%.1f,%.1f,"  // raw_predicted
		"%.1f,%.1f,%.1f,"  // smoothed_ghost
		"%d,"           // smooth_active
		"%.2f,%.2f,%.2f,"  // smooth_offset
		"%.1f,%.4f,"  // smooth_elapsed_ms, smooth_decay
		"%d,"           // original_rendered
		"%d\n",         // ghost_mode
		csp_render_frame,
		(int)cls.realtime, cl.frame.serverframe, cl.frame.servertime,
		ent_num,
		engine_interpolated_origin[0], engine_interpolated_origin[1], engine_interpolated_origin[2],
		st->raw_predicted[0], st->raw_predicted[1], st->raw_predicted[2],
		smoothed_ghost_origin[0], smoothed_ghost_origin[1], smoothed_ghost_origin[2],
		st->smooth_active ? 1 : 0,
		st->smooth_offset[0], st->smooth_offset[1], st->smooth_offset[2],
		elapsed_ms, decay,
		original_entity_rendered ? 1 : 0,
		ghost_mode
	);
	fflush (csp_render_fp);
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

	// Update entity state and velocity (idempotent).
	// Section B: entity-absent reseed is now handled inside UpdateEntityVelocity
	// BEFORE the stored state advances, using cent->serverframe for actual
	// snapshot membership.
	// Section C: snapshot logging is now handled inside UpdateEntityVelocity
	// for every state transition (first-sight, reseed, invalid, successful).
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

	// Store raw (pre-smoothing) prediction for render CSV (Req 2 / Req 5)
	VectorCopy (out_origin, st->raw_predicted);

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

	// Section C: Snapshot logging is now handled in CL_CSP_UpdateEntityVelocity
	// for every state transition (first-sight, reseed, invalid, successful),
	// independent of whether a ghost is rendered.
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
