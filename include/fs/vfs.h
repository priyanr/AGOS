#ifndef FS_VFS_H
#define FS_VFS_H

#include <kernel/types.h>

#define FS_FILE        0x01
#define FS_DIRECTORY   0x02
#define FS_CHARDEVICE  0x03
#define FS_BLOCKDEVICE 0x04
#define FS_PIPE        0x05
#define FS_SYMLINK     0x06
#define FS_MOUNTPOINT  0x08

struct vfs_node;

typedef struct dirent {
    char name[128];
    uint32_t ino;
    uint32_t type;
} dirent_t;

typedef ssize_t (*read_type_t)(struct vfs_node *node, uint64_t offset, size_t size, void *buffer);
typedef ssize_t (*write_type_t)(struct vfs_node *node, uint64_t offset, size_t size, const void *buffer);
typedef void (*open_type_t)(struct vfs_node *node);
typedef void (*close_type_t)(struct vfs_node *node);
typedef struct dirent *(*readdir_type_t)(struct vfs_node *node, uint32_t index);
typedef struct vfs_node *(*finddir_type_t)(struct vfs_node *node, const char *name);

typedef struct vfs_node {
    char name[128];
    uint32_t flags;
    uint64_t size;
    uint64_t inode;
    read_type_t read;
    write_type_t write;
    open_type_t open;
    close_type_t close;
    readdir_type_t readdir;
    finddir_type_t finddir;
    struct vfs_node *ptr; // Alias or mountpoint target
} vfs_node_t;

void vfs_init(void);
vfs_node_t *vfs_open(const char *path, uint32_t flags);
ssize_t vfs_read(vfs_node_t *node, uint64_t offset, size_t size, void *buffer);
ssize_t vfs_write(vfs_node_t *node, uint64_t offset, size_t size, const void *buffer);
void vfs_close(vfs_node_t *node);
dirent_t *vfs_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name);
void vfs_set_root(vfs_node_t *root);
vfs_node_t *vfs_get_root(void);
int vfs_resolve_path(const char *in_path, char *out_path, size_t max_len);
int vfs_chdir(const char *path);
const char *vfs_getcwd(void);

#endif // FS_VFS_H
