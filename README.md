# r1q2 (csp mod)

A fork of [R1Q2](http://r1ch.net/r1q2/) — R1CH's enhanced Quake II client — with **client-side prediction (CSP)** for reduced perceived latency.

> **This project is a fork of R1CH's R1Q2.** All original engine code, architecture, and design are R1CH's work. See [`readme.txt`](readme.txt) for the original R1Q2 documentation. This fork adds the CSP subsystem and associated diagnostics; everything else is upstream.

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
