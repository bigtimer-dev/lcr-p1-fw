/* SPDX-License-Identifier: EUPL-1.2
 * One probing cycle in the order of ComponentTester's main.c (cycle_start .. CheckAlternatives),
 * without its display code. Capacitor and semiconductor checks join as their modules are ported.
 */
#include "measure.h"
#include "config.h"
#include "common.h"
#include "variables.h"
#include "functions.h"
#include "uart.h"
#include "font.h"

void measure_init(void)
{
    memcpy(&NV, &NV_Adjust_1, sizeof NV);       /* adjustment profile #1 defaults (no NV store yet) */
    Cfg.Samples = ADC_SAMPLES;
    Cfg.AutoScale = 0;                          /* no bandgap switching on this ADC */
    Cfg.Ref = 1;
    Cfg.Vcc = UREF_VCC;
    Cfg.Bandgap = 1100;
    Cfg.OP_Mode = 0;
    ADC_DDR_set(1 << TP_REF);                   /* relay off: probes shorted (safe) */
}


/* main.c CheckVoltageRefs(), HW_REF25 part: ADC full scale from the 2.5 V reference */
static void check_refs(void)
{
    ct_adc_fs_mv = UREF_VCC;
    Cfg.Samples = 200;
    uint16_t u = ReadU(TP_REF);
    if (u > 2250 && u < 2750) {
        ct_adc_fs_mv = (uint16_t)((uint32_t)ct_adc_fs_mv * UREF_25 / u);
        Cfg.OP_Mode |= OP_EXT_REF;
    } else {
        Cfg.OP_Mode &= ~OP_EXT_REF;
    }
    Cfg.Vcc = ct_adc_fs_mv;
    Cfg.Samples = ADC_SAMPLES;
}

/* LCR-P1: the probe pins drive high to about 4.4 V, not to the ADC reference. Measure the drive level:
   pull each probe up via Rl with the other two floating (no DC path through any part: no current,
   so the probe sits at the drive level) and keep the highest reading as Cfg.Vcc. */
static void check_drive(void)
{
    static const uint8_t rl[3] = {1 << R_RL_1, 1 << R_RL_2, 1 << R_RL_3};
    uint16_t max = 0;
    ADC_DDR_set(0);
    ADC_PORT_set(0);
    for (uint8_t p = 0; p < 3; p++) {
        R_PORT_set(rl[p]);
        R_DDR_set(rl[p]);
        wait5ms();
        uint16_t u = ReadU(p);                  /* TP1..TP3 are channels 0..2 */
        if (u > max)
            max = u;
        R_DDR_set(0);
        R_PORT_set(0);
    }
    if (max > 3000 && max <= ct_adc_fs_mv)      /* plausible: use it */
        Cfg.Vcc = max;
}

static struct mres result;
static void describe(struct mres *r);

const struct mres *measure_result(void) { return &result; }

int measure_run(void)
{
    Check.Found = COMP_NONE;
    Check.Type = 0;
    Check.Done = DONE_NONE;
    Check.AltFound = COMP_NONE;
    Check.Diodes = 0;
    Check.Resistors = 0;
    Semi.Flags = 0;
    Semi.U_1 = Semi.U_2 = Semi.U_3 = 0;
    Semi.F_1 = 0;
    Semi.I_value = 0;
    AltSemi.U_1 = AltSemi.U_2 = 0;

    ADC_DDR_set(0);                             /* releases the probes (relay on) */
    MilliSleep(20);                             /* relay settles */
    check_refs();

    DischargeProbes();
    if (Check.Found != COMP_ERROR)
        check_drive();
    if (Check.Found != COMP_ERROR) {
        CheckProbes(PROBE_1, PROBE_2, PROBE_3);
        CheckProbes(PROBE_2, PROBE_1, PROBE_3);
        CheckProbes(PROBE_1, PROBE_3, PROBE_2);
        CheckProbes(PROBE_3, PROBE_1, PROBE_2);
        CheckProbes(PROBE_2, PROBE_3, PROBE_1);
        CheckProbes(PROBE_3, PROBE_2, PROBE_1);
        CheckAlternatives();
    }
    describe(&result);                          /* may measure more (diode leakage): before the relay */
    R_DDR_set(0);
    R_PORT_set(0);
    ADC_PORT_set(0);
    ADC_DDR_set(1 << TP_REF);                   /* probes shorted again */
    return Check.Found;
}

uint16_t measure_vcc(void) { return Cfg.Vcc; }
uint16_t measure_adc_fs(void) { return ct_adc_fs_mv; }
int measure_ref_ok(void) { return (Cfg.OP_Mode & OP_EXT_REF) != 0; }
int measure_resistors(void) { return Check.Found == COMP_RESISTOR ? Check.Resistors : 0; }

void measure_resistor(int i, uint8_t *a, uint8_t *b, uint32_t *value, int8_t *scale)
{
    uint8_t p = Resistors[i].A + 1, q = Resistors[i].B + 1;
    *a = p < q ? p : q;                         /* lower probe first */
    *b = p < q ? q : p;
    *value = Resistors[i].Value;
    *scale = Resistors[i].Scale;
}

void measure_format(char *out, int size, uint32_t value, int8_t scale, const char *unit)
{
    static const char pfx[] = "pnum kMG";               /* 10^-12 .. 10^9 */
    if (value == 0) {
        fmt(out, size, "0%s", unit);
        return;
    }
    while (value >= 10000)                              /* 4 significant digits */
        value = (value + 5) / 10, scale++;
    while (value < 1000)
        value *= 10, scale--;
    int e = scale + 3;                                  /* value = d.ddd * 10^e */
    int g = e >= 0 ? e / 3 : -((2 - e) / 3);            /* floor(e / 3) */
    if (g < -4) g = -4;
    if (g > 3) g = 3;
    int ip = e - 3 * g;                                 /* digits before the point - 1 */
    char d[6], *o = out;
    fmt(d, sizeof d, "%04u", (unsigned)value);
    for (int i = 0; i < 4 && o < out + size - 3; i++) {
        *o++ = d[i];
        if (i == ip && i < 3)
            *o++ = '.';
    }
    if (pfx[g + 4] != ' ' && o < out + size - 2)
        *o++ = pfx[g + 4];
    while (*unit && o < out + size - 1)
        *o++ = *unit++;
    *o = 0;
}

/* ---- result text (after upstream main.c Show_Diode / Show_BJT / Show_FET, condensed for 16 columns) ---- */

static char *add(struct mres *r)
{
    return r->n < MRES_LINES ? r->line[r->n++] : r->line[MRES_LINES - 1];
}

static void mv(char *o, int size, int32_t u)          /* "0.652V", "-2.10V" */
{
    const char *sign = u < 0 ? "-" : "";
    if (u < 0) u = -u;
    fmt(o, size, "%s%u.%03uV", sign, (unsigned)u / 1000, (unsigned)u % 1000);
}

static void pinout(struct mres *r)                    /* "1B 2C 3E" */
{
    char d[3] = {'?', '?', '?'};
    SemiPinDesignators();                               /* Semi.DesA..C for Check.Found (upstream main.c does this) */
    d[Semi.A] = Semi.DesA;
    d[Semi.B] = Semi.DesB;
    d[Semi.C] = Semi.DesC;
    fmt(add(r), 17, "1%c 2%c 3%c", d[0], d[1], d[2]);
}

static void value_line(struct mres *r, const char *label, uint32_t value, int8_t scale, const char *unit)
{
    char v[12];
    measure_format(v, sizeof v, value, scale, unit);
    fmt(add(r), 17, "%s %s", label, v);
}

static void diode_line(struct mres *r, const char *label, uint8_t a, uint8_t c)
{
    Diode_Type *d = SearchDiode(a, c);
    if (d) {
        char v[10];
        mv(v, sizeof v, d->V_f);
        fmt(add(r), 17, "%s%u>%u %s", label, a + 1, c + 1, v);
    }
}

static void describe_diodes(struct mres *r)
{
    fmt(r->title, sizeof r->title, Check.Diodes == 1 ? "Diode" : "Diodes x%u", Check.Diodes);
    for (uint8_t i = 0; i < Check.Diodes && i < MRES_LINES; i++) {
        char v[10];
        mv(v, sizeof v, Diodes[i].V_f);
        fmt(add(r), 17, "A%u K%u %s", Diodes[i].A + 1, Diodes[i].C + 1, v);
    }
    if (Check.Diodes == 1) {
        Diode_Type *d = &Diodes[0];
        if (d->V_f2 < 250) {                    /* Ge / Schottky: low-current V_f too */
            char v[10];
            mv(v, sizeof v, d->V_f2);
            fmt(add(r), 17, "@10uA %s", v);
        }
        UpdateProbes2(d->C, d->A);              /* reverse leakage (probes must still be free) */
        GetLeakageCurrent(1);
        if (CmpValue(Semi.I_value, Semi.I_scale, 10, -9) >= 0)
            value_line(r, "Ir", Semi.I_value, Semi.I_scale, "A");
    }
}

static void describe_bjt(struct mres *r)
{
    uint8_t npn = Check.Type & TYPE_NPN;
    fmt(r->title, sizeof r->title, "%s%s", npn ? "NPN" : "PNP", Check.Type & TYPE_PARASITIC ? " parasitic" : "");
    pinout(r);
    fmt(add(r), 17, "hFE %u", (unsigned)Semi.F_1);
    Diode_Type *d = npn ? SearchDiode(Semi.A, Semi.C) : SearchDiode(Semi.C, Semi.A);
    if (d) {                                    /* V_BE interpolated for the hFE class (upstream) */
        int16_t slope = ((int16_t)d->V_f - (int16_t)d->V_f2) / 3;
        int16_t vbe = Semi.F_1 < 100 ? (int16_t)d->V_f : Semi.F_1 < 250 ? (int16_t)d->V_f - slope
                                                                         : (int16_t)d->V_f2 + slope;
        if (d->V_f > 1000 && d->V_f2 < 700)
            vbe = d->V_f;
        char v[10];
        mv(v, sizeof v, vbe);
        fmt(add(r), 17, "Vbe %s", v);
    }
    if (CmpValue(Semi.I_value, Semi.I_scale, 10, -9) >= 0)
        value_line(r, "Iceo", Semi.I_value, Semi.I_scale, "A");
    if (npn)
        diode_line(r, "D ", Semi.C, Semi.B);   /* flyback diode C-E */
    else
        diode_line(r, "D ", Semi.B, Semi.C);
}

static void describe_fet(struct mres *r)
{
    uint8_t t = Check.Type;
    fmt(r->title, sizeof r->title, "%s%s %s", t & TYPE_N_CHANNEL ? "N-" : "P-",
        Check.Found == COMP_IGBT ? "IGBT" : t & TYPE_MOSFET ? "MOSFET" : "JFET",
        t & TYPE_ENHANCEMENT ? "enh" : t & TYPE_DEPLETION ? "dep" : "");
    pinout(r);
    if (t & TYPE_ENHANCEMENT) {
        if (Semi.U_2 != 0) {
            char v[10];
            mv(v, sizeof v, Semi.U_2);
            fmt(add(r), 17, "Vth %s", v);
        }
        if (Semi.C_value)
            value_line(r, "Cgs", Semi.C_value, Semi.C_scale, "F");
        if (Semi.U_1 > 0)
            value_line(r, "Rds", Semi.U_1, -2, CH_OHM);
    }
    if (t & TYPE_DEPLETION && CmpValue(Semi.I_value, Semi.I_scale, 1, -6) >= 0)
        value_line(r, "Idss", Semi.I_value, Semi.I_scale, "A");
    if (t & TYPE_N_CHANNEL)
        diode_line(r, "D ", Semi.C, Semi.B);
    else
        diode_line(r, "D ", Semi.B, Semi.C);
}

static void describe(struct mres *r)
{
    r->n = 0;
    r->title[0] = 0;
    switch (Check.Found) {
    case COMP_NONE:
        fmt(r->title, sizeof r->title, "no component");
        break;
    case COMP_ERROR:
        fmt(r->title, sizeof r->title, "error");
        break;
    case COMP_RESISTOR:
        fmt(r->title, sizeof r->title, Check.Resistors > 1 ? "Resistors" : "Resistor");
        break;
    case COMP_DIODE:
        describe_diodes(r);
        break;
    case COMP_BJT:
        describe_bjt(r);
        break;
    case COMP_FET:
    case COMP_IGBT:
        describe_fet(r);
        break;
    case COMP_THYRISTOR:
    case COMP_TRIAC: {
        fmt(r->title, sizeof r->title, Check.Found == COMP_TRIAC ? "Triac" : "Thyristor");
        pinout(r);
        char v[10];
        mv(v, sizeof v, Semi.U_1);
        fmt(add(r), 17, "Vgt %s", v);
        break;
    }
    case COMP_PUT:
    case COMP_UJT:
        fmt(r->title, sizeof r->title, Check.Found == COMP_PUT ? "PUT" : "UJT");
        break;
    default:
        fmt(r->title, sizeof r->title, "type %d", Check.Found);
        break;
    }
}
