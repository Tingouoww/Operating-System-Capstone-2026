#include "ramfs.h"
#include "cpio.h"
#include "mem_allocator.h"
#include "utils.h"

#define RAMFS_MAX_NAME 63
#define RAMFS_MAX_DIR_ENTRY 32

typedef enum { RAMFS_FILE, RAMFS_DIR } ramfs_node_type;

struct ramfs_node {
    ramfs_node_type type;
    char name[RAMFS_MAX_NAME + 1];
    struct vnode *entry[RAMFS_MAX_DIR_ENTRY];
    const char *data;
    size_t size;
};

static int ramfs_open(struct vnode *file_node, struct file **target);
static int ramfs_close(struct file *file);
static int ramfs_read(struct file *file, void *buf, size_t len);
static int ramfs_write(struct file *file, const void *buf, size_t len);
static int ramfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name);
static int ramfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name);
static int ramfs_mkdir(struct vnode *dir_node, struct vnode **target,
                       const char *component_name);

static struct vnode_operations ramfs_dir_vnode_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir = ramfs_mkdir,
};

static struct vnode_operations ramfs_file_vnode_ops = {
    .lookup = NULL,
    .create = NULL,
    .mkdir = NULL,
};

static struct file_operations ramfs_file_ops = {
    .open = ramfs_open,
    .close = ramfs_close,
    .read = ramfs_read,
    .write = ramfs_write,
    .lseek64 = NULL,
};

static int ramfs_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    return str_len(name) <= RAMFS_MAX_NAME;
}

static int ramfs_init_node(struct ramfs_node *node, ramfs_node_type type,
                           const char *name)
{
    if (node == NULL || !ramfs_name_valid(name))
        return -1;

    node->type = type;
    node->data = NULL;
    node->size = 0;
    for (int i = 0; i < RAMFS_MAX_DIR_ENTRY; i++)
        node->entry[i] = NULL;
    mem_cpy(node->name, name, str_len(name) + 1);
    return 0;
}

static int ramfs_create_vnode(struct vnode **target, ramfs_node_type type,
                              const char *name, struct mount *owner,
                              struct vnode *parent)
{
    struct ramfs_node *node = NULL;
    struct vnode *vnode = NULL;

    if (target == NULL)
        return -1;

    node = allocate(sizeof(struct ramfs_node));
    if (node == NULL)
        return -1;
    if (ramfs_init_node(node, type, name) != 0)
        return -1;

    vnode = allocate(sizeof(struct vnode));
    if (vnode == NULL)
        return -1;

    vnode->mount = NULL;
    vnode->owner = owner;
    vnode->parent = (parent != NULL) ? parent : vnode;
    vnode->v_ops = (type == RAMFS_DIR) ? &ramfs_dir_vnode_ops : &ramfs_file_vnode_ops;
    vnode->f_ops = &ramfs_file_ops;
    vnode->internal = node;
    *target = vnode;
    return 0;
}

static struct vnode *ramfs_find_child(struct vnode *dir_node, const char *name)
{
    struct ramfs_node *dir = NULL;

    if (dir_node == NULL || name == NULL)
        return NULL;

    dir = dir_node->internal;
    if (dir->type != RAMFS_DIR)
        return NULL;

    for (int i = 0; i < RAMFS_MAX_DIR_ENTRY; i++) {
        struct vnode *child = dir->entry[i];
        struct ramfs_node *child_node = NULL;

        if (child == NULL)
            continue;
        child_node = child->internal;
        if (str_cmp(child_node->name, name) == 0)
            return child;
    }
    return NULL;
}

static int ramfs_attach_child(struct vnode *dir_node, struct vnode *child)
{
    struct ramfs_node *dir = NULL;

    if (dir_node == NULL || child == NULL)
        return -1;

    dir = dir_node->internal;
    if (dir->type != RAMFS_DIR)
        return -1;

    for (int i = 0; i < RAMFS_MAX_DIR_ENTRY; i++) {
        if (dir->entry[i] == NULL) {
            dir->entry[i] = child;
            child->parent = dir_node;
            child->owner = dir_node->owner;
            return 0;
        }
    }
    return -1;
}

static int ramfs_ensure_dir(struct vnode *dir_node, const char *name,
                            struct mount *owner, struct vnode **target)
{
    struct vnode *existing = NULL;
    struct vnode *child = NULL;
    struct ramfs_node *child_node = NULL;

    if (dir_node == NULL || target == NULL || !ramfs_name_valid(name))
        return -1;

    existing = ramfs_find_child(dir_node, name);
    if (existing != NULL) {
        child_node = existing->internal;
        if (child_node->type != RAMFS_DIR)
            return -1;
        *target = existing;
        return 0;
    }

    if (ramfs_create_vnode(&child, RAMFS_DIR, name, owner, dir_node) != 0)
        return -1;
    if (ramfs_attach_child(dir_node, child) != 0)
        return -1;

    *target = child;
    return 0;
}

static int ramfs_add_file(struct vnode *dir_node, const char *name,
                          const char *data, size_t size, struct mount *owner)
{
    struct vnode *existing = NULL;
    struct vnode *child = NULL;
    struct ramfs_node *node = NULL;

    if (dir_node == NULL || !ramfs_name_valid(name))
        return -1;

    existing = ramfs_find_child(dir_node, name);
    if (existing != NULL) {
        node = existing->internal;
        if (node->type != RAMFS_FILE)
            return -1;
        node->data = data;
        node->size = size;
        return 0;
    }

    if (ramfs_create_vnode(&child, RAMFS_FILE, name, owner, dir_node) != 0)
        return -1;
    node = child->internal;
    node->data = data;
    node->size = size;

    return ramfs_attach_child(dir_node, child);
}

static int ramfs_next_component(const char **cursor, char *component, size_t size)
{
    const char *p = *cursor;
    size_t len = 0;

    while (*p == '/' || (*p == '.' && p[1] == '/'))
        p += (*p == '/') ? 1 : 2;

    if (*p == '\0') {
        *cursor = p;
        return 1;
    }

    while (*p != '\0' && *p != '/') {
        if (len + 1 >= size)
            return -1;
        component[len++] = *p++;
    }
    component[len] = '\0';

    while (*p == '/')
        p++;
    *cursor = p;
    return 0;
}

static int ramfs_mount_entry(struct mount *mount, const struct cpio_entry *entry)
{
    struct vnode *current = NULL;
    const char *cursor = NULL;
    char component[RAMFS_MAX_NAME + 1];

    if (mount == NULL || entry == NULL || entry->name == NULL)
        return -1;
    if (str_cmp(entry->name, ".") == 0 || entry->name[0] == '\0')
        return 0;

    current = mount->root;
    cursor = entry->name;
    while (1) {
        int ret = ramfs_next_component(&cursor, component, sizeof(component));

        if (ret < 0)
            return -1;
        if (ret > 0)
            return 0;

        if (*cursor == '\0') {
            if ((entry->mode & CPIO_MODE_TYPE_MASK) == CPIO_MODE_DIR)
                return ramfs_ensure_dir(current, component, mount, &current);
            if ((entry->mode & CPIO_MODE_TYPE_MASK) == CPIO_MODE_REG)
                return ramfs_add_file(current, component, entry->data,
                                      (size_t)entry->size, mount);
            return 0;
        }

        if (ramfs_ensure_dir(current, component, mount, &current) != 0)
            return -1;
    }
}

static int ramfs_build_tree_cb(const struct cpio_entry *entry, void *arg)
{
    return ramfs_mount_entry((struct mount *)arg, entry);
}

int ramfs_setup_mount(struct filesystem *fs, struct mount *mount)
{
    struct vnode *root_vnode = NULL;

    if (fs == NULL || mount == NULL)
        return -1;
    if (ramfs_create_vnode(&root_vnode, RAMFS_DIR, "/", mount, NULL) != 0)
        return -1;

    mount->root = root_vnode;
    mount->fs = fs;
    mount->mount_point = NULL;
    return cpio_iterate(ramfs_build_tree_cb, mount);
}

static struct filesystem ramfs_fs = {
    .name = "ramfs",
    .setup_mount = ramfs_setup_mount,
};

struct filesystem* ramfs_get_filesystem(void)
{
    return &ramfs_fs;
}

static int ramfs_open(struct vnode *file_node, struct file **target)
{
    struct file *file = NULL;

    if (file_node == NULL || target == NULL)
        return -1;

    file = allocate(sizeof(struct file));
    if (file == NULL)
        return -1;

    file->vnode = file_node;
    file->f_ops = file_node->f_ops;
    file->f_pos = 0;
    file->flags = 0;
    file->ref_count = 0;
    *target = file;
    return 0;
}

static int ramfs_close(struct file *file)
{
    if (file == NULL)
        return -1;

    free(file);
    return 0;
}

static int ramfs_read(struct file *file, void *buf, size_t len)
{
    struct ramfs_node *node = NULL;
    size_t readable = 0;

    if (file == NULL || buf == NULL)
        return -1;

    node = file->vnode->internal;
    if (node->type != RAMFS_FILE)
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

static int ramfs_write(struct file *file, const void *buf, size_t len)
{
    (void)file;
    (void)buf;
    (void)len;
    return -1;
}

static int ramfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
{
    struct vnode *child = NULL;

    if (dir_node == NULL || target == NULL || component_name == NULL)
        return -1;

    child = ramfs_find_child(dir_node, component_name);
    if (child == NULL)
        return -1;

    *target = child;
    return 0;
}

static int ramfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
{
    (void)dir_node;
    (void)target;
    (void)component_name;
    return -1;
}

static int ramfs_mkdir(struct vnode *dir_node, struct vnode **target,
                       const char *component_name)
{
    (void)dir_node;
    (void)target;
    (void)component_name;
    return -1;
}
