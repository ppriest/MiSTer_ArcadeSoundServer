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

Without the server the cores still run: their bridge answers the main CPU's sound handshake, and
the game is silent.

## Sega Model 3

I added the Model 3 board for my Model 3 core (`SegaModel3_MiSTer`). It splits the sound board between
the ARM and the FPGA, because one Cortex-A9 core cannot run the whole board in the time a sample has:

* **On the ARM:** the 68000 (arcsnd's Musashi) and both SCSPs' slots, from Supermodel's sound board
  and SCSP. The output is Supermodel's, sample for sample. The SCSP's timers follow the arcade's
  period, measured on a cabinet; Supermodel's own model is kept as a mode.
* **In the FPGA:** both SCSPs' effects DSPs, their effect return and the clip (the core's
  `m3_scsp_dsp`, inside its sound bridge `m3_sndbridge`). The server sends each sample's dry sums
  and DSP inputs as a record, and the 68000's writes to the DSP as commands, through a 2 MiB block
  of DDR3 at 0x3F200000 (`docs/PROTOCOL.md`, Model 3 version 2). The FPGA plays the result on the
  core's own clock, D samples (256, 5.8 ms) after the server could first render it. This board
  does not use the framework's ALSA path.
* **The Digital Sound Board** of Scud Race (DSB1: a Z80), Daytona 2 and Sega Rally 2 (DSB2: a 68000) runs on the
  ARM as well: Supermodel's DSB with minimp3 for the MPEG music. Its music goes into the records'
  front dry sums, so the FPGA needs nothing more for it.
* **The ROMs** (the 68000 program, the SCSP samples, the DSB's program and MPEG data) are read from
  the set's zip, and its parent's, in MiSTer's mame folders, while Main is loading the set into the
  core. The core loads nothing for sound. `src/model3/roms_table.h` has each set's file names,
  offsets and CRC-32s, made by `scripts/m3snd_table.py` from Supermodel's `Games.xml`; no ROM data.

It needs a build of the Model 3 core with the sound link (its `SND_DSP` on). Without the server
that core runs silent.

**Why the DSPs are in the FPGA.** I first ran the whole board on the ARM. Measured on the MiSTer,
it did not keep up:
* The two DSPs were about two thirds of the work. Each runs its program of up to 128 steps for every
  sample, playing or silent, as the chip does: that is what keeps reverb and echo tails going.
* Even with each DSP program translated to ARM code, the worst second of play needed 100-108% of one
  A9 core (VF3, Lost World, Bass Fishing), and only buffering of about 100 ms could have hidden it.
* MiSTer Main keeps the other core busy all the time, so there was no second core to spread onto.

With the DSPs in the FPGA, the ARM's worst second of play falls to 65-77% of the core, which leaves
room for the Digital Sound Board. In the FPGA the DSPs cost about 570 ALM and add one sample of
delay, and running every sample costs them nothing.

**The SCSP here is not a whole SCSP.** This board does not run the SCSPs' DSPs: it sends their
inputs and the 68000's writes to them to the core, and expects the core to run the DSPs, add their
effect return and play the result. A core that runs only a bridge to the server, with no DSP in
the FPGA, gets no sound from this board. Any other core that wants to use this SCSP session (for an
SCSP board other than Model 3's) has to carry the FPGA side as well:

* **The RTL:** `m3_scsp_dsp.sv` (the DSP engine, both chips) and `m3_sndbridge.sv` (the bridge) from
  `SegaModel3_MiSTer`'s `rtl/sound/`, GPL-3.0-or-later. About 570 ALM, 135 M10K (the two effect
  rings) and 2 DSP blocks on the Cyclone V.
* **The bridge's connections:**
  * the main CPU's writes to the sound board's port (`midi_we`, `midi_data`: 1 for the data port,
    `midi_byte`), which it time-stamps for the server;
  * one DDR3 port (`d_*`: request, write, 25-bit word address, burst length, data, byte enables,
    acknowledge, read valid, read data) on the core's DDR3 arbiter;
  * `audio_l` / `audio_r`, signed 16-bit, to the core's audio out;
  * the core clock (`CLK_HZ` must be its frequency, for the 44.1 kHz sample clock) and a reset.
* **Its parameters:** `BLOCK` is the shared block's DDR3 word address (byte 0x3F200000 here; it must
  match the server's), `D` the play delay in samples (256).
* **The protocol:** `docs/PROTOCOL.md`, Model 3 version 2, with the v4 command format: the server
  pre-decodes every DSP write into a kind, an address and a value, so the engine decodes nothing.
  A core's bridge and the server must agree on this format and on the block's layout exactly. Run by hand with `-v`, the status line also gives how far behind the server
ran, and the share of the core its writes to the block took.

## Installation

* Unzip the latest `releases/arcsnd_<YYYYMMDD>.zip` onto the SD card's root. It holds
  `games/arcsnd/arcsnd` and `Scripts/arcade_hybridsoundserver.sh`.

OR

* Install the DB for downloader from https://github.com/ppriest/MiSTer_ppriest

  
* Run `arcade_hybridsoundserver` from the Scripts menu (again to stop it), or add
  `/media/fat/Scripts/arcade_hybridsoundserver.sh start` to `linux/user-startup.sh` to start it at
  boot. It waits in the background, plays whenever a set it knows is loaded, and goes quiet at every core load.
* Its log is `/tmp/arcsnd.log`: the build (its date and commit), the sets it plays, each startup step, each core name and
  each board's start and stop; run by hand with `-v`, a status line every 5 s as well. A crash
  writes the signal, the faulting address and
  the code addresses on the stack there, for `addr2line -f -e obj/arm/builds/arcsnd_<commit>`.

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

`src/Makefile` builds the native benches (MSYS2 MinGW64; `model3_bench` on Linux too): `make hornet_bench` replays a
MAME capture of the Hornet board, `make model3_bench` a Supermodel capture of the Model 3 board.
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
