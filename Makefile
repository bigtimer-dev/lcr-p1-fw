# LCR-P1 open firmware. Needs arm-none-eabi-gcc / binutils, python3.
#   make            -> build/lcrp1-fw.bin (flash with: lcrp1 flash build/lcrp1-fw.bin)
#   make test       -> emulator tests (needs a venv with unicorn: python3 -m venv .venv && .venv/bin/pip install unicorn)
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
CFLAGS = -mcpu=cortex-m0 -mthumb -Os -g -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-common \
         -ffunction-sections -fdata-sections -Isrc
LDFLAGS = -mcpu=cortex-m0 -mthumb -nostartfiles -nostdlib -Wl,--gc-sections -Wl,-Map=$(BUILD)/lcrp1-fw.map \
          -T ld/lcrp1.ld
SRC = $(wildcard src/*.c) src/font_data.c
BUILD ?= build
OBJ = $(patsubst src/%.c,$(BUILD)/%.o,$(sort $(SRC)))
# ComponentTester (m-firmware, EUPL-1.2), converted by tools/ctport.py into ct/ported/
CT_SRC = ct/ported/probes.c ct/ported/resistor.c ct/ported/semi.c ct/ported/ct_extract.c
CT_OBJ = $(patsubst ct/ported/%.c,$(BUILD)/ct_%.o,$(CT_SRC))
CT_INC = -DLCRP1 -DF_CPU=48000000UL -Ict/ported -Ict -Ict/compat $(if $(filter 1,$(CT_DEBUG)),-DCT_DEBUG)
CT_CFLAGS = -mcpu=cortex-m0 -mthumb -Os -g -std=gnu11 -Wall -Werror -Wno-unused-but-set-variable \
            -ffreestanding -fno-common -ffunction-sections -fdata-sections -Isrc $(CT_INC)
OBJ += $(CT_OBJ)
PY ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

all: $(BUILD)/lcrp1-fw.bin

src/font_data.c: tools/mkfont.py fonts/ter-u16n.bdf fonts/ter-u24b.bdf
	python3 tools/mkfont.py > $@

$(BUILD)/%.o: src/%.c src/*.h ct/*.h ct/ported/*.h  | $(BUILD)
	$(CC) $(CFLAGS) $(CT_INC) -c $< -o $@

# LCR-P1 files built on the upstream headers: upstream's warning level
$(BUILD)/ct_glue.o $(BUILD)/ct_stubs.o $(BUILD)/measure.o: CFLAGS = $(CT_CFLAGS)
$(BUILD)/libc.o: CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin

$(BUILD)/ct_%.o: ct/ported/%.c ct/ported/*.h ct/*.h src/ct_hal.h  | $(BUILD)
	$(CC) $(CT_CFLAGS) -c $< -o $@

$(BUILD)/lcrp1-fw.elf: $(OBJ) ld/lcrp1.ld
	$(CC) $(LDFLAGS) $(OBJ) -lgcc -o $@
	arm-none-eabi-size $@

$(BUILD)/lcrp1-fw.bin: $(BUILD)/lcrp1-fw.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD):
	mkdir -p $(BUILD)

test: build/lcrp1-fw.bin
	$(PY) test/emu_test.py build/lcrp1-fw.bin build/lcrp1-fw.elf
	$(PY) test/emu_test.py --lowbat build/lcrp1-fw.bin build/lcrp1-fw.elf | grep -v '^PASS'
	@# the CT_DEBUG instrumentation must not change behaviour: same tests on a debug build
	rm -rf build-debug && $(MAKE) -s BUILD=build-debug CT_DEBUG=1 build-debug/lcrp1-fw.bin >/dev/null
	$(PY) test/emu_test.py build-debug/lcrp1-fw.bin build-debug/lcrp1-fw.elf | grep -v '^PASS'

clean:
	rm -rf build build-debug

.PHONY: all test clean
