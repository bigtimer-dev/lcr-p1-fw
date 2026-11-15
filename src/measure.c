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

/* main.c CheckVoltageRefs(), HW_REF25 part: Vcc from the 2.5 V reference */
static void check_refs(void)
{
    Cfg.Vcc = UREF_VCC;
    Cfg.Samples = 200;
    uint16_t u = ReadU(TP_REF);
    if (u > 2250 && u < 2750) {
        Cfg.Vcc = (uint16_t)((uint32_t)Cfg.Vcc * UREF_25 / u);
        Cfg.OP_Mode |= OP_EXT_REF;
    } else {
        Cfg.OP_Mode &= ~OP_EXT_REF;
    }
    Cfg.Samples = ADC_SAMPLES;
}

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
    if (Check.Found != COMP_ERROR) {
        CheckProbes(PROBE_1, PROBE_2, PROBE_3);
        CheckProbes(PROBE_2, PROBE_1, PROBE_3);
        CheckProbes(PROBE_1, PROBE_3, PROBE_2);
        CheckProbes(PROBE_3, PROBE_1, PROBE_2);
        CheckProbes(PROBE_2, PROBE_3, PROBE_1);
        CheckProbes(PROBE_3, PROBE_2, PROBE_1);
        CheckAlternatives();
    }
    R_DDR_set(0);
    R_PORT_set(0);
    ADC_PORT_set(0);
    ADC_DDR_set(1 << TP_REF);                   /* probes shorted again */
    return Check.Found;
}

uint16_t measure_vcc(void) { return Cfg.Vcc; }
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
