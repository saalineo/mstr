/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
/* csv_split: sum the third column of a CSV read from stdin, trimming each cell.
 *   printf 'a, b, 1.5\nc,d , 2\n' | csv_split
 * mstr_each splits lazily with zero allocation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mstr.h"

int main(void)
{
    static char buf[65536];
    double sum = 0;
    size_t rows = 0;
    while (fgets(buf, sizeof buf, stdin)) {
        mstr_scope {
            size_t col = 0;
            mstr_each(cell, mstr_trim(mstr_from_n(buf, strlen(buf))), S(",")) {
                if (col++ == 2) sum += strtod(MSTR_CSTR(mstr_trim(cell)), NULL);
            }
            rows += col > 0;
        }
    }
    printf("rows=%zu sum=%.2f\n", rows, sum);
    return 0;
}
