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
