/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* Stress mstr_share refcounting and reads from many threads. Meant for TSan. */
#include "mstr.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* usage: test_share_threads [threads<=16] [rounds]
 * With 2 threads TSan's shadow history is small enough that a too-weak memory
 * order in mstr_share_release is reliably flagged (checked by mutation). With
 * many threads the history saturates, so that run only stresses the refcount. */
#define MAX_THREADS 16
static int g_rounds = 20000;

static unsigned long expect_sum;

/* A free hook that WRITES to the block before releasing it. Under TSan this
 * makes the last release race with any reader that was not ordered before it,
 * so a too-weak memory order in mstr_share_release is detected (a plain free()
 * is not reliably flagged). Only the first 64 bytes are touched: header + data. */
static void *h_alloc(size_t n, void *ctx) { (void)ctx; return malloc(n); }
static void h_free(void *p, void *ctx)
{
    (void)ctx;
    memset(p, 0xDD, 64);
    free(p);
}

static void *reader(void *arg)
{
    mstr_shared *mine = (mstr_shared *)arg; /* the thread owns one reference */
    unsigned long bad = 0;
    int i;
    for (i = 0; i < g_rounds; i++) {
        mstr_shared *extra = mstr_share_retain(mine);
        mstr v = mstr_share_get(extra);
        unsigned long sum = 0;
        size_t k, n = mstr_len(v);
        for (k = 0; k < n; k++) sum += (unsigned char)mstr_ptr(&v)[k];
        bad += sum != expect_sum;
        mstr_share_release(extra);
    }
    mstr_share_release(mine);
    return (void *)bad;
}

int main(int argc, char **argv)
{
    pthread_t th[MAX_THREADS];
    int threads = argc > 1 ? atoi(argv[1]) : 8;
    mstr_shared *sh;
    unsigned long bad = 0;
    int i;
    mstr_allocator al;
    if (argc > 2) g_rounds = atoi(argv[2]);
    if (threads < 1 || threads > MAX_THREADS) return 2;
    al.alloc = h_alloc; al.realloc = NULL; al.free = h_free; al.ctx = NULL;
    mstr_set_allocator(al);
    mstr_scope {
        mstr big = mstr_repeat(S("0123456789abcdef"), 64); /* 1024 bytes */
        size_t k;
        for (k = 0; k < mstr_len(big); k++) expect_sum += (unsigned char)mstr_ptr(&big)[k];
        sh = mstr_share(big);
    }
    for (i = 0; i < threads; i++) pthread_create(&th[i], 0, reader, mstr_share_retain(sh));
    mstr_share_release(sh); /* main drops its own reference while readers run */
    for (i = 0; i < threads; i++) {
        void *r;
        pthread_join(th[i], &r);
        bad += (unsigned long)r;
    }
    printf("share stress %s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
