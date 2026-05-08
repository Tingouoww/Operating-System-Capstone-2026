#include "string.h"

size_t str_len(const char *s)
{
    size_t len = 0;
    while (s[len] != '\0')
    {
        len++;
    }
    return len;
}

const char *str_chr(const char *s, char c)
{
    while (*s)
    {
        if (*s == c)
        {
            return s;
        }
        s++;
    }
    return NULL;
}

int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int str_ncmp(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0')
        {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }

    return 0;
}

void *mem_cpy(void *dst, const void *src, size_t n)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    size_t i;

    for (i = 0; i < n; i++)
    {
        d[i] = s[i];
    }

    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    size_t i;

    for (i = 0; i < n; i++)
    {
        d[i] = (unsigned char)value;
    }

    return dst;
}
