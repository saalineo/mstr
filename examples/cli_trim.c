/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* cli_trim: read lines from stdin, print them trimmed, skip blank ones.
 *   printf '  a  \n\n  b c \n' | cli_trim
 * One scope per line, so memory never grows however long the input is. */
#include <stdio.h>
#include <string.h>
#include "mstr.h"

int main(void)
{
    static char buf[65536];
    size_t kept = 0;
    while (fgets(buf, sizeof buf, stdin)) {
        mstr_scope {
            mstr line = mstr_trim(mstr_from_n(buf, strlen(buf)));
            if (!mstr_is_empty(line)) {
                puts(MSTR_CSTR(line));
                kept++;
            }
        }
    }
    fprintf(stderr, "%zu non-blank lines\n", kept);
    return 0;
}
