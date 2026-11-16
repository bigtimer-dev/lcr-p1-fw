/* SPDX-License-Identifier: EUPL-1.2
 * Bootloader installer: an ordinary app image (0x08001800, flashed with `lcrp1 flash`) that carries a 6 KB
 * bootloader (build/boot/blob.c) and writes it to 0x08000000-0x080017FF after a confirmation on the meter.
 *
 *   - shows what is installed now (FNIRSI stock / LCR-P1 bootloader vX / unknown) and what it would write
 *   - refuses below 3.5 V battery (a power loss while writing the bootloader cannot be repaired without SWD)
 *   - refuses while the chip's read protection is on (FLASH OBR bit 1): it write-protects the first 4 KB, so only
 *     the last 2 KB would change and the loader would no longer boot. Happened on a meter 2026-11-16, repaired
 *     over SWD (FNIRSI ships the chip with read protection on).
 *   - hold TEST 3 s = install; ZENER = switch off without changing anything
 *   - writes, verifies the whole area against the embedded CRC, retries a failing page, reports on serial (#B)
 *   - afterwards TEST starts the new bootloader's update mode, to flash the real firmware
 */
#include <stdint.h>
#include <string.h>
#include "adc.h"
#include "board.h"
#include "font.h"
#include "hw.h"
#include "lcd.h"
#include "uart.h"

#define BL_BASE   0x08000000u
#define BL_SIZE   0x1800u
#define PAGE      512u
#define MIN_BAT   3500u
#define FL_KEY    R32(0x40022004u)
#define FL_STS    R32(0x4002200Cu)
#define FL_CTRL   R32(0x40022010u)
#define FL_ADD    R32(0x40022014u)
#define FL_OBR    R32(0x4002201Cu)        /* bit 1: read protection active */
#define FL_WRPR   R32(0x40022020u)        /* 0 bits: write-protected sectors */

extern const uint8_t blob[BL_SIZE];       /* build/boot/blob.c (boot/mkblob.py) */
extern const uint32_t blob_crc;
extern const char blob_name[];
extern const uint32_t stock_crc;           /* CRC32 of FNIRSI's loader as dumped 2026-11-14 */

static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n)
{
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static void say(int y, const char *s, uint16_t c)
{
    lcd_fill(0, y, LCD_W, 16, C_BLACK);
    lcd_text(0, y, s, &font16, c, C_BLACK);
}

/* what is in 0x08000000 now */
static void describe(char *out, int size, uint32_t crc)
{
    const uint32_t *info = (const uint32_t *)(BL_BASE + 0x17F0);
    if (crc == stock_crc)
        fmt(out, size, "FNIRSI stock");
    else if (info[0] == 0x314C424Cu && info[3] == ~0x314C424Cu && info[2] == crc32(0, (const uint8_t *)BL_BASE, 0x17F0))
        fmt(out, size, "LCR-P1 BL %u.%u", info[1] >> 8 & 255, info[1] & 255);
    else
        fmt(out, size, "unknown %x", crc);
}

static int fl_wait(void)
{
    for (uint32_t n = 0; n < 2000000u; n++)
        if (!(FL_STS & 1))
            return !(FL_STS & 0x14);
    return 0;
}

/* erase + program + verify one bootloader page; interrupts are off, the watchdog is fed here */
static int write_page(unsigned i)
{
    uint32_t a = BL_BASE + i * PAGE;
    const uint32_t *src = (const uint32_t *)(blob + i * PAGE);
    wdt_feed();
    FL_KEY = 0x45670123u;
    FL_KEY = 0xCDEF89ABu;
    FL_STS = 0x34;
    FL_CTRL = 2;
    FL_ADD = a;
    FL_CTRL = 0x42;
    int ok = fl_wait();
    FL_CTRL = 0;
    for (unsigned k = 0; ok && k < PAGE / 4; k++) {
        if (src[k] == 0xFFFFFFFFu)
            continue;
        FL_STS = 0x34;
        FL_CTRL = 1;
        ((volatile uint32_t *)a)[k] = src[k];
        ok = fl_wait();
        FL_CTRL = 0;
        wdt_feed();
    }
    FL_CTRL = 0x80;
    return ok && !memcmp((const void *)a, src, PAGE);
}

/* the new bootloader's update mode: request words + its reset vector (boot/bl.c) */
static void enter_bootloader(void)
{
    __asm volatile("cpsid i");
    *(volatile uint32_t *)0x20001FF8u = 0xB007104Du;
    *(volatile uint32_t *)0x20001FFCu = ~0xB007104Du;
    uint32_t sp = R32(BL_BASE), pc = R32(BL_BASE + 4);
    __asm volatile("msr msp, %0\n bx %1" : : "r"(sp), "r"(pc));
}

int main(void)
{
    char buf[24], cur[24];
    board_init();
    uart_init();
    lcd_init();
    adc_init();
    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    lcd_fill(0, 0, LCD_W, 18, C_ACCENT);
    lcd_text(4, 1, "BL installer", &font16, C_BLACK, C_ACCENT);

    uint32_t now = crc32(0, (const uint8_t *)BL_BASE, BL_SIZE);
    int blob_ok = crc32(0, blob, BL_SIZE) == blob_crc;
    describe(cur, sizeof cur, now);
    say(22, "Now:", C_LABEL);
    say(38, cur, C_WHITE);
    fmt(buf, sizeof buf, "New: %s", blob_name);
    say(56, buf, C_LABEL);
    uprintf("#B installer: now %s (crc %x), new %s (crc %x) %s\r\n", cur, now, blob_name, blob_crc,
            blob_ok ? "ok" : "CORRUPT");

    while (key_test_down())                           /* TEST from switching on is not a confirmation */
        delay_ms(10);
    int protect = (FL_OBR & 2) || (FL_WRPR & 0xFFFF) != 0xFFFF;
    uprintf("#B OBR %x WRPR %x%s\r\n", FL_OBR, FL_WRPR, protect ? " PROTECTED: will not write" : "");
    int state = protect ? 5 : !blob_ok ? 3 : now == blob_crc ? 2 : 0;   /* 0 ask, 1 done, 2 already, 3 corrupt,
                                                                           5 flash protected */
    uint32_t t_down = 0, shown = ~0u;
    for (;;) {
        wdt_feed();
        uint32_t bat = battery_mv();
        if (state == 0 && bat < MIN_BAT && bat > 2500)
            state = 4;                                /* 4 battery low (until charged above 3.5 V) */
        else if (state == 4 && bat >= MIN_BAT + 50)
            state = 0;
        if (state != (int)shown) {
            shown = (uint32_t)state;
            static const char *const l1[] = { "Hold TEST 3s", "Installed OK", "Already there", "Image CORRUPT",
                                              "Battery low", "Read-protected" };
            static const char *const l2[] = { "ZENER: cancel", "TEST: update", "TEST: update", "ZENER: off",
                                              "Charge > 3.5V", "ZENER: off" };
            say(80, l1[state], state == 1 || state == 2 ? C_GREEN : state >= 3 ? C_RED : C_WHITE);
            say(100, l2[state], C_LABEL);
        }
        if (key_zener_down() && state != 1) {
            uprintf("#B cancelled\r\n");
            delay_ms(20);
            power_off();
        }
        if (!key_test_down()) {
            t_down = 0;
        } else if (!t_down) {
            t_down = g_ms;
            if (state == 1 || state == 2) {
                uprintf("#B starting bootloader update mode\r\n");
                delay_ms(20);
                while (key_test_down())               /* the bootloader would take a held TEST as "off" */
                    delay_ms(10);
                enter_bootloader();
            }
        } else if (state == 0 && g_ms - t_down >= 3000) {
            say(80, "Writing...", C_ACCENT);
            say(100, "DO NOT POWER OFF", C_RED);
            uprintf("#B writing\r\n");
            delay_ms(30);
            __asm volatile("cpsid i");
            int ok = 0;
            for (int pass = 0; pass < 5 && !ok; pass++) {
                ok = 1;
                for (unsigned i = 0; i < BL_SIZE / PAGE; i++)
                    if (memcmp((const void *)(BL_BASE + i * PAGE), blob + i * PAGE, PAGE) && !write_page(i))
                        ok = 0;
                ok = ok && crc32(0, (const uint8_t *)BL_BASE, BL_SIZE) == blob_crc;
            }
            __asm volatile("cpsie i");
            if (ok) {
                describe(cur, sizeof cur, blob_crc);
                say(38, cur, C_WHITE);
                uprintf("#B installed %s\r\n", cur);
                state = 1;
            } else {
                /* keep trying: switching off now would leave no working bootloader */
                say(80, "WRITE FAILED", C_RED);
                say(100, "retrying...", C_RED);
                uprintf("#B FAILED, retrying\r\n");
                delay_ms(1000);
                t_down = g_ms - 3000;
            }
        }
        delay_ms(10);
    }
}
