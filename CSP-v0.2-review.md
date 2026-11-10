# CSP v0.2 – Correctness & Diagnostics Review

**Scope:** Review of the tagged `v0.2-csp` implementation, preparing it for reliable
parameter tuning using recorded gameplay.

**Constraint honored:** No changes to prediction mathematics, velocity calculation,
horizon formula, or configurable parameters. Findings below are limited to
smoothing state management, logging, RTT feed placement, and documentation.

**Status:** Findings only. No source files modified.

---

## 1. Correctness issues

### 1.1 Smoothing snapshot-detection / call-order — **CONFIRMED DEFECT (High)**

Call order in `cl_ents.c:2016`:

```
CL_CSP_ShouldRenderGhost(cent, s1->number)   // cl_ents.c:2016
  └─> CL_CSP_UpdateEntityVelocity(cent, ent_num)   // cl_csp.c:805  ← ADVANCES STATE
CL_CSP_PredictEntity(cent, s1->number, ...)        // cl_ents.c:2032
  └─> detects "new frame" via st->cur_serverframe != cl.frame.serverframe
```

`CL_CSP_ShouldRenderGhost` (cl_csp.c:803-805) calls `CL_CSP_UpdateEntityVelocity`
**before** `CL_CSP_PredictEntity` runs. `UpdateEntityVelocity` is idempotent per
`serverframe` and **shifts `cur → prev` and overwrites `st->cur_origin` /
`st->velocity`** on the first call for a new frame.

Consequence: by the time `CL_CSP_PredictEntity` reaches its "Phase 2 smoothing"
block (cl_csp.c:636-655), the `is_new_frame` check
`st->cur_serverframe != cl.frame.serverframe` is **already false** (ShouldRenderGhost
just advanced it). So `is_new_frame` is **never true** in the normal render path, and
the smoothing offset is **never captured**. The `csp_smooth` feature is effectively
dead in the standard flow.

Additionally, even if it did fire, the "old prediction" is **reconstructed** from
`st->cur_origin + st->velocity * horizon` (cl_csp.c:648-650) — a recomputation, not
the *actual previously displayed position*.

**Impact:** `csp_smooth` / `csp_smooth_tau` have no observable effect in the normal
path. Genuine correctness defect (feature silently broken), not a math change.

### 1.2 Smoothing state reset — **PARTIALLY HANDLED, GAPS (Medium)**

`CL_CSP_ClearState` (cl_csp.c:105) resets `csp_ack_rtt_ms`, `csp_has_rtt`, debug fp,
and all `csp_entities[]`. The per-entity reset loop (cl_csp.c:120-135) clears
`smooth_offset` (via `VectorClear`) and `smooth_offset_time = 0`, but **does not set
`smooth_active = false`** — needs verification/fix.

Reset coverage:

| Trigger | Handled? | Notes |
|---------|----------|-------|
| Level change / disconnect | ✅ | `CL_ClearState` (cl_main.c:1159) → `CL_CSP_ClearState` (but see `smooth_active` gap) |
| Entity disappearance / reappearance | ⚠️ | `entity_not_in_recent_frame` reseed (cl_csp.c:660-671) clears velocity, **not** `smooth_active`/`smooth_offset` |
| Teleport | ⚠️ | `CSP_MAX_DISP_VEL` reseed (cl_csp.c:330-345) clears velocity, **not** smoothing state |
| Invalid timestamps | ⚠️ | `dt <= 0` path (cl_csp.c:300-312) clears velocity, **not** smoothing state |
| Velocity reseeding | ❌ | No reseed path resets `smooth_active` |

**Impact:** A stale `smooth_active = true` with a large `smooth_offset` can persist
across a respawn/teleport and inject a decaying offset into the *new* entity's position.

### 1.3 Missing-frame detection — **AMBIGUOUS (Medium)**

Reseed condition (cl_csp.c:660-671):

```c
if (st->initialized && st->cur_serverframe != cl.frame.serverframe - 1
    && st->cur_serverframe != cl.frame.serverframe)
```

This treats "serverframe gap > 1" as absence. But a **skipped serverframe** (dropped
packet, `cls.netchan.dropped`) produces the *same* signature as a genuine entity
absence. The two are not distinguished, and the reason string is a single generic
`"entity_not_in_recent_frame"`.

**Impact:** Cannot tell, from the log, whether a reset was caused by a real absence
or by packet loss.

### 1.4 Teleport handling vs engine events — **NOT INTEGRATED (Low)**

CSP detects teleports purely via displacement magnitude (`CSP_MAX_DISP_VEL`,
cl_csp.c:330). It does **not** consult the engine's `EV_PLAYER_TELEPORT` /
`EV_OTHER_TELEPORT` events or the large-origin-change logic in `cl_ents.c:194`
(`abs(...) > 512`). These are independent heuristics that can disagree (e.g., a fast
legit move near the 1500 u/s threshold vs. a small teleport). Not a defect per se,
but a divergence worth documenting.

### 1.5 Entity index / state-transition safety — **MOSTLY SAFE, ONE EDGE (Low)**

- Bounds checks present in both `PredictEntity` (cl_csp.c:600) and
  `ShouldRenderGhost` (cl_csp.c:786). ✅
- `CSP_MAX_ENTITIES` = 128; `cl.maxclients + 1` guard present. ✅
- **Edge:** `csp_entities[]` is indexed by `ent_num` directly. The guard uses
  `cl.maxclients + 1` while the array is 128. If `maxclients` were ever > 127,
  `ShouldRenderGhost` would allow indices up to 128 that are out of bounds. In
  practice Q2 caps at 100, so low risk, but the two bounds (128 vs maxclients+1)
  are not reconciled.

### 1.6 "Actual player position used for comparison" — **NOT THE INTERPOLATED POSITION (High for tuning)**

The debug log (cl_csp.c:745) compares the prediction against `cent->current.origin`
(the **raw latest snapshot** origin), passed as the `rendered` argument:

```c
CL_CSP_DebugLog (ent_num, out_origin, cent->current.origin, ...);
```

But the **actual rendered** player position is the engine-interpolated `ent.origin`
computed at cl_ents.c:1813 / 1830 / 1860:

```c
ent.origin[0] = cent->prev.origin[0] + cl.lerpfrac * (cent->current.origin[0] - cent->prev.origin[0]);
```

So the "diff" in the current CSV is **prediction vs. raw snapshot**, not
**prediction vs. what's actually on screen**.

**Impact:** Tuning against the current log measures the wrong reference.

---

## 2. Logging corrections — **CONFIRMED GAPS (High)**

Current single CSV (`CL_CSP_DebugLog`, cl_csp.c:490-560) problems:

1. **Mislabels smoothed output as raw.** The `predicted_x/y/z` column is
   `out_origin`, which *includes* the smoothing offset when `csp_smooth` is on
   (cl_csp.c:700-720). There is no separate raw-vs-smoothed column.
2. **Missing "actual original rendered origin."** It logs `cent->current.origin`
   (raw snapshot), not the interpolated `ent.origin`.
3. **Missing fields** (all absent from the header at cl_csp.c:210-228):
   - Render timestamp (only `local_receive_ms` present)
   - Serverframe delta, snapshot interval
   - Raw RTT sample (only smoothed `rtt_ms` present)
   - Acknowledged command sequence (`in` index from cl_scrn.c:122)
   - `csp_latency_ms`, `csp_horizon_ms`, `csp_smooth`, `csp_smooth_tau`
   - Smoothing offset / elapsed / decay factor
   - Model index, solid, event, effects, renderfx
   - Current & previous angles
4. **Invalid/reseeded states not logged when ghost not rendered.**
   `CL_CSP_DebugLog` is only called from `CL_CSP_PredictEntity` (cl_csp.c:745),
   which is only reached when `ShouldRenderGhost` returned true. Reseed/invalid
   events that cause `ShouldRenderGhost` to return **false** (cl_csp.c:809-811)
   are never logged.
5. **Always-true field:** `valid` column = `st->initialized && st->has_velocity` —
   but the log is only reached when the ghost renders, which already requires both
   true, so `valid` is **always 1** in the current output. This is exactly the
   "misleading field that is always true" the spec warns against.

---

## 3. Logging architecture — **NOT MET (High)**

There is **one** stream (snapshot-scoped, gated on render). The spec requires
**two** distinct streams:

- **Snapshot CSV** — one row per entity per authoritative snapshot, including
  invalid/reseed states.
- **Render CSV** — one row per entity per rendered frame, with raw prediction,
  smoothed output, actual rendered position, render time, smoothing state.

Neither exists as specified.

---

## 4. RTT diagnostics — **CONFIRMED DEFECT (High)**

`CL_CSP_UpdateRTT` is called inside `CL_AddNetgraph` (cl_scrn.c:124), **after**
this guard:

```c
if (scr_debuggraph->intvalue || scr_timegraph->intvalue || scr_sizegraph->intvalue)
    return;   // ← CL_CSP_UpdateRTT is NEVER reached when any debug graph is active
```

So **CSP RTT tracking stops entirely** whenever the user has a debug/time/size graph
enabled. The RTT feed is coupled to a visualization function.

Additional RTT gaps:

- Raw sample vs. smoothed are not both logged.
- The 150 ms fallback (cl_csp.c:420) is returned indistinguishably from a measured
  RTT — `CL_CSP_GetRTTMs` has no way for the logger to tell "fallback" from "acked."
- The acknowledged command sequence (`in`) and sample time are not recorded.

---

## 5. Timing-model documentation — **INACCURATE (Low, doc only)**

The header comment and `CL_CSP_CalculateHorizon` doc (cl_csp.c:430-444) state:

> "snapshot_age includes downstream travel time already"

But `snapshot_age_ms = cls.realtime - st->cur_receive_ms` (cl_csp.c:457) measures
**time since the snapshot was received locally** — i.e. render lag / frame
staleness, **not** downstream network delay. Downstream delay already elapsed
*before* `cur_receive_ms` was stamped and is not in this term. The doc conflates
"time since reception" with "downstream delay." Documentation defect only (no math
change).

---

## 6. Constraints — **RESPECTED**

No acceleration prediction, no regression velocity, no new EMA, no constant/cvar-default
changes, no math changes, no feature removals. All preserved. The defects above are in
*smoothing state management*, *logging*, and *RTT feed placement* — none require
touching the velocity or horizon math.

---

## Summary table

| # | Area | Status | Severity |
|---|------|--------|----------|
| 1.1 | Smoothing new-frame detection (call order) | **Broken** — `is_new_frame` never true; offset reconstructed not captured | High |
| 1.2 | Smoothing state reset on reseed/teleport | **Gap** — `smooth_active`/`smooth_offset` not cleared on reseed paths | Medium |
| 1.3 | Missing-frame vs skipped-frame | **Ambiguous** — single generic reason | Medium |
| 1.4 | Teleport vs engine events | **Divergent** heuristics, undocumented | Low |
| 1.5 | Index safety | Safe in practice; 128 vs maxclients+1 not reconciled | Low |
| 1.6 | Comparison uses interpolated position | **No** — uses raw snapshot | High (for tuning) |
| 2 | CSV mislabels smoothed as raw; missing fields; always-true `valid` | **Confirmed** | High |
| 3 | Two-stream architecture | **Not met** | High |
| 4 | RTT feed coupled to netgraph; fallback not flagged | **Confirmed** | High |
| 5 | snapshot_age doc inaccurate | **Confirmed** (doc only) | Low |

---

## Proposed implementation plan (for review — no code yet)

**Phase A — Correctness (no math changes):**

1. **Decouple smoothing capture from render gating.** Move the "capture previous
   displayed position" logic so it reads the *last actually-rendered* ghost origin
   (store `st->last_rendered_origin[3]` + `st->last_rendered_time` at the point the
   ghost is drawn in `cl_ents.c`), rather than reconstructing from
   `cur_origin + velocity*horizon`. Fixes 1.1 and satisfies "capture actual
   previously displayed position."
2. **Add a `CL_CSP_ResetSmoothing(st)` helper** clearing `smooth_active`,
   `smooth_offset`, `smooth_offset_time`; call it from every reseed path (teleport,
   invalid dt, entity-not-in-frame, velocity-cleared) and from `CL_CSP_ClearState`.
   Fixes 1.2.
3. **Distinguish reset reasons.** Add distinct reason codes: `entity_absent`,
   `serverframe_skipped` (detect via `cls.netchan.dropped` or frame gap),
   `teleport_disp`, `invalid_dt`, `velocity_cleared`. Fixes 1.3.
4. **Reconcile index bounds** — clamp `ent_num` to `min(CSP_MAX_ENTITIES,
   cl.maxclients+1)` in one shared helper. Fixes 1.5.
5. **Log the interpolated rendered position** — pass the engine's `ent.origin`
   (cl_ents.c:1813/1830/1860) into the render log, not `cent->current.origin`.
   Fixes 1.6.

**Phase B — RTT decoupling:**

6. **Move the RTT sample out of `CL_AddNetgraph`.** Compute
   `in = cls.netchan.incoming_acknowledged & (CMD_BACKUP-1);
   ping = cls.realtime - cl.cmd_time[in];` at the point a new frame is
   acknowledged (independent of any graph cvar), and call `CL_CSP_UpdateRTT` there.
   Keep the netgraph drawing separate. Fixes 4.
7. **Track RTT source explicitly** — add `csp_rtt_source` enum
   (`OVERRIDE` / `ACKED` / `FALLBACK`) + `csp_rtt_ack_seq` (the `in` index) +
   `csp_rtt_sample_time`, exposed to the logger. Fixes 4.

**Phase C — Two-stream logging:**

8. **Snapshot CSV** (`csp_snap.log`): one row per entity per authoritative snapshot,
   emitted from `CL_CSP_UpdateEntityVelocity` (which already runs once per snapshot
   per entity). Includes invalid/reseed states, all entity fields (modelindex,
   solid, event, effects, renderfx, cur/prev angles), raw+smoothed RTT, all cvars,
   horizon, reason codes.
9. **Render CSV** (`csp_render.log`): one row per entity per rendered frame, emitted
   from the render path in `cl_ents.c`. Includes render timestamp, raw predicted
   origin, smoothed ghost origin, actual interpolated rendered origin, smoothing
   offset/elapsed/decay.
10. **Remove the always-true `valid` column** from the snapshot stream (replace with
    explicit `velocity_valid` + `reseeded` + `reason`).

**Phase D — Documentation:**

11. Correct the `snapshot_age` doc to state it measures *time since local reception
    (render staleness)*, and add a timing-model note distinguishing reception-time /
    downstream / upstream / RTT.

**Deliverables after plan approval:** updated source, the two CSV schemas with one
example row each, and a verification report (snapshot log fires once per snapshot;
render log fires per rendered frame).

---

## Open questions (before code changes)

1. **Phase A.1** changes *where* the smoothing offset is captured (from a
   reconstruction to the real last-rendered position). This does **not** change the
   velocity or horizon math — it only fixes the broken capture. Is that within the
   "don't change prediction mathematics" constraint, or should `csp_smooth` be left
   entirely as-is and only logging fixed?
2. For **Phase B.6**, moving the RTT sample out of `CL_AddNetgraph` means adding a
   small hook at frame-ack time. Drive it from `CL_ParseFrame` (where
   `CL_CSP_ParseFrameNotify` already fires) or from the netchan ack path?
   Recommendation: `CL_ParseFrame` for locality.

---

## Implementation status (Phases A–D)

All four phases are implemented on branch `feature/csp-latency-investigation`:

| Phase | Commit | Scope |
|-------|--------|-------|
| A | `d2ff178` | State correctness + smoothing lifecycle |
| B | `0d5e69e` | RTT sampling decoupling |
| C | `bd18f79` | Snapshot + render logging (two-stream CSV) |
| D | `3924850` | `snapshot_age` timing-model documentation |

Each phase built clean (x86 Release, zero warnings). No prediction values,
velocity math, horizon formula, EMA behaviour, cvar defaults, or thresholds were
changed.

## Req 6 — 10-point verification evidence

Evidence is cited by file and function. Line numbers are approximate (post-edit).

**1. Smoothing capture executes when a new snapshot is processed.**
`CL_CSP_PredictEntity` (cl_csp.c) captures the offset only when
`st->smooth_captured_for_frame != cl.frame.serverframe && st->last_rendered_valid`.
Because `CL_CSP_UpdateEntityVelocity` is idempotent per `serverframe`, the first
render for a new authoritative snapshot is the one that re-captures
`smooth_offset = last_rendered_ghost - new_predicted`. Subsequent renders in the
same snapshot skip capture and only apply the decaying offset.

**2. Smoothing is reset on all required state transitions.**
`CL_CSP_ResetSmoothing(st)` is called on every reseed path in
`CL_CSP_UpdateEntityVelocity` (invalid dt, serverframe skipped, teleport disp) and
in `CL_CSP_PredictEntity` (entity absent, velocity cleared). It clears
`smooth_active`, `smooth_offset`, `smooth_offset_time`, `last_rendered_valid`, and
`smooth_captured_for_frame`.

**3. Snapshot logging occurs once per entity per authoritative snapshot.**
`CL_CSP_LogSnapshot` dedupes via `st->snap_logged_serverframe == cl.frame.serverframe`
early-return. Multiple `PredictEntity` calls for the same `serverframe` (body,
weapon, linked) therefore emit exactly one CSV row.

**4. Render logging occurs at render frequency.**
`CL_CSP_LogRender` is called from the ghost block in cl_ents.c (line ~2034) on every
rendered frame for the entity, and increments `csp_render_frame` per row. It is not
deduped by serverframe — it fires at render rate.

**5. Raw prediction is independent of smoothing.**
In `CL_CSP_PredictEntity`, `out_origin` is computed from
`cur_origin + velocity * (horizon/1000)` and stored to `st->raw_predicted`
*before* the smoothing block adds `smooth_offset * decay`. The raw value is never
mutated by smoothing.

**6. Original rendered position is genuinely the engine-interpolated position.**
cl_ents.c line ~1860 sets
`ent.origin[i] = cent->prev.origin[i] + entity_lerp * (cent->current.origin[i] - cent->prev.origin[i])`
with `entity_lerp = cl.lerpfrac`. This `ent.origin` is passed unchanged to
`CL_CSP_LogRender` as `engine_interpolated_origin`, before the ghost block overwrites
it with `predicted_origin`.

**7. RTT continues updating with all netgraph settings enabled.**
`CL_CSP_SampleRTT` is called unconditionally from cl_main.c after
`CL_ParseServerMessage` (Phase B), independent of `CL_AddNetgraph`. The RTT call was
removed from cl_scrn.c's netgraph path, so netgraph on/off no longer gates sampling.

**8. RTT samples are not duplicated.**
`CL_CSP_SampleRTT` dedupes by `cls.netchan.incoming_acknowledged`:
`if (ack_seq == csp_last_acked_seq) return;`. A repeated ack sequence does not
re-feed the EMA.

**9. Invalid/reseeded states appear in the snapshot log.**
`CL_CSP_LogSnapshot` emits `has_velocity`, `reseeded`, `reseed_reason`
(`entity_not_in_recent_frame`, `velocity_cleared`, `teleport_disp`, `invalid_dt`,
`serverframe_skipped`), and `snap_age_clamped`. `CL_CSP_ResetReasonString` maps the
`csp_reset_reason_t` enum to these strings.

**10. CSV headers and data columns match exactly.**
Snapshot CSV header (cl_csp.c ~line 395) lists 44 columns; the `fprintf` in
`CL_CSP_LogSnapshot` emits exactly 44 comma-separated fields in the same order.
Render CSV header lists 24 columns; the `fprintf` in `CL_CSP_LogRender` emits exactly
24 fields in the same order. The `valid` column was removed; `has_velocity` is the
meaningful indicator.
