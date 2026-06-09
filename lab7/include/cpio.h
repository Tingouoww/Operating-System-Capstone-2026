#ifndef CPIO_H
#define CPIO_H

#include <stddef.h>

#define CPIO_MODE_TYPE_MASK 0170000
#define CPIO_MODE_REG       0100000
#define CPIO_MODE_DIR       0040000

struct cpio_newc_header
{
    // 8-byte hexadecimal fields
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
};

struct cpio_entry {
    const char *name;
    const void *data;
    unsigned long size;
    unsigned int mode;
};

typedef int (*cpio_iter_fn)(const struct cpio_entry *entry, void *arg);

void initrd_init(void *start, void *end);
void initrd_list(const void *rd);
void initrd_cat(const void *rd, const char *filename);
unsigned long cpio_find_exec(const char *filename);
unsigned long cpio_find_exec_size(const char *filename);
int cpio_iterate(cpio_iter_fn fn, void *arg);

#endif
