/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* Allocator hooks and out-of-memory handling, plus a no-leak proof: with a
 * counting allocator every allocation made by scopes, boxes, shares and
 * threads must be freed again. POSIX only (fork). */
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

/* ---------- allocators ---------- */
typedef struct { unsigned long allocs, frees, grows, left; } ctr; /* left: allocations still allowed */

static void *c_alloc(size_t n, void *ctx)
{
    ctr *c = (ctr *)ctx;
    if (c->left != (unsigned long)-1) {
        if (c->left == 0) return NULL;
        c->left--;
    }
    __atomic_fetch_add(&c->allocs, 1, __ATOMIC_RELAXED);
    return malloc(n);
}
static void c_free(void *p, void *ctx) { __atomic_fetch_add(&((ctr *)ctx)->frees, 1, __ATOMIC_RELAXED); free(p); }
static void *c_realloc(void *p, size_t n, void *ctx)
{
    __atomic_fetch_add(&((ctr *)ctx)->grows, 1, __ATOMIC_RELAXED);
    return realloc(p, n);
}

/* ---------- forked failure cases (the parent must not have used mstr yet) ---------- */
static ctr g_child_ctr;
static void use_allocator(unsigned long allow, int with_realloc)
{
    mstr_allocator a;
    g_child_ctr.left = allow;
    a.alloc = c_alloc; a.realloc = with_realloc ? c_realloc : NULL; a.free = c_free; a.ctx = &g_child_ctr;
    mstr_set_allocator(a);
}
static void handler_exit42(size_t n, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "HANDLER n=%lu\n", (unsigned long)n);
    _exit(42);
}
static void handler_returns(size_t n, void *ctx) { (void)n; (void)ctx; }

static void ch_oom_default(void) { use_allocator(0, 0); (void)mstr_scope_enter(); }
static void ch_oom_handler(void) { use_allocator(0, 0); mstr_set_oom_handler(handler_exit42, NULL); (void)mstr_scope_enter(); }
static void ch_oom_returns(void) { use_allocator(0, 0); mstr_set_oom_handler(handler_returns, NULL); (void)mstr_scope_enter(); }
static void ch_oom_midway(void)
{ /* the 3rd allocation fails: frames + first chunk succeed, then a 300 KB string needs more */
    use_allocator(2, 0);
    mstr_set_oom_handler(handler_exit42, NULL);
    mstr_scope { (void)mstr_repeat(S("0123456789"), 30000); (void)mstr_repeat(S("0123456789"), 30000); }
}
static void ch_set_late(void)
{
    mstr_scope { (void)mstr_repeat(S("0123456789"), 5); }
    use_allocator((unsigned long)-1, 0);
}
static void ch_set_oom_late(void)
{
    mstr_scope { (void)mstr_repeat(S("0123456789"), 5); }
    mstr_set_oom_handler(handler_exit42, NULL);
}
static void ch_null_alloc(void)
{
    mstr_allocator a;
    memset(&a, 0, sizeof a);
    mstr_set_allocator(a);
}

/* Frame-table growth through a NULL realloc hook (alloc + copy + free) must not leak. */
static int nest(int d, size_t *acc);
static void ch_no_realloc(void)
{
    size_t acc = 0;
    unsigned long live;
    use_allocator((unsigned long)-1, 0);
    if (nest(40, &acc) != 1 || acc != 40 * 50) _exit(3);
    mstr_thread_cleanup();
    live = g_child_ctr.allocs - g_child_ctr.frees;
    fprintf(stderr, "NOREALLOC allocs=%lu frees=%lu grows=%lu\n", g_child_ctr.allocs, g_child_ctr.frees, g_child_ctr.grows);
    _exit(live == 0 && g_child_ctr.grows == 0 && g_child_ctr.allocs >= 3 ? 0 : 4);
}

typedef void (*fn)(void);
/* want_exit < 0: expect SIGABRT. */
static void child(const char *name, fn f, int want_exit, const char *needle)
{
    int fd[2], st = 0, ok;
    char out[2048];
    size_t n = 0;
    ssize_t r;
    pid_t pid;
    if (pipe(fd) != 0) exit(2);
    fflush(NULL);
    pid = fork();
    if (pid == 0) { dup2(fd[1], 2); close(fd[0]); close(fd[1]); f(); _exit(0); }
    close(fd[1]);
    while (n < sizeof out - 1 && (r = read(fd[0], out + n, sizeof out - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0;
    close(fd[0]);
    waitpid(pid, &st, 0);
    if (want_exit < 0) ok = WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
    else ok = WIFEXITED(st) && WEXITSTATUS(st) == want_exit;
    ok = ok && strstr(out, needle) != NULL;
    g_run++;
    if (!ok) { g_fail++; fprintf(stderr, "FAIL child '%s' (status %d)\n%s\n", name, st, out); }
}

/* ---------- leak proof workload ---------- */
static int nest(int d, size_t *acc)
{
    int r = 0;
    if (d == 0) return 1;
    mstr_scope {
        mstr s = mstr_repeat(S("0123456789"), 5);
        *acc += mstr_len(s);
        r = nest(d - 1, acc);
    }
    return r;
}

static void *worker(void *arg)
{
    mstr_shared *sh = (mstr_shared *)arg;
    int i;
    for (i = 0; i < 50; i++) mstr_scope {
        mstr big = mstr_repeat(mstr_share_get(sh), 300);
        mstr_box b = mstr_own(big);
        mstr_box_free(&b);
    }
    mstr_share_release(sh);
    (void)mstr_repeat(S("kept in this thread's root scope until it exits"), 3);
    return 0;
}

static void workload(void)
{
    size_t acc = 0;
    int i;
    mstr_shared *sh;
    CHECK(nest(40, &acc) == 1 && acc == 40 * 50); /* > 16 frames: forces the frame table to grow */
    for (i = 0; i < 20; i++) mstr_scope {
        mstr huge = mstr_repeat(S("0123456789"), 40000); /* 400 KB: dedicated chunk */
        mstr_scope_t s = mstr_scope_enter();
        mstr mid = mstr_cat(huge, S("x"));
        mstr up = mstr_scope_return(&s, mstr_upper(mid));
        CHECK(mstr_len(up) == 400001);
    }
    {
        mstr_box a = mstr_own(mstr_repeat(S("ab"), 100)), c = mstr_own(S("tiny"));
        mstr_box_free(&a); mstr_box_free(&a); mstr_box_free(&c);
        { mstr_box_auto d = mstr_own(mstr_repeat(S("cd"), 50)); CHECK(d.kind == 2); }
    }
    sh = mstr_share(mstr_repeat(S("shared-bytes!"), 4));
    {
        pthread_t th[4];
        for (i = 0; i < 4; i++) pthread_create(&th[i], 0, worker, mstr_share_retain(sh));
        for (i = 0; i < 4; i++) pthread_join(th[i], 0);
    }
    CHECK(mstr_share_refs_(sh) == 1);
    mstr_share_release(sh);
}

int main(void)
{
    ctr c;
    mstr_allocator a;
    unsigned long live;

    /* 1. failure and misuse cases, each in a pristine child */
    child("oom default aborts", ch_oom_default, -1, "out of memory");
    child("oom handler is called", ch_oom_handler, 42, "HANDLER n=");
    child("oom handler that returns aborts", ch_oom_returns, -1, "handler returned");
    child("oom after some allocations", ch_oom_midway, 42, "HANDLER n=");
    child("set_allocator after first use", ch_set_late, -1, "before first use");
    child("set_oom_handler after first use", ch_set_oom_late, -1, "before first use");
    child("set_allocator without alloc/free", ch_null_alloc, -1, "required");
    child("NULL realloc hook, no leaks", ch_no_realloc, 0, "NOREALLOC");

    /* 2. everything allocated is freed (with the realloc hook; the NULL-realloc path is the child above) */
    memset(&c, 0, sizeof c);
    c.left = (unsigned long)-1;
    a.alloc = c_alloc; a.realloc = c_realloc; a.free = c_free; a.ctx = &c;
    mstr_set_allocator(a);
    workload();
    mstr_thread_cleanup();
    live = c.allocs - c.frees;
    printf("allocs=%lu frees=%lu grows=%lu live=%lu\n", c.allocs, c.frees, c.grows, live);
    CHECK(c.allocs > 100 && live == 0 && c.grows >= 1);
    printf("%d checks, %d failed\n", g_run, g_fail);

    return g_fail != 0;
}
