/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
#include "fuzz_common.h"
/* Fused pipeline == nested calls, driven by the input.
 * input: [nsteps] then per step: [kind][a_len][b_len][i][j] followed by the pool text. */
static mstr apply(mstr v, int kind, mstr a, mstr b, size_t i, size_t j)
{
    switch (kind % 10) {
    case 0: return mstr_trim(v);
    case 1: return mstr_ltrim(v);
    case 2: return mstr_rtrim(v);
    case 3: return mstr_upper(v);
    case 4: return mstr_lower(v);
    case 5: return mstr_replace_all(v, a, b);
    case 6: return mstr_replace(v, a, b);
    case 7: return mstr_cat(v, a);
    case 8: return mstr_repeat(v, i % 3);
    default: return mstr_slice(v, i, j);
    }
}
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    size_t nsteps, k, pos = 1, pool_at;
    if (n < 2) return 0;
    nsteps = 1 + d[0] % 6;
    pool_at = 1 + nsteps * 5;
    if (n < pool_at) return 0;
    mstr_scope {
        const char *pool = (const char *)d + pool_at;
        size_t pn = n - pool_at;
        mstr in = mstr_from_n(pool, pn), nested = in, want[6], a[6], b[6];
        int kind[6];
        size_t si[6], sj[6];
        for (k = 0; k < nsteps; k++, pos += 5) {
            size_t al = pn ? d[pos + 1] % 4u : 0, bl = pn ? d[pos + 2] % 24u : 0;
            kind[k] = d[pos] % 10;
            a[k] = mstr_from_n(pool, al < pn ? al : pn);
            b[k] = mstr_from_n(pool + (pn > bl ? d[pos + 1] % (pn - bl + 1) : 0), bl < pn ? bl : pn);
            si[k] = d[pos + 3]; sj[k] = d[pos + 4];
            if (kind[k] == 9) { /* a valid range inside the current length */
                size_t len = mstr_len(nested);
                si[k] = len ? si[k] % (len + 1) : 0;
                sj[k] = si[k] + (len - si[k] ? sj[k] % (len - si[k] + 1) : 0);
            }
            /* bound growth: replace by up to 23 bytes could otherwise multiply 6 times (OOM in the harness) */
            if (kind[k] >= 5 && kind[k] <= 8 && mstr_len(nested) > 1500) kind[k] = 3;
            nested = apply(nested, kind[k], a[k], b[k], si[k], sj[k]);
            want[k] = nested;
        }
        { /* the pipe runs after everything the reference needed was allocated */
            mstr_pipe_t p = mstr_pipe_begin_(in);
            for (k = 0; k < nsteps; k++) {
                (void)mstr_pipe_hop_(&p, apply(p.v, kind[k], a[k], b[k], si[k], sj[k]));
                REQUIRE(mstr_eq(p.v, want[k]));
                check_repr(p.v);
            }
            REQUIRE(mstr_eq(mstr_pipe_end_(&p), nested));
        }
    }
    return 0;
}
