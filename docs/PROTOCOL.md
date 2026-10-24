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
