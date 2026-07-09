#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/path.h"
#include "fs/fifo.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "util/refcount.h"
#include "util/timer.h"
#include "debug.h"

// ========================
// ======== INODES ========
// ========================

struct tmp_inode {
    struct refcount refcount;
    lock_t lock;

    struct statbuf stat;
    union {
        void *file_data;
        //char *symlink_data;
        struct fifo_file *fifo; // S_IFIFO: the shared named-pipe buffer
    };
    // S_IFREG only: once the file has been mmapped, its contents live in this
    // unlinked host temp file instead of the malloc'd file_data buffer, so
    // guest mappings can be host mmaps of it (see tmpfs_inode_host_backing).
    // -1 while still malloc-backed.
    int host_fd;
};

static bool tmpfs_is_cgroup2_mount(struct mount *mount) {
    return strcmp(mount->fs->name, "cgroup2") == 0;
}

static struct tmp_inode *tmp_inode_new(mode_t_ mode) {
    struct tmp_inode *node = malloc(sizeof(struct tmp_inode));
    if (node == NULL)
        return NULL;
    refcount_init(node);
    lock_init(&node->lock, "tmp_inode_new\0");

    node->stat = (struct statbuf) {};
    static _Atomic ino_t next_inode = 1;
    node->stat.inode = next_inode++;

    node->stat.mode = mode;
    node->stat.uid = current->euid;
    node->stat.gid = current->egid;
    node->file_data = NULL; // also clears the ->fifo union slot for S_IFIFO
    node->host_fd = -1;
    if (S_ISREG(mode)) {
        node->file_data = malloc(0);
        if (node->file_data == NULL) {
            free(node);
            return NULL;
        }
    }
    return node;
}

DEFINE_REFCOUNT_STATIC(tmp_inode)

static void tmp_inode_cleanup(struct tmp_inode *inode) {
    // Regular files keep their contents in file_data; symlinks keep their
    // target string there too (same union slot).
    if (S_ISREG(inode->stat.mode) || S_ISLNK(inode->stat.mode)) {
        free(inode->file_data);
        if (inode->host_fd >= 0)
            close(inode->host_fd);
    } else if (S_ISFIFO(inode->stat.mode)) {
        fifo_file_free(inode->fifo);
    }
    free(inode);
}

static void tmpfs_update_ctime(struct tmp_inode *inode) {
    struct timespec now = timespec_now(CLOCK_REALTIME);
    inode->stat.ctime = now.tv_sec;
    inode->stat.ctime_nsec = now.tv_nsec;
}

static void tmpfs_update_mtime_and_ctime(struct tmp_inode *inode) {
    struct timespec now = timespec_now(CLOCK_REALTIME);
    inode->stat.mtime = now.tv_sec;
    inode->stat.mtime_nsec = now.tv_nsec;
    inode->stat.ctime = now.tv_sec;
    inode->stat.ctime_nsec = now.tv_nsec;
}

// ===================================
// ======== DIRECTORY ENTRIES ========
// ===================================

struct tmp_dirent {
    char name[MAX_NAME + 1];
    struct tmp_inode *inode;
    unsigned long index;

    struct tmp_dirent *parent;
    struct list children;
    unsigned long next_index;

    struct refcount refcount;
    lock_t lock;
    struct list dir;
};

DEFINE_REFCOUNT_STATIC(tmp_dirent)

static void tmp_dirent_cleanup(struct tmp_dirent *dirent) {
    if (dirent->parent != NULL)
        tmp_dirent_release(dirent->parent);
    tmp_inode_release(dirent->inode);
    free(dirent);
}

static void tmp_dirent_init(struct tmp_dirent *dirent) {
    refcount_init(dirent);
    list_init(&dirent->children);
    dirent->next_index = 0;
    lock_init(&dirent->lock, "tmp_dirent_init\0");
}

// Frees the child inode on failure, so you don't need to! But be careful you don't free it yourself.
// In other words: Takes ownership of `child`
static int tmpfs_dir_link(struct tmp_dirent *dir, const char *name, struct tmp_inode *child, struct tmp_dirent **dirent_out) {
    if (!S_ISDIR(dir->inode->stat.mode)) {
        tmp_inode_release(child);
        return _ENOTDIR;
    }
    struct tmp_dirent *new_dirent = malloc(sizeof(struct tmp_dirent));
    if (new_dirent == NULL) {
        tmp_inode_release(child);
        return _ENOMEM;
    }

    tmp_dirent_init(new_dirent);
    strncpy(new_dirent->name, name, sizeof(new_dirent->name) - 1);
    new_dirent->name[sizeof(new_dirent->name) - 1] = '\0';
    new_dirent->inode = tmp_inode_retain(child);
    new_dirent->index = dir->next_index++;
    new_dirent->parent = tmp_dirent_retain(dir);
    list_add_tail(&dir->children, &new_dirent->dir);

    if (dirent_out)
        *dirent_out = tmp_dirent_retain(new_dirent);
    return 0;
}

static void tmpfs_fd_seekdir(struct fd *fd, struct tmp_dirent *dirent) {
    if (dirent != NULL)
        tmp_dirent_retain(dirent);
    if (fd->tmpfs.dir_pos != NULL)
        tmp_dirent_release(fd->tmpfs.dir_pos);
    fd->tmpfs.dir_pos = dirent;
}

static struct tmp_dirent *tmpfs_dir_lookup(struct tmp_dirent *dir, const char *name) {
    if (!S_ISDIR(dir->inode->stat.mode))
        return ERR_PTR(_ENOTDIR);
    struct tmp_dirent *dirent = NULL;
    struct tmp_dirent *d;
    list_for_each_entry(&dir->children, d, dir) {
        if (d->inode == NULL)
            continue;
        if (strcmp(d->name, name) == 0) {
            dirent = d;
            break;
        }
    }
    if (dirent == NULL)
        return ERR_PTR(_ENOENT);
    return tmp_dirent_retain(dirent);
}

// TODO: should this function even exist? can't tmpfs_dir_link check for existence?
static int tmpfs_dir_lookup_existence(struct tmp_dirent *dir, const char *name) {
    struct tmp_dirent *dirent = tmpfs_dir_lookup(dir, name);
    if (dirent == ERR_PTR(_ENOENT))
        return 0;
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    tmp_dirent_release(dirent);
    return _EEXIST;
}

static int tmpfs_init_regular_file(struct tmp_inode *inode, const char *contents) {
    assert(S_ISREG(inode->stat.mode));
    if (contents == NULL || contents[0] == '\0')
        return 0;

    size_t len = strlen(contents);
    void *new_data = realloc(inode->file_data, len);
    if (new_data == NULL)
        return _ENOMEM;
    inode->file_data = new_data;
    memcpy(inode->file_data, contents, len);
    inode->stat.size = len;
    tmpfs_update_mtime_and_ctime(inode);
    return 0;
}

static int tmpfs_add_file(struct tmp_dirent *dir, const char *name, mode_t_ mode, const char *contents) {
    int err = tmpfs_dir_lookup_existence(dir, name);
    if (err == _EEXIST)
        return 0;
    if (err < 0)
        return err;

    struct tmp_inode *inode = tmp_inode_new(S_IFREG | mode);
    if (inode == NULL)
        return _ENOMEM;

    err = tmpfs_init_regular_file(inode, contents);
    if (err == 0)
        err = tmpfs_dir_link(dir, name, inode, NULL);
    tmp_inode_release(inode);
    return err;
}

static int tmpfs_populate_cgroup2_dir(struct tmp_dirent *dir) {
    int err = tmpfs_add_file(dir, "cgroup.procs", 0644, "");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.threads", 0644, "");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.controllers", 0444, "cpu io memory pids\n");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.subtree_control", 0644, "");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.events", 0444, "populated 1\nfrozen 0\n");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.stat", 0444, "nr_descendants 0\nnr_dying_descendants 0\n");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.type", 0444, "domain\n");
    if (err < 0)
        return err;
    return 0;
}

static struct tmp_dirent *__tmpfs_lookup(struct mount *mount, const char *path, bool parent, const char **filename_out) {
    struct tmp_dirent *root = mount->data;
    struct tmp_dirent *dirent = tmp_dirent_retain(root); // strong reference

    char component[MAX_NAME + 1] = {};
    int err = 0;
    while (path_next_component(&path, component, &err)) {
        if (parent && *path == '\0')
            break;

        lock(&dirent->lock, 0);
        struct tmp_dirent *child = tmpfs_dir_lookup(dirent, component);
        unlock(&dirent->lock);

        tmp_dirent_release(dirent);
        if (IS_ERR(child))
            return child;
        dirent = child;
    }

    if (parent && filename_out)
        *filename_out = path - strlen(component);

    if (err < 0)
        return ERR_PTR(err);
    return dirent;
}
static struct tmp_dirent *tmpfs_lookup(struct mount *mount, const char *path) {
    return __tmpfs_lookup(mount, path, false, NULL);
}
static struct tmp_dirent *tmpfs_lookup_parent(struct mount *mount, const char *path, const char **filename_out) {
    if (strcmp(path, "/") == 0)
        return NULL;
    return __tmpfs_lookup(mount, path, true, filename_out);
}

// Lazily switch a regular file's backing from the malloc'd file_data buffer to
// an unlinked host temp file. The malloc buffer moves on every realloc
// (tmpfs_file_resize / tmpfs_write), which would leave a guest mapping pointing
// at freed memory, so it can never be mmapped directly; a host file makes every
// guest mapping a host mmap of the same file, and the host kernel provides
// MAP_SHARED write-back and mmap<->read/write coherence exactly as it does for
// realfs. Only mmapped files pay the host-fd cost. Call with inode->lock held.
// Returns the host fd, or a negative errno.
static int tmpfs_inode_host_backing(struct tmp_inode *inode) {
    assert(S_ISREG(inode->stat.mode));
    if (inode->host_fd >= 0)
        return inode->host_fd;
    // TMPDIR is always set on iOS (the app sandbox tmp dir); fall back to /tmp
    // for the command-line build.
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0')
        tmpdir = "/tmp";
    char path[MAX_PATH];
    if (snprintf(path, sizeof(path), "%s/ish-tmpfs.XXXXXX", tmpdir) >= (int) sizeof(path))
        return _ENAMETOOLONG;
    int host_fd = mkstemp(path);
    if (host_fd < 0)
        return errno_map();
    unlink(path); // the inode owns the fd; the name never needs to exist again
    fcntl(host_fd, F_SETFD, FD_CLOEXEC);
    size_t size = inode->stat.size;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pwrite(host_fd, (char *) inode->file_data + done, size - done, done);
        if (n < 0) {
            int err = errno_map();
            close(host_fd);
            return err;
        }
        done += n;
    }
    free(inode->file_data);
    inode->file_data = NULL;
    inode->host_fd = host_fd;
    return host_fd;
}

static int tmpfs_file_resize(struct tmp_inode *file, size_t size) {
    assert(S_ISREG(file->stat.mode));
    if (file->host_fd >= 0) {
        // Host-file-backed (has been mmapped): ftruncate keeps live guest
        // mappings coherent, and the host zero-fills growth.
        if (ftruncate(file->host_fd, size) < 0)
            return errno_map();
        file->stat.size = size;
        tmpfs_update_mtime_and_ctime(file);
        return 0;
    }
    size_t old_size = file->stat.size;
    void *new_data = realloc(file->file_data, size);
    // realloc(ptr, 0) may legitimately return NULL (it frees ptr); only treat
    // NULL as an error for a non-zero request.
    if (new_data == NULL && size != 0)
        return _ENOMEM;
    file->file_data = new_data;
    file->stat.size = size;
    // Only zero newly-grown bytes. When shrinking (e.g. ftruncate to a smaller
    // size, or to 0) size < old_size, and size - old_size is an unsigned
    // underflow -> a huge memset that writes far past the buffer -> SIGSEGV.
    if (size > old_size)
        memset((char *) file->file_data + old_size, 0, size - old_size);
    tmpfs_update_mtime_and_ctime(file);
    return 0;
}

static int tmpfs_dir_unlink(struct tmp_dirent *parent, const char *name, bool remove_dir) {
    struct tmp_dirent *dirent = tmpfs_dir_lookup(parent, name);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);

    int err = 0;
    lock(&dirent->lock, 0);
    if (S_ISDIR(dirent->inode->stat.mode)) {
        if (!remove_dir)
            err = _EISDIR;
        else if (!list_empty(&dirent->children))
            err = _ENOTEMPTY;
    } else if (remove_dir) {
        err = _ENOTDIR;
    }
    unlock(&dirent->lock);
    if (err < 0) {
        tmp_dirent_release(dirent);
        return err;
    }

    list_remove(&dirent->dir);
    tmpfs_update_mtime_and_ctime(parent->inode);
    tmp_dirent_release(dirent); // drop tree reference
    tmp_dirent_release(dirent); // drop lookup reference
    return 0;
}

// ========================
// ======== FS OPS ========
// ========================

extern const struct fd_ops tmpfs_fdops;

static int tmpfs_mount(struct mount *mount) {
    struct tmp_inode *root_inode = tmp_inode_new(S_IFDIR | 0777);
    if (root_inode == NULL)
        return _ENOMEM;

    struct tmp_dirent *root = malloc(sizeof(struct tmp_dirent));
    if (root == NULL) {
        free(root_inode);
        return _ENOMEM;
    }

    tmp_dirent_init(root);
    memcpy(root->name, "", 1);
    root->inode = root_inode;
    root->parent = NULL;

    mount->data = root;
    if (tmpfs_is_cgroup2_mount(mount)) {
        lock(&root->lock, 0);
        int err = tmpfs_populate_cgroup2_dir(root);
        unlock(&root->lock);
        if (err < 0)
            return err;
    }
    return 0;
}


// Post-order teardown of the whole directory tree. Each dirent carries one
// "tree reference" (the refcount it was created with in tmpfs_mount /
// tmpfs_dir_link); dropping it runs tmp_dirent_cleanup, which in turn releases
// the dirent's parent and inode references, so the refcounts unwind cleanly up
// to the root. mount_remove() already rejected the umount with EBUSY if any fd
// still referenced this mount, so there are no live fd references to race with.
static void tmpfs_free_tree(struct tmp_dirent *dirent) {
    while (!list_empty(&dirent->children)) {
        struct tmp_dirent *child = list_first_entry(&dirent->children, struct tmp_dirent, dir);
        list_remove(&child->dir);
        tmpfs_free_tree(child);
    }
    tmp_dirent_release(dirent);
}

static int tmpfs_umount(struct mount *mount) {
    struct tmp_dirent *root = mount->data;
    if (root != NULL) {
        tmpfs_free_tree(root);
        mount->data = NULL;
    }
    return 0;
}

static struct fd *tmpfs_open(struct mount *mount, const char *path, int flags, int mode) {
    struct tmp_dirent *dirent;
    if (flags & O_CREAT_) {
        // Stale: `path` never has a trailing slash here. tmpfs_open is only
        // ever reached through generic_openat (the sole caller of
        // mount->fs->open), which already resolves a trailing slash in
        // path_raw against the ENOENT/non-directory cases (EISDIR / ENOTDIR)
        // before calling into this backend. Verified against real Linux:
        // O_CREAT on "nonexistent/" -> EISDIR, on "existingfile/" -> ENOTDIR,
        // neither reaches or corrupts this function.
        const char *filename;
        struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
        if (IS_ERR(parent))
            return ERR_PTR(PTR_ERR(parent));
        lock(&parent->lock, 0);
        int err = 0;

        dirent = tmpfs_dir_lookup(parent, filename);
        if (flags & O_EXCL_ && !IS_ERR(dirent)) {
            err = _EEXIST;
            goto out_creat;
        }

        if (dirent == ERR_PTR(_ENOENT)) {
            struct tmp_inode *inode = tmp_inode_new(S_IFREG | mode);
            if (inode == NULL) {
                err = _ENOMEM;
                goto out_creat;
            }
            err = tmpfs_dir_link(parent, filename, inode, &dirent);
            tmp_inode_release(inode);
            if (err < 0) {
                goto out_creat;
            }
        }

out_creat:
        if (err < 0) {
            tmp_dirent_release(dirent);
            dirent = ERR_PTR(err);
        }
        unlock(&parent->lock);
        tmp_dirent_release(parent);
    } else {
        dirent = tmpfs_lookup(mount, path);
    }
    if (IS_ERR(dirent))
        return ERR_PTR(PTR_ERR(dirent));

    struct fd *fd = fd_create(&tmpfs_fdops);
    if (fd == NULL) {
        tmp_dirent_release(dirent);
        return ERR_PTR(_ENOMEM);
    }
    fd->tmpfs.dirent = dirent;

    fd->tmpfs.dir_pos = NULL;
    lock(&dirent->lock, 0);
    if (!list_empty(&dirent->children)) {
        tmpfs_fd_seekdir(fd, list_first_entry(&dirent->children, struct tmp_dirent, dir));
    }
    unlock(&dirent->lock);

    // A FIFO inode shares one named-pipe buffer across all opens; create it
    // lazily and run the POSIX open rendezvous. generic_openat drops the global
    // inodes_lock around this open (open_may_block) so blocking here is safe.
    if (S_ISFIFO(dirent->inode->stat.mode)) {
        struct tmp_inode *inode = dirent->inode;
        lock(&inode->lock, 0);
        if (inode->fifo == NULL)
            inode->fifo = fifo_file_new();
        struct fifo_file *fifo = inode->fifo;
        unlock(&inode->lock);
        if (fifo == NULL) {
            fd_close(fd);
            return ERR_PTR(_ENOMEM);
        }
        // generic_openat only assigns fd->flags after fs->open returns, but the
        // FIFO open rendezvous needs the access mode now.
        fd->flags = flags;
        int ferr = fifo_file_open(fifo, fd);
        if (ferr < 0) {
            fd_close(fd);
            return ERR_PTR(ferr);
        }
    }
    return fd;
}

static int tmpfs_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    struct tmp_inode *inode = dirent->inode;
    lock(&inode->lock, 0);
    *stat = dirent->inode->stat;
    unlock(&inode->lock);
    tmp_dirent_release(dirent);
    return 0;
}

static int tmpfs_unlink(struct mount *mount, const char *path) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return _EPERM;
    lock(&parent->lock, 0);
    int err = tmpfs_dir_unlink(parent, filename, false);
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

static int tmpfs_rmdir(struct mount *mount, const char *path) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return _EBUSY;
    lock(&parent->lock, 0);
    int err = tmpfs_dir_unlink(parent, filename, true);
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

static int tmpfs_apply_attr(struct tmp_inode *inode, struct attr attr) {
    int err = 0;
    lock(&inode->lock, 0);
    switch (attr.type) {
        case attr_uid:
            inode->stat.uid = attr.uid;
            tmpfs_update_ctime(inode);
            break;
        case attr_gid:
            inode->stat.gid = attr.gid;
            tmpfs_update_ctime(inode);
            break;
        case attr_mode:
            inode->stat.mode = (inode->stat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
            tmpfs_update_ctime(inode);
            break;
        case attr_size:
            if (S_ISDIR(inode->stat.mode))
                err = _EISDIR;
            else
                err = tmpfs_file_resize(inode, attr.size);
            break;
        default:
            err = _EPERM;
    }
    unlock(&inode->lock);
    return err;
}

static int tmpfs_setattr(struct mount *mount, const char *path, struct attr attr) {
    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    int err = tmpfs_apply_attr(dirent->inode, attr);
    tmp_dirent_release(dirent);
    return err;
}

static int tmpfs_utime(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime, bool follow_links) {
    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    struct tmp_inode *inode = dirent->inode;
    (void) follow_links;
    lock(&inode->lock, 0);
    inode->stat.atime = atime.tv_sec;
    inode->stat.atime_nsec = atime.tv_nsec;
    inode->stat.mtime = mtime.tv_sec;
    inode->stat.mtime_nsec = mtime.tv_nsec;
    tmpfs_update_ctime(inode);
    unlock(&inode->lock);
    tmp_dirent_release(dirent);
    return 0;
}

static int tmpfs_close(struct fd *fd) {
    // shouldn't need locking as this is the last reference to the fd
    struct tmp_inode *inode = fd->tmpfs.dirent->inode;
    if (S_ISFIFO(inode->stat.mode) && inode->fifo != NULL)
        fifo_file_close(inode->fifo, fd);
    tmp_dirent_release(fd->tmpfs.dirent);
    fd->tmpfs.dirent = NULL;
    return 0;
}

static int tmpfs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    lock(&parent->lock, 0);

    int err = tmpfs_dir_lookup_existence(parent, filename);
    if (err < 0)
        goto out;

    struct tmp_inode *inode = tmp_inode_new(S_IFDIR | mode);
    err = _ENOMEM;
    if (inode == NULL)
        goto out;

    struct tmp_dirent *new_dirent = NULL;
    err = tmpfs_dir_link(parent, filename, inode, &new_dirent);
    if (err == 0) {
        if (tmpfs_is_cgroup2_mount(mount)) {
            lock(&new_dirent->lock, 0);
            err = tmpfs_populate_cgroup2_dir(new_dirent);
            unlock(&new_dirent->lock);
        }
        tmpfs_update_mtime_and_ctime(parent->inode);
    }
    if (new_dirent != NULL)
        tmp_dirent_release(new_dirent);
out:
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

static int tmpfs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ dev) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return _EPERM;
    lock(&parent->lock, 0);

    int err = tmpfs_dir_lookup_existence(parent, filename);
    if (err < 0)
        goto out;

    struct tmp_inode *inode = tmp_inode_new(mode);
    err = _ENOMEM;
    if (inode == NULL)
        goto out;
    inode->stat.rdev = dev;

    err = tmpfs_dir_link(parent, filename, inode, NULL);
    tmp_inode_release(inode);
    if (err == 0)
        tmpfs_update_mtime_and_ctime(parent->inode);
out:
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

static int tmpfs_symlink(struct mount *mount, const char *target, const char *link) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, link, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return _EEXIST;
    lock(&parent->lock, 0);

    int err = tmpfs_dir_lookup_existence(parent, filename);
    if (err < 0)
        goto out;

    struct tmp_inode *inode = tmp_inode_new(S_IFLNK | 0777);
    err = _ENOMEM;
    if (inode == NULL)
        goto out;
    // Store the link target in file_data (the union slot files use for content).
    size_t target_len = strlen(target);
    inode->file_data = malloc(target_len + 1);
    if (inode->file_data == NULL) {
        tmp_inode_release(inode);
        goto out;
    }
    memcpy(inode->file_data, target, target_len);
    inode->stat.size = target_len;

    err = tmpfs_dir_link(parent, filename, inode, NULL);
    tmp_inode_release(inode);
    if (err == 0)
        tmpfs_update_mtime_and_ctime(parent->inode);
out:
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

static ssize_t tmpfs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    struct tmp_inode *inode = dirent->inode;
    ssize_t res;
    lock(&inode->lock, 0);
    if (!S_ISLNK(inode->stat.mode)) {
        res = _EINVAL;
    } else {
        res = inode->stat.size;
        if ((size_t) res > bufsize)
            res = bufsize;
        memcpy(buf, inode->file_data, res);
    }
    unlock(&inode->lock);
    tmp_dirent_release(dirent);
    return res;
}

// ========================
// ======== FD OPS ========
// ========================

static struct tmp_inode *tmpfs_fd_inode(struct fd *fd) {
    return fd->tmpfs.dirent->inode;
}

static int tmpfs_getpath(struct fd *fd, char *buf) {
    struct tmp_dirent *dirent = fd->tmpfs.dirent;
    struct tmp_dirent *root_dirent = fd->mount->data;
    char *p = buf + MAX_PATH - 1;
    *p = '\0';
    while (dirent != root_dirent) {
        size_t name_len = strlen(dirent->name);
        p -= name_len + 1;
        if (p < buf)
            return _ENAMETOOLONG;
        p[0] = '/';
        memcpy(&p[1], dirent->name, name_len);
        dirent = dirent->parent;
    }
    memmove(buf, p, (size_t)((buf + MAX_PATH) - p));
    return 0;
}

static int tmpfs_fstat(struct fd *fd, struct statbuf *stat) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock, 0);
    *stat = inode->stat;
    unlock(&inode->lock);
    return 0;
}

static int tmpfs_fsetattr(struct fd *fd, struct attr attr) {
    return tmpfs_apply_attr(tmpfs_fd_inode(fd), attr);
}

// fd-direct futimens/utimensat(fd, NULL): unlike tmpfs_utime there is no path
// lookup, so this works on an unlinked-but-open file like real Linux.
static int tmpfs_futime(struct fd *fd, struct timespec atime, struct timespec mtime) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock, 0);
    inode->stat.atime = atime.tv_sec;
    inode->stat.atime_nsec = atime.tv_nsec;
    inode->stat.mtime = mtime.tv_sec;
    inode->stat.mtime_nsec = mtime.tv_nsec;
    tmpfs_update_ctime(inode);
    unlock(&inode->lock);
    return 0;
}

static ssize_t tmpfs_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t res;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    if (S_ISFIFO(inode->stat.mode))
        return fifo_file_read(inode->fifo, fd, buf, bufsize);
    lock(&inode->lock, 0);
    res = _EISDIR;
    if (S_ISDIR(inode->stat.mode))
        goto out;
    assert(S_ISREG(inode->stat.mode));

    if (inode->host_fd >= 0) {
        // Host-file-backed (has been mmapped): read the host file so writes
        // made through a MAP_SHARED guest mapping are visible.
        ssize_t n = pread(inode->host_fd, buf, bufsize, fd->offset);
        if (n < 0) {
            res = errno_map();
            goto out;
        }
        fd->offset += n;
        res = n;
        goto out;
    }

    // Clamp to the bytes actually available. The past-EOF check must come
    // first: stat.size and fd->offset are unsigned, so computing
    // stat.size - fd->offset when offset > size underflows to a huge value,
    // leaving bufsize unclamped and making the memcpy read wildly out of
    // bounds (a pread past EOF on tmpfs -> SIGSEGV; Linux just returns 0).
    if (fd->offset >= inode->stat.size)
        bufsize = 0;
    else if (bufsize > inode->stat.size - fd->offset)
        bufsize = inode->stat.size - fd->offset;
    memcpy(buf, inode->file_data + fd->offset, bufsize);
    fd->offset += bufsize;
    res = bufsize;

out:
    unlock(&inode->lock);
    return res;
}

static ssize_t tmpfs_write(struct fd *fd, const void *buf, size_t bufsize) {
    ssize_t res;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    if (S_ISFIFO(inode->stat.mode))
        return fifo_file_write(inode->fifo, fd, buf, bufsize);
    lock(&inode->lock, 0);
    res = _EISDIR;
    if (S_ISDIR(inode->stat.mode))
        goto out;
    assert(S_ISREG(inode->stat.mode));

    // Guard against fd->offset + bufsize wrapping (a write at an offset near
    // SIZE_MAX would otherwise skip the grow and memcpy far past the buffer).
    size_t end;
    if (__builtin_add_overflow((size_t) fd->offset, bufsize, &end)) {
        res = _EFBIG;
        goto out;
    }
    if (inode->host_fd >= 0) {
        // Host-file-backed (has been mmapped): write the host file so the data
        // is visible through any MAP_SHARED guest mapping.
        ssize_t n = pwrite(inode->host_fd, buf, bufsize, fd->offset);
        if (n < 0) {
            res = errno_map();
            goto out;
        }
        fd->offset += n;
        if (inode->stat.size < (size_t) fd->offset)
            inode->stat.size = fd->offset;
        if (n > 0)
            tmpfs_update_mtime_and_ctime(inode);
        res = n;
        goto out;
    }
    if (inode->stat.size < end) {
        res = tmpfs_file_resize(inode, end);
        if (res < 0)
            goto out;
    }
    memcpy(inode->file_data + fd->offset, buf, bufsize);
    fd->offset += bufsize;
    res = bufsize;

out:
    unlock(&inode->lock);
    return res;
}

static off_t_ tmpfs_lseek(struct fd *fd, off_t_ off, int whence) {
    qword_t size = 0;
    if (whence == LSEEK_END) {
        struct tmp_inode *inode = tmpfs_fd_inode(fd);
        lock(&inode->lock, 0);
        size = inode->stat.size;
        unlock(&inode->lock);
    }

    int err = generic_seek(fd, off, whence, size);
    if (err < 0)
        return err;

    return fd->offset;
}

static int tmpfs_readdir(struct fd *fd, struct dir_entry *entry) {
    struct tmp_dirent *parent = fd->tmpfs.dirent;
    int res = _ENOTDIR;
    if (!S_ISDIR(parent->inode->stat.mode))
        goto out;

    lock(&fd->lock, 0);
    lock(&parent->lock, 0);
    struct tmp_dirent *dirent = fd->tmpfs.dir_pos;
    if (dirent == NULL) {
        res = 0;
        goto out;
    }
    struct tmp_dirent *next_dirent = list_next_entry(dirent, dir);
    if (&next_dirent->dir == &parent->children) // end of list
        next_dirent = NULL;
    tmpfs_fd_seekdir(fd, next_dirent);

    entry->inode = dirent->inode->stat.inode;
    entry->type = dir_entry_type_for_mode(dirent->inode->stat.mode);
    strncpy(entry->name, dirent->name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    res = 1;

out:
    unlock(&parent->lock);
    unlock(&fd->lock);
    return res;
}

static unsigned long tmpfs_telldir(struct fd *fd) {
    if (fd->tmpfs.dir_pos == NULL)
        return (unsigned long) -1;
    return fd->tmpfs.dir_pos->index;
}

static void tmpfs_seekdir(struct fd *fd, unsigned long ptr) {
    struct tmp_dirent *dir = fd->tmpfs.dirent;
    lock(&dir->lock, 0);
    assert(S_ISDIR(dir->inode->stat.mode));
    struct tmp_dirent *child;
    list_for_each_entry(&dir->children, child, dir) {
        if (child->index >= ptr)
            break;
    }
    if (&child->dir == &dir->children)
        child = NULL;
    tmpfs_fd_seekdir(fd, child);
    unlock(&dir->lock);
}

#define TMPFS_BLOCK_SIZE 4096

static void tmpfs_count_tree(struct tmp_dirent *dir, uint64_t *pages, uint64_t *inodes) {
    struct tmp_dirent *child;
    lock(&dir->lock, 0);
    list_for_each_entry(&dir->children, child, dir) {
        if (child->inode == NULL)
            continue;
        (*inodes)++;
        if (S_ISREG(child->inode->stat.mode))
            *pages += ((uint64_t) child->inode->stat.size + TMPFS_BLOCK_SIZE - 1) / TMPFS_BLOCK_SIZE;
        if (S_ISDIR(child->inode->stat.mode))
            tmpfs_count_tree(child, pages, inodes);
    }
    unlock(&dir->lock);
}

static int tmpfs_statfs(struct mount *mount, struct statfsbuf *stat) {
    // Linux tmpfs reports the mount's size limit as f_blocks (default: half
    // of RAM) and decrements f_bfree/f_bavail by pages actually stored; the
    // inode cap defaults to the same count as the block cap. There is no
    // size= limit enforcement here, so report the Linux default cap derived
    // from host RAM and subtract what the mount's inodes actually hold.
    uint64_t total_pages = 0;
    long phys_pages = sysconf(_SC_PHYS_PAGES);
    long host_page_size = sysconf(_SC_PAGESIZE);
    if (phys_pages > 0 && host_page_size > 0)
        total_pages = (uint64_t) phys_pages * host_page_size / 2 / TMPFS_BLOCK_SIZE;
    if (total_pages == 0)
        total_pages = 1 << 18; // 1 GiB fallback

    uint64_t used_pages = 0, used_inodes = 1; // root inode
    struct tmp_dirent *root = mount->data;
    if (root != NULL)
        tmpfs_count_tree(root, &used_pages, &used_inodes);

    stat->bsize = TMPFS_BLOCK_SIZE;
    stat->frsize = TMPFS_BLOCK_SIZE;
    stat->blocks = total_pages;
    stat->bfree = stat->bavail = used_pages < total_pages ? total_pages - used_pages : 0;
    stat->files = total_pages;
    stat->ffree = used_inodes < total_pages ? total_pages - used_inodes : 0;
    stat->namelen = 255;
    return 0;
}

// Linux reports zero block and inode counts for cgroup filesystems, just the
// block size and name limit.
static int cgroupfs_statfs(struct mount *UNUSED(mount), struct statfsbuf *stat) {
    stat->bsize = TMPFS_BLOCK_SIZE;
    stat->frsize = TMPFS_BLOCK_SIZE;
    stat->namelen = 255;
    return 0;
}

const struct fs_ops tmpfs = {
    .name = "tmpfs", .magic = 0x01021994,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .statfs = tmpfs_statfs,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .fsetattr = tmpfs_fsetattr,
    .utime = tmpfs_utime,
    .futime = tmpfs_futime,
    .getpath = tmpfs_getpath,
    .mkdir = tmpfs_mkdir,
    .mknod = tmpfs_mknod,
    .symlink = tmpfs_symlink,
    .readlink = tmpfs_readlink,
};

const struct fs_ops cgroupfs = {
    .name = "cgroup", .magic = 0x27e0eb,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .statfs = cgroupfs_statfs,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .fsetattr = tmpfs_fsetattr,
    .utime = tmpfs_utime,
    .futime = tmpfs_futime,
    .getpath = tmpfs_getpath,
    .mkdir = tmpfs_mkdir,
    .mknod = tmpfs_mknod,
    .symlink = tmpfs_symlink,
    .readlink = tmpfs_readlink,
};

const struct fs_ops cgroup2fs = {
    .name = "cgroup2", .magic = 0x63677270,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .statfs = cgroupfs_statfs,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .fsetattr = tmpfs_fsetattr,
    .utime = tmpfs_utime,
    .futime = tmpfs_futime,
    .getpath = tmpfs_getpath,
    .mkdir = tmpfs_mkdir,
    .mknod = tmpfs_mknod,
    .symlink = tmpfs_symlink,
    .readlink = tmpfs_readlink,
};

static int tmpfs_poll(struct fd *fd) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    if (S_ISFIFO(inode->stat.mode))
        return fifo_file_poll(inode->fifo, fd);
    return POLL_READ | POLL_WRITE;
}

static int tmpfs_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    // Linux (verified): mmap of a tmpfs directory or FIFO fails with ENODEV.
    if (!S_ISREG(inode->stat.mode))
        return _ENODEV;
    // Linux mmap access checks (verified): the fd must be readable for any
    // mapping, and MAP_SHARED + PROT_WRITE additionally needs it writable.
    // realfs gets these for free from the host mmap of real_fd (opened with
    // the same access mode); here the backing host fd is always O_RDWR, so
    // check the guest fd's mode explicitly.
    int accmode = fd->flags & O_ACCMODE_;
    if (accmode == O_WRONLY_)
        return _EACCES;
    if ((flags & MMAP_SHARED) && (prot & P_WRITE) && accmode != O_RDWR_)
        return _EACCES;
    // Linux (verified): a file offset that isn't page-aligned is EINVAL.
    if (offset % PAGE_SIZE != 0)
        return _EINVAL;
    lock(&inode->lock, 0);
    int host_fd = tmpfs_inode_host_backing(inode);
    unlock(&inode->lock);
    if (host_fd < 0)
        return host_fd;
    return host_fd_mmap(host_fd, mem, start, pages, offset, prot, flags);
}

const struct fd_ops tmpfs_fdops = {
    .read = tmpfs_read,
    .write = tmpfs_write,
    .poll = tmpfs_poll,
    .lseek = tmpfs_lseek,
    .mmap = tmpfs_mmap,
    .readdir = tmpfs_readdir,
    .telldir = tmpfs_telldir,
    .seekdir = tmpfs_seekdir,
};
