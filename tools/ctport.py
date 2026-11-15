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
FILES = ['probes.c', 'resistor.c', 'semi.c', 'common.h', 'variables.h', 'functions.h', 'config_support.h',
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


# per-file patches for MCU specific code: (file, start marker, end marker incl., replacement)
PATCHES = [
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
     '      /* LCR-P1: the probe nodes hold about 2 nF (AVR: tens of pF), so the open base keeps the charge of the\n'
     '         previous step and the BJT conducts for many ms: start I_CEO with the base at emitter level */\n'
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
     '      for (Timeout = 0; Timeout < 50000; Timeout++)   /* about 1 s */\n'
     '      {\n'
     '        uint8_t High = ct_adc_read(Drain_Ch, 1) >= 2048;\n'
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
     '      CT_DBG("#C   n=%u t=%u Ug=%u\\r\\n", Counter, (unsigned)Timeout, (unsigned)(ct_adc_read(Probes.Ch_3, 1) * (uint32_t)ct_adc_fs_mv / 4096));\n'
     '      Done++;\n'
     '    }\n'
     '    Ugs = Done ? Ugs / Done : 0;     /* average */\n'
     '  }\n'),
]

# debug output (CT_DBG, active with make CT_DEBUG=1): (file, anchor line, line inserted after it).
# A line starting with '<' goes BEFORE the anchor (use it for anchors that are if/while headers:
# a statement inserted after such a header becomes its body and changes the program).
DEBUG = [
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
    for f, start, end, new in PATCHES:
        if f != name:
            continue
        a = text.find(start)
        assert a >= 0 and text.count(start) == 1, f'{f}: start marker not unique: {start!r}'
        b = text.find(end, a)
        assert b >= 0 and text.count(end) == 1, f'{f}: end marker not unique: {end!r}'
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
