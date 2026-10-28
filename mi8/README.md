# Mi 8 (dipper, SDM845) bring-up

**Touch probe (shim v8: vddio + avdd + GPIO bit-bang I2C):** the touch bus (global SE14, gpio33/34) is bit-banged because the SE is
GSI-only. v7 on a real phone: pin writes work (`SELF hi=1 lo=0`), the bus reads low even released (`rel=0`), the RPMh vote for
pm8998_l28 worked (`RAIL rc=0`, TCS completed, `L28 addr=0x42100`) yet the bus stayed low, and gpio23 (the enable of
`disp_vddio_vreg`, the touch vdd) read cfg 0 / io 0, i.e. not driven: the touch chip's I/O supply is most likely off and its
pins clamp the bus. v8 finds gpio23's tile (the one whose neighbours read non-zero), drives it high only if the pin answers,
waits 10 ms, reports bus idle levels (`IDLE n sda= scl=`, 1/1 = free), scans, then votes L28, pulses reset and scans again.
`V23 hi=1 lo=0` = pin answered; `TILE` lines show what each tile reads for gpio22/23/24. `RAIL rc` and `I2C49n rc` as before
(0 ack, 1 nack, 2 stuck).

**Boot 1 result (2026-10-25):** kernel text on screen up to `zone_init`, then frozen on both images. Cause found in the
patches: the boot-CPU driver (0018) and AppleKeyStore (0038) match `IONameMatch qcom,kryo470`, and mkadt had replaced it
with `qcom,kryo385`, so no CPU driver attached. Fixed (both strings kept). Rebuild only the images, no macOS run needed:
Actions -> **mi8-image** -> run id of the green xnu run.

**Milestone 1 (kernel boot):** fork dipa-studio/ios-sweet on GitHub (public, so macOS runners are free), upload this
overlay on top, run the **xnu** workflow (macOS, 30-90 min; board defaults to `mi8`). Its second job builds the
device tree (`tools/mkadt.py --board mi8`), the payload, and the boot images `mi8-xnu-gz.img` / `mi8-xnu-ucomp.img`
(artifact `mi8-xnu-boot`). Then `fastboot boot mi8-xnu-gz.img` (RAM only, nothing flashed). Expected: a white band
(early debug), kernel text on the panel, and, because `userdata` still holds Android's ext4, a panic when it tries to
mount the HFS+ iOS root. That proves kernel + display + UFS + interrupts on this phone. `mi8/patch-board.py` rewrites the
three sweet patches that hardcode the panel (early framebuffer at `0x9d400000`, 1080x2248, crop of the iPhone X canvas).
Touch and buttons are off (`sweettouch=0 sweetkeys=0`): they need Mi 8 drivers (STM FTS touch, PM8998 keys).


First target for the ios-sweet approach on a Snapdragon 845 phone. Status: **diagnostic shim only**,
nothing boots XNU yet. It answers the one question that decides whether this port is cheap or not:
the panel (Samsung EA8074 AMOLED) is DSI **command mode**, so unlike sweet's video-mode panel, copying
pixels into ABL's splash buffer will not change what you see unless the display controller is told to
send a frame.

## Result of v1 on a real Mi 8 (2026-10-24)

- The display **does update without any trigger**: ABL leaves `PP0` in auto-refresh (`PP_AUTOREFRESH = 0x80000001`, tear
  check on). Writing pixels into the buffer ABL scans out is enough, like on sweet. Buffer `0x9d400000`, 1080 px stride, a
  `0xffRRGGBB` fill shows the right colours.
- The manual `CTL0/CTL1 START` kicks were not needed and the picture stopped updating after them, so v2 removes every
  display write.
- CPU0 (Kryo 385 Silver, `midr=517f803c`) reports `tg16=1`: the 16 KB translation granule iOS needs is supported.

## What v2 does (no storage writes, no display writes)

1. Dumps the display pipeline ABL left programmed (read-only): SSPP, CTL0/1, PP0/1, INTF1/2, DSI0.
2. Cycles blue / red / green / purple, 3 s each, twice (about 25 s), with the dump and a live `PP0C` counter on screen.
3. Clears the screen and runs the **read-only UFS probe** (`shim/ufs.c`: dumps the UFS host registers ABL left, and if the
   host is up reads the GPT of LUN 0; it contains no SCSI WRITE). Results stay on screen for about 3 minutes.
4. Resets to Android.

## Build

CI (no local toolchain): push to your GitHub fork, Actions -> **mi8-shim** -> Run workflow, download the
`mi8-diag` artifact. Local (Linux/WSL): `sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu`
then `bash mi8/build-mi8.sh` -> `mi8/out/mi8-diag-gz.img`, `mi8/out/mi8-diag-ucomp.img`.

The build also runs a host test of `shim/mi8.c` against a fake MMIO + ramoops (`mi8/host/`).

## Run (TWRP not needed; nothing is flashed)

```
fastboot boot mi8-diag-gz.img
```

- Phone not booting the image (stays in fastboot / "boot image invalid"): try `mi8-diag-ucomp.img`.
- Either one: if neither is accepted, note the exact fastboot message and send it.
- Watch the screen: expect the four colours twice, then a blue screen titled "ufs probe" with UFS lines. Take photos.
- If the phone hangs for more than ~90 s, hold power until it restarts (a forced power-off may lose the log).

## Reading results

The screen is the channel (no root needed). Photograph the colour-cycle frames and the UFS screen. With root, the same
dump is also in `/sys/fs/pstore/console-ramoops*` after the reset.

## UFS probe result on a real Mi 8 (2026-10-25)

sweet's UFS code worked unchanged: UFS 2.1 (`ver=0x210`), host enabled and ready (`hcs=0x10f`), TEST UNIT READY, READ
CAPACITY and READ all succeeded. LUN 0 is `0xdbffff + 1` blocks of 4096 bytes, GPT valid; `userdata` at LBA `0x220000`,
about 46.5 GiB. All four display colours were seen with no display register writes.

## Constants (from the live FDT of this phone)

| thing | value |
|---|---|
| ABL splash buffer | `0x9d400000`, 36 MB (cont_splash_region), panel 1080x2248 |
| MDP base | `0xae00000`; CTL `+0x2000/+0x200n`; PP `+0x71000/+0x800n`; INTF1 (DSI0) `+0x6b800`; DSI0 `0xae94000` |
| UFS | `0x1d84000` (SK hynix, ABL leaves HS-G3 2-lane unset; Linux negotiates later) |
| GICv3 | GICD `0x17a00000`, GICR `0x17a60000` stride `0x20000`, PSCI via smc |
| IMEM restart reason | `0x146bf65c` |
| RAM | 3 GB at `0x80000000` + about 3.95 GiB at `0x100000000..0x1fd400000`, no carve-outs above 4 GiB |
| ramoops | `0xb0000000`, 4 MB; console zone expected at `+0x100000` (also written at `+0x120000`) |
| CPUs | cpu0-3 Kryo 385 Silver (A55-based, part 0x803), cpu4-7 Gold (0x802) |
| boot image | header v1, 4 KB pages, DTB appended to the kernel, ABL selects by msm-id 321/0x20001 board-id 55 |

## Safety

`fastboot boot` only. The diagnostic never touches UFS or any partition. Back up the small partitions and
the GPT heads/tails (see the top-level README) before any later step that writes to `userdata`.
