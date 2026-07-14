#ifndef COMP_SUBCARRIER_H
#define COMP_SUBCARRIER_H

enum {
    COMP_STD_PAL,
    COMP_STD_NTSC,
};

/* At 4xfsc the subcarrier advances exactly 1/4 cycle per sample, so a
 * line's phase is fully described by its start phase, which is an exact
 * rational: units of 1/COMP_SC_DEN_* of a cycle. */
#define COMP_SC_DEN_PAL  2500
#define COMP_SC_DEN_NTSC 720

typedef struct comp_sc_line_t comp_sc_line_t;

struct comp_sc_line_t {
    int phase;    /* phase at the line's first active sample, 1/den cycles */
    int vswitch;  /* PAL V-axis switch, +1 or -1; always +1 for NTSC */
};

/* row is 0-based within the active picture: 576 lines PAL, 486 NTSC */
comp_sc_line_t comp_sc_line(int standard, int frame, int row);

#endif
