/* Converted from ComponentTester 1.57m (c) Markus Reschke, EUPL-1.2, by tools/ctport.py.
 * Do not edit: change ct/upstream, tools/ctport.py or the LCR-P1 layer instead. */
/* ************************************************************************
 *
 *   inductor measurements
 *
 *   (c) 2012-2023 by Markus Reschke
 *   based on code from Karl-Heinz Kübbeler
 *
 * ************************************************************************ */

/* local includes */
#include "config.h"           /* global configuration */

#ifdef SW_INDUCTOR


/*
 *  local constants
 */

/* source management */
#define INDUCTOR_C


/*
 *  include header files
 */

/* local includes */
#include "common.h"           /* common header file */
#include "variables.h"        /* global variables */
#include "functions.h"        /* external functions */


/*
 *  local defines
 */

/* mode bitfield */
#define MODE_LOW_CURRENT      0b00000001     /* low test current */
#define MODE_HIGH_CURRENT     0b00000010     /* high test current */
#define MODE_DELAYED_START    0b00000100     /* delayed start */



/* ************************************************************************
 *   inductance measurements
 * ************************************************************************ */


/*

Current of an inductor when switching on power:

  i_L(t) = I_0 * (1 - e^(-t R_total / L))

  I_0: end current 

  i_L(t) / I_0 = 1 - e^(-t R_total / L)

  e^(-t R_total / L) = 1 - (i_L(t) / I_0))

With ln(e^x) = x we get:

  -t R_total / L = ln(1 - (i_L(t) / I_0))

  L = -t R_total / ln(1 - (i_L(t) / I_0))

So we can measure the current at a specific time after switching on to
get L.

  R_total = Ri_H + R_L + Rl + Ri_L

  I_0 = 5V / R_total = 5V / (Ri_H + R_L + Rl + Ri_L) 

We get i_L(t) by measuring the voltage accross a shunt resistor. For a proper
time measurement we'll use the integrated analog comparator and a timer, i.e.
we'll wait until the voltage across the shunt resistor reaches the voltage
of the bandgap reference.

  R_shunt = Rl + Ri_L

  i_L(t_stop) = U_ref / R_shunt

  L = -t_stop * R_total / ln(1 - ((U_ref / R_shunt) / (5V / R_total)))

    = -t_stop * R_total / ln(1 - (U_ref * R_total) / (5V * R_shunt))

Instead of calculating L directly we'll use a table with pre-calculated values
to speed up things and keep the firmware small. The table is based on the
the ratio:
  a = (U_ref * R_total) / (5V * R_shunt)

Estimates for minimal and maximal values (R_L max. 2k Ohm):
- min: 1.0V * 720 Ohm / 5V * 700 Ohm = 0.206
- max: 1.2V * 2770 Ohm / 5V * 700 Ohm = 0.950
  999 is maximum due to ln(1-a/1000)
  Hence the maximum R_L supported is 2k Ohms.

Table:
- ratio a = ((U_ref * R_total) / (5V * R_shunt)) * 10^3
  estimated range: 206 - 977
- values are: (-1 / ln(1 - (a * 10^-3))) * 10^3
  - internal scale factor 10^3 
- bc:
  - options: -i -l
  - define x (a) { return (-1 / l(1 - a/1000)) * 1000; }

For a small inductance we have to use a higher test current, i.e. using Ri_L as
current shunt (Rl = 0).

Estimates for minimal and maximal values (R_L max. 40 Ohm):
- min: 1.0V * 40 Ohm / 5V * 20 Ohm = 0.4
- max: 1.2V * 80 Ohm / 5V * 20 Ohm = 0.960
  999 is maximum due to ln(1-a/1000)
  Hence the maximum R_L supported is 40 Ohms.

Since the range overlaps with the low test current we may use a single table.

*/



/*
 *  measure inductance via time between two probe pins
 *  - probes have to be set by UpdateProbes()
 *
 *  requires:
 *  - pointer to time variable (ns)
 *  - measurement mode (low/high current, delayed start)
 *
 *  returns:
 *  - 3 on success
 *  - 2 if inductance is too low
 *  - 1 if inductance is too high (not implemented)
 *  - 0 on any problem
 */

uint8_t MeasureInductance(uint32_t *Time, uint8_t Mode)
{
  uint8_t           Flag = 3;      /* return value */
  uint8_t           Test;          /* test flag */
  uint8_t           Offset;        /* counter offet */
  uint16_t          Ticks_L;       /* timer counter */
  uint16_t          Ticks_H;       /* timer overflow counter */
  uint32_t          Counter;       /* counter */

  /* sanity check */
  if (Time == NULL) return 0;

  DischargeProbes();                    /* try to discharge probes */
  if (Check.Found == COMP_ERROR) return 0;


  /*
   *  measurement modes:
   *  - low current: Gnd -- Rl -- probe-2 / probe-1 -- Vcc
   *  - high current: Gnd -- probe-2 / probe-1 -- Vcc
   */


  /*
   *  init hardware
   */

  /* set probes: Gnd -- (Rl) -- probe-2 / Gnd -- probe-1 */
  R_PORT_set(0);                           /* set resistor port to low */
  ADC_PORT_set(0);                         /* set ADC port to low */

  if (Mode & MODE_LOW_CURRENT)     /* low current */
  {
    /* set probes: Gnd -- Rl -- probe-2 / Gnd -- probe-1 */
    R_DDR_set(Probes.Rl_2);                /* pull down probe-2 via Rl */
    ADC_DDR_set(Probes.Pin_1);             /* pull down probe-1 directly */
  }
  else                             /* high current */
  {
    /* set probes: Gnd -- probe-2 / Gnd -- probe-1 */
    R_DDR_set(0);                          /* disable probe resistors */
    /* pull down probe-1 and probe-2 directly */
    ADC_DDR_set(Probes.Pin_1 | Probes.Pin_2);
  }

  /* LCR-P1: COMP1 compares probe-2 with VREF1 (Cfg.Bandgap), TIM3 counts at 48 MHz from the switch-on
     of probe-1 to the crossing (src/ct_hal.c ct_ind_charge). Delayed start: the capture is armed 4 us
     after the switch-on (skips the capacitive spike of large inductors), the time counts from it. */
  wdt_reset();                          /* reset watchdog */
  Counter = ct_ind_charge(Probes.Ch_2, Probes.Pin_1, (Mode & MODE_DELAYED_START) ? 4 : 0, CPU_FREQ / 250000);
  (void)Test;
  /* cut off: Gnd -- Rl -- probe-2 / probe-1 -- Rl -- Gnd */
  R_DDR_set(Probes.Rl_2 | Probes.Rl_1);
  ADC_DDR_set(0);
  CT_DBG("#C L %u-%u mode=%u ticks=%u\r\n", Probes.ID_1 + 1, Probes.ID_2 + 1, Mode, (unsigned)Counter);
  if (Counter == UINT32_MAX) Flag = 0;  /* timeout */
  Ticks_L = (uint16_t)Counter;
  Ticks_H = (uint16_t)(Counter >> 16);
  if (Flag)                   /* got valid measurement */
  {
    Counter = (uint32_t)Ticks_L | (uint32_t)Ticks_H << 16;
    Offset = 10;              /* pin, comparator and capture delay: a 10 Ohm resistor reads 12 ticks */
    if (Counter > Offset) Counter -= Offset;
    else Counter = 0;
    Counter = (Counter * 125 + 3) / 6;    /* 48 MHz ticks to ns */
    *Time = Counter;               /* save time */

    /* check for lower limit */
    if (Mode & MODE_LOW_CURRENT)   /* low current mode */
    {
      if (Counter < 1250)          /* time < 1.25µs */
      {
        Flag = 2;                  /* signal "inductance too low" */
      }
    }
    else                           /* high current mode */
    {
      /* LCR-P1: a plain resistor trips the comparator after 230-310 ns here (pin, comparator and
         capture delays; measured: 10 Ohm 250 ns, a 36 uH coil 670 ns) */
      if (Counter <= 350)          /* time <= 350ns */
      {
        Flag = 2;                  /* signal "inductance too low" */
      }
    }
  }

  /* update reference source for next ADC run */
  Cfg.Ref = ADC_REF_BANDGAP;       /* we've used the bandgap reference */

  return Flag;
}



/*
 *  measure inductance between two probe pins of a resistor
 *
 *  requires:
 *  - pointer the resistor data structure
 *
 *  returns:
 *  - 1 on success
 *  - 0 on any error
 */

uint8_t MeasureInductor(Resistor_Type *Resistor)
{
  uint8_t           Test = 0;      /* return value / measurement result */
  uint8_t           Mode;          /* measurement mode */
  uint8_t           Scale;         /* scale of value */
  uint16_t          R_total;       /* total resistance */
  uint16_t          Factor;        /* factor */
  uint16_t          Temp;
  int16_t           Offset = 0;    /* offset for U_ref */
  uint32_t          Value;         /* value */
  uint32_t          Time1;         /* time #1 */

  /* reset data */
  Inductor.Scale = 0;
  Inductor.Value = 0;

  /* sanity check */
  if (Resistor == NULL) return Test;

  /* limit resistance to 2k (feasibilty & prevent variable overflow) */ 
  if (CmpValue(Resistor->Value, Resistor->Scale, 2000, 0) >= 0) return Test;


  /*
   *  Manage measurements:
   *  - run in delayed mode to deal with capacitive effects of large inductors
   *    (low current only)
   *  - run in low current mode to check for high inductance
   *  - run in high current mode to check for low inductance, when high
   *    current mode failed and DUT's resistance is low
   */

  UpdateProbes2(Resistor->A, Resistor->B);   /* update probes */

  /*
   *  low current & delayed start
   *  - to catch large inductance with capacitive effect
   */

  Mode = MODE_LOW_CURRENT | MODE_DELAYED_START;
  Test = MeasureInductance(&Time1, Mode);
  if (Test == 3)              /* valid measurement */
  {
    /* a valid time should be larger than the delay (4µs) */
    if (Time1 <= 5000)         /* <= 5µs */
    {
      Test = 0;                /* invalid time */
    }
  }


  /*
   *  low current & immediate start
   *  - to check for high inductance
   */

  if (Test == 0)               /* no valid measurement yet */
  {
    Mode = MODE_LOW_CURRENT;
    Test = MeasureInductance(&Time1, Mode);

    if (Test == 2)             /* inductance too low */
    {
      /*
       *  high current & immediate start
       *  - to check for low inductance
       */

      /* if resistance < 40 Ohms we may run the high current test */
      if (CmpValue(Resistor->Value, Resistor->Scale, 40, 0) < 0)
      {
        Mode = MODE_HIGH_CURRENT;
        Test = MeasureInductance(&Time1, Mode);
      }
    }
  }

  if (Test != 3) Test = 0;         /* all measurements failed */


  /*
   *  calculate inductance
   */

  if (Test == 3)                   /* valid measurement */
  {
    /*
     *  resistance
     */

    /* total resistance (in 0.1 Ohms) */
    R_total = RescaleValue(Resistor->Value, Resistor->Scale, -1);  /* R_L */

    #ifdef R_MULTIOFFSET
    uint8_t            n;
    /* get index number for probe pair */
    n = GetOffsetIndex(Probes.ID_1, Probes.ID_2);
    Factor = NV.RZero[n];          /* probe leads (0.01 Ohms) */
    #else
    Factor = NV.RZero;             /* probe leads (0.01 Ohms) */
    #endif

    Factor += 5;                   /* for rounding */
    Factor /= 10;                  /* scale to 0.1 Ohms */
    R_total += NV.RiH + NV.RiL + Factor;

    Factor = NV.RiL;          /* shunt resistance (in 0.1 Ohms) */

    if (Mode & MODE_LOW_CURRENT)   /* low current measurement mode */
    {
      /* add R_l */
      R_total += (R_LOW * 10);
      Factor += (R_LOW * 10);

      /*
       *  compensation offset for U_ref
       *  - positive/larger offset -> lower L
       *  - negative/lower offset -> higher L
       */

      #if CPU_FREQ == 8000000
      /* 8 MHz */
      if (Time1 < 6000)       /* < 6µs / < 18mH */
      {
        Offset = 38;
      }
      else if (Time1 < 9500) /* 6-9.5µs / 18-27mH */
      {
        Offset = -10;
      }
      else                    /* > 9.5µs / > 27mH */
      {
        Offset = 20;
      }
      #endif

      #if (CPU_FREQ == 16000000) || (CPU_FREQ == 20000000)
      /* 16 & 20 MHz */
      if (Time1 < 6000)       /* < 6µs / < 18mH */
      {
        Offset = 80;
      }
      else if (Time1 < 12500) /* 6-12.5µs / 18-33mH */
      {
        Offset = 45;
      }
      else                    /* > 12.5µs / > 33mH */
      {
        Offset = 55;
      }
      #endif
    }
    else                           /* high current measurement mode */
    {
      /*
       *  compensation offset for U_ref
       *  - positive/larger offset -> lower L
       *  - negative/lower offset -> higher L
       */

      Temp = (uint16_t)Time1;      /* time < 50µs expected */

      #if CPU_FREQ == 8000000
      /* 8 MHz */
      if (Temp < 1500)        /* < 1.5µs / < 100µH */
      {
        Offset = -10;
      }
      else if (Temp < 5000)   /* 1.5-5µs / 100-330µH */
      {
        Offset = -5; 
      }
      else                    /* > 5µs / > 330µH */
      {
        Offset = -50;
      }
      #endif

      #if CPU_FREQ == 16000000
      /* 16 MHz */
      if (Temp < 1500)        /* < 1.5µs / < 100µH */
      {
        Offset = 10;
      }
      else if (Temp < 5000)   /* 1.5-5µs / 100-330µH */
      {
        Offset = -5; 
      }
      else                    /* > 5µs / > 330µH */
      {
        Offset = -50;
      }
      #endif

      #if CPU_FREQ == 20000000
      /* 20 MHz */
      if (Temp < 1500)        /* < 1.5µs / < 100µH */
      {
        Offset = 10;
      }
      else if (Temp < 5000)   /* 1.5-5µs / 100-330µH */
      {
        Offset = -20; 
      }
      else                    /* > 5µs / > 330µH */
      {
        Offset = -70;
      }
      #endif
    }


    /*
     *  ratio and factor
     *  - ratio = ((U_ref * R_total) / (5V * R_shunt)) * 10^3
     */

    /* calculate ratio */
    Value = Cfg.Bandgap + NV.CompOffset;          /* = U_ref (in mV) */
    Value += Offset;                              /* +/- offset */
    /* LCR-P1: in high current mode the pin itself is the shunt; at about 60 mA it reaches U_ref
       sooner than RiL (measured at low current) says. Fitted on the meter against a DE-5000 at 10 kHz
       (10 tick offset): 44.49 uH reads -2.4 %, 31.01 uH +2.1 %, 2.016 mH (25 Ohm) +2.1 %. */
    if (!(Mode & MODE_LOW_CURRENT)) Value = Value * 177 / 200;
    Value *= R_total;                             /* * R_total (in 0.1 Ohms) */
    Value /= Factor;                              /* / R_shunt (in 0.1 Ohms) */
    Value /= 5;                                   /* / 5000mV, * 10^3 */

    /* get ratio based factor */
    Factor = GetFactor((uint16_t)Value, TABLE_INDUCTOR);


   /*
    *  calculate inductance
    *  L = t_stop * R_total * factor
    */

    Scale = -9;               /* nH by default */
    Value = Time1;            /* t_stop (in ns) */

    while (Value > 100000)    /* re-scale to prevent overflow */
    {
      Value += 5;             /* for automagic rounding */
      Value /= 10;            /* scale down by 10^1 */
      Scale++;                /* increase exponent by 1 */
    }

    Value *= Factor;          /* * factor (in 10^-3) */

    while (Value > 100000)    /* re-scale to prevent overflow */
    {
      Value += 5;             /* for automagic rounding */
      Value /= 10;            /* scale down by 10^1 */
      Scale++;                /* increase exponent by 1 */
    }

    Value *= R_total;         /* * R_total (in 0.1 Ohms) */
    Value /= 10000;           /* /10 for 1 Ohms, /1000 for factor */

    /* update data */
    Inductor.Scale = Scale;
    Inductor.Value = Value;
    Test = 1;                 /* signal success */
  }

  return Test;
}



/* ************************************************************************
 *   clean-up of local constants
 * ************************************************************************ */


/* local defines */
#undef MODE_LOW_CURRENT
#undef MODE_HIGH_CURRENT
#undef MODE_DELAYED_START


/* source management */
#undef INDUCTOR_C

#endif

/* ************************************************************************
 *   EOF
 * ************************************************************************ */
