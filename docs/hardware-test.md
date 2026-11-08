# Hardware test card (FRITZ!Box 7362 SL)

Rules: `docs/` facts. Only 0x00/0xFF bytes are
ever transmitted. Register reads only of the map in `hardware.md`.

## Setup
```
ssh root@192.168.1.1 (private known_hosts, BatchMode)
sysctl -w kernel.panic=10
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
scp -O lantiq_tdm_pcm.ko tdm-pcm-test root@192.168.1.1:/tmp/
```

## A. No bus clock (chip in reset)
1. `insmod /tmp/lantiq_tdm_pcm.ko` — dmesg: "ready (rx irq 60, tx irq
   59)", "fallback device for avm,fritz7362sl registered";
   `/proc/iomem` shows `1f100000-1f1000ff : lantiq-tdm-pcm`.
2. `cat /sys/kernel/debug/tdm-pcm/pins` — note port 0/1/2 values.
3. `/tmp/tdm-pcm-test regs rx=2 idle=ff` — expect 0 frames, no storm,
   `CH_EN=0x000000ff CFG=0x00008101` at step 4 (`0x0000c101` with
   `pcm_cfg=0x4101`).
4. `pins` again: identical to 2. **Passed 2026-11-01.**

## B. Live bus
1. `/root/dect-chip-boot.sh` (radio off).
2. `/tmp/tdm-pcm-test rx=5 dump=4 idle=ff` — expect ~8000 frames/s,
   seq gaps 0, top frame `ffff fefe fefe fefe 55d5 0c08 ffff ffff`,
   only bytes 8..9 changing, `overruns=0`. **Passed.**
3. Pads: `cat /sys/kernel/debug/tdm-pcm/scope > /tmp/s.txt` while 2
   runs; on the host `tools/scope-decode.py s.txt` — DCL ~1.02 MHz, 128
   clocks per FSC, DI frames equal to `read()`. **Passed.**
4. TX: `tdm-pcm-test rx=3 idle=0f00000000000000` + scope: DO shows
   `00ff 0000 ...` (byte 1 marked). `txalt=1`: whole 0xFF/0x00 frames.
   **Passed** (push-pull).
5. NAND: during `tdm-pcm-test rx=9 idle=ff`, run
   `taskset -c 0 dd if=/dev/mtd1 of=/dev/null bs=131072 count=1`, then
   the same on CPU 1. Read `stats`: `lost`, the `timer` lines (the
   blocked CPU's `max_gap_us` ~27000, the other's < 250).
   **Passed with the defaults** (0 lost, 7 reads on CPU 0, 5 on CPU 1);
   failed with the old interrupt engine (745 lost).
6. 15-minute run with ubihealthd: `tdm-pcm-test rx=900 stats=60
   idle=ff ndesc=16`
   — **Passed 2026-11-01**: 0 lost in 900 s, 7,200,321 frames, one
   ~27 ms blackout bridged (timer gap).
7. Console: `taskset -c N sh -c 'echo line > /dev/kmsg'` (N = 0, 1)
   while 2 runs. One line from CPU 1 lost ~50 frames (both CPUs held
   ~6.4 ms); from CPU 0 none. With `conloglevel 4` (since session 4):
   0 lost for warning-level lines; error-level lines still stall.

## End of session
`/root/dect-chip-reset.sh`; `rmmod lantiq_tdm_pcm`; pins 0/25/40/41
unclaimed in `/sys/kernel/debug/pinctrl/*/pinmux-pins`; nothing at
0x1F100000 in `/proc/iomem`.
