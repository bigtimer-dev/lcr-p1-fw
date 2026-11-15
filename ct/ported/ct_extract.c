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
