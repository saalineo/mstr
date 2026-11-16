/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
#include "fuzz_common.h"

/* Independent RFC 3629 validator (pattern table, not sharing code with the decoder). */
#define CONT(b) ((b) >= 0x80 && (b) <= 0xBF)
static int ref_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c <= 0x7F) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { if (i + 1 < n && CONT(s[i + 1])) { i += 2; continue; } return 0; }
        if (c == 0xE0) { if (i + 2 < n && s[i + 1] >= 0xA0 && s[i + 1] <= 0xBF && CONT(s[i + 2])) { i += 3; continue; } return 0; }
        if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) { if (i + 2 < n && CONT(s[i + 1]) && CONT(s[i + 2])) { i += 3; continue; } return 0; }
        if (c == 0xED) { if (i + 2 < n && s[i + 1] >= 0x80 && s[i + 1] <= 0x9F && CONT(s[i + 2])) { i += 3; continue; } return 0; }
        if (c == 0xF0) { if (i + 3 < n && s[i + 1] >= 0x90 && s[i + 1] <= 0xBF && CONT(s[i + 2]) && CONT(s[i + 3])) { i += 4; continue; } return 0; }
        if (c >= 0xF1 && c <= 0xF3) { if (i + 3 < n && CONT(s[i + 1]) && CONT(s[i + 2]) && CONT(s[i + 3])) { i += 4; continue; } return 0; }
        if (c == 0xF4) { if (i + 3 < n && s[i + 1] >= 0x80 && s[i + 1] <= 0x8F && CONT(s[i + 2]) && CONT(s[i + 3])) { i += 4; continue; } return 0; }
        return 0;
    }
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    mstr_scope {
        mstr s = mstr_from_n((const char *)d, n), acc = mstr_empty();
        size_t units = mstr_u8_count(s), k = 0, i;
        int valid = mstr_u8_valid(s);
        REQUIRE(valid == ref_valid(d, n));
        REQUIRE(units <= n && units * 4 >= n);
        mstr_u8_each(ch, s) { REQUIRE(mstr_u8_valid(ch) && mstr_len(ch) >= 1 && mstr_len(ch) <= 4); k++; if (valid) acc = mstr_cat(acc, ch); }
        REQUIRE(k == units);
        if (valid) REQUIRE(mstr_eq(acc, s));
        /* unit slices partition the original bytes; a cut never splits a sequence */
        if (units) {
            size_t cut = d[0] % (units + 1);
            mstr l = mstr_u8_slice(s, 0, cut), r = mstr_u8_slice(s, cut, units);
            REQUIRE(mstr_len(l) + mstr_len(r) == n && mstr_u8_count(l) == cut && mstr_u8_count(r) == units - cut);
            if (valid) REQUIRE(mstr_u8_valid(l) && mstr_u8_valid(r));
            check_repr(l); check_repr(r);
        }
        for (i = 0; i < units && i < 64; i++) {
            mstr u = mstr_u8_slice(s, i, i + 1);
            REQUIRE(mstr_u8_count(u) == 1);
        }
        /* encode/decode of any code point derived from the input */
        if (n >= 3) {
            uint32_t cp = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
            mstr e = mstr_u8_from_cp(cp);
            REQUIRE(mstr_u8_valid(e) && mstr_u8_count(e) == 1);
            if (cp <= 0x10FFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu)) REQUIRE(mstr_u8_cp(e) == cp);
            else REQUIRE(mstr_u8_cp(e) == 0xFFFDu);
        }
    }
    return 0;
}
