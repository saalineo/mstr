/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* Replaces SDS's own sdsalloc.h so the benchmark can count allocations. */
#ifndef BENCH_SDSALLOC_H
#define BENCH_SDSALLOC_H
#include <stddef.h>
void *bench_malloc(size_t n);
void *bench_realloc(void *p, size_t n);
void bench_free(void *p);
#define s_malloc bench_malloc
#define s_realloc bench_realloc
#define s_free bench_free
#endif
