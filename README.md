# kmod-lantiq-tdm-pcm

> **Note:** this module was written by an AI coding agent under the direction of the openDECTcell maintainers; only smaller
> parts have been reviewed by a human. It is provided without any warranty, is purely experimental and has been tested only on
> the hardware named below.

Linux kernel module that runs the Lantiq VR9 **TDM/PCM block** as a PCM
**slave** transport (bit clock and frame sync from the peer) and exposes
it as **`/dev/tdm-pcm0`** with the interface of `kmod-lantiq-ssc-pcm`
(`src/include/uapi/ssc_pcm.h`, uapi version 3, unchanged). Target:
OpenWrt 25.12.x, Linux 6.12, `lantiq/xrx200`; developed on the FRITZ!Box
7362 SL, whose DECT chip is the peer. Generic: nothing DECT-specific.

Status (2026-11-01): receive and transmit work on the 7362 SL, frame
aligned to FSC, **loss-free during NAND reads** (ubihealthd) on either
CPU, and over 15 minutes with ubihealthd running .
Still lossy: a kernel console line printed at 115200 Bd can stall both
CPUs for one line (~6 ms). With the console log level at 4
(`system.@system[0].conloglevel`, set on the test box) only error-level
messages still reach the console.

## Interface
- `SSC_PCM_IOC_INFO` reports name `lantiq-tdm-pcm`, default `slots = 8`
  (16-bit slots), `frame_bytes = 16`.
- `CONFIG`: `slots` 1..16 (one TDM channel per 16-bit slot, time slots
  2k and 2k+1), `frames_per_block` (block ≤ 4096 bytes), `n_desc` 2..16
  (default 32 frames × 16 blocks = 64 ms, enough for a 30 ms reader
  blackout).
- `STEP` 1 clock gate, 2 pin mux, 3 framing, 4 engine; 0 undoes all.
- `read()`: whole blocks with the `ssc_pcm_block` header; frames start at
  bit 0 after FSC, bytes in wire order (MSB first).
- `write()`: whole frames, same order; the idle frame (`TX_IDLE`, zeros
  by default) when none are queued.
- `STATS`: `overruns` and `tx_underruns` both carry the **estimated
  number of lost frames** (the block has no overflow flag); `rx_irqs`,
  `polls` count engine runs.
- `REGS`: CFG, STAT, RX_STAT, TX_STAT, CH_EN, CH_MODE, FREQ, RX_IEN.

## Module parameters
`pcm_cfg` (default 0x0101: two-frame buffers, offsets 1/1), `use_irq`
(0: per-CPU timers only), `poll_us` (100), `poll_all_cpus` (1),
`poll_fast_us` (0; with `use_irq`, the other CPUs' timer period),
`tx_lead` (1), `tx_open_drain` (0), `dcl_code` (0 = detect), `rx_depth`
(0 = from the buffer mode). The interrupt mode needs `pcm_cfg=0x4101`
(one-frame buffer) and is not loss-free under NAND reads.

## debugfs (`/sys/kernel/debug/tdm-pcm/`)
`stats`, `pins` (pin registers, PMU gate), `peek` (one register),
`scope` (pad sampler; `tools/scope-decode.py`), `bench` (bus latency).

## Build
`tools/build.sh [sdk]` (an OpenWrt 25.12.6 lantiq-xrx200 SDK copy in
`build/`), or out of tree against the SDK's kernel.
Package `kmod-lantiq-tdm-pcm`, no autoload. Test tool:
`tools/tdm-pcm-test`. Hardware facts: `docs/hardware.md`; test card:
`docs/hardware-test.md`.

Licence: GPL-2.0-only (uapi header: GPL-2.0 WITH Linux-syscall-note, from
kmod-lantiq-ssc-pcm). No code from AVM's GPL drop.

## Provenance

The hardware facts in `docs/` come from public sources (data sheets, the kernel's own drivers, the vendor's published GPL
source drops) and from observing the hardware; where a fact comes from the clean-room record of the openDECTcell project it is
labelled `[CR ...]` or `A-nnnn`, see [opendectcell's clean-room-process.md](https://github.com/openDECTcell/opendectcell/blob/main/docs/clean-room-process.md).
No vendor firmware, binaries or kernel modules are in this repository.

A mechanical line comparison of the code against the kernel tree of the vendor's 7362 SL GPL drop (2026-11-08) found no copied code: at most a few percent of lines match anything, and those are generic kernel idioms; the register header matches none.
