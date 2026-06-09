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
static int tmpfs_mkdir(struct vnode* dir_node, struct vnode** target,
                       const char* component_name);

static struct vnode_operations tmpfs_dir_vnode_ops = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir,
};

static struct vnode_operations tmpfs_file_vnode_ops = {
    .lookup = NULL,
    .create = NULL,
    .mkdir = NULL,
};

static struct file_operations tmpfs_file_ops = {
    .open = tmpfs_open,
    .close = tmpfs_close,
    .read = tmpfs_read,
    .write = tmpfs_write,
    .lseek64 = NULL,
};

static int tmpfs_name_valid(const char* name) {
    if (name == NULL || name[0] == '\0')
        return 0;

    return str_len(name) <= TMPFS_MAX_FILE_NAME;
}

static int tmpfs_init_node(struct tmpfs_node* node, fsnode_type type,
                           const char* name) {
    if (node == NULL || !tmpfs_name_valid(name))
        return -1;

    node->type = type;
    node->data = NULL;
    node->size = 0;
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++)
        node->entry[i] = NULL;

    mem_cpy(node->name, name, str_len(name) + 1);
    return 0;
}

static int tmpfs_create_node(struct vnode** target, fsnode_type type,
                             const char* name) {
    struct tmpfs_node* node = NULL;
    struct vnode* vnode = NULL;

    if (target == NULL)
        return -1;

    node = allocate(sizeof(struct tmpfs_node));
    if (node == NULL)
        return -1;
    if (tmpfs_init_node(node, type, name) != 0)
        return -1;

    vnode = allocate(sizeof(struct vnode));
    if (vnode == NULL)
        return -1;

    vnode->mount = NULL;
    vnode->owner = NULL;
    vnode->parent = vnode;
    vnode->v_ops = (type == TMPFS_DIR) ? &tmpfs_dir_vnode_ops : &tmpfs_file_vnode_ops;
    vnode->f_ops = &tmpfs_file_ops;
    vnode->internal = node;

    *target = vnode;
    return 0;
}

static int tmpfs_add_child(struct vnode* dir_node, struct vnode** target,
                           const char* component_name, fsnode_type type) {
    struct tmpfs_node* dir = NULL;
    struct vnode* existing = NULL;
    int slot = -1;
    int ret = 0;

    if (dir_node == NULL || target == NULL || !tmpfs_name_valid(component_name))
        return -1;

    dir = dir_node->internal;
    if (dir->type != TMPFS_DIR)
        return -1;

    if (tmpfs_lookup(dir_node, &existing, component_name) == 0)
        return -1;

    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (dir->entry[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    ret = tmpfs_create_node(target, type, component_name);
    if (ret != 0)
        return ret;

    (*target)->parent = dir_node;
    (*target)->owner = dir_node->owner;
    dir->entry[slot] = *target;
    return 0;
}

int tmpfs_setup_mount(struct filesystem* fs, struct mount* mount) {
    struct vnode* root_vnode = NULL;

    if (fs == NULL || mount == NULL)
        return -1;
    if (tmpfs_create_node(&root_vnode, TMPFS_DIR, "/") != 0)
        return -1;

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
    struct file* file = NULL;

    if (file_node == NULL || target == NULL)
        return -1;

    file = allocate(sizeof(struct file));
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
    struct tmpfs_node* node = NULL;
    size_t readable = 0;

    if (file == NULL || buf == NULL)
        return -1;

    node = file->vnode->internal;
    if (node->type != TMPFS_FILE)
        return -1;

    if (file->f_pos >= node->size)
        return 0;

    readable = node->size - file->f_pos;
    if (len > readable)
        len = readable;

    mem_cpy(buf, node->data + file->f_pos, len);
    file->f_pos += len;
    return (int)len;
}

static int tmpfs_write(struct file* file, const void* buf, size_t len) {
    struct tmpfs_node* node = NULL;
    size_t remain = 0;

    if (file == NULL || buf == NULL)
        return -1;

    node = file->vnode->internal;
    if (node->type != TMPFS_FILE)
        return -1;

    if (file->f_pos >= TMPFS_MAX_FILE_SIZE)
        return 0;

    remain = TMPFS_MAX_FILE_SIZE - file->f_pos;
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
    struct tmpfs_node* node = NULL;

    if (dir_node == NULL || target == NULL || component_name == NULL)
        return -1;

    node = dir_node->internal;
    if (node->type != TMPFS_DIR)
        return -1;

    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        struct vnode* child_vnode = node->entry[i];
        struct tmpfs_node* child = NULL;

        if (child_vnode == NULL)
            continue;

        child = child_vnode->internal;
        if (str_cmp(child->name, component_name) == 0) {
            *target = child_vnode;
            return 0;
        }
    }

    return -1;
}

static int tmpfs_create(struct vnode* dir_node, struct vnode** target,
                        const char* component_name) {
    return tmpfs_add_child(dir_node, target, component_name, TMPFS_FILE);
}

static int tmpfs_mkdir(struct vnode* dir_node, struct vnode** target,
                       const char* component_name) {
    return tmpfs_add_child(dir_node, target, component_name, TMPFS_DIR);
}
