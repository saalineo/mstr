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
