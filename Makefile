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
# ComponentTester (m-firmware, EUPL-1.2), converted by tools/ctport.py into ct/ported/
CT_SRC = ct/ported/probes.c ct/ported/resistor.c ct/ported/ct_extract.c
CT_OBJ = $(patsubst ct/ported/%.c,build/ct_%.o,$(CT_SRC))
CT_INC = -DLCRP1 -DF_CPU=48000000UL -Ict/ported -Ict -Ict/compat $(if $(filter 1,$(CT_DEBUG)),-DCT_DEBUG)
CT_CFLAGS = -mcpu=cortex-m0 -mthumb -Os -g -std=gnu11 -Wall -Werror -Wno-unused-but-set-variable \
            -ffreestanding -fno-common -ffunction-sections -fdata-sections -Isrc $(CT_INC)
OBJ += $(CT_OBJ)
PY ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

all: build/lcrp1-fw.bin

src/font_data.c: tools/mkfont.py fonts/ter-u16n.bdf fonts/ter-u24b.bdf
	python3 tools/mkfont.py > $@

build/%.o: src/%.c src/*.h ct/*.h ct/ported/*.h | build
	$(CC) $(CFLAGS) $(CT_INC) -c $< -o $@

# LCR-P1 files built on the upstream headers: upstream's warning level
build/ct_glue.o build/ct_stubs.o build/measure.o: CFLAGS = $(CT_CFLAGS)
build/libc.o: CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin

build/ct_%.o: ct/ported/%.c ct/ported/*.h ct/*.h src/ct_hal.h | build
	$(CC) $(CT_CFLAGS) -c $< -o $@

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
