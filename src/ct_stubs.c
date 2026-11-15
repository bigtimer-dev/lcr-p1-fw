/* SPDX-License-Identifier: EUPL-1.2
 * Temporary stand-ins for upstream modules not ported yet (semi.c). CheckProbes() calls these;
 * as no-ops they only mean "no semiconductor found", so resistors are still detected.
 * Remove each one when its module is ported.
 */
#include "config.h"
#include "common.h"
#include "variables.h"
#include "functions.h"

void CheckDiode(void) {}
void VerifyMOSFET(void) {}
void CheckTransistor(uint8_t BJT_Type, uint16_t U_Rl) { (void)BJT_Type; (void)U_Rl; }
void CheckDepletionModeFET(uint16_t U_Rl) { (void)U_Rl; }
uint8_t CheckThyristorTriac(void) { return 0; }
void CheckPUT(void) {}
void CheckUJT(void) {}
