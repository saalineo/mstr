/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* Checked-mode death tests. Built against the MSTR_CHECKED library variant.
 * Each case runs in a forked child; we require SIGABRT and a useful message. */
#ifndef MSTR_CHECKED
#error "test_death.c must be built with MSTR_CHECKED"
#endif
#define _POSIX_C_SOURCE 200809L
#include "mstr.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail, g_run;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        g_run++;                                                                         \
        if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static mstr big(size_t n, char ch)
{
    char buf[256];
    memset(buf, ch, n);
    return mstr_from_n(buf, n);
}

typedef void (*body_fn)(void);

static void expect_death(const char *name, body_fn f, const char *m1, const char *m2)
{
    int fd[2], st = 0, ok;
    char out[4096];
    size_t n = 0;
    ssize_t r;
    pid_t pid;
    if (pipe(fd) != 0) { perror("pipe"); exit(2); }
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        dup2(fd[1], 2);
        close(fd[0]);
        close(fd[1]);
        f();
        _exit(0); /* survived: the check did not fire */
    }
    close(fd[1]);
    while (n < sizeof out - 1 && (r = read(fd[0], out + n, sizeof out - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0;
    close(fd[0]);
    waitpid(pid, &st, 0);
    ok = WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT && strstr(out, m1) && (!m2 || strstr(out, m2));
    g_run++;
    if (!ok) {
        g_fail++;
        fprintf(stderr, "FAIL death test '%s' (status %d)\n--- child stderr ---\n%s--------------------\n", name, st, out);
    }
}

/* ---- the violations ---- */
static void d_use_after_scope(void)
{
    mstr leaked = S("");
    mstr_scope { leaked = big(50, 'a'); }
    (void)mstr_len(leaked);
}
static void d_use_after_scope_view(void)
{
    mstr view = S("");
    mstr_scope { view = mstr_slice(big(60, 'a'), 5, 50); }
    (void)mstr_eq(view, S("x"));
}
static void d_same_depth_reused(void)
{ /* a new scope at the same depth must not resurrect the old string */
    mstr old = S("");
    mstr_scope { old = big(50, 'a'); }
    mstr_scope { (void)big(50, 'b'); (void)mstr_ptr(&old); }
}
static void d_use_after_promote(void)
{
    mstr_scope_t s = mstr_scope_enter();
    mstr r = big(50, 'p');
    mstr p = mstr_promote(&s, r);
    (void)p;
    (void)mstr_slice(r, 0, 30); /* r's bytes were moved; r is dead */
}
static void d_nested_use_after_inner(void)
{
    mstr_scope {
        mstr inner_val = S("");
        mstr_scope { inner_val = mstr_cat(big(30, 'i'), big(30, 'j')); }
        (void)mstr_cat(inner_val, S("x"));
    }
}

static mstr g_foreign;
static void *producer(void *arg)
{
    (void)arg;
    mstr_scope_t s = mstr_scope_enter();
    g_foreign = big(50, 't');
    (void)s; /* never left: the thread's arena is freed at thread exit */
    return 0;
}
static void d_cross_thread(void)
{
    pthread_t th;
    pthread_create(&th, 0, producer, 0);
    pthread_join(th, 0);
    (void)mstr_len(g_foreign);
}

static mstr_scope_t g_scope;
static void *leaver(void *arg)
{
    (void)arg;
    mstr_scope_leave(&g_scope);
    return 0;
}
static void d_leave_on_wrong_thread(void)
{
    pthread_t th;
    g_scope = mstr_scope_enter();
    pthread_create(&th, 0, leaver, 0);
    pthread_join(th, 0);
}

static void d_box_double_free_copy(void)
{ /* boxes are bit-copyable: freeing a stale copy must be caught */
    mstr_box a = mstr_own(big(50, 'a'));
    mstr_box b = a;
    mstr_box_free(&a);
    mstr_box_free(&b);
}
static void d_box_use_after_free(void)
{
    mstr_box a = mstr_own(big(50, 'a'));
    mstr_box_free(&a);
    (void)mstr_unbox(&a);
}
static void d_oob_slice(void) { mstr_scope { (void)mstr_slice(big(10, 'a'), 2, 99); } }
static void d_inverted_slice(void) { (void)mstr_slice(S("abcdef"), 4, 2); }
static void d_oob_at(void) { (void)mstr_at(S("abc"), 3); }
static void d_len_overflow(void) { (void)mstr_repeat(S("ab"), ((size_t)-1) / 2 + 1); }
static void d_promote_with_open_inner(void)
{
    mstr_scope_t outer = mstr_scope_enter();
    mstr_scope_t inner = mstr_scope_enter();
    (void)inner;
    (void)mstr_promote(&outer, S("x"));
}

/* ---- things that must NOT die ---- */
static void test_valid_uses(void)
{
    mstr kept = S("");
    mstr stat_view = mstr_slice(S("a static literal that is longer than twenty-two"), 3, 40);
    { /* promoted into the ROOT scope, so it outlives every scope below */
        mstr_scope_t s = mstr_scope_enter();
        mstr r = mstr_cat(big(40, 'k'), S("!"));
        kept = mstr_scope_return(&s, r);
        CHECK(mstr_len(kept) == 41);
    }
    mstr_scope { CHECK(mstr_len(stat_view) == 37 && mstr_ptr(&stat_view)[0] == 't'); }
    /* kept lived in the (root) parent: still valid after other scopes come and go */
    CHECK(mstr_len(kept) == 41);
    mstr_scope { mstr_scope { CHECK(mstr_len(kept) == 41); } }
    CHECK(mstr_len(stat_view) == 37); /* VIEW into STATIC data never expires */
}

int main(void)
{
    setenv("MSTR_NO_ROOT_WARNING", "1", 1);
    test_valid_uses();
    expect_death("use after scope end", d_use_after_scope, "used after it ended", "test_death.c:");
    expect_death("view used after scope end", d_use_after_scope_view, "used after it ended", "test_death.c:");
    expect_death("same depth reused", d_same_depth_reused, "used after it ended", "test_death.c:");
    expect_death("use after promote source rewind", d_use_after_promote, "used after it ended", "test_death.c:");
    expect_death("nested inner value used in outer", d_nested_use_after_inner, "used after it ended", "test_death.c:");
    expect_death("cross-thread use", d_cross_thread, "different thread", NULL);
    expect_death("leave on wrong thread", d_leave_on_wrong_thread, "not live on this thread", "test_death.c:");
    expect_death("slice out of bounds", d_oob_slice, "out of bounds", "test_death.c:");
    expect_death("slice begin > end", d_inverted_slice, "out of bounds", "test_death.c:");
    expect_death("at out of bounds", d_oob_at, "out of bounds", "test_death.c:");
    expect_death("length overflow", d_len_overflow, "overflow", NULL);
    expect_death("promote with open inner scope", d_promote_with_open_inner, "unclosed inner scopes", "test_death.c:");
    expect_death("box double free through a copy", d_box_double_free_copy, "double free", NULL);
    expect_death("box used after free", d_box_use_after_free, "already freed", NULL);
    printf("%d death/validity checks, %d failed\n", g_run, g_fail);
    return g_fail != 0;
}
