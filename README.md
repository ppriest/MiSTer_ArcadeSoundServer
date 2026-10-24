# MiSTer Arcade Sound Server

`arcsnd`: one process on the MiSTer's ARM (the HPS) that runs the sound boards of arcade cores too
large to keep their sound CPU in the FPGA. The core passes its main CPU's writes to the sound
board through a block of DDR3; the server emulates the board from MAME's code and plays it through
the framework's audio path. It watches `/tmp/CORENAME` and runs the session of the board the loaded
set has.

| Board | Sets | Core | Session |
|---|---|---|---|
| Konami Hornet: 68000, K056800, RF5C400 | gradius4 and clones; nbapbp, nbaatw and clones | `Arcade-KonamiHornet_MiSTer` | `src/hornet/` |
| SNK Hyper NeoGeo 64: V53A, L7A1045 | sams64, sams64_2, fatfurwa, buriki, roadedge, xrally, bbust2 | `Arcade-HyperNG64_MiSTer` | `src/hng64/` |

Without the server the cores still run: their bridge answers the main CPU's sound handshake, and
the game is silent.

## Installation

* Unzip the latest `releases/arcsnd_<YYYYMMDD>.zip` onto the SD card's root. It holds
  `games/arcsnd/arcsnd` and `Scripts/arcade_hybridsoundserver.sh`.
OR
* Install the DB for downloader from https://github.com/ppriest/MiSTer_ppriest

  
* Run `arcade_hybridsoundserver` from the Scripts menu (again to stop it), or add
  `/media/fat/Scripts/arcade_hybridsoundserver.sh start` to `linux/user-startup.sh` to start it at
  boot. It waits in the background, plays whenever a set it knows is loaded, and goes quiet at every core load.
* Its log is `/tmp/arcsnd.log` (`-v`: a status line every 5 s).

## Building

```
python scripts/deploy.py [--no-install] [--package]
```

Cross-compiles a static ARM binary with WSL's `arm-linux-gnueabihf-gcc`/`g++` (Cortex-A9, NEON),
then installs it, the start script and, per Hornet set, `games/<set>/_handler.sh` (run by MiSTer
Frontier's Master_Daemon.sh while that set is loaded). `--package` writes
`releases/arcsnd_<YYYYMMDD>.zip`. The MiSTer is reached with PuTTY's `plink`/`pscp`; `MISTER_HOST`,
`MISTER_USER` and `MISTER_PASSWORD` come from the environment, `mister.env` (gitignored; see
`mister.env.example`) or the per-machine `~/.mister-core.env` the cores share.

`src/Makefile` builds the native benches (MSYS2 MinGW64): `make hornet_bench` replays a MAME
capture of the Hornet board.

## Adding a board

A session is a pair of functions in `src/server.cpp`'s dispatch: `<board>_claims(setname)` and
`<board>_run(mem, verbose)`, which runs until the core changes. The core's side is a bridge that
writes the main CPU's sound accesses into the shared block; `docs/PROTOCOL.md` has the blocks in
use. `src/common/` holds the daemon plumbing: `/dev/mem` mapping, the core-name watch, audio output
ahead of the FPGA's playback.

## Layout

| Path | Contents |
|---|---|
| `src/server.cpp` | the process: one copy, the core-name watch, the sessions |
| `src/common/` | daemon, audio output, MAME shims |
| `src/hornet/` | the Hornet sound board: Musashi 68000, MAME's RF5C400 and board logic |
| `src/hng64/` | the HNG64 sound board: MAME's V53A (NEC core) and L7A1045 |
| `docs/PROTOCOL.md` | the shared DDR3 blocks |
| `scripts/deploy.py` | build, install, package |
| `releases/` | packaged builds |

## Licence

GPL-3.0-or-later (`LICENSE`). Parts are MAME's (BSD-3-Clause) and Musashi (MIT); each file keeps
its licence and copyright holders in its header, and `PROVENANCE.md` lists the sources and what
was changed.

## AI attestation

Developed with heavy use of a frontier coding assistant, which wrote the code, benches and these
documents under the author's direction. The author tests it on hardware.
