/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
// Proves the public header is valid C++ and behaves the same.
#include "mstr.h"

#include <cstdio>
#include <cstring>

static int fails;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
    mstr s = S("  padded string that is longer than twenty-two bytes  ");
    mstr t = mstr_trim(s);
    CHECK(mstr_len(t) == 50);
    CHECK(mstr_state(&t) == MSTR_VIEW);
    CHECK(mstr_eq(mstr_slice(t, 0, 6), S("padded")));
    CHECK(mstr_find(t, S("string")) == 7);
    CHECK(mstr_at(t, 0) == 'p');
    CHECK(mstr_is_empty(mstr_empty()));

    mstr_scope {
        mstr c = mstr_cat(t, S("!"));
        CHECK(mstr_state(&c) == MSTR_REGION && mstr_len(c) == 51);
        CHECK(std::strcmp(MSTR_CSTR(mstr_upper(S("abc"))), "ABC") == 0);
        std::size_t n = 0;
        mstr_each(tok, S("a,b,c"), S(",")) { (void)tok; n++; }
        CHECK(n == 3);
        n = 0;
        mstr_lines(ln, S("x\ny\n")) { (void)ln; n++; }
        CHECK(n == 2);
        mstr f = mstr_fmt("%s-%d", "a long enough string to spill out of SSO", 1);
        CHECK(mstr_state(&f) == MSTR_REGION);
        mstr_scope_t sc = mstr_scope_enter();
        mstr p = mstr_scope_return(&sc, mstr_replace_all(f, S("a"), S("A")));
        CHECK(mstr_find(p, S("A long")) == 0);
    }
    std::printf("cpp header %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
