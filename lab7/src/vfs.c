#include "vfs.h"
#include "mem_allocator.h"
#include "utils.h"

#define MAX_FS 16

struct mount* rootfs = NULL;

static struct filesystem* fs_table[MAX_FS];
static int fs_count = 0;

static struct filesystem* find_filesystem(const char* name) {
    for (int i = 0; i < fs_count; i++) {
        if (str_cmp(fs_table[i]->name, name) == 0)
            return fs_table[i];
    }
    return NULL;
}

int register_filesystem(struct filesystem* fs) {
    if (fs == NULL || fs->name == NULL || fs->setup_mount == NULL)
        return -1;

    if (find_filesystem(fs->name) != NULL)
        return -1;

    if (fs_count >= MAX_FS)
        return -1;

    fs_table[fs_count++] = fs;
    return 0;
}

int vfs_open(const char* pathname, int flags, struct file** target) {
    if (rootfs == NULL || rootfs->root == NULL || pathname == NULL || target == NULL)
        return -1;

    struct vnode* vnode = NULL;
    int ret = rootfs->root->v_ops->lookup(rootfs->root, &vnode, pathname);

    if (ret != 0) {
        if (flags & O_CREAT) {
            ret = rootfs->root->v_ops->create(rootfs->root, &vnode, pathname);
            if (ret != 0)
                return ret;
        } else {
            return ret;
        }
    }

    ret = vnode->f_ops->open(vnode, target);
    if (ret == 0 && *target != NULL)
        (*target)->flags = flags;
    return ret;
}

int vfs_close(struct file* file) {
    if (file == NULL || file->f_ops == NULL || file->f_ops->close == NULL)
        return -1;
    return file->f_ops->close(file);
}

int vfs_write(struct file* file, const void* buf, size_t len) {
    if (file == NULL || file->f_ops == NULL || file->f_ops->write == NULL)
        return -1;
    return file->f_ops->write(file, buf, len);
}

int vfs_read(struct file* file, void* buf, size_t len) {
    if (file == NULL || file->f_ops == NULL || file->f_ops->read == NULL)
        return -1;
    return file->f_ops->read(file, buf, len);
}

int vfs_mkdir(const char* pathname) {
    (void)pathname;
    return -1;
}

int vfs_mount(const char* target, const char* filesystem) {
    if (target == NULL || filesystem == NULL)
        return -1;

    if (!(target[0] == '/' && target[1] == '\0'))
        return -1;

    struct filesystem* fs = find_filesystem(filesystem);
    if (fs == NULL)
        return -1;

    struct mount* mount = allocate(sizeof(struct mount));
    if (mount == NULL)
        return -1;

    mount->root = NULL;
    mount->fs = fs;

    if (fs->setup_mount(fs, mount) != 0)
        return -1;

    rootfs = mount;
    return 0;
}

int vfs_lookup(const char* pathname, struct vnode** target) {
    if (rootfs == NULL || rootfs->root == NULL || pathname == NULL || target == NULL)
        return -1;

    if (pathname[0] == '/' && pathname[1] == '\0') {
        *target = rootfs->root;
        return 0;
    }

    if (pathname[0] == '/')
        pathname++;

    if (pathname[0] == '\0')
        return -1;

    for (const char* p = pathname; *p != '\0'; p++) {
        if (*p == '/')
            return -1;
    }

    return rootfs->root->v_ops->lookup(rootfs->root, target, pathname);
}
