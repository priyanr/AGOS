#include <fs/vfs.h>
#include <mm/heap.h>
#include <kernel/string.h>
#include <kernel/printk.h>

static vfs_node_t *fs_root = NULL;
static char vfs_cwd[256] = "/";

void vfs_init(void) {
    fs_root = NULL;
    strcpy(vfs_cwd, "/");
    printk("VFS: Virtual File System initialized.\n");
}

void vfs_set_root(vfs_node_t *root) {
    fs_root = root;
}

vfs_node_t *vfs_get_root(void) {
    return fs_root;
}

const char *vfs_getcwd(void) {
    return vfs_cwd;
}

int vfs_resolve_path(const char *in_path, char *out_path, size_t max_len) {
    if (!in_path || !out_path || max_len == 0) return -1;

    char temp[256];
    if (in_path[0] == '/') {
        strncpy(temp, in_path, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    } else {
        if (strcmp(vfs_cwd, "/") == 0) {
            snprintf(temp, sizeof(temp), "/%s", in_path);
        } else {
            snprintf(temp, sizeof(temp), "%s/%s", vfs_cwd, in_path);
        }
    }

    char *parts[32];
    int count = 0;
    char *p = temp;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *end = p;
        while (*end && *end != '/') end++;
        if (*end == '/') {
            *end = '\0';
            end++;
        }

        if (strcmp(p, ".") == 0) {
            // Current dir: ignore
        } else if (strcmp(p, "..") == 0) {
            if (count > 0) count--;
        } else {
            if (count < 32) {
                parts[count++] = p;
            }
        }
        p = end;
    }

    if (count == 0) {
        strncpy(out_path, "/", max_len);
    } else {
        out_path[0] = '\0';
        for (int i = 0; i < count; i++) {
            size_t clen = strlen(out_path);
            snprintf(out_path + clen, max_len - clen, "/%s", parts[i]);
        }
    }
    return 0;
}

int vfs_chdir(const char *path) {
    if (!path) return -1;
    char resolved[256];
    if (vfs_resolve_path(path, resolved, sizeof(resolved)) != 0) {
        return -1;
    }

    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) {
        return -1;
    }

    if (!(node->flags & FS_DIRECTORY)) {
        return -2;
    }

    strncpy(vfs_cwd, resolved, sizeof(vfs_cwd) - 1);
    vfs_cwd[sizeof(vfs_cwd) - 1] = '\0';
    return 0;
}

ssize_t vfs_read(vfs_node_t *node, uint64_t offset, size_t size, void *buffer) {
    if (!node || !node->read) return -1;
    return node->read(node, offset, size, buffer);
}

ssize_t vfs_write(vfs_node_t *node, uint64_t offset, size_t size, const void *buffer) {
    if (!node || !node->write) return -1;
    return node->write(node, offset, size, buffer);
}

void vfs_close(vfs_node_t *node) {
    if (node && node->close) {
        node->close(node);
    }
}

dirent_t *vfs_readdir(vfs_node_t *node, uint32_t index) {
    if (!node || !(node->flags & FS_DIRECTORY) || !node->readdir) return NULL;
    return node->readdir(node, index);
}

vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name) {
    if (!node || !(node->flags & FS_DIRECTORY) || !node->finddir) return NULL;
    return node->finddir(node, name);
}

vfs_node_t *vfs_open(const char *path, uint32_t flags) {
    (void)flags;
    if (!path || !fs_root) return NULL;

    char resolved[256];
    if (vfs_resolve_path(path, resolved, sizeof(resolved)) != 0) {
        return NULL;
    }

    if (strcmp(resolved, "/") == 0) {
        return fs_root;
    }

    // Skip leading slash
    const char *p = resolved;
    if (*p == '/') p++;

    char path_buf[256];
    strncpy(path_buf, p, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    vfs_node_t *current = fs_root;
    char *token = path_buf;
    char *next_token = NULL;

    while (token && *token) {
        // Find next '/'
        next_token = token;
        while (*next_token && *next_token != '/') {
            next_token++;
        }
        if (*next_token == '/') {
            *next_token = '\0';
            next_token++;
        } else {
            next_token = NULL;
        }

        if (strlen(token) > 0) {
            current = vfs_finddir(current, token);
            if (!current) {
                return NULL;
            }
        }

        token = next_token;
    }

    if (current && current->open) {
        current->open(current);
    }

    return current;
}
