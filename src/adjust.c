/* SPDX-License-Identifier: EUPL-1.2
 * Adjustment values (ComponentTester's NV / Adjust_Type) kept in flash, and the self-adjustment
 * after upstream adjust.c SelfAdjustment() (steps 2-6; no reference cap), with our own screen prompts.
 *
 * Storage: a log of 16-byte records in the 512-byte page at 0x0800F800. The linker keeps the app below
 * 0x0800F800 and 0x0800FC00.. (stock calibration record at 0x0800FFD0) is never touched, even if the
 * pages were 1 KB. A save writes the next free slot; the page is erased only when full (every 32 saves).
 * The last record with a valid magic and checksum wins.
 */
#include "config.h"
#include "common.h"
#include "variables.h"
#include "functions.h"
#include "adjust.h"
#include "measure.h"
extern volatile uint32_t g_ms;               /* board.h (its KEY_* names clash with common.h) */
void wdt_feed(void);
#define R32(a) (*(volatile uint32_t *)(a))
#include "uart.h"

#define NV_PAGE      0x0800F800u
#define NV_PAGE_SIZE 512u
#define NV_MAGIC     0x564E3150u                        /* "P1NV" */

struct rec {
    uint32_t magic;
    Adjust_Type nv;
};
#define NV_LEN __builtin_offsetof(Adjust_Type, CheckSum)   /* the values (compared and summed) */
_Static_assert(sizeof(Adjust_Type) == NV_LEN + 2, "Adjust_Type: values, CheckSum, one pad byte");
#define REC_SIZE  ((sizeof(struct rec) + 3u) & ~3u)
#define REC_SLOTS (NV_PAGE_SIZE / REC_SIZE)

/* flash controller (STM32F1 style) */
#define FL_KEY   R32(0x40022004u)
#define FL_STS   R32(0x4002200Cu)
#define FL_CTRL  R32(0x40022010u)
#define FL_ADD   R32(0x40022014u)
#define FL_PG    0x01u
#define FL_PER   0x02u
#define FL_START 0x40u
#define FL_LOCK  0x80u
#define FL_BUSY  0x01u
#define FL_ERR   0x14u                                  /* PGERR | WRPERR */
#define FL_EOP   0x20u

/* upstream CheckSum(): byte sum of the values before the checksum (upstream sums sizeof - 1 bytes:
   on the ARM the struct has a pad byte after CheckSum, so that would include the checksum itself) */
static uint8_t nv_sum(const Adjust_Type *a)
{
    const uint8_t *p = (const uint8_t *)a;
    uint8_t s = 0;
    for (unsigned n = 0; n < NV_LEN; n++)
        s += p[n];
    return s ? s : 1;
}

static const struct rec *slot(unsigned i)
{
    return (const struct rec *)(NV_PAGE + i * REC_SIZE);
}

static int slot_empty(unsigned i)
{
    const uint32_t *w = (const uint32_t *)slot(i);
    for (unsigned k = 0; k < REC_SIZE / 4; k++)
        if (w[k] != 0xFFFFFFFFu)
            return 0;
    return 1;
}

static const struct rec *last_valid(void)
{
    const struct rec *best = 0;
    for (unsigned i = 0; i < REC_SLOTS; i++) {
        const struct rec *r = slot(i);
        if (r->magic == NV_MAGIC && r->nv.CheckSum == nv_sum(&r->nv))
            best = r;
    }
    return best;
}

static int fl_wait(void)
{
    for (uint32_t n = 0; n < 2000000u; n++)
        if (!(FL_STS & FL_BUSY))
            return !(FL_STS & FL_ERR);
    return 0;
}

static int fl_erase(uint32_t page)
{
    FL_STS = FL_ERR | FL_EOP;
    FL_CTRL = FL_PER;
    FL_ADD = page;
    FL_CTRL = FL_PER | FL_START;
    int ok = fl_wait();
    FL_CTRL = 0;
    return ok;
}

static int fl_word(uint32_t addr, uint32_t w)
{
    FL_STS = FL_ERR | FL_EOP;
    FL_CTRL = FL_PG;
    *(volatile uint32_t *)addr = w;
    int ok = fl_wait();
    FL_CTRL = 0;
    return ok && *(volatile uint32_t *)addr == w;
}

/* settings in the byte after CheckSum (struct padding, 0xFF in records of v1.0/v1.1): auto-off minutes,
   0xFF = default. Not part of the checksum, compared on save like the values. */
#define PAD (((uint8_t *)&NV)[sizeof NV - 1])
static uint8_t off_min = 0xFF;

int nv_load(void)
{
    const struct rec *r = last_valid();
    if (!r)
        return 0;
    memcpy(&NV, &r->nv, sizeof NV);
    off_min = PAD;
    return 1;
}

unsigned nv_off_min(void)
{
    return off_min == 1 || off_min == 3 || off_min == 10 || off_min == 30 ? off_min : 3;
}

int nv_set_off_min(unsigned min)
{
    off_min = (uint8_t)min;
    return nv_save();
}

int nv_save(void)
{
    NV.CheckSum = nv_sum(&NV);
    PAD = off_min;                                      /* pad byte: the settings (0xFF = defaults) */
    const struct rec *r = last_valid();
    if (r && !memcmp(&r->nv, &NV, NV_LEN) && ((const uint8_t *)&r->nv)[sizeof NV - 1] == off_min)
        return 1;                                       /* unchanged */
    unsigned i = 0;
    while (i < REC_SLOTS && !slot_empty(i))
        i++;
    uint32_t buf[REC_SIZE / 4];
    memset(buf, 0xFF, sizeof buf);
    struct rec *n = (struct rec *)buf;
    n->magic = NV_MAGIC;
    memcpy(&n->nv, &NV, sizeof NV);
    FL_KEY = 0x45670123u;
    FL_KEY = 0xCDEF89ABu;
    int ok = 1;
    if (i == REC_SLOTS) {                               /* page full: start over */
        ok = fl_erase(NV_PAGE);
        i = 0;
    }
    for (unsigned k = 0; ok && k < REC_SIZE / 4; k++)
        ok = fl_word((uint32_t)slot(i) + 4 * k, buf[k]);
    FL_CTRL = FL_LOCK;
    wdt_feed();
    uprintf("#N save slot %u %s\r\n", i, ok ? "ok" : "FAILED");
    return ok;
}

/* CompOffset follows every 100 nF - 20 uF measurement (cap.c); store it when it moved by 3 mV or more,
   and anything else that changed */
int nv_autosave(void)
{
    const struct rec *r = last_valid();
    Adjust_Type a;
    memcpy(&a, r ? &r->nv : &NV_Adjust_1, sizeof a);
    int d = (int)NV.CompOffset - a.CompOffset;
    a.CompOffset = NV.CompOffset;
    if (!memcmp(&a, &NV, NV_LEN) && d > -3 && d < 3)
        return 1;
    return nv_save();
}

void nv_print(const char *tag)
{
    uprintf("#N %s RiL=%u RiH=%u R0=%u C0=%u CompOffset=%d off=%umin\r\n", tag, NV.RiL, NV.RiH, NV.RZero, NV.CapZero,
            NV.CompOffset, nv_off_min());
}


/* ---- self-adjustment ---- */

static const struct adjust_ui *ui;

static void clear_probes(void)
{
    ADC_DDR_set(0);                                     /* input mode; relay stays open */
    ADC_PORT_set(0);
    R_DDR_set(0);
    R_PORT_set(0);
}

/* after upstream ShortCircuit(): probes shorted (mode 1) or open (mode 0), confirmed with TEST, so the
   measurement starts only once the wire is clamped (or the hand is away); 0 = cancelled (ZENER) / timeout */
static int short_circuit(int mode)
{
    uint8_t want = mode ? 3 : 0;
    ui->say(mode ? "Short all probes" : "Remove the short", "then press TEST");
    uint32_t t0 = g_ms;
    while (g_ms - t0 < 180000u) {
        int k = ui->key();
        if (k == 2)
            return 0;
        if (k == 1) {
            uint8_t n = ShortedProbes();
            uprintf("#J TEST: shorted pairs %u (want %u)\r\n", n, want);
            if (n == want) {
                MilliSleep(200);
                return 1;
            }
            ui->say(mode ? "Not shorted" : "Still shorted", "fix, press TEST");
            t0 = g_ms;
        }
        MilliSleep(20);
    }
    return 0;
}

int adjust_run(const struct adjust_ui *u)
{
    ui = u;
    uint16_t rv[15], csum = 0, ril = 0, rih = 0;
    uint8_t rn = 0, cn = 0, riln = 0, rihn = 0;
    int done = 0;

    ADC_DDR_set(0);                                     /* relay open: probes free */
    MilliSleep(20);
    if (!measure_prepare()) {
        ui->say("Adjust: error", "probes charged?");
        return -1;
    }
    if (!short_circuit(1))
        goto out;

    ui->say("Adjusting", "R0 (shorted)");               /* step 2: resistance of probes in series */
    for (int run = 0; run < 5; run++) {
        static const uint8_t pair[3][2] = {{PROBE_2, PROBE_1}, {PROBE_3, PROBE_1}, {PROBE_3, PROBE_2}};
        for (int p = 0; p < 3; p++) {
            UpdateProbes2(pair[p][0], pair[p][1]);
            uint16_t r = SmallResistor(0);
            uprintf("#J R0 %u%u %u\r\n", pair[p][0] + 1, pair[p][1] + 1, r);
            if (r < 150)
                rv[rn++] = r;
        }
        wdt_feed();
    }
    clear_probes();
    if (!short_circuit(0))                              /* step 3 */
        goto out;

    ui->say("Adjusting", "Ri, C0 (open)");
    for (int run = 0; run < 5; run++) {
        for (int p = 0; p < 3; p++) {                   /* step 4: RiL, pin pulled down */
            ADC_PORT_set(0);
            ADC_DDR_set(1 << p);
            R_PORT_set(1 << (2 * p));
            R_DDR_set(1 << (2 * p));
            ril += ReadU_5ms(p), riln++;
        }
        clear_probes();
        for (int p = 0; p < 3; p++) {                   /* step 5: RiH, pin pulled up */
            R_PORT_set(0);
            ADC_PORT_set(1 << p);
            ADC_DDR_set(1 << p);
            R_DDR_set(1 << (2 * p));
            rih += Cfg.Vcc - ReadU_5ms(p), rihn++;
        }
        clear_probes();
        wdt_feed();
    }
    for (int run = 0; run < 5; run++) {                 /* step 6: C offset of the probe pairs */
        static const uint8_t pair[3][2] = {{PROBE_2, PROBE_1}, {PROBE_3, PROBE_1}, {PROBE_3, PROBE_2}};
        for (int p = 0; p < 3; p++) {
            Check.Found = COMP_NONE;
            MeasureCap(pair[p][0], pair[p][1], 0);
            uprintf("#J C0 %u%u scale=%d raw=%u\r\n", pair[p][0] + 1, pair[p][1] + 1, Caps[0].Scale,
                    (unsigned)Caps[0].Raw);
            if (Caps[0].Scale == -12 && Caps[0].Raw > 0 && Caps[0].Raw <= 100)   /* 0: nothing measured */
                csum += (uint16_t)Caps[0].Raw, cn++;
        }
        wdt_feed();
    }
    Check.Found = COMP_NONE;

    /* upstream: calculate values and offsets */
    if (cn >= 10)                                       /* most of the 15 readings valid */
        NV.CapZero = (uint8_t)(csum / cn), done++;
    if (rn == 15) {                                     /* median: a contact still settling gives outliers */
        for (int i = 1; i < 15; i++)
            for (int j = i; j > 0 && rv[j - 1] > rv[j]; j--) {
                uint16_t t = rv[j]; rv[j] = rv[j - 1]; rv[j - 1] = t;
            }
        NV.RZero = rv[7], done++;
    }
    if (riln == 15 && rihn == 15) {
        ril /= 5, rih /= 5;                             /* sums of 3 */
        uint32_t url = (uint32_t)Cfg.Vcc * 3 - ril - rih;   /* U_Rl * 3 */
        uint32_t v = ((uint32_t)R_LOW * 100 * ril / url + 5) / 10;
        if (v < 250)
            NV.RiL = (uint16_t)v, done++;
        v = ((uint32_t)R_LOW * 100 * rih / url + 5) / 10;
        if (v < 280)
            NV.RiH = (uint16_t)v, done++;
    }
out:
    clear_probes();
    ADC_DDR_set(1 << TP_REF);                           /* probes shorted by the relay again */
    uprintf("#J done %d (R0 %u/15, C0 %u/15, Ri %u/15)\r\n", done, rn, cn, riln);
    nv_print("adjusted");
    if (done)
        nv_save();
    return done;
}
