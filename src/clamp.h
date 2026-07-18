/*
 * Saturating integer clamps for the output quantizers.
 */

#ifndef COMP_CLAMP_H
#define COMP_CLAMP_H

#include <stdint.h>

static inline int16_t clamp_i16(long v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

#endif
