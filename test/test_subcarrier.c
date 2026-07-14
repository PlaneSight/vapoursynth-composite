/*
 * Subcarrier phase: integer implementation vs a double-precision reference
 * following ld-chroma-encoder's encodeLine(), plus sequence properties.
 */

#include <math.h>
#include <stdio.h>

#include "subcarrier.h"

static int fail = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        fail = 1; \
    } \
} while (0)

/* subcarrier cycles at the first active sample, as ld-chroma-encoder
 * computes them (palencoder.cpp/ntscencoder.cpp encodeLine) */
static double ref_cycles(int standard, int frame, int row, int *vswitch)
{
    if (standard == COMP_STD_PAL) {
        const int frame_line = 44 + row;
        const int field_id = (frame * 2 + (frame_line % 2)) % 8;
        const int prev_lines = (field_id / 2) * 625 + (field_id % 2) * 313 + frame_line / 2;
        *vswitch = (prev_lines % 2) == 0 ? 1 : -1;
        return (182 / 4.0) + prev_lines * 283.7516;
    } else {
        const int frame_line = 39 + row;
        const int field_id = (frame * 2 + (frame_line % 2)) % 4;
        const int prev_lines = (field_id / 2) * 525 + (field_id % 2) * 263 + frame_line / 2;
        *vswitch = 1;
        const double zero_h = 784 + 33.0 / 90.0 - 768;
        return (147 - zero_h) / 4.0 + prev_lines * 227.5 - 0.25;
    }
}

static void test_against_reference(int standard, int den, int rows, const char *name)
{
    for (int frame = 0; frame < 8; frame++) {
        for (int row = 0; row < rows; row++) {
            int ref_vsw;
            const double ref = fmod(ref_cycles(standard, frame, row, &ref_vsw), 1.0);
            const comp_sc_line_t sc = comp_sc_line(standard, frame, row);
            double diff = fabs((double)sc.phase / den - ref);
            if (diff > 0.5)
                diff = 1.0 - diff;
            CHECK(diff < 1e-6, "%s frame %d row %d: phase %d/%d, reference %f",
                  name, frame, row, sc.phase, den, ref);
            CHECK(sc.vswitch == ref_vsw, "%s frame %d row %d: vswitch %d, reference %d",
                  name, frame, row, sc.vswitch, ref_vsw);
        }
    }
}

static void test_sequence(int standard, int period, int rows, const char *name)
{
    for (int frame = 0; frame < period; frame++) {
        for (int row = 0; row < rows; row++) {
            const comp_sc_line_t a = comp_sc_line(standard, frame, row);
            const comp_sc_line_t b = comp_sc_line(standard, frame + period, row);
            CHECK(a.phase == b.phase && a.vswitch == b.vswitch,
                  "%s frame %d row %d: sequence does not repeat after %d frames",
                  name, frame, row, period);
        }
        const comp_sc_line_t a = comp_sc_line(standard, frame, 0);
        const comp_sc_line_t b = comp_sc_line(standard, frame + 1, 0);
        CHECK(a.phase != b.phase || a.vswitch != b.vswitch,
              "%s frame %d: consecutive frames have identical phase", name, frame);
    }
}

int main(void)
{
    test_against_reference(COMP_STD_PAL, COMP_SC_DEN_PAL, 576, "pal");
    test_against_reference(COMP_STD_NTSC, COMP_SC_DEN_NTSC, 486, "ntsc");

    test_sequence(COMP_STD_PAL, 4, 576, "pal");
    test_sequence(COMP_STD_NTSC, 2, 486, "ntsc");

    /* known anchor values, computed by hand from the ratios */
    CHECK(comp_sc_line(COMP_STD_PAL, 0, 0).phase == 88, "pal anchor");
    CHECK(comp_sc_line(COMP_STD_PAL, 0, 0).vswitch == 1, "pal anchor vswitch");
    CHECK(comp_sc_line(COMP_STD_NTSC, 0, 0).phase == 294, "ntsc anchor");

    /* PAL V-switch alternates between consecutive lines of a field */
    for (int row = 0; row < 574; row++)
        CHECK(comp_sc_line(COMP_STD_PAL, 0, row).vswitch ==
              -comp_sc_line(COMP_STD_PAL, 0, row + 2).vswitch,
              "pal row %d: vswitch does not alternate", row);

    if (!fail)
        printf("test_subcarrier: all tests passed\n");
    return fail;
}
