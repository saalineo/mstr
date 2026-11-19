/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* Allocation-count and throughput benchmark: mstr vs SDS vs hand-written malloc code.
 *
 * W1  "normalise": trim + upper + replace(' ' -> '_') on a ~110-byte padded line
 * W2  "csv":       split a ~230-byte, 20-field line; sum the trimmed field lengths
 *
 * Every implementation is first checked to produce the SAME bytes. Then, per
 * implementation: malloc calls and bytes per op (counted at the allocator), arena
 * bumps/bytes per op (mstr only; reclaimed by the scope in O(1), no free call),
 * arena bytes still held when the chain finishes (mstr only) and ns/op (best of 5).
 *
 * Build: see bench/CMakeLists.txt (needs the library compiled with -DMSTR_STATS). */
#define _POSIX_C_SOURCE 200809L
#include "mstr.h"
#include "sds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

/* ---------- counting allocator, shared by hand-written code, SDS and mstr ---------- */
static unsigned long long g_mallocs, g_frees, g_bytes;
void *bench_malloc(size_t n) { g_mallocs++; g_bytes += n; return malloc(n); }
void *bench_realloc(void *p, size_t n) { g_mallocs++; g_bytes += n; return realloc(p, n); }
void bench_free(void *p) { if (p) g_frees++; free(p); }
static void *m_alloc(size_t n, void *c) { (void)c; return bench_malloc(n); }
static void *m_realloc(void *p, size_t n, void *c) { (void)c; return bench_realloc(p, n); }
static void m_free(void *p, void *c) { (void)c; bench_free(p); }

static volatile size_t g_sink;
static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ================= W1: normalise ================= */
static const char *W1_IN =
    "   the quick brown fox jumps over the lazy dog and keeps running through the quiet forest until night falls   ";

/* Same semantics as mstr_upper (ASCII only), inlined, so the hand-written baselines
 * are not slowed down by a per-character libc call that mstr does not make. */
static inline char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static inline int ws(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

/* hand-written, step by step: every step returns a fresh malloc'd string */
static char *h_trim(const char *s)
{
    size_t b = 0, e = strlen(s), n;
    char *o;
    while (s[b] && ws(s[b])) b++;
    while (e > b && ws(s[e - 1])) e--;
    n = e - b;
    o = (char *)bench_malloc(n + 1);
    memcpy(o, s + b, n);
    o[n] = 0;
    return o;
}
static char *h_upper(const char *s)
{
    size_t n = strlen(s), i;
    char *o = (char *)bench_malloc(n + 1);
    for (i = 0; i <= n; i++) o[i] = up(s[i]);
    return o;
}
static char *h_repl(const char *s)
{
    size_t n = strlen(s), i;
    char *o = (char *)bench_malloc(n + 1);
    for (i = 0; i <= n; i++) o[i] = s[i] == ' ' ? '_' : s[i];
    return o;
}
static size_t w1_malloc_steps(const char *in, char *out, size_t cap)
{
    char *a = h_trim(in), *b = h_upper(a), *c = h_repl(b);
    size_t n = strlen(c);
    if (out && n < cap) memcpy(out, c, n + 1);
    bench_free(a); bench_free(b); bench_free(c);
    return n;
}
/* hand-written, one fused pass, one allocation */
static size_t w1_malloc_fused(const char *in, char *out, size_t cap)
{
    size_t b = 0, e = strlen(in), n, i;
    char *o;
    while (in[b] && ws(in[b])) b++;
    while (e > b && ws(in[e - 1])) e--;
    n = e - b;
    o = (char *)bench_malloc(n + 1);
    for (i = 0; i < n; i++) { char c = in[b + i]; o[i] = c == ' ' ? '_' : up(c); }
    o[n] = 0;
    if (out && n < cap) memcpy(out, o, n + 1);
    bench_free(o);
    return n;
}
/* SDS: one sdsnew, then everything in place */
static size_t w1_sds(const char *in, char *out, size_t cap)
{
    sds s = sdsnew(in);
    size_t n;
    s = sdstrim(s, " \t\n\v\f\r");
    sdstoupper(s);
    s = sdsmapchars(s, " ", "_", 1);
    n = sdslen(s);
    if (out && n < cap) memcpy(out, s, n + 1);
    sdsfree(s);
    return n;
}
static size_t w1_mstr_nested(const char *in, char *out, size_t cap)
{
    size_t n;
    mstr_scope {
        mstr r = mstr_replace_all(mstr_upper(mstr_trim(mstr_from(in))), S(" "), S("_"));
        n = mstr_len(r);
        if (out && n < cap) memcpy(out, mstr_ptr(&r), n + 1);
    }
    return n;
}
static size_t w1_mstr_pipe(const char *in, char *out, size_t cap)
{
    size_t n;
    mstr_scope {
        mstr r = MSTR_PIPE(mstr_from(in), mstr_trim, mstr_upper, mstr_replace_all(S(" "), S("_")));
        n = mstr_len(r);
        if (out && n < cap) memcpy(out, mstr_ptr(&r), n + 1);
    }
    return n;
}
static size_t w1_mstr_fused(const char *in, char *out, size_t cap)
{
    size_t n;
    mstr_scope {
        mstr r = mstr_trim_case_replace_all(mstr_from(in), MSTR_ASCII_UPPER, S(" "), S("_"));
        n = mstr_len(r);
        if (out && n < cap) memcpy(out, mstr_ptr(&r), n + 1);
    }
    return n;
}
/* mstr bytes still held when the chain is done (what a scope would keep alive) */
static size_t w1_retained(int fused)
{
    size_t held = 0;
    mstr_scope {
        mstr x = mstr_from(W1_IN);
        size_t before = mstr_debug_arena_used_();
        mstr r = fused ? MSTR_PIPE(x, mstr_trim, mstr_upper, mstr_replace_all(S(" "), S("_")))
                       : mstr_replace_all(mstr_upper(mstr_trim(x)), S(" "), S("_"));
        g_sink += mstr_len(r);
        held = mstr_debug_arena_used_() - before;
    }
    return held;
}
static size_t w1_retained_transform(void)
{
    size_t held = 0;
    mstr_scope {
        mstr x = mstr_from(W1_IN);
        size_t before = mstr_debug_arena_used_();
        mstr r = mstr_trim_case_replace_all(x, MSTR_ASCII_UPPER, S(" "), S("_"));
        g_sink += mstr_len(r);
        held = mstr_debug_arena_used_() - before;
    }
    return held;
}

/* ================= W2: csv ================= */
static char g_csv[512];
static void build_csv(void)
{
    static const char *words[] = {"alpha", "be", "gamma-ray", "delta", "epsilon-long", "z", "theta", "iota-kappa", "mu", "nu-xi-omicron"};
    size_t i, pos = 0;
    for (i = 0; i < 20; i++) {
        const char *w = words[(i * 7 + 3) % 10];
        pos += (size_t)sprintf(g_csv + pos, "%s%s%s%s", (i % 3) ? " " : "", w, (i % 4) ? "  " : "", i < 19 ? "," : "");
    }
}
static size_t w2_mstr_each(const char *line)
{
    size_t total = 0;
    mstr_scope {
        mstr l = mstr_from(line);
        mstr_each(f, l, S(",")) { total += mstr_len(mstr_trim(f)); }
    }
    return total;
}
static mstr g_csv_m; /* the same line, already an mstr (e.g. read once, scanned many times) */
static size_t w2_mstr_each_prebuilt(const char *line)
{
    size_t total = 0;
    (void)line;
    mstr_each(f, g_csv_m, S(",")) { total += mstr_len(mstr_trim(f)); }
    return total;
}
static size_t w2_mstr_split(const char *line)
{
    size_t total = 0, n, i;
    mstr_scope {
        mstr *f = mstr_split(mstr_from(line), S(","), &n);
        for (i = 0; i < n; i++) total += mstr_len(mstr_trim(f[i]));
    }
    return total;
}
static size_t w2_sds(const char *line)
{
    int n, i;
    size_t total = 0;
    sds *f = sdssplitlen(line, (ssize_t)strlen(line), ",", 1, &n);
    for (i = 0; i < n; i++) { f[i] = sdstrim(f[i], " \t"); total += sdslen(f[i]); }
    sdsfreesplitres(f, n);
    return total;
}
static size_t w2_malloc_dup(const char *line)
{ /* typical C: array of strdup'd fields, trimmed in place */
    size_t total = 0, n = 1, i = 0;
    const char *p;
    char **f;
    for (p = line; *p; p++) n += *p == ',';
    f = (char **)bench_malloc(n * sizeof *f);
    for (p = line; ; ) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p), b = 0;
        char *d = (char *)bench_malloc(len + 1);
        memcpy(d, p, len);
        d[len] = 0;
        while (len > b && (d[len - 1] == ' ' || d[len - 1] == '\t')) d[--len] = 0;
        while (d[b] == ' ' || d[b] == '\t') b++;
        total += len - b;
        f[i++] = d;
        if (!e) break;
        p = e + 1;
    }
    for (i = 0; i < n; i++) bench_free(f[i]);
    bench_free(f);
    return total;
}
static size_t w2_malloc_scan(const char *line)
{ /* hand-written with pointers only: the best case for plain C */
    size_t total = 0;
    const char *p = line;
    for (;;) {
        const char *e = strchr(p, ','), *b = p, *t;
        t = e ? e : p + strlen(p);
        while (b < t && (*b == ' ' || *b == '\t')) b++;
        while (t > b && (t[-1] == ' ' || t[-1] == '\t')) t--;
        total += (size_t)(t - b);
        if (!e) break;
        p = e + 1;
    }
    return total;
}

/* ================= harness ================= */
typedef size_t (*w1_fn)(const char *, char *, size_t);
typedef size_t (*w2_fn)(const char *);
typedef struct { const char *name; double ns, mallocs, bytes, bumps, bumpbytes, retained; } row;

static row run_w1(const char *name, w1_fn f, int is_mstr, size_t retained)
{
    row r;
    const long iters = 300000;
    double best = 1e30;
    int rep;
    long i;
    unsigned long long m0, b0;
    mstr_stats s0;
    for (i = 0; i < 20000; i++) g_sink += f(W1_IN, 0, 0); /* warm up (incl. arena chunk) */
    for (rep = 0; rep < 5; rep++) {
        double t0 = now_ns(), t1;
        for (i = 0; i < iters; i++) g_sink += f(W1_IN, 0, 0);
        t1 = now_ns();
        if ((t1 - t0) / (double)iters < best) best = (t1 - t0) / (double)iters;
    }
    m0 = g_mallocs; b0 = g_bytes; mstr_stats_reset_();
    for (i = 0; i < 1000; i++) g_sink += f(W1_IN, 0, 0);
    s0 = mstr_stats_get_();
    r.name = name; r.ns = best;
    r.mallocs = (double)(g_mallocs - m0) / 1000.0; r.bytes = (double)(g_bytes - b0) / 1000.0;
    r.bumps = is_mstr ? (double)s0.bumps / 1000.0 : 0; r.bumpbytes = is_mstr ? (double)s0.bytes / 1000.0 : 0;
    r.retained = (double)retained;
    return r;
}
static row run_w2(const char *name, w2_fn f, int is_mstr)
{
    row r;
    const long iters = 200000;
    double best = 1e30;
    int rep;
    long i;
    unsigned long long m0, b0;
    mstr_stats s0;
    for (i = 0; i < 10000; i++) g_sink += f(g_csv);
    for (rep = 0; rep < 5; rep++) {
        double t0 = now_ns(), t1;
        for (i = 0; i < iters; i++) g_sink += f(g_csv);
        t1 = now_ns();
        if ((t1 - t0) / (double)iters < best) best = (t1 - t0) / (double)iters;
    }
    m0 = g_mallocs; b0 = g_bytes; mstr_stats_reset_();
    for (i = 0; i < 1000; i++) g_sink += f(g_csv);
    s0 = mstr_stats_get_();
    r.name = name; r.ns = best;
    r.mallocs = (double)(g_mallocs - m0) / 1000.0; r.bytes = (double)(g_bytes - b0) / 1000.0;
    r.bumps = is_mstr ? (double)s0.bumps / 1000.0 : 0; r.bumpbytes = is_mstr ? (double)s0.bytes / 1000.0 : 0;
    r.retained = 0;
    return r;
}

static void print_rows(const char *title, const row *rows, int n, int md, int with_retained)
{
    int i;
    if (md) {
        printf("\n### %s\n\n| implementation | malloc calls/op | bytes malloc'd/op | arena bumps/op | arena bytes/op |%s ns/op |\n|---|---:|---:|---:|---:|%s---:|\n",
               title, with_retained ? " arena bytes held after chain |" : "", with_retained ? "---:|" : "");
        for (i = 0; i < n; i++) {
            printf("| %s | %.2f | %.0f | %.2f | %.0f |", rows[i].name, rows[i].mallocs, rows[i].bytes, rows[i].bumps, rows[i].bumpbytes);
            if (with_retained) { if (rows[i].retained > 0) printf(" %.0f |", rows[i].retained); else printf(" - |"); }
            printf(" %.0f |\n", rows[i].ns);
        }
    } else {
        printf("\n%s\n%-26s %10s %12s %10s %12s %10s\n", title, "implementation", "malloc/op", "bytes/op", "bumps/op", "bumpB/op", "ns/op");
        for (i = 0; i < n; i++)
            printf("%-26s %10.2f %12.0f %10.2f %12.0f %10.0f\n", rows[i].name, rows[i].mallocs, rows[i].bytes, rows[i].bumps, rows[i].bumpbytes, rows[i].ns);
    }
}

int main(int argc, char **argv)
{
    int md = argc > 1 && strcmp(argv[1], "--md") == 0;
    mstr_allocator a;
    char ref[512], got[512];
    size_t rn, gn, i;
    row r1[6], r2[6];
    w1_fn f1[6] = {w1_malloc_steps, w1_malloc_fused, w1_sds, w1_mstr_nested, w1_mstr_pipe, w1_mstr_fused};
    w2_fn f2[6] = {w2_mstr_each, w2_mstr_each_prebuilt, w2_mstr_split, w2_sds, w2_malloc_dup, w2_malloc_scan};

    a.alloc = m_alloc; a.realloc = m_realloc; a.free = m_free; a.ctx = 0;
    mstr_set_allocator(a);
    build_csv();
    g_csv_m = mstr_from(g_csv);

    /* all implementations must agree before anything is timed */
    rn = w1_malloc_fused(W1_IN, ref, sizeof ref);
    for (i = 0; i < 6; i++) {
        gn = f1[i](W1_IN, got, sizeof got);
        if (gn != rn || memcmp(ref, got, rn + 1) != 0) { fprintf(stderr, "W1 mismatch in impl %zu\n", i); return 1; }
    }
    for (i = 1; i < 6; i++)
        if (f2[i](g_csv) != f2[0](g_csv)) { fprintf(stderr, "W2 mismatch in impl %zu (%zu vs %zu)\n", i, f2[i](g_csv), f2[0](g_csv)); return 1; }
    if (!md) printf("all implementations agree: W1 -> \"%s\" (%zu bytes); W2 sum of trimmed field lengths = %zu\n", ref, rn, f2[0](g_csv));

    r1[0] = run_w1("malloc, step by step", f1[0], 0, 0);
    r1[1] = run_w1("malloc, fused loop", f1[1], 0, 0);
    r1[2] = run_w1("SDS (in place)", f1[2], 0, 0);
    r1[3] = run_w1("mstr, nested calls", f1[3], 1, w1_retained(0));
    r1[4] = run_w1("mstr, MSTR_PIPE", f1[4], 1, w1_retained(1));
    r1[5] = run_w1("mstr, fused transform", f1[5], 1, w1_retained_transform());
    r2[0] = run_w2("mstr_each, from char*", f2[0], 1);
    r2[1] = run_w2("mstr_each, input is mstr", f2[1], 1);
    r2[2] = run_w2("mstr_split", f2[2], 1);
    r2[3] = run_w2("SDS sdssplitlen", f2[3], 0);
    r2[4] = run_w2("malloc, strdup fields", f2[4], 0);
    r2[5] = run_w2("malloc, pointer scan", f2[5], 0);
    print_rows("W1: trim + upper + replace (110-byte line)", r1, 6, md, 1);
    print_rows("W2: split a 20-field CSV line and sum trimmed lengths", r2, 6, md, 0);
    return g_sink == 0;
}
