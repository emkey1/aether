#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/path.h"
#include "fs/real.h"

#define MAX_FILESYSTEMS 10
static const struct fs_ops *filesystems[MAX_FILESYSTEMS] = {
    &realfs,
    &procfs,
    &aokfs,
    &devptsfs,
    &tmpfs,
    &sysfs,
    &cgroupfs,
    &cgroup2fs,
};

static bool mount_trace_elogind(void) {
    return false;
}

void fs_register(const struct fs_ops *fs) {
    for (unsigned i = 0; i < MAX_FILESYSTEMS; i++) {
        if (filesystems[i] == NULL) {
            filesystems[i] = fs;
            return;
        }
    }
    assert(!"reached filesystem limit");
}

char * get_filesystems(void) {
    unsigned int i;
    size_t total_len = 0;

    // Pass 1: Calculate the exact length required
    for (i = 0; i < MAX_FILESYSTEMS; i++) {
        if (filesystems[i] != NULL) {
            total_len += strlen("nodev    ") + strlen(filesystems[i]->name) + 1; // +1 for newline
        }
    }

    // Pass 2: Allocate and populate the buffer
    char *fs_list = malloc(total_len + 1); // +1 for null terminator
    if (fs_list == NULL)
        return NULL;

    char *ptr = fs_list;
    for (i = 0; i < MAX_FILESYSTEMS; i++) {
        if (filesystems[i] != NULL) {
            size_t name_len = strlen(filesystems[i]->name);
            memcpy(ptr, "nodev    ", 9);
            ptr += 9;
            memcpy(ptr, filesystems[i]->name, name_len);
            ptr += name_len;
            *ptr++ = '\n';
        }
    }
    *ptr = '\0';

    return fs_list;
}

struct mount *mount_find(char *path) {
    assert(path_is_normalized(path));
    lock(&mounts_lock, 0);
    struct mount *mount = NULL;
    if (list_empty(&mounts)) {
        unlock(&mounts_lock);
        return NULL;
    }
    list_for_each_entry(&mounts, mount, mounts) {
        // Optimization: Use cached point_len instead of strlen(mount->point)
        size_t n = mount->point_len;
        if (strncmp(path, mount->point, n) == 0 && (path[n] == '/' || path[n] == '\0'))
            break;
    }
    if (&mount->mounts == &mounts) {
        unlock(&mounts_lock);
        return NULL;
    }
    mount->refcount++;
    unlock(&mounts_lock);
    return mount;
}

void mount_retain(struct mount *mount) {
    lock(&mounts_lock, 0);
    mount->refcount++;
    unlock(&mounts_lock);
}

void mount_release(struct mount *mount) {
    lock(&mounts_lock, 0);
    mount->refcount--;
    unlock(&mounts_lock);
}

int do_mount(const struct fs_ops *fs, const char *source, const char *point, const char *info, int flags) {
    struct mount *new_mount = malloc(sizeof(struct mount));
    if (new_mount == NULL)
        return _ENOMEM;
    new_mount->point = strdup(point);
    new_mount->point_len = strlen(point);
    new_mount->source = strdup(source);
    new_mount->info = strdup(info);
    new_mount->flags = flags;
    new_mount->fs = fs;
    new_mount->data = NULL;
    new_mount->refcount = 0;
    new_mount->bind_origin = NULL;
    new_mount->bind_prefix = NULL;
    if (fs->mount) {
        int err = fs->mount(new_mount);
        if (err < 0) {
            free((void *) new_mount->point);
            free((void *) new_mount->source);
            free(new_mount);
            return err;
        }
    }

    // the list must stay in descending order of mount point length
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        // Optimization: Use cached point_len to avoid O(N) calculations in list traversal
        if (mount->point_len <= new_mount->point_len)
            break;
    }
    list_add_before(&mount->mounts, &new_mount->mounts);
    return 0;
}

int mount_remove(struct mount *mount) {
    if (mount->refcount != 0)
        return _EBUSY;

    if (mount->bind_origin != NULL) {
        // A bind owns one reference on its origin. We hold mounts_lock here
        // (do_umount / exit unmount), so drop it directly rather than via
        // mount_release, which would re-acquire the same non-recursive lock.
        mount->bind_origin->refcount--;
        free((void *) mount->bind_prefix);
    } else if (mount->fs->umount) {
        mount->fs->umount(mount);
    }
    list_remove(&mount->mounts);
    free((void *) mount->info);
    free((void *) mount->source);
    free((void *) mount->point);
    free(mount);
    return 0;
}

int do_umount(const char *point) {
    struct mount *mount;
    bool found = false;
    list_for_each_entry(&mounts, mount, mounts) {
        if (strcmp(point, mount->point) == 0) {
            found = true;
            break;
        }
    }
    if (!found)
        return _EINVAL;
    return mount_remove(mount);
}

// FIXME: this is shit
bool mount_param_flag(const char *info, const char *flag) {
    // Optimization: Hoist strlen(flag) to avoid recalculating it inside the loop
    size_t flag_len = strlen(flag);
    while (*info != '\0') {
        // Corrected logic: Verify exact prefix match and then properly advance past the comma
        if (strncmp(info, flag, flag_len) == 0 && (info[flag_len] == ',' || info[flag_len] == '\0'))
            return true;
        info += strcspn(info, ",");
        if (*info == ',')
            info++;
    }
    return false;
}

#define MS_SUPPORTED (MS_READONLY_|MS_NOSUID_|MS_NODEV_|MS_NOEXEC_|MS_REMOUNT_|MS_NOATIME_|MS_NODIRATIME_|MS_SILENT_|MS_RELATIME_|MS_STRICTATIME_)
#define MS_FLAGS (MS_READONLY_|MS_NOSUID_|MS_NODEV_|MS_NOEXEC_|MS_NOATIME_|MS_NODIRATIME_|MS_RELATIME_|MS_STRICTATIME_)

// Operation-selector bits: like Linux's do_mount(), these pick *what* the call
// does rather than acting as plain mount options.
#define MS_PROPAGATION (MS_SHARED_|MS_PRIVATE_|MS_SLAVE_|MS_UNBINDABLE_)
#define MS_OP (MS_REMOUNT_|MS_BIND_|MS_MOVE_|MS_PROPAGATION)
// Flags iSH accepts but does not act on: durability/atime/symlink-resolution
// niceties plus the recursive-bind modifier. Without mount namespaces these are
// effectively no-ops, so we strip them rather than reject the whole mount.
#define MS_IGNORED (MS_SYNCHRONOUS_|MS_MANDLOCK_|MS_DIRSYNC_|MS_NOSYMFOLLOW_|MS_REC_|MS_POSIXACL_|MS_I_VERSION_|MS_KERNMOUNT_|MS_LAZYTIME_)

// Create a bind mount at `point` aliasing the already-normalized guest path
// `norm_source`. The bind carries no backing of its own: it shares the source's
// real mount via bind_origin/bind_prefix, and find_mount_and_trim_path() does the
// redirect for every path operation.
static int do_bind_mount(const char *norm_source, const char *point, const char *info, int flags) {
    char src[MAX_PATH];
    if (strlen(norm_source) >= sizeof(src))
        return _ENAMETOOLONG;
    strcpy(src, norm_source);

    // Resolve the source to its real backing mount and the path relative to that
    // mount. find_mount_and_trim_path applies any existing bind redirect, so the
    // origin captured here is always a real (non-bind) mount and `src` becomes the
    // source path relative to it. The returned reference is handed to the bind.
    struct mount *origin = find_mount_and_trim_path(src);
    if (origin == NULL)
        return _EINVAL;

    struct mount *bind = malloc(sizeof(struct mount));
    if (bind == NULL) {
        mount_release(origin);
        return _ENOMEM;
    }
    bind->point = strdup(point);
    bind->point_len = strlen(point);
    bind->source = strdup(norm_source);
    bind->info = strdup(info);
    bind->bind_prefix = strdup(src);
    if (bind->point == NULL || bind->source == NULL || bind->info == NULL || bind->bind_prefix == NULL) {
        free((void *) bind->point);
        free((void *) bind->source);
        free((void *) bind->info);
        free((void *) bind->bind_prefix);
        free(bind);
        mount_release(origin);
        return _ENOMEM;
    }
    bind->flags = flags;
    bind->fs = origin->fs;
    bind->refcount = 0;
    bind->root_fd = -1;
    bind->data = NULL;
    bind->bind_origin = origin; // keeps the reference returned above

    lock(&mounts_lock, 0);
    // keep the mounts list ordered by descending mount-point length
    struct mount *after;
    list_for_each_entry(&mounts, after, mounts) {
        if (after->point_len <= bind->point_len)
            break;
    }
    list_add_before(&after->mounts, &bind->mounts);
    unlock(&mounts_lock);
    return 0;
}

dword_t sys_mount_guest(guest_addr_t source_addr, guest_addr_t point_addr, guest_addr_t type_addr, dword_t flags, guest_addr_t data_addr) {
    // source/data/type are copy_mount_string() args in Linux (strndup_user,
    // EINVAL when over-long), NOT getname() pathnames -- so they keep the plain
    // user_read_string path. Only the mount point below is a real getname path.
    char source[MAX_PATH] = "";
    if (source_addr != 0 && user_read_string(source_addr, source, sizeof(source)))
        return _EFAULT;
    char point_raw[MAX_PATH];
    int path_err = user_read_path(point_addr, point_raw, sizeof(point_raw));
    if (path_err)
        return path_err;
    char data[MAX_PATH] = "";
    if (data_addr != 0 && user_read_string(data_addr, data, sizeof(data)))
        return _EFAULT;
    char type[100] = "";
    if (type_addr != 0 && user_read_string(type_addr, type, sizeof(type)))
        return _EFAULT;
    STRACE("mount(\"%s\", \"%s\", \"%s\", %#x, \"%s\")", source, point_raw, type, flags, data_addr != 0 ? data : NULL);
    if (mount_trace_elogind())
        printk("INFO: elogind mount pid=%d comm=%s source=%s target=%s type=%s flags=%#x data=%s\n",
               current->pid, current->comm, source, point_raw, type, flags, data_addr != 0 ? data : "");

    dword_t unhandled = flags & ~MS_SUPPORTED & ~MS_OP & ~MS_IGNORED;
    if (unhandled) {
        FIXME("missing mount flags %#x from %s[%d]", unhandled, current->comm, current->pid);
        return _EINVAL;
    }

    // A propagation-type change (mount --make-[r]private/shared/slave/unbindable)
    // is its own operation that carries no new filesystem. iSH has no mount
    // namespaces or peer groups, so the change has no observable effect -- accept
    // it as a no-op rather than rejecting the caller (systemd issues these at boot).
    if ((flags & MS_PROPAGATION) && !(flags & MS_BIND_))
        return 0;

    struct statbuf stat;
    int err = generic_statat(AT_PWD, point_raw, &stat, 0);
    if (err < 0)
        return err;
    if (!S_ISDIR(stat.mode))
        return _ENOTDIR;

    char point[MAX_PATH];
    err = path_normalize(AT_PWD, point_raw, point, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    // MS_MOVE and MS_BIND name an existing path as their "source"; resolve it
    // before taking mounts_lock (path_normalize / find_mount_and_trim_path
    // re-enter mount_find, which takes the lock).
    char op_source[MAX_PATH];
    if (flags & (MS_MOVE_ | MS_BIND_)) {
        err = path_normalize(AT_PWD, source, op_source, N_SYMLINK_FOLLOW);
        if (err < 0)
            return err;
    }

    // A bind shares the source mount's backing; do_bind_mount resolves the source
    // and takes mounts_lock itself, so dispatch it before we lock here.
    if (flags & MS_BIND_)
        return do_bind_mount(op_source, point, data, flags & MS_FLAGS);

    lock(&mounts_lock, 0);
    if (flags & MS_MOVE_) {
        struct mount *mount, *found = NULL;
        list_for_each_entry(&mounts, mount, mounts) {
            if (strcmp(mount->point, op_source) == 0) {
                found = mount;
                break;
            }
        }
        if (found == NULL) {
            unlock(&mounts_lock);
            return _EINVAL;
        }
        const char *new_point = strdup(point);
        if (new_point == NULL) {
            unlock(&mounts_lock);
            return _ENOMEM;
        }
        free((void *) found->point);
        found->point = new_point;
        found->point_len = strlen(new_point);
        // keep the mounts list ordered by descending mount-point length
        list_remove(&found->mounts);
        struct mount *after;
        list_for_each_entry(&mounts, after, mounts) {
            if (after->point_len <= found->point_len)
                break;
        }
        list_add_before(&after->mounts, &found->mounts);
        unlock(&mounts_lock);
        return 0;
    }

    if (flags & MS_REMOUNT_) {
        struct mount *mount;
        bool found = false;
        list_for_each_entry(&mounts, mount, mounts) {
            bool is_root_remount = strcmp(point, "/") == 0 && mount->point[0] == '\0';
            if (strcmp(point, mount->point) == 0 || is_root_remount) {
                mount->flags = (mount->flags & ~MS_FLAGS) | (flags & MS_FLAGS);
                found = true;
                break;
            }
        }
        unlock(&mounts_lock);
        return found ? 0 : _EINVAL;
    }

    const struct fs_ops *fs = NULL;
    for (size_t i = 0; i < sizeof(filesystems)/sizeof(filesystems[0]); i++) {
        if (filesystems[i] && (strcmp(filesystems[i]->name, type) == 0)) {
            fs = filesystems[i];
            break;
        }
    }
    if (fs == NULL &&
            strcmp(point, "/proc/sys/fs/binfmt_misc") == 0 &&
            (strcmp(type, "binfmt_misc") == 0 ||
             strcmp(source, "binfmt_misc") == 0 ||
             strcmp(source, "none") == 0)) {
        unlock(&mounts_lock);
        return 0;
    }
    if (fs == NULL) {
        unlock(&mounts_lock);
        return _EINVAL;
    }

    err = do_mount(fs, source, point, data, flags & MS_FLAGS);
    unlock(&mounts_lock);
    if (mount_trace_elogind())
        printk("INFO: elogind mount-result pid=%d comm=%s target=%s result=%d\n",
               current->pid, current->comm, point, err);
    return err;
}

dword_t sys_mount(addr_t source_addr, addr_t point_addr, addr_t type_addr, dword_t flags, addr_t data_addr) {
    return sys_mount_guest(source_addr, point_addr, type_addr, flags, data_addr);
}

#define UMOUNT_NOFOLLOW_ 8

dword_t sys_umount2_guest(guest_addr_t target_addr, dword_t flags) {
    char target_raw[MAX_PATH];
    int path_err = user_read_path(target_addr, target_raw, sizeof(target_raw));
    if (path_err)
        return path_err;
    char target[MAX_PATH];
    int err = path_normalize(AT_PWD, target_raw, target,
            flags & UMOUNT_NOFOLLOW_ ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    lock(&mounts_lock, 0);
    err = do_umount(target);
    unlock(&mounts_lock);
    return err;
}

dword_t sys_umount2(addr_t target_addr, dword_t flags) {
    return sys_umount2_guest(target_addr, flags);
}

struct list mounts = {&mounts, &mounts};
lock_t mounts_lock = LOCK_INITIALIZER;
