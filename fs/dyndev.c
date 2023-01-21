#include "kernel/errno.h"
#include "fs/dev.h"
#include "fs/dyndev.h"
#include "fs/devices.h"

#define MAX_MINOR 255

typedef struct fd rtc_fd;

// Handles DYNDEV_MAJOR device number
// XXX(stek29): unregister might be added later
struct dyn_dev_info {
    // devs & next_dev lock
    lock_t devs_lock;
    // table of dev_ops registered by minor number
    struct dev_ops *devs[MAX_MINOR+1];
};

struct dyn_dev_info dyn_info_char = {
    .devs_lock = LOCK_INITIALIZER,
    .devs = {},
};

int dyn_dev_register(struct dev_ops *ops, int type, int major, int minor) {
    // Validate arguments
    if (minor < 0 || minor > MAX_MINOR) {
        return _EINVAL;
    }
    if (major != DYN_DEV_MAJOR) {
        return _EINVAL;
    }
    if (ops == NULL) {
        return _EINVAL;
    }
    if (type != DEV_CHAR) {
        return _EINVAL;
    }

    lock(&dyn_info_char.devs_lock, 0);

    // Make sure minor number isn't taken yet
    if (dyn_info_char.devs[minor] != NULL) {
        unlock(&dyn_info_char.devs_lock);
        return _EEXIST;
    }

    dyn_info_char.devs[minor] = ops;
    unlock(&dyn_info_char.devs_lock);

    return 0;
}

static int dyn_open(int type, int major, int minor, struct fd *fd) {
    assert(type == DEV_CHAR);
    assert(major == DYN_DEV_MAJOR);
    // it's safe to access devs without locking (read-only)
    struct dev_ops *ops = dyn_info_char.devs[minor];
    if (ops == NULL) {
        return _ENXIO;
    }
    fd->ops = &ops->fd;

    // Succeed if there's no open provided by ops
    if (!ops->open)
        return 0;
    return ops->open(major, minor, fd);
}

static int dyn_open_char(int major, int minor, struct fd *fd) {
    return dyn_open(DEV_CHAR, major, minor, fd);
}

static int rtc_open(int major, int minor, struct fd *fd) {
    return dyn_open(DEV_CHAR, major, minor, fd);
}

static int rtc_close(int major, int minor, struct fd *fd) {
    return 0;
}

static struct tm rtc_read(int major, int minor, struct fd *fd) {
    time_t rawtime;
    struct tm * timeinfo;

    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    return *timeinfo;
}

struct dev_ops dyn_dev_char = {
    .open = dyn_open_char,
};
struct dev_rtc rtc_dev = {
    .open = rtc_open,
    .time = rtc_read,
    .close = rtc_close,
};

/* struct dev_ops clipboard_dev = {
    .open = clipboard_open,
    .fd.read = clipboard_read,
    .fd.write = clipboard_write,
    .fd.lseek = clipboard_lseek,
    .fd.poll = clipboard_poll,
    .fd.close = clipboard_close,
    .fd.fsync = clipboard_write_sync,
}; */
