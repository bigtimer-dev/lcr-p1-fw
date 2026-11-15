# LCR-P1 open firmware

Replacement firmware for the **FNIRSI LCR-P1** component tester, written from scratch. The plan is to bring
the measurement algorithms of Markus Reschke's ComponentTester (the refined successor of Karl-Heinz
Kübbeler's TransistorTester, which the stock firmware is also derived from) to this board.

Hardware: Nations **N32G032C8L7** (Cortex-M0, 48 MHz, 64 KB flash, 16 KB RAM), the classic 3-probe network
(680 Ω + 470 kΩ per probe), 1.44" 128x128 ST7735S panel, probe-shorting relay, HV boost for Zener tests,
IR receiver, CH340 USB serial.

Not affiliated with FNIRSI. The FNIRSI bootloader (0x08000000–0x080017FF) is not touched: hold **ZENER**,
then press **TEST** to enter update mode at any time, so stock firmware can always be restored.

## Status

Version 0.8 runs on the meter and measures with ComponentTester's algorithms:

| | |
|---|---|
| resistors | 1 or 2 per test (checked on the meter from 10 Ω to 1 MΩ) |
| capacitors | pF to mF, with **ESR** (from 10 nF) and leakage current |
| diodes | forward voltage, anti-parallel pairs |
| BJTs | NPN / PNP, pinout, hFE, Iceo |
| FETs | pinout, gate threshold (Vth) |
| inductors | from about 20 µH, when the coil is below 2 kΩ (shown with its resistance) |
| self-adjustment | R0 (probe resistance), RiL / RiH (MCU pin resistance), C0 (probe capacitance) |

Not done yet: Zener test (the HV boost is never switched on), IR decoding.

Typical test times: empty socket 1.1 s, resistor 1.2 s, 330 µF electrolytic 5.6 s (large caps need time to
charge and discharge).

### Use

- **TEST**: measure the part in the socket. The result stays on screen until the next key.
- **ZENER**: switch between the result and a status page.
- **ZENER held + TEST**: self-adjustment. Short all three probes and press TEST, then remove the short and
  press TEST. The values are stored in flash and survive firmware updates. ZENER cancels.
- **TEST held 1 s**: power off. The meter also switches off after 3 min without use and at 3.3 V battery.
- **ZENER held, then TEST** while off: FNIRSI update mode (bootloader, untouched).

### ESR

As upstream: 255 short current pulses through the 680 Ω resistors in both directions, the voltage sampled
at the end of each pulse, the cap's own voltage cancelled by a reverse pre-charge pulse. Upstream's AVR
timer delays are replaced by GPIO writes timed against the measured sampling instant of the ADC
(`#E`, debug build), and the result is computed from the full voltage divider instead of upstream's
small-ESR approximation (with 2 kΩ in series: 1913 Ω instead of 504 Ω). Low ESR readings agree with a
DE-5000 at 10 kHz; near 0 Ω repeated readings vary by about 0.1 Ω.

### Inductors

As upstream: probe-1 is switched high and the time until the current through the coil (on Rl, or for
coils below 40 Ω on the pin's own resistance) reaches the comparator threshold gives L. The comparator
and timer are COMP1 and TIM3 at 48 MHz. Two corrections were fitted on the meter: 10 timer ticks of fixed
delay, and the threshold scaled by 0.83 in the high-current mode, where the pin reaches it sooner than
its resistance at low current predicts. Checked on the meter: 50 µH reads 47.2 µH, 36 µH reads 34.4 µH,
2.2 mH reads 2.30 mH; plain resistors (0 Ω, 10 Ω, 2 kΩ) show no inductance. Coils above 40 Ω (the
low-current mode) are not checked on the meter yet.

### Serial port

USB serial, 115200 8N1. The meter logs events and the ADC once a second (`#A ...`) and takes commands
(`#` + letter + optional number, then Enter):

| | |
|---|---|
| `#T` | run a test (result as `#R` lines) |
| `#J` | self-adjustment (`T` / `Z` over serial stand for TEST / ZENER) |
| `#N` | print the adjustment values |
| `#?` | status line |
| `#S<n>`, `#P<n>` | ADC sample time code for measurements / fast polls |
| `#W` | measure with sample time codes 12..15 |
| `#D`, `#G`, `#X` | probe network diagnostics, FET gate trace, drive experiment |

The debug build (`make CT_DEBUG=1 BUILD=build-dbg`) adds upstream's measurement details (`#C`), per-step
timing (`#T <step> ms= sleep= adc=`) and the ADC timing scan `#E<code>`. `tools/serlog.py` streams the
port line by line and follows the adapter when the meter is switched off and on.

## Build, test, flash

```sh
make                       # build/lcrp1-fw.bin (needs arm-none-eabi-gcc)
python3 -m venv .venv && .venv/bin/pip install unicorn
make test                  # boots the image in an emulated board, renders build/screen_*.png
lcrp1 flash build/lcrp1-fw.bin   # from github.com/bigtimer-dev/lcr-p1-linux, meter in update mode
```

The emulator test runs the image on a model of the board (probe network, relay, comparator + timer capture,
ADC, flash controller, LCD, UART) and checks start-up, keys, power-off, the LCD init (byte-identical to
stock), measurements of resistors, capacitors with ESR, diodes and transistors, the self-adjustment and
the flash store, and that the HV boost pin is never set.

## ComponentTester code

`ct/upstream/` holds the unmodified ComponentTester 1.57m sources (© Markus Reschke, EUPL-1.2, from
github.com/madires/Transistortester-Warehouse). `tools/ctport.py` converts the files in use into `ct/ported/`:
Latin-1 to UTF-8, the AVR port register statements (`R_DDR = x;` ...) into calls to the LCR-P1 probe layer
(`src/ct_hal.c`), a few marked MCU-specific blocks, and the board selections in `config.h`. The board
configuration is `ct/config_lcrp1.h`. Re-run the script after an upstream update.

## Licence

EUPL-1.2 (see LICENSE). Fonts: Terminus Font 4.49.1, © Dimitar Toshkov Zhekov, SIL Open Font License 1.1
(`fonts/OFL.TXT`), converted to bitmaps by `tools/mkfont.py`.
