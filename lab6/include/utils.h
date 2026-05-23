#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

size_t str_len(const char *s);
const char *str_chr(const char *s, char c);
int str_cmp(const char *a, const char *b);
int str_ncmp(const char *a, const char *b, size_t n);
void *mem_cpy(void *dst, const void *src, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int mem_cmp(const void *a, const void *b, int n);

int hex_to_int(const char *s, int n);
int align_up_int(int n, int byte);

#endif
