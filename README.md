# R1Q2 (mod)

R1CH's R1Q2 client for Quake II, made to build with Visual Studio 2022, with a
few additions for playing on modern displays and on servers with some ping.
It reports itself as `R1Q2 (mod) b8012-premod` and works with any normal
Quake II / R1Q2 server; nothing is needed on the server side.

Additions:

- [Prediction of other players](#player-prediction) (off by default)
- [Widescreen video modes and automatic FOV](#video-modes-and-fov)
- [Scaling of the HUD, scores, console and crosshair](#hud-scaling)
- 24-bit FPU precision fix, so movement feels the same as in the original builds

R1CH's original notes are in `readme.txt` and `r1q2.txt`. Build instructions
are in `BUILDING-VS2022.md`.

## Installing

Build the `Release | x86` configuration and copy these from
`build\bin\Release\` into your Quake II folder:

| File           | Needed for                                   |
|----------------|----------------------------------------------|
| `r1q2.exe`     | everything                                   |
| `ref_r1gl.dll` | HUD scaling (the old DLL works, but unscaled) |
| `gamex86.dll`  | only for hosting games; goes into `baseq2\`  |

## Player prediction

### What it does

A Quake II server sends the positions of other players ten times a second and
does not compensate for lag. The client normally shows other players slightly
in the past, and your shot takes your ping to reach the server. So to hit a
moving player you have to aim ahead of them by roughly

    your ping + 100 ms

With prediction on, the client instead draws other players where it expects
them to be when your shot arrives. It takes the movement seen in the latest
server updates and runs it forward: along the ground, up and down stairs, with
gravity while airborne, riding lifts, and stopping at walls. Your ping is
measured automatically all the time, so nothing has to be entered for it.

The forecast can only assume that a player keeps doing what they are doing.
Someone running or flying in a straight line is drawn in the right place.
Someone who changes direction is drawn wrong until the next server update
shows the change. The further ahead the client has to guess, the more often
that happens, and that is what the settings below trade off.

### Settings

All of them are in the menu under **Multiplayer > Player prediction**, with a
short explanation shown for the selected item, and can also be set in the
console. They are saved in your config.

| Menu item             | Cvar                         | Default | Meaning |
|-----------------------|------------------------------|---------|---------|
| Predict other players | `cl_predict_players`         | `0`     | `1` turns prediction on. |
| Ping compensation     | `cl_predict_players_maxping` | `50`    | How many milliseconds of your ping are compensated at most. |
| Smoothing             | `cl_predict_players_smooth`  | `30`    | Time in milliseconds over which a correction is blended in. |
| Fine tuning           | `cl_predict_players_offset`  | `0`     | Milliseconds added to (or, negative, taken from) the measured ping. |

**Ping compensation** decides how far ahead the client guesses. The delay of
the server updates (about 100 ms) is always compensated; on top of that comes
your ping, up to this limit. What is left over you still lead yourself:

    lead left for you = your ping - ping compensation   (never below 0)

| Your ping | Setting | Lead left for you | Lead with prediction off |
|-----------|---------|-------------------|--------------------------|
| 50 ms     | 50      | none              | 150 ms                   |
| 100 ms    | 50      | 50 ms             | 200 ms                   |
| 150 ms    | 50      | 100 ms            | 250 ms                   |
| 150 ms    | 150     | none              | 250 ms                   |

A higher value means less leading, but more guessing. As a rough guide from
logged deathmatch games, the predicted position was within a player's body
width of the truth about 85% of the time when guessing 100 ms ahead, 65% at
150 ms and 40% at 250 ms. If your ping is at or below the setting, raising it
further changes nothing. Good starting points:

- ping up to about 60: leave it at 50, everything is compensated;
- higher ping: start at 50 and get used to the shorter lead, then try 75 or
  100. Setting it to your full ping removes the lead completely, but players
  who dodge will often not be where they are drawn.

**Smoothing** applies when a new server update shows that the forecast was
wrong. Instead of jumping to the corrected position, the model slides there
over this time. Low values are more accurate but look jumpier when a player
dodges; high values look smoother, but the model spends more time in the
wrong place. `0` turns it off. Values much above 60 make players noticeably
harder to hit.

**Fine tuning** is for the case where a player running in a straight line is
consistently drawn too far back or too far ahead. Raise it if you still have
to lead such a player, lower it if your shots pass in front of them. It is
normally left at 0.

### What to expect

- Hitscan weapons (machinegun, chaingun, railgun, shotguns) benefit the most.
  Rockets, grenades and blaster bolts still need leading for their flight time.
- Prediction helps most against players who are running, falling or flying
  on a steady course, and least in close fights with a lot of dodging.
- It makes no difference to how other players see you, or to what the server
  counts as a hit. It only changes where the client draws other players.
- `cl_async 0` and `cl_async 1` behave the same.

### Diagnostics

These are not saved in the config.

| Cvar                           | Meaning |
|--------------------------------|---------|
| `cl_predict_players_debug -1`  | Prints the current lead and ping estimate now and then. |
| `cl_predict_players_debug -2`  | Accuracy meter: compares earlier forecasts with where players really ended up and prints a summary. |
| `cl_predict_players_debug N`   | Prints details for the player with entity number N. |
| `cl_predict_players_log 1`     | Writes a log of every shot you fire to `<gamedir>\pplog\pp_<date>_<time>.log`: where you aimed, where each nearby player was drawn, where they really were, and whether the shot hit. It also records every player's movement each server frame. |

The header of each log file explains its fields. `tools\pplog\` has two awk
scripts that summarise a log: `shot_stats.awk` (hits and misses by cause) and
`movement_replay.awk` (how well different forecast lengths would have done on
the recorded movement).

## Video modes and FOV

Added to the video menu, and usable as `gl_mode`:

| Mode | Resolution | Mode | Resolution |
|------|------------|------|------------|
| 15   | 1280x720   | 22   | 2560x1600  |
| 16   | 1366x768   | 23   | 3440x1440  |
| 17   | 1600x900   | 24   | 3840x1600  |
| 18   | 1920x1080  | 25   | 3840x2160  |
| 19   | 1920x1200  | 26   | 5120x1440  |
| 20   | 2560x1080  | 27   | desktop resolution |
| 21   | 2560x1440  |      |            |

`cl_widescreen_fov` (default `1`) widens the view on wide screens so that the
picture keeps the same height as on a 4:3 screen and shows more at the sides.
`fov` then means the 4:3 value, so `fov 90` looks the way it always did. Set
`cl_widescreen_fov 0` to have `fov` used exactly as given.

## HUD scaling

Each of these takes a whole-number factor. `0` (the default) picks one
automatically from the screen height (1 below 1080 lines, 2 at 1080, 4 at
2160); `1` is the original size.

| Cvar                 | Scales |
|----------------------|--------|
| `scr_hudscale`       | status bar (health, ammo, armour) and centre-printed text |
| `scr_scorescale`     | scoreboard, other server layouts and the inventory |
| `scr_conscale`       | console and the chat HUD |
| `scr_crosshairscale` | crosshair |

Scaling needs the `ref_r1gl.dll` from this build. With another renderer
everything is drawn at the original size, except the crosshair, which scales
with any renderer.
