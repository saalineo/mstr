/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "mstr.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#define MSTR_HAVE_PTHREAD 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define MSTR_TLS __thread
#elif defined(_MSC_VER)
#define MSTR_TLS __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define MSTR_TLS _Thread_local
#else
#error "no thread-local storage keyword known for this compiler"
#endif

void mstr_fail_(const char *file, int line, const char *msg)
{
    if (file) fprintf(stderr, "mstr: %s (at %s:%d)\n", msg, file, line);
    else fprintf(stderr, "mstr: %s\n", msg);
    abort();
}

/* Thread-local arena*/

#define CHUNK_HDR 32u
#define CHUNK_FIRST (64u * 1024u)
#define CHUNK_MAX (1024u * 1024u) /* growth cap; also the largest chunk we cache */
#define FREELIST_MAX 4u
#define BLOCK_BITS 20u /* serials are handed out in blocks of 2^20 per thread */
#define QUAR_SIZE 64u
#define RING_SIZE 256u /* remembers where recent scopes were entered (checked mode) */
#define DEPTH_MAX 0xFFFFu

typedef struct chunk {
    struct chunk *prev; /* older chunk; also links the free list / detached pool */
    size_t cap, used;
} chunk;

typedef char chunk_hdr_fits_[(sizeof(chunk) <= CHUNK_HDR) ? 1 : -1];

typedef struct frame {
    uint64_t serial;
    const char *file;
    int line;
} frame;

typedef struct ringent {
    uint64_t serial;
    const char *file;
    int line;
} ringent;

typedef struct tstate {
    chunk *cur;
    chunk *freelist;
    size_t nfree, next_size;
    frame *frames; /* frames[d] describes the scope at depth d; 0 = root */
    unsigned cap, depth;
    uint64_t counter, block_end;
    int inited, warned_root;
    mstr_stats stats;
#ifdef MSTR_CHECKED
    ringent ring[RING_SIZE];
    void *quar[QUAR_SIZE]; /* recently freed box buffers (double-free detection) */
    unsigned qpos;
#endif
} tstate;

static MSTR_TLS tstate g_ts;

/*  atomics: C11 <stdatomic.h> when available, else compiler builtins */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
typedef _Atomic uint64_t mstr_au64;
static uint64_t a_add(mstr_au64 *p, uint64_t v) { return atomic_fetch_add_explicit(p, v, memory_order_relaxed); }
static uint64_t a_sub_acqrel(mstr_au64 *p, uint64_t v) { return atomic_fetch_sub_explicit(p, v, memory_order_acq_rel); }
static uint64_t a_load(mstr_au64 *p) { return atomic_load_explicit(p, memory_order_relaxed); }
static void a_store(mstr_au64 *p, uint64_t v) { atomic_store_explicit(p, v, memory_order_relaxed); }
#elif defined(__GNUC__) || defined(__clang__)
typedef uint64_t mstr_au64;
static uint64_t a_add(mstr_au64 *p, uint64_t v) { return __atomic_fetch_add(p, v, __ATOMIC_RELAXED); }
static uint64_t a_sub_acqrel(mstr_au64 *p, uint64_t v) { return __atomic_fetch_sub(p, v, __ATOMIC_ACQ_REL); }
static uint64_t a_load(mstr_au64 *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static void a_store(mstr_au64 *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
#elif defined(_MSC_VER)
#include <intrin.h>
typedef volatile uint64_t mstr_au64;
static uint64_t a_add(mstr_au64 *p, uint64_t v) { return (uint64_t)_InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)v); }
static uint64_t a_sub_acqrel(mstr_au64 *p, uint64_t v) { return (uint64_t)_InterlockedExchangeAdd64((volatile __int64 *)p, -(__int64)v); }
static uint64_t a_load(mstr_au64 *p) { return (uint64_t)_InterlockedOr64((volatile __int64 *)p, 0); }
static void a_store(mstr_au64 *p, uint64_t v) { (void)_InterlockedExchange64((volatile __int64 *)p, (__int64)v); }
#else
#error "no atomics available for this compiler"
#endif

/*  global state
 * g_next_block: hands out serial blocks so serials are unique across threads.
 * g_alloc/g_oom/g_used: write-once-before-first-use configuration. */
static mstr_au64 g_next_block = 1; /* block 0 is never used: serial 0 = immortal */
static mstr_au64 g_used;           /* set by the first library allocation */

static void *def_alloc(size_t n, void *ctx) { (void)ctx; return malloc(n); }
static void *def_realloc(void *p, size_t n, void *ctx) { (void)ctx; return realloc(p, n); }
static void def_free(void *p, void *ctx) { (void)ctx; free(p); }
static mstr_allocator g_alloc = {def_alloc, def_realloc, def_free, 0};
static mstr_oom_handler g_oom;
static void *g_oom_ctx;

void mstr_set_allocator(mstr_allocator a)
{
    if (a_load(&g_used)) mstr_fail_(0, 0, "mstr_set_allocator: must be called before first use of the library");
    if (!a.alloc || !a.free) mstr_fail_(0, 0, "mstr_set_allocator: alloc and free are required");
    g_alloc = a;
}

void mstr_set_oom_handler(mstr_oom_handler h, void *ctx)
{
    if (a_load(&g_used)) mstr_fail_(0, 0, "mstr_set_oom_handler: must be called before first use of the library");
    g_oom = h;
    g_oom_ctx = ctx;
}

static void oom_(size_t n)
{
    if (g_oom) {
        g_oom(n, g_oom_ctx);
        mstr_fail_(0, 0, "out of memory: the oom handler returned (it must not)");
    }
    fprintf(stderr, "mstr: out of memory (%lu bytes requested)\n", (unsigned long)n);
    abort();
}

static void *mm_alloc(size_t n)
{
    void *p;
    a_store(&g_used, 1);
    p = g_alloc.alloc(n, g_alloc.ctx);
    if (!p) oom_(n);
    return p;
}

static void mm_free(void *p) { g_alloc.free(p, g_alloc.ctx); }

/* Grow a block from old_n to new_n bytes (contents preserved). */
static void *mm_grow(void *p, size_t old_n, size_t new_n)
{
    void *q;
    if (g_alloc.realloc) {
        a_store(&g_used, 1);
        q = g_alloc.realloc(p, new_n, g_alloc.ctx);
        if (!q) oom_(new_n);
        return q;
    }
    q = mm_alloc(new_n);
    memcpy(q, p, old_n);
    mm_free(p);
    return q;
}

static void *xmalloc(size_t n) { return mm_alloc(n); }

static char *cdata(chunk *c) { return (char *)c + CHUNK_HDR; }

static uint64_t next_serial(tstate *t)
{
    if (t->counter >= t->block_end) {
        uint64_t blk = a_add(&g_next_block, 1u);
        if (blk >= ((uint64_t)1 << (40u - BLOCK_BITS))) mstr_fail_(0, 0, "scope serial space exhausted");
        t->counter = blk << BLOCK_BITS;
        t->block_end = (blk + 1u) << BLOCK_BITS;
    }
    return t->counter++;
}

static void ts_free(tstate *t)
{
    chunk *c;
    while (t->cur) { c = t->cur; t->cur = c->prev; mm_free(c); }
    while (t->freelist) { c = t->freelist; t->freelist = c->prev; mm_free(c); }
    mm_free(t->frames);
#ifdef MSTR_CHECKED
    { unsigned i; for (i = 0; i < QUAR_SIZE; i++) if (t->quar[i]) mm_free(t->quar[i]); }
#endif
    memset(t, 0, sizeof *t);
}

#ifdef MSTR_HAVE_PTHREAD
static pthread_key_t g_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static void key_dtor(void *p) { ts_free((tstate *)p); }
static void key_make(void) { (void)pthread_key_create(&g_key, key_dtor); }
#endif

static tstate *ts_get(void)
{
    tstate *t = &g_ts;
    if (!t->inited) {
        t->cap = 16;
        t->frames = (frame *)xmalloc(t->cap * sizeof(frame));
        memset(t->frames, 0, t->cap * sizeof(frame));
        t->frames[0].serial = next_serial(t); /* root scope */
#ifdef MSTR_CHECKED
        t->ring[t->frames[0].serial % RING_SIZE].serial = t->frames[0].serial;
#endif
        t->depth = 0;
        t->next_size = CHUNK_FIRST;
        t->inited = 1;
#ifdef MSTR_HAVE_PTHREAD
        (void)pthread_once(&g_key_once, key_make);
        (void)pthread_setspecific(g_key, t); /* run ts_free at thread exit */
#endif
    }
    return t;
}

void mstr_thread_cleanup(void)
{
    if (g_ts.inited) ts_free(&g_ts);
}

static void release_list(tstate *t, chunk *list)
{
    while (list) {
        chunk *c = list;
        list = c->prev;
        if (c->cap <= CHUNK_MAX && t->nfree < FREELIST_MAX) {
            c->prev = t->freelist;
            t->freelist = c;
            t->nfree++;
        } else {
            mm_free(c);
        }
    }
}

/* Remove and return a chunk with cap >= need: pool, then free list, then malloc. */
static chunk *chunk_take(tstate *t, size_t need, chunk **pool)
{
    chunk **pp, *c, *none = 0;
    for (pp = pool ? pool : &none; *pp; pp = &(*pp)->prev)
        if ((*pp)->cap >= need) { c = *pp; *pp = c->prev; goto found; }
    for (pp = &t->freelist; *pp; pp = &(*pp)->prev)
        if ((*pp)->cap >= need) { c = *pp; *pp = c->prev; t->nfree--; goto found; }
    {
        size_t cap = t->next_size;
        if (need > cap) cap = need; /* oversized: dedicated chunk */
        else if (cap < CHUNK_MAX) t->next_size = cap * 2;
        c = (chunk *)xmalloc(CHUNK_HDR + cap);
        c->cap = cap;
    }
found:
    c->prev = 0;
    c->used = 0;
    return c;
}

static size_t round8(size_t n)
{
    if (n > (size_t)-1 / 2) mstr_fail_(0, 0, "allocation size overflow");
    return (n + 7u) & ~(size_t)7u;
}

static void *arena_alloc(tstate *t, size_t n)
{
    size_t sz = round8(n);
#ifdef MSTR_CHECKED
    if (t->depth == 0 && !t->warned_root) {
        t->warned_root = 1;
        if (!getenv("MSTR_NO_ROOT_WARNING"))
            fprintf(stderr, "mstr: warning: allocating in the implicit root scope; memory is only "
                            "reclaimed at thread exit. Open an mstr_scope (set MSTR_NO_ROOT_WARNING to silence)\n");
    }
#endif
    chunk *c = t->cur;
    void *p;
#ifdef MSTR_STATS
    t->stats.bumps++;
    t->stats.bytes += sz;
#endif
    if (!c || c->cap - c->used < sz) {
        chunk *nc = chunk_take(t, sz, 0);
        nc->prev = c;
        t->cur = c = nc;
    }
    p = cdata(c) + c->used;
    c->used += sz;
    return p;
}

void *mstr_scope_alloc(size_t n) { return arena_alloc(ts_get(), n); }

/* Rewind to a bookmark; returns the detached chunks (still intact) so that
 * promote can read from them before they are recycled. */
static chunk *rewind_detach(tstate *t, chunk *b, size_t off)
{
    chunk *pool = 0;
    while (t->cur && t->cur != b) {
        chunk *c = t->cur;
        t->cur = c->prev;
        c->prev = pool;
        pool = c;
    }
    if (t->cur != b) mstr_fail_(0, 0, "arena corrupted: scope bookmark not found");
    if (b) {
        assert(off <= b->used);
        b->used = off;
    }
    return pool;
}

size_t mstr_debug_arena_used_(void)
{
    size_t n = 0;
    chunk *c;
    for (c = ts_get()->cur; c; c = c->prev) n += c->used;
    return n;
}
size_t mstr_debug_chunk_count_(void)
{
    size_t n = 0;
    chunk *c;
    for (c = ts_get()->cur; c; c = c->prev) n++;
    return n;
}
size_t mstr_debug_free_chunks_(void) { return ts_get()->nfree; }


/* Scopes */

mstr_scope_t mstr_scope_enter_(const char *file, int line)
{
    tstate *t = ts_get();
    mstr_scope_t s;
    if (t->depth >= DEPTH_MAX) mstr_fail_(file, line, "scopes nested too deeply (65535)");
    if (t->depth + 1u >= t->cap) {
        unsigned ncap = t->cap * 2u;
        t->frames = (frame *)mm_grow(t->frames, t->cap * sizeof(frame), ncap * sizeof(frame));
        t->cap = ncap;
    }
    s.chunk = t->cur;
    s.off = t->cur ? t->cur->used : 0;
    s.serial = next_serial(t);
    s.depth = ++t->depth;
    s.active = 1;
    s.file = file;
    s.line = line;
    t->frames[s.depth].serial = s.serial;
    t->frames[s.depth].file = file;
    t->frames[s.depth].line = line;
#ifdef MSTR_CHECKED
    t->ring[s.serial % RING_SIZE].serial = s.serial;
    t->ring[s.serial % RING_SIZE].file = file;
    t->ring[s.serial % RING_SIZE].line = line;
#endif
    return s;
}

void mstr_check_(const mstr *s)
{
    unsigned st = mstr_state(s), d;
    uint64_t ser;
    tstate *t;
    if (st == MSTR_SSO || st == MSTR_STATIC) return;
    ser = mstr_serial_(s);
    if (ser == 0) return; /* VIEW into STATIC data: immortal */
    d = mstr_depth_(s);
    t = ts_get();
    if (d <= t->depth && t->frames[d].serial == ser) return;
#ifdef MSTR_CHECKED
    if (t->ring[ser % RING_SIZE].serial == ser) {
        const ringent *r = &t->ring[ser % RING_SIZE];
        if (r->file)
            fprintf(stderr, "mstr: string from scope entered at %s:%d used after it ended (depth %u)\n", r->file,
                    r->line, d);
        else
            fprintf(stderr, "mstr: string from the implicit root scope used after it ended (depth %u)\n", d);
        abort();
    }
#endif
    fprintf(stderr,
            "mstr: string from scope (depth %u, serial %llu) used after it ended, or on a different thread than "
            "the one that created it\n",
            d, (unsigned long long)ser);
    abort();
}

/* Is `s` still the scope it claims to be on this thread's stack? */
static int scope_live(const tstate *t, const mstr_scope_t *s)
{
    return s->active && s->depth >= 1 && t->depth >= s->depth && t->frames[s->depth].serial == s->serial;
}

void mstr_scope_leave(mstr_scope_t *s)
{
    tstate *t;
    if (!s->active) return;
    t = ts_get();
    if (!scope_live(t, s)) { /* an enclosing scope already unwound us */
#ifdef MSTR_CHECKED
        mstr_fail_(s->file, s->line,
                   "mstr_scope_leave: scope is not live on this thread (left on another thread, or already unwound)");
#endif
        s->active = 0;
        return;
    }
    s->active = 0;
    release_list(t, rewind_detach(t, (chunk *)s->chunk, s->off));
    t->depth = s->depth - 1u;
}


/* Allocation of results */

static size_t add_(size_t a, size_t b);

/* Build space for an n-byte result. n <= 22 -> SSO inside *out (the caller
 * fills the returned pointer, which points into *out); else a REGION in the
 * current scope, already NUL-terminated. */
static char *new_(size_t n, mstr *out)
{
    tstate *t;
    char *p;
    if (n <= MSTR_SSO_MAX) {
        *out = mstr_make_sso_("", 0);
        out->b[MSTR_TAG_] = (unsigned char)(n << 2);
        return (char *)out->b;
    }
    t = ts_get();
    p = (char *)arena_alloc(t, add_(n, 1u));
    p[n] = '\0';
    *out = mstr_make_long_(MSTR_REGION, p, n, t->depth, t->frames[t->depth].serial);
    return p;
}

static size_t add_(size_t a, size_t b)
{
    if (a > (size_t)-1 - b) mstr_fail_(0, 0, "string length overflow");
    return a + b;
}
static size_t mul_(size_t a, size_t b)
{
    if (a && b > (size_t)-1 / a) mstr_fail_(0, 0, "string length overflow");
    return a * b;
}

mstr mstr_from_n(const char *p, size_t n)
{
    mstr r;
    char *d = new_(n, &r);
    if (n) memcpy(d, p, n);
    return r;
}

mstr mstr_from(const char *cstr)
{
    return mstr_from_n(cstr, strlen(cstr)); /* strlen on C input, never on mstr data */
}

mstr mstr_promote_(mstr_scope_t *s, mstr v, const char *file, int line)
{
    tstate *t = ts_get();
    unsigned st = mstr_state(&v);
    size_t n, sz;
    const char *p;
    chunk *pool, *c;
    char *dest;
    if (!scope_live(t, s)) mstr_fail_(file, line, "mstr_promote: scope is not the live scope of this thread");
    if (t->depth != s->depth) mstr_fail_(file, line, "mstr_promote: scope has unclosed inner scopes");
    /* Nothing to copy if the bytes do not live in this scope's arena range. */
    if (st == MSTR_SSO || st == MSTR_STATIC || mstr_serial_(&v) == 0 || mstr_depth_(&v) < s->depth) {
        mstr_scope_leave(s);
        return v;
    }
    n = mstr_len(v);
    p = mstr_ptr(&v);
    sz = round8(add_(n, 1u));
    pool = rewind_detach(t, (chunk *)s->chunk, s->off); /* source bytes stay intact */
    s->active = 0;
    t->depth = s->depth - 1u;
    c = t->cur;
    if (!c || c->cap - c->used < sz) {
        chunk *nc = chunk_take(t, sz, &pool);
        nc->prev = c;
        t->cur = c = nc;
    }
    dest = cdata(c) + c->used;
    memmove(dest, p, n); /* may overlap the source (same chunk, shifting down) */
    dest[n] = '\0';
    c->used += sz;
    release_list(t, pool);
    return mstr_make_long_(MSTR_REGION, dest, n, t->depth, t->frames[t->depth].serial);
}

/* Core operations  */
/* Core of the Law of Small Results. Precondition: off + n <= len(s). */
mstr mstr_sub_(const mstr *s, size_t off, size_t n)
{
    const char *p = mstr_ptr(s) + off;
    if (n <= MSTR_SSO_MAX) return mstr_make_sso_(p, n); /* demote: R3 */
    /* n > 22 implies s is not SSO, so a VIEW never points into an SSO buffer. */
    assert(mstr_state(s) != MSTR_SSO);
    return mstr_make_long_(MSTR_VIEW, p, n, mstr_depth_(s), mstr_serial_(s));
}

mstr mstr_slice_(mstr s, size_t begin, size_t end, const char *file, int line)
{
    if (begin > end || end > mstr_len(s)) mstr_fail_(file, line, "mstr_slice: range out of bounds");
    return mstr_sub_(&s, begin, end - begin);
}

char mstr_at_(mstr s, size_t i, const char *file, int line)
{
    if (i >= mstr_len(s)) mstr_fail_(file, line, "mstr_at: index out of bounds");
    return mstr_ptr(&s)[i];
}

int mstr_eq(mstr a, mstr b)
{
    size_t n = mstr_len(a);
    if (n != mstr_len(b)) return 0;
    return n == 0 || memcmp(mstr_ptr(&a), mstr_ptr(&b), n) == 0;
}

int mstr_cmp(mstr a, mstr b)
{
    size_t la = mstr_len(a), lb = mstr_len(b), m = la < lb ? la : lb;
    int c = m ? memcmp(mstr_ptr(&a), mstr_ptr(&b), m) : 0;
    if (c) return c < 0 ? -1 : 1;
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

static size_t raw_find(const char *h, size_t hl, const char *n, size_t nl)
{
    size_t i;
    if (nl == 0) return 0;
    if (nl > hl) return MSTR_NPOS;
    for (i = 0; i + nl <= hl;) {
        const char *hit = (const char *)memchr(h + i, (unsigned char)n[0], hl - nl - i + 1);
        if (!hit) return MSTR_NPOS;
        i = (size_t)(hit - h);
        if (memcmp(hit, n, nl) == 0) return i;
        i++;
    }
    return MSTR_NPOS;
}

size_t mstr_find(mstr hay, mstr needle)
{
    return raw_find(mstr_ptr(&hay), mstr_len(hay), mstr_ptr(&needle), mstr_len(needle));
}

size_t mstr_rfind(mstr hay, mstr needle)
{
    size_t hl = mstr_len(hay), nl = mstr_len(needle), i;
    const char *h = mstr_ptr(&hay), *n = mstr_ptr(&needle);
    if (nl == 0) return hl;
    if (nl > hl) return MSTR_NPOS;
    for (i = hl - nl + 1; i-- > 0;)
        if (h[i] == n[0] && memcmp(h + i, n, nl) == 0) return i;
    return MSTR_NPOS;
}

int mstr_contains(mstr hay, mstr needle) { return mstr_find(hay, needle) != MSTR_NPOS; }

int mstr_starts_with(mstr s, mstr p)
{
    size_t pl = mstr_len(p);
    return pl <= mstr_len(s) && (pl == 0 || memcmp(mstr_ptr(&s), mstr_ptr(&p), pl) == 0);
}

int mstr_ends_with(mstr s, mstr x)
{
    size_t sl = mstr_len(s), xl = mstr_len(x);
    return xl <= sl && (xl == 0 || memcmp(mstr_ptr(&s) + (sl - xl), mstr_ptr(&x), xl) == 0);
}

static int is_ws_(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static mstr trim_(mstr s, int left, int right)
{
    const char *p = mstr_ptr(&s);
    size_t b = 0, e = mstr_len(s);
    if (left) while (b < e && is_ws_(p[b])) b++;
    if (right) while (e > b && is_ws_(p[e - 1])) e--;
    return mstr_sub_(&s, b, e - b);
}

mstr mstr_trim(mstr s) { return trim_(s, 1, 1); }
mstr mstr_ltrim(mstr s) { return trim_(s, 1, 0); }
mstr mstr_rtrim(mstr s) { return trim_(s, 0, 1); }


/* Building operations */


mstr mstr_cat(mstr a, mstr b)
{
    size_t la = mstr_len(a), lb = mstr_len(b);
    mstr r;
    char *d = new_(add_(la, lb), &r);
    if (la) memcpy(d, mstr_ptr(&a), la);
    if (lb) memcpy(d + la, mstr_ptr(&b), lb);
    return r;
}

static mstr replace_(mstr s, mstr from, mstr to, int all)
{
    size_t sl = mstr_len(s), fl = mstr_len(from), tl = mstr_len(to), count = 0, pos, nl;
    const char *sp = mstr_ptr(&s), *fp = mstr_ptr(&from), *tp = mstr_ptr(&to);
    mstr r;
    char *d;
    if (fl == 0) return s;
    if (fl == 1) { /* single-byte pattern: plain loops beat a memchr+memcmp call per match */
        char f = fp[0];
        size_t k, first = 0;
        if (all) {
            for (k = 0; k < sl; k++) count += sp[k] == f;
        } else {
            const char *h = (const char *)memchr(sp, (unsigned char)f, sl);
            if (h) { count = 1; first = (size_t)(h - sp); }
        }
        if (count == 0) return s;
        nl = add_(sl - count, mul_(count, tl));
        d = new_(nl, &r);
        if (!all) {
            memcpy(d, sp, first);
            if (tl) memcpy(d + first, tp, tl);
            memcpy(d + first + tl, sp + first + 1, sl - first - 1u);
            return r;
        }
        if (tl == 1) {
            char t = tp[0];
            for (k = 0; k < sl; k++) d[k] = sp[k] == f ? t : sp[k];
            return r;
        }
        for (k = 0; k < sl; k++) {
            if (sp[k] == f) { if (tl) memcpy(d, tp, tl); d += tl; }
            else *d++ = sp[k];
        }
        return r;
    }
    for (pos = 0; pos <= sl;) {
        size_t i = raw_find(sp + pos, sl - pos, fp, fl);
        if (i == MSTR_NPOS) break;
        count++;
        pos += i + fl;
        if (!all) break;
    }
    if (count == 0) return s;
    /* new length = sl - count*fl + count*tl, computed without underflow */
    nl = add_(sl - mul_(count, fl), mul_(count, tl));
    d = new_(nl, &r);
    for (pos = 0; count; count--) {
        size_t i = raw_find(sp + pos, sl - pos, fp, fl);
        if (i) memcpy(d, sp + pos, i);
        d += i;
        if (tl) memcpy(d, tp, tl);
        d += tl;
        pos += i + fl;
    }
    if (sl - pos) memcpy(d, sp + pos, sl - pos);
    return r;
}

mstr mstr_replace(mstr s, mstr from, mstr to) { return replace_(s, from, to, 0); }
mstr mstr_replace_all(mstr s, mstr from, mstr to) { return replace_(s, from, to, 1); }

static mstr casemap_(mstr s, int up)
{
    size_t n = mstr_len(s), i, j;
    const char *p = mstr_ptr(&s);
    mstr r;
    char *d;
    unsigned char lo = up ? 'a' : 'A';
    for (i = 0; i < n; i++)
        if ((unsigned)((unsigned char)p[i] - lo) < 26u) break;
    if (i == n) return s; /* nothing to change: no allocation */
    d = new_(n, &r);
    /* branch-free so the compiler can vectorise: ASCII letters differ from their
     * other case in bit 0x20 only */
    for (j = 0; j < n; j++) {
        unsigned char c = (unsigned char)p[j];
        d[j] = ((unsigned)(c - lo) < 26u) ? (char)(c ^ 0x20u) : (char)c;
    }
    return r;
}

mstr mstr_upper(mstr s) { return casemap_(s, 1); }
mstr mstr_lower(mstr s) { return casemap_(s, 0); }

mstr mstr_repeat(mstr s, size_t times)
{
    size_t n = mstr_len(s), total = mul_(n, times), i;
    mstr r;
    char *d = new_(total, &r);
    for (i = 0; i < times; i++) memcpy(d + i * n, mstr_ptr(&s), n);
    return r;
}

mstr mstr_join(mstr sep, const mstr *parts, size_t count)
{
    size_t sl = mstr_len(sep), total = 0, i;
    mstr r;
    char *d;
    for (i = 0; i < count; i++) total = add_(total, mstr_len(parts[i]));
    if (count > 1) total = add_(total, mul_(count - 1, sl));
    d = new_(total, &r);
    for (i = 0; i < count; i++) {
        mstr part = parts[i];
        size_t pl = mstr_len(part);
        if (i && sl) { memcpy(d, mstr_ptr(&sep), sl); d += sl; }
        if (pl) { memcpy(d, mstr_ptr(&part), pl); d += pl; }
    }
    return r;
}

mstr mstr_fmt(const char *fmt, ...)
{
    char small[MSTR_SSO_MAX + 1u];
    va_list ap, ap2;
    int n;
    mstr r;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); mstr_fail_(0, 0, "mstr_fmt: formatting error"); }
    if ((size_t)n <= MSTR_SSO_MAX) {
        va_end(ap2);
        return mstr_make_sso_(small, (size_t)n);
    }
    {
        char *d = new_((size_t)n, &r);
        (void)vsnprintf(d, (size_t)n + 1u, fmt, ap2);
        va_end(ap2);
    }
    return r;
}

/* Iteration and split */


/* it[0] = remaining text, it[1] = delimiter, it[2].b[0] = done, it[2].b[1] = lines mode */
int mstr_iter_next_(mstr *tok, mstr *it)
{
    mstr rest = it[0];
    size_t n = mstr_len(rest), i;
    const char *p = mstr_ptr(&rest);
    if (it[2].b[0]) return 0;
    if (it[2].b[1]) { /* lines */
        const char *nl;
        size_t e;
        if (n == 0) { it[2].b[0] = 1; return 0; }
        nl = (const char *)memchr(p, '\n', n);
        if (!nl) { *tok = rest; it[2].b[0] = 1; return 1; }
        i = (size_t)(nl - p);
        e = (i > 0 && p[i - 1] == '\r') ? i - 1u : i;
        *tok = mstr_sub_(&rest, 0, e);
        it[0] = mstr_sub_(&rest, i + 1u, n - i - 1u);
        return 1;
    } else {
        size_t dl = mstr_len(it[1]);
        if (dl == 1) { /* the common single-byte delimiter: one memchr, no memcmp */
            const char *hit = (const char *)memchr(p, (unsigned char)mstr_ptr(&it[1])[0], n);
            i = hit ? (size_t)(hit - p) : MSTR_NPOS;
        } else {
            i = dl ? raw_find(p, n, mstr_ptr(&it[1]), dl) : MSTR_NPOS;
        }
        if (i == MSTR_NPOS) { *tok = rest; it[2].b[0] = 1; return 1; }
        *tok = mstr_sub_(&rest, 0, i);
        it[0] = mstr_sub_(&rest, i + dl, n - i - dl);
        return 1;
    }
}

mstr *mstr_split(mstr s, mstr delim, size_t *count)
{
    size_t n = 0, k = 0;
    mstr tok = mstr_empty(), it[3];
    mstr *out;
    it[0] = s; it[1] = delim; it[2] = mstr_empty();
    while (mstr_iter_next_(&tok, it)) n++;
    out = (mstr *)mstr_scope_alloc(mul_(n, sizeof(mstr)));
    it[0] = s; it[1] = delim; it[2] = mstr_empty();
    while (mstr_iter_next_(&tok, it)) out[k++] = tok;
    if (count) *count = n;
    return out;
}

/* Ownership: boxes */

#define BOX_HDR 16u
#define BOX_LIVE 0x4D5354524C495645ull /* "MSTRLIVE" */
#define BOX_DEAD 0x4D53545244454144ull /* "MSTRDEAD" */
enum { BOX_EMPTY = 0, BOX_SMALL = 1, BOX_HEAP = 2, BOX_FREED = 3 };

typedef struct bhdr {
    uint64_t magic, len;
} bhdr;
typedef char bhdr_fits_[(sizeof(bhdr) == BOX_HDR) ? 1 : -1];

mstr_box mstr_own(mstr s)
{
    size_t n = mstr_len(s);
    mstr_box b;
    memset(&b, 0, sizeof b);
    b.len = n;
    if (n <= MSTR_SSO_MAX) {
        b.small_ = mstr_sub_(&s, 0, n);
        b.kind = BOX_SMALL;
    } else {
        char *raw = (char *)mm_alloc(add_(BOX_HDR + 1u, n));
        bhdr h;
        h.magic = BOX_LIVE;
        h.len = n;
        memcpy(raw, &h, sizeof h);
        b.ptr = raw + BOX_HDR;
        memcpy(b.ptr, mstr_ptr(&s), n);
        b.ptr[n] = '\0';
        b.kind = BOX_HEAP;
    }
    return b;
}

mstr mstr_unbox(const mstr_box *b)
{
    switch (b->kind) {
    case BOX_SMALL: return b->small_;
    case BOX_HEAP: return mstr_make_long_(MSTR_VIEW, b->ptr, b->len, 0, 0); /* immortal tag: see DESIGN.md */
    case BOX_FREED:
#ifdef MSTR_CHECKED
        mstr_fail_(0, 0, "mstr_unbox: the box was already freed");
#endif
        return mstr_empty();
    default: return mstr_empty();
    }
}

void mstr_box_free(mstr_box *b)
{
    if (b->kind == BOX_HEAP) {
        char *raw = b->ptr - BOX_HDR;
#ifdef MSTR_CHECKED
        {
            tstate *t = ts_get();
            bhdr h;
            void **slot;
            memcpy(&h, raw, sizeof h);
            if (h.magic != BOX_LIVE)
                mstr_fail_(0, 0, "mstr_box_free: double free (this buffer was already freed through another copy of the box)");
            h.magic = BOX_DEAD;
            memcpy(raw, &h, sizeof h);
            memset(b->ptr, 0xDD, b->len + 1u); /* stale views now read poison */
            slot = &t->quar[t->qpos++ % QUAR_SIZE];
            if (*slot) mm_free(*slot);
            *slot = raw; /* really freed only after 64 more frees */
        }
#else
        mm_free(raw);
#endif
    }
    if (b->kind != BOX_EMPTY) b->kind = BOX_FREED;
    b->ptr = 0;
    b->len = 0;
    b->small_ = mstr_empty();
}

/* Sharing across threads */

#define SHARE_HDR 16u
struct mstr_shared {
    mstr_au64 refs;
    uint64_t len;
};
typedef char share_hdr_fits_[(sizeof(struct mstr_shared) <= SHARE_HDR) ? 1 : -1];

static const char *share_data(const mstr_shared *sh) { return (const char *)sh + SHARE_HDR; }

mstr_shared *mstr_share(mstr s)
{
    size_t n = mstr_len(s);
    mstr_shared *sh = (mstr_shared *)mm_alloc(add_(SHARE_HDR + 1u, n));
    char *d = (char *)sh + SHARE_HDR;
    a_store(&sh->refs, 1u);
    sh->len = n;
    if (n) memcpy(d, mstr_ptr(&s), n);
    d[n] = '\0';
    return sh;
}

mstr_shared *mstr_share_retain(mstr_shared *sh)
{
    if (a_add(&sh->refs, 1u) == 0) mstr_fail_(0, 0, "mstr_share_retain: the share was already released");
    return sh;
}

void mstr_share_release(mstr_shared *sh)
{
    uint64_t prev = a_sub_acqrel(&sh->refs, 1u);
    if (prev == 0) mstr_fail_(0, 0, "mstr_share_release: refcount underflow (released more often than retained)");
    if (prev == 1u) mm_free(sh);
}

mstr mstr_share_get(const mstr_shared *sh)
{
    size_t n = (size_t)sh->len;
    if (n <= MSTR_SSO_MAX) return mstr_make_sso_(share_data(sh), n);
    return mstr_make_long_(MSTR_VIEW, share_data(sh), n, 0, 0);
}

size_t mstr_share_refs_(const mstr_shared *sh) { return (size_t)a_load((mstr_au64 *)&sh->refs); }

/* UTF-8 layer */

#define U8_REPL 0xFFFDu

uint32_t mstr_u8_decode(const char *p, size_t n, size_t *adv, int *ok)
{
    unsigned char c = (unsigned char)p[0], lo = 0x80, hi = 0xBF;
    uint32_t cp;
    size_t need, i, k;
    if (ok) *ok = 1;
    if (c < 0x80) { *adv = 1; return c; }
    if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) {
        need = 2; cp = c & 0x0Fu;
        if (c == 0xE0) lo = 0xA0;       /* no overlongs */
        else if (c == 0xED) hi = 0x9F;  /* no surrogates */
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3; cp = c & 0x07u;
        if (c == 0xF0) lo = 0x90;       /* no overlongs */
        else if (c == 0xF4) hi = 0x8F;  /* <= U+10FFFF */
    } else { /* 80..C1, F5..FF can never start a sequence */
        *adv = 1;
        if (ok) *ok = 0;
        return U8_REPL;
    }
    i = 1;
    for (k = 0; k < need; k++) {
        unsigned char b;
        if (i >= n) goto bad; /* truncated */
        b = (unsigned char)p[i];
        if (b < lo || b > hi) goto bad;
        cp = (cp << 6) | (b & 0x3Fu);
        i++;
        lo = 0x80; hi = 0xBF;
    }
    *adv = i;
    return cp;
bad:
    *adv = i; /* the maximal well-formed prefix, at least the lead byte */
    if (ok) *ok = 0;
    return U8_REPL;
}

int mstr_u8_valid(mstr s)
{
    size_t n = mstr_len(s), i = 0, adv;
    const char *p = mstr_ptr(&s);
    int ok;
    while (i < n) {
        (void)mstr_u8_decode(p + i, n - i, &adv, &ok);
        if (!ok) return 0;
        i += adv;
    }
    return 1;
}

size_t mstr_u8_count(mstr s)
{
    size_t n = mstr_len(s), i = 0, adv, count = 0;
    const char *p = mstr_ptr(&s);
    while (i < n) {
        (void)mstr_u8_decode(p + i, n - i, &adv, 0);
        i += adv;
        count++;
    }
    return count;
}

mstr mstr_u8_from_cp(uint32_t cp)
{
    char b[4];
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = U8_REPL;
    if (cp < 0x80u) { b[0] = (char)cp; return mstr_make_sso_(b, 1); }
    if (cp < 0x800u) {
        b[0] = (char)(0xC0u | (cp >> 6)); b[1] = (char)(0x80u | (cp & 0x3Fu));
        return mstr_make_sso_(b, 2);
    }
    if (cp < 0x10000u) {
        b[0] = (char)(0xE0u | (cp >> 12)); b[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (char)(0x80u | (cp & 0x3Fu));
        return mstr_make_sso_(b, 3);
    }
    b[0] = (char)(0xF0u | (cp >> 18)); b[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    b[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); b[3] = (char)(0x80u | (cp & 0x3Fu));
    return mstr_make_sso_(b, 4);
}

uint32_t mstr_u8_cp(mstr ch)
{
    size_t adv;
    if (mstr_len(ch) == 0) return U8_REPL;
    return mstr_u8_decode(mstr_ptr(&ch), mstr_len(ch), &adv, 0);
}

mstr mstr_u8_slice_(mstr s, size_t begin, size_t end, const char *file, int line)
{
    size_t n = mstr_len(s), i = 0, k = 0, bb = 0, be = 0, adv;
    const char *p = mstr_ptr(&s);
    if (begin > end) mstr_fail_(file, line, "mstr_u8_slice: range out of bounds (begin > end)");
    for (;;) {
        if (k == begin) bb = i;
        if (k == end) { be = i; break; }
        if (i >= n) mstr_fail_(file, line, "mstr_u8_slice: range out of bounds (past the last code point)");
        (void)mstr_u8_decode(p + i, n - i, &adv, 0);
        i += adv;
        k++;
    }
    return mstr_sub_(&s, bb, be - bb);
}

int mstr_u8_next_(mstr *ch, mstr *it)
{
    mstr rest = it[0];
    size_t n = mstr_len(rest), adv;
    int ok;
    if (n == 0) return 0;
    (void)mstr_u8_decode(mstr_ptr(&rest), n, &adv, &ok);
    *ch = ok ? mstr_sub_(&rest, 0, adv) : mstr_make_sso_("\xEF\xBF\xBD", 3);
    it[0] = mstr_sub_(&rest, adv, n - adv);
    return 1;
}

/* Fused pipeline: results slide down over their dead inputs */

/* Is p inside arena bytes allocated after the bookmark (b, off)? */
static int above_mark(const tstate *t, const chunk *b, size_t off, const char *p)
{
    const chunk *c;
    for (c = t->cur; c; c = c->prev) {
        uintptr_t lo = (uintptr_t)((const char *)c + CHUNK_HDR) + (c == b ? off : 0u);
        uintptr_t hi = (uintptr_t)((const char *)c + CHUNK_HDR) + c->used;
        if ((uintptr_t)p >= lo && (uintptr_t)p < hi) return 1;
        if (c == b) break;
    }
    return 0;
}

mstr_pipe_t mstr_pipe_begin_(mstr x)
{
    tstate *t = ts_get();
    mstr_pipe_t p;
    p.chunk = t->cur;
    p.off = t->cur ? t->cur->used : 0;
    p.v = x;
    return p;
}

mstr mstr_pipe_hop_(mstr_pipe_t *p, mstr r)
{
    tstate *t = ts_get();
    chunk *b = (chunk *)p->chunk, *c, *pool;
    unsigned st = mstr_state(&r);
    size_t n = 0, sz;
    const char *src = 0;
    char *dest;
    int copy = 0;
    if ((st == MSTR_REGION || st == MSTR_VIEW) && mstr_serial_(&r) != 0) {
        src = mstr_ptr(&r);
        n = mstr_len(r);
        copy = above_mark(t, b, p->off, src);
    }
    pool = rewind_detach(t, b, p->off); /* drops the dead input and all step temporaries */
    if (!copy) { /* SSO, STATIC, or bytes that live below the mark: nothing to keep */
        release_list(t, pool);
        p->v = r;
        return r;
    }
    sz = round8(add_(n, 1u));
    c = t->cur;
    if (!c || c->cap - c->used < sz) { /* no room at the mark: continue in a fresh chunk */
        chunk *nc = chunk_take(t, sz, &pool);
        nc->prev = c;
        t->cur = c = nc;
        p->chunk = nc;
        p->off = 0;
    }
    dest = cdata(c) + c->used;
    if (dest != src) memmove(dest, src, n); /* overlap-safe; the source stays intact in `pool` until now */
    dest[n] = '\0';
    c->used += sz;
    release_list(t, pool);
    p->v = mstr_make_long_(MSTR_REGION, dest, n, t->depth, t->frames[t->depth].serial);
    return p->v;
}

mstr mstr_pipe_end_(mstr_pipe_t *p) { return p->v; }

mstr_stats mstr_stats_get_(void) { return ts_get()->stats; }
void mstr_stats_reset_(void) { memset(&ts_get()->stats, 0, sizeof(mstr_stats)); }

/* C interop */

const char *mstr_cstr_p(const mstr *s)
{
    unsigned st = mstr_state(s);
    size_t n;
    const char *p;
    mstr r;
    char *d;
    if (st != MSTR_VIEW) return mstr_ptr(s); /* SSO, REGION, STATIC are NUL-terminated */
    n = mstr_len(*s);
    p = mstr_ptr(s);
    if (p[n] == '\0') return p; /* view ends at the buffer's NUL */
    d = new_(n, &r);            /* n > 22 for any VIEW, so this is a REGION */
    memcpy(d, p, n);
    return d;
}
