/* SPDX-License-Identifier: EUPL-1.2
 * The few C library functions the firmware needs (it links without a libc).
 * Built with -fno-tree-loop-distribute-patterns so GCC does not turn these loops into calls to themselves.
 */
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *o = d;
    const unsigned char *i = s;
    while (n--)
        *o++ = *i++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *o = d;
    const unsigned char *i = s;
    if (o < i)
        while (n--)
            *o++ = *i++;
    else
        while (n--)
            o[n] = i[n];
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *o = d;
    while (n--)
        *o++ = (unsigned char)c;
    return d;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}
