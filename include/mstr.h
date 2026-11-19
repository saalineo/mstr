/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* mstr: lifetime-based immutable strings for C99  */
#ifndef MSTR_H
#define MSTR_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* GNU extensions (attributes, statement expressions, cleanup). Define MSTR_NO_GNU_EXT to
 * force the portable code paths on gcc/clang (used by the tests to exercise the MSVC variant). */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(MSTR_NO_GNU_EXT)
#define MSTR_GNU_ 1
#endif
/* Without GNU extensions there is no cleanup attribute, so mstr_scope is the strict variant. */
#if !defined(MSTR_GNU_) && !defined(MSTR_STRICT_SCOPE)
#define MSTR_STRICT_SCOPE 1
#endif
#if defined(MSTR_GNU_) && !defined(MSTR_STRICT_SCOPE)
#define MSTR_HAVE_CLEANUP_ 1
#endif

#ifdef __cplusplus
#if (defined(_MSVC_LANG) ? _MSVC_LANG : __cplusplus) >= 201703L
#define MSTR_CXX17_ 1
#include <string>
#include <string_view>
#endif
extern "C" {
#endif

#if UINTPTR_MAX != 0xFFFFFFFFFFFFFFFFu
#error "mstr supports 64-bit targets only (see docs/DESIGN.md)"
#endif

#ifdef MSTR_GNU_
#define MSTR_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define MSTR_NORETURN __declspec(noreturn)
#else
#define MSTR_NORETURN
#endif

/* 24 bytes, alignment 8 (the uint64_t member only provides alignment;
 * fields are accessed with memcpy / byte indices, never typed pointers). */
typedef union mstr {
    unsigned char b[24];
    uint64_t align_;
#ifdef __cplusplus
    /* C++ conveniences. Still a trivially copyable 24-byte aggregate. The
     * namespace-style spellings mstr::Scope / mstr::Box are nested classes
     * (a namespace cannot share a name with this type); see mstr.hpp. */
    class Scope;
    class Box;
    size_t size() const;
    const char *data() const; /* SSO: points INTO this object */
    char operator[](size_t i) const; /* bounds-checked, aborts */
    bool operator==(const union mstr &o) const;
    bool operator!=(const union mstr &o) const;
#ifdef MSTR_CXX17_
    operator std::string_view() const;          /* implicit; SSO views point into *this */
    explicit operator std::string() const;
    bool operator==(std::string_view o) const;
    static union mstr from(std::string_view v); /* copies: SSO, else REGION in the current scope */
#endif
#endif
} mstr;

typedef char mstr_assert_size_[(sizeof(mstr) == 24) ? 1 : -1];

#define MSTR_SSO_MAX 22u
#define MSTR_TAG_ 23
#define MSTR_NPOS ((size_t)-1)

enum { MSTR_SSO = 0, MSTR_REGION = 1, MSTR_VIEW = 2, MSTR_STATIC = 3 };

#ifdef MSTR_CHECKED
#define MSTR_LOC_ __FILE__, __LINE__
#else
#define MSTR_LOC_ ((const char *)0), 0
#endif

/* Aborts with a message (file/line when known). Never returns. */
MSTR_NORETURN void mstr_fail_(const char *file, int line, const char *msg);

/* representation accessors  */
 
/* Validates a REGION/VIEW against this thread's scope stack; aborts with the
 * scope's entry file:line if it is dead or foreign. Always defined; only
 * called automatically when MSTR_CHECKED is set. */
void mstr_check_(const mstr *s);
#ifdef MSTR_CHECKED
#define MSTR_VALIDATE_(sp) mstr_check_(sp)
#else
#define MSTR_VALIDATE_(sp) ((void)0)
#endif

static inline unsigned mstr_state(const mstr *s) { return (unsigned)(s->b[MSTR_TAG_] & 3u); }
static inline size_t mstr_sso_len_(const mstr *s) { return (size_t)((s->b[MSTR_TAG_] >> 2) & 31u); }

static inline size_t mstr_len(mstr s)
{
    size_t n;
    if (mstr_state(&s) == MSTR_SSO) return mstr_sso_len_(&s);
    MSTR_VALIDATE_(&s);
    memcpy(&n, s.b + 8, sizeof n);
    return n;
}

/* Pointer to the bytes. For SSO this points INTO *s, so *s must outlive it. */
static inline const char *mstr_ptr(const mstr *s)
{
    const char *p;
    if (mstr_state(s) == MSTR_SSO) return (const char *)s->b;
    MSTR_VALIDATE_(s);
    memcpy(&p, s->b, sizeof p);
    return p;
}

static inline unsigned mstr_depth_(const mstr *s) { return (unsigned)s->b[16] | ((unsigned)s->b[17] << 8); }
static inline uint64_t mstr_serial_(const mstr *s)
{
    uint64_t v = 0;
    int i;
    for (i = 4; i >= 0; i--) v = (v << 8) | s->b[18 + i];
    return v;
}

static inline mstr mstr_empty(void)
{
    mstr r;
    memset(r.b, 0, sizeof r.b);
    return r;
}

/* Copy n <= 22 bytes with overlapping fixed-size moves (no libc call): the
 * hottest primitive in the library. Reads exactly bytes [0, n) of p. */
static inline void mstr_copy_small_(unsigned char *d, const char *p, size_t n)
{
    if (n >= 16) { memcpy(d, p, 16); memcpy(d + n - 16, p + n - 16, 16); }
    else if (n >= 8) { memcpy(d, p, 8); memcpy(d + n - 8, p + n - 8, 8); }
    else if (n >= 4) { memcpy(d, p, 4); memcpy(d + n - 4, p + n - 4, 4); }
    else if (n > 0) { d[0] = (unsigned char)p[0]; d[n >> 1] = (unsigned char)p[n >> 1]; d[n - 1] = (unsigned char)p[n - 1]; }
}

/* Precondition: n <= MSTR_SSO_MAX. */
static inline mstr mstr_make_sso_(const char *p, size_t n)
{
    mstr r = mstr_empty();
    mstr_copy_small_(r.b, p, n);
    r.b[MSTR_TAG_] = (unsigned char)(n << 2); /* state 0 */
    return r;
}

/* Builds REGION/VIEW/STATIC. Precondition: n > MSTR_SSO_MAX for VIEW. */
static inline mstr mstr_make_long_(unsigned state, const char *p, size_t n,
                                   unsigned depth, uint64_t serial)
{
    mstr r = mstr_empty();
    int i;
    memcpy(r.b, &p, sizeof p);
    memcpy(r.b + 8, &n, sizeof n);
    r.b[16] = (unsigned char)(depth & 0xFFu);
    r.b[17] = (unsigned char)((depth >> 8) & 0xFFu);
    for (i = 0; i < 5; i++) r.b[18 + i] = (unsigned char)((serial >> (8 * i)) & 0xFFu);
    r.b[MSTR_TAG_] = (unsigned char)state;
    return r;
}

/* construction  */

/* Literal-only: `"" lit` makes a non-literal a compile error. <=22 -> SSO. */
static inline mstr mstr_lit_(const char *p, size_t n)
{
    if (n <= MSTR_SSO_MAX) return mstr_make_sso_(p, n);
    return mstr_make_long_(MSTR_STATIC, p, n, 0, 0);
}
#ifndef MSTR_NO_SHORT_S
#define S(lit) mstr_lit_("" lit, sizeof("" lit) - 1u)
#endif

/* n <= 22 -> SSO copy. Longer input needs the arena (M2); aborts until then. */
mstr mstr_from_n(const char *p, size_t n);
mstr mstr_from(const char *cstr);

/* queries (no allocation) */

static inline int mstr_is_empty(mstr s) { return mstr_len(s) == 0; }
int mstr_eq(mstr a, mstr b);
int mstr_cmp(mstr a, mstr b); /* -1, 0, 1: bytewise then length */
size_t mstr_find(mstr hay, mstr needle);  /* MSTR_NPOS if absent */
size_t mstr_rfind(mstr hay, mstr needle); /* MSTR_NPOS if absent */
int mstr_contains(mstr hay, mstr needle);
int mstr_starts_with(mstr s, mstr prefix);
int mstr_ends_with(mstr s, mstr suffix);

char mstr_at_(mstr s, size_t i, const char *file, int line);
#define mstr_at(s, i) mstr_at_((s), (i), MSTR_LOC_)

/* transforms*/

/* Half-open [begin, end). Out of range aborts (always). Result <= 22 bytes is
 * SSO; longer is a zero-copy VIEW. */
mstr mstr_slice_(mstr s, size_t begin, size_t end, const char *file, int line);
#define mstr_slice(s, b, e) mstr_slice_((s), (b), (e), MSTR_LOC_)

mstr mstr_trim(mstr s);  /* ASCII whitespace " \t\n\v\f\r" */
mstr mstr_ltrim(mstr s);
mstr mstr_rtrim(mstr s);

/* Zero-copy sub-range helper (no bounds check; precondition off+n <= len). */
mstr mstr_sub_(const mstr *s, size_t off, size_t n);

/* Results of <= 22 bytes are SSO, otherwise REGION in the current scope. */
mstr mstr_cat(mstr a, mstr b);
mstr mstr_replace(mstr s, mstr from, mstr to);     /* first match; no match -> s */
mstr mstr_replace_all(mstr s, mstr from, mstr to); /* empty `from` -> s unchanged */
mstr mstr_upper(mstr s);                           /* ASCII only; unchanged -> s */
mstr mstr_lower(mstr s);

/* Fused hot-path equivalent to
 * mstr_replace_all(mstr_upper(mstr_trim(s)), from, to), or with lower/keep.
 * It makes one result at most, rather than materialising every intermediate.
 * `from` is matched after the requested ASCII case mapping. */
typedef enum mstr_ascii_case {
    MSTR_ASCII_KEEP = 0,
    MSTR_ASCII_UPPER = 1,
    MSTR_ASCII_LOWER = 2
} mstr_ascii_case;
mstr mstr_trim_case_replace_all(mstr s, mstr_ascii_case ascii_case, mstr from, mstr to);

mstr mstr_repeat(mstr s, size_t times);
mstr mstr_join(mstr sep, const mstr *parts, size_t count);
#ifdef MSTR_GNU_
mstr mstr_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
mstr mstr_fmt(const char *fmt, ...);
#endif

/* scopes
 * A scope is a bookmark into the thread-local arena. Leaving it frees every
 * REGION created inside in O(1). Each thread has an implicit root scope.
 * IN LOOPS open a scope per iteration, or memory grows until the outer scope
 * ends. mstr_scope_t must stay on the thread that entered it. */
typedef struct mstr_scope_t {
    void *chunk;
    size_t off;
    uint64_t serial;
    unsigned depth;
    int active;
    const char *file;
    int line;
} mstr_scope_t;

mstr_scope_t mstr_scope_enter_(const char *file, int line);
#define mstr_scope_enter() mstr_scope_enter_(MSTR_LOC_)
void mstr_scope_leave(mstr_scope_t *s); /* idempotent */

/* Ends the scope AND returns `v` valid in the parent: SSO/STATIC and data that
 * already lives in an older scope are returned unchanged; otherwise the bytes
 * are memmove'd down to the child's bookmark. After this the scope is closed
 * (a later leave is a no-op) and every other value created inside is dead. */
mstr mstr_promote_(mstr_scope_t *s, mstr v, const char *file, int line);
#define mstr_promote(s, v) mstr_promote_((s), (v), MSTR_LOC_)
#define mstr_scope_return(s, v) mstr_promote((s), (v))

/* Raw 8-byte-aligned bytes in the current scope (uninitialised). */
void *mstr_scope_alloc(size_t n);
/* Frees this thread's arena. Run automatically at thread exit on POSIX. */
void mstr_thread_cleanup(void);

#ifdef MSTR_HAVE_CLEANUP_
/* break / return / goto out of the block still rewind (cleanup attribute).
 * `continue` and `break` bind to this macro's own loop. */
static inline void mstr_scope_cleanup_(mstr_scope_t *s) { mstr_scope_leave(s); }
#define mstr_scope                                                              \
    for (mstr_scope_t mstr_sc_ __attribute__((cleanup(mstr_scope_cleanup_))) =  \
             mstr_scope_enter(), *mstr_once_ = &mstr_sc_;                       \
         mstr_once_; mstr_once_ = 0)
#else
/* Strict C / MSVC: an early `return` or `break` skips the rewind; the next
 * enclosing scope rewinds to its own older bookmark, so nothing is corrupted,
 * only reclaimed later. `break`/`continue` bind to this macro's loop. */
#define mstr_scope                                                              \
    for (mstr_scope_t mstr_sc_ = mstr_scope_enter(), *mstr_once_ = &mstr_sc_;   \
         mstr_once_; mstr_scope_leave(&mstr_sc_), mstr_once_ = 0)
#endif

/* iteration (zero allocation) 
 * mstr_each(tok, s, delim) { ... }: tokens are slices of s (SSO or VIEW).
 * Policy: "a,b,,c" -> "a" "b" "" "c"; "" -> one empty token; empty delim ->
 * the whole string once. mstr_lines: splits on "\n", strips a "\r" before it,
 * and a trailing newline does not yield an extra empty line ("" -> none).
 * `break` works normally. */
int mstr_iter_next_(mstr *tok, mstr *it);
static inline mstr mstr_lines_mode_(void)
{
    mstr m = mstr_empty();
    m.b[1] = 1;
    return m;
}
#define mstr_each(tok, s, delim)                                                \
    for (mstr tok = mstr_empty(), mstr_it_[3] = {(s), (delim), mstr_empty()};   \
         mstr_iter_next_(&tok, mstr_it_);)
#define mstr_lines(tok, s)                                                      \
    for (mstr tok = mstr_empty(),                                               \
              mstr_it_[3] = {(s), mstr_empty(), mstr_lines_mode_()};            \
         mstr_iter_next_(&tok, mstr_it_);)

/* Array of tokens (VIEW/SSO values) allocated in the CURRENT scope. */
mstr *mstr_split(mstr s, mstr delim, size_t *count);

/* C interop 
 * SSO -> inline buffer (valid as long as *s); REGION/STATIC -> ptr; VIEW -> ptr
 * if the next byte is '\0', else a NUL-terminated copy in the current scope.
 * Embedded NULs truncate what C code sees. There is no scratch ring. */
const char *mstr_cstr_p(const mstr *s);
#ifndef __cplusplus
#define MSTR_CSTR(e) mstr_cstr_p((mstr[1]){(e)}) /* valid to end of enclosing block */
#endif

/* allocator hooks 
 * All library memory (arena chunks, boxes, shares, scope bookkeeping) comes
 * from here. Must be set BEFORE the first library allocation, from a single
 * thread (aborts otherwise). Memory must be aligned like malloc's. `realloc`
 * may be NULL (grow = alloc + copy + free). The defaults are libc. */
typedef struct mstr_allocator {
    void *(*alloc)(size_t n, void *ctx);
    void *(*realloc)(void *p, size_t n, void *ctx);
    void (*free)(void *p, void *ctx);
    void *ctx;
} mstr_allocator;
void mstr_set_allocator(mstr_allocator a);

/* Called when the allocator returns NULL. It must not return (longjmp, exit,
 * abort...); if it does, the library aborts. Default: print and abort.
 * Same "before first use" rule as the allocator. */
typedef void (*mstr_oom_handler)(size_t bytes, void *ctx);
void mstr_set_oom_handler(mstr_oom_handler h, void *ctx);

/* ownership: the ONLY owning type 
 * A box owns heap memory that outlives scopes. Strings <= 22 bytes are stored
 * inline (no allocation). Boxes are bit-copyable but MOVE-ONLY BY CONVENTION:
 * free exactly one copy. Freeing the same box object twice is a no-op (free
 * nulls it); freeing a stale COPY is detected in checked mode (best effort:
 * the last 64 frees per thread). A zeroed box is a valid empty box. */
typedef struct mstr_box {
    char *ptr;     /* heap bytes (len+1, NUL-terminated) or NULL */
    size_t len;
    mstr small_;   /* inline value for len <= 22 */
    unsigned kind; /* 0 empty, 1 small, 2 heap, 3 freed */
} mstr_box;

mstr_box mstr_own(mstr s);             /* copies; does not consume s */
mstr mstr_unbox(const mstr_box *b);    /* SSO copy, or a VIEW into the box buffer */
void mstr_box_free(mstr_box *b);       /* idempotent */
#ifdef MSTR_GNU_
#define mstr_box_auto mstr_box __attribute__((cleanup(mstr_box_free)))
#else
#define mstr_box_auto mstr_box
#endif

/* sharing across threads 
 * An immutable, atomically reference-counted buffer. mstr_share copies `s`
 * (refcount 1). Hand the pointer to another thread after retaining for it; each
 * holder releases once; the last release frees. A VIEW from mstr_share_get is
 * valid on any thread, but only while that thread holds a reference. Plain
 * REGION/VIEW values tied to a scope must never cross threads. */
typedef struct mstr_shared mstr_shared;
mstr_shared *mstr_share(mstr s);
mstr_shared *mstr_share_retain(mstr_shared *sh); /* returns sh */
void mstr_share_release(mstr_shared *sh);
mstr mstr_share_get(const mstr_shared *sh);      /* SSO copy or VIEW */
size_t mstr_share_refs_(const mstr_shared *sh);  /* diagnostics only */

/*  UTF-8 layer (optional; the core stays byte-oriented, R5) 
 * INVALID-INPUT POLICY: replace, never reject. Decoding follows the Unicode
 * "maximal subpart" practice: every ill-formed sequence (stray continuation,
 * overlong, surrogate, > U+10FFFF, truncated) consumes 1..3 bytes and decodes
 * as U+FFFD. Count, iteration and slicing all use these same units, so indexes
 * agree. mstr_u8_slice returns the ORIGINAL bytes (invalid ones are preserved,
 * never rewritten); iteration yields U+FFFD (EF BF BD) in their place.
 * A slice never splits a sequence. */
uint32_t mstr_u8_decode(const char *p, size_t n, size_t *adv, int *ok); /* n >= 1; *adv >= 1 */
int mstr_u8_valid(mstr s);                 /* 1 iff the whole string is well-formed UTF-8 */
size_t mstr_u8_count(mstr s);              /* decoded units (== code points when valid) */
mstr mstr_u8_from_cp(uint32_t cp);         /* encode; invalid cp -> U+FFFD */
uint32_t mstr_u8_cp(mstr ch);              /* first code point of ch (U+FFFD if ill-formed) */
/* Half-open [begin, end) in code point units; out of range aborts. */
mstr mstr_u8_slice_(mstr s, size_t begin, size_t end, const char *file, int line);
#define mstr_u8_slice(s, b, e) mstr_u8_slice_((s), (b), (e), MSTR_LOC_)
int mstr_u8_next_(mstr *ch, mstr *it);
/* mstr_u8_each(ch, s) { uint32_t cp = mstr_u8_cp(ch); ... }  zero allocation;
 * ch is a 1-4 byte SSO value. */
#define mstr_u8_each(ch, s)                                                     \
    for (mstr ch = mstr_empty(), mstr_it_[3] = {(s), mstr_empty(), mstr_empty()}; \
         mstr_u8_next_(&ch, mstr_it_);)

/* fused pipeline 
 * MSTR_PIPE(x, f1, f2, f3(args)) == f3(f2(f1(x)), args) byte for byte, but each
 * step's result is slid down over its dead input inside the arena
 * (two buffers: the live value and the step's output), so a chain keeps only
 * the final result instead of every intermediate. Steps are written without
 * the value argument: `mstr_trim` (unary) or `mstr_replace(from, to)`.
 *   unary:   mstr_trim mstr_ltrim mstr_rtrim mstr_upper mstr_lower
 *   with args: mstr_cat(b) mstr_replace(f,t) mstr_replace_all(f,t) mstr_repeat(n)
 *              mstr_slice_range(b,e) mstr_u8_slice_range(b,e)
 * (mstr_slice and mstr_u8_slice are macros that capture __FILE__/__LINE__ and
 * cannot be steps; use the *_range spellings.) Up to 10 steps. Add your own
 * with `#define MSTR_P_myfn(...) myfn(MSTR_PIPE_V, __VA_ARGS__)` (or, for a
 * unary one, `#define MSTR_P_myfn myfn(MSTR_PIPE_V)`).
 * MSTR_PIPE needs GNU statement expressions (gcc, clang, clang-cl). Everywhere
 * else use the identical explicit API:
 *     mstr_pipe_t p = mstr_pipe_begin_(x);
 *     mstr_pipe_hop_(&p, mstr_replace(p.v, a, b));
 *     mstr r = mstr_pipe_end_(&p);
 * HAZARD: every hop discards ALL arena data allocated since mstr_pipe_begin_
 * except the new live value (that is how intermediates disappear). Inside a
 * pipe, step arguments and temporaries are therefore dead after their step;
 * never keep another value created after begin. Values created before begin
 * are untouched. Do not abandon a pipe half-way, and keep it inside one scope. */
typedef struct mstr_pipe_t {
    void *chunk;
    size_t off;
    mstr v;
} mstr_pipe_t;
mstr_pipe_t mstr_pipe_begin_(mstr x);
mstr mstr_pipe_hop_(mstr_pipe_t *p, mstr step_result); /* stores and returns the new live value */
mstr mstr_pipe_end_(mstr_pipe_t *p);

#define MSTR_PIPE_V mstr_pp_.v
#define MSTR_P_mstr_trim mstr_trim(MSTR_PIPE_V)
#define MSTR_P_mstr_ltrim mstr_ltrim(MSTR_PIPE_V)
#define MSTR_P_mstr_rtrim mstr_rtrim(MSTR_PIPE_V)
#define MSTR_P_mstr_upper mstr_upper(MSTR_PIPE_V)
#define MSTR_P_mstr_lower mstr_lower(MSTR_PIPE_V)
#define MSTR_P_mstr_cat(...) mstr_cat(MSTR_PIPE_V, __VA_ARGS__)
#define MSTR_P_mstr_replace(...) mstr_replace(MSTR_PIPE_V, __VA_ARGS__)
#define MSTR_P_mstr_replace_all(...) mstr_replace_all(MSTR_PIPE_V, __VA_ARGS__)
#define MSTR_P_mstr_repeat(...) mstr_repeat(MSTR_PIPE_V, __VA_ARGS__)
#define MSTR_P_mstr_slice_range(b, e) mstr_slice(MSTR_PIPE_V, b, e)
#define MSTR_P_mstr_u8_slice_range(b, e) mstr_u8_slice(MSTR_PIPE_V, b, e)

#define MSTR_PS_(s) (void)mstr_pipe_hop_(&mstr_pp_, MSTR_P_##s);
#define MSTR_PS1_(a) MSTR_PS_(a)
#define MSTR_PS2_(a, ...) MSTR_PS_(a) MSTR_PS1_(__VA_ARGS__)
#define MSTR_PS3_(a, ...) MSTR_PS_(a) MSTR_PS2_(__VA_ARGS__)
#define MSTR_PS4_(a, ...) MSTR_PS_(a) MSTR_PS3_(__VA_ARGS__)
#define MSTR_PS5_(a, ...) MSTR_PS_(a) MSTR_PS4_(__VA_ARGS__)
#define MSTR_PS6_(a, ...) MSTR_PS_(a) MSTR_PS5_(__VA_ARGS__)
#define MSTR_PS7_(a, ...) MSTR_PS_(a) MSTR_PS6_(__VA_ARGS__)
#define MSTR_PS8_(a, ...) MSTR_PS_(a) MSTR_PS7_(__VA_ARGS__)
#define MSTR_PS9_(a, ...) MSTR_PS_(a) MSTR_PS8_(__VA_ARGS__)
#define MSTR_PS10_(a, ...) MSTR_PS_(a) MSTR_PS9_(__VA_ARGS__)
#define MSTR_PIPE_PICK_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, N, ...) N
#define MSTR_PIPE_STEPS_(...)                                                   \
    MSTR_PIPE_PICK_(__VA_ARGS__, MSTR_PS10_, MSTR_PS9_, MSTR_PS8_, MSTR_PS7_,   \
                    MSTR_PS6_, MSTR_PS5_, MSTR_PS4_, MSTR_PS3_, MSTR_PS2_,      \
                    MSTR_PS1_, _)(__VA_ARGS__)
#ifdef MSTR_GNU_
#define MSTR_PIPE(x, ...)                                                       \
    __extension__({                                                             \
        mstr_pipe_t mstr_pp_ = mstr_pipe_begin_(x);                             \
        MSTR_PIPE_STEPS_(__VA_ARGS__)                                           \
        mstr_pipe_end_(&mstr_pp_);                                              \
    })
#endif

/* Optional arena counters (compile the library with -DMSTR_STATS): `bumps` is
 * the number of arena allocations, `bytes` their rounded size. For benchmarks. */
typedef struct mstr_stats {
    unsigned long long bumps, bytes;
} mstr_stats;
mstr_stats mstr_stats_get_(void);
void mstr_stats_reset_(void);

/* Test/diagnostic hooks for the arena (not part of the stable API). */
size_t mstr_debug_arena_used_(void);
size_t mstr_debug_chunk_count_(void);
size_t mstr_debug_free_chunks_(void);

#ifdef __cplusplus
}
/* C++: temporary lives until the end of the full-expression, e.g. puts(MSTR_CSTR(x)). */
struct mstr_cstr_ {
    mstr v;
    operator const char *() const { return mstr_cstr_p(&v); }
};
inline mstr_cstr_ mstr_cstr_make_(mstr v)
{
    mstr_cstr_ r;
    r.v = v;
    return r;
}
#define MSTR_CSTR(e) mstr_cstr_make_(e)

/* ---- member definitions (declared in the union above) ---- */
inline size_t mstr::size() const { return mstr_len(*this); }
inline const char *mstr::data() const { return mstr_ptr(this); }
inline char mstr::operator[](size_t i) const { return mstr_at_(*this, i, static_cast<const char *>(nullptr), 0); }
inline bool mstr::operator==(const mstr &o) const { return mstr_eq(*this, o) != 0; }
inline bool mstr::operator!=(const mstr &o) const { return mstr_eq(*this, o) == 0; }
#ifdef MSTR_CXX17_
inline mstr::operator std::string_view() const { return std::string_view(data(), size()); }
inline mstr::operator std::string() const { return std::string(data(), size()); }
inline bool mstr::operator==(std::string_view o) const { return std::string_view(data(), size()) == o; }
inline mstr mstr::from(std::string_view v) { return mstr_from_n(v.data(), v.size()); }
#endif
#endif
#endif /* MSTR_H */
