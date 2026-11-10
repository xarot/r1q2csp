# r1q2 (csp mod)

A fork of [R1Q2](http://r1ch.net/r1q2/) — R1CH's enhanced Quake II client — with **client-side prediction (CSP)** for reduced perceived latency.

> **This project is a fork of R1CH's R1Q2.** All original engine code, architecture, and design are R1CH's work. See [`readme.txt`](readme.txt) for the original R1Q2 documentation. This fork adds the CSP subsystem and associated diagnostics; everything else is upstream.

## Why this fork exists

We've been playing Quake II since it shipped in December 1997. The community that built those late-night frag-fests is getting smaller every year — people are aging, moving on, or simply gone. What remains is a scattered group of players spread across time zones: someone in Stockholm with 5 ms ping, someone in Toronto with 150 ms, someone in Sydney with 300 ms. The game was designed for a LAN room full of friends, not for a global lobby where the slowest connection sets the pace for everyone.

Modern game clients solved this with **client-side prediction**: the client guesses where things will be so you don't have to wait for the server to catch up. Quake II already does this for *your own movement* — try `cl_predict 1` and you'll feel the difference immediately. But that's all. The moment you raise your shotgun at an opponent, the prediction stops. You're still aiming at where the server *last told* the enemy was, which at 200 ms round-trip can be a full second old.

**Wayne Gretzky said: "I skate to where the puck is going to be, not where it has been."** That's exactly what this fork does, but for every entity on the screen.

### How it works (the short version)

Think of it like watching a car drive down a highway through a slightly delayed video feed. You see the car at position A, then a moment later at position B. Your brain instantly knows: "It's moving from A to B, so right now it's probably a bit past B." You don't wait for the next frame — you *extrapolate*.

That's what CSP does for every player, monster, and projectile:

1. **Measure the delay** — how old the latest server snapshot is, plus the round-trip time.
2. **Estimate the velocity** — how far the entity moved between two snapshots, divided by the time that took.
3. **Project forward** — last known position + velocity × delay = where the entity *should be right now*.
4. **Render a ghost** — that predicted position is drawn as a translucent overlay on top of the server-authoritative model.

**Where do you aim?** At the ghost, not the solid model. The solid model is where the server says the enemy *was*. The ghost is where the client believes the enemy *is*. At 5 ms ping the two are nearly identical. At 250 ms ping the ghost can be several meters ahead — and that's the difference between a clean headshot and a spray into empty air.

**The goal: make a 250 ms connection feel like 50 ms, so that Stockholm, Toronto, and Sydney can frag each other in the same room.**

---

## What is CSP?

CSP (Client-Side Prediction) renders a **ghost entity** at a predicted future position for each remote player, compensating for network latency. The server's authoritative position is still used for hit detection and game logic — CSP only affects what you *see*.

### Prediction model

```
predicted = cur_origin + velocity × (horizon / 1000)

horizon   = snapshot_age + (RTT × csp_mult / 2) + csp_horizon_bias
velocity  = (cur_origin − prev_origin) / Δservertime
```

The ghost is rendered as a translucent overlay on top of the server-authoritative entity, so you can see both the "true" position and the predicted position simultaneously.

---

## CSP Cvars

| Cvar | Default | Description |
|------|---------|-------------|
| `csp_enable` | `1` | `0` = off, `1` = ghost + both, `2` = ghost only |
| `csp_latency_ms` | `0` | Manual latency override in ms (0 = auto-detect via RTT) |
| `csp_mult` | `1.0` | Multiplier applied to RTT/2 in the horizon calculation |
| `csp_horizon_ms` | `0` | Fixed horizon override in ms (0 = auto) |
| `csp_horizon_bias` | `0` | Additional bias added to horizon (ms) |
| `csp_smooth` | `0` | Enable exponential smoothing of the ghost offset |
| `csp_smooth_tau` | `80` | Smoothing time constant (ms) |
| `csp_debug` | `0` | Enable per-frame debug logging |
| `csp_debug_file` | `csp_debug.log` | Output file for debug logs |

### Quick start

```cfg
// In autoexec.cfg — CSP is on by default
set csp_enable 1
set csp_debug 1          // enable logging while testing
set csp_debug_file "csp_debug.log"
```

---

## Build (Windows / VS2022)

**Prerequisites:**
- Visual Studio 2022 (Community or higher) with "Desktop development with C++"
- [vcpkg](https://github.com/microsoft/vcpkg) at `C:\vcpkg` (triplet: `x86-windows-static-md`)
- Dependencies: `zlib`, `libpng`, `libjpeg-turbo` (installed via `vcpkg.json`)

**Build (x86 / Win32):**

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86
msbuild r1q2.sln /p:Configuration=Release /p:Platform=x86 /m /v:minimal /nologo
```

Output: `build\bin\Release\r1q2.exe`

> **Note:** The solution targets **Win32 (x86)**, not x64. This is intentional — the engine uses IA32/x87 math paths.

---

## Project structure

```
r1q2csp/
├── client/
│   ├── cl_csp.c          # CSP prediction, ghost rendering, diagnostics
│   ├── cl_csp.h          # CSP public interface
│   ├── cl_ents.c         # Entity parsing (CSP frame-validation hooks)
│   ├── cl_main.c         # Client main (CSP cvar registration)
│   └── ...
├── qcommon/
│   └── qcommon.h         # Shared defines (version string, product name)
├── game/                 # Game logic DLL (unmodified from upstream)
├── ref_gl/               # R1GL renderer (unmodified from upstream)
├── server/               # Dedicated server (unmodified from upstream)
├── build.h               # Build number (8012)
├── r1q2.sln              # VS2022 solution
└── vcpkg.json            # vcpkg dependency manifest
```

---

## Versioning

| Tag | Description |
|-----|-------------|
| `v0.1-vanilla` | Clean fork of upstream R1Q2 b8012, VS2022 build |
| `v0.2-csp` | Initial CSP port (ghost rendering, basic prediction) |
| `v0.3-csp` | Full CSP: validated frame gating, snapshot lifecycle, RTT decoupling, diagnostics |

The client reports its version as **`r1q2 (csp mod) b8012`** in the console and via `!versions`.

---

## Attribution & licensing

- **R1Q2 engine** — Copyright R1CH (r1ch.net). All original client, server, renderer, and game code.
- **CSP subsystem** (`cl_csp.c`, `cl_csp.h`, and related hooks) — Added by this fork.
- **Quake II** — Copyright id Software. R1Q2 is a non-commercial modification.

This fork is provided as-is, for non-commercial use, in the spirit of the original R1Q2 project.

---

## Links

- Original R1Q2: [http://r1ch.net/r1q2/](http://r1ch.net/r1q2/)
- R1Q2 forum: [http://r1ch.net/forum/](http://r1ch.net/forum/)
- R1GL renderer: [http://r1ch.net/stuff/r1gl/](http://r1ch.net/stuff/r1gl/)
- This fork: [https://github.com/xarot/r1q2csp](https://github.com/xarot/r1q2csp)
