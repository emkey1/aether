#include <string.h>
#include <sys/stat.h>
#include "debug.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "kernel/errno.h"

static struct mount adhoc_mount;
static unsigned adhoc_inode_seq;

struct fd *adhoc_fd_create(const struct fd_ops *ops) {
    struct fd *fd = fd_create(ops);
    if (fd == NULL)
        return NULL;
    mount_retain(&adhoc_mount);
    fd->mount = &adhoc_mount;
    fd->stat = (struct statbuf) {};
    // Give every adhoc fd a unique nonzero inode so /proc/<pid>/fd/<n> and lsof
    // can tell sockets/pipes/eventfds apart. st_dev is 0 for these, so they
    // never collide with real-fs inodes.
    fd->stat.inode = __atomic_add_fetch(&adhoc_inode_seq, 1, __ATOMIC_RELAXED);
    return fd;
}

static int adhoc_fstat(struct fd *fd, struct statbuf *stat) {
    *stat = fd->stat;
    return 0;
}

static int adhoc_fsetattr(struct fd *fd, struct attr attr) {
    switch (attr.type) {
        case attr_uid:
            fd->stat.uid = attr.uid;
            break;
        case attr_gid:
            fd->stat.gid = attr.gid;
            break;
        case attr_mode:
            fd->stat.mode = (fd->stat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
            break;
        case attr_size:
            return _EINVAL;
    }
    return 0;
}

static int adhoc_getpath(struct fd *fd, char *buf) {
    // Render the way Linux does in /proc/<pid>/fd: sockets and pipes show their
    // type plus inode; everything else is anon_inode:[class], where the class
    // (eventfd, eventpoll, signalfd, ...) comes from the fd's ops. Previously
    // every adhoc fd reported "anon_inode:[unknown]" with a zero inode, so lsof
    // could not classify sockets/eventfds (the listener of a daemon like sshd
    // showed up as an unstattable anon_inode).
    unsigned long ino = (unsigned long) fd->stat.inode;
    if (S_ISSOCK(fd->stat.mode))
        snprintf(buf, MAX_PATH, "socket:[%lu]", ino);
    else if (S_ISFIFO(fd->stat.mode))
        snprintf(buf, MAX_PATH, "pipe:[%lu]", ino);
    else {
        const char *cls = (fd->ops != NULL && fd->ops->anon_inode_class != NULL)
            ? fd->ops->anon_inode_class : "anon_inode";
        snprintf(buf, MAX_PATH, "anon_inode:[%s]", cls);
    }
    return 0;
}

bool is_adhoc_fd(struct fd *fd) {
    return fd->mount == &adhoc_mount;
}

// futimens() on a socket/pipe/anon fd sets the times on its (anonymous) inode
// on Linux; mirror that on the fd's fake stat so fstat reflects the update.
// (stress-ng --sockabuse futimens()es a socket fd in a loop.)
static int adhoc_futime(struct fd *fd, struct timespec atime, struct timespec mtime) {
    fd->stat.atime = (dword_t) atime.tv_sec;
    fd->stat.atime_nsec = (dword_t) atime.tv_nsec;
    fd->stat.mtime = (dword_t) mtime.tv_sec;
    fd->stat.mtime_nsec = (dword_t) mtime.tv_nsec;
    return 0;
}

static const struct fs_ops adhoc_fs = {
    .magic = 0x09041934, // FIXME wrong for pipes and sockets
    .fstat = adhoc_fstat,
    .fsetattr = adhoc_fsetattr,
    .futime = adhoc_futime,
    .getpath = adhoc_getpath,
};

static struct mount adhoc_mount = {
    .fs = &adhoc_fs,
    .point = "",
};
