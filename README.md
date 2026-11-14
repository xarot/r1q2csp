# R1Q2 (mod)

R1CH's R1Q2 client for Quake II, made to build with Visual Studio 2022, with a
few additions for playing on modern displays and on servers with some ping.
It reports itself as `R1Q2 (mod) b8012-premod` and works with any normal
Quake II / R1Q2 server; nothing is needed on the server side.

Additions:

- [Prediction of other players](#player-prediction) (off by default)
- [Immediate feedback for your own weapons](#weapon-prediction) (comes with player prediction)
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

Prediction is meant to make up for a high ping, not to give players with a
good connection something extra. It therefore only works while your measured
ping is 100 ms or more. Below that, other players are drawn exactly as in
the normal game, whatever the settings are. (It switches on at 105 ms and off
again below 100 ms, so that it does not flicker at the limit.)

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

| Your ping | Setting | Lead left for you | Lead with prediction off | Model in the right place |
|-----------|---------|-------------------|--------------------------|--------------------------|
| 100 ms    | 50      | 50 ms             | 200 ms                   | about 85%                |
| 100 ms    | 100     | none              | 200 ms                   | about 65%                |
| 150 ms    | 0       | 150 ms            | 250 ms                   | about 99%                |
| 150 ms    | 50      | 100 ms            | 250 ms                   | about 85%                |
| 150 ms    | 75      | 75 ms             | 250 ms                   | about 75%                |
| 150 ms    | 100     | 50 ms             | 250 ms                   | about 65%                |
| 150 ms    | 150     | none              | 250 ms                   | about 50%                |

A higher value means less leading, but more guessing. The last column depends
only on how much ping is actually compensated (your ping or the setting,
whichever is lower):

| Ping compensated | Model in the right place | Average distance from the real position |
|------------------|--------------------------|------------------------------------------|
| 0 ms             | about 99%                | 4 units                                  |
| 50 ms            | about 85%                | 8 units                                  |
| 75 ms            | about 75%                | 11 units                                 |
| 100 ms           | about 65%                | 14 units                                 |
| 150 ms           | about 50%                | 20 units                                 |
| 200 ms           | about 40%                | 29 units                                 |

"In the right place" means within 16 units of where the player really was,
which is half the width of a player (32 units). The numbers come from replaying
the recorded movement of every player in a logged free-for-all game (about
4,000 samples), so take them as a rough guide: they will be better against
players who move steadily and worse in close fights. With prediction off the
model is never in the right place for a moving player, which is why you lead.

If your ping is at or below the setting, raising it further changes nothing.
A good starting point is 50: get used to the shorter lead, then try 75 or
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

### Why not simply compensate the whole ping?

Because the client would have to know what the other player is going to do
before they have done it.

Think of throwing a ball to someone who is running. If the ball is in the air
for a blink of an eye, you throw it at them and it arrives. If it is in the
air for a quarter of a second, you have to throw it to where they will be, and
that only works if they keep running the same way. If they stop or turn while
the ball is in the air, it lands where they would have been.

Prediction is the client making that guess for you. With a ping of 150 ms it
has to guess about a quarter of a second ahead (the 100 ms update delay plus
your ping). A player at full speed covers about 75 units in that time, more
than two body widths, and a good player changes direction several times a
second. The key press that makes them turn has not happened yet when the
client draws the picture, so no amount of cleverness can show it. In logged
games a guess that far ahead was right only about half of the time.

A short guess is right nearly every time, because nobody can change course
much in a tenth of a second. So the practical choice is between two things:

- compensate everything: you aim straight at the model, but it is often in
  the wrong place when a player dodges, and you cannot tell when;
- compensate part of it: the model is where the player really will be most of
  the time, and you lead the rest yourself, using your own judgement of what
  that player is about to do. People are better at that part than a formula.

The only real cure for the whole delay is lag compensation on the server,
where the server rewinds players to what you saw when you fired. Quake II
servers do not do that, and a client cannot add it.

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

## Weapon prediction

### What it does

In Quake II the server decides when your weapon fires and tells the client
afterwards. Everything you see and hear of your own shot (the muzzle flash,
the sound, the gun moving in your hand, the railgun trail) therefore comes one
ping after you pulled the trigger, and at a ping of 150 ms that is clearly
noticeable.

Weapon prediction is part of player prediction: it is in use whenever player
prediction is (`cl_predict_players 1` and a ping of 100 ms or more), and never
otherwise, so a normal configuration is not affected. The client then runs the
same weapon logic as the game itself and shows those things the moment you
press fire. When the server's own report of the shot arrives, it is recognised
and not shown a second time.

To use player prediction without it, set `cl_predict_weapons 0` (menu:
**Multiplayer > Player prediction > Predict own weapons**; default `1`).

| Shown at once                              | Still comes from the server, one ping later |
|--------------------------------------------|---------------------------------------------|
| muzzle flash and firing sound              | rockets, grenades, blaster bolts and the BFG ball appearing |
| the gun's firing and reload animation      | bullet and pellet impacts, blood, explosions |
| the railgun trail                          | the view kick, quad damage sound, chaingun spin sounds |
| repeat fire while the button is held       | hand grenades (not predicted at all) |

Projectiles are left to the server on purpose. One drawn at once would have
to jump backwards when the real one appears, because the real one only leaves
the gun when your command reaches the server.

### Should the delay be removed, or should it match the lead?

It is removed completely, and that is the right amount. The feedback only
tells you "you fired now", and that is true the moment you press the button,
whatever your ping is. It is not connected to how far you have to lead: the
shot still reaches the server one half of your ping later, and the server
still decides what it hits. So the ping compensation setting above keeps its
meaning, and nothing about hitting changes; the game only stops feeling as if
the trigger were late.

### Limits

- It knows the ten standard weapons by their view models. With any other
  weapon (mods with their own guns) it does nothing and the server's feedback
  is shown as before.
- If a server's weapons behave differently from the standard game (other fire
  rates, for example), predicted shots will not be confirmed. After three in a
  row the prediction switches itself off for five seconds at a time.
- A predicted shot can be wrong: for example when you die, run out of ammo or
  the server switches your weapon in the instant before your command arrives.
  You then see and hear a shot that never happened. The railgun trail is the
  most visible case.
- For a short moment after you change weapons nothing is predicted, because
  the client cannot know yet which weapon the server will fire.
- A silencer is only known after the first confirmed shot, so the first
  silenced shot sounds loud to you.

`cl_predict_weapons_debug 1` prints every predicted shot, every confirmation
and every correction to the console.

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
