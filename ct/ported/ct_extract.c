/* Converted from ComponentTester 1.57m (c) Markus Reschke, EUPL-1.2, by tools/ctport.py.
 * Do not edit: change ct/upstream, tools/ctport.py or the LCR-P1 layer instead. */
#include "config.h"
#include "common.h"
#include "variables.h"
#include "functions.h"

/* from user.c */
uint8_t NumberOfDigits(uint32_t Value)
{
  uint8_t           Counter = 1;   /* return value */

  while (Value >= 10)
  {
    Value /= 10;
    Counter++;
  }

  return Counter;
}

/* from user.c */
int8_t CmpValue(uint32_t Value1, int8_t Scale1, uint32_t Value2, int8_t Scale2)
{
  int8_t            Flag;               /* return value */
  int8_t            Len1, Len2;         /* length */

  /* determine virtual length */
  Len1 = NumberOfDigits(Value1) + Scale1;
  Len2 = NumberOfDigits(Value2) + Scale2;

  /* pre-process */
  if ((Value1 == 0) || (Value2 == 0))    /* special case */
  {
    Flag = 10;                /* perform direct comparison */
  }
  else if (Len1 > Len2)       /* more digits -> larger */
  {
    Flag = 1;                 /* first value is larger */
  }
  else if (Len1 == Len2)      /* same length */
  {
    /* re-scale to longer value */
    Len1 -= Scale1;           /* just number of digits */
    Len2 -= Scale2;           /* just number of digits */

    while (Len1 > Len2)       /* up-scale Value #2 */
    {
      Value2 *= 10;
      Len2++;
      /* Scale2-- */
    }

    while (Len2 > Len1)       /* up-scale Value #1 */
    {
      Value1 *= 10;
      Len1++;
      /* Scale1-- */
    }   

    Flag = 10;                /* perform direct comparison */
  }
  else                        /* less digits -> smaller */
  {
    Flag = -1;                /* first value is smaller */
  }

  /* direct comparison */
  if (Flag == 10)             /* requested direct comparison */
  {
    if (Value1 > Value2)
    {
      Flag = 1;               /* first value is larger */
    }
    else if (Value1 < Value2)
    {
      Flag = -1;              /* first value is smaller */
    }
    else
    {
      Flag = 0;               /* values are equal */
    }
  }

  return Flag;
}

/* from adjust.c */
uint8_t GetOffsetIndex(uint8_t Probe1, uint8_t Probe2)
{
  uint8_t           Index;

  /*
   *  - use sum of probe IDs (order doesn't matter):
   *    probes #1 & #2: 0 + 1 = 1  -> index: 0
   *    probes #1 & #3: 0 + 2 = 2  -> index: 1
   *    probes #2 & #3: 1 + 2 = 3  -> index: 2
   *  - index = (sum of probe IDs) - 1
   */

  Index = Probe1 + Probe2;
  Index--;

  return Index;
}

/* from user.c */
uint32_t RescaleValue(uint32_t Value, int8_t Scale, int8_t NewScale)
{
  uint32_t          NewValue;      /* return value */

  NewValue = Value;           /* take old value */

  while (Scale != NewScale)   /* processing loop */
  {
    if (NewScale > Scale)     /* upscale */
    {
      NewValue /= 10;
      Scale++;
    }
    else                      /* downscale */
    {
      NewValue *= 10;
      Scale--;
    }
  }

  return NewValue;
}
