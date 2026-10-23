/* SPDX-License-Identifier: MIT */
#ifndef AX_PACING_INCLUDED
#define AX_PACING_INCLUDED
#include <stdint.h>

struct ax_pacer {
    uint64_t next_ns;
    uint32_t fps;
};

/* The bridge exposes integer measurements (e.g. 59 for 59.94/60 Hz).
 * Preserve exact standard rates before rounding a near-standard counter. */
static inline uint32_t ax_nominal_fps(uint32_t measured)
{
    static const uint32_t rates[] = {24, 25, 30, 50, 60, 100, 120, 144, 240};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        if (measured == rates[i]) return measured;
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        if (measured + 1 == rates[i]) return rates[i];
    return measured ? measured : 60;
}

/* Only pace below the input rate. Native-rate capture already provides its
 * own clock; adding a second clock there risks dropping alternating frames. */
static inline uint64_t ax_pacer_deadline(struct ax_pacer *p, uint32_t requested,
                                        uint32_t input)
{
    uint32_t fps = requested && input && requested < input ? requested : 0;
    if (p->fps != fps) {
        p->next_ns = 0;
        p->fps = fps;
    }
    return p->next_ns;
}

static inline void ax_pacer_advance(struct ax_pacer *p, uint64_t now)
{
    if (!p->fps) return;
    uint64_t period = 1000000000ull / p->fps;
    if (!p->next_ns) p->next_ns = now + period;
    else if (p->next_ns <= now)
        p->next_ns += ((now - p->next_ns) / period + 1) * period;
}
#endif
