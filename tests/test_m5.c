/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* M5: UTF-8 layer and the fused pipeline. */
#define _POSIX_C_SOURCE 200809L
#include "mstr.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

static int g_fail, g_run;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        g_run++;                                                                         \
        if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

#ifdef MSTR_PIPE
static mstr big(size_t n, char ch)
{
    mstr r;
    char *d = (char *)malloc(n);
    memset(d, ch, n);
    r = mstr_from_n(d, n);
    free(d);
    return r;
}
#endif
static int is_nul_term(mstr s) { return mstr_ptr(&s)[mstr_len(s)] == '\0'; }
static mstr B(const char *bytes, size_t n) { return mstr_from_n(bytes, n); }
#define BYTES(lit) B(lit, sizeof(lit) - 1u)

/* ---- independent UTF-8 validator (RFC 3629 table, written as patterns) ---- */
#define CONT(b) ((b) >= 0x80 && (b) <= 0xBF)
static int ref_valid(const unsigned char *s, size_t n)
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

static void test_u8_basics(void)
{
    mstr_scope {
        mstr s = BYTES("a\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80z"); /* a é 日 😀 z */
        CHECK(mstr_len(s) == 11 && mstr_u8_count(s) == 5 && mstr_u8_valid(s));
        mstr mid = mstr_u8_slice(s, 1, 4); /* é日😀 */
        CHECK(mstr_len(mid) == 9 && memcmp(mstr_ptr(&mid), "\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80", 9) == 0);
        CHECK(mstr_u8_valid(mid) && mstr_u8_count(mid) == 3);
        CHECK(mstr_is_empty(mstr_u8_slice(s, 2, 2)) && mstr_is_empty(mstr_u8_slice(s, 5, 5)));
        CHECK(mstr_eq(mstr_u8_slice(s, 0, 5), s));
        CHECK(mstr_eq(mstr_u8_slice(s, 0, 1), S("a")) && mstr_eq(mstr_u8_slice(s, 4, 5), S("z")));

        static const uint32_t want[5] = {'a', 0xE9, 0x65E5, 0x1F600, 'z'};
        size_t k = 0;
        mstr_u8_each(ch, s) {
            CHECK(k < 5 && mstr_u8_cp(ch) == want[k] && mstr_state(&ch) == MSTR_SSO);
            CHECK(mstr_eq(ch, mstr_u8_from_cp(want[k])));
            k++;
        }
        CHECK(k == 5);
        k = 0;
        mstr_u8_each(c2, S("")) k++;
        CHECK(k == 0);

        /* the Law of Small Results holds for code point slices too */
        mstr longs = mstr_repeat(BYTES("\xE6\x97\xA5"), 20); /* 60 bytes, 20 cp */
        mstr part = mstr_u8_slice(longs, 2, 12);             /* 30 bytes */
        CHECK(mstr_state(&part) == MSTR_VIEW && mstr_ptr(&part) == mstr_ptr(&longs) + 6);
        part = mstr_u8_slice(longs, 2, 9);                   /* 21 bytes */
        CHECK(mstr_state(&part) == MSTR_SSO && mstr_len(part) == 21);
    }
}

/* every code point round-trips; surrogates and out-of-range map to U+FFFD */
static void test_u8_exhaustive(void)
{
    uint32_t cp;
    int bad = 0;
    for (cp = 0; cp <= 0x10FFFFu; cp++) {
        mstr e = mstr_u8_from_cp(cp);
        size_t adv;
        int ok;
        uint32_t back;
        if (cp >= 0xD800u && cp <= 0xDFFFu) { bad += mstr_u8_cp(e) != 0xFFFDu; continue; }
        back = mstr_u8_decode(mstr_ptr(&e), mstr_len(e), &adv, &ok);
        bad += !(back == cp && ok && adv == mstr_len(e) && mstr_state(&e) == MSTR_SSO && mstr_u8_valid(e) &&
                 ref_valid((const unsigned char *)mstr_ptr(&e), mstr_len(e)));
    }
    CHECK(bad == 0);
    CHECK(mstr_u8_cp(mstr_u8_from_cp(0x110000u)) == 0xFFFDu && mstr_u8_cp(mstr_u8_from_cp(0xFFFFFFFFu)) == 0xFFFDu);
}

/* The documented policy: replace with U+FFFD, one unit per maximal ill-formed subpart. */
static void expect_units(const char *bytes, size_t n, size_t units, int valid)
{
    mstr s = mstr_from_n(bytes, n);
    size_t k = 0, repl = 0;
    CHECK(mstr_u8_count(s) == units);
    CHECK(mstr_u8_valid(s) == valid);
    CHECK(ref_valid((const unsigned char *)bytes, n) == valid);
    mstr_u8_each(ch, s) {
        CHECK(mstr_u8_valid(ch) && mstr_len(ch) <= 4);
        if (mstr_eq(ch, BYTES("\xEF\xBF\xBD"))) repl++;
        k++;
    }
    CHECK(k == units);
    if (!valid) CHECK(repl >= 1); /* ill-formed input is reported as U+FFFD */
}
#define EXPECT(lit, units, valid) expect_units(lit, sizeof(lit) - 1u, units, valid)

static void test_u8_policy(void)
{
    mstr_scope {
        EXPECT("", 0, 1);
        EXPECT("abc", 3, 1);
        EXPECT("\xEF\xBF\xBD", 1, 1);            /* a genuine U+FFFD is valid */
        EXPECT("\x80", 1, 0);                    /* stray continuation */
        EXPECT("\xC0\xAF", 2, 0);                /* overlong '/': C0 never leads, AF stray */
        EXPECT("\xC1\xBF", 2, 0);
        EXPECT("\xE0\x80\x80", 3, 0);            /* overlong 3-byte: E0 + 2 strays */
        EXPECT("\xED\xA0\x80", 3, 0);            /* UTF-16 surrogate U+D800 */
        EXPECT("\xF4\x90\x80\x80", 4, 0);        /* > U+10FFFF */
        EXPECT("\xF5\x80\x80\x80", 4, 0);
        EXPECT("\xFF\xFE", 2, 0);
        EXPECT("\xF0\x9F\x98", 1, 0);            /* truncated 4-byte: ONE unit (maximal subpart) */
        EXPECT("\xE2\x82", 1, 0);                /* truncated 3-byte */
        EXPECT("\xC3", 1, 0);
        EXPECT("a\xE2\x82" "b", 3, 0);           /* a, truncated, b */
        EXPECT("\xE2\x82\xAC\xE2\x82\xAC", 2, 1);
        EXPECT("\xF0\x9F\x98\x80\x80", 2, 0);    /* valid emoji then a stray continuation */

        /* slices keep the ORIGINAL bytes; iteration shows U+FFFD */
        mstr s = BYTES("a\x80" "b");
        mstr bad = mstr_u8_slice(s, 1, 2);
        CHECK(mstr_len(bad) == 1 && (unsigned char)mstr_ptr(&bad)[0] == 0x80);
        size_t k = 0;
        mstr_u8_each(ch, s) { if (k == 1) CHECK(mstr_u8_cp(ch) == 0xFFFDu && mstr_eq(ch, BYTES("\xEF\xBF\xBD"))); k++; }
        mstr t = BYTES("\xF0\x9F\x98" "x");              /* truncated emoji then 'x' */
        CHECK(mstr_u8_count(t) == 2 && mstr_eq(mstr_u8_slice(t, 1, 2), S("x")));
    }
}

#ifndef _WIN32
static int dies(int which)
{
    pid_t p;
    int st;
    fflush(NULL);
    p = fork();
    if (p == 0) {
        if (!freopen("/dev/null", "w", stderr)) _exit(2);
        if (which == 0) (void)mstr_u8_slice(S("h\xC3\xA9llo"), 0, 9);
        else (void)mstr_u8_slice(S("hello"), 3, 2);
        _exit(0);
    }
    waitpid(p, &st, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}
#endif

static void test_u8_properties(void)
{
    static unsigned char buf[96];
    int it;
    /* biased towards bytes that matter in UTF-8 */
    static const unsigned char pool[] = {0x00, 'a', 'Z', 0x7F, 0x80, 0xBF, 0xC0, 0xC2, 0xDF, 0xE0, 0xE1, 0xED, 0xEF, 0xF0,
                                         0xF4, 0xF5, 0xFF, 0xA0, 0x9F, 0x90, 0x8F, 0xBD};
    for (it = 0; it < 60000; it++) mstr_scope {
        size_t n = (size_t)(rnd() % 40), i, k = 0, units, cat_len = 0;
        mstr s, rebuilt;
        for (i = 0; i < n; i++) buf[i] = (rnd() % 4 == 0) ? (unsigned char)rnd() : pool[rnd() % sizeof pool];
        s = mstr_from_n((const char *)buf, n);
        units = mstr_u8_count(s);
        CHECK(mstr_u8_valid(s) == ref_valid(buf, n));
        CHECK(units <= n && units * 4 >= n);
        /* iteration yields `units` well-formed tokens */
        mstr_u8_each(ch, s) { CHECK(mstr_u8_valid(ch) && mstr_len(ch) <= 4); k++; }
        CHECK(k == units);
        /* unit-wise slices partition the ORIGINAL bytes exactly */
        rebuilt = mstr_empty();
        for (i = 0; i < units; i++) { rebuilt = mstr_cat(rebuilt, mstr_u8_slice(s, i, i + 1)); cat_len++; }
        CHECK(mstr_eq(rebuilt, s) && cat_len == units);
        /* any split point partitions as well, and slices of valid text stay valid (no split sequences) */
        if (units) {
            size_t cut = (size_t)(rnd() % (units + 1));
            mstr l = mstr_u8_slice(s, 0, cut), r = mstr_u8_slice(s, cut, units);
            CHECK(mstr_eq(mstr_cat(l, r), s) && mstr_u8_count(l) == cut && mstr_u8_count(r) == units - cut);
            if (mstr_u8_valid(s)) CHECK(mstr_u8_valid(l) && mstr_u8_valid(r));
        }
        /* valid input: tokens re-concatenate to the input */
        if (mstr_u8_valid(s)) {
            mstr acc = mstr_empty();
            mstr_u8_each(c2, s) acc = mstr_cat(acc, c2);
            CHECK(mstr_eq(acc, s));
        }
    }
}

/* ======================= pipeline ======================= */

#ifdef MSTR_PIPE /* statement expressions: gcc/clang/clang-cl. MSVC runs the explicit-API property test below. */
static void test_pipe_basic(void)
{
    mstr_scope {
        mstr in = S("   hello brave new world, this is a padded line   ");
        mstr nested = mstr_replace_all(mstr_upper(mstr_trim(in)), S(" "), S("_"));
        mstr piped = MSTR_PIPE(in, mstr_trim, mstr_upper, mstr_replace_all(S(" "), S("_")));
        CHECK(mstr_eq(piped, nested) && mstr_eq(piped, S("HELLO_BRAVE_NEW_WORLD,_THIS_IS_A_PADDED_LINE")));
        CHECK(mstr_state(&piped) == MSTR_REGION && is_nul_term(piped));

        /* a result of <= 22 bytes is SSO, like the nested version */
        mstr tiny = MSTR_PIPE(S("  abc  "), mstr_trim, mstr_upper);
        CHECK(mstr_state(&tiny) == MSTR_SSO && mstr_eq(tiny, S("ABC")));

        /* every supported step */
        mstr all = MSTR_PIPE(S("  Mixed Case  "), mstr_ltrim, mstr_rtrim, mstr_lower, mstr_cat(S("!")),
                             mstr_replace(S("mixed"), S("MIXED")), mstr_repeat(3), mstr_slice_range(2, 20));
        CHECK(mstr_eq(all, mstr_slice(mstr_repeat(mstr_replace(mstr_cat(mstr_lower(mstr_rtrim(mstr_ltrim(S("  Mixed Case  ")))), S("!")), S("mixed"), S("MIXED")), 3), 2, 20)));
        mstr u8 = MSTR_PIPE(BYTES("\xC3\xA9t\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC"), mstr_u8_slice_range(1, 4), mstr_upper);
        CHECK(mstr_eq(u8, BYTES("T\xC3\xA9 ")));

        /* steps whose arguments allocate in the arena themselves */
        mstr withargs = MSTR_PIPE(mstr_repeat(S("ab "), 12), mstr_replace_all(mstr_lower(S("B")), mstr_cat(S("<"), S(">"))), mstr_trim);
        CHECK(mstr_eq(withargs, mstr_trim(mstr_replace_all(mstr_repeat(S("ab "), 12), mstr_lower(S("B")), mstr_cat(S("<"), S(">"))))));
        CHECK(mstr_eq(withargs, S("a<> a<> a<> a<> a<> a<> a<> a<> a<> a<> a<> a<>")));
        mstr nest = MSTR_PIPE(S("outer value that is long enough to be a region"), mstr_cat(MSTR_PIPE(S("  inner  "), mstr_trim, mstr_upper)), mstr_lower);
        CHECK(mstr_eq(nest, S("outer value that is long enough to be a regioninner")));

        /* the explicit API is the same machine */
        mstr_pipe_t p = mstr_pipe_begin_(in);
        (void)mstr_pipe_hop_(&p, mstr_trim(p.v));
        (void)mstr_pipe_hop_(&p, mstr_upper(p.v));
        (void)mstr_pipe_hop_(&p, mstr_replace_all(p.v, S(" "), S("_")));
        CHECK(mstr_eq(mstr_pipe_end_(&p), nested));
    }
}

/* retained arena bytes: a chain keeps only its final result */
static void test_pipe_memory(void)
{
    mstr_scope {
        mstr in = mstr_repeat(S("  a b c d e f  "), 20); /* 300 bytes, long */
        size_t base = mstr_debug_arena_used_(), nested_used, piped_used;
        mstr n_res = mstr_replace_all(mstr_upper(mstr_replace_all(mstr_trim(in), S("a"), S("aa"))), S("B"), S("bbb"));
        nested_used = mstr_debug_arena_used_() - base;
        {
            size_t before = mstr_debug_arena_used_();
            mstr p_res = MSTR_PIPE(in, mstr_trim, mstr_replace_all(S("a"), S("aa")), mstr_upper, mstr_replace_all(S("B"), S("bbb")));
            piped_used = mstr_debug_arena_used_() - before;
            CHECK(mstr_eq(p_res, n_res));
            CHECK(piped_used == ((mstr_len(p_res) + 1 + 7) & ~(size_t)7)); /* exactly the final result */
        }
        CHECK(piped_used < nested_used);
        printf("  pipe memory: nested retains %zu bytes, pipe retains %zu\n", nested_used, piped_used);

        /* unchanged input: no allocation at all, result is the input */
        size_t before = mstr_debug_arena_used_();
        mstr same = MSTR_PIPE(in, mstr_replace(S("zzz"), S("y")), mstr_lower);
        CHECK(mstr_ptr(&same) == mstr_ptr(&in) && mstr_debug_arena_used_() == before);
        /* a short result frees everything */
        mstr shortr = MSTR_PIPE(in, mstr_replace_all(S(" "), S("")), mstr_slice_range(0, 5));
        CHECK(mstr_state(&shortr) == MSTR_SSO && mstr_debug_arena_used_() == before);
    }
}

/* chunk boundaries: growth past the bookmark chunk, shrinking back, and a NULL bookmark */
static void test_pipe_chunks(void)
{
    mstr_thread_cleanup(); /* bookmark chunk will be NULL */
    mstr_scope {
        mstr start = S("seed");
        mstr r = MSTR_PIPE(start, mstr_repeat(100000), mstr_upper, mstr_cat(S("-end")), mstr_slice_range(99990, 100004));
        CHECK(mstr_len(r) == 14 && mstr_state(&r) == MSTR_SSO);
        CHECK(mstr_eq(r, mstr_slice(mstr_cat(mstr_upper(mstr_repeat(start, 100000)), S("-end")), 99990, 100004)));
        mstr big1 = MSTR_PIPE(S("xy"), mstr_repeat(200000), mstr_replace_all(S("x"), S("xx")), mstr_replace_all(S("y"), S("yyy")));
        CHECK(mstr_len(big1) == 1000000); /* each "xy" becomes "xx" + "yyy" */
        CHECK(is_nul_term(big1) && mstr_ptr(&big1)[0] == 'x' && mstr_ptr(&big1)[999999] == 'y');
        /* ping-pong across many hops keeps memory flat */
        size_t before = mstr_debug_arena_used_(), chunks = mstr_debug_chunk_count_();
        mstr v = big(1000, 'q');
        size_t k;
        mstr_pipe_t p = mstr_pipe_begin_(v);
        for (k = 0; k < 500; k++) /* double, then halve: the size oscillates, memory must stay flat */
            (void)mstr_pipe_hop_(&p, (k & 1) ? mstr_replace_all(p.v, S("qq"), S("q")) : mstr_replace_all(p.v, S("q"), S("qq")));
        (void)mstr_pipe_end_(&p);
        CHECK(mstr_debug_chunk_count_() <= chunks + 1);
        CHECK(mstr_debug_arena_used_() < before + 2000 + 2000 + 64);
    }
}
#endif /* MSTR_PIPE */

typedef struct { int kind; mstr a, b; size_t i, j; } step;
static mstr apply(mstr v, const step *s)
{
    switch (s->kind) {
    case 0: return mstr_trim(v);
    case 1: return mstr_ltrim(v);
    case 2: return mstr_rtrim(v);
    case 3: return mstr_upper(v);
    case 4: return mstr_lower(v);
    case 5: return mstr_replace_all(v, s->a, s->b);
    case 6: return mstr_replace(v, s->a, s->b);
    case 7: return mstr_cat(v, s->a);
    case 8: return mstr_repeat(v, s->i);
    default: return mstr_slice(v, s->i, s->j);
    }
}

/* PROPERTY: for random inputs and random chains, fused == nested, byte for byte.
 * Everything the reference needs is computed BEFORE mstr_pipe_begin_: each hop
 * discards all arena data allocated after the mark except the live value. */
static void test_pipe_property(void)
{
    static char sbuf[200];
    static const char alpha[] = {'a', 'b', 'A', 'B', ' ', ' ', '\t', '\0', 'z'};
    int it;
    for (it = 0; it < 30000; it++) mstr_scope {
        step st[6];
        mstr want[6];
        size_t n = (size_t)(rnd() % 150), i, nsteps = 1 + (size_t)(rnd() % 6), k;
        mstr in, nested, cur;
        mstr_pipe_t p;
        for (i = 0; i < n; i++) sbuf[i] = alpha[rnd() % sizeof alpha];
        in = mstr_from_n(sbuf, n);
        nested = in;
        for (k = 0; k < nsteps; k++) {
            memset(&st[k], 0, sizeof st[k]);
            st[k].kind = (int)(rnd() % 10);
            st[k].a = mstr_from_n(sbuf + (rnd() % 8), (size_t)(rnd() % 3));
            st[k].b = mstr_from_n(sbuf + (rnd() % 8), (size_t)(rnd() % 30));
            st[k].i = (size_t)(rnd() % 3);
            if (st[k].kind == 9) { /* slice within the CURRENT length */
                size_t len = mstr_len(nested);
                st[k].i = len ? (size_t)(rnd() % (len + 1)) : 0;
                st[k].j = st[k].i + (len - st[k].i ? (size_t)(rnd() % (len - st[k].i + 1)) : 0);
            }
            if (st[k].kind == 8 && mstr_len(nested) > 60) st[k].kind = 3;
            nested = apply(nested, &st[k]);
            want[k] = nested;
        }
        p = mstr_pipe_begin_(in);
        for (k = 0; k < nsteps; k++) {
            (void)mstr_pipe_hop_(&p, apply(p.v, &st[k]));
            CHECK(mstr_eq(p.v, want[k]));
        }
        cur = mstr_pipe_end_(&p);
        CHECK(mstr_eq(cur, nested) && mstr_len(cur) == mstr_len(nested));
        if (mstr_len(cur) <= MSTR_SSO_MAX) CHECK(mstr_state(&cur) == MSTR_SSO);
        if (mstr_state(&cur) == MSTR_REGION) CHECK(is_nul_term(cur));
    }
}

int main(void)
{
#ifdef _WIN32
    _putenv_s("MSTR_NO_ROOT_WARNING", "1"); /* MSVC has no setenv */
#else
    setenv("MSTR_NO_ROOT_WARNING", "1", 1);
#endif
    test_u8_basics();
    test_u8_exhaustive();
    test_u8_policy();
#ifndef _WIN32
    CHECK(dies(0) && dies(1));
#endif
    test_u8_properties();
#if defined(MSTR_PIPE)
    test_pipe_basic();
    test_pipe_memory();
    test_pipe_chunks();
#endif
    test_pipe_property();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail != 0;
}
