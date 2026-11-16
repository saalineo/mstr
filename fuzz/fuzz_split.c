/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
#include "fuzz_common.h"
/* input: [dlen:1..3][delim][text...] */
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    size_t dl, tn, cnt = 0, k = 0, nl = 0;
    if (n < 2) return 0;
    dl = 1u + d[0] % 3u;
    if (n < 1 + dl) return 0;
    tn = n - 1 - dl;
    mstr_scope {
        mstr delim = mstr_from_n((const char *)d + 1, dl), s = mstr_from_n((const char *)d + 1 + dl, tn);
        mstr *parts = mstr_split(s, delim, &cnt);
        REQUIRE(cnt >= 1);
        mstr_each(tok, s, delim) { REQUIRE(k < cnt && mstr_eq(tok, parts[k])); check_repr(tok); k++; }
        REQUIRE(k == cnt);
        REQUIRE(mstr_eq(mstr_join(delim, parts, cnt), s)); /* split/join round-trip */
        mstr_lines(ln, s) { check_repr(ln); REQUIRE(mstr_find(ln, S("\n")) == MSTR_NPOS); nl++; }
        size_t newlines = 0, i;
        for (i = 0; i < tn; i++) newlines += d[1 + dl + i] == '\n';
        REQUIRE(nl == (tn == 0 ? 0 : newlines + (d[n - 1] == '\n' ? 0 : 1)));
    }
    return 0;
}
