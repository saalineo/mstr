/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
#include "mstr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_run;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        g_run++;                                                                         \
        if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static mstr big(size_t n, char ch)
{
    mstr r;
    char *d = (char *)malloc(n);
    memset(d, ch, n);
    r = mstr_from_n(d, n);
    free(d);
    return r;
}
static int is_nul_term(mstr s) { return mstr_ptr(&s)[mstr_len(s)] == '\0'; }

static void test_cat(void)
{
    mstr_scope {
        mstr a = mstr_cat(S("hello, "), S("world"));
        CHECK(mstr_state(&a) == MSTR_SSO && mstr_eq(a, S("hello, world")));
        mstr b = mstr_cat(S("0123456789012"), S("456789012")); /* 22 */
        CHECK(mstr_state(&b) == MSTR_SSO && mstr_len(b) == 22);
        mstr c = mstr_cat(S("0123456789012"), S("4567890123")); /* 23 */
        CHECK(mstr_state(&c) == MSTR_REGION && mstr_len(c) == 23 && is_nul_term(c));
        CHECK(mstr_eq(mstr_cat(S(""), S("")), S("")));
        mstr e = mstr_cat(S("a\0b"), S("\0c"));
        CHECK(mstr_len(e) == 5 && mstr_ptr(&e)[2] == 'b' && mstr_ptr(&e)[3] == '\0');
        mstr v = mstr_cat(c, c);
        CHECK(mstr_len(v) == 46 && mstr_find(v, S("3012")) == 22);
    }
}

static void test_replace(void)
{
    mstr_scope {
        CHECK(mstr_eq(mstr_replace(S("a-b-c"), S("-"), S("+")), S("a+b-c")));
        CHECK(mstr_eq(mstr_replace_all(S("a-b-c"), S("-"), S("+")), S("a+b+c")));
        CHECK(mstr_eq(mstr_replace_all(S("aaa"), S("aa"), S("b")), S("ba"))); /* non-overlapping */
        CHECK(mstr_eq(mstr_replace_all(S("abc"), S("b"), S("")), S("ac")));
        CHECK(mstr_eq(mstr_replace_all(S("abc"), S("abc"), S("")), S("")));
        CHECK(mstr_eq(mstr_replace_all(S("a.b"), S("."), S("<->")), S("a<->b")));
        size_t before = mstr_debug_arena_used_();
        mstr h = S("no match anywhere in this long literal text");
        mstr r = mstr_replace_all(h, S("zzz"), S("y"));
        CHECK(mstr_ptr(&r) == mstr_ptr(&h) && mstr_debug_arena_used_() == before); /* no alloc */
        CHECK(mstr_eq(mstr_replace_all(S("abc"), S(""), S("x")), S("abc")));        /* empty from */
        mstr grown = mstr_replace_all(S("x.y.z"), S("."), S("[a long replacement text]"));
        CHECK(mstr_state(&grown) == MSTR_REGION && is_nul_term(grown));
        CHECK(mstr_eq(grown, S("x[a long replacement text]y[a long replacement text]z")));
        mstr nul = mstr_replace_all(mstr_from_n("a\0b\0c", 5), mstr_from_n("\0", 1), S("--"));
        CHECK(mstr_eq(nul, S("a--b--c")));
    }
}

static void test_case_fmt_misc(void)
{
    mstr_scope {
        CHECK(mstr_eq(mstr_upper(S("Hello, World 1!")), S("HELLO, WORLD 1!")));
        CHECK(mstr_eq(mstr_lower(S("Hello, World 1!")), S("hello, world 1!")));
        mstr up = S("ALREADY UPPER CASE BUT LONG ENOUGH TO BE STATIC");
        CHECK(mstr_ptr((mstr[1]){mstr_upper(up)}) == mstr_ptr(&up)); /* unchanged: same bytes */
        mstr lng = mstr_lower(S("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"));
        CHECK(mstr_state(&lng) == MSTR_REGION && mstr_eq(lng, S("abcdefghijklmnopqrstuvwxyz0123456789")));
        CHECK(mstr_eq(mstr_upper(mstr_from_n("\xe9z", 2)), mstr_from_n("\xe9Z", 2))); /* bytes >127 untouched */

        /* The fused form has exactly the nested operation's semantics, but no
         * intermediate REGIONs. Its match is against the case-mapped bytes. */
        mstr fused = mstr_trim_case_replace_all(S("  a b a b a b a b a b a b  "),
                                                 MSTR_ASCII_UPPER, S("A B"), S("long"));
        mstr nested = mstr_replace_all(mstr_upper(mstr_trim(S("  a b a b a b a b a b a b  "))),
                                       S("A B"), S("long"));
        CHECK(mstr_eq(fused, nested) && mstr_state(&fused) == MSTR_REGION && is_nul_term(fused));
        CHECK(mstr_eq(mstr_trim_case_replace_all(S("  MiXeD  "), MSTR_ASCII_LOWER, S("ix"), S("_")), S("m_ed")));
        CHECK(mstr_eq(mstr_trim_case_replace_all(S("  unchanged  "), MSTR_ASCII_KEEP, S(""), S("x")), S("unchanged")));

        mstr f = mstr_fmt("%d-%s", 42, "x");
        CHECK(mstr_state(&f) == MSTR_SSO && mstr_eq(f, S("42-x")));
        mstr f22 = mstr_fmt("%022d", 7);
        CHECK(mstr_state(&f22) == MSTR_SSO && mstr_len(f22) == 22);
        mstr fl = mstr_fmt("%s|%s|%d", "a fairly long first part", "second", 123456);
        CHECK(mstr_state(&fl) == MSTR_REGION && mstr_eq(fl, S("a fairly long first part|second|123456")));

        CHECK(mstr_eq(mstr_repeat(S("ab"), 3), S("ababab")));
        CHECK(mstr_is_empty(mstr_repeat(S("ab"), 0)) && mstr_is_empty(mstr_repeat(S(""), 99)));
        mstr rl = mstr_repeat(S("0123456789"), 5);
        CHECK(mstr_state(&rl) == MSTR_REGION && mstr_len(rl) == 50 && is_nul_term(rl));
        mstr parts[3] = {S("a"), S("bb"), S("ccc")};
        CHECK(mstr_eq(mstr_join(S(", "), parts, 3), S("a, bb, ccc")));
        CHECK(mstr_is_empty(mstr_join(S(","), parts, 0)));
        CHECK(mstr_eq(mstr_join(S(","), parts, 1), S("a")));
    }
}

static void test_iter(void)
{
    mstr_scope {
        size_t before = mstr_debug_arena_used_(), n = 0;
        mstr expect[4] = {S("a"), S("b"), S(""), S("c")};
        mstr_each(tok, S("a,b,,c"), S(",")) {
            CHECK(n < 4 && mstr_eq(tok, expect[n]));
            n++;
        }
        CHECK(n == 4);
        mstr text = S("one two  three four five six seven eight nine ten");
        n = 0;
        mstr_each(w, text, S(" ")) { n++; CHECK(mstr_state(&w) == MSTR_SSO); }
        CHECK(n == 11); /* 10 words + one empty from the double space */
        CHECK(mstr_debug_arena_used_() == before);        /* zero allocation */

        n = 0;
        mstr_each(t, S(""), S(",")) { n++; CHECK(mstr_is_empty(t)); }
        CHECK(n == 1);
        n = 0;
        mstr_each(t2, S(",a,"), S(",")) n++;
        CHECK(n == 3);
        n = 0;
        mstr_each(t3, S("abc"), S("")) { n++; CHECK(mstr_eq(t3, S("abc"))); }
        CHECK(n == 1);
        n = 0;
        mstr_each(t4, S("a<>b<>c"), S("<>")) n++;
        CHECK(n == 3);
        n = 0;
        mstr_each(t5, S("a,b,c,d"), S(",")) { n++; if (n == 2) break; }
        CHECK(n == 2);
        /* nested */
        n = 0;
        mstr_each(row, S("a b;c d e;f"), S(";")) { mstr_each(cell, row, S(" ")) { (void)cell; n++; } }
        CHECK(n == 6);

        /* long tokens are zero-copy VIEWs into the source */
        mstr src = S("0123456789012345678901234,abcdefghijklmnopqrstuvwxyz");
        n = 0;
        mstr_each(lt, src, S(",")) {
            CHECK(mstr_state(&lt) == MSTR_VIEW);
            CHECK(mstr_ptr(&lt) == mstr_ptr(&src) + (n ? 26 : 0));
            n++;
        }
        CHECK(n == 2);

        n = 0;
        mstr lines[4] = {S("l1"), S(""), S("l3"), S("last")};
        mstr_lines(ln2, S("l1\r\n\nl3\nlast")) { CHECK(n < 4 && mstr_eq(ln2, lines[n])); n++; }
        CHECK(n == 4);
        n = 0;
        mstr_lines(ln3, S("a\nb\n")) n++;
        CHECK(n == 2);
        n = 0;
        mstr_lines(ln4, S("")) n++;
        CHECK(n == 0);
        n = 0;
        mstr_lines(ln5, S("\n")) { n++; CHECK(mstr_is_empty(ln5)); }
        CHECK(n == 1);

        size_t cnt = 0;
        mstr *arr = mstr_split(S("x,yy,,zzz"), S(","), &cnt);
        CHECK(cnt == 4 && mstr_eq(arr[0], S("x")) && mstr_eq(arr[1], S("yy")) && mstr_is_empty(arr[2]) && mstr_eq(arr[3], S("zzz")));
        CHECK(((uintptr_t)arr & 7u) == 0);
        arr = mstr_split(S(""), S(","), &cnt);
        CHECK(cnt == 1 && mstr_is_empty(arr[0]));
    }
}

static void test_scopes(void)
{
    size_t base = mstr_debug_arena_used_(), i;
    mstr_scope {
        mstr a = big(100, 'a');
        size_t mid = mstr_debug_arena_used_();
        CHECK(mid > base);
        mstr_scope {
            mstr b = big(1000, 'b');
            CHECK(mstr_len(b) == 1000);
            CHECK(mstr_debug_arena_used_() > mid);
        }
        CHECK(mstr_debug_arena_used_() == mid); /* inner rewound, outer intact */
        CHECK(mstr_len(a) == 100 && mstr_ptr(&a)[99] == 'a');
#ifndef MSTR_STRICT_SCOPE /* strict variant: break skips the rewind (documented) */
        for (i = 0; i < 5; i++) mstr_scope { (void)big(50, 'x'); if (i == 2) break; }
#else
        for (i = 0; i < 5; i++) mstr_scope { (void)big(50, 'x'); }
#endif
        CHECK(mstr_debug_arena_used_() == mid);
    }
    CHECK(mstr_debug_arena_used_() == base);

    /* oversized and multi-chunk allocation, then rewind returns chunks */
    size_t chunks = mstr_debug_chunk_count_();
    mstr_scope {
        mstr huge = big(300000, 'h'); /* dedicated chunk */
        CHECK(mstr_len(huge) == 300000 && is_nul_term(huge) && mstr_ptr(&huge)[299999] == 'h');
        for (i = 0; i < 50; i++) (void)big(20000, 'p'); /* several standard chunks */
        CHECK(mstr_debug_chunk_count_() > chunks + 2);
    }
    CHECK(mstr_debug_chunk_count_() == chunks);
    CHECK(mstr_debug_free_chunks_() >= 1 && mstr_debug_free_chunks_() <= 4);
    /* many iterations reuse cached chunks instead of growing */
    for (i = 0; i < 200; i++) mstr_scope { (void)big(100000, 'q'); }
    CHECK(mstr_debug_free_chunks_() <= 4 && mstr_debug_chunk_count_() == chunks);
}

#ifndef MSTR_STRICT_SCOPE
static mstr early(int which)
{
    mstr_scope {
        (void)big(5000, 'e');
        if (which) return S("returned"); /* cleanup attribute rewinds on return */
    }
    return S("fell out");
}
static void test_gcc_cleanup(void)
{
    size_t base = mstr_debug_arena_used_();
    CHECK(mstr_eq(early(1), S("returned")) && mstr_debug_arena_used_() == base);
    CHECK(mstr_eq(early(0), S("fell out")) && mstr_debug_arena_used_() == base);
    mstr_scope { (void)big(300, 'g'); goto out; }
out:
    CHECK(mstr_debug_arena_used_() == base);
}
#endif

static mstr make_greeting(mstr who)
{
    mstr_scope_t s = mstr_scope_enter();
    mstr tmp = mstr_cat(S("Hello, dear and very honoured "), who);
    mstr up = mstr_upper(tmp);
    return mstr_scope_return(&s, mstr_cat(up, S("!")));
}

static void test_promote(void)
{
    size_t base;
    mstr_thread_cleanup(); /* fresh arena: bookmark chunk is NULL */
    base = mstr_debug_arena_used_();
    mstr_scope {
        mstr g = make_greeting(S("guest"));
        CHECK(mstr_eq(g, S("HELLO, DEAR AND VERY HONOURED GUEST!")) && is_nul_term(g));
        CHECK(mstr_state(&g) == MSTR_REGION);
        /* only the promoted string (rounded) remains in the arena */
        CHECK(mstr_debug_arena_used_() == base + 40);
        mstr g2 = make_greeting(S("another"));
        CHECK(mstr_eq(g, S("HELLO, DEAR AND VERY HONOURED GUEST!")));
        CHECK(mstr_eq(g2, S("HELLO, DEAR AND VERY HONOURED ANOTHER!")));
    }

    mstr_scope { /* same-chunk, overlapping: source starts inside destination */
        mstr_scope_t s = mstr_scope_enter();
        (void)mstr_scope_alloc(8);
        mstr r = big(40, 'o');
        const char *rp = mstr_ptr(&r); /* read before promote: r is dead afterwards */
        mstr p = mstr_promote(&s, r);
        CHECK(mstr_len(p) == 40 && memcmp(mstr_ptr(&p), "oooooooooooooooooooooooooooooooooooooooo", 40) == 0);
        CHECK(is_nul_term(p) && mstr_ptr(&p) + 8 == rp);
        CHECK(mstr_debug_arena_used_() > 0);
    }

    mstr_scope { /* source in a later chunk than the bookmark chunk */
        mstr_scope_t s = mstr_scope_enter();
        (void)big(100000, 'j'); /* pushes into a new chunk */
        mstr r = mstr_cat(big(70, 'k'), S("tail"));
        mstr p = mstr_promote(&s, r);
        CHECK(mstr_len(p) == 74 && mstr_ptr(&p)[69] == 'k' && mstr_eq(mstr_slice(p, 70, 74), S("tail")) && is_nul_term(p));
    }

    mstr_scope { /* values from older scopes, SSO and STATIC pass through unchanged */
        mstr outer = big(64, 'z');
        mstr_scope_t s = mstr_scope_enter();
        mstr view = mstr_slice(outer, 2, 60); /* VIEW into the parent: still valid */
        mstr p = mstr_promote(&s, view);
        CHECK(mstr_state(&p) == MSTR_VIEW && mstr_ptr(&p) == mstr_ptr(&outer) + 2);
        mstr_scope_t s2 = mstr_scope_enter();
        mstr lit = S("a static string longer than twenty-two bytes");
        mstr p2 = mstr_promote(&s2, lit);
        CHECK(mstr_state(&p2) == MSTR_STATIC && mstr_ptr(&p2) == mstr_ptr(&lit));
        mstr_scope_t s3 = mstr_scope_enter();
        mstr p3 = mstr_scope_return(&s3, S("tiny"));
        CHECK(mstr_state(&p3) == MSTR_SSO && mstr_eq(p3, S("tiny")));
        mstr_scope_leave(&s); /* already closed by promote: no-op */
    }

    mstr_scope { /* a VIEW into the child's data is copied (promoted to REGION) */
        mstr_scope_t s = mstr_scope_enter();
        mstr child = big(80, 'v');
        mstr p = mstr_promote(&s, mstr_slice(child, 5, 70));
        CHECK(mstr_state(&p) == MSTR_REGION && mstr_len(p) == 65 && is_nul_term(p));
        CHECK(mstr_ptr(&p)[0] == 'v' && mstr_ptr(&p)[64] == 'v');
    }
}

static void test_cstr(void)
{
    mstr_scope {
        mstr sso = S("short");
        CHECK(strcmp(MSTR_CSTR(sso), "short") == 0);
        mstr st = S("a static string that is certainly long enough");
        CHECK(MSTR_CSTR(st) == mstr_ptr(&st));
        mstr reg = big(40, 'r');
        CHECK(MSTR_CSTR(reg) == mstr_ptr(&reg) && strlen(MSTR_CSTR(reg)) == 40);
        /* VIEW ending at the buffer's NUL: no copy */
        mstr tail = mstr_slice(reg, 10, 40);
        size_t used = mstr_debug_arena_used_();
        CHECK(MSTR_CSTR(tail) == mstr_ptr(&tail) && mstr_debug_arena_used_() == used);
        /* VIEW without NUL after it: copy into the scope */
        mstr mid = mstr_slice(reg, 2, 35);
        const char *c = MSTR_CSTR(mid);
        CHECK(c != mstr_ptr(&mid) && strlen(c) == 33 && mstr_debug_arena_used_() > used);
        CHECK(strcmp(MSTR_CSTR(S("")), "") == 0);
        CHECK(strcmp(MSTR_CSTR(mstr_cat(S("ab"), S("cd"))), "abcd") == 0);
    }
}

/* ---- property tests against naive references ---- */
static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static size_t naive_replace_all(const char *s, size_t sl, const char *f, size_t fl, const char *t, size_t tl, char *out)
{
    size_t i = 0, o = 0;
    while (i < sl) {
        if (fl && i + fl <= sl && memcmp(s + i, f, fl) == 0) { memcpy(out + o, t, tl); o += tl; i += fl; }
        else out[o++] = s[i++];
    }
    return o;
}

static void test_properties(void)
{
    static char sbuf[256], fbuf[4], tbuf[8], obuf[2048];
    int it;
    for (it = 0; it < 20000; it++) mstr_scope {
        size_t sl = (size_t)(rnd() % 120), fl = 1 + (size_t)(rnd() % 3), tl = (size_t)(rnd() % 6), i, on;
        mstr_ascii_case mode = (mstr_ascii_case)(rnd() % 3);
        for (i = 0; i < sl; i++) sbuf[i] = (char)(" aAbB\0"[rnd() % 6]);
        for (i = 0; i < fl; i++) fbuf[i] = (char)("aAbB\0"[rnd() % 5]);
        for (i = 0; i < tl; i++) tbuf[i] = (char)("xY "[rnd() % 3]);
        mstr s = mstr_from_n(sbuf, sl), f = mstr_from_n(fbuf, fl), t = mstr_from_n(tbuf, tl);
        mstr r = mstr_replace_all(s, f, t);
        on = naive_replace_all(sbuf, sl, fbuf, fl, tbuf, tl, obuf);
        CHECK(mstr_len(r) == on && memcmp(mstr_ptr(&r), obuf, on) == 0 && is_nul_term(r));
        if (mstr_len(r) <= MSTR_SSO_MAX) CHECK(mstr_state(&r) == MSTR_SSO); /* Law of Small Results */
        /* split + join round-trips */
        size_t cnt;
        mstr *parts = mstr_split(s, f, &cnt);
        mstr j = mstr_join(f, parts, cnt);
        CHECK(mstr_eq(j, s));
        /* cat associativity on lengths and bytes */
        mstr c1 = mstr_cat(mstr_cat(s, f), t), c2 = mstr_cat(s, mstr_cat(f, t));
        CHECK(mstr_eq(c1, c2) && mstr_len(c1) == sl + fl + tl);
        if (mstr_len(c1) <= MSTR_SSO_MAX) CHECK(mstr_state(&c1) == MSTR_SSO);

        {
            mstr trimmed = mstr_trim(s);
            mstr cased = mode == MSTR_ASCII_UPPER ? mstr_upper(trimmed) :
                         mode == MSTR_ASCII_LOWER ? mstr_lower(trimmed) : trimmed;
            mstr nested = mstr_replace_all(cased, f, t);
            mstr fused = mstr_trim_case_replace_all(s, mode, f, t);
            CHECK(mstr_eq(fused, nested));
        }
    }
}

int main(void)
{
    test_cat();
    test_replace();
    test_case_fmt_misc();
    test_iter();
    test_scopes();
#ifndef MSTR_STRICT_SCOPE
    test_gcc_cleanup();
#endif
    test_promote();
    test_cstr();
    test_properties();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail != 0;
}
