# mstr design (living document, current through M1)

## Concept
Strings are immutable 24-byte values with no ownership. Lifetime lives in one
place: the scope (thread-local arena, rewound in O(1); arrives in M2). One
owning type, `mstr_box`, for data that outlives scopes (M4).

## Representation
```
SSO    [ chars (<=22) + NUL ............. 23 bytes ][ tag ]
LONG   [ ptr:8 ][ len:8 ][ depth:2 | serial:5 ][ tag ]
```
* `mstr` is `union { unsigned char b[24]; uint64_t }`: size 24, alignment 8.
  Alignment 8 is only to keep copies fast; **no field is ever read through a
  typed pointer**. ptr/len are read with `memcpy`, so there is no aliasing or
  misalignment UB. Tag, depth and serial use explicit byte indices
  (depth little-endian in bytes 16-17, serial little-endian in 18-22).
* Tag byte 23: bits 0-1 state (0 SSO, 1 REGION, 2 VIEW, 3 STATIC),
  bits 2-6 SSO length, bit 7 spare (always 0).
* An all-zero `mstr` is a valid empty SSO string.
* SSO values are fully zero-padded, so unused bytes are deterministic.
* 64-bit only. 32-bit targets are explicitly unsupported (`#error`): the
  LONG layout needs an 8-byte pointer and length. A 32-bit layout is possible
  but is outside the brief.

## Invariants (asserted in code / tests)
* I1 SSO length <= 22 and byte [len] == 0.
* I2 Law of Small Results: every result of length <= 22 is SSO. A VIEW never
  has length <= 22, so it never refers to short text or to an SSO buffer.
* I3 REGION/SSO/STATIC-from-`S()` are NUL-terminated; VIEW may not be.
* I4 Length-based everywhere; no `strlen` on mstr data; embedded NULs OK.
* I5 `depth`/`serial` are meaningful for REGION/VIEW only. Serial 0 means
  "immortal" (a VIEW into STATIC data). Real scopes use serial >= 1.

## Decisions where the brief was silent
* Ranges are half-open `[begin, end)`: `mstr_slice(s, begin, end)`.
* Out-of-range slice/at **always abort**, even in release (cheap compare,
  and the alternative is silent corruption). Checked mode adds file/line.
  The brief only requires checks in checked mode; this is the safer reading.
* Trim set is the C locale `isspace` set: ` \t\n\v\f\r`.
* `find` returns `MSTR_NPOS` when absent; empty needle matches at 0 (rfind:
  at len).
* `S()` pastes `"" lit`, so passing a non-literal is a compile error rather
  than a silent `sizeof(char*)` bug.
* M1 has no arena yet. `mstr_from_n` with n > 22 aborts with a clear message
  until M2 provides REGION allocation. Tests build REGION values over local
  buffers via `mstr_make_long_`.

## Disagreements / unsound-rule notes
* **R7 vs allocator hooks (section 5):** `mstr_set_allocator` is global mutable
  state, as is the "root scope warned once" flag. Both are tiny and
  init-once; I will make them atomic/idempotent in M4 and list them here.
* **Boxes vs checked mode (M4 preview):** `mstr_unbox` yields a VIEW into
  malloc'd memory. Scope serials cannot detect a freed box, so use-after-box-free
  is outside checked mode's guarantee unless boxes get their own generation
  tag. Will raise again in M4 with a concrete test.

## arena, scopes, builders
* **Arena:** thread-local chain of chunks (64 KB, doubling to a 1 MB cap; a
  request larger than the next size gets a dedicated chunk). All allocations
  are rounded to 8 bytes. A scope bookmark is (chunk, offset). Rewinding pops
  chunks above the bookmark into a free list (max 4 chunks of <= 1 MB; the
  rest are `free`d). The arena is freed at thread exit via a pthread key
  destructor on POSIX; elsewhere call `mstr_thread_cleanup()`.
* **Frames:** `frames[depth] = {serial, file, line}` is kept per thread now
  (checked mode in M3 validates against it). Root = depth 0, serial 1;
  serial 0 means immortal. Depth max 65535, serial max 2^40-1 (aborts).
* **Promote ends the scope.** `mstr_promote(&s, v)` rewinds to the child's
  bookmark and memmoves `v` down, so the scope is closed afterwards (leave
  becomes a no-op) and `mstr_scope_return` is the same call. A copy is needed
  only when `v` is REGION/VIEW with depth >= the child's depth; SSO, STATIC and
  data living in older scopes pass through unchanged. If the destination
  chunk has no room, a chunk is taken from the detached pool so the source
  bytes are never recycled before the copy. `memmove` covers the overlap case.
* **No-op results return the input:** `replace`/`replace_all` with no match or
  empty `from`, and `upper`/`lower` with nothing to change, return `s` itself
  with no allocation.
* **Iteration policy:** `"a,b,,c"` -> `a b "" c`; `""` -> one empty token; empty
  delimiter -> the whole string once. `mstr_lines` strips `\r` before `\n`; a
  trailing newline adds no empty line; `""` yields none. The iterator keeps its
  state in three local `mstr` slots (so the macro is a single `for`, and
  `break` works) and allocates nothing.
* **`MSTR_CSTR` in C++** is a temporary with `operator const char*`, valid to
  the end of the full expression (an inline function would return a pointer
  into its own parameter).
* **Strict scope variant** (`-DMSTR_STRICT_SCOPE`, default on non-GNU
  compilers): `break`/`return` skip the rewind; the enclosing scope reclaims
  later. Tested in CI as a separate binary.
* `upper`/`lower` are ASCII only (R5). `fmt` formats at most twice (once into a
  23-byte stack buffer, again only if the result is > 22 bytes).

## checked mode
* **Validity rule:** a REGION/VIEW is valid iff `serial == 0` (immortal) or
  `depth <= current_depth && frames[depth].serial == serial`. In checked mode
  `mstr_len` and `mstr_ptr` call `mstr_check_`, and every operation reads its
  inputs through those two, so nothing is missed. Release builds compile the
  call away.
* **Serials are globally unique (brief refinement).** A per-thread counter
  cannot detect cross-thread use: thread B's serial 5 at depth 2 can equal
  thread A's. Each thread therefore takes serials from 2^20-sized blocks handed
  out by one global atomic counter (`g_next_block`, relaxed fetch-add). This is
  a second, deliberate R7 exception (with the allocator hook), kept to a single
  word. Capacity: 2^20 blocks (threads or exhausted blocks) x 2^20 scopes.
* **Error text:** a per-thread ring of 256 recent `(serial, file, line)` entries
  recovers "scope entered at foo.c:42". If the serial is not in this thread's
  ring the message says "or on a different thread", because an old scope and a
  foreign thread cannot be told apart without more state.
* **Other checks:** `mstr_scope_leave` on a scope that is not live on this
  thread aborts (wrong thread or already unwound); `promote` always aborts for
  a dead scope or one with open inner scopes; slice/at bounds and length
  overflow abort in every build (see M1 decisions). The root scope warns once
  per thread when it allocates (silence with `MSTR_NO_ROOT_WARNING`).
* **Test helpers must use real scope tags:** values fabricated by hand (as
  `tests/test_m1.c` does) copy depth/serial from a live REGION.
* **Not yet covered:** box double free (needs `mstr_box`, M4) and the utf8 fuzz
  harness (needs the M5 UTF-8 layer).

## allocator hooks, boxes, shares
* **Allocator:** `mstr_allocator {alloc, realloc, free, ctx}`; every library
  allocation (arena chunks, frame table, boxes, shares) goes through it.
  `realloc` may be NULL (grow = alloc + copy + free). Signatures take `ctx`
  last. `mstr_set_allocator` / `mstr_set_oom_handler` abort unless called before
  the first library allocation (tracked by a relaxed atomic flag; configuration
  is single-threaded by contract). On NULL from the allocator the OOM handler
  runs; it must not return (if it does, we abort). Default: print and abort.
  A counting allocator test proves every allocation is freed (216/216).
* **Global state, final list (R7):** `g_alloc`, `g_oom` (write-once), `g_used`
  and `g_next_block` (atomics). Everything else is thread-local.
* **Box = the only owning type.** `{ptr, len, small_, kind}`. <= 22 bytes is
  stored inline (no allocation, R3). Heap buffers have a 16-byte header
  (magic, len) and a trailing NUL. `mstr_unbox` returns an SSO copy or a VIEW
  that carries the *immortal* tag (serial 0): it is not scope-tied.
* **Idempotent free vs "double free aborts" (brief conflict, resolved):**
  freeing the *same box object* twice is a no-op (free nulls it). Boxes are
  bit-copyable, so the real hazard is freeing a stale *copy*. Checked mode
  detects that by the header magic plus a per-thread quarantine of the last 64
  freed buffers. It is best effort: after 64 further frees on that thread the
  buffer is really freed and a stale copy is undefined behaviour. A zeroed box
  is a valid empty box. Using a freed box via `mstr_unbox` aborts in checked
  mode (kind 3), returns empty otherwise.
* **Use-after-box-free of a VIEW is NOT covered by scope serials (M1 concern,
  confirmed).** A VIEW into a box carries no scope; sliced VIEWs lose the base
  pointer, so a header lookup is impossible, and a live-box registry would need
  a global lock (violates R7). Mitigations: checked mode memsets freed box
  bytes to 0xDD and quarantines them, so a stale view reads poison instead of
  plausible data (`test_stale_view_reads_poison`); ASan reports the real
  use-after-free once the quarantine releases the buffer. This is a limit of the
  design, not a bug: lifetimes of box views are the owner's responsibility.
* **Share:** `{atomic refs, len}` header + bytes + NUL. `retain` is a relaxed
  add; `release` is `fetch_sub(acq_rel)` and the thread that sees 1 frees. Atomics
  use C11 `<stdatomic.h>` when `__STDC_VERSION__ >= 201112L`, else GCC/Clang
  `__atomic` builtins, else MSVC `_Interlocked*` (tested: builtins via C99
  builds, stdatomic via C11 builds). `mstr_share_get` returns an SSO copy or an
  immortal-tag VIEW, valid on any thread while a reference is held.
* **Test sensitivity (lesson):** a TSan stress test that merely passes proves
  little. Weakening `release` to relaxed was *not* detected by the first 8-thread
  test: TSan's 4-slot shadow history saturates, and a plain `free()` is not a
  reliable race witness. The final test installs a free hook that writes to
  the buffer and runs with 2 threads, where the weakened order is flagged 5/5
  while the correct code is never flagged. The 8-thread run is kept for
  refcount contention only.

## M5: UTF-8, MSTR_PIPE, C++ wrapper, benchmarks

### UTF-8 layer (`mstr_u8_*`)
* **Policy: replace, never reject.** Decoding follows the Unicode "maximal subpart"
  practice (Table 3-7): a lead byte plus however many valid continuation bytes follow
  it form one unit; ill-formed units (stray continuation, C0/C1/F5..FF, overlong,
  surrogate, > U+10FFFF, truncated) decode as U+FFFD and consume 1..3 bytes.
  `count`, `each` and `slice` use the same units so indexes always agree.
* `mstr_u8_slice` returns the ORIGINAL bytes of the selected units (never rewritten);
  `mstr_u8_each` yields U+FFFD (`EF BF BD`) in place of ill-formed units. A genuine
  U+FFFD is valid input. Slicing a valid string always yields a valid string.
* Verified against an **independent** validator (RFC 3629 as a pattern table, not
  sharing code with the decoder): exhaustively for all 1,114,112 code points, by
  60,000 random byte strings, and by `fuzz_utf8` (1.48M executions).
* `mstr_u8_each` yields each code point as a 1..4 byte SSO `mstr`; use `mstr_u8_cp`
  for its value. (A `uint32_t` loop variable would need a second declaration type in
  one `for` header, which C99 does not allow; the mstr-valued loop also keeps `break` working.)

### `MSTR_PIPE`: slide-down compaction, not scratch buffers (deviation from the brief)
* The brief asks for a two-buffer ping-pong so a 3-step chain does "at most ~1-2
  allocations". **This implementation does not reduce the allocation count.**
  Measured: 3 arena bumps for both the pipe and nested calls. It reduces *retained*
  memory (112 vs 224 bytes in the benchmark) and costs about 19% more time (extra move).
* Mechanism: `begin` records the arena top. After each step the result is `memmove`d down
  to that mark (overlap-safe), the arena is rewound, and the dead input plus every
  temporary disappear. Footprint = the live value plus the step's output, i.e. two buffers.
  Results that are SSO, STATIC, or live below the mark are left alone (no copy); a result
  that does not fit in the mark's chunk continues in a fresh chunk.
* **Why not real scratch buffers:** redirecting each step's output to two reusable
  buffers needs the allocator to know "this allocation is the step's result". But step
  arguments are evaluated inside the same call expression and may allocate too (e.g.
  `mstr_replace(mstr_cat(a, b), c)`), and C leaves their evaluation order unspecified; an
  argument temporary could land in, and then be clobbered by, the output buffer.
  Compaction is immune because everything is bump-allocated linearly and sorted out after
  the step. A stack of marks was also rejected: unspecified argument order lets a nested
  pipe's `begin` interleave with an outer one. The mark therefore lives in an explicit
  `mstr_pipe_t` object, and `MSTR_PIPE` creates one with a statement expression.
* **Hazard (documented in the header and README):** each hop discards ALL arena data
  allocated since `begin` except the live value. Keep nothing else created inside a pipe.
  The property test computes its reference values before `begin` for exactly this reason.
* **Step spelling:** steps are written without the value argument (`mstr_trim`,
  `mstr_replace(a, b)`), resolved by token pasting to adapter macros `MSTR_P_<name>`. This
  is what the preprocessor can do: the brief's `f3(args)` for an *arbitrary* `f3` needs the
  value inserted as first argument, which C cannot do without a per-name adapter. Library
  operations have adapters; user functions add one line (`#define MSTR_P_myfn(...) ...`).
  `mstr_slice` and `mstr_u8_slice` are macros (they capture `__FILE__`/`__LINE__`) and would
  be expanded early, so their steps are spelled `mstr_slice_range` / `mstr_u8_slice_range`.
* Needs GNU statement expressions; the explicit begin/hop/end API is portable.
  Equivalence with nested calls is a property test (30,000 random chains, 1-6 steps, checked
  per step) plus `fuzz_pipe` (4.76M executions).

### C++ wrapper
* **`mstr::Scope` / `mstr::Box` are nested classes of the union**, because a namespace
  cannot share a name with the type `mstr` at global scope. They are declared under
  `#ifdef __cplusplus` in `mstr.h` and defined in `mstr.hpp`. The union also gains member
  functions (`size`, `data`, `operator[]` bounds-checked, `==`, `!=`, implicit conversion to
  `std::string_view`, explicit to `std::string`, `from(string_view)`), and stays a trivially
  copyable, standard-layout 24-byte value (static-asserted in `tests/test_cpp.cpp`).
* `MSTR_CSTR(x)` in C++ is a temporary with `operator const char*`. Compound literals are
  not used anywhere C++ sees.

### Allocation-free claims, measured
* `mstr_each` allocated 0 bytes (asserted), but it is not faster than hand-written C; see
  `docs/BENCHMARKS.md`. The benchmark led to three fixes (single-byte `replace`, `memchr` for
  one-byte delimiters, size-bucketed SSO copy) and exposed unfair baselines in the harness itself.
