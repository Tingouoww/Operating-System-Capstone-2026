#include "vfs.h"
#include "mem_allocator.h"
#include "utils.h"

#define MAX_FS 16
#define VFS_PATH_MAX 256

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

int vfs_is_dir(struct vnode* vnode) {
    return vnode != NULL && vnode->v_ops != NULL && vnode->v_ops->lookup != NULL;
}

static struct vnode* follow_mounts(struct vnode* vnode) {
    while (vnode != NULL && vnode->mount != NULL)
        vnode = vnode->mount->root;
    return vnode;
}

static int extract_component(const char** cursor, char* component, size_t size) {
    const char* p = *cursor;
    size_t len = 0;

    while (*p == '/')
        p++;
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

static int is_mount_root(struct vnode* vnode) {
    return vnode != NULL && vnode->owner != NULL && vnode->owner->root == vnode;
}

static struct vnode* step_to_parent(struct vnode* current, struct vnode* root_dir) {
    if (current == NULL)
        return NULL;
    if (current == root_dir)
        return root_dir;

    if (is_mount_root(current) && current->owner->mount_point != NULL) {
        struct vnode* mount_point = current->owner->mount_point;

        if (mount_point == root_dir)
            return root_dir;
        if (mount_point->parent != NULL)
            return mount_point->parent;
        return mount_point;
    }

    if (current->parent != NULL)
        return current->parent;
    return current;
}

static int resolve_path(struct vnode* root_dir, struct vnode* cwd,
                        const char* pathname, int follow_final_mount,
                        struct vnode** target) {
    struct vnode* current = NULL;
    const char* cursor = NULL;
    char component[VFS_PATH_MAX];

    if (root_dir == NULL || cwd == NULL || pathname == NULL || target == NULL)
        return -1;

    current = (pathname[0] == '/') ? root_dir : cwd;
    cursor = pathname;

    while (1) {
        struct vnode* next = NULL;
        int ret = extract_component(&cursor, component, sizeof(component));

        if (ret < 0)
            return -1;
        if (ret > 0) {
            *target = follow_final_mount ? follow_mounts(current) : current;
            return 0;
        }

        if (str_cmp(component, ".") == 0)
            continue;
        if (str_cmp(component, "..") == 0) {
            current = step_to_parent(current, root_dir);
            continue;
        }

        if (!vfs_is_dir(current))
            return -1;

        ret = current->v_ops->lookup(current, &next, component);
        if (ret != 0)
            return ret;

        if (*cursor != '\0' || follow_final_mount)
            next = follow_mounts(next);

        current = next;
    }
}

static int resolve_parent_dir(struct vnode* root_dir, struct vnode* cwd,
                              const char* pathname, struct vnode** parent,
                              char* name, size_t name_size) {
    struct vnode* current = NULL;
    const char* cursor = NULL;
    char component[VFS_PATH_MAX];

    if (root_dir == NULL || cwd == NULL || pathname == NULL ||
        parent == NULL || name == NULL || name_size == 0)
        return -1;

    current = (pathname[0] == '/') ? root_dir : cwd;
    cursor = pathname;

    while (1) {
        int ret = extract_component(&cursor, component, sizeof(component));

        if (ret != 0)
            return -1;

        if (*cursor == '\0') {
            size_t len = str_len(component);

            if (str_cmp(component, ".") == 0 || str_cmp(component, "..") == 0)
                return -1;
            if (!vfs_is_dir(current) || len + 1 > name_size)
                return -1;

            mem_cpy(name, component, len + 1);
            *parent = current;
            return 0;
        }

        if (str_cmp(component, ".") == 0)
            continue;
        if (str_cmp(component, "..") == 0) {
            current = step_to_parent(current, root_dir);
            continue;
        }
        if (!vfs_is_dir(current))
            return -1;

        struct vnode* next = NULL;
        ret = current->v_ops->lookup(current, &next, component);
        if (ret != 0)
            return ret;

        current = follow_mounts(next);
    }
}

static int setup_new_mount(struct filesystem* fs, struct mount* mount,
                           struct vnode* mount_point) {
    if (fs == NULL || mount == NULL)
        return -1;
    if (fs->setup_mount(fs, mount) != 0)
        return -1;
    if (mount->root == NULL)
        return -1;

    mount->mount_point = mount_point;
    mount->root->owner = mount;
    if (mount_point == NULL)
        mount->root->parent = mount->root;
    else
        mount->root->parent = mount->root;
    return 0;
}

static struct vnode* get_boot_root(void) {
    if (rootfs == NULL)
        return NULL;
    return rootfs->root;
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

void vfs_file_retain(struct file* file) {
    if (file != NULL)
        file->ref_count++;
}

int vfs_file_release(struct file* file) {
    if (file == NULL)
        return -1;
    if (file->ref_count > 1) {
        file->ref_count--;
        return 0;
    }

    file->ref_count = 0;
    return vfs_close(file);
}

int vfs_open_from(struct vnode* root_dir, struct vnode* cwd,
                  const char* pathname, int flags, struct file** target) {
    struct vnode* vnode = NULL;
    int ret = 0;

    if (pathname == NULL || target == NULL)
        return -1;

    ret = resolve_path(root_dir, cwd, pathname, 1, &vnode);
    if (ret != 0) {
        if (!(flags & O_CREAT))
            return ret;

        struct vnode* parent = NULL;
        char name[VFS_PATH_MAX];

        ret = resolve_parent_dir(root_dir, cwd, pathname, &parent, name, sizeof(name));
        if (ret != 0)
            return ret;
        if (parent->v_ops == NULL || parent->v_ops->create == NULL)
            return -1;

        ret = parent->v_ops->create(parent, &vnode, name);
        if (ret != 0)
            return ret;
    }

    if (vnode == NULL || vnode->f_ops == NULL || vnode->f_ops->open == NULL)
        return -1;

    ret = vnode->f_ops->open(vnode, target);
    if (ret == 0 && *target != NULL) {
        (*target)->flags = flags;
        (*target)->ref_count = 1;
    }
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

int vfs_mkdir_from(struct vnode* root_dir, struct vnode* cwd,
                   const char* pathname) {
    struct vnode* parent = NULL;
    struct vnode* vnode = NULL;
    char name[VFS_PATH_MAX];
    int ret = 0;

    if (pathname == NULL)
        return -1;

    if (resolve_path(root_dir, cwd, pathname, 1, &vnode) == 0)
        return -1;

    ret = resolve_parent_dir(root_dir, cwd, pathname, &parent, name, sizeof(name));
    if (ret != 0)
        return ret;
    if (parent->v_ops == NULL || parent->v_ops->mkdir == NULL)
        return -1;

    return parent->v_ops->mkdir(parent, &vnode, name);
}

int vfs_mount_from(struct vnode* root_dir, struct vnode* cwd,
                   const char* target, const char* filesystem) {
    struct filesystem* fs = NULL;
    struct mount* mount = NULL;
    struct vnode* mount_point = NULL;
    int ret = 0;

    if (target == NULL || filesystem == NULL)
        return -1;

    fs = find_filesystem(filesystem);
    if (fs == NULL)
        return -1;

    mount = allocate(sizeof(struct mount));
    if (mount == NULL)
        return -1;

    mount->root = NULL;
    mount->mount_point = NULL;
    mount->fs = fs;

    if (target[0] == '/' && target[1] == '\0') {
        if (rootfs != NULL) {
            free(mount);
            return -1;
        }
        if (setup_new_mount(fs, mount, NULL) != 0) {
            free(mount);
            return -1;
        }
        rootfs = mount;
        return 0;
    }

    ret = resolve_path(root_dir, cwd, target, 0, &mount_point);
    if (ret != 0) {
        free(mount);
        return ret;
    }
    if (!vfs_is_dir(mount_point) || mount_point->mount != NULL) {
        free(mount);
        return -1;
    }

    if (setup_new_mount(fs, mount, mount_point) != 0) {
        free(mount);
        return -1;
    }

    mount_point->mount = mount;
    return 0;
}

int vfs_lookup_from(struct vnode* root_dir, struct vnode* cwd,
                    const char* pathname, struct vnode** target) {
    return resolve_path(root_dir, cwd, pathname, 1, target);
}

int vfs_open(const char* pathname, int flags, struct file** target) {
    struct vnode* boot_root = get_boot_root();

    if (boot_root == NULL)
        return -1;
    return vfs_open_from(boot_root, boot_root, pathname, flags, target);
}

int vfs_mkdir(const char* pathname) {
    struct vnode* boot_root = get_boot_root();

    if (boot_root == NULL)
        return -1;
    return vfs_mkdir_from(boot_root, boot_root, pathname);
}

int vfs_mount(const char* target, const char* filesystem) {
    struct vnode* boot_root = get_boot_root();

    if (rootfs == NULL && target != NULL && target[0] == '/' && target[1] == '\0')
        return vfs_mount_from(NULL, NULL, target, filesystem);
    if (boot_root == NULL)
        return -1;
    return vfs_mount_from(boot_root, boot_root, target, filesystem);
}

int vfs_lookup(const char* pathname, struct vnode** target) {
    struct vnode* boot_root = get_boot_root();

    if (boot_root == NULL)
        return -1;
    return vfs_lookup_from(boot_root, boot_root, pathname, target);
}
