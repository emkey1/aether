#define DEFAULT_CHANNEL instr
#include "debug.h"
#include "jit/jit.h"
#include "jit/gen.h"
#include "jit/frame.h"
#include "emu/cpu.h"
#include "emu/memory.h"
#include "emu/interrupt.h"
#include "kernel/task.h"
#include "util/list.h"
#include "util/sync.h"
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>

extern int current_pid(struct task *task);

static atomic_bool amd64_jit_enabled = false;
static pthread_mutex_t i386_single_step_comm_lock = PTHREAD_MUTEX_INITIALIZER;
static char i386_single_step_comm[16] = "";
static pthread_mutex_t i386_no_cache_comm_lock = PTHREAD_MUTEX_INITIALIZER;
static char i386_no_cache_comm[16] = "";
static pthread_mutex_t i386_special_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t_ i386_special_trace_tgid;
static char i386_special_trace_comm[16] = "";
static unsigned i386_special_trace_count;

bool amd64_jit_is_enabled(void) {
    return atomic_load_explicit(&amd64_jit_enabled, memory_order_relaxed);
}

void amd64_jit_set_enabled(bool enabled) {
    atomic_store_explicit(&amd64_jit_enabled, enabled, memory_order_relaxed);
}

bool i386_single_step_comm_matches(const char *comm) {
    bool match = false;
    if (comm == NULL || comm[0] == '\0')
        return false;
    pthread_mutex_lock(&i386_single_step_comm_lock);
    match = i386_single_step_comm[0] != '\0' &&
            strcmp(i386_single_step_comm, comm) == 0;
    pthread_mutex_unlock(&i386_single_step_comm_lock);
    return match;
}

void i386_single_step_comm_set(const char *comm) {
    pthread_mutex_lock(&i386_single_step_comm_lock);
    if (comm == NULL) {
        i386_single_step_comm[0] = '\0';
    } else {
        strncpy(i386_single_step_comm, comm, sizeof(i386_single_step_comm));
        i386_single_step_comm[sizeof(i386_single_step_comm) - 1] = '\0';
    }
    pthread_mutex_unlock(&i386_single_step_comm_lock);
}

void i386_single_step_comm_get(char *buf, size_t bufsize) {
    if (buf == NULL || bufsize == 0)
        return;
    pthread_mutex_lock(&i386_single_step_comm_lock);
    strncpy(buf, i386_single_step_comm, bufsize);
    buf[bufsize - 1] = '\0';
    pthread_mutex_unlock(&i386_single_step_comm_lock);
}

bool i386_no_cache_comm_matches(const char *comm) {
    bool match = false;
    if (comm == NULL || comm[0] == '\0')
        return false;
    pthread_mutex_lock(&i386_no_cache_comm_lock);
    match = i386_no_cache_comm[0] != '\0' &&
            strcmp(i386_no_cache_comm, comm) == 0;
    pthread_mutex_unlock(&i386_no_cache_comm_lock);
    return match;
}

void i386_no_cache_comm_set(const char *comm) {
    pthread_mutex_lock(&i386_no_cache_comm_lock);
    if (comm == NULL) {
        i386_no_cache_comm[0] = '\0';
    } else {
        strncpy(i386_no_cache_comm, comm, sizeof(i386_no_cache_comm));
        i386_no_cache_comm[sizeof(i386_no_cache_comm) - 1] = '\0';
    }
    pthread_mutex_unlock(&i386_no_cache_comm_lock);
}

void i386_no_cache_comm_get(char *buf, size_t bufsize) {
    if (buf == NULL || bufsize == 0)
        return;
    pthread_mutex_lock(&i386_no_cache_comm_lock);
    strncpy(buf, i386_no_cache_comm, bufsize);
    buf[bufsize - 1] = '\0';
    pthread_mutex_unlock(&i386_no_cache_comm_lock);
}

void i386_special_trace_reset(pid_t_ tgid, const char *comm) {
    pthread_mutex_lock(&i386_special_trace_lock);
    i386_special_trace_tgid = tgid;
    i386_special_trace_count = 0;
    if (comm == NULL) {
        i386_special_trace_comm[0] = '\0';
    } else {
        strncpy(i386_special_trace_comm, comm, sizeof(i386_special_trace_comm));
        i386_special_trace_comm[sizeof(i386_special_trace_comm) - 1] = '\0';
    }
    pthread_mutex_unlock(&i386_special_trace_lock);
}

void i386_trace_special_op(const char *op, addr_t ip) {
    (void) op;
    (void) ip;
}

void i386_trace_special_reg_op(const char *op, addr_t ip, int reg) {
    (void) op;
    (void) ip;
    (void) reg;
}

// Defined in app/hook.c; installs EXC_BAD_ACCESS handler on the calling thread.
// No-op on non-arm64.  Forward-declared here to avoid a circular header dep.
extern void jit_install_thread_exception_handler(void);

// Thread-local jetsam_lock pointer for JIT crash recovery.
// Set to &jit->jetsam_lock while this thread is in cpu_step_to_interrupt (i.e.
// while the read lock is held).  Cleared before returning.  Accessed by
// jit_crash_fn() which runs on the same thread after a Mach exception redirect.
__thread wrlock_t *jit_crash_lock = NULL;
__thread sigjmp_buf jit_crash_unwind_buf;
__thread bool jit_crash_unwind_active = false;
__thread struct jit_frame *jit_crash_frame = NULL;
__thread struct cpu_state *jit_crash_cpu = NULL;
__thread int jit_crash_interrupt = INT_GPF;
__thread addr_t jit_crash_addr = 0;

static void jit_block_disconnect(struct jit *jit, struct jit_block *block);
static void jit_block_free(struct jit *jit, struct jit_block *block);
static void jit_free_jetsam(struct jit *jit);
static void jit_resize_hash(struct jit *jit, size_t new_size);

// Called by hook.c's Mach exception handler when a JIT thread faults while
// executing translated code. The handler redirects the faulting thread's PC
// here, so this runs on the faulting thread and can safely access thread-local
// state.
//
// Releasing the jetsam_lock read lock prevents write-lock waiters
// (cpu_run_to_interrupt doing jetsam cleanup) from blocking forever, which
// would hang any guest program that spawns many OS threads (e.g. Go programs).
__attribute__((__noreturn__))
void jit_crash_fn(void) {
    // If this thread holds atomic_l_lock (possible when the crash occurs inside
    // read_lock/read_unlock, in the narrow window where both that mutex and the
    // jetsam read lock are held), release it first so other threads aren't
    // permanently blocked on it.
    extern lock_t atomic_l_lock;
    extern bool doEnableExtraLocking;
    if (doEnableExtraLocking && pthread_equal(atomic_l_lock.owner, pthread_self())) {
        atomic_l_lock.owner = zero_init(pthread_t);
        pthread_mutex_unlock(&atomic_l_lock.m);
    }

    if (jit_crash_lock != NULL) {
        printk("JIT: crash recovery (pid %d) - releasing jetsam_lock after bad-access fault\n",
               current ? current->pid : -1);
        pthread_rwlock_unlock(&jit_crash_lock->l);
        jit_crash_lock = NULL;
        if (jit_crash_unwind_active)
            siglongjmp(jit_crash_unwind_buf, 1);
    } else {
        // EXC_BAD_ACCESS outside JIT execution context — real bug, let it crash.
        abort();
    }
    pthread_exit(NULL);
}

// Acquire jetsam write lock with a short timeout. Uses non-blocking
// pthread_rwlock_trywrlock in a retry loop so that a permanently stuck
// read-lock holder (e.g. a goroutine that crashed in jit_enter while LLDB
// intercepted the exception before jit_crash_fn could release the lock)
// doesn't deadlock all write-lock waiters. Callers must unlock with
// pthread_rwlock_unlock(&jit->jetsam_lock.l) on success.
static bool jetsam_write_lock_timed(struct jit *jit) {
    static const struct timespec kDelay = {0, 5000000}; // 5ms per retry
    for (int i = 0; i < 20; i++) {                      // up to 100ms total
        if (pthread_rwlock_trywrlock(&jit->jetsam_lock.l) == 0)
            return true;
        nanosleep(&kDelay, NULL);
    }
    return false;
}

static bool jit_i386_gpf_addr_accessible(guest_addr_t addr, int type) {
    if (current == NULL || addr == 0)
        return false;

    bool accessible = false;
    if (trylockr(&current->mem->lock) != 0)
        return false;
    struct pt_entry *entry = mem_pt(current->mem, PAGE(addr));
    if (entry != NULL && entry->data != NULL && entry->data->data != NULL) {
        accessible = type != MEM_WRITE || P_WRITABLE(entry->flags);
    }
    read_unlock(&current->mem->lock);
    return accessible;
}

static bool jit_i386_gpf_looks_retryable(struct cpu_state *cpu) {
    if (current == NULL || current->abi == GUEST_ABI_AMD64)
        return false;
    if (cpu->segfault_addr == 0)
        return false;

    bool read_ok = jit_i386_gpf_addr_accessible(cpu->segfault_addr, MEM_READ);
    bool write_ok = jit_i386_gpf_addr_accessible(cpu->segfault_addr, MEM_WRITE);
    if (cpu->segfault_was_write)
        return write_ok;
    return read_ok || write_ok;
}

struct jit *jit_new(struct mmu *mmu) {
    struct jit *jit = calloc(1, sizeof(struct jit));
    if (!jit) {
        // Handle allocation failure
        printk("ERROR: Failed to allocate memory for JIT\n");
        return NULL;
    }
    lock(&jit->lock, 0);
    jit->mmu = mmu;
    jit_resize_hash(jit, JIT_INITIAL_HASH_SIZE);
    jit->page_hash = calloc(JIT_PAGE_HASH_SIZE, sizeof(*jit->page_hash));
    list_init(&jit->jetsam);
    lock_init(&jit->lock, "jit_new\0");
    wrlock_init(&jit->jetsam_lock);
    unlock(&jit->lock);
    return jit;
}

void jit_free(struct jit *jit) {
    if (!jit) return;
    lock(&jit->lock, 0);
    for (size_t i = 0; i < jit->hash_size; i++) {
        struct jit_block *block, *tmp;
        if (list_null(&jit->hash[i]))
            continue;
        list_for_each_entry_safe(&jit->hash[i], block, tmp, chain) {
            jit_block_free(jit, block);
        }
    }
    jit_free_jetsam(jit);
    free(jit->page_hash);
    free(jit->hash);
    write_lock(&jit->jetsam_lock);
    unlock(&jit->lock);
    free(jit);
}

static inline struct list *blocks_list(struct jit *jit, page_t page, int i) {
    // TODO is this a good hash function?
    return &jit->page_hash[page % JIT_PAGE_HASH_SIZE].blocks[i];
}

void jit_invalidate_range(struct jit *jit, page_t start, page_t end) {
    lock(&jit->lock, 0);
    struct jit_block *block, *tmp;
    for (page_t page = start; page < end; page++) {
        for (int i = 0; i <= 1; i++) {
            struct list *blocks = blocks_list(jit, page, i);
            if (list_null(blocks))
                continue;
            list_for_each_entry_safe(blocks, block, tmp, page[i]) {
                jit_block_disconnect(jit, block);
                block->is_jetsam = true;
                list_add(&jit->jetsam, &block->jetsam);
            }
        }
    }
    unlock(&jit->lock);
}

void jit_invalidate_page(struct jit *jit, page_t page) {
    jit_invalidate_range(jit, page, page + 1);
}

void jit_invalidate_all(struct jit *jit) {
    struct mem *mem = container_of(jit->mmu, struct mem, mmu);
    jit_invalidate_range(jit, 0, mem->page_limit);
}

static void jit_resize_hash(struct jit *jit, size_t new_size) {
    TRACE_(verbose, "%d resizing hash to %lu, using %lu bytes for gadgets\n", current_pid(current), new_size, jit->mem_used);
    struct list *new_hash = calloc(new_size, sizeof(struct list));
    for (size_t i = 0; i < jit->hash_size; i++) {
        if (list_null(&jit->hash[i]))
            continue;
        struct jit_block *block, *tmp;
        list_for_each_entry_safe(&jit->hash[i], block, tmp, chain) {
            list_remove(&block->chain);
            list_init_add(&new_hash[block->addr % new_size], &block->chain);
        }
    }
    free(jit->hash);
    jit->hash = new_hash;
    jit->hash_size = new_size;
}

static void jit_insert(struct jit *jit, struct jit_block *block) {
    jit->mem_used += block->used;
    jit->num_blocks++;
    // target an average hash chain length of 1-2
    if (jit->num_blocks >= jit->hash_size * 2)
        jit_resize_hash(jit, jit->hash_size * 2);

    list_init_add(&jit->hash[block->addr % jit->hash_size], &block->chain);
    list_init_add(blocks_list(jit, PAGE(block->addr), 0), &block->page[0]);
    if (PAGE(block->addr) != PAGE(block->end_addr))
        list_init_add(blocks_list(jit, PAGE(block->end_addr), 1), &block->page[1]);
}

static struct jit_block *jit_lookup(struct jit *jit, addr_t addr) {
    struct list *bucket = &jit->hash[addr % jit->hash_size];
    if (list_null(bucket))
        return NULL;
    struct jit_block *block;
    list_for_each_entry(bucket, block, chain) {
        if (block->addr == addr)
            return block;
    }
    return NULL;
}

static struct jit_block *jit_block_compile_common(addr_t ip, struct tlb *tlb,
        bool amd64, bool *fallback_to_interp) {
    struct gen_state state;
    TRACE("%d %08x --- compiling:\n", current_pid(current), ip);

    if (fallback_to_interp != NULL)
        *fallback_to_interp = false;

    if (!(amd64 ? gen_start_amd64(ip, &state) : gen_start(ip, &state)))
        return NULL;
    state.oom_active = true;
    if (setjmp(state.oom_recovery) != 0) {
        // OOM hit during compilation; free the partial block and signal failure
        free(state.block);
        return NULL;
    }
    while (true) {
        if (!gen_step(&state, tlb))
            break;
        // no block should span more than 2 pages
        // guarantee this by limiting total block size to 1 page
        // guarantee that by stopping as soon as there's less space left than
        // the maximum length of an x86 instruction
        // TODO refuse to decode instructions longer than 15 bytes
        if (state.ip - ip >= PAGE_SIZE - 15) {
            gen_exit(&state);
            break;
        }
    }

    if (amd64 && state.amd64_fallback_to_interp && state.size == 0) {
        free(state.block);
        if (fallback_to_interp != NULL)
            *fallback_to_interp = true;
        return NULL;
    }

    gen_end(&state);
    assert(state.ip - ip <= PAGE_SIZE);
    state.block->used = state.capacity;
    return state.block;
}

static struct jit_block *jit_block_compile(addr_t ip, struct tlb *tlb) {
    return jit_block_compile_common(ip, tlb, false, NULL);
}

static struct jit_block *jit_block_compile_amd64(addr_t ip, struct tlb *tlb,
        bool *fallback_to_interp) {
    return jit_block_compile_common(ip, tlb, true, fallback_to_interp);
}

// Remove all pointers to the block. It can't be freed yet because another
// thread may be executing it.
static void jit_block_disconnect(struct jit *jit, struct jit_block *block) {
    if (jit != NULL) {
        jit->mem_used -= block->used;
        jit->num_blocks--;
    }
    list_remove(&block->chain);
    for (int i = 0; i <= 1; i++) {
        list_remove(&block->page[i]);
        list_remove_safe(&block->jumps_from_links[i]);

        struct jit_block *prev_block, *tmp;
        
        list_for_each_entry_safe(&block->jumps_from[i], prev_block, tmp, jumps_from_links[i]) {
            if (prev_block->jump_ip[i] != NULL)
                *prev_block->jump_ip[i] = prev_block->old_jump_ip[i]; // Crashed here June 12 2022, 19 Nov 2022
            list_remove(&prev_block->jumps_from_links[i]);
        }
    }
}

static void jit_block_free(struct jit *jit, struct jit_block *block) {
    jit_block_disconnect(jit, block);
    free(block);
}

static void jit_free_jetsam(struct jit *jit) {
    struct jit_block *block, *tmp;
    list_for_each_entry_safe(&jit->jetsam, block, tmp, jetsam) {
        list_remove(&block->jetsam);
        free(block);
    }
}

int jit_enter(struct jit_block *block, struct jit_frame *frame, struct tlb *tlb);

static inline size_t jit_cache_hash(addr_t ip) {
    return (ip ^ (ip >> 12)) % JIT_CACHE_SIZE;
}

static void amd64_seed_legacy_exec_state(struct cpu_state *cpu) {
    cpu->eax = (dword_t) cpu->amd64_regs[amd64_rax];
    cpu->ecx = (dword_t) cpu->amd64_regs[amd64_rcx];
    cpu->edx = (dword_t) cpu->amd64_regs[amd64_rdx];
    cpu->ebx = (dword_t) cpu->amd64_regs[amd64_rbx];
    cpu->esp = (dword_t) cpu->amd64_regs[amd64_rsp];
    cpu->ebp = (dword_t) cpu->amd64_regs[amd64_rbp];
    cpu->esi = (dword_t) cpu->amd64_regs[amd64_rsi];
    cpu->edi = (dword_t) cpu->amd64_regs[amd64_rdi];
    cpu->eip = (dword_t) cpu->amd64_rip;
}

static void amd64_commit_legacy_exec_state(struct cpu_state *cpu, uint16_t reg_write_mask) {
    if (reg_write_mask & (1u << amd64_rax))
        cpu->amd64_regs[amd64_rax] = cpu->eax;
    if (reg_write_mask & (1u << amd64_rcx))
        cpu->amd64_regs[amd64_rcx] = cpu->ecx;
    if (reg_write_mask & (1u << amd64_rdx))
        cpu->amd64_regs[amd64_rdx] = cpu->edx;
    if (reg_write_mask & (1u << amd64_rbx))
        cpu->amd64_regs[amd64_rbx] = cpu->ebx;
    if (reg_write_mask & (1u << amd64_rsp))
        cpu->amd64_regs[amd64_rsp] = cpu->esp;
    if (reg_write_mask & (1u << amd64_rbp))
        cpu->amd64_regs[amd64_rbp] = cpu->ebp;
    if (reg_write_mask & (1u << amd64_rsi))
        cpu->amd64_regs[amd64_rsi] = cpu->esi;
    if (reg_write_mask & (1u << amd64_rdi))
        cpu->amd64_regs[amd64_rdi] = cpu->edi;
    cpu->amd64_rip = cpu->eip;
}

static void amd64_merge_legacy_exec_result(struct cpu_state *dst, const struct cpu_state *src) {
    dst->cycle = src->cycle;
    dst->segfault_addr = src->segfault_addr;
    dst->segfault_was_write = src->segfault_was_write;
    dst->trapno = src->trapno;
    dst->_poked = src->_poked;

    dst->eax = src->eax;
    dst->ecx = src->ecx;
    dst->edx = src->edx;
    dst->ebx = src->ebx;
    dst->esp = src->esp;
    dst->ebp = src->ebp;
    dst->esi = src->esi;
    dst->edi = src->edi;
    dst->eip = src->eip;
}

static inline bool cpu_take_poke(struct cpu_state *cpu) {
    return __atomic_exchange_n(cpu->poked_ptr, false, __ATOMIC_SEQ_CST);
}

static inline bool jit_should_yield(struct jit *jit, struct cpu_state *cpu) {
    if (__atomic_load_n(&jit->write_wanted, __ATOMIC_SEQ_CST))
        return true;
    return cpu_take_poke(cpu);
}

static int cpu_step_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    struct jit *jit = cpu->mmu->jit;

    // Install EXC_BAD_ACCESS on this thread once, for JIT crash recovery.
    // Thread-level exception ports are scoped to only this pthread; no other
    // thread in the process is affected.  Cost is one Mach call per OS thread
    // lifetime (guarded by a thread-local flag), not per jit_enter call.
    static __thread bool exception_handler_installed = false;
    if (!exception_handler_installed) {
        jit_install_thread_exception_handler();
        exception_handler_installed = true;
    }

    // Keep the hot path off malloc/free. With iOS debug malloc enabled
    // (guard pages + scribbling), even these small short-lived allocations
    // are expensive enough to dominate guest startup helpers like /bin/uname.
    struct jit_block *cache[JIT_CACHE_SIZE] = {};
    struct jit_frame frame_storage = {};
    struct jit_frame *frame = &frame_storage;
    frame->cpu = *cpu;
    assert(jit->mmu == cpu->mmu);

    jit_crash_frame = frame;
    jit_crash_cpu = cpu;
    jit_crash_interrupt = INT_GPF;
    jit_crash_addr = frame->cpu.eip;
    if (sigsetjmp(jit_crash_unwind_buf, 1) != 0) {
        if (jit_crash_cpu != NULL && jit_crash_frame != NULL)
            *jit_crash_cpu = jit_crash_frame->cpu;
        cpu->segfault_addr = jit_crash_addr;
        cpu->segfault_was_write = false;
        jit_crash_unwind_active = false;
        jit_crash_frame = NULL;
        jit_crash_cpu = NULL;
        return jit_crash_interrupt;
    }
    jit_crash_unwind_active = true;

    // Use pthread directly (not read_lock) to block in the kernel rather than
    // spinning through atomic_l_lock — eliminates mutex saturation when many
    // goroutines wait for a jetsam write-lock to clear.
    pthread_rwlock_rdlock(&jit->jetsam_lock.l);
    // Register lock for crash recovery: on any EXC_BAD_ACCESS (null gadget
    // dispatch PC=0, or non-zero bad address), hook.c's handler redirects the
    // thread to jit_crash_fn() which reads this pointer and releases the lock.
    jit_crash_lock = &jit->jetsam_lock;

    // Track cleanup_seq locally (NOT in frame, which would corrupt assembly
    // gadget offsets for ret_cache — see frame.h "keep in sync with asm").
    unsigned last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);
    addr_t last_retry_eip = 0;
    guest_addr_t last_retry_addr = 0;
    bool last_retry_write = false;
    unsigned last_retry_count = 0;

    int interrupt = INT_NONE;
    while (interrupt == INT_NONE) {
        // Another task thread can change this address space while we are still
        // running translated blocks. Revalidate the software TLB at block
        // boundaries so stale cached host pointers do not survive mmap/munmap/COW.
        if (tlb->mem_changes != cpu->mmu->changes)
            tlb_refresh(tlb, cpu->mmu);

        // Check write_wanted before any potentially slow operation (block lookup,
        // compilation). This ensures we release the read lock promptly even if we
        // haven't reached jit_enter yet — e.g. while waiting for jit->lock or
        // inside jit_block_compile under debug malloc.
        if (jit_should_yield(jit, cpu)) {
            interrupt = INT_TIMER;
            break;
        }
        addr_t ip = frame->cpu.eip;
        size_t cache_index = jit_cache_hash(ip);
        struct jit_block *block = cache[cache_index];
        if (block == NULL || block->addr != ip) {
            lock(&jit->lock, 0);
            block = jit_lookup(jit, ip);
            if (block == NULL) {
                // Compile outside jetsam_lock: jit_block_compile allocates memory,
                // and under debug malloc (guard pages + scribbling) this is very slow.
                // Holding jetsam_lock during compilation starves jetsam write-lock
                // waiters, and can chain-deadlock when malloc zone locks are contested
                // between goroutines that do and don't yet hold jetsam_lock.
                unlock(&jit->lock);
                jit_crash_lock = NULL;  // Lock released; disable crash recovery until re-acquired
                pthread_rwlock_unlock(&jit->jetsam_lock.l);

                if (jit_should_yield(jit, cpu)) {
                    interrupt = INT_TIMER;
                    goto done_unlocked;
                }

                block = jit_block_compile(ip, tlb);

                if (block == NULL) {
                    // OOM attempt 1: free already-invalidated (jetsam) blocks and retry.
                    // Set write_wanted before write_lock so goroutines executing in
                    // jit_enter see the flag at the next block boundary and exit promptly,
                    // releasing their read locks without waiting for the cycle counter.
                    __atomic_store_n(&jit->write_wanted, 1, __ATOMIC_SEQ_CST);
                    if (jetsam_write_lock_timed(jit)) {
                        lock(&jit->lock, 0);
                        jit_free_jetsam(jit);
                        unlock(&jit->lock);
                        atomic_fetch_add_explicit(&jit->cleanup_seq, 1, memory_order_relaxed);
                        pthread_rwlock_unlock(&jit->jetsam_lock.l);
                        memset(cache, 0, sizeof(cache));
                        memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
                        frame->last_block = NULL;
                    }
                    __atomic_store_n(&jit->write_wanted, 0, __ATOMIC_SEQ_CST);

                    if (jit_should_yield(jit, cpu)) {
                        interrupt = INT_TIMER;
                        goto done_unlocked;
                    }
                    block = jit_block_compile(ip, tlb);

                    if (block == NULL) {
                        // OOM attempt 2: flush the entire JIT cache for this task.
                        printk("JIT OOM at %#x pid %d: flushed entire cache\n", ip, current->pid);
                        __atomic_store_n(&jit->write_wanted, 1, __ATOMIC_SEQ_CST);
                        if (jetsam_write_lock_timed(jit)) {
                            // jit_invalidate_all acquires/releases jit->lock internally
                            jit_invalidate_all(jit);
                            lock(&jit->lock, 0);
                            jit_free_jetsam(jit);
                            unlock(&jit->lock);
                            atomic_fetch_add_explicit(&jit->cleanup_seq, 1, memory_order_relaxed);
                            pthread_rwlock_unlock(&jit->jetsam_lock.l);
                            memset(cache, 0, sizeof(cache));
                            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
                            frame->last_block = NULL;
                        }
                        __atomic_store_n(&jit->write_wanted, 0, __ATOMIC_SEQ_CST);

                        if (jit_should_yield(jit, cpu)) {
                            interrupt = INT_TIMER;
                            goto done_unlocked;
                        }
                        block = jit_block_compile(ip, tlb);
                        if (block == NULL) {
                            // Still OOM even after full flush: kill this guest task
                            printk("JIT OOM at %#x pid %d: even after full flush, killing task\n",
                                   ip, current->pid);
                            jit_crash_unwind_active = false;
                            jit_crash_frame = NULL;
                            jit_crash_cpu = NULL;
                            jit_crash_lock = NULL;
                            return INT_GPF;
                        }
                    }
                }

                // Re-acquire jetsam_lock now that compilation is done. If a jetsam
                // cleanup is already pending, discard the block and yield — it will
                // be recompiled (or found in the hash) on the next call.
                pthread_rwlock_rdlock(&jit->jetsam_lock.l);
                jit_crash_lock = &jit->jetsam_lock;  // Re-enable crash recovery
                if (jit_should_yield(jit, cpu)) {
                    jit_block_free(NULL, block);
                    interrupt = INT_TIMER;
                    break;
                }

                // Insert into hash. Another thread may have compiled the same block
                // while we were outside the locks; if so, use theirs and discard ours.
                lock(&jit->lock, 0);
                struct jit_block *existing = jit_lookup(jit, ip);
                if (existing != NULL) {
                    jit_block_free(NULL, block);
                    block = existing;
                } else {
                    jit_insert(jit, block);
                }
            } else {
                TRACE("%d %08x --- missed cache\n", current_pid(current), ip);
            }
            cache[cache_index] = block;
            unlock(&jit->lock);
        }
        struct jit_block *last_block = frame->last_block;
        // If cleanup_seq changed since last_block was set, jetsam blocks were
        // freed (by cpu_run_to_interrupt or an OOM path). last_block may be a
        // dangling pointer, AND the cache may contain stale pointers to freed
        // blocks whose memory has been reused — clear both.
        if (atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed) != last_block_cleanup_seq) {
            last_block = frame->last_block = NULL;
            memset(cache, 0, sizeof(cache));
            // Also clear the assembly-level return cache: it holds raw pointers
            // into jit_block code arrays. If jetsam freed those blocks, a ret
            // gadget reading a stale entry reads scribbled memory and stores
            // it into frame->last_block, causing a crash on the next iteration.
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);
        }
        if (last_block != NULL &&
                (last_block->jump_ip[0] != NULL ||
                 last_block->jump_ip[1] != NULL)) {
            lock(&jit->lock, 0);
            // can't mint new pointers to a block that has been marked jetsam
            // and is thus assumed to have no pointers left
            if (!last_block->is_jetsam && !block->is_jetsam) {
                for (int i = 0; i <= 1; i++) {
                    if (last_block->jump_ip[i] != NULL &&
                            (*last_block->jump_ip[i] & 0xffffffff) == block->addr) {
                        // Don't link backward jumps (target addr < source block addr).
                        // Backward edges form loop cycles that cause jit_enter to run
                        // indefinitely without returning to C, which prevents the cycle
                        // counter from firing and starves jetsam_lock write waiters.
                        // Unlinked backward jumps exit via jit_ret each iteration,
                        // allowing the cycle counter and poke checks to fire normally.
                        if (block->addr <= last_block->addr)
                            continue;
                        // Use store-release so that block->code[] writes from
                        // compilation are visible to any thread that reads this
                        // linked pointer. The reader side (gret in entry.S) has
                        // a matching dmb ishld load barrier; together they form
                        // a proper acquire-release pair on AArch64. Without
                        // this, a goroutine can see the new code pointer but
                        // still read 0 from code[0], causing br x0 → PC=0x0.
                        __atomic_store_n(last_block->jump_ip[i], (unsigned long) block->code, __ATOMIC_RELEASE);
                        list_add(&block->jumps_from[i], &last_block->jumps_from_links[i]);
                    }
                }
            }

            unlock(&jit->lock);
        }
        
        frame->last_block = block;
        last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);

        // block may be jetsam, but that's ok, because it can't be freed until
        // every thread on this jit is not executing anything

        // Defensive: a block with a null code[0] would crash the goroutine via
        // gret's `br x0` (PC=0x0) and, because the dying thread holds jetsam_lock
        // read, leave write-lock waiters permanently stuck. Detect and discard it
        // here; recompilation on the next iteration will produce a valid block.
        if (__atomic_load_n(&block->code[0], __ATOMIC_RELAXED) == 0) {
            printk("WARNING: JIT block %08x pid %d has null code[0]; invalidating\n",
                   block->addr, current ? current->pid : -1);
            lock(&jit->lock, 0);
            if (!block->is_jetsam) {
                jit_block_disconnect(jit, block);
                block->is_jetsam = true;
                list_add(&jit->jetsam, &block->jetsam);
            }
            cache[cache_index] = NULL;
            frame->last_block = NULL;
            unlock(&jit->lock);
            continue;
        }

        TRACE("%d %08x --- cycle %ld\n", current_pid(current), ip, frame->cpu.cycle);

        bool force_block_boundary_break = current != NULL && current->force_no_jit_cache;
        if (force_block_boundary_break)
            __atomic_store_n(cpu->poked_ptr, true, __ATOMIC_SEQ_CST);
        interrupt = jit_enter(block, frame, tlb);
        // Use load (not exchange) so we don't clear write_wanted — only the
        // write-lock holder should clear it after jetsam cleanup completes.
        if (interrupt == INT_NONE && jit_should_yield(jit, cpu))
            interrupt = INT_TIMER;
        if (interrupt == INT_NONE && ++frame->cpu.cycle % (1 << 10) == 0)
            interrupt = INT_TIMER;
        *cpu = frame->cpu;
        if (current != NULL && current->force_no_jit_cache) {
            frame->last_block = NULL;
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            if (force_block_boundary_break && interrupt == INT_TIMER)
                interrupt = INT_NONE;
        }
        if (interrupt == INT_GPF && current != NULL && current->abi != GUEST_ABI_AMD64) {
            bool retryable = jit_i386_gpf_looks_retryable(cpu);
            if (!retryable)
                goto no_jit_retry;
            bool same_retry = cpu->eip == last_retry_eip &&
                    cpu->segfault_addr == last_retry_addr &&
                    cpu->segfault_was_write == last_retry_write;
            if (!same_retry) {
                last_retry_eip = cpu->eip;
                last_retry_addr = cpu->segfault_addr;
                last_retry_write = cpu->segfault_was_write;
                last_retry_count = 0;
            }
            if (last_retry_count < 1) {
                last_retry_count++;
                lock(&jit->lock, 0);
                if (!block->is_jetsam) {
                    jit_block_disconnect(jit, block);
                    block->is_jetsam = true;
                    list_add(&jit->jetsam, &block->jetsam);
                }
                cache[cache_index] = NULL;
                frame->last_block = NULL;
                memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
                unlock(&jit->lock);
                tlb_flush(tlb);
                interrupt = INT_NONE;
                continue;
            }
        }
no_jit_retry:
    }

    // Release jetsam_lock before freeing: with debug malloc scribbling, free()
    // is O(size) and would hold the lock unnecessarily long.
    jit_crash_lock = NULL;  // Disable crash recovery before unlocking (avoids
                            // double-unlock if EXC_BAD_ACCESS fires during unlock)
    pthread_rwlock_unlock(&jit->jetsam_lock.l);
done_unlocked:
    jit_crash_unwind_active = false;
    jit_crash_frame = NULL;
    jit_crash_cpu = NULL;
    return interrupt;

}

static int cpu_single_step(struct cpu_state *cpu, struct tlb *tlb) {
    struct gen_state state;
    if (!gen_start(cpu->eip, &state))
        return INT_GPF;
    gen_step(&state, tlb);
    gen_exit(&state);
    gen_end(&state);

    struct jit_block *block = state.block;
    struct jit_frame frame = {.cpu = *cpu};
    int interrupt = jit_enter(block, &frame, tlb);
    *cpu = frame.cpu;
    jit_block_free(NULL, block);
    if (interrupt == INT_NONE)
        interrupt = INT_DEBUG;
    return interrupt;
}

static int cpu_single_step_no_debug(struct cpu_state *cpu, struct tlb *tlb) {
    struct gen_state state;
    if (!gen_start(cpu->eip, &state))
        return INT_GPF;
    gen_step(&state, tlb);
    gen_exit(&state);
    gen_end(&state);

    struct jit_block *block = state.block;
    struct jit_frame frame = {.cpu = *cpu};
    int interrupt = jit_enter(block, &frame, tlb);
    *cpu = frame.cpu;
    jit_block_free(NULL, block);
    return interrupt;
}

static int cpu_step_to_interrupt_amd64_frontend(struct cpu_state *cpu, struct tlb *tlb) {
    struct jit_frame frame_storage = {};
    struct jit_frame *frame = &frame_storage;
    struct cpu_state merged_cpu;
    int interrupt;
    bool fallback_to_interp = false;
    addr_t ip;
    struct jit_block *block;

    cpu->poked_ptr = &cpu->_poked;
    tlb_refresh(tlb, cpu->mmu);
    frame->cpu = *cpu;

    if (cpu_take_poke(cpu))
        return INT_TIMER;

    ip = (addr_t) frame->cpu.amd64_rip;
    block = jit_block_compile_amd64(ip, tlb, &fallback_to_interp);
    if (block == NULL) {
        *cpu = frame->cpu;
        if (fallback_to_interp)
            return cpu_run_to_interrupt_amd64(cpu, tlb);
        return INT_GPF;
    }
    if (!block->amd64_compat_legacy_exec) {
        jit_block_free(NULL, block);
        *cpu = frame->cpu;
        return cpu_run_to_interrupt_amd64(cpu, tlb);
    }

    frame->last_block = block;
    amd64_seed_legacy_exec_state(&frame->cpu);
    interrupt = jit_enter(block, frame, tlb);
    merged_cpu = *cpu;
    amd64_merge_legacy_exec_result(&merged_cpu, &frame->cpu);
    amd64_commit_legacy_exec_state(&merged_cpu, block->amd64_low_reg_write_mask);
    if (interrupt == INT_NONE) {
        merged_cpu.amd64_rip = block->end_addr + 1;
        merged_cpu.eip = (dword_t) merged_cpu.amd64_rip;
    }
    *cpu = merged_cpu;
    jit_block_free(NULL, block);

    if (interrupt != INT_NONE)
        return interrupt;
    if (cpu_take_poke(cpu))
        return INT_TIMER;
    return cpu_run_to_interrupt_amd64(cpu, tlb);
}

static int cpu_single_step_amd64_frontend(struct cpu_state *cpu, struct tlb *tlb) {
    // Keep trap-flag semantics on the interpreter until amd64 JIT single-step
    // has an exact one-instruction translation path.
    return cpu_run_to_interrupt_amd64(cpu, tlb);
}

int cpu_run_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    if (current != NULL && current->abi == GUEST_ABI_AMD64) {
        if (!amd64_jit_is_enabled())
            return cpu_run_to_interrupt_amd64(cpu, tlb);
        return cpu->tf ? cpu_single_step_amd64_frontend(cpu, tlb)
                       : cpu_step_to_interrupt_amd64_frontend(cpu, tlb);
    }

    struct jit *jit = cpu->mmu->jit;
    // Keep normal signal/timer pokes per-CPU. The JIT checks write_wanted
    // separately as a jetsam hint; sharing the same flag lets an ordinary poke
    // leave the JIT permanently in "yield now" mode.
    cpu->poked_ptr = &cpu->_poked;

    tlb_refresh(tlb, cpu->mmu);
    int interrupt;
    if (current != NULL && current->force_single_step) {
        interrupt = INT_NONE;
        int steps = 0;
        while (interrupt == INT_NONE) {
            interrupt = cpu_single_step_no_debug(cpu, tlb);
            if (interrupt == INT_NONE && cpu_take_poke(cpu))
                interrupt = INT_TIMER;
            if (interrupt == INT_NONE && ++steps >= 1024) {
                steps = 0;
                interrupt = INT_TIMER;
            }
        }
    } else {
        interrupt = (cpu->tf ? cpu_single_step : cpu_step_to_interrupt)(cpu, tlb); // Crashed here 26 Jul 2022, 27 Aug 2022. -mke
    }
    cpu->trapno = interrupt;

    lock(&jit->lock, 0);
    if (!list_empty(&jit->jetsam)) {
        // write-lock the jetsam_lock to wait until other jit threads get to
        // this point, so they will all clear out their block pointers.
        // Set write_wanted BEFORE write_lock so goroutines still in jit_enter
        // see the flag at the next jit_ret_chain call and exit promptly.
        unlock(&jit->lock);
        __atomic_store_n(&jit->write_wanted, 1, __ATOMIC_SEQ_CST);
        if (jetsam_write_lock_timed(jit)) {
            lock(&jit->lock, 0);
            jit_free_jetsam(jit);
            // Increment cleanup_seq so goroutines that temporarily released
            // jetsam_lock during jit_block_compile detect stale last_block pointers.
            atomic_fetch_add_explicit(&jit->cleanup_seq, 1, memory_order_relaxed);
            // Clear write_wanted before unlock so resumed goroutines don't fire INT_TIMER.
            __atomic_store_n(&jit->write_wanted, 0, __ATOMIC_SEQ_CST);
            pthread_rwlock_unlock(&jit->jetsam_lock.l);
            unlock(&jit->lock);
        } else {
            // Timed out waiting for write lock. A goroutine is stuck holding the
            // read lock (crashed in jit_enter with exception recovery blocked, e.g.
            // LLDB intercepting EXC_BAD_ACCESS). Clear write_wanted so goroutines
            // trying to re-acquire the read lock aren't blocked indefinitely.
            // Jetsam blocks remain until the next successful cleanup pass.
            __atomic_store_n(&jit->write_wanted, 0, __ATOMIC_SEQ_CST);
        }
        return interrupt;
    }
    unlock(&jit->lock);

    return interrupt;
}

void cpu_poke(struct cpu_state *cpu) {
    __atomic_store_n(cpu->poked_ptr, true, __ATOMIC_SEQ_CST);
}
