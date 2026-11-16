/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
#include "mstr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#define MSTR_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MSTR_TEST_ASAN 1
#endif
#endif

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

static void test_box_basic(void)
{
    mstr_scope {
        mstr_box z;
        memset(&z, 0, sizeof z); /* zeroed box is a valid empty box */
        CHECK(mstr_is_empty(mstr_unbox(&z)));
        mstr_box_free(&z);
        mstr_box_free(&z);

        mstr_box sb = mstr_own(S("short text"));
        CHECK(sb.ptr == NULL && sb.len == 10); /* no allocation for <= 22 bytes */
        mstr su = mstr_unbox(&sb);
        CHECK(mstr_state(&su) == MSTR_SSO && mstr_eq(su, S("short text")));
        mstr_box_free(&sb);

        mstr src = big(60, 'b');
        mstr_box hb = mstr_own(src);
        CHECK(hb.ptr != NULL && hb.len == 60 && hb.ptr[60] == '\0');
        CHECK(hb.ptr != mstr_ptr(&src)); /* a real copy */
        mstr hu = mstr_unbox(&hb);
        CHECK(mstr_state(&hu) == MSTR_VIEW && mstr_len(hu) == 60 && mstr_ptr(&hu) == hb.ptr);
        CHECK(mstr_eq(hu, src));
        /* a VIEW ending at the box's NUL needs no copy for C interop */
        CHECK(MSTR_CSTR(hu) == hb.ptr);
        /* slices follow the Law of Small Results */
        mstr part = mstr_slice(hu, 10, 50);
        CHECK(mstr_state(&part) == MSTR_VIEW && mstr_ptr(&part) == hb.ptr + 10);
        part = mstr_slice(hu, 10, 20);
        CHECK(mstr_state(&part) == MSTR_SSO);
        /* cat copies into the scope: independent of the box */
        mstr cat = mstr_cat(hu, S("!"));
        CHECK(mstr_state(&cat) == MSTR_REGION && mstr_ptr(&cat) != hb.ptr);

        mstr_box_free(&hb);
        CHECK(hb.ptr == NULL && hb.len == 0);
        mstr_box_free(&hb); /* same object twice: idempotent no-op */
        CHECK(mstr_len(src) == 60 && mstr_len(cat) == 61); /* owner independence */
    }
}

static void test_box_escapes_scope(void)
{
    mstr_box keep;
    memset(&keep, 0, sizeof keep);
    mstr_scope {
        mstr tmp = mstr_cat(big(40, 'x'), S("-tail"));
        keep = mstr_own(tmp);
    }
    { /* the scope is gone; the box and views of it are still valid */
        mstr v = mstr_unbox(&keep);
        CHECK(mstr_len(v) == 45 && mstr_ptr(&v)[0] == 'x' && mstr_eq(mstr_slice(v, 40, 45), S("-tail")));
        mstr_scope { CHECK(mstr_len(v) == 45); }
    }
    mstr_box_free(&keep);

    { /* embedded NUL round-trips */
        mstr nul = mstr_cat(big(30, 'n'), mstr_from_n("\0\0z", 3));
        mstr_box b = mstr_own(nul);
        mstr back = mstr_unbox(&b);
        CHECK(mstr_len(back) == 33 && mstr_eq(back, nul) && mstr_ptr(&back)[31] == '\0');
        mstr_box_free(&b);
    }
#ifndef MSTR_STRICT
    { /* the cleanup attribute frees at scope exit */
        mstr_box_auto a = mstr_own(big(50, 'c'));
        CHECK(a.kind == 2);
    }
#endif
}

#if defined(MSTR_CHECKED) && !defined(MSTR_TEST_ASAN)
/* Concrete demonstration of the limit discussed in docs/DESIGN.md: a VIEW into
 * a box carries no scope serial, so use-after-box-free is invisible to the
 * serial check. Checked mode instead poisons the freed bytes (and keeps the
 * buffer in quarantine) so stale views read 0xDD, never silently-valid data. */
static void test_stale_view_reads_poison(void)
{
    mstr_box b = mstr_own(S("a long enough string to need a heap buffer.."));
    mstr v = mstr_unbox(&b);
    unsigned char first = (unsigned char)mstr_ptr(&v)[0];
    CHECK(first == 'a');
    mstr_box_free(&b);
    CHECK((unsigned char)mstr_ptr(&v)[0] == 0xDD && (unsigned char)mstr_ptr(&v)[mstr_len(v) - 1] == 0xDD);
}
#endif

static void test_share_basic(void)
{
    mstr_shared *keep = NULL, *tiny = NULL;
    mstr_scope {
        mstr src = big(70, 's');
        keep = mstr_share(src);
        tiny = mstr_share(S("tiny"));
    }
    { /* independent of the source scope */
        mstr v = mstr_share_get(keep), v2 = mstr_share_get(keep);
        CHECK(mstr_state(&v) == MSTR_VIEW && mstr_len(v) == 70 && mstr_ptr(&v) == mstr_ptr(&v2));
        CHECK(mstr_ptr(&v)[0] == 's' && mstr_ptr(&v)[69] == 's' && mstr_ptr(&v)[70] == '\0');
        CHECK(mstr_share_refs_(keep) == 1);
        CHECK(mstr_share_retain(keep) == keep && mstr_share_refs_(keep) == 2);
        mstr_share_release(keep);
        CHECK(mstr_share_refs_(keep) == 1);
        mstr_scope { CHECK(mstr_len(mstr_share_get(keep)) == 70); } /* a VIEW with no scope tag survives scopes */
    }
    {
        mstr t = mstr_share_get(tiny);
        CHECK(mstr_state(&t) == MSTR_SSO && mstr_eq(t, S("tiny")));
    }
    mstr_share_release(keep);
    mstr_share_release(tiny);
}

int main(void)
{
    test_box_basic();
    test_box_escapes_scope();
#if defined(MSTR_CHECKED) && !defined(MSTR_TEST_ASAN)
    test_stale_view_reads_poison();
#endif
    test_share_basic();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail != 0;
}
