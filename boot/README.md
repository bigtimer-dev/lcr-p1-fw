# LCR-P1 bootloader

A replacement for FNIRSI's IAP bootloader (0x08000000-0x080017FF, 6 KB). It starts every app the FNIRSI
loader starts, in the same way, and still accepts FNIRSI's update tool, but adds:

- **Checked updates.** In the framed protocol every frame carries a CRC32. Each page is verified after
  writing, and the image's CRC32 is checked before the app's first page goes in. An interrupted or
  corrupt update leaves no startable app, so the meter stays in update mode instead of crashing.
- **Recovery without keys.** If there is no valid app (bad initial SP or reset vector), the meter
  switches on in update mode.
- **Entry from the firmware.** Send `#U` on serial to a running lcrp1-fw and it hands over to update
  mode. The PC flasher does this itself, so ZENER+TEST is no longer needed.
- **Timeouts and power.** A stalled transfer ends after 3 s. Hold TEST 1 s to switch off. Update mode
  switches itself off after 5 minutes without data.
- **Read-back.** You can read any flash (bootloader, app, calibration pages) and get CRCs of any range.
  `lcrp1 flash --backup` uses this.
- **Faster link.** Optional higher baud rate (`--baud 460800`). If the new rate doesn't work within 1 s,
  the meter falls back to 115200.
- **A screen.** Update mode shows version, entry reason, address, progress and result.

It never writes below 0x08001800: the bootloader cannot overwrite itself.

## Hand-off to the app

The direct boot path (no key, valid app) leaves the hardware exactly as FNIRSI's loader does. That means
the same clock setup, GPIO registers (including the stock loader's quirks), vector remap at 0x40024C30,
NVIC state, PRIMASK, and even the core registers r0-r7, r12 and lr. The stock app reads some of these
uninitialised, for example as TIM1's repetition counter. `test/bl_test.py` checks that the peripheral
state and the first ~800k MMIO writes of both the stock app and lcrp1-fw are identical under both
loaders.

## Entering update mode

| How | Screen says |
| --- | --- |
| hold ZENER, press TEST (as with FNIRSI's loader) | `ZENER key` |
| `#U` to running lcrp1-fw (app writes `0xB007104D`, `~0xB007104D` to 0x20001FF8/FFC and jumps to the loader) | `asked by app` |
| no valid app in flash | `no valid app` |

## Protocols

**FNIRSI (legacy), unchanged.** Send `AC 55 02 03`, then the image in 512-byte blocks without acks.
The last block ends with `78 56 34 12`. The reply is `AC 55 03 04`, or `AC 55 04 xx` on error (new).
Block 0 is kept in RAM and written last, after the end marker, so a cut transfer cannot leave a
half-written app that the loader would start.

**Framed.** After the FNIRSI header, a host that stays quiet for 500 ms gets a hello: an INFO reply with
cmd 0. FNIRSI hosts start sending blocks within ~10 ms, so they never see it. After that, or at any time
in update mode, the host can send:

```
host  A5 cmd len16 payload crc32(cmd, len16, payload)
meter 5A cmd status len16 payload crc32(cmd, status, len16, payload)
```

All values are little-endian. CRC32 is zlib's.

| cmd | payload | reply |
| --- | --- | --- |
| 1 INFO | - | `"LCRPBL" maj min app_base end page max_write bl_crc valid entry pending` (`<6sBBIIHHIBBB`) |
| 2 READ | addr u32, len u16, 2 pad | up to 512 bytes of flash (0x08000000-0x0800FFFF) |
| 3 CRC | addr u32, len u32 | crc32 u32 |
| 5 WRITE | addr u32 (page aligned, ≥ 0x08001800) + 512 bytes | - (the page is erased, written and verified; the app's first page is only buffered) |
| 6 COMMIT | len u32, crc32 u32 of the image | - (checks the CRC over the buffered first page + flash, then writes the first page) |
| 7 BOOT | - | - then starts the app (status 6 if there is none) |
| 8 OFF | - | - then powers off |
| 9 BAUD | rate u32 | - then switches; send any frame within 1 s at the new rate or it falls back to 115200 |
| 10 BEGIN | len u32 | - erases the app's first page (the old app is invalid from here on) |

Status codes: 0 ok, 1 frame CRC error (send again), 2 unknown command, 3 bad arguments, 4 flash error,
5 image CRC mismatch, 6 no valid app.

The loader's own version and CRC sit in a 16-byte block at 0x080017F0: `"LBL1"` magic, version u16,
used size u16, crc32 of 0x08000000-0x080017EF, and the inverted magic. lcrp1-fw and the installer use it
to recognise the loader.

## Building

```
make boot                  # build/lcrp1-bl.bin (6 KB, stamped by boot/mkinfo.py)
make installer             # build/lcrp1-bl-install.bin: an app image that installs it
make test-boot PY=../lcr-p1-linux/.venv/bin/python    # emulator tests (unicorn), ~10 min
```

## Installing

> **Meters as FNIRSI ships them need SWD (see [Installing over SWD](#installing-over-swd)).** The chip
> comes with read protection on (FLASH OBR 0x4002201C bit 1). That write-protects the first 4 KB of
> flash, so an app can rewrite only the last 2 KB of the bootloader area. On 2026-11-16 the first
> installer did exactly that and left a mix of FNIRSI's first 4 KB and this loader's last 2 KB. The meter
> no longer booted. It was repaired over SWD (a Raspberry Pi as the probe, the CPU single-stepping the
> installer's own `ldr`/`str` instructions) by writing FNIRSI's original 2 KB back. The installer now
> checks OBR/WRPR and refuses ("Read-protected"). The same meter was then installed over SWD.
>
> There is no way to repair a broken bootloader without SWD. The installer is the only code that writes
> 0x08000000-0x080017FF. It refuses below 3.5 V battery, retries and verifies, and keeps retrying rather
> than switching off with a broken loader. Use a charged battery, and don't switch off while the screen
> says "Writing...".

On a chip without read protection:

1. Hold ZENER and press TEST: FNIRSI's update mode. Then run `lcrp1 flash build/lcrp1-bl-install.bin`.
2. The installer shows the current loader ("FNIRSI stock" if the dump CRC matches) and the new one.
   Hold TEST 3 s to install. Press ZENER to switch off without changes.
3. "Installed OK". Release TEST, then press TEST: the new loader's update mode starts.
4. Run `lcrp1 flash build/lcrp1-fw.bin` (or any FNIRSI image). From now on `lcrp1 flash` works with
   the meter running lcrp1-fw.

To go back to FNIRSI's loader, build an installer around your own dump. The dump is not in this repo:

```
make installer BLOB=path/to/bootloader.bin BLOB_NAME="FNIRSI stock" IDIR=build/inst-stock
```

Flash the resulting `build/inst-stock.bin` the same way. Run `lcrp1 flash --backup` first if you want a
full image of the flash.

## Installing over SWD

This removes FNIRSI's read protection, which mass-erases the flash, and writes the loader, lcrp1-fw
and the meter's own calibration back. Done on a real meter on 2026-11-16 with a Raspberry Pi Zero 2 W
and OpenOCD 0.12 (`bcm2835gpio`). Scripts are in [`swd/`](swd). FNIRSI's loader is gone afterwards.
From then on the chip is unprotected, so SWD can read and write everything directly.

**Wiring.** The 4 holes on the battery side, left of the buzzer, are CLK, GND, DIO, 5V. Connect
CLK to Pi pin 22 (GPIO25), DIO to pin 18 (GPIO24), GND to pin 20. Leave 5V open. Run `pads.py 0x08`
(2 mA, slew limited) before every OpenOCD run, or the reads come back garbled from ringing. OpenOCD resets
the pads when it exits.

**The chip.** N32G032; the option bytes follow the N32G031 user manual (sections 2.2.1.7 to 2.2.1.10):
RDP1 is the low byte of 0x1FFFF600, RDP2 that of 0x1FFFF60C. Level 2 (permanent, SWD disabled) is
RDP2 = 0xCC with nRDP2 = 0x33. **Nothing here writes 0x1FFFF60C.** Under level 1 the debugger cannot
read flash or run flash commands itself, so the scripts let the CPU single-step one `ldr`/`str`
instruction in FNIRSI's loader (0x08000392 `ldr r1,[r1]`, 0x08000258 `str r2,[r0]`). This works only
with FNIRSI's original loader (CRC32 97e1e1e5). The FNIRSI loader does not start the watchdog, so the
core may stay halted. lcrp1-fw does start it: never halt it for more than a moment.

1. Hold ZENER, press TEST and keep holding: FNIRSI's update mode. On the Pi run `sudo ./wait_latch.sh`.
   It halts the core and switches the power latch PA8 on. Release TEST.
2. `openocd -f n32slow.cfg -f backup.tcl > backup.txt`: read-only. It prints the option bytes (factory:
   `00ff00ff 00ff00ff 00ff00ff 00ffff00`) and the 2 KB at 0x0800F800-0x0800FFFF: lcrp1-fw's adjustment
   page and FNIRSI's calibration record (`dEnK` at 0x0800FFD0). Turn the `W addr value` lines into a
   2 KB little-endian file and check the record before going on.
3. `openocd -f n32slow.cfg -f rdp0.tcl`: unlocks FLASH and the option bytes, erases the option page,
   then writes 0x00FF00A5 to 0x1FFFF600 (RDP1 = 0xA5, USER = 0xFF). The chip then mass-erases its flash.
   It stops before writing if the unlock or the erase does not check out.
4. Hold TEST, then `openocd -f n32slow.cfg -f reset0.tcl`. It resets (the option bytes reload) and
   latches the power again. It should print OB 0x03fffffc, and the debugger now reads 0xffffffff from
   flash. Release TEST.
5. `./mkprog.py lcrp1-bl.bin lcrp1-fw.bin backup-2k.bin`, copy `prog_data.tcl` next to `prog.tcl`,
   run `openocd -f n32slow.cfg -f prog.tcl`. It programs from the debugger (87 s at 50 kHz) and saves
   `readback.bin`, whose sha256 must equal `image64k.bin`.
6. `openocd -f n32slow.cfg -f go.tcl` resets: the meter starts lcrp1-fw through the new loader.
   `lcrp1 flash --backup` then reads the same 64 KB over serial.
