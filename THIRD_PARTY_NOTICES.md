# Third-party notices

mstr itself is MIT licensed (see `LICENSE`). The only third-party code in this repository is used by the
benchmark and is **not** part of the library, and is not compiled into it:

## SDS (Simple Dynamic Strings)

* Location: `bench/third_party/sds/` (`sds.c`, `sds.h`, vendored unmodified)
* Upstream: https://github.com/antirez/sds, commit `5347739b1581fcba74fd5cab1fc21d2aef317d71`
* License: BSD-2-Clause. The full text, with its copyright notice, is in `bench/third_party/sds/LICENSE`
  and must stay with any redistribution of those files.
* Use: compared against mstr in `bench/bench.c`. Built only with `-DMSTR_BENCH=ON`.
