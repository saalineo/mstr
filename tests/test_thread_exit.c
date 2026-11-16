/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* Each thread has its own arena; it is freed at thread exit (LSan-visible). */
#include "mstr.h"

#include <pthread.h>
#include <stdio.h>

static void *worker(void *arg)
{
    int i, ok = 1;
    (void)arg;
    for (i = 0; i < 200; i++) mstr_scope {
        mstr big = mstr_repeat(S("0123456789"), 20000); /* 200 KB: dedicated chunk */
        ok = ok && mstr_len(big) == 200000;
    }
    {
        mstr keep = mstr_repeat(S("xy"), 40); /* left in the root scope on purpose */
        ok = ok && mstr_len(keep) == 80;
    }
    return ok ? (void *)0 : (void *)1;
}

int main(void)
{
    pthread_t th[4];
    int i, bad = 0;
    void *rv;
    for (i = 0; i < 4; i++) pthread_create(&th[i], 0, worker, 0);
    for (i = 0; i < 4; i++) { pthread_join(th[i], &rv); bad |= rv != 0; }
    printf("thread exit test %s\n", bad ? "FAILED" : "ok");
    return bad;
}
