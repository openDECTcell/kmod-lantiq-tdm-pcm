# VR9 TDM/PCM block on the FRITZ!Box 7362 SL: facts

Each fact names its source and how it is known: **derived** (statically,
from a source), **measured** (on the 7362 SL, OpenWrt 25.12.6, Linux
6.12.112, 2026-11-01) or **assumed**. Written in our own words; no code
from any source is used in this repository.

Sources: [AVM] AVM GPL drop FRITZ!Box 7362 SL, FRITZ!OS 07.12, kernel
3.10.107 (the 7362 SL GPL kernel headers). [ML] Linux v6.12.112 `arch/mips/lantiq/xway/sysctrl.c`,
`arch/mips/lantiq/irq.c`, `drivers/pinctrl/pinctrl-xway.c`,
`drivers/mtd/nand/raw/xway_nand.c`. [OW] OpenWrt 25.12.6 lantiq target.

## Register map (base 0x1F100000, 32-bit, big endian) — derived [AVM]

| Off | Our name | Content |
|---|---|---|
| 0x00 | RX_IEN | receive-ready interrupt enable, bit per channel (15:0) |
| 0x04 | TX_IEN | transmit-ready (31:16) and transmit-empty (15:0) interrupt enables |
| 0x08 | RX_STAT | channel holds received data (15:0), read only |
| 0x0C | TX_STAT | transmit ready = room (31:16), empty (15:0), read only |
| 0x10 | FREQ | bit-clock rate code (4:0); bit 5 forces the code instead of detection |
| 0x14 | CFG | 15 enable, 14 buffer mode (1 = for data), 12/11 AC97 resets, 10:8 transmit offset, 7 double clocking, 6 transmit on falling edge, 5 sample on rising edge, 4 half-period drive, 3 shift (double clocking), 2:0 receive offset |
| 0x18 | CH_EN | time-slot channel enable (15:0) |
| 0x1C | LOOP | receive-to-transmit loopback per channel |
| 0x20 | CH_MODE | 0 = 8-bit, 1 = 16-bit channel (two consecutive time slots) |
| 0x24 | STAT | 15/14 slot collision highway B/A (write 1 clears); low bits: detected clock code |
| 0x28 + 4ch | CH_SLOT | receive slot 23:16 (+ highway bit 24), transmit slot 7:0 (+ highway bit 8) |
| 0x68 + 4ch | CH_DATA | write queues a transmit word, read takes a received word; 16-bit mode: 15:8 = slot k, 7:0 = slot k+1 |

16 channels. Measured: every register above reads back without harm
with the gate open (each first read was done alone); reset values all 0
except TX_STAT 0xFFFFFFFF and STAT 0x1F with no bus clock. STAT reads
0x01 with the chip's 1.024 MHz clock (code 1; the code table is not in
the source). CH_EN keeps a value **only while CFG.enable is set**
(measured: written before enable, it reads 0). The source lists no
overflow/underflow flag; none seen.

## Clock gate, reset, power — derived [AVM], [ML]; measured
- PMU power-down bit **25** gates the block ([AVM] VR9 PMU table; the
  table's bit numbers match [ML]'s for USIF 7, SPI 8, DMA 5, DEU 20).
  Mainline 6.12 has **no clkdev entry** for it. Measured: on the running
  box PMU PWDSR = 0x00010880, bit 25 clear: **the gate is already open**
  after boot. The driver opens it with `ltq_pmu_enable()` only if closed
  and closes it again on unload only if it opened it.
- RCU reset request bit 19 / status bit 25 ([AVM]); not used.
- A TDM power domain exists in [AVM]; nothing gates it by default;
  not touched.

## Interrupts — derived [AVM], [ML]; measured
TX = ICU 1 line 19, RX = ICU 1 line 20 ([AVM] hw 51/52). OpenWrt's xway
domain counts from 8 ([ML] irq.c): **59 (TX), 60 (RX)**. Measured: RX
fires per frame when RX_IEN enables the last channel; **twice per
frame** (the level line is latched again before the read clears it; the
second run finds nothing). Lands on CPU 0.

## DMA — derived, not tested
VR9's central DMA map ([AVM] `ifxmips_dma_vr9.h`) names no PCM/TDM port
(port 4 is "MCTRL"). AR10 ([AVM] `_ar10.h`, `ar10/ifxmips_pcmdma.h`) has a
PCM port 4 plus a separate FIFO channel controller; VR9 has neither in
the drop. AVM's VR9 kernel instead routes PCM RX/TX interrupts to a MIPS
MT "yield" thread context ([AVM] `ifx_yield.h`), i.e. per-frame CPU
service. Conclusion: **no DMA path for this block on VR9** (assumed from
the above; port 4 was not probed).

## Pins — derived [AVM], [ML]; measured
| GPIO | Port/bit | Role | Mux (ALT0/ALT1) | Direction / driver |
|---|---|---|---|---|
| 0 | 0/0 | FSC | 1/1 | in |
| 25 | **1**/9 | DO | 1/0 | out, **push-pull** |
| 40 | 2/8 | DCL | 0/1 | in |
| 41 | 2/9 | DI | 0/1 | in |

Pin registers: base 0x1E100B10, 0x30 per port, IN +0x04, DIR +0x08,
ALT0 +0x0C, ALT1 +0x10, OD +0x14 ([ML]). OD bit **1 = push-pull**, 0 =
open drain ([ML] pinctrl-xway). [AVM]'s board table sets "OD_SET" for
DO, which writes OD = 1 ([AVM] `ifxmips_gpio.c`), i.e. push-pull; every
Infineon reference output uses the same flag. **A-0001 §3's "open
drain" for DO is therefore wrong.** Measured: open drain drives
all-0xFF idle high (pull-up present) but never shows 0xFF frames between
0x00 frames at 1 Mbit/s; push-pull does. pinctrl-xway has no TDM group;
the driver sets exactly these bits, saves them first and restores them
(measured: port 0/1/2 DIR/ALT0/ALT1/OD identical before and after).
Before our module touches them the four pads are GPIO inputs.

## The bus in mode 0x101 — measured
Pads sampled from the GPIO IN registers (debugfs `scope`, 76-217 ns per
sample) and frames read through the block:
- DCL **1.023 MHz** (978 ns period by the SoC clock; nominal 1.024),
  ~48 % high. **128 DCL periods per FSC**. FSC **8.000 kHz** (24120
  frames in 3.015 s; 72120 in 9.0 s). FSC high for **one DCL period**,
  rising with a DCL rising edge. Bit 0 of the frame is the bit period
  **after** the FSC pulse; the chip changes data on the rising edge,
  sample on the falling edge (rising-edge sampling gave bit errors).
- Block settings that match: single clocking, sample on falling edge,
  transmit from rising edge, **receive offset 1, transmit offset 1**
  (CFG 0x4101 incl. frame-synchronous buffers). Verified both ways: the
  pad decode of DI equals what `read()` returns; a marker idle frame
  (byte 1 = 0xFF, all other bytes 0x00) decodes from DO as
  `00ff 0000 ...`, i.e. exactly at byte 1 (with transmit offset 0 it
  came one bit early, `01fe`).
- 128 bits = 16 time slots = 8 channels in 16-bit mode = uapi
  `slots = 8`, 16 bytes per frame.
- Content from the chip after `/root/dect-chip-boot.sh` (radio off),
  bytes in wire order:
  `ffff fefe fefe fefe 55d5 0c08 ffff ffff` (~90 %); only bytes 8..9
  change (`55d5`, `5555`, `5557`, `d5d5` ...); bytes 10..11 constant
  `0c08`; bytes 2..7 the 0xFE idle pattern; the rest 0xFF.

## Buffering and timing — measured
- CFG.14 = 1 (frame-synchronous, "for data"): receive holds **1 frame**;
  all channels become ready together; a frame must be read within
  ~125 µs of FSC. CFG.14 = 0: **2 frames**, channels ready one by one.
- Transmit: one frame can be written ahead. Written when its receive
  frame is read it goes out in the next frame, whole (no tearing seen on DO
  with alternating 0xFF/0x00 frames).
- A missed frame window: the block **repeats the last written word** on
  every channel (seen as runs of equal frames on DO while the engine
  was held off).
- Quiet run, interrupt + 1 ms per-CPU backstop (first engine): 0 lost
  in 5 s and 9 s, largest receive lag 72-95 µs, engine run ≤ 16 µs.

## Interrupts off on one CPU: NAND reads, console lines — measured
- A 128 KiB raw read of mtd1 (NAND, `dd bs=131072 count=1`, what
  ubihealthd does) keeps interrupts off on the reading CPU for
  **~27 ms** (that CPU's engine timer: `max_gap_us` 26354-27936). The
  other CPU is **not** held off: its timers keep their period (gaps
  ≤ 200 µs at 100 µs) and register access stays fast (bench ≤ 12 µs).
  mtd3 (64 KiB-erase NOR) reads cost nothing.
- A kernel **console line** (ttyLTQ0, 115200 Bd; console log level 7 on
  this box) keeps the printing CPU's interrupts off for **~87 µs per
  character**, ~9.6 ms for the first driver's ~110-character loss
  warning. Printed from the engine, each warning caused the next loss:
  that was the first session's "745 lost, CPU 1 drained only every
  9-10 ms, one engine run 9.58 ms" (gaps alternating between the CPUs at
  9.57 ms). With the console level at 1, CPU 1's 1 ms timer ran on time
  throughout the same read. The engine prints nothing now.
- A console line printed **from CPU 1** (`/dev/kmsg`) held **both** CPUs
  for ~6.4 ms (both timers' `max_gap_us`; ~50 frames lost, reproduced 3
  times); from CPU 0 it held only CPU 0 (no loss). Cause not identified
  (assumed: CPU 0's `asc_tx` interrupt waiting for the serial port's
  lock while CPU 1 prints). The transport cannot bridge it; a lower
  console log level avoids it. Measured with `conloglevel 4` (set on
  the box since session 4): the same lines (warning level) from CPU 1
  or 0 lost **0** frames in 4 runs; lines at error level still reach
  the console and still lost ~50 frames each.

## Engine — measured
CPU only (no DMA). Read a frame only when every channel is ready, write
one only when every channel has room, one frame out per frame in. Loss
is estimated from the receive phase on the 125 µs grid (verified:
forced losses add up to 8000 frames/s; a 6.36 ms stall counted 50).

Default: **two-frame buffers (CFG 0x0101) and one hrtimer per CPU every
100 µs, no interrupt.** While one CPU is blocked, the other's timer runs
within ≤ 200 µs, inside the 250 µs the buffers hold. NAND reads on
either CPU: **0 lost** (12 reads). Receive lag ≤ 120 µs. Frame data and
transmit alignment equal those of 0x4101 (pad decode: the marker at
byte 1; alternating frames switch whole at FSC).

Alternatives measured and rejected:
- Interrupt + 1 ms timers, one-frame buffer (the first engine): a
  blackout of the interrupt's CPU loses 7 frames per ms (189 in a NAND
  read, console off).
- The same with the other CPU's timer at 100 / 75 / 50 µs: 2 / 0-2 / 0
  lost per read; at 75 µs that timer's runs come up to ~160 µs apart,
  too close to the 125 µs one frame lasts; at 50 µs (20 k runs/s) the
  reader starved and its ring overflowed.
- Two-frame buffers with the interrupt: the last channel's receive
  interrupt fires **~9 times per frame** (~73 k/s) in this buffer mode
  and tripped the storm guard. Unusable.

Cost (a fixed md5 job's time on each CPU; on this MT core both VPEs
share one pipeline, so it shows on both): engine off 1.15-1.25 s;
interrupt + 1 ms timers 1.39-1.52 s; default 1.51-1.59 s; interrupt +
75 µs 1.58-1.64 s. `/proc/stat` does not show it (no interrupt time
accounting).

## Device tree
None in OpenWrt for this block. A future node: compatible
`lantiq,xrx200-tdm-pcm`, `reg = <0x1f100000 0x100>`, interrupts 60
(RX) then 59 (TX) on the ICU. The pin mux still needs the driver
(no pinctrl group); the fallback in `src/tdm_pcm_fallback.c` supplies
board pins and the PMU bit.
