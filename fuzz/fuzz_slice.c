/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
#include "fuzz_common.h"
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 2) return 0;
    mstr_scope {
        mstr s = mstr_from_n((const char *)d + 2, n - 2);
        size_t len = n - 2, a = d[0] % (len + 1), b = d[1] % (len + 1), lo = a < b ? a : b, hi = a < b ? b : a;
        mstr r = mstr_slice(s, lo, hi);
        check_repr(r);
        REQUIRE(mstr_len(r) == hi - lo && memcmp(mstr_ptr(&r), d + 2 + lo, hi - lo) == 0);
        if (mstr_len(r) > MSTR_SSO_MAX) REQUIRE(mstr_ptr(&r) == mstr_ptr(&s) + lo); /* zero-copy */
        mstr r2 = mstr_slice(r, 0, mstr_len(r) / 2); /* slice of a slice */
        check_repr(r2);
        REQUIRE(memcmp(mstr_ptr(&r2), d + 2 + lo, mstr_len(r2)) == 0);
        REQUIRE(mstr_find(s, r) <= lo);
    }
    return 0;
}
