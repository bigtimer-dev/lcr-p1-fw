# LCR-P1 open firmware. Needs arm-none-eabi-gcc / binutils, python3.
#   make            -> build/lcrp1-fw.bin (flash with: lcrp1 flash build/lcrp1-fw.bin)
#   make test       -> emulator tests (needs a venv with unicorn: python3 -m venv .venv && .venv/bin/pip install unicorn)
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
CFLAGS = -mcpu=cortex-m0 -mthumb -Os -g -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-common \
         -ffunction-sections -fdata-sections -Isrc
LDFLAGS = -mcpu=cortex-m0 -mthumb -nostartfiles -nostdlib -Wl,--gc-sections -Wl,-Map=build/lcrp1-fw.map \
          -T ld/lcrp1.ld
SRC = $(wildcard src/*.c) src/font_data.c
OBJ = $(patsubst src/%.c,build/%.o,$(sort $(SRC)))
PY ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

all: build/lcrp1-fw.bin

src/font_data.c: tools/mkfont.py fonts/ter-u16n.bdf fonts/ter-u24b.bdf
	python3 tools/mkfont.py > $@

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/lcrp1-fw.elf: $(OBJ) ld/lcrp1.ld
	$(CC) $(LDFLAGS) $(OBJ) -lgcc -o $@
	arm-none-eabi-size $@

build/lcrp1-fw.bin: build/lcrp1-fw.elf
	$(OBJCOPY) -O binary $< $@

build:
	mkdir -p build

test: build/lcrp1-fw.bin
	$(PY) test/emu_test.py build/lcrp1-fw.bin build/lcrp1-fw.elf
	$(PY) test/emu_test.py --lowbat build/lcrp1-fw.bin build/lcrp1-fw.elf | grep -v '^PASS'

clean:
	rm -rf build

.PHONY: all test clean
