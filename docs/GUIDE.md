# mstr user guide

This guide teaches you to use mstr from nothing to confident. Read sections 1 to 6 first;
they cover 90% of everyday use. Every complete program here (the ones starting with
`#include`) is compiled and run by the project's tests, so you can copy it as is.

* [1. Get it into your project](#1-get-it-into-your-project)
* [2. Your first program](#2-your-first-program)
* [3. The mental model](#3-the-mental-model)
* [4. Making strings](#4-making-strings)
* [5. Asking questions about strings](#5-asking-questions-about-strings)
* [6. Making new strings](#6-making-new-strings)
* [7. Scopes in depth](#7-scopes-in-depth)
* [8. Splitting and iterating](#8-splitting-and-iterating)
* [9. Talking to C code](#9-talking-to-c-code)
* [10. Keeping a string alive: `mstr_box`](#10-keeping-a-string-alive-mstr_box)
* [11. Sharing between threads: `mstr_share`](#11-sharing-between-threads-mstr_share)
* [12. Pipelines: `MSTR_PIPE`](#12-pipelines-mstr_pipe)
* [13. UTF-8](#13-utf-8)
* [14. Checked mode: finding your mistakes](#14-checked-mode-finding-your-mistakes)
* [15. Allocators and out-of-memory](#15-allocators-and-out-of-memory)
* [16. C++](#16-c)
* [17. Mistakes to avoid](#17-mistakes-to-avoid)
* [18. Cheat sheet](#18-cheat-sheet)

---

## 1. Get it into your project

mstr is one header and one source file. 64-bit targets only, C99 or later (C++17 for the wrapper).

**Copy two files** (simplest):

```sh
cp include/mstr.h src/mstr.c  your_project/
cc -std=c99 -O2 your_program.c mstr.c -o your_program
```

**Or use CMake:**

```cmake
add_subdirectory(mstr)                       # path to this repository
target_link_libraries(your_target PRIVATE mstr)
```

For development, build with `-DMSTR_CHECKED` (see [section 14](#14-checked-mode-finding-your-mistakes)).
Link with `-pthread` if you use threads.

## 2. Your first program

```c
#include <stdio.h>
#include "mstr.h"

int main(void)
{
    mstr_scope {
        mstr name  = mstr_from("  ada lovelace  ");
        mstr clean = mstr_trim(name);
        mstr loud  = mstr_upper(clean);
        mstr msg   = mstr_cat(S("Hello, "), loud);
        puts(MSTR_CSTR(msg));
    }
    return 0;
}
```

Output: `Hello, ADA LOVELACE`. Things to notice:

* There is no `free` anywhere, and none is possible to forget.
* Every `mstr` is a plain value. `clean` and `name` are different values and both stay valid.
* The braces after `mstr_scope` are the *lifetime*. When they close, everything allocated inside is gone.
* `S("Hello, ")` is a literal. It costs nothing at run time.
* `MSTR_CSTR(msg)` gives you a normal NUL-terminated `const char *` for `puts`.

## 3. The mental model

Hold on to these three rules and the rest follows.

1. **An `mstr` is a value, like an `int`.** It is 24 bytes. You can copy it, pass it,
   return it. Nothing is ever modified in place: every operation returns a new value.
   Nothing "owns" a string, and no function frees one.
2. **Lifetime is the scope.** Short strings (22 bytes or fewer) live inside the value itself and have no
   lifetime problem at all. Longer strings live in a per-thread arena. When the `mstr_scope` they were
   created in ends, the arena rewinds and they are gone, all at once.
3. **If something must outlive its scope, say so explicitly** with `mstr_scope_return`
   ([section 7](#7-scopes-in-depth)) or `mstr_own` ([section 10](#10-keeping-a-string-alive-mstr_box)).

What a value can be:

| kind | where the bytes are | how long they live |
|---|---|---|
| SSO | inside the 24-byte value (22 bytes or fewer) | as long as you keep the value |
| STATIC | a string literal | forever |
| REGION | the scope's arena | until the scope ends |
| VIEW | inside another string's bytes (zero copy) | as long as that other string |

You never have to ask which kind you have. The one thing to remember: **a long string made inside a scope
must not be used after that scope ends.** Short strings are always safe. Because you cannot always tell by
looking whether a string is short, treat every string from an inner scope as dead once the scope closes. Checked
mode will catch you if you forget.

**The Law of Small Results.** If a result is 22 bytes or fewer it is *always* a self-contained value, even
when it was sliced out of a gigantic string. So `mstr_slice(huge, 0, 5)` never keeps `huge` alive.

## 4. Making strings

| you write | you get | allocates? |
|---|---|---|
| `S("literal")` | a literal (the argument must be a real literal; anything else is a compile error) | never |
| `mstr_from(cstr)` | a copy of a C string | only if longer than 22 bytes |
| `mstr_from_n(ptr, len)` | a copy of `len` bytes (may contain `'\0'`) | only if longer than 22 bytes |
| `mstr_fmt("%s=%d", a, b)` | printf-style formatting | only if the result is longer than 22 bytes |
| `mstr_empty()` | the empty string | never |
| a zeroed `mstr` | also the empty string | never |

```c
#include <stdio.h>
#include "mstr.h"

int main(void)
{
    mstr_scope {
        mstr a = S("literal");
        mstr b = mstr_from("from a C string");
        mstr c = mstr_from_n("bin\0ary", 7);        // embedded NUL is fine: mstr tracks the length
        mstr d = mstr_fmt("%s has %zu bytes", "c", mstr_len(c));
        printf("%zu %zu %zu %s\n", mstr_len(a), mstr_len(b), mstr_len(c), MSTR_CSTR(d));
    }
    return 0;
}
```

Output: `7 15 7 c has 7 bytes`.

Because the length is stored, mstr never calls `strlen` on its own data and handles binary data.
(`mstr_from` itself uses `strlen` on *your* C string, naturally.)

## 5. Asking questions about strings

None of these allocate.

```c
size_t mstr_len(mstr s);
int    mstr_is_empty(mstr s);
int    mstr_eq(mstr a, mstr b);               // 1 if equal
int    mstr_cmp(mstr a, mstr b);              // -1, 0, 1 (bytewise, then length)
size_t mstr_find(mstr hay, mstr needle);      // index, or MSTR_NPOS
size_t mstr_rfind(mstr hay, mstr needle);     // last index, or MSTR_NPOS
int    mstr_contains(mstr hay, mstr needle);
int    mstr_starts_with(mstr s, mstr prefix);
int    mstr_ends_with(mstr s, mstr suffix);
char   mstr_at(mstr s, size_t i);             // bounds-checked: aborts if i >= len
```

Search results are plain byte indexes. An empty needle matches at index 0 (`rfind`: at the end).

```c
#include <stdio.h>
#include "mstr.h"

int main(void)
{
    mstr line = S("key = value");
    size_t eq = mstr_find(line, S("="));
    if (eq == MSTR_NPOS) { puts("no ="); return 1; }
    mstr_scope {
        mstr key = mstr_trim(mstr_slice(line, 0, eq));
        mstr val = mstr_trim(mstr_slice(line, eq + 1, mstr_len(line)));
        printf("[%s] -> [%s]\n", MSTR_CSTR(key), MSTR_CSTR(val));
    }
    return 0;
}
```

Output: `[key] -> [value]`.

## 6. Making new strings

Every operation returns a new value and leaves its inputs alone.

```c
mstr mstr_slice(mstr s, size_t begin, size_t end);   // [begin, end): out of range aborts
mstr mstr_trim(mstr s);   mstr mstr_ltrim(mstr s);   mstr mstr_rtrim(mstr s);   // " \t\n\v\f\r"
mstr mstr_cat(mstr a, mstr b);
mstr mstr_replace(mstr s, mstr from, mstr to);       // first match only
mstr mstr_replace_all(mstr s, mstr from, mstr to);
mstr mstr_upper(mstr s);  mstr mstr_lower(mstr s);   // ASCII only
mstr mstr_trim_case_replace_all(mstr s, mstr_ascii_case mode, mstr from, mstr to);
mstr mstr_repeat(mstr s, size_t times);
mstr mstr_join(mstr sep, const mstr *parts, size_t count);
```

What each one costs (this is the design's promise: *each operation picks the cheapest valid result*):

| operation | result |
|---|---|
| slice or trim, result 22 bytes or fewer | a new self-contained short value (no allocation) |
| slice or trim, result longer than 22 | a zero-copy VIEW into the source |
| `cat`, `replace`, `upper`, `repeat`, `join`... | short value if 22 bytes or fewer, otherwise a new REGION in the current scope |
| `replace` / `replace_all` with no match, `upper` / `lower` with nothing to change | the input itself, no allocation |
| `find`, `eq`, `cmp`, `starts_with`... | nothing allocated |

Details worth knowing:

* `mstr_slice` takes **half-open ranges**: `mstr_slice(s, 2, 5)` is bytes 2, 3, 4.
* An out-of-range slice **always aborts**, even in release builds. Silent wrong data is worse than a crash.
* `mstr_replace_all` with an empty `from` returns the input unchanged. Matches do not overlap: replacing
  `"aa"` in `"aaa"` gives one replacement.
* `upper`/`lower` change only ASCII letters, so UTF-8 text is never corrupted (see [section 13](#13-utf-8)).

For a hot normalisation path, fuse trim, case mapping, and replacement rather than nesting three
general transforms:

```c
mstr key = mstr_trim_case_replace_all(line, MSTR_ASCII_UPPER, S(" "), S("_"));
// exactly: mstr_replace_all(mstr_upper(mstr_trim(line)), S(" "), S("_"))
```

`MSTR_ASCII_KEEP`, `MSTR_ASCII_UPPER`, and `MSTR_ASCII_LOWER` select the mapping. The pattern is
matched after mapping; the fused operation makes at most one result allocation and performs no
intermediate `mstr` calls.

## 7. Scopes in depth

### The basic scope

```c
mstr_scope {
    // anything long you make here ...
}   // ... is freed here, in O(1)
```

`mstr_scope` is a statement (it expands to a one-iteration `for`). On gcc and clang, `break`, `return` and `goto`
out of the block still free the memory, because the library uses the `cleanup` attribute.

### Rule: in a loop, put the scope *inside* the loop

```c
#include <stdio.h>
#include <string.h>
#include "mstr.h"

int main(void)
{
    char buf[256];
    size_t lines = 0;
    while (fgets(buf, sizeof buf, stdin)) {
        mstr_scope {                                   // one scope per iteration
            mstr line = mstr_trim(mstr_from_n(buf, strlen(buf)));
            if (!mstr_is_empty(line)) { puts(MSTR_CSTR(line)); lines++; }
        }                                              // this iteration's memory is gone
    }
    fprintf(stderr, "%zu lines\n", lines);
    return 0;
}
```

If the scope were *outside* the loop, every iteration's strings would pile up until the loop ended.
That is the most common way to waste memory with mstr.

### Returning a string out of a scope

A string made inside a scope dies with it. To hand one back, use a scope handle and `mstr_scope_return`:

```c
#include <stdio.h>
#include "mstr.h"

static mstr greeting(mstr who)
{
    mstr_scope_t sc = mstr_scope_enter();               // a scope you control by hand
    mstr tmp  = mstr_cat(S("Hello, dear and very honoured "), who);   // scratch work
    mstr loud = mstr_upper(tmp);
    return mstr_scope_return(&sc, loud);                // ends the scope; `loud` moves to the caller's scope
}

int main(void)
{
    mstr_scope {
        mstr g = greeting(S("guest"));
        puts(MSTR_CSTR(g));
    }
    return 0;
}
```

What `mstr_scope_return` does: it ends the scope (freeing `tmp`) and copies `loud` into the *parent* scope,
cheaply, by sliding the bytes down. If the value is short, or already lives somewhere that survives (a literal, an
older scope), it is returned unchanged. **After it returns, the scope is closed and every other value made inside is
dead.** `mstr_promote` is the same call under another name.

Manual handles: `mstr_scope_enter()` and `mstr_scope_leave(&sc)` (safe to call twice). Prefer the `mstr_scope { }`
block when you do not need to return something.

### The implicit root scope

If you create strings outside any scope, they go in a per-thread root scope that is only freed when the thread
exits. That is fine for a small program, but the checked build warns about it once. Open a scope.

### On compilers without `cleanup` (strict C, MSVC)

`mstr_scope` still works, with one difference: an early `return` or `break` skips the rewind. Nothing breaks;
the enclosing scope simply reclaims the memory later. Inside the macro, `break` and `continue` apply to the
macro's own loop.

## 8. Splitting and iterating

### `mstr_each`: lazy, zero allocation

```c
#include <stdio.h>
#include "mstr.h"

int main(void)
{
    mstr_each(tok, S("a,b,,c"), S(",")) {
        printf("[%s]", MSTR_CSTR(tok));
    }
    puts("");
    return 0;
}
```

Output: `[a][b][][c]`. Tokens are slices of the source, so nothing is allocated. `break` works.
You can nest `mstr_each`.

Rules: `"a,b,,c"` gives four tokens (empty fields are kept); `""` gives one empty token; an empty delimiter gives
the whole string once; the delimiter may be longer than one byte.

### `mstr_lines`

```c
mstr_lines(line, text) { ... }      // splits on "\n", strips a "\r" before it
```

A trailing newline does not produce an extra empty line, and `""` produces no lines.

### `mstr_split`: when you really need an array

```c
size_t n;
mstr *parts = mstr_split(s, S(","), &n);    // array allocated in the CURRENT scope
for (size_t i = 0; i < n; i++) { /* parts[i] */ }
```

The array and its tokens live until the scope ends. Prefer `mstr_each` when you only need to loop.

### Joining

```c
mstr parts[3] = { S("a"), S("b"), S("c") };
mstr joined = mstr_join(S(", "), parts, 3);       // "a, b, c"
```

## 9. Talking to C code

Calling a function that wants a `const char *`:

```c
puts(MSTR_CSTR(s));
```

| the string is | `MSTR_CSTR` returns |
|---|---|
| short | a pointer into the value itself |
| a literal or a region | the data pointer (these are always NUL-terminated) |
| a VIEW ending where the buffer ends | the data pointer |
| any other VIEW (no NUL after it) | a NUL-terminated **copy** in the current scope |

Two rules: the pointer is only valid while the string (and its scope) is, and a string that contains `'\0'`
appears truncated to C code. In C++, `MSTR_CSTR(s)` is a temporary valid until the end of the full expression.
For more control use `mstr_cstr_p(const mstr *)`.

Going the other way, `mstr_from(cstr)` and `mstr_from_n(ptr, len)` copy bytes in.

## 10. Keeping a string alive: `mstr_box`

A **box** is the one owning type. Use it when data must outlive any scope: a cache, a config value, a global.

```c
#include <stdio.h>
#include "mstr.h"

static mstr_box g_title;                         // zeroed: a valid empty box

static void set_title(mstr s)
{
    mstr_box_free(&g_title);                     // release the old one
    g_title = mstr_own(s);                       // an independent copy on the heap
}

int main(void)
{
    mstr_scope {
        set_title(mstr_cat(S("a title built in a scope "), S("that is about to end")));
    }                                            // the scope is gone; the box is not
    mstr t = mstr_unbox(&g_title);               // a borrowed view of the box's bytes
    puts(MSTR_CSTR(t));
    mstr_box_free(&g_title);                     // free exactly once
    return 0;
}
```

* `mstr_own(s)` copies. It does not consume `s`. Strings of 22 bytes or fewer need no allocation.
* `mstr_unbox(&b)` is a **borrow**. It is valid until the box is freed.
* `mstr_box_free(&b)` is idempotent: it nulls the box, so freeing the same box object twice is harmless.
* On gcc and clang, `mstr_box_auto b = mstr_own(s);` frees automatically at the end of the block.
* **Boxes are move-only by convention.** The compiler lets you copy a box struct, but only free one copy.
  Freeing a stale copy is a double free; checked mode catches it (best effort).

## 11. Sharing between threads: `mstr_share`

Ordinary `mstr` values must **not** cross threads: their lifetime belongs to one thread's scopes. To pass
text to another thread, make a shared, atomically reference-counted buffer:

```c
#include <pthread.h>
#include <stdio.h>
#include "mstr.h"

static void *worker(void *arg)
{
    mstr_shared *sh = (mstr_shared *)arg;        // this thread owns one reference
    mstr text = mstr_share_get(sh);              // a view; valid while we hold the reference
    printf("worker sees %zu bytes\n", mstr_len(text));
    mstr_share_release(sh);                      // drop our reference
    return 0;
}

int main(void)
{
    pthread_t th;
    mstr_shared *sh = mstr_share(S("text that several threads may read at the same time"));
    pthread_create(&th, 0, worker, mstr_share_retain(sh));   // retain ONE reference per thread, before it starts
    pthread_join(th, 0);
    mstr_share_release(sh);                      // main's own reference; the last release frees
    return 0;
}
```

The pattern: **`retain` once for every thread you hand it to, `release` once in every thread when done.**
The buffer is immutable, so reading it needs no locks. Put each thread's own work inside that thread's own scopes.

## 12. Pipelines: `MSTR_PIPE`

A chain of operations normally leaves every intermediate result alive until the scope ends. `MSTR_PIPE` runs the
same chain but discards each step's dead input as it goes:

```c
#include <stdio.h>
#include "mstr.h"

int main(void)
{
    mstr_scope {
        mstr id = MSTR_PIPE(mstr_from("  John Ronald  Tolkien "),
                            mstr_trim, mstr_upper, mstr_replace_all(S(" "), S("_")));
        puts(MSTR_CSTR(id));                     // JOHN_RONALD__TOLKIEN
    }
    return 0;
}
```

It means exactly `mstr_replace_all(mstr_upper(mstr_trim(x)), S(" "), S("_"))`, byte for byte, but it keeps
only the final result alive afterwards (about half the memory held in the benchmark, at slightly higher cost in
time). Use it when a chain produces big intermediates. For short chains, nested calls are just as good.

How to write steps: **leave out the value argument.**

| step | write it as |
|---|---|
| unary | `mstr_trim`, `mstr_ltrim`, `mstr_rtrim`, `mstr_upper`, `mstr_lower` |
| with arguments | `mstr_cat(b)`, `mstr_replace(from, to)`, `mstr_replace_all(from, to)`, `mstr_repeat(n)` |
| slices | `mstr_slice_range(begin, end)`, `mstr_u8_slice_range(begin, end)` |

Up to 10 steps. To add your own function `myfn(mstr v, int k)`:

```c
#define MSTR_P_myfn(...) myfn(MSTR_PIPE_V, __VA_ARGS__)      // then write:  MSTR_PIPE(x, myfn(3))
```

**Warning:** each step discards everything allocated since the pipe began except the live value. Inside a pipe, do not
keep any other value created there. Values that existed before the pipe are untouched.

`MSTR_PIPE` needs GNU statement expressions (gcc, clang, clang-cl). Everywhere else use the explicit form
with the same behaviour:

```c
mstr_pipe_t p = mstr_pipe_begin_(x);
(void)mstr_pipe_hop_(&p, mstr_trim(p.v));
(void)mstr_pipe_hop_(&p, mstr_upper(p.v));
mstr result = mstr_pipe_end_(&p);
```

## 13. UTF-8

The core treats strings as bytes, which is what you want for most work. The `mstr_u8_*` functions add
code-point awareness when you need it:

```c
#include <stdio.h>
#include "mstr.h"

int main(void)
{
    mstr s = S("h\xC3\xA9llo \xE6\x97\xA5\xE6\x9C\xAC");     // "héllo 日本": 13 bytes, 8 code points
    printf("bytes=%zu code points=%zu valid=%d\n", mstr_len(s), mstr_u8_count(s), mstr_u8_valid(s));

    mstr_scope {
        mstr first3 = mstr_u8_slice(s, 0, 3);                 // by code point index; never splits a character
        printf("first 3 = %s\n", MSTR_CSTR(first3));          // hél
    }
    mstr_u8_each(ch, s) {                                     // one code point at a time
        printf("U+%04X ", (unsigned)mstr_u8_cp(ch));
    }
    puts("");
    return 0;
}
```

Output: `bytes=13 code points=8 valid=1`, then `first 3 = hél`, then
`U+0068 U+00E9 U+006C U+006C U+006F U+0020 U+65E5 U+672C`.

**Invalid input is replaced, never rejected.** Bad bytes (stray continuation bytes, overlong forms, surrogates,
values above U+10FFFF, truncated sequences) each count as one unit and come out as U+FFFD when iterating.
`count`, `each` and `slice` use the same units, so indexes always agree. `mstr_u8_slice` returns your **original
bytes** (it never rewrites them). Use `mstr_u8_valid` if you want to reject bad input yourself.

Out-of-range code point slices abort, like byte slices. `mstr_u8_from_cp(cp)` encodes one code point.
`upper`/`lower` change ASCII only. There is no Unicode case mapping, collation, or normalization.

## 14. Checked mode: finding your mistakes

Compile with `-DMSTR_CHECKED` while developing and testing. Release builds skip every check and pay nothing.

```sh
cc -std=c99 -DMSTR_CHECKED -g -fsanitize=address,undefined your_program.c mstr.c
```

In checked mode, every long string remembers which scope made it, and every operation verifies that scope is still alive.

```c
#include <stdio.h>
#include "mstr.h"
int main(void)
{
    mstr leaked;
    mstr_scope { leaked = mstr_repeat(S("0123456789"), 5); }
    puts(MSTR_CSTR(leaked));        // abort!
    return 0;
}
```

Saved as `demo.c`, this prints and aborts:

```
mstr: string from scope entered at demo.c:6 used after it ended (depth 1)
```

The message names the line where the dead scope was *entered*, which is where to look for the bug.

What checked mode catches:

| mistake | message contains |
|---|---|
| using a string after its scope ended | `string from scope entered at FILE:LINE used after it ended` |
| using a string after `mstr_scope_return` moved/freed its source | the same message |
| a string from another thread | `... or on a different thread than the one that created it` |
| leaving a scope on the wrong thread, or twice out of order | `scope is not live on this thread` |
| slice or `at` out of range | `range out of bounds` (reported in every build) |
| length overflow in `repeat`/`cat`/`join` | `string length overflow` (every build) |
| freeing a stale copy of a box | `double free` (best effort: the last 64 frees per thread) |
| using a freed box | `the box was already freed` |
| `mstr_promote` with an inner scope still open | `unclosed inner scopes` (every build) |

Also in checked mode: the first allocation in the implicit root scope prints a warning
(silence it with the environment variable `MSTR_NO_ROOT_WARNING`).

A limit to know: a VIEW into a `mstr_box` is not tied to a scope, so checked mode cannot see "the box was freed
but I still hold a view". Checked builds overwrite freed box bytes with `0xDD` so stale reads are obvious, and
AddressSanitizer finds the rest.

## 15. Allocators and out-of-memory

By default mstr uses `malloc`. To plug in your own allocator, **before any other mstr call** and from one thread:

```c
static void *my_alloc(size_t n, void *ctx)               { return malloc(n); }
static void *my_realloc(void *p, size_t n, void *ctx)    { return realloc(p, n); }   // may be NULL
static void  my_free(void *p, void *ctx)                 { free(p); }

mstr_allocator a = { my_alloc, my_realloc, my_free, NULL };
mstr_set_allocator(a);                                    // aborts if called after first use
```

Memory must be aligned like `malloc`'s. If your allocator returns `NULL`, mstr calls the out-of-memory handler,
which by default prints a message and aborts. Replace it (also before first use):

```c
static void on_oom(size_t bytes, void *ctx) { /* log, then exit or longjmp; must not return */ }
mstr_set_oom_handler(on_oom, NULL);
```

If the handler returns, mstr aborts. Each thread's arena is freed when the thread exits (POSIX); elsewhere
call `mstr_thread_cleanup()` before a thread ends.

## 16. C++

```cpp
#include <cstdio>
#include "mstr.hpp"                                   // C++17

int main()
{
    mstr::Scope scope;                                // RAII scope: frees everything at the closing brace
    mstr s = mstr::from(std::string_view("some text longer than twenty-two bytes"));

    std::string_view v = s;                           // implicit
    std::string str = static_cast<std::string>(s);    // explicit
    char c = s[0];                                    // bounds-checked
    bool same = (s == std::string_view("x"));

    mstr::Box keep(s);                                // move-only owner
    mstr view = keep.view();                          // borrowed
    std::printf("%zu %zu %c %d %zu\n", v.size(), str.size(), c, same, view.size());
}
```

* `mstr::Scope` is not copyable or movable. `scope.promote(v)` ends it early and returns `v` in the parent.
* `mstr::Box` is move-only. `reset()` leaves a valid empty box.
* A `std::string_view` of a *short* string points into the `mstr` variable itself, so do not keep it after
  that variable goes away.
* The type `mstr` in C++ is still a trivially-copyable 24-byte value. All the C functions and macros
  (`mstr_each`, `MSTR_PIPE`, ...) work from C++ too.
* Why `mstr::Scope` and not a namespace: a namespace cannot share its name with the type `mstr`, so `Scope` and
  `Box` are classes nested inside it.

## 17. Mistakes to avoid

| don't | do instead |
|---|---|
| keep a string from an inner scope after the scope ends | `mstr_scope_return` it, or `mstr_own` it |
| put a long-lived loop's work in one big scope | open a scope **per iteration** |
| pass an `mstr` to another thread | `mstr_share` it |
| hold on to `MSTR_CSTR(x)` pointers beyond the string's life | use them immediately, or copy |
| call `mstr_box_free` on two copies of one box | free exactly one copy |
| keep a value made *inside* a `MSTR_PIPE` | only the pipe's final result is kept |
| call `mstr_set_allocator` late | call it first thing in `main` |
| rely on `mstr_upper` for non-ASCII text | it changes ASCII letters only |
| read the bytes of a VIEW assuming a trailing NUL | use `MSTR_CSTR` |
| do work in the implicit root scope | open a scope (checked mode warns you) |

## 18. Cheat sheet

```c
// make                 S("lit")  mstr_from(c)  mstr_from_n(p,n)  mstr_fmt(f,...)  mstr_empty()
// ask                  mstr_len  mstr_is_empty  mstr_eq  mstr_cmp  mstr_find  mstr_rfind  mstr_contains
//                      mstr_starts_with  mstr_ends_with  mstr_at
// transform            mstr_slice(s,b,e)  mstr_trim/ltrim/rtrim  mstr_cat  mstr_replace(_all)
//                      mstr_upper/lower  mstr_repeat  mstr_join
// loop                 mstr_each(tok,s,delim) { }   mstr_lines(line,s) { }   mstr_split(s,delim,&n)
// scopes               mstr_scope { }   mstr_scope_t sc = mstr_scope_enter();  mstr_scope_leave(&sc);
//                      mstr_scope_return(&sc, v)   mstr_promote(&sc, v)
// C interop            MSTR_CSTR(s)   mstr_cstr_p(&s)
// ownership            mstr_own  mstr_unbox  mstr_box_free  mstr_box_auto
// threads              mstr_share  mstr_share_retain  mstr_share_release  mstr_share_get
// pipeline             MSTR_PIPE(x, step, step(args), ...)
// utf-8                mstr_u8_valid  mstr_u8_count  mstr_u8_slice  mstr_u8_each  mstr_u8_cp  mstr_u8_from_cp
// config               mstr_set_allocator  mstr_set_oom_handler  mstr_thread_cleanup
// debug                -DMSTR_CHECKED   MSTR_NO_ROOT_WARNING=1
```

For why it is built this way, every design decision and where it departs from the original brief, read
[DESIGN.md](DESIGN.md). For measured speed and allocation counts, read [BENCHMARKS.md](BENCHMARKS.md).
