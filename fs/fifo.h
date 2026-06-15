#ifndef FS_FIFO_H
#define FS_FIFO_H
#include <sys/types.h>
#include <stddef.h>

struct fd;

// A named-pipe (FIFO special file) buffer shared by every fd opened on one
// FIFO inode. A filesystem that backs a FIFO inode (tmpfs, fakefs) keeps one of
// these on the inode and routes the fd's read/write/poll/close to the helpers
// below. This is real pipe behaviour -- a shared ring buffer with reader/writer
// accounting -- not a host pipe (which cannot model a FIFO's EOF/EPIPE and
// buffer-persists-across-opens semantics) and not a stub.
//
// (Named fifo_file* to avoid colliding with util/fifo.c's generic byte FIFO.)
struct fifo_file;

struct fifo_file *fifo_file_new(void);
void fifo_file_free(struct fifo_file *fifo);

// Attach fd as a reader and/or writer (per fd->flags) and perform the POSIX
// FIFO open rendezvous: a blocking O_RDONLY waits for a writer and a blocking
// O_WRONLY waits for a reader. Returns 0 with fd attached, or a negative errno
// with fd left unattached (_ENXIO for O_NONBLOCK|O_WRONLY with no reader,
// _EINTR if interrupted by a signal).
int fifo_file_open(struct fifo_file *fifo, struct fd *fd);
// Detach fd. Safe (no-op) on an fd that was never attached.
void fifo_file_close(struct fifo_file *fifo, struct fd *fd);

ssize_t fifo_file_read(struct fifo_file *fifo, struct fd *fd, void *buf, size_t bufsize);
ssize_t fifo_file_write(struct fifo_file *fifo, struct fd *fd, const void *buf, size_t bufsize);
int fifo_file_poll(struct fifo_file *fifo, struct fd *fd);

#endif
