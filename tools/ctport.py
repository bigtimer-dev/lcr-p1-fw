#!/usr/bin/env python3
"""Convert ComponentTester (m-firmware, EUPL-1.2) sources for the LCR-P1 build.

    python3 tools/ctport.py            # ct/upstream/*.c,h -> ct/ported/

- ISO-8859-1 -> UTF-8, CRLF -> LF
- AVR port register statements -> calls into the virtual ports of src/ct_hal.c:
      R_DDR = x;   -> R_DDR_set(x);
      R_PORT |= x; -> R_PORT_set(R_PORT_get() | (x));
      ADC_DDR &= x; -> ADC_DDR_set(ADC_DDR_get() & (x));
      ADC_PIN      -> ADC_PIN_get()
- config.h: upstream file with the LCR-P1 selections below (EDITS); every edit must match once.
Everything else is left as upstream wrote it. The output is committed; re-run after an upstream update.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UP = os.path.join(ROOT, 'ct', 'upstream')
OUT = os.path.join(ROOT, 'ct', 'ported')
FILES = ['probes.c', 'resistor.c', 'semi.c', 'cap.c', 'common.h', 'variables.h', 'functions.h', 'config_support.h',
         'colors.h'] + sorted(f for f in os.listdir(UP) if f.startswith('var_') and f.endswith('.h'))
PORT_STMT = re.compile(r'\b(R_DDR|R_PORT|ADC_DDR|ADC_PORT)\s*(\|=|&=|=(?!=))\s*([^;]*?)\s*;')
PIN_READ = re.compile(r'\bADC_PIN\b')

# config.h: (old, new) exact text replacements
EDITS = [
    ('//#define HW_REF25\n', '#define HW_REF25                          /* LCR-P1: PA0 */\n'),
    ('//#define HW_DISCHARGE_RELAY\n', '#define HW_DISCHARGE_RELAY                /* LCR-P1: relay on PB3 */\n'),
    ('#define SW_PWM_SIMPLE\n', '//#define SW_PWM_SIMPLE     /* LCR-P1: no OC1B */\n'),
    ('#define SW_SQUAREWAVE\n', '//#define SW_SQUAREWAVE     /* LCR-P1: no OC1B */\n'),
    ('#define SW_IR_RECEIVER\n', '//#define SW_IR_RECEIVER    /* LCR-P1: own IR receiver (later) */\n'),
    ('#define BAT_R1           10000\n', '#define BAT_R1           10000             /* LCR-P1: 1:1 divider */\n'),
    ('#define BAT_R2           3300\n', '#define BAT_R2           10000\n'),
    ('#define BAT_OFFSET       290\n', '#define BAT_OFFSET       0\n'),
    ('#define BAT_WEAK         7400\n', '#define BAT_WEAK         3500              /* LCR-P1: 1S Li-ion */\n'),
    ('#define BAT_LOW          6400 \n', '#define BAT_LOW          3300\n'),
    ('#define UREF_VCC         5001\n', '#define UREF_VCC         5020              /* LCR-P1: measured via 2.5V ref */\n'),
    ('#if defined(__AVR_ATmega328__)\n  #include "config_328.h"\n',
     '#if defined(LCRP1)\n  #include "config_lcrp1.h"\n#elif defined(__AVR_ATmega328__)\n  #include "config_328.h"\n'),
]


# per-file patches for MCU specific code: (file, start marker, end marker incl., replacement[, n]).
# The start marker must be unique, or with n given its n-th occurrence (0 = first) is used;
# the end marker is its first occurrence after the start.
PATCHES = [
    ('probes.c',
     '    else                                /* go for another round */\n',
     '      MilliSleep(50);                   /* wait for 50ms */\n    }\n',
     '    else if (!(Flags & (1 << ID)))      /* LCR-P1: wait only while this probe is still charged */\n'
     '    {                                   /* (upstream slept 50 ms after every probe that was already\n'
     '                                           discharged too: 50-100 ms per call, 10+ calls per test) */\n'
     '      wdt_reset();                      /* reset watchdog */\n'
     '      MilliSleep(U_c < 50 ? 10 : 50);   /* LCR-P1: 10 ms steps for the last few mV */\n    }\n', 0),
    ('cap.c',
     '    Value = (uint32_t)(NV.RiL * 10);    /* RiL in 0.01 Ohms */\n',
     '    U_1 = (uint16_t)Value;              /* raw ESR (0.01 Ohms) */\n',
     '    /* LCR-P1: exact divider Vcc - (Rl + RiH) - DUT - RiL - Gnd instead of RiL * U_DUT / U_RiL, which\n'
     '       assumes the pulse current does not depend on the DUT (a 2k in series read 504 Ohms):\n'
     '       R = (Rl + RiH) * (U2 / (Vcc - U2) - U1 / (Vcc - U1)) = (Rl + RiH) * Vs * dS / ((Vs - S2) (Vs - S1))\n'
     '       with sums S (Sum_2 is already S2 - S1) and Vs = Vcc as the same sum of 510 ADC codes */\n'
     '    {\n'
     '      uint64_t Vs = (uint64_t)Cfg.Vcc * 4096 * 510 / ct_adc_fs_mv;\n'
     '      uint64_t S2 = (uint64_t)Sum_2 + Sum_1, Rt = (uint64_t)R_LOW * 100 + NV.RiH * 10;  /* 0.01 Ohm */\n'
     '      if (S2 + 510 >= Vs) return ESR;    /* pulse at the rail: open or far too high */\n'
     '      uint64_t v = Rt * Vs * Sum_2 / ((Vs - S2) * (Vs - Sum_1));\n'
     '      if (v >= UINT16_MAX) return ESR;   /* above 655 Ohms: not an ESR */\n'
     '      U_1 = (uint16_t)v;                 /* raw ESR (0.01 Ohms) */\n'
     '    }\n'
     '    (void)Value;\n', 0),
    ('cap.c',
     'uint8_t SetUpDelayTimer(uint8_t Cycles)\n',
     '  TIFR0 = (1 << OCF0A);            /* clear flag */\n}\n',
     '/* LCR-P1: no Timer0 delays: ct_esr_sums() (src/ct_hal.c) times the pulses */\n'),
    ('cap.c',
     '  Probe1 = Probes.Ch_1;                 /* ADC MUX for probe-1 */\n',
     '  /* probes: reset to safe mode */\n',
     '  /* LCR-P1: the pulse loop below with GPIO mode writes, sample code 0 and a reverse pre-charge of the\n'
     '     pulse length (src/ct_hal.c ct_esr_sums()); same sums: Sum_1 = U_1 + U_3, Sum_2 = U_2 + U_4 */\n'
     '  (void)Probe1; (void)Probe2; (void)Bits; (void)U_3; (void)U_4;\n'
     '  if (!ct_esr_sums(Probes.ID_1, Probes.ID_2, &Sum_1, &Sum_2)) return ESR;\n'
     '  CT_DBG("#C ESR %u-%u Sum_1=%u Sum_2=%u RiL=%u R0=%u\\r\\n", Probes.ID_1 + 1, Probes.ID_2 + 1,\n'
     '         (unsigned)Sum_1, (unsigned)Sum_2, (unsigned)NV.RiL, (unsigned)NV.RZero);\n'
     '  n = 0;\n'
     '  /* probes: reset to safe mode */\n', 0),
    ('cap.c',
     '    U_Cap = ReadU(Probes.Ch_1);         /* get voltage */\n',
     '    U_Cap = ReadU(Probes.Ch_1);         /* get voltage */\n',
     '    /* LCR-P1: an open probe node (about 40 pF) loses its charge within a few ms (it settles at about\n'
     '       0.8 V), so the averaged ReadU() lands around the 1300 mV "small cap" limit and an empty socket\n'
     '       could give a value from one pulse. A fast sample right after the first pulse sees the real level;\n'
     '       a real cap reads the same either way. */\n'
     '    TempInt = (uint16_t)((uint32_t)ct_adc_poll(Probes.Ch_1) * ct_adc_fs_mv / 4096);\n'
     '    U_Cap = ReadU(Probes.Ch_1);         /* get voltage */\n'
     '    if ((Pulses == 1) && (TempInt > U_Cap)) U_Cap = TempInt;\n'),
    ('semi.c',
     '  if (U_1 < 10)             /* no conduction */\n',
     '  if (U_1 < 10)             /* no conduction */\n',
     '  /* LCR-P1: a large cap charged by the checks before (about 22 uF) passes the test above too: no current\n'
     '     once charged. A PUT must turn on again when the gate is pulled down. */\n'
     '  if (U_1 < 10)\n'
     '  {\n'
     '    R_PORT = 0;\n'
     '    R_DDR = Probes.Rl_2 | Probes.Rl_3;    /* pull down Gate and Cathode via Rl */\n'
     '    if (ReadU_5ms(Probes.Ch_2) < 977) U_1 = 10;    /* still no current: not a PUT */\n'
     '  }\n'
     '  if (U_1 < 10)             /* no conduction */\n'),
    ('cap.c',
     '  /* set up analog comparator */\n',
     '  ADCSRB &= ~(1 << ACME);     /* disable ADC multiplexer as negative input */\n',
     '  /* LCR-P1: COMP1 compares probe-1 with VREF1 (Cfg.Bandgap = VDD * CT_COMP_VREF / 64 mV) and TIM3\n'
     '     captures its output at 48 MHz (src/ct_hal.c); the timer start and the release of probe-1 are\n'
     '     two register writes apart */\n'
     '  R_PORT = Probes.Rh_1;                 /* pull-up probe-1 via Rh */\n'
     '  if (Check.Found == COMP_FET)          /* measuring C_GS */\n'
     '  {\n'
     '    /* keep all probe pins pulled down but probe-1 */\n'
     '    TempByte = ((1 << TP1) | (1 << TP2) | (1 << TP3)) & ~(Probes.Pin_1);\n'
     '  }\n'
     '  else                                  /* normal measurement */\n'
     '  {\n'
     '    TempByte = Probes.Pin_2;            /* keep just probe-2 pulled down */\n'
     '  }\n'
     '  {\n'
     '    uint32_t Count = ct_cap_charge(Probes.Ch_1, TempByte, CPU_FREQ / 5000);\n'
     '    Ticks = (uint16_t)Count;\n'
     '    Ticks2 = (uint16_t)(Count >> 16);\n'
     '  }\n'
     '  R_DDR = 0;                  /* set resistor port to HiZ mode */\n', 0),
    ('cap.c',
     '       while (ReadU(Probes.Ch_1) > 980)',
     '         Ticks += (int8_t)Value;             /* update local U_bandgap */\n       }\n',
     '       /* LCR-P1: no bandgap reference to adjust; the comparator threshold is VREF1 */\n'
     '       Ticks = Cfg.Bandgap;\n'
     '       (void)TempLong;\n'),
    ('resistor.c',
     '    /* set ADC to use bandgap reference and run a dummy conversion */\n',
     '    Value /= 1024;                 /* / 1024 for 10bit ADC */\n',
     '    /* LCR-P1: 12-bit ADC with VDD reference (1.22 mV/step, about the AVR bandgap resolution) */\n'
     '    while (Counter < 100)\n'
     '    {\n'
     '      Value += ct_adc_read(Probe, 1);   /* 12-bit reading */\n'
     '      wait400us();\n'
     '      Counter++;\n'
     '    }\n'
     '    Value *= Cfg.Vcc;              /* * U_ref (VDD) */\n'
     '    Value /= 4096;                 /* / 4096 for 12bit ADC */\n'),
    ('semi.c',
     '      GetLeakageCurrent(0);             /* get leakage current */\n',
     '      GetLeakageCurrent(0);             /* get leakage current */\n',
     '      /* LCR-P1: the open base keeps the charge of the previous step (pin leakage is low) and the BJT\n'
     '         conducts for many ms: start I_CEO with the base at emitter level */\n'
     '      PullProbe(Probes.Rl_3, PULL_10MS | (BJT_Type == TYPE_NPN ? PULL_DOWN : PULL_UP));\n'
     '      GetLeakageCurrent(0);             /* get leakage current */\n'),
    ('semi.c',
     '  ADMUX = Probes.Ch_3 | ADC_REF_VCC;    /* select probe-3 for ADC input */\n',
     '  Ugs /= 1024;                   /* using 10 bit resolution */\n',
     '  /* LCR-P1: the probe pins have no digital input (analog mode), so the drain is polled with\n'
     '     the ADC (threshold half of full scale, like ADC_PIN_get()). A missing transition ends the\n'
     '     loop after a timeout instead of waiting for the watchdog. Ugs is summed in mV. */\n'
     '  {\n'
     '    uint8_t Drain_Ch = (Type & TYPE_N_CHANNEL) ? Probes.Ch_1 : Probes.Ch_2;\n'
     '    uint8_t Done = 0;\n'
     '    (void)Drain_ADC;\n'
     '    CT_DBG("#C Vth type=%x Rl3=%x Rh3=%x DrainRl=%x Pull=%x Rport=%x\\r\\n", Type, Probes.Rl_3, Probes.Rh_3, Drain_Rl, PullMode, R_PORT_get());\n'
     '    for (Counter = 0; Counter < 10; Counter++)\n'
     '    {\n'
     '      uint32_t Timeout;\n'
     '      wdt_reset();                       /* reset watchdog */\n'
     '      PullProbe(Probes.Rl_3, PullMode);  /* discharge gate via Rl for 10 ms */\n'
     '      R_DDR = Drain_Rl | Probes.Rh_3;    /* pull up/down gate via Rh to slowly charge gate */\n'
     '      uint16_t T0 = ct_us();\n'
     '      (void)T0;\n'
     '      for (Timeout = 0; Timeout < 50000; Timeout++)   /* about 1 s */\n'
     '      {\n'
     '        uint8_t High = ct_adc_poll(Drain_Ch) >= 2048;\n'
     '        if ((Type & TYPE_N_CHANNEL) ? !High : High) break;\n'
     '      }\n'
     '      R_DDR = Drain_Rl;                  /* set probe-3 to HiZ mode */\n'
     '      if (Timeout >= 50000) break;       /* FET did not switch */\n'
     '      {\n'
     '        uint32_t U_g = ct_adc_read(Probes.Ch_3, 1) * (uint32_t)ct_adc_fs_mv / 4096;\n'
     '        if (Type & TYPE_N_CHANNEL)\n'
     '          Ugs += (int32_t)U_g;           /* Ugs = U_g */\n'
     '        else\n'
     '          Ugs += (int32_t)U_g - (int32_t)Cfg.Vcc;   /* Ugs = - (Vcc - U_g) */\n'
     '      }\n'
     '      CT_DBG("#C   n=%u t=%u us=%u Ug=%u\\r\\n", Counter, (unsigned)Timeout, (uint16_t)(ct_us() - T0), (unsigned)(ct_adc_read(Probes.Ch_3, 1) * (uint32_t)ct_adc_fs_mv / 4096));\n'
     '      Done++;\n'
     '    }\n'
     '    Ugs = Done ? Ugs / Done : 0;     /* average */\n'
     '  }\n'),
]

# debug output (CT_DBG, active with make CT_DEBUG=1): (file, anchor line, line inserted after it).
# A line starting with '<' goes BEFORE the anchor (use it for anchors that are if/while headers:
# a statement inserted after such a header becomes its body and changes the program).
DEBUG = [
    ('probes.c', '  if (U_Rl > 15)         /* > 21µA */\n',
     '<  CT_DBG("#C Probes %u-%u-%u U_Rl=%u\\r\\n", Probes.ID_1 + 1, Probes.ID_2 + 1, Probes.ID_3 + 1, U_Rl);\n'),
    ('cap.c', '  if (Ticks2 >= (CPU_FREQ / 5000)) Flag = 1;\n',
     '<  CT_DBG("#C SmallC %u-%u ovf=%u ticks=%u Uc=%u Vref=%u\\r\\n", Probes.ID_1 + 1, Probes.ID_2 + 1, Ticks2, Ticks, U_c, Cfg.Bandgap);\n'),
    ('cap.c', '      Offset = U_c - Ticks;\n',
     '      CT_DBG("#C   CompOffset %d\\r\\n", Offset);\n'),
    ('cap.c', '  if (U_Cap < 300)\n',
     '<  CT_DBG("#C LargeC %u-%u mode=%x pulses=%u U=%u Uz=%d\\r\\n", Probes.ID_1 + 1, Probes.ID_2 + 1, Mode, Pulses, U_Cap, U_Zero);\n'),
    ('resistor.c', '  U_Rh_L = ReadU_5ms(Probes.Ch_1);      /* get voltage at probe-1 */\n',
     '  CT_DBG("#C R %u-%u UriL=%u UrlH=%u UrhL0=%u\\r\\n", Probes.ID_1 + 1, Probes.ID_2 + 1, U_Ri_L, U_Rl_H, U_Rh_L);\n'),
    ('resistor.c', '    U_Rh_L = ReadU_5ms(Probes.Ch_2);    /* get voltage at Rh pulled down */\n',
     '    CT_DBG("#C   UrhH=%u UriH=%u UrlL=%u UrhL=%u Vcc=%u\\r\\n", U_Rh_H, U_Ri_H, U_Rl_L, U_Rh_L, Cfg.Vcc);\n'),
    ('resistor.c', '        if (Compare == 1)          /* valid measurement */\n',
     '<        CT_DBG("#C   Compare=%d Value=%u\\r\\n", Compare, Value);\n'),
    ('semi.c', '  if (U_1 < 10)             /* no conduction */\n',
     '<  CT_DBG("#C PUT? %u-%u-%u Uf=%u Vt=%u U_Rl=%u\\r\\n", Probes.ID_1 + 1, Probes.ID_2 + 1, Probes.ID_3 + 1, U_2, U_3, U_1);\n'),
]


def patch(name, text):
    for f, start, end, new, *nth in PATCHES:
        if f != name:
            continue
        if nth:
            a = -1
            for _ in range(nth[0] + 1):
                a = text.find(start, a + 1)
            assert a >= 0, f'{f}: start marker not found: {start!r}'
        else:
            a = text.find(start)
            assert a >= 0 and text.count(start) == 1, f'{f}: start marker not unique: {start!r}'
        b = text.find(end, a)
        assert b >= 0, f'{f}: end marker not found: {end!r}'
        text = text[:a] + new + text[b + len(end):]
    for f, anchor, line in DEBUG:
        if f == name:
            assert text.count(anchor) == 1, f'{f}: debug anchor not unique: {anchor!r}'
            assert not anchor.lstrip().startswith(('if ', 'if(', 'while', 'for ', 'else')) or line.startswith('<'), \
                f'{f}: debug line after a control header changes the code: {anchor!r}'
            text = text.replace(anchor, line[1:] + anchor if line.startswith('<') else anchor + line)
    return text


# single functions taken from upstream files whose remaining code is not used (yet)
EXTRACT = [('user.c', 'uint8_t NumberOfDigits('), ('user.c', 'int8_t CmpValue('),
           ('adjust.c', 'uint8_t GetOffsetIndex(')]


def extract():
    out = ['#include "config.h"', '#include "common.h"', '#include "variables.h"', '#include "functions.h"', '']
    for f, head in EXTRACT:
        text = read(f)
        a = text.find('\n' + head)
        assert a >= 0 and text.count('\n' + head) == 1, f'{f}: {head} not found once'
        b = text.find('\n}\n', a)
        out.append(f'/* from {f} */' + text[a:b + 3])
    return '\n'.join(out)


def read(name):
    return open(os.path.join(UP, name), 'rb').read().decode('latin-1').replace('\r\n', '\n')


def convert(text):
    def stmt(m):
        reg, op, expr = m.groups()
        if op == '=':
            return f'{reg}_set({expr});'
        return f'{reg}_set({reg}_get() {op[0]} ({expr}));'
    text, n = PORT_STMT.subn(stmt, text)
    text, k = PIN_READ.subn('ADC_PIN_get()', text)
    return text, n + k


def main():
    os.makedirs(OUT, exist_ok=True)
    banner = ('/* Converted from ComponentTester 1.57m (c) Markus Reschke, EUPL-1.2, by tools/ctport.py.\n'
              ' * Do not edit: change ct/upstream, tools/ctport.py or the LCR-P1 layer instead. */\n')
    for f in FILES:
        text, n = convert(patch(f, read(f)))
        open(os.path.join(OUT, f), 'w').write(banner + text)
        print(f'{f}: {n} port accesses converted')
    open(os.path.join(OUT, 'ct_extract.c'), 'w').write(banner + extract())
    print(f'ct_extract.c: {len(EXTRACT)} functions')
    cfg = read('config.h')
    for old, new in EDITS:
        assert cfg.count(old) == 1, f'config.h edit does not match exactly once: {old!r}'
        cfg = cfg.replace(old, new)
    open(os.path.join(OUT, 'config.h'), 'w').write(banner + cfg)
    print(f'config.h: {len(EDITS)} edits')


if __name__ == '__main__':
    sys.exit(main())
