#include <stdlib.h>
#include "fs/mmap_cache.h"
#include "util/list.h"
#include "util/sync.h"

struct mmap_cache_entry {
    dev_t dev;
    ino_t ino;
    off_t offset;
    size_t length;
    unsigned count; // locked by mmap_cache_lock
    struct list chain;
};

#define MMAP_CACHE_HASH_BITS 10
#define MMAP_CACHE_HASH_SIZE (1 << MMAP_CACHE_HASH_BITS)
static lock_t mmap_cache_lock = LOCK_INITIALIZER;
static struct list mmap_cache_hash[MMAP_CACHE_HASH_SIZE];

static void __attribute__((constructor)) init_mmap_cache_hash(void) {
    for (int i = 0; i < MMAP_CACHE_HASH_SIZE; i++)
        list_init(&mmap_cache_hash[i]);
}

static unsigned mmap_cache_hash_key(dev_t dev, ino_t ino, off_t offset, size_t length) {
    unsigned long h = (unsigned long) dev;
    h = h * 31 + (unsigned long) ino;
    h = h * 31 + (unsigned long) offset;
    h = h * 31 + (unsigned long) length;
    return (unsigned) (h % MMAP_CACHE_HASH_SIZE);
}

struct mmap_cache_entry *mmap_cache_register(dev_t dev, ino_t ino, off_t offset, size_t length) {
    unsigned hash = mmap_cache_hash_key(dev, ino, offset, length);
    struct list *bucket = &mmap_cache_hash[hash];
    lock(&mmap_cache_lock, 0);
    struct mmap_cache_entry *entry;
    list_for_each_entry(bucket, entry, chain) {
        if (entry->dev == dev && entry->ino == ino &&
                entry->offset == offset && entry->length == length) {
            entry->count++;
            unlock(&mmap_cache_lock);
            return entry;
        }
    }

    entry = malloc(sizeof(struct mmap_cache_entry));
    if (entry == NULL) {
        unlock(&mmap_cache_lock);
        return NULL;
    }
    entry->dev = dev;
    entry->ino = ino;
    entry->offset = offset;
    entry->length = length;
    entry->count = 1;
    list_init(&entry->chain);
    list_add(bucket, &entry->chain);
    unlock(&mmap_cache_lock);
    return entry;
}

void mmap_cache_unregister(struct mmap_cache_entry *entry) {
    if (entry == NULL)
        return;
    lock(&mmap_cache_lock, 0);
    if (--entry->count == 0) {
        list_remove(&entry->chain);
        free(entry);
    }
    unlock(&mmap_cache_lock);
}

unsigned mmap_cache_count(struct mmap_cache_entry *entry) {
    if (entry == NULL)
        return 1;
    lock(&mmap_cache_lock, 0);
    unsigned count = entry->count;
    unlock(&mmap_cache_lock);
    return count;
}
