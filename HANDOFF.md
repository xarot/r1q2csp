# Handoff: prediction modes (branch `feature/prediction-modes`)

Notes for whoever continues this work (written for Claude Sonnet picking it up
in a new session). Delete this file before merging to `master`.

## Rules the owner has set

- This line of work started from tag `v0.1-vanilla`. **Do not read or copy from
  the older CSP attempt**: the `r1q2csp` working tree next to this folder, the
  local branches `feature/csp-latency-investigation`,
  `feature/ghost-normal-appearance`, `feature/q3-player-prediction`, the tags
  `v0.2-csp` to `v0.5-csp`, or any old analysis/diff/log files beside the repo.
- Client only. Nothing may depend on server changes; only data a normal
  Quake II / R1Q2 server already sends may be used.
- Must behave the same with `cl_async 0` and `1`.
- Commit each change separately. Push, tag and release only when asked.
- The README must not mention Russia. Credit line stays:
  "Additions by Dakotha / Clan Kontula, built on R1Q2 by R1CH".
- The owner tests by playing on real servers (ping about 150, sometimes 280)
  and brings back logs. Nothing here can be tested automatically.

## Building

    "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" r1q2.sln -p:Configuration=Release -p:Platform=x86 -m -v:m -nologo

Output: `build\bin\Release\r1q2.exe` (plus `ref_r1gl.dll`, `gamex86.dll`).
The owner copies the exe into the game folder by hand. There is no Python on
this machine; the log tools are awk scripts in `tools\pplog\`.

## Where things are

| File | What |
|---|---|
| `client/cl_predplayers.c` | Forecast of other players, ping/clock estimate, the modes, the shot log |
| `client/cl_predweapons.c` | Prediction of the player's own weapon (flash, sound, gun frames, rail trail) |
| `client/menu.c` | Multiplayer > Player prediction menu (`Prediction_MenuInit`, `prediction_help`) |
| `README.md` | User documentation |

## What this branch adds (all compiles; **none of it has been played yet**)

`cl_predict_players 0/1` is still the master switch. New cvars, all archived:

| Cvar | Default | Meaning |
|---|---|---|
| `cl_predict_players_mode` | 0 | 0 fixed (old behaviour), 1 per weapon, 2 adaptive |
| `cl_predict_players_setup` | 0 | 0 easy (built-in values from the ping), 1 advanced (cvars below) |
| `cl_predict_players_stream` | -1 | Machinegun, chaingun. ms, -1 = automatic |
| `cl_predict_players_burst` | -1 | Shotgun, super shotgun, railgun |
| `cl_predict_players_projectile` | -1 | Rockets, grenades, cells, blaster |
| `cl_predict_players_minping` | 0 | No prediction below this ping. 0 = no limit |

Compensation in ms (`PP_CompRange`), `ping` being the measured round trip:

|                    | stream    | burst    | projectile        | minimum (adaptive only) |
|--------------------|-----------|----------|-------------------|-------------------------|
| per weapon, easy   | 2/3 ping  | 2/3 ping | 1/3 ping          | -                       |
| adaptive, easy     | ping + 50 | ping     | ping, 100 at most | 1/3 ping                |
| advanced, auto (-1)| ping + 50 | ping     | ping, 100 at most | `cl_predict_players_maxping` |

- The weapon class comes from the ammo icon (`PP_WeaponClass`), not the gun
  model, so it works with `cl_gun 0`. Unknown icon = fixed mode.
- Adaptive: on every snapshot `PP_MeasureMiss` checks how far a 100 ms forecast
  from the previous snapshot landed from the new one (errors along our line of
  sight count half). `PP_Confidence` maps that miss from `PP_MISS_GOOD` (6) to
  `PP_MISS_BAD` (18) onto 1..0, and `PP_PlayerComp` moves each player's
  compensation between minimum and maximum at a limited rate
  (`PP_COMP_RISE`/`PP_COMP_FALL`). **These four constants are first guesses.**
- The weapon feedback delay (`WP_Delay`) follows the weapon's compensation in
  modes 1 and 2 (`CL_PP_WeaponComp`).
- The old fixed 100 ms ping requirement was removed at the owner's request and
  replaced by the optional `cl_predict_players_minping`.
- "Fine tuning" (`cl_predict_players_offset`) was taken out of the menu. The
  cvar still works in mode 0 only.

## Tuning from logs

`cl_predict_players_log 1` writes `baseq2\pplog\pp_<date>_<time>.log` in the
game folder (`D:\Pelit\Q2 uudet säädöt 2026`). The header explains the fields.
New for this branch:

- `SHOT ... mode= wclass= comp=<min>-<max>`
- `TGT ... comp= miss=` (what that target had when the shot was fired)
- `TRK ... miss= comp=` (every player, every server frame)

Useful checks with `tools\pplog\shot_stats.awk` and ad-hoc awk:

1. Distribution of `miss` for players moving straight vs. changing direction,
   to set `PP_MISS_GOOD` / `PP_MISS_BAD`. If straight runners often sit above
   6, raise it (the server moves players once per packet, which adds a few
   units of noise).
2. `err` and `ms` in TGT lines grouped by `comp`, to see whether high
   compensation targets really are the accurate ones.
3. Per weapon class: `ms` (lead left) and `err`, compared with these figures
   measured before this branch at ping about 150, held fire:
   compensation 50 -> about 135 ms behind, 17 units average forecast error;
   whole ping -> about 70-75 ms behind, 35 units.

## Open items

1. **README is not updated for this branch.** It still describes the 100 ms
   limit as fixed (sections "What it does", "Fair play", "Weapon prediction")
   and does not mention modes, setup, the weapon sliders or `minping`, and it
   still lists fine tuning as a menu item.
2. Menu: all nine items are always shown. Greying out or hiding the weapon
   sliders outside advanced setup was planned but not done.
3. Weapon prediction still needs `cl_gun 1` (with `cl_gun 0` an R1Q2 server
   does not send the gun frame). Offered to the owner, not yet asked for:
   keep the gun hidden but still request the frames.
4. Weapon prediction known leftovers: a phantom shot when clicking in the
   moment of death; the gun animation jumps once on about 1 shot in 20 when
   the command lands one server frame later than estimated; no shots should
   be predicted during OpenTDM pauses/countdowns (not handled).
5. Ideas discussed, not built: a QuakeWorld-style option (no animation/angle
   interpolation; smoothing 0 already gives most of it); separate railgun
   class; console cvars for the adaptive thresholds.
6. Owner has not decided whether to push this branch. `master` is at tag
   `v1.1` (released on GitHub with a zip).
