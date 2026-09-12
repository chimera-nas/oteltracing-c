// SPDX-FileCopyrightText: 2026 Ben Jarvis
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
struct otel_random_state { uint64_t lo, hi; };
static inline uint64_t otel_random_next(struct otel_random_state *state)
{
    /* Low 128 bits of the original PCG multiplication, using 32-bit limbs. */
    const uint64_t multiplier = UINT64_C(0xda942042e4dd58b5);
    uint64_t a0 = (uint32_t) state->lo, a1 = state->lo >> 32;
    uint64_t b0 = (uint32_t) multiplier, b1 = multiplier >> 32;
    uint64_t w0 = a0 * b0;
    uint64_t t = a1 * b0 + (w0 >> 32);
    uint64_t w1 = (uint32_t) t;
    uint64_t w2 = t >> 32;
    w1 += a0 * b1;
    state->hi = state->hi * multiplier + a1 * b1 + w2 + (w1 >> 32);
    state->lo *= multiplier;
    return state->hi;
}
