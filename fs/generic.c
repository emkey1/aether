#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/dev.h"
#include "kernel/inotify.h"
#include "kernel/task.h"
#include "kernel/errno.h"

static struct fdtable *procfd_task_files_retain(struct task *task) {
    struct fdtable *files = NULL;
    lock(&task->general_lock, 0);
    if (!task->exiting && task->files != NULL)
        files = fdtable_retain(task->files);
    unlock(&task->general_lock);
    return files;
}

static struct fd *procfd_reopen_regular(struct fd *fd) {
    if (fd->mount == NULL || fd->mount->fs == &procfs || !S_ISREG(fd->type))
        return NULL;

    char path[MAX_PATH];
    int err = generic_getpath(fd, path);
    if (err < 0)
        return NULL;

    int flags = fd_getflags(fd);
    if (flags < 0)
        return NULL;

    struct fd *reopened = generic_open(path, flags & ~O_CLOEXEC_, 0);
    if (IS_ERR(reopened))
        return NULL;
    return reopened;
}

static struct fd *procfd_openat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return NULL;

    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return NULL;
    if (mount->fs != &procfs) {
        mount_release(mount);
        return NULL;
    }

    int pid;
    int fd_no;
    int n = 0;
    if (sscanf(path, "/%d/fd/%d%n", &pid, &fd_no, &n) != 2 || path[n] != '\0') {
        mount_release(mount);
        return NULL;
    }
    mount_release(mount);

    struct task *task = pid_get_task_ref(pid);
    if (task == NULL)
        return ERR_PTR(_ENOENT);

    struct fdtable *files = procfd_task_files_retain(task);
    if (files == NULL) {
        task_ref_cnt_mod(task, -1);
        return ERR_PTR(_ENOENT);
    }

    lock(&files->lock, 0);
    struct fd *fd = fdtable_get(files, fd_no);
    if (fd == NULL) {
        unlock(&files->lock);
        fdtable_release(files);
        task_ref_cnt_mod(task, -1);
        return ERR_PTR(_ENOENT);
    }
    fd = fd_retain(fd);
    unlock(&files->lock);
    fdtable_release(files);
    task_ref_cnt_mod(task, -1);

    // Linux procfd opens give regular files a fresh file position, which shell
    // script loaders rely on when they execute /proc/self/fd/N after the
    // parent has already inspected the script FD. Prefer a reopen for normal
    // file-backed descriptors.
    struct fd *reopened = procfd_reopen_regular(fd);
    if (reopened != NULL) {
        fd_close(fd);
        return reopened;
    }
    // Deleted or anonymous regular files may not have a stable path we can
    // reopen. We cannot cheaply create a distinct open-file description here,
    // but resetting the retained descriptor keeps shell interpreters from
    // starting mid-script after apk has read the shebang.
    if (S_ISREG(fd->type) && fd->ops != NULL && fd->ops->lseek != NULL)
        fd->ops->lseek(fd, 0, SEEK_SET);
    return fd;
}

struct mount *find_mount_and_trim_path(char *path) {
    struct mount *mount = mount_find(path);
    if (mount == NULL)
        return NULL;
    char *dst = path;
    const char *src = path + mount->point_len;
    while (*src != '\0')
        *dst++ = *src++;
    *dst = '\0';

    // Bind mount: it has no backing of its own, so redirect to the origin mount.
    // Rewrite the (now mount-relative) path to bind_prefix + path and return the
    // origin instead. bind_origin/bind_prefix are immutable for the mount's life,
    // and the reference taken by mount_find keeps the bind alive (and thus those
    // fields valid) while we read them. The caller's buffer is MAX_PATH (every
    // caller normalizes into one), so a redirect that fits is safe to copy back;
    // an over-long result resolves to "not found" rather than overflowing.
    if (mount->bind_origin != NULL) {
        struct mount *origin = mount->bind_origin;
        char redirected[MAX_PATH];
        int n = snprintf(redirected, sizeof(redirected), "%s%s", mount->bind_prefix, path);
        if (n < 0 || (size_t) n >= sizeof(redirected)) {
            mount_release(mount);
            return NULL;
        }
        strcpy(path, redirected);
        mount_retain(origin);
        mount_release(mount);
        return origin;
    }
    return mount;
}

bool contains_mount_point(const char *path) {
    struct mount *mount;
    // Optimization: hoist strlen(path) outside the loop to avoid redundant O(N) recalculations
    int n = strlen(path);
    list_for_each_entry(&mounts, mount, mounts) {
        if (strncmp(path, mount->point, n) == 0 &&
                (mount->point[n] == '\0' || mount->point[n] == '/'))
            return true;
    }
    return false;
}

// fd referring to a symlink itself, from openat(O_PATH|O_NOFOLLOW) on a
// final symlink component (the primitive systemd's chase() is built on).
// No read/write/... ops -> those fail with EBADF, matching Linux O_PATH.
// Owns one mount reference and a malloc'd copy of the mount-relative path;
// fd->mount is deliberately left NULL (fd_close must not run the backend's
// close on an fd the backend never opened).
static int opath_link_close(struct fd *fd) {
    mount_release(fd->opath_link.mount);
    free(fd->opath_link.path);
    return 0;
}

static const struct fd_ops opath_link_ops = {
    .close = opath_link_close,
};

bool fd_is_opath_link(struct fd *fd) {
    // AT_PWD (fs/path.h) is a non-dereferenceable sentinel meaning "current
    // directory", not a real struct fd* -- generic_statat_full's
    // AT_EMPTY_PATH branch runs whenever a caller passes AT_FDCWD together
    // with AT_EMPTY_PATH and an empty path (e.g. systemd's chase() internals
    // do this), so `at` can legitimately be AT_PWD here. Without this check
    // fd->ops dereferenced (struct fd *)-2 + offsetof(ops), which wraps
    // (mod 2^64) to a low, easily-reached address and crashed with SIGSEGV
    // during Arch aarch64 boot.
    return fd != NULL && fd != AT_PWD && fd->ops == &opath_link_ops;
}

struct fd *opath_link_fd_create(struct mount *mount, const char *path) {
    struct fd *fd = adhoc_fd_create(&opath_link_ops);
    if (fd == NULL)
        return NULL;
    fd->opath_link.path = strdup(path);
    if (fd->opath_link.path == NULL) {
        fd->ops = NULL; // nothing to clean up; don't run opath_link_close
        fd_close(fd);
        return NULL;
    }
    fd->opath_link.mount = mount; // takes over the caller's reference
    fd->type = S_IFLNK;
    return fd;
}

// Live stat of the symlink an O_PATH-link fd refers to (fstat/AT_EMPTY_PATH).
int opath_link_fstat(struct fd *fd, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    struct mount *mount = fd->opath_link.mount;
    int err = mount->fs->stat(mount, fd->opath_link.path, stat);
    if (err >= 0 && stat->dev == 0)
        stat->dev = mount->fake_dev;
    return err;
}

struct mount *opath_link_get_mount(struct fd *fd) {
    return fd->opath_link.mount;
}

// readlinkat(fd, "", ...) on an O_PATH symlink fd (Linux allows exactly this).
ssize_t opath_link_readlink(struct fd *fd, char *buf, size_t bufsize) {
    struct mount *mount = fd->opath_link.mount;
    return mount->fs->readlink(mount, fd->opath_link.path, buf, bufsize);
}

struct fd *generic_openat(struct fd *at, const char *path_raw, int flags, int mode) {
    if (flags & O_RDWR_ && flags & O_WRONLY_)
        return ERR_PTR(_EINVAL);

    struct fd *procfd = procfd_openat(at, path_raw);
    if (procfd != NULL)
        return procfd;

    // TODO really, really, seriously reconsider what I'm doing with the strings
    char path[MAX_PATH];
    // O_NOFOLLOW: do not resolve a *final* symlink component (intermediate
    // components are still followed), so opening one fails with ELOOP below.
    int norm = (flags & O_NOFOLLOW_) ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW;
    // N_PARENT_DIR_WRITE is deliberately NOT used here even though O_CREAT is
    // set: at this point we don't yet know whether the target already
    // exists. O_CREAT is very commonly passed defensively on an open() of an
    // existing file (e.g. open(path, O_CREAT|O_WRONLY, mode)), and Linux
    // only requires write access to the parent directory when a new dentry
    // is actually about to be created -- if the target exists, only the
    // target's own permissions matter. So the parent-write check below is
    // deferred until after we know (via the ENOENT-from-stat below) that we
    // are really creating something.
    int err = path_normalize(at, path_raw, path, norm);
    if (err < 0)
        return ERR_PTR(err);
    // A trailing slash demands a directory; open() must not create through it.
    size_t raw_len = strlen(path_raw);
    bool trailing_slash = raw_len > 0 && path_raw[raw_len - 1] == '/';
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return ERR_PTR(_ENOENT);

    bool created = false;

    struct statbuf stat;
    lock(&inodes_lock, 0); // TODO: don't do this

    // Stat before open so permission checks happen before backends can truncate
    // or otherwise mutate an existing file as a side effect of open.
    err = mount->fs->stat(mount, path, &stat);
    if (err < 0) {
        if ((flags & O_CREAT_) && err == _ENOENT) {
            // "newname/" names a directory; open() cannot create one (EISDIR).
            if (trailing_slash) {
                unlock(&inodes_lock);
                mount_release(mount);
                return ERR_PTR(_EISDIR);
            }
            // The target does not exist, so O_CREAT is really about to create
            // a new directory entry: this is the point (unlike the "target
            // already exists" branch below) where Linux requires write+exec
            // permission on the parent directory. path is mount-relative and
            // normalized (find_mount_and_trim_path only trims the mount
            // prefix), so strip the final component to get the parent.
            {
                char parent[MAX_PATH];
                size_t len = strlen(path);
                size_t last_slash = 0;
                for (size_t i = 0; i < len; i++)
                    if (path[i] == '/')
                        last_slash = i;
                if (last_slash == 0)
                    // Root directory: this codebase's mount-relative
                    // representation of the root is "" (see
                    // generic_getpath, fix_path), not "/".
                    parent[0] = '\0';
                else {
                    memcpy(parent, path, last_slash);
                    parent[last_slash] = '\0';
                }
                struct statbuf parent_stat;
                int perr = mount->fs->stat(mount, parent, &parent_stat);
                if (perr >= 0)
                    perr = access_check(&parent_stat, AC_W | AC_X);
                if (perr < 0) {
                    unlock(&inodes_lock);
                    mount_release(mount);
                    return ERR_PTR(perr);
                }
            }
            created = true;
        } else {
            unlock(&inodes_lock);
            mount_release(mount);
            return ERR_PTR(err);
        }
    } else {
        // O_NOFOLLOW: a final symlink we deliberately did not resolve is an
        // error -- unless O_PATH is also set, in which case Linux opens the
        // symlink ITSELF (fstat sees S_IFLNK, readlinkat(fd, "") returns the
        // target, read/write give EBADF). systemd's chase() opens every path
        // component this way, so without this any chase ending on a symlink
        // failed with ELOOP.
        if ((flags & O_NOFOLLOW_) && S_ISLNK(stat.mode)) {
            unlock(&inodes_lock);
            if (flags & O_PATH_) {
                if (flags & O_DIRECTORY_) {
                    mount_release(mount);
                    return ERR_PTR(_ENOTDIR);
                }
                struct fd *lfd = opath_link_fd_create(mount, path);
                if (lfd == NULL) {
                    mount_release(mount);
                    return ERR_PTR(_ENOMEM);
                }
                // opath_link_fd_create took over the mount reference.
                lfd->flags = flags;
                return lfd;
            }
            mount_release(mount);
            return ERR_PTR(_ELOOP);
        }
        // O_PATH ignores the access mode: Linux performs no read/write
        // permission check for O_PATH opens (the fd can't do I/O anyway).
        if (!(flags & O_PATH_)) {
            int accmode;
            if (flags & O_RDWR_) accmode = AC_R | AC_W;
            else if (flags & O_WRONLY_) accmode = AC_W;
            else accmode = AC_R;
            err = access_check(&stat, accmode);
            if (err < 0) {
                unlock(&inodes_lock);
                mount_release(mount);
                return ERR_PTR(err);
            }
        }
    }

    // mount->fs->open can issue a host open() that blocks indefinitely -- most
    // notably opening a FIFO (e.g. syslog-ng's /dev/xconsole) with no peer,
    // which blocks until the other end is opened. inodes_lock is a single global
    // lock (see the "don't do this" above), so holding it across such an open
    // wedges every other open() in the emulator -- the whole app appears to
    // freeze. Drop the lock around the open for files that can block, and
    // re-acquire it for the inode bookkeeping below.
    bool open_may_block = !created && S_ISFIFO(stat.mode) && !(flags & O_NONBLOCK_);
    if (open_may_block)
        unlock(&inodes_lock);
    // Strip O_PATH before handing flags to the backend: its bit value
    // (0x200000) is Darwin's O_SYMLINK, so realfs would otherwise pass a
    // meaningfully different flag to the host open(). A non-symlink O_PATH
    // open behaves like an ordinary open downstream (a deliberate
    // simplification: read() on it succeeds where Linux gives EBADF).
    struct fd *fd = mount->fs->open(mount, path, flags & ~O_PATH_, mode);
    if (open_may_block)
        lock(&inodes_lock, 0);
    if (IS_ERR(fd)) {
        unlock(&inodes_lock);
        // if an error happens after this point, fd_close will release the
        // mount, but right now we need to do it manually
        mount_release(mount);
        return fd;
    }
    fd->mount = mount;

    err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0) {
        unlock(&inodes_lock);
        goto error;
    }
    fd->inode = inode_get_unlocked(mount, stat.inode);
    unlock(&inodes_lock);
    fd->type = stat.mode & S_IFMT;
    fd->flags = flags;

    // path_normalize should have already followed every symlink component
    // (including the final one, unless O_NOFOLLOW), so fd->type should never
    // land here as S_IFLNK. It did on-device under heavy concurrent load
    // (700+ threads, many processes opening shared libraries): fakefs's
    // per-syscall metadata reads (fakefs_readlink at path_normalize time vs.
    // fakefs_fstat here, both against the same SQLite ish_stat row) are not
    // atomic with each other, so a concurrent writer can flip a path's
    // recorded type between the two reads. That is a narrow, rare
    // inconsistency in fakefs locking, not a corrupted filesystem -- but this
    // used to be an assert(), which aborted the whole app on every occurrence.
    // Fail just this open() instead, like Linux does when open() loses a
    // symlink race (ELOOP), and let the caller (sshd's dlopen, in the crash
    // that motivated this) retry or report an ordinary error.
    if (S_ISLNK(fd->type)) {
        err = _ELOOP;
        goto error;
    }
    if (S_ISBLK(fd->type) || S_ISCHR(fd->type)) {
        int type;
        if (S_ISBLK(fd->type))
            type = DEV_BLOCK;
        else
            type = DEV_CHAR;
        err = dev_open(dev_major((dev_t_)stat.rdev), dev_minor((dev_t_)stat.rdev), type, fd);
        if (err < 0)
            goto error;
    }
    err = _ENXIO;
    if (S_ISSOCK(fd->type))
        goto error;
    err = _EISDIR;
    if (S_ISDIR(fd->type) && flags & (O_RDWR_ | O_WRONLY_))
        goto error;
    err = _ENOTDIR;
    if (!S_ISDIR(fd->type) && flags & O_DIRECTORY_)
        goto error;
    inotify_notify_open(path);
    if (created)
        inotify_notify_create(path, S_ISDIR(fd->type));
    return fd;

error:
    fd_close(fd);
    return ERR_PTR(err);
}

struct fd *generic_open(const char *path, int flags, int mode) {
    return generic_openat(AT_PWD, path, flags, mode);
}

int generic_getpath(struct fd *fd, char *buf) {
    if (fd_is_opath_link(fd)) {
        struct mount *mount = fd->opath_link.mount;
        size_t point_len = mount->point_len;
        size_t path_len = strlen(fd->opath_link.path);
        if (point_len + path_len >= MAX_PATH)
            return _ENAMETOOLONG;
        memcpy(buf, mount->point, point_len);
        memcpy(buf + point_len, fd->opath_link.path, path_len + 1);
        if (buf[0] == '\0')
            memcpy(buf, "/", 2);
        return 0;
    }
    if(fd->ops != NULL) {
        int err = fd->mount->fs->getpath(fd, buf);
        if (err < 0)
            return err;
        size_t point_len = fd->mount->point_len;
        size_t buf_len = strlen(buf);
        if (buf_len + point_len >= MAX_PATH)
            return _ENAMETOOLONG;
        memmove(buf + point_len, buf, buf_len + 1);
        memcpy(buf, fd->mount->point, point_len);
        if (buf[0] == '\0')
            memcpy(buf, "/", 2);
        return 0;
    } else {
        return -EBADF;
    }
}

int generic_accessat(struct fd *dirfd, const char *path_raw, int mode) {
    char path[MAX_PATH];
    int err = path_normalize(dirfd, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    struct statbuf stat = {};
    err = mount->fs->stat(mount, path, &stat);
    mount_release(mount);
    if (err < 0)
        return err;
    return access_check(&stat, mode);
}

int generic_linkat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw) {
    char src[MAX_PATH];
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount == NULL || dst_mount == NULL) {
        if (mount != NULL)
            mount_release(mount);
        if (dst_mount != NULL)
            mount_release(dst_mount);
        return _ENOENT;
    }
    // Serialize against generic_openat/generic_mkdirat/etc. on the same path:
    // see the inodes_lock comment in generic_openat for why fakefs needs this
    // (a mutating fs op is a real-host-op + SQLite-metadata-update pair that
    // isn't atomic against a concurrent one of these on its own).
    lock(&inodes_lock, 0); // TODO: don't do this
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->link == NULL)
        err = _EPERM;
    else
        err = mount->fs->link(mount, src, dst);
    unlock(&inodes_lock);
    mount_release(mount);
    mount_release(dst_mount);
    return err;
}

int generic_unlinkat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    // See the inodes_lock comment in generic_openat: this serializes the
    // stat-check + unlink pair against a concurrent open(O_CREAT)/mkdir/etc.
    // on the same path, so fakefs's real-op + metadata-update pair can't
    // interleave with another one and leave the metadata mismatched with
    // what's actually on the host filesystem.
    lock(&inodes_lock, 0); // TODO: don't do this
    // Linux reports EISDIR for unlink of a directory. Enforce it here so the
    // host's own errno (EPERM on Darwin/iOS hosts) does not leak to the guest.
    struct statbuf ust;
    if (mount->fs->stat(mount, path, &ust) >= 0 && S_ISDIR(ust.mode)) {
        unlock(&inodes_lock);
        mount_release(mount);
        return _EISDIR;
    }
    err = _EPERM;
    if (mount->fs->unlink)
        err = mount->fs->unlink(mount, path);
    unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_delete(path, false);
    return err;
}

int generic_renameat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw, int flags) {
    // RENAME_NOREPLACE is implemented; RENAME_EXCHANGE/WHITEOUT and any unknown
    // flag are rejected with EINVAL (Linux's response for unsupported flags).
    if (flags & ~RENAME_NOREPLACE_)
        return _EINVAL;
    char src[MAX_PATH];
    // Linux requires write+exec on both the source and destination parent
    // directories for rename (removing the entry from one, adding it to the
    // other), not just the destination.
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(src))
        return _EBUSY;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount == NULL || dst_mount == NULL) {
        if (mount != NULL)
            mount_release(mount);
        if (dst_mount != NULL)
            mount_release(dst_mount);
        return _ENOENT;
    }
    // See the inodes_lock comment in generic_openat: serialize the
    // stat-check(s) + rename pair against a concurrent open(O_CREAT)/mkdir/
    // unlink/etc. on either path.
    lock(&inodes_lock, 0); // TODO: don't do this
    bool is_dir = false;
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->rename == NULL)
        err = _EPERM;
    else {
        struct statbuf stat;
        if ((flags & RENAME_NOREPLACE_) && mount->fs->stat(mount, dst, &stat) >= 0) {
            err = _EEXIST;
        } else {
            if (mount->fs->stat(mount, src, &stat) >= 0)
                is_dir = S_ISDIR(stat.mode);
            err = mount->fs->rename(mount, src, dst);
        }
    }
    unlock(&inodes_lock);
    mount_release(mount);
    mount_release(dst_mount);
    if (err >= 0)
        inotify_notify_move(src, dst, is_dir);
    return err;
}

int generic_symlinkat(const char *target, struct fd *at, const char *link_raw) {
    char link[MAX_PATH];
    int err = path_normalize(at, link_raw, link, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(link);
    if (mount == NULL)
        return _ENOENT;
    // See the inodes_lock comment in generic_openat: serializes the
    // real-symlink-create + metadata-write pair against a concurrent
    // open(O_CREAT)/mkdir/unlink/etc. on the same path.
    lock(&inodes_lock, 0); // TODO: don't do this
    err = _EPERM;
    if (mount->fs->symlink)
        err = mount->fs->symlink(mount, target, link);
    unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_create(link, false);
    return err;
}

int generic_mknodat(struct fd *at, const char *path_raw, mode_t_ mode, dev_t_ dev) {
    if (S_ISDIR(mode) || S_ISLNK(mode))
        return _EINVAL;
    if (!superuser() && (S_ISBLK(mode) || S_ISCHR(mode)))
        return _EPERM;

    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    // See the inodes_lock comment in generic_openat: serializes the
    // real-mknod + metadata-write pair against a concurrent
    // open(O_CREAT)/mkdir/unlink/etc. on the same path.
    lock(&inodes_lock, 0); // TODO: don't do this
    err = _EPERM;
    if (mount->fs->mknod)
        err = mount->fs->mknod(mount, path, mode, dev);
    unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_create(path, false);
    return err;
}

int generic_setattrat(struct fd *at, const char *path_raw, struct attr attr, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    err = _EPERM;
    if (mount->fs->setattr)
        err = mount->fs->setattr(mount, path, attr);
    mount_release(mount);
    if (err >= 0) {
        if (attr.type == attr_size)
            inotify_notify_modify(path);
        else
            inotify_notify_attrib(path);
    }
    return err;
}

int generic_utime(struct fd *at, const char *path_raw, struct timespec atime, struct timespec mtime, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    err = _EPERM;
    if (mount->fs->utime)
        err = mount->fs->utime(mount, path, atime, mtime, follow_links);
    mount_release(mount);
    return err;
}

ssize_t generic_readlinkat(struct fd *at, const char *path_raw, char *buf, size_t bufsize) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    err = _EINVAL;
    if (mount->fs->readlink)
        err = mount->fs->readlink(mount, path, buf, bufsize);
    mount_release(mount);
    return err;
}

int generic_mkdirat(struct fd *at, const char *path_raw, mode_t_ mode) {
    char path[MAX_PATH];
    // The final component is the name being created and is never followed, so
    // mkdir over an existing (even dangling) symlink reports EEXIST like Linux.
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    // See the inodes_lock comment in generic_openat: serializes the
    // exists-check + real-mkdir + metadata-write against a concurrent
    // open(O_CREAT)/unlink/mkdir/etc. on the same path.
    lock(&inodes_lock, 0); // TODO: don't do this
    struct statbuf stat;
    err = mount->fs->stat(mount, path, &stat);
    if (err == 0) {
        unlock(&inodes_lock);
        mount_release(mount);
        return _EEXIST;
    }
    if (err < 0 && err != _ENOENT) {
        unlock(&inodes_lock);
        mount_release(mount);
        return err;
    }
    err = _EPERM;
    if (mount->fs->mkdir)
        err = mount->fs->mkdir(mount, path, mode);
    unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_create(path, true);
    return err;
}

int generic_rmdirat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    // rmdir does not follow a final symlink: rmdir("symlink-to-dir") is ENOTDIR.
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(path))
        return _EBUSY;
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    // See the inodes_lock comment in generic_openat: serializes the
    // real-rmdir + metadata-update against a concurrent
    // open(O_CREAT)/mkdir/unlink/etc. on the same path.
    lock(&inodes_lock, 0); // TODO: don't do this
    err = _EPERM;
    if (mount->fs->rmdir)
        err = mount->fs->rmdir(mount, path);
    unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_delete(path, true);
    return err;
}

int generic_seek(struct fd *fd, off_t_ off, int whence, size_t size) {
    off_t_ new_off = fd->offset;
    if (whence == LSEEK_SET) {
        fd->offset = off;
    } else if (whence == LSEEK_CUR) {
        if (__builtin_add_overflow(new_off, off, &new_off) || new_off < 0)
            return _EINVAL;
        fd->offset = new_off;
    } else if (whence == LSEEK_END) {
        new_off = size + off;
        if (new_off < 0)
            return _EINVAL;
        fd->offset = new_off;
    } else {
        return _EINVAL;
    }
    return 0;
}
