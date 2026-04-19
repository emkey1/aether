#include <sys/stat.h>
#include <string.h>
#include <stdint.h>
#include "fs/stat.h"
#include "fs/proc.h"
#include "kernel/task.h"

mode_t_ proc_entry_mode(struct proc_entry *entry) {
    mode_t_ mode = entry->meta->mode;
    if ((mode & S_IFMT) == 0)
        mode |= S_IFREG;
    if ((mode & 0777) == 0) {
        if (S_ISREG(mode))
            mode |= 0444;
        else if (S_ISDIR(mode))
            mode |= 0555;
        else if (S_ISLNK(mode))
            mode |= 0777;
    }
    return mode;
}

int proc_entry_stat(struct proc_entry *entry, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->mode = proc_entry_mode(entry);

    complex_lockt(&pids_lock, 1);
    struct task *task = pid_get_task(entry->pid);
    if (task != NULL) {
        stat->uid = task->uid;
        stat->gid = task->gid;
    } // else the memset above will have initialized memory to zero, which is the root uid/gid
    unlock(&pids_lock);

    stat->inode = proc_entry_inode(entry);
    return 0;
}

qword_t proc_entry_inode(struct proc_entry *entry) {
    // Keep procfs inode numbers unique enough to avoid bogus cycles, but also
    // small enough to fit the legacy 32-bit stat ABI that userspace still hits.
    uint32_t hash = 2166136261U;
#define FNV_MIX_BYTE(v) do { hash ^= (uint8_t) (v); hash *= 16777619U; } while (0)
#define FNV_MIX_VALUE(v) do { \
    uintptr_t value__ = (uintptr_t) (v); \
    for (size_t i__ = 0; i__ < sizeof(value__); i__++) \
        FNV_MIX_BYTE(value__ >> (i__ * 8)); \
} while (0)

    FNV_MIX_VALUE(entry->parent);
    FNV_MIX_VALUE(entry->meta);
    if (entry->meta->getname || entry->meta->name != NULL) {
        char name[MAX_NAME + 1];
        proc_entry_getname(entry, name);
        for (size_t i = 0; name[i] != '\0'; i++)
            FNV_MIX_BYTE(name[i]);
    }
    FNV_MIX_VALUE(entry->pid);
    FNV_MIX_VALUE(entry->fd);

    // Avoid returning 0, which some callers treat as invalid or "not present".
    if (hash == 0)
        hash = 1;
    return hash;
#undef FNV_MIX_VALUE
#undef FNV_MIX_BYTE
}

void proc_entry_getname(struct proc_entry *entry, char *buf) {
    if (entry->meta->getname)
        entry->meta->getname(entry, buf);
    else if (entry->meta->name)
        strcpy(buf, entry->meta->name);
    else
        assert(!"missing name in proc entry");
}

bool proc_dir_read(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    if (entry->meta->readdir)
        return entry->meta->readdir(entry, index, next_entry);

    if (entry->meta->children) {
        if (*index >= entry->meta->children->count)
            return false;
        next_entry->meta = &entry->meta->children->entries[*index];
        next_entry->pid = entry->pid;
        (*index)++;
        return true;
    }
    assert(!"read from invalid proc directory");
}

void free_string_array(char **array) {
    for (int i = 0; array[i] != NULL; i++)
        free(array[i]);
    free(array);
}

void proc_entry_cleanup(struct proc_entry *entry) {
    if (entry->name != NULL)
        free(entry->name);
    if (entry->child_names != NULL)
        free_string_array(entry->child_names);
    *entry = (struct proc_entry) {0};
}

void proc_set_entries_parent(struct proc_dir_entry *entries, size_t count, struct proc_dir_entry *parent) {
    for (size_t i = 0; i < count; i++)
        entries[i].parent = parent;
}

void proc_set_children_parent(struct proc_children *children, struct proc_dir_entry *parent) {
    proc_set_entries_parent(children->entries, children->count, parent);
}

struct proc_dir_entry *proc_find_entry(struct proc_dir_entry *entries, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (entries[i].name != NULL && strcmp(entries[i].name, name) == 0)
            return &entries[i];
    }
    return NULL;
}

struct proc_dir_entry *proc_children_find(struct proc_children *children, const char *name) {
    return proc_find_entry(children->entries, children->count, name);
}
