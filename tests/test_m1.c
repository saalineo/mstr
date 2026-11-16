/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
#include "mstr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

static int g_fail, g_run;
#define CHECK(c)                                                              \
    do {                                                                      \
        g_run++;                                                              \
        if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static const char LONG40[] = "0123456789abcdefghijklmnopqrstuvwxyz!?:;";   /* 40 bytes */

/* REGION values over local buffers, tagged with the depth/serial of a REAL live
 * scope so they stay valid under MSTR_CHECKED. */
static unsigned g_depth;
static uint64_t g_serial;
static mstr region_of(const char *buf, size_t n) { return mstr_make_long_(MSTR_REGION, buf, n, g_depth, g_serial); }

static void test_layout(void)
{
    mstr z = mstr_empty(), l;
    CHECK(sizeof(mstr) == 24);
    CHECK(sizeof(void *) == 8);
    CHECK(mstr_state(&z) == MSTR_SSO && mstr_len(z) == 0 && mstr_is_empty(z));
    CHECK(z.b[23] == 0);
    mstr zz;
    memset(&zz, 0, sizeof zz);
    CHECK(mstr_eq(zz, z)); /* all-zero is empty */

    mstr s = S("hello");
    CHECK(mstr_state(&s) == MSTR_SSO && mstr_len(s) == 5);
    CHECK(s.b[23] == (5 << 2) && s.b[5] == 0);
    CHECK(strcmp(mstr_ptr(&s), "hello") == 0);

    mstr s22 = S("0123456789012345678901");
    mstr s23 = S("01234567890123456789012");
    CHECK(mstr_state(&s22) == MSTR_SSO && mstr_len(s22) == 22 && s22.b[22] == 0);
    CHECK(mstr_state(&s23) == MSTR_STATIC && mstr_len(s23) == 23);

    l = region_of(LONG40, 40);
    CHECK(mstr_state(&l) == MSTR_REGION && mstr_len(l) == 40);
    CHECK(mstr_depth_(&l) == g_depth && mstr_serial_(&l) == g_serial);
    CHECK((l.b[23] & 3) == MSTR_REGION && (l.b[23] >> 7) == 0);
    CHECK(mstr_ptr(&l) == LONG40);
}

static void test_embedded_nul(void)
{
    mstr a = S("ab\0cd");
    CHECK(mstr_len(a) == 5);
    CHECK(mstr_ptr(&a)[2] == '\0' && mstr_ptr(&a)[3] == 'c');
    CHECK(mstr_eq(a, mstr_from_n("ab\0cd", 5)));
    CHECK(!mstr_eq(a, S("ab")));
    CHECK(mstr_cmp(a, S("ab")) > 0);
    CHECK(mstr_find(a, S("cd")) == 3);
    CHECK(mstr_find(a, mstr_from_n("\0c", 2)) == 2);
    mstr sl = mstr_slice(a, 1, 4);
    CHECK(mstr_len(sl) == 3 && mstr_ptr(&sl)[1] == '\0');
}

static void test_queries(void)
{
    mstr h = S("the quick brown fox jumps over the lazy dog"); /* 43, STATIC */
    CHECK(mstr_eq(S("abc"), S("abc")) && !mstr_eq(S("abc"), S("abd")) && !mstr_eq(S("abc"), S("ab")));
    CHECK(mstr_cmp(S("a"), S("b")) == -1 && mstr_cmp(S("b"), S("a")) == 1 && mstr_cmp(S("a"), S("a")) == 0);
    CHECK(mstr_cmp(S("a"), S("ab")) == -1 && mstr_cmp(S(""), S("")) == 0);
    CHECK(mstr_cmp(mstr_from_n("\xff", 1), S("a")) == 1); /* unsigned compare */

    CHECK(mstr_find(h, S("the")) == 0);
    CHECK(mstr_rfind(h, S("the")) == 31);
    CHECK(mstr_find(h, S("dog")) == 40 && mstr_rfind(h, S("dog")) == 40);
    CHECK(mstr_find(h, S("cat")) == MSTR_NPOS && mstr_rfind(h, S("cat")) == MSTR_NPOS);
    CHECK(mstr_find(h, S("")) == 0 && mstr_rfind(h, S("")) == mstr_len(h));
    CHECK(mstr_find(S("ab"), S("abc")) == MSTR_NPOS);
    CHECK(mstr_find(S("aaab"), S("aab")) == 1); /* false first-byte hits */
    CHECK(mstr_contains(h, S("fox")) && !mstr_contains(h, S("Fox")));
    CHECK(mstr_starts_with(h, S("the q")) && !mstr_starts_with(h, S("The")));
    CHECK(mstr_ends_with(h, S("lazy dog")) && !mstr_ends_with(h, S("lazy cat")));
    CHECK(mstr_starts_with(S("a"), S("")) && mstr_ends_with(S("a"), S("")));
    CHECK(!mstr_starts_with(S("a"), S("ab")) && !mstr_ends_with(S("a"), S("ba")));
    CHECK(mstr_at(S("xyz"), 1) == 'y');
}

static void test_slice(void)
{
    mstr sso = S("hello world");
    mstr r = mstr_slice(sso, 6, 11);
    CHECK(mstr_state(&r) == MSTR_SSO && mstr_eq(r, S("world")));

    mstr l = region_of(LONG40, 40);
    r = mstr_slice(l, 5, 10); /* long -> short: demoted */
    CHECK(mstr_state(&r) == MSTR_SSO && mstr_len(r) == 5 && memcmp(mstr_ptr(&r), LONG40 + 5, 5) == 0);
    r = mstr_slice(l, 0, 22);
    CHECK(mstr_state(&r) == MSTR_SSO && mstr_len(r) == 22 && r.b[22] == 0);
    r = mstr_slice(l, 0, 23); /* boundary: first VIEW */
    CHECK(mstr_state(&r) == MSTR_VIEW && mstr_len(r) == 23 && mstr_ptr(&r) == LONG40);
    CHECK(mstr_depth_(&r) == g_depth && mstr_serial_(&r) == g_serial);
    mstr v2 = mstr_slice(r, 1, 23); /* view of view: 22 -> SSO */
    CHECK(mstr_state(&v2) == MSTR_SSO && mstr_len(v2) == 22);
    mstr v3 = mstr_slice(l, 4, 40);
    CHECK(mstr_state(&v3) == MSTR_VIEW && mstr_ptr(&v3) == LONG40 + 4);
    mstr v4 = mstr_slice(v3, 2, 36); /* view of view, zero-copy */
    CHECK(mstr_state(&v4) == MSTR_VIEW && mstr_ptr(&v4) == LONG40 + 6 && mstr_len(v4) == 34);

    mstr st = S("0123456789abcdefghijklmnopqrstuvwxyz"); /* STATIC, 36 */
    mstr sv = mstr_slice(st, 2, 30);
    CHECK(mstr_state(&sv) == MSTR_VIEW && mstr_serial_(&sv) == 0 && mstr_depth_(&sv) == 0);
    CHECK(mstr_len(mstr_slice(st, 3, 3)) == 0 && mstr_state(&(mstr){{0}}) == MSTR_SSO);
}

static void test_trim(void)
{
    CHECK(mstr_eq(mstr_trim(S("  \t hi \r\n")), S("hi")));
    CHECK(mstr_eq(mstr_ltrim(S("  hi  ")), S("hi  ")));
    CHECK(mstr_eq(mstr_rtrim(S("  hi  ")), S("  hi")));
    CHECK(mstr_is_empty(mstr_trim(S("   \t\n"))) && mstr_is_empty(mstr_trim(S(""))));
    CHECK(mstr_eq(mstr_trim(S("\v\fx\f\v")), S("x")));

    char buf[64];
    memset(buf, ' ', sizeof buf);
    memcpy(buf + 5, LONG40, 40);
    mstr l = region_of(buf, 50);
    mstr t = mstr_trim(l); /* 40 bytes: stays a zero-copy VIEW */
    CHECK(mstr_state(&t) == MSTR_VIEW && mstr_ptr(&t) == buf + 5 && mstr_len(t) == 40);
    mstr lt = mstr_ltrim(l);
    CHECK(mstr_state(&lt) == MSTR_VIEW && mstr_len(lt) == 45);
    memset(buf, ' ', sizeof buf);
    memcpy(buf + 20, "abc", 3);
    t = mstr_trim(region_of(buf, 60)); /* long -> 3 bytes: demoted */
    CHECK(mstr_state(&t) == MSTR_SSO && mstr_eq(t, S("abc")));
    memset(buf, ' ', sizeof buf);
    t = mstr_trim(region_of(buf, 60)); /* all-whitespace long -> empty SSO */
    CHECK(mstr_state(&t) == MSTR_SSO && mstr_len(t) == 0);
}

/* xorshift for deterministic property tests */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_properties(void)
{
    static char buf[300];
    int iter;
    for (iter = 0; iter < 20000; iter++) {
        size_t n = (size_t)(rnd() % 200), i;
        for (i = 0; i < n; i++) { /* heavy on whitespace and NUL */
            uint64_t r = rnd() % 6;
            buf[i] = r == 0 ? ' ' : r == 1 ? '\0' : r == 2 ? '\t' : (char)('a' + rnd() % 26);
        }
        buf[n] = 0;
        mstr s = n > 22 ? region_of(buf, n) : mstr_make_sso_(buf, n);
        size_t b = n ? (size_t)(rnd() % (n + 1)) : 0, e = b + (n - b ? (size_t)(rnd() % (n - b + 1)) : 0);
        mstr r[4];
        int k;
        r[0] = mstr_slice(s, b, e);
        r[1] = mstr_trim(s);
        r[2] = mstr_ltrim(r[0]);
        r[3] = mstr_rtrim(mstr_slice(s, 0, e));
        CHECK(mstr_len(r[0]) == e - b && memcmp(mstr_ptr(&r[0]), buf + b, e - b) == 0);
        for (k = 0; k < 4; k++) {
            size_t len = mstr_len(r[k]);
            if (len <= MSTR_SSO_MAX) {
                CHECK(mstr_state(&r[k]) == MSTR_SSO);             /* Law of Small Results */
                CHECK(r[k].b[len] == 0);                          /* NUL-terminated */
            } else {
                CHECK(mstr_state(&r[k]) == MSTR_VIEW);
                CHECK(mstr_ptr(&r[k]) >= buf && mstr_ptr(&r[k]) + len <= buf + n); /* not into an SSO */
            }
        }
        /* trim agrees with a naive reference */
        size_t tb = 0, te = n;
        while (tb < te && (buf[tb] == ' ' || (buf[tb] >= '\t' && buf[tb] <= '\r'))) tb++;
        while (te > tb && (buf[te - 1] == ' ' || (buf[te - 1] >= '\t' && buf[te - 1] <= '\r'))) te--;
        CHECK(mstr_len(r[1]) == te - tb && memcmp(mstr_ptr(&r[1]), buf + tb, te - tb) == 0);
        /* find agrees with a naive reference */
        if (n >= 2) {
            size_t nb = (size_t)(rnd() % (n - 1)), nl = 1 + (size_t)(rnd() % 3), ref = MSTR_NPOS, last = MSTR_NPOS, j;
            if (nb + nl > n) nl = n - nb;
            mstr nd = mstr_slice(s, nb, nb + nl);
            for (j = 0; j + nl <= n; j++)
                if (memcmp(buf + j, mstr_ptr(&nd), nl) == 0) { if (ref == MSTR_NPOS) ref = j; last = j; }
            CHECK(mstr_find(s, nd) == ref && mstr_rfind(s, nd) == last);
        }
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
        freopen("/dev/null", "w", stderr);
        if (which == 0) (void)mstr_slice(S("abc"), 2, 9);
        else if (which == 1) (void)mstr_slice(S("abc"), 2, 1);
        else (void)mstr_at(S("abc"), 3);
        _exit(0);
    }
    waitpid(p, &st, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}
#endif

int main(void)
{
    mstr_scope {
    mstr real = mstr_from_n(LONG40, 40);
    g_depth = mstr_depth_(&real);
    g_serial = mstr_serial_(&real);
    test_layout();
    test_embedded_nul();
    test_queries();
    test_slice();
    test_trim();
    test_properties();
#ifndef _WIN32
    CHECK(dies(0)); CHECK(dies(1)); CHECK(dies(2));
#endif
    }
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail != 0;
}
