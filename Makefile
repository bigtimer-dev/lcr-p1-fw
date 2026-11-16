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
CT_SRC = ct/ported/probes.c ct/ported/resistor.c ct/ported/semi.c ct/ported/cap.c ct/ported/inductor.c ct/ported/ct_extract.c
CT_OBJ = $(patsubst ct/ported/%.c,$(BUILD)/ct_%.o,$(CT_SRC))
CT_INC = -DLCRP1 -DF_CPU=48000000UL -Ict/ported -Ict -Ict/compat $(if $(filter 1,$(CT_DEBUG)),-DCT_DEBUG)
CT_CFLAGS = -mcpu=cortex-m0 -mthumb -Os -g -std=gnu11 -Wall -Werror -Wno-unused-but-set-variable \
            -ffreestanding -fno-common -ffunction-sections -fdata-sections -Isrc $(CT_INC)
OBJ += $(CT_OBJ)
PY ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

all: $(BUILD)/lcrp1-fw.bin

# bootloader (boot/README.md): build/lcrp1-bl.bin, exactly 6 KB for 0x08000000
BL_CFLAGS = -mcpu=cortex-m0 -mthumb -Os -g -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-common \
            -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns -fno-builtin
boot: build/lcrp1-bl.bin

boot/font.c: boot/mkfont.py boot/bl.c fonts/ter-u16n.bdf
	python3 boot/mkfont.py > $@

build/boot/%.o: boot/%.c src/hw.h | build/boot
	$(CC) $(BL_CFLAGS) -c $< -o $@

build/boot/libc.o: src/libc.c | build/boot
	$(CC) $(BL_CFLAGS) -c $< -o $@

build/boot/bl.elf: build/boot/bl.o build/boot/font.o build/boot/libc.o boot/bl.ld
	$(CC) -mcpu=cortex-m0 -mthumb -nostartfiles -nostdlib -Wl,--gc-sections -Wl,-Map=build/boot/bl.map \
	    -T boot/bl.ld build/boot/bl.o build/boot/font.o build/boot/libc.o -lgcc -o $@
	arm-none-eabi-size $@

build/lcrp1-bl.bin: build/boot/bl.elf boot/mkinfo.py
	$(OBJCOPY) -O binary --gap-fill 0xff $< build/boot/bl.raw
	python3 boot/mkinfo.py build/boot/bl.raw $@

build/boot:
	mkdir -p build/boot

# installer app (boot/install.c): writes BLOB to 0x08000000 after confirmation on the meter.
#   make installer                -> build/lcrp1-bl-install.bin (this bootloader)
#   make installer BLOB=stock.bin BLOB_NAME="FNIRSI stock" IDIR=build/inst-stock   (restore a dumped loader)
STOCK_BL_CRC = 0x97e1e1e5    # CRC32 of the FNIRSI loader dumped 2026-11-14 (the dump itself is not in the repo)
BLOB ?= build/lcrp1-bl.bin
BL_VER := $(shell sed -n 's/^#define BL_VERSION "\(.*\)"/\1/p' boot/bl.c)
BLOB_NAME ?= LCR-P1 BL $(BL_VER)
IDIR ?= build/inst
INST_SRC = boot/install.c src/startup.c src/board.c src/lcd.c src/uart.c src/font_data.c src/libc.c src/adc.c
INST_OBJ = $(patsubst %.c,$(IDIR)/%.o,$(notdir $(INST_SRC))) $(IDIR)/blob.o
installer: $(IDIR)/lcrp1-bl-install.bin
	@cp $< $(if $(filter build/inst,$(IDIR)),build/lcrp1-bl-install.bin,$(IDIR).bin) && echo "-> $(if $(filter build/inst,$(IDIR)),build/lcrp1-bl-install.bin,$(IDIR).bin)"

$(IDIR)/blob.c: $(BLOB) boot/mkblob.py | $(IDIR)
	python3 boot/mkblob.py $(BLOB) "$(BLOB_NAME)" $(STOCK_BL_CRC) > $@

$(IDIR)/%.o: boot/%.c src/*.h | $(IDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IDIR)/%.o: src/%.c src/*.h | $(IDIR)
	$(CC) $(CFLAGS) $(if $(filter libc.c,$(notdir $<)),-fno-tree-loop-distribute-patterns -fno-builtin) -c $< -o $@
$(IDIR)/blob.o: $(IDIR)/blob.c
	$(CC) $(CFLAGS) -c $< -o $@

$(IDIR)/lcrp1-bl-install.bin: $(INST_OBJ) ld/lcrp1.ld
	$(CC) -mcpu=cortex-m0 -mthumb -nostartfiles -nostdlib -Wl,--gc-sections -T ld/lcrp1.ld $(INST_OBJ) -lgcc -o $(IDIR)/inst.elf
	arm-none-eabi-size $(IDIR)/inst.elf
	$(OBJCOPY) -O binary $(IDIR)/inst.elf $@

$(IDIR):
	mkdir -p $(IDIR)

src/font_data.c: tools/mkfont.py fonts/ter-u16n.bdf fonts/ter-u24b.bdf
	python3 tools/mkfont.py > $@

$(BUILD)/%.o: src/%.c src/*.h ct/*.h ct/ported/*.h  | $(BUILD)
	$(CC) $(CFLAGS) $(CT_INC) -c $< -o $@

# LCR-P1 files built on the upstream headers: upstream's warning level
$(BUILD)/ct_glue.o $(BUILD)/measure.o $(BUILD)/adjust.o: CFLAGS = $(CT_CFLAGS)
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

# bootloader + installer in the emulator (slow: ~10 min), then the PC flasher against it (needs ../lcr-p1-linux)
test-boot: boot installer build/lcrp1-fw.bin
	$(PY) -u test/bl_test.py build/lcrp1-bl.bin build/lcrp1-fw.bin
	$(PY) -u test/host_test.py

clean:
	rm -rf build build-debug

.PHONY: all boot installer test test-boot clean
