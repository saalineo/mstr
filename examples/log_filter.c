/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* log_filter LEVEL: print "[time] message" for log lines of the given level.
 *   printf '2024-05-01 12:00:01 ERROR db: timeout\n2024-05-01 12:00:02 INFO ok\n' | log_filter error
 * Line format: DATE TIME LEVEL message...   Level matching ignores case. */
#include <stdio.h>
#include <string.h>
#include "mstr.h"

int main(int argc, char **argv)
{
    static char buf[65536];
    if (argc < 2) { fputs("usage: log_filter LEVEL\n", stderr); return 2; }
    while (fgets(buf, sizeof buf, stdin)) {
        mstr_scope {
            mstr want = mstr_upper(mstr_from(argv[1]));
            size_t n;
            mstr *f = mstr_split(mstr_trim(mstr_from_n(buf, strlen(buf))), S(" "), &n);
            if (n >= 4 && mstr_eq(f[2], want)) {
                size_t head = mstr_len(f[0]) + 1 + mstr_len(f[1]) + 1 + mstr_len(f[2]) + 1;
                mstr line = mstr_trim(mstr_from_n(buf, strlen(buf)));
                mstr msg = mstr_slice(line, head, mstr_len(line));
                puts(MSTR_CSTR(mstr_fmt("[%s] %s", MSTR_CSTR(f[1]), MSTR_CSTR(msg))));
            }
        }
    }
    return 0;
}
