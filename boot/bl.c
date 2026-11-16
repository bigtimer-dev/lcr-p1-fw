/* SPDX-License-Identifier: EUPL-1.2
 * LCR-P1 bootloader: replaces FNIRSI's IAP loader in 0x08000000-0x080017FF (6 KB).
 *
 * Start-up, as stock: power latch, clock, keys; ZENER held at power-on = update mode, otherwise the app at
 * 0x08001800 is started exactly the way the stock loader does it (vector remap 0x40024C30, same clocks off).
 * Also update mode: the app's vector page is not a valid app (erased by an unfinished update), or the app
 * asked for it (two magic words at 0x20001FF8 and a jump to our reset vector, see boot/README.md).
 *
 * Update mode speaks two protocols on USART1 (115200 8N1), received by DMA into a ring so nothing is lost
 * while the CPU stalls on flash erase:
 *   - FNIRSI's (BootTool.exe, `lcrp1 flash` up to v1.2): AC 55 02 03, then 512-byte blocks to 0x08001800+,
 *     the last one ending with 78 56 34 12; reply AC 55 03 04. Now every page is verified, and the app's
 *     first page (vector table) is held in RAM and written last, so an interrupted update never boots.
 *   - Framed with CRC32 and replies (A5 ...), see boot/README.md: info, read (backup), CRC, page write,
 *     commit with whole-image CRC, boot, power off, baud rate.
 * Pages below 0x08001800 are never erased or written. Idle 5 min or TEST held 1 s = power off.
 * No interrupts are used.
 */
#include <stdint.h>
#include <string.h>
#include "../src/hw.h"

#define BL_VER_MAJ 1
#define BL_VER_MIN 0
#define BL_VERSION "1.0"
#define APP_BASE   0x08001800u
#define FLASH_END  0x08010000u
#define PAGE       512u
#define REMAP      R32(0x40024C30u)       /* N32 vector table remap (Cortex-M0 here has no VTOR) */
#define MAGIC      ((volatile uint32_t *)0x20001FF8u)  /* app -> bootloader request, above every app's stack */
#define MAGIC_REQ  0xB007104Du
#define LEGACY_END 0x12345678u            /* last word of FNIRSI's last block */
#define IDLE_OFF_MS (5u * 60u * 1000u)

/* flash controller (as src/adjust.c, verified on the meter) */
#define FL_KEY   R32(0x40022004u)
#define FL_STS   R32(0x4002200Cu)
#define FL_CTRL  R32(0x40022010u)
#define FL_ADD   R32(0x40022014u)

/* DMA channel 2 = USART1 RX (stock loader: request select 2, circular, byte, memory increment) */
#define DMA_CH2   0x4002001Cu
#define DMA_CFG   R32(DMA_CH2 + 0x00)
#define DMA_CNT   R32(DMA_CH2 + 0x04)
#define DMA_PADDR R32(DMA_CH2 + 0x08)
#define DMA_MADDR R32(DMA_CH2 + 0x0C)
#define DMA_SEL   R32(DMA_CH2 + 0x10)
#define T6_CNT    R32(TIM6 + 0x24)
#define NVIC_ICER R32(0xE000E180u)
#define NVIC_ICPR R32(0xE000E280u)
#define AIRCR     R32(0xE000ED0Cu)

/* screen: RGB565 */
#define C_BG     0x0000u
#define C_FG     0xFFFFu
#define C_ACCENT 0xB47Fu
#define C_DIM    0x8C33u
#define C_OK     0x07E0u
#define C_ERR    0xF800u

extern uint32_t _sbss, _ebss, _estack;
extern const uint8_t bl_font[], bl_font_map[95];   /* boot/font.c: 8x16, only the glyphs bl.c uses */

struct bl_info {                          /* last 16 bytes of the bootloader area, filled by boot/mkinfo.py */
    uint32_t magic;                       /* "LBL1" */
    uint16_t ver, size;
    uint32_t crc;                         /* CRC32 of 0x08000000..0x080017EF */
    uint32_t nmagic;
};
__attribute__((section(".blinfo"), used))
const struct bl_info bl_info = { 0x314C424Cu, BL_VER_MAJ << 8 | BL_VER_MIN, 0, 0, ~0x314C424Cu };

/* ---------------------------------------------------------------- basics */

static void wdt(void) { IWDG_KR = 0xAAAA; }   /* the app may have started the IWDG; harmless otherwise */

static void pin_mode(uint32_t port, unsigned pin, unsigned mode, unsigned pull)
{
    GP_PULL(port) = (GP_PULL(port) & ~(3u << 2 * pin)) | pull << 2 * pin;
    GP_MODE(port) = (GP_MODE(port) & ~(3u << 2 * pin)) | mode << 2 * pin;
}

static void pin_set(uint32_t port, unsigned pin, int on) { GP_BSC(port) = on ? 1u << pin : 1u << (pin + 16); }
static int pin_low(uint32_t port, unsigned pin) { return !(GP_IN(port) >> pin & 1); }

/* ms clock: TIM6 counts at 1 kHz (update mode only), extended to 32 bit; called far more often than every 65 s */
static uint32_t now32;
static uint16_t last16;
static uint32_t ms(void)
{
    uint16_t c = (uint16_t)T6_CNT;
    now32 += (uint16_t)(c - last16);
    last16 = c;
    return now32;
}
static uint32_t since(uint32_t t) { return ms() - t; }

static void delay(uint32_t n)
{
    uint32_t t = ms();
    while (since(t) < n)
        wdt();
}

/* stock SystemInit steps (src/board.c clock_init, the same on the meter): PLL 48 MHz from HSI */
static void clock_init(void)
{
    RCC_CR |= 1;
    RCC_CFGR &= 0x21FF0000u;
    RCC_CR &= 0xFEF6FFFFu;
    RCC_CR &= ~(1u << 18);
    RCC_CFGR &= 0xFE00FFFFu;
    RCC_CFGR2 = 7u << 11;
    RCC_CIR = 0xBFu << 16;
    FLASH_ACR |= 0x10;
    for (unsigned n = 0; !(RCC_CR & 2) && n < 0x500; n++)
        ;
    if (!(RCC_CR & 2))
        return;
    FLASH_ACR = (FLASH_ACR & ~3u) | 2;
    RCC_CFGR &= ~(3u << 22);
    RCC_CFGR &= 0xFEC0FFFFu;
    RCC_CFGR |= 3u << 16;
    RCC_CR |= 1u << 24;
    for (unsigned n = 0; !(RCC_CR & (1u << 25)); n++)
        if (n > 100000)
            return;
    RCC_CFGR = (RCC_CFGR & ~3u) | 2;
    for (unsigned n = 0; (RCC_CFGR & 0xC008u) != 0x4000u; n++)
        if (n > 100000)
            break;
    RCC_CFGR &= ~0xF0u;                           /* AHB /1 */
    RCC_CFGR &= ~(7u << 11);                      /* APB2 /1 */
}

static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n)   /* zlib.crc32 compatible */
{
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

/* ---------------------------------------------------------------- LCD (as src/lcd.c) */

static void spi_idle(void)
{
    for (unsigned n = 0; n < 10000 && (SPI_STS & 0x82) != 0x02; n++)
        ;
}

static void spi_byte(uint8_t b)
{
    while (!(SPI_STS & 2))
        ;
    SPI_DAT = b;
}

static void lcd_cmd(uint8_t c)
{
    spi_idle();
    pin_set(PIN_LCD_DC, 0);
    pin_set(PIN_LCD_CS, 0);
    spi_byte(c);
    spi_idle();
    pin_set(PIN_LCD_CS, 1);
    pin_set(PIN_LCD_DC, 1);
}

static void lcd_data(const uint8_t *d, unsigned n)
{
    pin_set(PIN_LCD_CS, 0);
    while (n--)
        spi_byte(*d++);
    spi_idle();
    pin_set(PIN_LCD_CS, 1);
}

static const uint8_t lcd_seq[] = {
    0xB1, 3, 0x00, 0x2C, 0x2B,
    0xB2, 3, 0x00, 0x01, 0x01,
    0xB3, 6, 0x00, 0x01, 0x01, 0x00, 0x01, 0x01,
    0xB4, 1, 0x03,
    0xB9, 3, 0xFF, 0x83, 0x47,
    0xC0, 3, 0xA2, 0x02, 0x84,
    0xC1, 1, 0x02,
    0xC2, 2, 0x0A, 0x00,
    0xC3, 2, 0x8A, 0x2A,
    0xC4, 2, 0x8A, 0xEE,
    0xC5, 1, 0x09,
    0x20, 0,
    0xC7, 1, 0x10,
    0x36, 1, 0x08,
    0xE0, 16, 0x0C, 0x1C, 0x1B, 0x1A, 0x2F, 0x28, 0x20, 0x24, 0x23, 0x22, 0x2A, 0x36, 0x00, 0x05, 0x00, 0x10,
    0xE1, 16, 0x0C, 0x1A, 0x1A, 0x1A, 0x2E, 0x27, 0x21, 0x24, 0x24, 0x22, 0x2A, 0x35, 0x00, 0x05, 0x00, 0x10,
    0x35, 1, 0x00,
    0x3A, 1, 0x05,
    0x29, 0,
    0x36, 1, 0x68,
    0xFF,
};

static void lcd_init(void)
{
    RCC_APB2EN |= APB2_AFIO | APB2_SPI;
    GP_AFH(GPIOB) &= ~0xF0F00000u;                 /* PB13, PB15 AF0 = SPI */
    pin_mode(GPIOB, 13, MODE_AF, 0);
    pin_mode(GPIOB, 15, MODE_AF, 0);
    pin_set(PIN_LCD_CS, 1);
    pin_set(PIN_LCD_DC, 1);
    pin_set(PIN_LCD_RST, 1);
    pin_mode(PIN_LCD_CS, MODE_OUT, 0);
    pin_mode(PIN_LCD_DC, MODE_OUT, 0);
    pin_mode(PIN_LCD_RST, MODE_OUT, 0);
    pin_set(GPIOB, 9, 0);                          /* backlight off until the screen is cleared */
    pin_mode(GPIOB, 9, MODE_OUT, 0);
    SPI_CR1 = 0xC317;
    SPI_CRCPOLY = 7;
    SPI_I2SCFG = 0;
    SPI_CR1 = 0xC357;
    delay(1);
    pin_set(PIN_LCD_RST, 0);
    delay(10);
    pin_set(PIN_LCD_RST, 1);
    delay(120);
    lcd_cmd(0x11);
    delay(120);
    for (const uint8_t *p = lcd_seq; *p != 0xFF; p += 2 + p[1]) {
        lcd_cmd(p[0]);
        if (p[1])
            lcd_data(p + 2, p[1]);
    }
}

static void lcd_window(int x, int y, int w, int h)
{
    uint8_t a[4] = { 0, (uint8_t)(x + 1), 0, (uint8_t)(x + w) };   /* panel offsets 1, 2 */
    lcd_cmd(0x2A);
    lcd_data(a, 4);
    a[1] = (uint8_t)(y + 2), a[3] = (uint8_t)(y + h + 1);
    lcd_cmd(0x2B);
    lcd_data(a, 4);
    lcd_cmd(0x2C);
    pin_set(PIN_LCD_CS, 0);
}

static void lcd_px(uint16_t c)
{
    spi_byte(c >> 8);
    spi_byte(c);
}

static void lcd_end(void)
{
    spi_idle();
    pin_set(PIN_LCD_CS, 1);
}

static void fill(int x, int y, int w, int h, uint16_t c)
{
    lcd_window(x, y, w, h);
    for (int n = w * h; n; n--)
        lcd_px(c);
    lcd_end();
}

/* one text line of 16 characters at row y, padded with the background */
static void line(int y, const char *s, uint16_t fg, uint16_t bg)
{
    lcd_window(0, y, 128, 16);
    for (int r = 0; r < 16; r++) {
        const char *p = s;
        for (int col = 0; col < 16; col++) {
            unsigned ch = *p ? (uint8_t)*p++ : ' ', g = ch >= 32 && ch <= 126 ? bl_font_map[ch - 32] : 0xFF;
            uint8_t bits = g == 0xFF ? 0 : bl_font[g * 16 + r];
            for (int b = 0; b < 8; b++, bits <<= 1)
                lcd_px(bits & 0x80 ? fg : bg);
        }
        wdt();
    }
    lcd_end();
}

static char *hex(char *p, uint32_t v, int digits)
{
    while (digits--)
        *p++ = "0123456789ABCDEF"[v >> 4 * digits & 15];
    *p = 0;
    return p;
}

static char *cat(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

/* rows: 0 title, 20 state, 40 detail, 62 progress bar, 84 result, 110 hint */
static void status(const char *s, uint16_t c) { line(20, s, c, C_BG); }
static void detail(const char *s) { line(40, s, C_DIM, C_BG); }
static void result(const char *s, uint16_t c) { line(84, s, c, C_BG); }

static uint32_t bar_done;
static void bar(uint32_t done, uint32_t total)
{
    uint32_t w = total ? (done >= total ? 124 : done * 124 / total) : 0;
    if (w == bar_done && done)
        return;
    if (w < bar_done || !done)
        fill(2, 64, 124, 8, 0x2104), bar_done = 0;
    fill(2 + (int)bar_done, 64, (int)(w - bar_done), 8, C_ACCENT);
    bar_done = w;
}

static void screen(const char *why)
{
    fill(0, 0, 128, 128, C_BG);
    line(0, " LCR-P1 BOOT " BL_VERSION, C_BG, C_ACCENT);
    status("Update mode", C_FG);
    detail(why);
    bar(0, 1);
    line(110, "TEST 1s: off", C_DIM, C_BG);
    pin_set(GPIOB, 9, 1);                         /* backlight on */
}

/* ---------------------------------------------------------------- flash */

static int fl_wait(void)
{
    for (uint32_t n = 0; n < 2000000u; n++)
        if (!(FL_STS & 1))
            return !(FL_STS & 0x14);
    return 0;
}

/* erase + program + verify one 512-byte page; never below the app */
static int page_write(uint32_t addr, const uint32_t *d)
{
    if (addr < APP_BASE || addr >= FLASH_END || (addr & (PAGE - 1)))
        return 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        int ok = 1;
        wdt();
        FL_KEY = 0x45670123u;
        FL_KEY = 0xCDEF89ABu;
        FL_STS = 0x34;
        FL_CTRL = 2;                              /* page erase */
        FL_ADD = addr;
        FL_CTRL = 0x42;
        ok = fl_wait();
        FL_CTRL = 0;
        for (unsigned i = 0; ok && d && i < PAGE / 4; i++) {
            if (d[i] == 0xFFFFFFFFu)
                continue;
            FL_STS = 0x34;
            FL_CTRL = 1;                          /* program */
            ((volatile uint32_t *)addr)[i] = d[i];
            ok = fl_wait();
            FL_CTRL = 0;
        }
        FL_CTRL = 0x80;                           /* lock */
        for (unsigned i = 0; ok && i < PAGE / 4; i++)
            if (((volatile uint32_t *)addr)[i] != (d ? d[i] : 0xFFFFFFFFu))
                ok = 0;
        if (ok)
            return 1;
    }
    return 0;
}

static int app_valid(void)
{
    uint32_t sp = R32(APP_BASE), pc = R32(APP_BASE + 4);
    return sp > 0x20000100u && sp <= 0x20002000u && !(sp & 3) &&     /* 8 KB RAM (stock rule: < 0x20002000) */
           (pc & 1) && pc > APP_BASE + 8 && pc < FLASH_END;
}

/* ---------------------------------------------------------------- UART + DMA ring */

#define RX_SIZE 4096u
static uint8_t rx[RX_SIZE] __attribute__((aligned(4)));
static uint32_t rx_rd, last_rx;

static void uart_baud(uint32_t brr)
{
    UART_CR1 = 0;
    UART_BRR = brr;
    UART_CR1 = 0x200C;
}

static void uart_init(void)
{
    RCC_AHBEN |= 1;                               /* DMA */
    RCC_APB2EN |= APB2_UART1;
    GP_AFH(GPIOA) = (GP_AFH(GPIOA) & ~0xFF0u) | 0x440u;   /* PA9, PA10 AF4 */
    pin_mode(GPIOA, 9, MODE_AF, 0);
    pin_mode(GPIOA, 10, MODE_AF, PULL_UP);
    DMA_CFG = 0;
    DMA_CNT = RX_SIZE;
    DMA_PADDR = UART + 4;
    DMA_MADDR = (uint32_t)rx;
    DMA_SEL = 2;
    DMA_CFG = 0x30A1;                             /* very high priority, memory increment, circular, on */
    UART_CR2 = 0;
    UART_CR3 = 0x40;                              /* receive by DMA */
    uart_baud(417);                               /* 48 MHz / 115200 */
    rx_rd = 0;
}

static uint32_t rx_avail(void) { return (RX_SIZE - DMA_CNT - rx_rd) & (RX_SIZE - 1); }

static void tx(const uint8_t *p, unsigned n)
{
    while (n--) {
        while (!(UART_STS & 0x80))
            ;
        UART_DAT = *p++;
    }
    for (unsigned k = 0; !(UART_STS & 0x40) && k < 1000000; k++)   /* transmission complete */
        ;
}

/* ---------------------------------------------------------------- keys, power */

static uint8_t armed;                             /* TEST released since power-on */
static uint32_t t_test;

static void power_off(void)
{
    pin_set(GPIOB, 9, 0);
    pin_set(PIN_POWER, 0);
    uint32_t t = ms();
    for (;;) {                                    /* power fades once TEST is released */
        wdt();
        if (since(t) > 3000 && pin_low(PIN_KEY_TEST))
            AIRCR = 0x05FA0004u;                  /* still running (held up?): restart */
    }
}

/* called from every wait: TEST held 1 s or 5 min without serial input = off */
static void housekeeping(void)
{
    wdt();
    if (!pin_low(PIN_KEY_TEST))
        armed = 1, t_test = ms();
    else if (armed && since(t_test) >= 1000)
        power_off();
    if (rx_avail())
        last_rx = ms();
    else if (since(last_rx) >= IDLE_OFF_MS)
        power_off();
}

/* next received byte, -1 after `timeout` ms */
static int getb(uint32_t timeout)
{
    uint32_t t = ms();
    while (!rx_avail()) {
        housekeeping();
        if (since(t) >= timeout)
            return -1;
    }
    int c = rx[rx_rd];
    rx_rd = (rx_rd + 1) & (RX_SIZE - 1);
    return c;
}

/* ---------------------------------------------------------------- start the app */

/* MSP = sp, then the core registers as the stock loader leaves them (the stock app pushes r4-r7 early and later
   uses that stack memory uninitialised, e.g. as TIM1's repetition counter: test/bl_test.py compares traces) */
__attribute__((naked, noreturn)) static void jump(__attribute__((unused)) uint32_t sp, __attribute__((unused)) uint32_t pc)
{
    __asm volatile(
        "msr msp, r0\n mov r3, r0\n mov r0, r1\n mov r1, r3\n"
        "ldr r2, =0xC000\n mov r12, r2\n ldr r2, =0x080001F3\n mov lr, r2\n"
        "ldr r2, =0x40021000\n movs r3, #0\n ldr r4, =0x08001800\n movs r5, #1\n ldr r6, =0x080016F8\n"
        "movs r7, #0\n cpsie i\n bx r0\n .ltorg");      /* apps expect PRIMASK clear */
}

static void start_app(void)
{
    uint32_t sp = R32(APP_BASE), pc = R32(APP_BASE + 4);
    /* undo update mode (no-ops on the direct path) */
    UART_CR1 = 0, UART_CR3 = 0;
    DMA_CFG = 0;
    SPI_CR1 = 0;
    T6_CR1 = 0, T6_PSC = 0, T6_ARR = 0xFFFF, T6_CNT = 0, T6_SR = 0;
    RCC_APB1EN &= ~APB1_TIM6;
    RCC_APB2EN &= ~APB2_SPI;
    /* as the stock loader */
    REMAP |= APP_BASE;
    REMAP |= 0x80000000u;
    RCC_APB2EN &= ~(APB2_GPIOB | APB2_GPIOF | APB2_GPIOA | APB2_UART1);
    RCC_AHBEN &= ~1u;
    NVIC_ICER = 0x800;
    NVIC_ICPR = 0x800;
    jump(sp, pc);
}

/* ---------------------------------------------------------------- update protocols */

static uint32_t page_buf[PAGE / 4];               /* the app's vector page, written last */
static uint8_t page_pending;
static uint32_t total, written;                   /* for the progress bar */
static uint32_t frame[(8 + 4 + PAGE + 4) / 4];    /* frame payload / legacy block */

enum { ST_OK, ST_CRC, ST_CMD, ST_ARG, ST_FLASH, ST_VERIFY, ST_INVALID };
enum { C_INFO = 1, C_READ, C_CRC, C_WRITE = 5, C_COMMIT, C_BOOT, C_OFF, C_BAUD, C_BEGIN };

static uint8_t entry_why;                         /* 0 ZENER, 1 app request, 2 no valid app */

static void reply(uint8_t cmd, uint8_t st, const void *d, unsigned n)
{
    uint8_t h[5] = { 0x5A, cmd, st, (uint8_t)n, (uint8_t)(n >> 8) };
    uint32_t c = crc32(crc32(0, h + 1, 4), d, n);
    tx(h, 5);
    tx(d, n);
    tx((const uint8_t *)&c, 4);
}

/* INFO reply; also the "hello" after a FNIRSI header when the host stays quiet (see legacy()) */
static void send_info(uint8_t cmd)
{
    struct __attribute__((packed)) {
        char id[6];
        uint8_t maj, min;
        uint32_t app, end;
        uint16_t page, max;
        uint32_t bl_crc;
        uint8_t valid, why, pending;
    } i = { { 'L', 'C', 'R', 'P', 'B', 'L' }, BL_VER_MAJ, BL_VER_MIN, APP_BASE, FLASH_END, PAGE, PAGE,
            R32((uint32_t)&bl_info.crc),          /* stamped after linking: read it from flash */
            (uint8_t)app_valid(), entry_why, page_pending };
    reply(cmd, ST_OK, &i, sizeof i);
}

static void framed(void);

static void show_addr(const char *what, uint32_t a)
{
    char t[17], *p = cat(t, what);
    hex(p, a, 8);
    detail(t);
}

static int commit(void)
{
    if (!page_pending || !page_write(APP_BASE, page_buf) || !app_valid())
        return 0;
    page_pending = 0;
    return 1;
}

static void fail(const char *s)
{
    result(s, C_ERR);
    status("Update mode", C_FG);
}

/* FNIRSI stream: blocks of 512 to APP_BASE + 512 n until a block ends with LEGACY_END */
static void legacy(void)
{
    static const uint8_t ok[4] = { 0xAC, 0x55, 0x03, 0x04 };
    uint8_t err[4] = { 0xAC, 0x55, 0x04, 0 };
    /* FNIRSI hosts send block 0 within ~10 ms. A quiet host gets a hello and may continue with frames
       (0xA5 cannot start an image: it would be the low byte of a word-aligned SP). */
    int c = getb(500);
    if (c < 0) {
        send_info(0);
        c = getb(3000);
    }
    if (c == 0xA5) {
        framed();
        return;
    }
    status("Receiving...", C_ACCENT);
    result("", C_BG);
    page_pending = 0;
    for (uint32_t n = 0;; n++) {
        uint32_t addr = APP_BASE + n * PAGE;
        uint8_t *b = (uint8_t *)frame;
        for (unsigned i = 0; i < PAGE; i++) {
            if (n || i)
                c = getb(3000);
            if (c < 0) {
                fail(n ? "Timeout: no app" : "Timeout");
                return;                           /* the app page is erased: stays in update mode */
            }
            b[i] = (uint8_t)c;
        }
        int ok_page;
        if (addr >= FLASH_END)
            ok_page = 0;
        else if (n == 0) {
            memcpy(page_buf, frame, PAGE);
            page_pending = 1;
            ok_page = page_write(APP_BASE, 0);    /* invalidate the old app first */
        } else
            ok_page = page_write(addr, frame);
        if (!ok_page) {
            err[3] = 1;
            tx(err, 4);
            show_addr("ERR ", addr);
            fail("Write FAILED");
            return;
        }
        show_addr("", addr);
        bar(n + 1, 116);                          /* stock image size; just a rough bar */
        if (frame[PAGE / 4 - 1] == LEGACY_END) {
            if (!commit()) {
                err[3] = 2;
                tx(err, 4);
                fail("Not an app");
                return;
            }
            tx(ok, 4);
            bar(1, 1);
            result("OK", C_OK);
            delay(10);
            start_app();
        }
    }
}

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* A5 cmd len16 payload crc32(cmd..payload); timeouts between bytes drop the frame */
static void framed(void)
{
    uint8_t h[3], *d = (uint8_t *)frame;
    for (int i = 0; i < 3; i++) {
        int c = getb(200);
        if (c < 0)
            return;
        h[i] = (uint8_t)c;
    }
    unsigned n = h[1] | h[2] << 8;
    if (n > sizeof frame - 4)
        return;
    for (unsigned i = 0; i < n + 4; i++) {
        int c = getb(200);
        if (c < 0)
            return;
        d[i] = (uint8_t)c;
    }
    uint8_t cmd = h[0];
    if (crc32(crc32(0, h, 3), d, n) != u32(d + n)) {
        reply(cmd, ST_CRC, 0, 0);
        return;
    }
    uint32_t a = n >= 4 ? u32(d) : 0, b = n >= 8 ? u32(d + 4) : 0;
    switch (cmd) {
    case C_INFO:
        send_info(cmd);
        return;
    case C_READ:                                  /* addr, len16: any flash, also the bootloader (backup) */
        b &= 0xFFFF;
        if (n != 8 || b > PAGE || a < 0x08000000u || a + b > FLASH_END)
            break;
        reply(cmd, ST_OK, (const void *)a, b);
        return;
    case C_CRC:                                   /* addr, len */
        if (n != 8 || a < 0x08000000u || b > FLASH_END - a)
            break;
        b = crc32(0, (const uint8_t *)a, b);
        reply(cmd, ST_OK, &b, 4);
        return;
    case C_BEGIN:                                 /* total image length: invalidates the app */
        if (n != 4 || a > FLASH_END - APP_BASE)
            break;
        total = a, written = 0, page_pending = 0;
        status("Receiving...", C_ACCENT);
        result("", C_BG);
        bar(0, 1);
        reply(cmd, page_write(APP_BASE, 0) ? ST_OK : ST_FLASH, 0, 0);
        return;
    case C_WRITE:                                 /* addr + 512 bytes; the first app page waits for commit */
        if (n != 4 + PAGE || a < APP_BASE || a >= FLASH_END || (a & (PAGE - 1)))
            break;
        memmove(frame, d + 4, PAGE);
        if (a == APP_BASE) {
            memcpy(page_buf, frame, PAGE);
            page_pending = 1;
            reply(cmd, page_write(APP_BASE, 0) ? ST_OK : ST_FLASH, 0, 0);
        } else {
            show_addr("", a);
            reply(cmd, page_write(a, frame) ? ST_OK : ST_FLASH, 0, 0);
        }
        written += PAGE;
        bar(written, total);
        return;
    case C_COMMIT: {                              /* image length, CRC32 of the whole image */
        if (n != 8 || !page_pending || a < 8 || a > FLASH_END - APP_BASE)
            break;
        uint32_t c = crc32(0, (const uint8_t *)page_buf, a < PAGE ? a : PAGE);
        if (a > PAGE)
            c = crc32(c, (const uint8_t *)(APP_BASE + PAGE), a - PAGE);
        if (c != b) {
            reply(cmd, ST_VERIFY, &c, 4);
            fail("CRC mismatch");
            return;
        }
        if (!commit()) {
            reply(cmd, ST_INVALID, 0, 0);
            fail("Not an app");
            return;
        }
        bar(1, 1);
        result("OK", C_OK);
        status("Update mode", C_FG);
        detail("Firmware OK");
        reply(cmd, ST_OK, 0, 0);
        return;
    }
    case C_BOOT:
        if (!app_valid() || page_pending) {
            reply(cmd, ST_INVALID, 0, 0);
            return;
        }
        reply(cmd, ST_OK, 0, 0);
        start_app();
        return;
    case C_OFF:
        reply(cmd, ST_OK, 0, 0);
        power_off();
        return;
    case C_BAUD:                                  /* baud: reply at the old rate, back to 115200 if silent */
        if (n != 4 || a < 9600 || a > 1000000)
            break;
        reply(cmd, ST_OK, 0, 0);
        uart_baud((48000000u + a / 2) / a);
        for (uint32_t t = ms(); since(t) < 1000;) {
            housekeeping();
            if (rx_avail())
                return;
        }
        uart_baud(417);
        return;
    default:
        reply(cmd, ST_CMD, 0, 0);
        return;
    }
    reply(cmd, ST_ARG, 0, 0);
}

static void update_mode(void)
{
    static const char *const why[] = { "ZENER key", "Asked by app", "No valid app" };
    pin_mode(PIN_KEY_TEST, MODE_IN, PULL_UP);
    lcd_init();
    screen(why[entry_why]);
    uart_init();
    last_rx = ms();
    for (;;) {
        int c = getb(0xFFFF);
        if (c == 0xA5)
            framed();
        else if (c == 0xAC) {                     /* FNIRSI header AC 55 02 03 */
            uint8_t h[3];
            int k = 0;
            for (; k < 3; k++) {
                int x = getb(200);
                if (x < 0)
                    break;
                h[k] = (uint8_t)x;
            }
            if (k == 3 && h[0] == 0x55 && h[1] == 0x02 && h[2] == (uint8_t)(0xAC + 0x55 + 0x02))
                legacy();
        }
    }
}

/* ---------------------------------------------------------------- reset */

__attribute__((noreturn)) void Reset_Handler(void)
{
    /* power latch first: the user may already have released TEST */
    RCC_APB2EN |= APB2_GPIOA;
    pin_set(PIN_POWER, 1);
    pin_mode(PIN_POWER, MODE_OUT, PULL_UP);
    wdt();
    __asm volatile("cpsid i");
    NVIC_ICER = 0xFFFFFFFFu;                      /* entered from an app: its interrupts off */
    NVIC_ICPR = 0xFFFFFFFFu;
    SYST_CSR = 0;
    int asked = MAGIC[0] == MAGIC_REQ && MAGIC[1] == ~MAGIC_REQ;
    MAGIC[0] = 0, MAGIC[1] = 0;
    for (uint32_t *p = &_sbss; p < &_ebss;)
        *p++ = 0;
    clock_init();
    RCC_APB2EN |= APB2_GPIOA | APB2_GPIOB | APB2_GPIOC | APB2_GPIOF;
    /* keys: the registers end up exactly as the stock loader leaves them (test/bl_test.py compares), because
       the stock app read-modify-writes them. Its PF7 init also writes an uninitialised field of its init struct
       to GPIOF+0x08 (slew rate; the set bits are unused pins) and 0x80 to GPIOF+0x2C. */
    pin_mode(PIN_KEY_TEST, MODE_IN, PULL_UP);
    GP_BSC(GPIOC) = 1u << 13;
    pin_mode(PIN_KEY_ZENER, MODE_IN, PULL_UP);
    R32(GPIOF + 0x08) |= 0xB7C00u;
    R32(GPIOF + 0x2C) |= 0x80u;
    for (volatile unsigned n = 0; n < 12000; n++)  /* ~1-2 ms: pull-ups settle */
        ;
    if (pin_low(PIN_KEY_ZENER))
        entry_why = 0;
    else if (asked)
        entry_why = 1;
    else if (!app_valid())
        entry_why = 2;
    else
        start_app();
    RCC_APB1EN |= APB1_TIM6;                      /* update mode: TIM6 = 1 kHz counter, no interrupt */
    T6_CR1 = 0;
    T6_PSC = 47999;
    T6_ARR = 0xFFFF;
    T6_EGR = 1;
    T6_SR = 0;
    T6_CR1 = 1;
    last16 = (uint16_t)T6_CNT;
    update_mode();
    for (;;)
        ;
}

__attribute__((section(".vectors"), used))
const void *const vectors[16] = { &_estack, Reset_Handler, power_off, power_off };
