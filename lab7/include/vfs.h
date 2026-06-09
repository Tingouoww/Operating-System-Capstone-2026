#ifndef VFS_H
#define VFS_H
#include <stddef.h>

#define O_CREAT 00000100

struct vnode {
  struct mount* mount;
  struct mount* owner;
  struct vnode* parent;
  struct vnode_operations* v_ops;
  struct file_operations* f_ops;
  void* internal;
};

// 已經 open 的檔案句柄
struct file {
  struct vnode* vnode;
  size_t f_pos;  // 此檔案句柄的讀寫位置 offset
  struct file_operations* f_ops; // 透過這張表呼叫對應檔案系統的實作
  int flags; // open 時帶進來的旗標
  int ref_count;
};

/* 表示已掛載的 file system */
struct mount {
  struct vnode* root;
  struct vnode* mount_point;
  struct filesystem* fs;
};

struct filesystem {
  const char* name;
  int (*setup_mount)(struct filesystem* fs, struct mount* mount);
};

struct file_operations {
  int (*open)(struct vnode* file_node, struct file** target);
  int (*close)(struct file* file);
  int (*read)(struct file* file, void* buf, size_t len);
  int (*write)(struct file* file, const void* buf, size_t len);
  long (*lseek64)(struct file* file, long offset, int whence);
};

struct vnode_operations {
  int (*lookup)(struct vnode* dir_node, struct vnode** target,
                const char* component_name);
  int (*create)(struct vnode* dir_node, struct vnode** target,
                const char* component_name);
  int (*mkdir)(struct vnode* dir_node, struct vnode** target,
               const char* component_name);
};

extern struct mount* rootfs;

int register_filesystem(struct filesystem* fs);
int vfs_open  (const char* pathname, int flags, struct file** target);
int vfs_close (struct file* file);
int vfs_write (struct file* file, const void* buf, size_t len);
int vfs_read  (struct file* file, void* buf, size_t len);
int vfs_mkdir (const char* pathname);
int vfs_mount (const char* target, const char* filesystem);
int vfs_lookup(const char* pathname, struct vnode** target);
int vfs_open_from(struct vnode* root_dir, struct vnode* cwd,
                  const char* pathname, int flags, struct file** target);
int vfs_mkdir_from(struct vnode* root_dir, struct vnode* cwd,
                   const char* pathname);
int vfs_mount_from(struct vnode* root_dir, struct vnode* cwd,
                   const char* target, const char* filesystem);
int vfs_lookup_from(struct vnode* root_dir, struct vnode* cwd,
                    const char* pathname, struct vnode** target);
int vfs_is_dir(struct vnode* vnode);
void vfs_file_retain(struct file* file);
int vfs_file_release(struct file* file);

#endif
