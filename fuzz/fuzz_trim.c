/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
#include "fuzz_common.h"
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    mstr_scope {
        mstr s = mstr_from_n((const char *)d, n);
        size_t b = 0, e = n;
        while (b < e && is_ws(d[b])) b++;
        while (e > b && is_ws(d[e - 1])) e--;
        mstr t = mstr_trim(s), l = mstr_ltrim(s), r = mstr_rtrim(s);
        check_repr(t); check_repr(l); check_repr(r);
        REQUIRE(mstr_len(t) == e - b && memcmp(mstr_ptr(&t), d + b, e - b) == 0);
        REQUIRE(mstr_eq(t, mstr_ltrim(r)) && mstr_eq(t, mstr_rtrim(l)));
        REQUIRE(mstr_eq(mstr_trim(t), t)); /* idempotent */
    }
    return 0;
}
