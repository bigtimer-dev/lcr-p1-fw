/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef ADJUST_H
#define ADJUST_H

int nv_load(void);                  /* stored adjustment values -> NV; 0 = none stored (defaults stay) */
int nv_save(void);                  /* NV -> flash (no write when unchanged); 0 = flash error */
int nv_autosave(void);              /* after a test: save when CompOffset moved >= 3 mV or anything else changed */
void nv_print(const char *tag);     /* "#N ..." over serial */

struct adjust_ui {
    void (*say)(const char *line1, const char *line2);  /* prompt / progress */
    int (*key)(void);                                   /* 0 none, 1 TEST (continue), 2 ZENER (cancel) */
};
/* self-adjustment (upstream SelfAdjustment steps 2-6): R0 with the probes shorted, then Ri+/Ri- and C0
   with the probes open, each started with TEST; stores the result. Returns the number of values adjusted (0..4), -1 on error. */
int adjust_run(const struct adjust_ui *ui);

#endif
