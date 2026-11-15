/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef IR_H
#define IR_H
#include <stdint.h>

struct ir_result {
    const char *name;           /* protocol, 0 = unknown */
    uint8_t code[8];            /* NEC/Samsung: address, ~address (or address high), command, ~command */
    uint8_t nbytes;             /* bytes of code[] that matter: 4, 8 (AC), 0 (repeat) */
    int edges;
};

int ir_active(void);                                    /* receiver sees a carrier now */
int ir_capture(const uint16_t **b);                     /* blocks until 50 ms quiet; returns entries */
int ir_decode(const uint16_t *b, int n, struct ir_result *r);   /* 1: known protocol */

#endif
