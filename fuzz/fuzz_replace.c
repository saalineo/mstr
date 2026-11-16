/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
#include "fuzz_common.h"
#include <stdlib.h>
/* input: [flen:1..4][tlen:0..7][from][to][text...] */
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    size_t fl, tl;
    if (n < 2) return 0;
    fl = 1u + d[0] % 4u; tl = d[1] % 8u;
    if (n < 2 + fl + tl) return 0;
    const char *from = (const char *)d + 2, *to = from + fl, *text = to + tl;
    size_t sl = n - 2 - fl - tl, i, o = 0;
    char *ref = (char *)malloc(sl * 8 + 8);
    mstr_scope {
        mstr s = mstr_from_n(text, sl), f = mstr_from_n(from, fl), t = mstr_from_n(to, tl);
        for (i = 0; i < sl;) {
            if (i + fl <= sl && memcmp(text + i, from, fl) == 0) { memcpy(ref + o, to, tl); o += tl; i += fl; }
            else ref[o++] = text[i++];
        }
        mstr all = mstr_replace_all(s, f, t);
        check_repr(all);
        REQUIRE(mstr_len(all) == o && memcmp(mstr_ptr(&all), ref, o) == 0);
        size_t pos = mstr_find(s, f);
        mstr one = mstr_replace(s, f, t);
        check_repr(one);
        if (pos == MSTR_NPOS) REQUIRE(mstr_eq(one, s));
        else REQUIRE(mstr_len(one) == sl - fl + tl && memcmp(mstr_ptr(&one), text, pos) == 0 &&
                     memcmp(mstr_ptr(&one) + pos, to, tl) == 0);
        check_repr(mstr_upper(s)); check_repr(mstr_lower(s));
    }
    free(ref);
    return 0;
}
