#include "utils.h"

size_t str_len(const char *s)
{
    size_t len = 0;
    while (s[len] != '\0')
        len++;
    return len;
}

const char *str_chr(const char *s, char c)
{
    while (*s) {
        if (*s == c)
            return s;
        s++;
    }
    return NULL;
}

int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int str_ncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0')
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

void *mem_cpy(void *dst, const void *src, size_t n)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    return mem_cpy(dst, src, n);
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++)
        d[i] = (unsigned char)value;
    return dst;
}

int mem_cmp(const void *a, const void *b, int n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y) return *x - *y;
        x++; y++;
    }
    return 0;
}

int hex_to_int(const char *s, int n)
{
    int r = 0;
    while (n-- > 0) {
        r <<= 4;
        if (*s >= '0' && *s <= '9')      r += *s++ - '0';
        else if (*s >= 'A' && *s <= 'F') r += *s++ - 'A' + 10;
        else if (*s >= 'a' && *s <= 'f') r += *s++ - 'a' + 10;
        else s++;
    }
    return r;
}

int align_up_int(int n, int byte)
{
    return (n + byte - 1) & ~(byte - 1);
}
