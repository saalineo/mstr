# mstr: a string library with a lifetime model

Strings in C cost you `malloc`, length arithmetic, pointer juggling and a `free`
per string, and one slip is a crash or a leak. `mstr` takes the ownership out of
the string:

1. **A string is an immutable 24-byte value with no owner.** Copying it is as safe
   as copying an `int`. No `mstr` function ever frees anything.
2. **Lifetime lives in exactly one place: the scope.** Strings longer than 22
   bytes are bump-allocated in a thread-local arena; leaving a scope rewinds the
   arena and frees everything inside it in O(1).
3. **There is exactly one owning type, `mstr_box`,** for data that must outlive
   scopes, so ownership code is visible and greppable.

Result: no double free by construction, no leaks inside a scope, **use after a
scope ends aborts loudly in checked builds**, zero allocation for strings up to
22 bytes, and zero-copy slicing, trimming, and splitting.

C99, usable from C++17. 64-bit only. The library is one header (`include/mstr.h`)
plus one source file (`src/mstr.c`); `include/mstr.hpp` is an optional C++ wrapper.

## Contents

[Why it exists](#why-it-exists) · [Quick start](#quick-start) · [The same task, twice](#the-same-task-twice) ·
[How it works](#how-it-works) · [A short tour](#a-short-tour) · [Rules you must know](#rules-you-must-know) ·
[C++](#c) · [Build and test](#build-test-benchmark) · [Performance](#performance-honestly) · [Limits](#limits) · [License](#license)

**Documentation:** [**User guide**](docs/GUIDE.md) (start here to learn it) ·
[Design and rationale](docs/DESIGN.md) · [Benchmarks](docs/BENCHMARKS.md)

## Why it exists

Doing a simple thing with strings in C (trim a line, upper-case it, replace some characters) takes 20 to 30 lines
of `malloc`, length arithmetic, pointer juggling and `free`. One mistake is a crash, a leak or a security hole.
String libraries such as SDS, bstring and GLib's `GString` help, but they still ask you to free every string, and
they typically allocate even for short ones.

The deeper problem is that C has no destructors and no move semantics. A design that tries to track *who owns
a string* inside a value you can copy bit for bit cannot be made safe: the compiler will happily let you copy
the owner. mstr takes the other road and removes ownership from the string altogether.

## The idea in one minute

* **A string is a plain value.** It is 24 bytes, immutable, and copyable like an `int`. Every operation returns a
  new value; nothing is changed in place; no function ever frees a string.
* **Lifetime belongs to the scope.** Short strings (22 bytes or fewer) live inside the value and need no
  lifetime at all. Longer ones are bump-allocated in a thread-local arena, and leaving a `mstr_scope` rewinds the
  arena, freeing everything inside it in O(1).
* **Ownership beyond a scope is explicit and rare.** `mstr_box` is the only owning type, so every place that
  must manage memory is easy to find with `grep`.
* **Mistakes are loud.** Build with `-DMSTR_CHECKED` and using a string after its scope ended (or from another
  thread) aborts with the file and line where that scope began. Release builds pay nothing for this.
* **Each operation picks the cheapest valid result.** A slice of a long string is a zero-copy view; a slice or trim
  that comes out at 22 bytes or fewer is copied into a self-contained value, so a tiny result never keeps a huge
  buffer alive.

mstr also gives you a lazy, allocation-free splitting iterator, printf-style formatting, an optional UTF-8 layer,
atomically shared buffers for threads, pluggable allocators, a fused operation pipeline, and a C++17 wrapper.

**A good fit:** command-line tools, parsers, log and config processing, servers that handle a request in a
scope, anything with lots of short-lived string work where you want C's speed and simplicity without a `free`
per string.

**Not a fit:** programs that need in-place mutation of a growing buffer, very large per-string reference counting,
regex, locale-aware collation or Unicode normalization (all deliberate non-goals), 32-bit targets, or code that
must beat hand-tuned C on speed (see [Performance](#performance-honestly)).

## Quick start

1. Copy `include/mstr.h` and `src/mstr.c` into your project (or `add_subdirectory` this repository and link `mstr`).
2. Write this:

```c
#include <stdio.h>
#include "mstr.h"

int main(void)
{
    mstr_scope {
        mstr name = mstr_trim(mstr_from("  ada lovelace  "));
        puts(MSTR_CSTR(mstr_cat(S("Hello, "), mstr_upper(name))));
    }
    return 0;
}
```

3. Build and run: `cc -std=c99 -Iinclude hello.c src/mstr.c -o hello && ./hello` prints `Hello, ADA LOVELACE`.

Then read the **[user guide](docs/GUIDE.md)**: it walks through making and transforming strings, scopes, splitting,
C interop, boxes, threads, pipelines, UTF-8, checked mode and C++, with a runnable example in every section.

## The same task, twice

Normalise a name: trim, upper-case, replace spaces with `_`. Both programs are in
`examples/`, build with the commands below, and print the same thing for every
input (a test checks this).

**Plain C, careful version (31 lines):**

```c
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Normalise a name: trim, upper-case, spaces -> '_'. Caller frees the result. */
static char *normalise(const char *in)
{
    size_t start = 0, end = strlen(in), i, n;
    char *out;
    while (in[start] && isspace((unsigned char)in[start])) start++;
    while (end > start && isspace((unsigned char)in[end - 1])) end--;
    n = end - start;
    out = malloc(n + 1);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[start + i];
        out[i] = c == ' ' ? '_' : (char)toupper(c);
    }
    out[n] = '\0';
    return out;
}

int main(int argc, char **argv)
{
    char *id = normalise(argc > 1 ? argv[1] : "  John  Ronald Reuel  Tolkien ");
    if (!id) { fputs("out of memory\n", stderr); return 1; }
    puts(id);
    free(id);
    return 0;
}
```

**mstr (12 lines; the work is the three lines inside `mstr_scope`):**

```c
#include <stdio.h>
#include "mstr.h"

int main(int argc, char **argv)
{
    mstr_scope {
        mstr id = MSTR_PIPE(mstr_from(argc > 1 ? argv[1] : "  John  Ronald Reuel  Tolkien "),
                            mstr_trim, mstr_upper, mstr_replace_all(S(" "), S("_")));
        puts(MSTR_CSTR(id));
    }
    return 0;
}
```

```sh
cc -std=c99 -Iinclude examples/readme_mstr.c src/mstr.c -o readme_mstr && ./readme_mstr "  hello world "
```

`MSTR_PIPE` needs GNU statement expressions (gcc, clang, clang-cl). Elsewhere use the
equivalent explicit API (`mstr_pipe_begin_`, `mstr_pipe_hop_`, `mstr_pipe_end_`).

## How it works

### How values are stored

```
SSO    [ chars (<=22) + NUL ............ 23 bytes ][ tag ]    no allocation, no lifetime
LONG   [ ptr:8 ][ len:8 ][ depth:2 | serial:5 ][ tag ]
```

| state | what it is | lifetime |
|---|---|---|
| SSO | up to 22 bytes inline | none: it is just a value |
| STATIC | a string literal (`S("...")` longer than 22 bytes) | forever |
| REGION | bytes in the thread's arena, NUL-terminated | until its scope ends |
| VIEW | pointer + length into a REGION, STATIC or box buffer (zero copy) | same as what it points into |

**Law of Small Results:** any result of at most 22 bytes is an SSO value, even if it was
cut from a huge string. A VIEW never points at short text.

## A short tour

```c
mstr_scope {                                   // everything allocated inside dies at the closing brace
    mstr line = mstr_from("  key = value  ");
    mstr kv   = mstr_trim(line);                // short: new SSO value, no allocation
    size_t eq = mstr_find(kv, S("="));          // no allocation
    mstr key  = mstr_trim(mstr_slice(kv, 0, eq));
    mstr_each(tok, S("a,b,,c"), S(",")) { /* "a" "b" "" "c", zero allocation */ }
    mstr s = mstr_fmt("%s=%d", MSTR_CSTR(key), 42);
    puts(MSTR_CSTR(s));                         // C interop: NUL-terminated pointer
}

mstr make(mstr who) {                          // return a string out of a scope
    mstr_scope_t sc = mstr_scope_enter();
    mstr tmp = mstr_cat(S("Hello, "), who);
    return mstr_scope_return(&sc, mstr_upper(tmp));   // ends the scope, value moves to the parent
}

mstr_box keep = mstr_own(s);                    // malloc'd copy that outlives scopes
mstr v = mstr_unbox(&keep);                     // borrowed view
mstr_box_free(&keep);                           // idempotent; or declare `mstr_box_auto keep = ...`
```

Also: `mstr_eq/cmp/rfind/contains/starts_with/ends_with`, `mstr_ltrim/rtrim`, `mstr_replace(_all)`,
`mstr_lower`, `mstr_repeat`, `mstr_join`, `mstr_lines`, `mstr_split` (array in the current scope),
`mstr_share` (atomically refcounted buffer to hand to another thread), `mstr_set_allocator`.

## Rules you must know

* **In loops, open a scope per iteration.** Otherwise memory grows until the enclosing scope ends.
* A REGION/VIEW must not outlive its scope. Compile with `-DMSTR_CHECKED` and every use of
  a dead string aborts with `string from scope entered at foo.c:42 used after it ended`.
  Release builds ignore all of this at zero cost.
* Plain `mstr` values do not cross threads (checked mode catches it). Use `mstr_share`.
* `break`/`continue` inside `mstr_scope` bind to the macro's own loop. On strict C / MSVC (no
  `cleanup` attribute) an early `return` or `break` skips the rewind; the enclosing scope reclaims the
  memory later. Nothing is corrupted.
* Inside `MSTR_PIPE`/`mstr_pipe_*`, every step discards all arena data allocated since the pipe began
  except the live value. Do not keep other values created inside a pipe.
* A VIEW into a `mstr_box` is not tracked by scope serials: using it after the box is freed is
  undefined. Checked builds poison freed boxes and ASan finds the rest. See `docs/DESIGN.md`.
* The core is byte oriented. `mstr_u8_*` is an optional UTF-8 layer: counting, iteration and slicing by
  code point never split a sequence. Invalid input is **replaced** by U+FFFD (one per ill-formed
  subpart) when iterating and counting; slices keep the original bytes.

## C++

```cpp
#include <cstdio>
#include <string>
#include <string_view>
#include "mstr.hpp"

int main()
{
    mstr::Scope sc;                                  // RAII scope
    mstr s = mstr::from(std::string_view("hello, a string longer than 22 bytes"));
    std::string_view v = s;                          // implicit to string_view
    std::string str = static_cast<std::string>(s);   // explicit to std::string
    bool same = (s == std::string_view("hello"));    // operator==
    mstr::Box owned(s);                              // move-only owner
    std::printf("%c %zu %zu %d %zu\n", s[0], v.size(), str.size(), same, owned.view().size());
}
```

## Build, test, benchmark

```sh
cmake -S . -B build -DMSTR_SANITIZE=ON && cmake --build build && ctest --test-dir build   # ASan+UBSan
cmake -S . -B build -DMSTR_CHECKED=ON ...        # checked mode
cmake -S . -B build -DMSTR_TSAN=ON ...           # ThreadSanitizer
cmake -S . -B build -DMSTR_FUZZ=ON -DCMAKE_C_COMPILER=clang ...   # libFuzzer harnesses
cmake -S . -B build -DMSTR_BENCH=ON && cmake --build build --target bench && build/bench
```

## Performance, honestly

Measured on an i5-11400H, gcc 16 `-O2`, against vendored SDS and hand-written C
(full tables and method in `docs/BENCHMARKS.md`). All implementations are checked to produce the same bytes.

| trim + upper + replace on a 110-byte line | malloc calls/op | ns/op |
|---|---:|---:|
| hand-written C, fused loop | 1 | 23 |
| hand-written C, step by step | 3 | 54 |
| SDS (in place) | 1 | 208 |
| **mstr, nested calls** | **0** | 199 |
| **mstr, `MSTR_PIPE`** | **0** | 236 |

| split a 20-field CSV line, sum trimmed lengths | malloc calls/op | ns/op |
|---|---:|---:|
| hand-written C, pointer scan | 0 | 105 |
| hand-written C, `strdup` each field | 21 | 590 |
| **mstr, `mstr_each` (lazy)** | **0** | 757 |
| **mstr, `mstr_split` (array)** | **0** | 1328 |
| SDS `sdssplitlen` | 24 | 1316 |

**What this says:** mstr removes every `malloc`/`free` from these tasks (the arena is reclaimed by the scope),
and it is on par with SDS on throughput. It is **not faster than careful hand-written C**, which wins by 4 to 9x
on these micro-tasks; `mstr_each` is even slower than a version that calls `malloc` 21 times. The cost
is the by-value 24-byte representation and non-inlined library calls. You buy memory safety and
simplicity, not speed. `MSTR_PIPE` halves the bytes still held after a chain (112 vs 224 here) but
does **not** reduce the number of arena allocations, and costs about 19% more time.

## Limits

64-bit only (32-bit is rejected at compile time). Strings longer than the arena's address space are
not supported. `MSTR_PIPE` supports 10 steps. MSVC is covered by CI only through the strict-C scope variant,
without `MSTR_PIPE`. See `docs/DESIGN.md` for every decision and where this implementation departs from the original brief.

## License

mstr is released under the **MIT License**: see [`LICENSE`](LICENSE). Copyright (c) 2026 saalineo.
You may use, copy, modify and distribute it, including in closed-source products, as long as the copyright and
license notice is kept. Source files carry an `SPDX-License-Identifier: MIT` header.

The benchmark vendors [SDS](https://github.com/antirez/sds) (BSD-2-Clause) in `bench/third_party/sds/` under its own
license. It is used only for comparison and is not part of the library. See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
