/* SPDX-License-Identifier: EUPL-1.2
 * IR remote decoding (mode switch on IR). The receiver on PA11 idles high and goes low while it sees the
 * 38 kHz carrier ("mark"). With nothing else to do in this mode the edges are polled against the 1 MHz
 * timer: a capture starts at the first low level and ends after 50 ms without a mark, so the 20/40 ms
 * gaps inside air-conditioner frames stay in one capture. Durations are stored like the stock firmware's
 * edge buffer: b[0] unused, b[1] = first mark, b[2] = space, ... Gaps over 12 ms split it into segments.
 * Decoders: NEC, Samsung32, Sony SIRC 12/15/20, RC5, Gree/Kelvinator AC (from the author's v1.1.0
 * stock-firmware patch in lcr-p1-linux, MIT); anything else goes to the serial port as raw timings.
 */
#include "ir.h"
#include "board.h"
#include "hw.h"

#define IR_MAX 760
#define GAP    12000                    /* us: a longer space starts a new segment */
#define END_US 50000                    /* us without a mark: capture done */

static uint16_t buf[IR_MAX];

static inline uint16_t us(void) { return (uint16_t)R32(0x40000824u); }     /* TIM3, 1 MHz */

int ir_active(void) { return !gpio_get(PIN_IR); }

int ir_capture(const uint16_t **b)
{
    int n = 1, lvl = 0;
    uint16_t t0 = us();
    buf[0] = 0;
    for (;;) {
        uint16_t now = us(), d = (uint16_t)(now - t0);
        int p = gpio_get(PIN_IR);
        if (p != lvl) {
            buf[n++] = d;
            t0 = now, lvl = p;
            if (n == IR_MAX)
                break;
        } else if (d > END_US) {        /* also ends a mark stuck low (receiver fault, sunlight) */
            if (!lvl)
                buf[n++] = d;
            break;
        }
        if (!(n & 63))
            wdt_feed();
    }
    wdt_feed();
    *b = buf;
    return n;
}

static int in(unsigned v, unsigned lo, unsigned hi) { return v - lo <= hi - lo; }

/* Pulse distance bits (NEC style), LSB first: bit k = mark b[i+2k], space b[i+2k+1]. */
static int pd_bits(const uint16_t *b, int i, int nbits, uint8_t *o)
{
    for (int k = 0; k < nbits; k++) {
        unsigned m = b[i + 2 * k], sp = b[i + 2 * k + 1];
        if (!in(m, 250, 900))
            return 0;
        if (in(sp, 1200, 2100))
            o[k >> 3] |= 1 << (k & 7);
        else if (!in(sp, 250, 900))
            return 0;
    }
    return 1;
}

/* NEC / Samsung32: leader mark b[s], leader space b[s+1], 32 bits from b[s+2]. */
static int pulse_distance(const uint16_t *b, int s, int n, uint8_t *o)
{
    return s + 65 <= n - 1 && in(b[s + 1], 3500, 5500) && pd_bits(b, s + 2, 32, o);
}

/* Gree / Kelvinator AC message from leader mark b[i]: 9000/4500, 35 bits (bytes 0-3 + "010"), mark +
   ~20 ms gap, 32 bits (bytes 4-7), mark = 139 entries. Checksum in the high nibble of byte 7. */
static int gree(const uint16_t *b, int i, int total, uint8_t *o)
{
    uint8_t x[5] = {0};
    if (i + 139 > total || !in(b[i], 7500, 10500) || !in(b[i + 1], 3500, 5500) || !in(b[i + 73], 15000, 25000))
        return 0;
    if (!pd_bits(b, i + 2, 35, x) || (x[4] & 7) != 2 || !pd_bits(b, i + 74, 32, o + 4))
        return 0;
    unsigned sum = 10;
    for (int k = 0; k < 4; k++) {
        o[k] = x[k];
        sum += (o[k] & 15) + (o[k + 4] >> 4);
    }
    sum -= o[7] >> 4;
    return (sum & 15) == (unsigned)(o[7] >> 4);
}

/* Sony SIRC: leader 2.4 ms, then per bit space 0.6 + mark 0.6 (0) / 1.2 ms (1), LSB first. */
static int sony(const uint16_t *b, int s, int n, uint32_t *v)
{
    int nb = (n - 1 - s) >> 1;
    if (((n - 1 - s) & 1) || !(nb == 12 || nb == 15 || nb == 20))
        return 0;
    uint32_t x = 0;
    for (int k = 0; k < nb; k++) {
        unsigned sp = b[s + 1 + 2 * k], m = b[s + 2 + 2 * k];
        if (!in(sp, 300, 1000))
            return 0;
        if (in(m, 900, 1600))
            x |= 1u << k;
        else if (!in(m, 300, 900))
            return 0;
    }
    *v = x;
    return nb;
}

/* RC5: 14 bits Manchester, 889 us halves, MSB first, 1 = space then mark. The first half (a space) and,
   for a final 0, the last half (a space) are not seen. */
static int rc5(const uint16_t *b, int n, uint32_t *v)
{
    uint8_t half[30];
    int h = 0;
    half[h++] = 0;
    for (int i = 1; i < n; i++) {
        int k = in(b[i], 600, 1200) ? 1 : in(b[i], 1300, 2100) ? 2 : 0;
        if (!k || h + k > 28)
            return 0;
        while (k--)
            half[h++] = i & 1;          /* odd entries are marks */
    }
    if (h == 27)
        half[h++] = 0;
    if (h != 28)
        return 0;
    uint32_t x = 0;
    for (int j = 0; j < 14; j++) {
        if (half[2 * j] == half[2 * j + 1])
            return 0;
        x = x << 1 | half[2 * j + 1];
    }
    *v = x;
    return x >> 13 & 1;                 /* start bit */
}

int ir_decode(const uint16_t *b, int total, struct ir_result *r)
{
    int n = 1;                          /* first segment = b[0 .. n-1] */
    while (n < total && b[n] <= GAP)
        n++;
    for (int k = 0; k < 8; k++)
        r->code[k] = 0;
    r->name = 0;
    r->nbytes = 4;
    r->edges = total - 1;
    uint32_t v;
    if (gree(b, 1, total, r->code)) {
        r->name = "AC Gree";
        r->nbytes = 8;
        return 1;
    }
    for (int k = 0; k < 8; k++)
        r->code[k] = 0;
    if (n <= 4 && in(b[1], 7500, 10500) && in(b[2], 1800, 2700)) {
        r->name = "NEC repeat";
        r->nbytes = 0;
        return 1;
    }
    for (int s = 1; s <= 2; s++) {      /* s = 2: the capture started with a short glitch */
        unsigned lead = b[s];
        if (n >= 60 && in(lead, 7500, 10500) && pulse_distance(b, s, n, r->code)) {
            r->name = "NEC";
            return 1;
        }
        if (n >= 60 && in(lead, 3500, 5500) && pulse_distance(b, s, n, r->code)) {
            r->name = "Samsung";
            return 1;
        }
        int nb;
        if (in(lead, 1900, 2900) && (nb = sony(b, s, n, &v)) != 0) {
            uint32_t addr = v >> 7;
            r->code[0] = addr >> 8, r->code[1] = addr, r->code[3] = v & 0x7F;
            r->name = nb == 12 ? "Sony12" : nb == 15 ? "Sony15" : "Sony20";
            return 1;
        }
    }
    for (int k = 0; k < 8; k++)
        r->code[k] = 0;
    if (rc5(b, n, &v)) {                /* address 5 bits, command 6 bits + inverted field bit as bit 6 */
        r->code[1] = v >> 6 & 31;
        r->code[3] = (v & 63) | (!(v >> 12 & 1)) << 6;
        r->code[2] = v >> 11 & 1;       /* toggle */
        r->name = "RC5";
        return 1;
    }
    return 0;
}
