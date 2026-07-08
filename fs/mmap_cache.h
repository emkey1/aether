#ifndef MMAP_CACHE_H
#define MMAP_CACHE_H

#include <sys/types.h>

// Cross-process accounting for read-only file-backed mmaps that reference
// the exact same host file range (same host device+inode+offset+length).
//
// This does NOT share memory or struct data objects across processes, and
// does not change how mmap/COW/pt_map work. The host OS's own page cache
// (Linux page cache, XNU's unified buffer cache on iOS) already backs
// independent mmap() calls onto the same file+offset with the same
// physical pages for as long as the mapping stays clean -- confirmed
// empirically (see reflection-notes.md). This registry exists purely so
// /proc/<pid>/smaps can report an accurate Pss for that pre-existing
// sharing, which it otherwise has no way to see: a struct data's refcount
// only reflects pt_entries within one fork lineage, never independent
// mmap() calls made by unrelated processes.
//
// Only ever registered for mappings that can't be written by the guest
// (no P_WRITE), so the underlying struct data is guaranteed to never be
// mutated or COW-broken while registered -- see fs/real.c:realfs_mmap.
struct mmap_cache_entry;

// Registers a struct data under (dev, ino, offset, length), creating the
// entry if needed and incrementing its live count. Returns NULL on
// allocation failure; callers should treat that as "not cached" and
// continue (this is purely an accounting aid, never load-bearing).
struct mmap_cache_entry *mmap_cache_register(dev_t dev, ino_t ino, off_t offset, size_t length);
// Decrements the entry's live count, freeing it once it reaches zero.
// Safe to call with entry == NULL (no-op).
void mmap_cache_unregister(struct mmap_cache_entry *entry);
// Current live registration count. Returns 1 for entry == NULL (i.e. "not
// cross-process shared" is the same as "shared with exactly itself").
unsigned mmap_cache_count(struct mmap_cache_entry *entry);

#endif
