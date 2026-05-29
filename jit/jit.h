#ifndef JIT_H
#define JIT_H
#define ENGINE_JIT 1
#include <stdatomic.h>
#include "misc.h"
#include "emu/mmu.h"
#include "util/list.h"
#include "util/sync.h"

#if ENGINE_JIT

#define JIT_INITIAL_HASH_SIZE (1 << 10)
#define JIT_CACHE_SIZE (1 << 10)
#define JIT_PAGE_HASH_SIZE (1 << 10)

struct jit {
    // there is one jit per address space
    struct mmu *mmu;
    size_t mem_used;
    size_t num_blocks;

    struct list *hash;
    size_t hash_size;

    // list of jit_blocks that should be freed soon (at the next RCU grace
    // period, if we had such a thing)
    struct list jetsam;

    // A way to look up blocks in a page
    struct {
        struct list blocks[2];
    } *page_hash;
    
    struct {
        pthread_mutex_t lock;
        int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;

    lock_t lock;
    wrlock_t jetsam_lock;
    // Incremented after each jit_free_jetsam in OOM paths; frames compare
    // against last_block_cleanup_seq to detect stale last_block pointers.
    atomic_uint cleanup_seq;
    // Set to true before acquiring jetsam_lock for write, cleared after. All
    // goroutines point their cpu->poked_ptr here, so jit_ret_chain sees this
    // flag on every block boundary and exits jit_enter, releasing the read
    // lock promptly — even if a linked-block cycle somehow forms.
    uint8_t write_wanted;
};

// this is roughly the average number of instructions in a basic block according to anonymous sources
// times 4, roughly the average number of gadgets/parameters in an instruction, according to anonymous sources
#define JIT_BLOCK_INITIAL_CAPACITY 16

struct jit_block {
    guest_addr_t addr;
    guest_addr_t end_addr;
    size_t used;

    // pointers to the ip values in the last gadget
    unsigned long *jump_ip[2];
    // original values of *jump_ip[]
    unsigned long old_jump_ip[2];
    // blocks that jump to this block
    struct list jumps_from[2];

    // hashtable bucket links
    struct list chain;
    // list of blocks in a page
    struct list page[2];
    // links for jumps_from
    struct list jumps_from_links[2];
    // links for free list
    struct list jetsam;
    bool is_jetsam;

    unsigned long code[];
};

// Create a new jit
struct jit *jit_new(struct mmu *mmu);
void jit_free(struct jit *jit);

// Invalidate all jit blocks in pages start (inclusive) to end (exclusive).
// Locks the jit. Should only be called by memory.c in conjunction with
// mem_changed.
void jit_invalidate_range(struct jit *jit, page_t start, page_t end);
void jit_invalidate_page(struct jit *jit, page_t page);
void jit_invalidate_all(struct jit *jit);

bool amd64_jit_is_enabled(void);
void amd64_jit_set_enabled(bool enabled);
bool i386_single_step_comm_matches(const char *comm);
void i386_single_step_comm_set(const char *comm);
void i386_single_step_comm_get(char *buf, size_t bufsize);
bool i386_no_cache_comm_matches(const char *comm);
void i386_no_cache_comm_set(const char *comm);
void i386_no_cache_comm_get(char *buf, size_t bufsize);
void i386_special_trace_reset(pid_t_ tgid, const char *comm);
void i386_trace_special_op(const char *op, addr_t ip);
void i386_trace_special_reg_op(const char *op, addr_t ip, int reg);
struct cpu_state;
void dump_amd64_cc1_jit_trace(const struct cpu_state *cpu);
void jit_cleanup_jetsam_after_interrupt(struct cpu_state *cpu);

#endif

#endif
