# The shared blocks

Each core keeps a block of DDR3 at `0x3F200000` (physical, as the HPS sees it: the cores' DDR3
window starts at `0x30000000`), written and read by its bridge in the FPGA and mapped by the server
through `/dev/mem`. Little-endian 64-bit words; offsets in bytes. The core's bridge source is the
authority; these tables are copied from their headers.

Common to both:

* A magic and version at +0x00 tells the server which board's block it is and that the bridge has
  reset. DDR3 is not cleared between cores, so a block left by another core can still be there:
  the server also waits for the bridge's beat or count to move.
* The server's word at +0x40 carries the board's replies, flags with `0xA5` and a running bit, and
  a heartbeat. The bridge reads it every POLL clocks and uses the replies only while the heartbeat
  moves; otherwise the core's stand-in answers.
* The server stops (silence, the board freed) when the beat stops for 200 ms (the core reloaded or
  held in reset), the magic goes, or `/tmp/CORENAME` is rewritten.

## Konami Hornet (`rtl/mister/hornet_sndbridge.sv`, version 1)

| Offset | Contents | Writer |
|---|---|---|
| +0x00 | "HRNS", version 1 | FPGA, every BEAT (10 ms) |
| +0x08 | the 68000 program's and the samples' DDR3 byte addresses | FPGA, every BEAT |
| +0x10 | {count of ring entries written, 32} | FPGA, after each entry |
| +0x18 | {beat count (+1 every BEAT), 32} | FPGA, every BEAT |
| +0x20 | {SNDRES in bit 32, host registers 3-0 in bits 31-0} | FPGA, after each entry |
| +0x40 | {heartbeat 16, 0xA5 8, running 8 (bit 0); 0 16, reply 1 8, reply 0 8} | server |
| +0x1000 | ring of 512 entries {0, data 8, register 8 (0-7, 8 SNDRES), microseconds 32} | FPGA |

Each PPC write to a K056800 host register or to SNDRES becomes a ring entry stamped with a
microsecond count; the server applies it at that stamp mapped onto the board's clock 2 ms later,
keeping the stamps' spacing. A write follows its entry through the same DDR3 master, so the count
never runs ahead of the ring. The 68000 program and the samples are where the core's fast ROM load
put them in DDR3 (the `.mra` places them); the server copies them when it starts.

## SNK Hyper NeoGeo 64 (`rtl/hng64_sndbridge.sv`, version 2)

| Offset | Contents | Writer |
|---|---|---|
| +0x00 | "HNGS" (bytes 0-3), version (4-7) | FPGA, once after reset |
| +0x08 | the sample ROM: DDR3 offset (bytes 0-3), size (4-7) | FPGA, once |
| +0x10 | main latch 0 (0-1), main latch 1 (2-3), interrupt-5 writes (4-5), sound CPU enables (6-7) | FPGA, on every change |
| +0x18 | bit 0: the sound CPU runs (0x55AA last, not 0xAA55) | FPGA, on every change |
| +0x20 | a count (0-1), up every BEAT clocks out of reset | FPGA |
| +0x40 | sound latch 0 (0-1) and 1 (2-3); flags (4-5): byte 5 0xA5, bit 0 the process runs the V53A; heartbeat (6-7) | server |
| +0x10000 | sound RAM, 2 MB, as the V53A sees it | FPGA, every main-CPU write |

The main CPU's sound RAM writes are copied as the core's memory takes them; the +0x10 word, whose
enable count tells the server to start, goes out only once that copy's queue is empty, so the
server never reads a sound RAM older than the enable. At each enable the server copies sound RAM
and the sample ROM and starts the V53A.

## Sega Model 3 (`SegaModel3_MiSTer` `rtl/sound/m3_sndbridge.sv`, version 2)

The 68000 and both SCSPs' slots run on the ARM (`src/model3/session.cpp`); both SCSPs' DSPs, their
effect return and the clip run in the FPGA (`m3_scsp_dsp`, inside the bridge), which plays its own
output. The block is 2 MiB at byte `0x3F200000` (the bridge's `BLOCK`, 25'h1E4_0000 in 64-bit words
from the core's DDR3 base 0x30000000). The sound ROMs come from the set's zip; nothing of them is in
DDR3.

| Offset | Contents | Writer |
|---|---|---|
| +0x00 | {0 6, MIDI entries written 10, fs 16, D 16, "M" 8, version 4 (2), 0 2, epoch 2} | FPGA, every 32 samples |
| +0x40 | {heartbeat 16, 0xA5 8, running 8 (bit 0); samples rendered 32} | server (diagnostic) |
| +0x48 | {late chunks 32; MIDI entries applied 32} | server (diagnostic) |
| +0x1000 | MIDI ring, 512 entries {0 16, fs 16, 0 23, data port 1, byte 8} | FPGA |
| +0x40000 | record ring, 512 records, one every 512 bytes (37 beats used) | server |
| +0x80000 | unused by the bridge; the server measures its store costs here when a set loads | server |
| +0x100000 | command ring, 131,072 entries of 8 bytes | server |

**Commands.** An entry, in a record or in the command ring, is the engine's command as the bridge
hands it on, laid out by the server as the engine's arrays hold it (`dsplink_map` in
`src/model3/dsplink_shm.h`), so the FPGA decodes nothing (v4 of the engine):

    {0 8, data 24} {kind 3, 0 3, address 16, 0 9}      (low word first)

| Kind | Command |
|---|---|
| 0 | both lanes of the engine's 24-bit store: TEMP {0, chip, index 7}, MEMS {10, chip, 0, index 5}, MIXS << 4 {10, chip, 10, index 4}, the four channels' dry sums 0x190-0x193 |
| 1 | its high lane (bits 23..8): a 68000 word into TEMP / MEMS, or EFREG {11, chip, 00, index 4} |
| 2 | its low lane (bits 7..0) |
| 3 | a DSP ring word {chip, offset 15} (offset from RBP << 12; no other sound RAM word is sent) |
| 4 | COEF {0, chip, index 6}, MADRS {10, chip, index 5}, an effect-return pan {11, chip, right, slot 4} (0..16,384) |
| 5 | an MPRO word {chip, step 7, word 2}; rows from LastStep on are sent as zeros, and the whole program again at each Start |
| 6 | a chip's setting {chip, which 2}: 0 RBL, 1 Start {steps run 8, LastStep 8}, 2 DAC18B |
| 7 | nothing (the engine ignores it) |

**Records.** Record p sits in slot p % 512. It has 36 entries, all kind 0: MIXS of the master
(16), MIXS of the slave (16), then the dry sums front L, front R, rear L, rear R. The dry sums are
taken before the effect return and the clip, and held to +-2^22. The Digital Sound Board's music
is added into front L and R at Supermodel's level: the SCSP's own when the master's DAC18B is set,
four times it when not. Then comes the tag beat {0 14, epoch 2, p 16} {commands written 32}. The
server writes the count, then the tag, last.

The bridge takes the first 36 beats of any slot it reads straight into the engine as commands,
before it checks the tag. A record not yet written is tried again the next sample, and past 128
samples late it is skipped. So:
- **The FPGA's clear** (after a core reset, before its first header) writes every beat of every slot as
  {kind 7, 0 43, ~epoch 2, 0 16}, which does nothing in the engine. The epoch then moves on.
- **The server** writes a slot in full once in a session (and once every 128 laps). After that it
  writes only the data words that are not zero now or were not zero in that slot, then the count
  and the tag. The kind and address words never change, and nothing else writes the ring between
  clears, so the slot holds the same words as if the whole record were written. This is because the
  block is strongly-ordered memory from user space: every store waits for DDR3.
- **When a session stops** while the bridge's header is there and its epoch is the session's, the
  server writes every slot's tag beat as the clear's {~epoch, 0}. Otherwise a record left behind
  matches the bridge's 16-bit p again 65,536 samples on, and its count takes stale commands into
  the engine.

**Time.** One clock: the FPGA's fs, 16 bits here, which the server widens.
- The server attaches once the header is valid and fs moves. The board's sample 0 is that fs, and
  the server sends the DSP's whole state and both ring windows as commands.
- It renders the board's sample n once fs has passed it, applying MIDI bytes at their fs + 14 (the
  uPD71051's 10 bits at 31.25 kbaud). It files sample n as record p = base + n + D - 1.
- The bridge takes record p at fs = p: the record's commands, the tag check, then the command ring
  up to the tag's count (the bridge counts 17 bits). Then the engine runs, and the audio goes out
  at fs = p + 1.
- The server starts a new session when the epoch moves, and stops when fs stalls for 200 ms or the
  core changes.

### Version 1 (superseded)

The first design had the whole board on the ARM, with a PCM ring at +0x10000 that the FPGA played,
and the sound ROMs in DDR3 at 0x3E000000. One A9 core could not run it in real time, so the DSP
moved into the FPGA and the block became version 2.
