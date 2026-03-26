#ifndef CPIO_H
#define CPIO_H

#include <stddef.h>

struct cpio_newc_header {
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

void initrd_init(void *start, void *end);
void initrd_list(const void* rd);
void initrd_cat(const void* rd, const char* filename);

#endif
