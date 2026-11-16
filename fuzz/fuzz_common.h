/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* Shared helpers for libFuzzer harnesses. A failed invariant traps. */
#ifndef MSTR_FUZZ_COMMON_H
#define MSTR_FUZZ_COMMON_H
#include "mstr.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) __builtin_trap(); } while (0)

/* Every result must obey the representation invariants. */
static void check_repr(mstr r)
{
    size_t n = mstr_len(r);
    if (n <= MSTR_SSO_MAX) { REQUIRE(mstr_state(&r) == MSTR_SSO); REQUIRE(r.b[n] == 0); }
    else { REQUIRE(mstr_state(&r) != MSTR_SSO); }
    if (mstr_state(&r) == MSTR_REGION) REQUIRE(mstr_ptr(&r)[n] == '\0');
}
static __attribute__((unused)) int is_ws(unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
#endif
