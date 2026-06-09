#include "vfs.h"
#include "mem_allocator.h"
#include "utils.h"
#include "tmpfs.h"

#define TMPFS_MAX_FILE_NAME 15
#define TMPFS_MAX_DIR_ENTRY 16
#define TMPFS_MAX_FILE_SIZE 4096

typedef enum { TMPFS_FILE, TMPFS_DIR } fsnode_type;

struct tmpfs_node {
    fsnode_type type;
    char name[TMPFS_MAX_FILE_NAME + 1];
    struct vnode* entry[TMPFS_MAX_DIR_ENTRY];
    char* data;
    size_t size;
};

static int tmpfs_open(struct vnode* file_node, struct file** target);
static int tmpfs_close(struct file* file);
static int tmpfs_read(struct file* file, void* buf, size_t len);
static int tmpfs_write(struct file* file, const void* buf, size_t len);
static int tmpfs_lookup(struct vnode* dir_node, struct vnode** target,
                        const char* component_name);
static int tmpfs_create(struct vnode* dir_node, struct vnode** target,
                        const char* component_name);

static struct vnode_operations tmpfs_vnode_ops = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .mkdir = NULL,
};

static struct file_operations tmpfs_file_ops = {
    .open = tmpfs_open,
    .close = tmpfs_close,
    .read = tmpfs_read,
    .write = tmpfs_write,
    .lseek64 = NULL,
};

int tmpfs_setup_mount(struct filesystem* fs, struct mount* mount) {
    if (fs == NULL || mount == NULL)
        return -1;

    struct tmpfs_node* root_node = allocate(sizeof(struct tmpfs_node));
    if (root_node == NULL)
        return -1;

    root_node->type = TMPFS_DIR;
    root_node->name[0] = '/';
    root_node->name[1] = '\0';
    root_node->data = NULL;
    root_node->size = 0;

    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        root_node->entry[i] = NULL;
    }

    struct vnode* root_vnode = allocate(sizeof(struct vnode));
    if (root_vnode == NULL)
        return -1;

    root_vnode->mount = NULL;
    root_vnode->v_ops = &tmpfs_vnode_ops;
    root_vnode->f_ops = &tmpfs_file_ops;
    root_vnode->internal = root_node;

    mount->root = root_vnode;
    mount->fs = fs;
    return 0;
}

static struct filesystem tmpfs_fs = {
    .name = "tmpfs",
    .setup_mount = tmpfs_setup_mount,
};

struct filesystem* tmpfs_get_filesystem(void) {
    return &tmpfs_fs;
}

static int tmpfs_open(struct vnode* file_node, struct file** target) {
    if (file_node == NULL || target == NULL)
        return -1;

    struct file* file = allocate(sizeof(struct file));
    if (file == NULL)
        return -1;

    file->vnode = file_node;
    file->f_ops = file_node->f_ops;
    file->f_pos = 0;
    file->flags = 0;
    *target = file;
    return 0;
}

static int tmpfs_close(struct file* file) {
    if (file == NULL)
        return -1;

    free(file);
    return 0;
}

static int tmpfs_read(struct file* file, void* buf, size_t len) {
    if (file == NULL || buf == NULL)
        return -1;

    struct tmpfs_node* node = file->vnode->internal;
    if (node->type != TMPFS_FILE)
        return -1;

    if (file->f_pos >= node->size)
        return 0;

    size_t readable = node->size - file->f_pos;
    if (len > readable)
        len = readable;

    mem_cpy(buf, node->data + file->f_pos, len);
    file->f_pos += len;
    return (int)len;
}

static int tmpfs_write(struct file* file, const void* buf, size_t len) {
    if (file == NULL || buf == NULL)
        return -1;

    struct tmpfs_node* node = file->vnode->internal;
    if (node->type != TMPFS_FILE)
        return -1;

    if (file->f_pos >= TMPFS_MAX_FILE_SIZE)
        return 0;

    size_t remain = TMPFS_MAX_FILE_SIZE - file->f_pos;
    if (len > remain)
        len = remain;

    if (node->data == NULL) {
        node->data = allocate(TMPFS_MAX_FILE_SIZE);
        if (node->data == NULL)
            return -1;
    }

    mem_cpy(node->data + file->f_pos, buf, len);
    file->f_pos += len;
    if (file->f_pos > node->size)
        node->size = file->f_pos;
    return (int)len;
}

static int tmpfs_lookup(struct vnode* dir_node, struct vnode** target,
                        const char* component_name) {
    if (dir_node == NULL || target == NULL || component_name == NULL)
        return -1;

    struct tmpfs_node* node = dir_node->internal;
    if (node->type != TMPFS_DIR)
        return -1;

    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (node->entry[i] == NULL)
            continue;

        struct tmpfs_node* child = node->entry[i]->internal;
        if (str_cmp(child->name, component_name) == 0) {
            *target = node->entry[i];
            return 0;
        }
    }

    return -1;
}

static int tmpfs_create(struct vnode* dir_node, struct vnode** target,
                        const char* component_name) {
    if (dir_node == NULL || target == NULL || component_name == NULL)
        return -1;

    struct tmpfs_node* dir = dir_node->internal;
    if (dir->type != TMPFS_DIR)
        return -1;

    struct vnode* existing = NULL;
    if (tmpfs_lookup(dir_node, &existing, component_name) == 0)
        return -1;

    int slot = -1;
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (dir->entry[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    struct tmpfs_node* new_node = allocate(sizeof(struct tmpfs_node));
    if (new_node == NULL)
        return -1;

    new_node->type = TMPFS_FILE;
    new_node->size = 0;
    new_node->data = allocate(TMPFS_MAX_FILE_SIZE);
    if (new_node->data == NULL)
        return -1;

    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        new_node->entry[i] = NULL;
    }

    int i = 0;
    while (component_name[i] != '\0' && i < TMPFS_MAX_FILE_NAME) {
        new_node->name[i] = component_name[i];
        i++;
    }
    new_node->name[i] = '\0';

    struct vnode* vnode = allocate(sizeof(struct vnode));
    if (vnode == NULL)
        return -1;

    vnode->mount = NULL;
    vnode->v_ops = &tmpfs_vnode_ops;
    vnode->f_ops = &tmpfs_file_ops;
    vnode->internal = new_node;

    dir->entry[slot] = vnode;
    *target = vnode;
    return 0;
}
