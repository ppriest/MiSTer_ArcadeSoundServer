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
| Sega Model 3: 68000, 2 x SCSP; Digital Sound Board (DSB1: Z80, DSB2: 68000; MPEG) | every set in Supermodel's Games.xml with a sound board (63; tested: vf3, bassdx, scuddxo, lostwsga, daytona2, srally2) | `SegaModel3_MiSTer` | `src/model3/` |
| Konami Firebeat Keyboardmania: the XT446 synth's MIDI in, on a SoundFont GM synth (TinySoundFont), not the XT446 itself | kbm, kbh, kbm2nd, kbm3rd | `Arcade-Firebeat_MiSTer` | `src/firebeat/` |

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
* Keyboardmania plays its keys on `games/arcsnd/Yamaha XG Sound Set.sf2`, installed with `arcsnd`
  (`TimGM6mb.sf2` beside it when that is missing). To use another SoundFont, set `soundfont=` under `[firebeat_kbm]` in `games/arcsnd/arcsnd.ini` (a path,
  relative to `games/arcsnd`), or put it there as `firebeat.sf2`. It is mapped from the SD card and
  locked in memory when a Keyboardmania set starts (the file's size, no copy), and released when the
  core changes.
* Its log is `/tmp/arcsnd.log`: the build (its date and commit), the sets it plays, each startup step, each core name and
  each board's start and stop; run by hand with `-v`, a status line every 5 s as well. A crash
  writes the signal, the faulting address and
  the code addresses on the stack there, for `addr2line -f -e obj/arm/builds/arcsnd_<commit>`.

## Building

```
python scripts/deploy.py [--no-install] [--package]
```

Cross-compiles a static ARM binary with WSL's `arm-linux-gnueabihf-gcc`/`g++` (Cortex-A9, NEON),
then installs it and the start script. `--package` writes
`releases/arcsnd_<YYYYMMDD>.zip`. The MiSTer is reached with PuTTY's `plink`/`pscp`; `MISTER_HOST`,
`MISTER_USER` and `MISTER_PASSWORD` come from the environment, `mister.env` (gitignored) or the per-machine `~/.mister-core.env` the cores share.

`src/Makefile` builds the native benches (MSYS2 MinGW64; `model3_bench` on Linux too): `make hornet_bench` replays a
MAME capture of the Hornet board, `make model3_bench` a Supermodel capture of the Model 3 board,
`make firebeat_bench` a Keyboardmania MIDI capture through the synth into a WAV.
`deploy.py` builds the Model 3 board into the same binary; I build it with ARM's GNU toolchain 10.2
(`arm-none-linux-gnueabihf`, the one Main_MiSTer uses) and the same flags.

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
| `src/model3/` | the Model 3 sound board: Musashi 68000, Supermodel's SCSP, board logic and Digital Sound Board (its Z80, minimp3), the link to the FPGA's DSP, the ROMs from the zip |
| `src/firebeat/` | Keyboardmania's MIDI: the parser over TinySoundFont, the session |
| `assets/` | the default `arcsnd.ini` and SoundFonts |
| `assets/arcsnd.ini` | the settings, a section per system; installed when the MiSTer has none |
| `docs/PROTOCOL.md` | the shared DDR3 blocks |
| `scripts/deploy.py` | build, install, package |
| `releases/` | packaged builds |

## Licence

GPL-3.0-or-later (`LICENSE`). Parts are MAME's (BSD-3-Clause), Supermodel's (GPL-3.0-or-later), Musashi (MIT), miniz (MIT) and minimp3 (CC0-1.0); each file keeps
its licence and copyright holders in its header, and `PROVENANCE.md` lists the sources and what
was changed.

## AI attestation

Developed with heavy use of a frontier coding assistant, which wrote the code, benches and these
documents under the author's direction. The author tests it on hardware.
