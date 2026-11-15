# LCR-P1 open firmware

Replacement firmware for the **FNIRSI LCR-P1** component tester, written from scratch. The plan is to bring
the measurement algorithms of Markus Reschke's ComponentTester (the refined successor of Karl-Heinz
Kübbeler's TransistorTester, which the stock firmware is also derived from) to this board.

Hardware: Nations **N32G032C8L7** (Cortex-M0, 48 MHz, 64 KB flash, 16 KB RAM), the classic 3-probe network
(680 Ω + 470 kΩ per probe), 1.44" 128x128 ST7735S panel, probe-shorting relay, HV boost for Zener tests,
IR receiver, CH340 USB serial. Pin map: see `docs/` (TODO) / the companion RE notes.

Not affiliated with FNIRSI. The FNIRSI bootloader (0x08000000–0x080017FF) is not touched: hold **ZENER**,
then press **TEST** to enter update mode at any time, so stock firmware can always be restored.

## Status

| Phase | | |
|---|---|---|
| 0 | hardware map | done |
| 1 | bring-up: clock, power latch, LCD, keys, mode switch, relay, buzzer, backlight, USB serial, watchdog | done, runs on the meter |
| 2 | measurement core: ADC + probe layer + ComponentTester resistors verified on the meter (10R, 360R, 1M, all socket pairs); C, diodes, transistors next | in progress |
| 3 | IR decoding, serial protocol, UI polish | |

Phase 1 shows the key, switch and relay states. TEST toggles the relay, ZENER beeps (the HV boost is never
switched on), holding TEST for 1 s powers off; it also switches off after 3 min idle. The USB serial port
(115200 8N1) logs events and answers `?` with a status line. There is **no low-battery cut-off** yet.

## Build, test, flash

```sh
make                       # build/lcrp1-fw.bin (needs arm-none-eabi-gcc)
python3 -m venv .venv && .venv/bin/pip install unicorn
make test                  # boots the image in an emulated board, renders build/screen_*.png
lcrp1 flash build/lcrp1-fw.bin   # from github.com/bigtimer-dev/lcr-p1-linux, meter in update mode
```

The emulator test checks the power latch, clock setup, that the LCD init is byte-identical to what the stock
firmware sends, keys, relay, buzzer and that the HV boost pin is never set.

## ComponentTester code

`ct/upstream/` holds the unmodified ComponentTester 1.57m sources (© Markus Reschke, EUPL-1.2, from
github.com/madires/Transistortester-Warehouse). `tools/ctport.py` converts the files in use into `ct/ported/`:
Latin-1 to UTF-8, the AVR port register statements (`R_DDR = x;` ...) into calls to the LCR-P1 probe layer
(`src/ct_hal.c`), a few marked MCU-specific blocks, and the board selections in `config.h`. The board
configuration is `ct/config_lcrp1.h`. Re-run the script after an upstream update.

## Licence

EUPL-1.2 (see LICENSE). Fonts: Terminus Font 4.49.1, © Dimitar Toshkov Zhekov, SIL Open Font License 1.1
(`fonts/OFL.TXT`), converted to bitmaps by `tools/mkfont.py`.
