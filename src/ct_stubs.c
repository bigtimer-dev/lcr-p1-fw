/* SPDX-License-Identifier: EUPL-1.2
 * Temporary stand-ins for upstream modules not ported yet (cap.c). semi.c calls MeasureCap() for the
 * BJT B-E / MOSFET G-S capacitance; until cap.c is ported that value is reported as 0.
 * Remove each one when its module is ported.
 */
#include "config.h"
#include "common.h"
#include "variables.h"
#include "functions.h"

void MeasureCap(uint8_t Probe1, uint8_t Probe2, uint8_t ID)
{
    (void)Probe1, (void)Probe2;
    Caps[ID].Value = 0;
    Caps[ID].Scale = 0;
}
