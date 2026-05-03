#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu/cpuid.h"
#include "emu/cpu.h"
#include "emu/fpu.h"
#include "emu/memory.h"
#include "emu/tlb.h"
#include "emu/vec.h"
#include "emu/interrupt.h"
#include "emu/modrm.h"
#include "kernel/task.h"
#include "kernel/elf.h"
#include "util/sync.h"

struct amd64_rex_prefix {
    bool present;
    bool w;
    bool r;
    bool x;
    bool b;
};

struct amd64_modrm {
    bool is_reg;
    bool rex_present;
    uint8_t reg;
    uint8_t rm;
    bool has_base;
    uint8_t base;
    bool has_index;
    uint8_t index;
    uint8_t scale;
    bool rip_relative;
    int32_t disp;
};

static struct tlb *volatile amd64_jit_bridge_tlb;

static inline bool amd64_ignored_segment_prefix(byte_t byte) {
    return byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e;
}

struct fpu_env32 {
    uint32_t control;
    uint32_t status;
    uint32_t tag;
    uint32_t ip;
    uint32_t ip_selector;
    uint32_t operand;
    uint32_t operand_selector;
};

struct fpu_state32 {
    struct fpu_env32 env;
    uint8_t regs[8][10];
};

struct amd64_fxsave_fpxreg {
    word_t significand[4];
    word_t exponent;
    word_t padding[3];
};

struct amd64_fxsave_xmmreg {
    dword_t element[4];
};

struct amd64_fxsave_area {
    word_t fcw;
    word_t fsw;
    byte_t ftw;
    byte_t reserved0;
    word_t fop;
    qword_t rip;
    qword_t rdp;
    dword_t mxcsr;
    dword_t mxcsr_mask;
    struct amd64_fxsave_fpxreg st[8];
    struct amd64_fxsave_xmmreg xmm[16];
    byte_t reserved1[96];
};

static_assert(sizeof(struct amd64_fxsave_area) == 512, "amd64 fxsave area size");

#define AMD64_BUSYBOX_INIT_SLOT 0x5661a6d8ull
#define AMD64_BUSYBOX_INIT_SLOT_SIZE 8
#define AMD64_BUSYBOX_INIT_LOAD_RIP 0x565e39fbull
#define AMD64_BUSYBOX_INIT_TEST_RIP 0x565e3a02ull
#define AMD64_BUSYBOX_INIT_JNE_RIP 0x565e3a05ull
#define AMD64_BUSYBOX_INIT_CMP_RIP 0x565e3a0bull
#define AMD64_BUSYBOX_INIT_CORRUPT_WRITE_RIP 0xfff929caull
#define AMD64_HTOP_RBX_LOAD_RIP 0x5656370eull
#define AMD64_HTOP_R13_CORRUPT_WRITE_RIP 0x5656375full
#define AMD64_HTOP_TRACE_WINDOW_START 0x56563700ull
#define AMD64_HTOP_TRACE_WINDOW_END 0x56563780ull
#define AMD64_HTOP_RBX_FIELD_OFFSET 0x170ull
#define AMD64_HTOP_RBX_FIELD_SIZE 8
#define AMD64_HTOP_RBX_FIELD_ABS_ADDR 0xf7f019e0ull
#define AMD64_HTOP_FIELD_FILL_RIP 0x56569e88ull
#define AMD64_HTOP_R13_CORRUPT_BLOCK_BASE 0x56587de0ull
#define AMD64_HTOP_R13_CORRUPT_BLOCK_SIZE 32
#define AMD64_CARGO_R12_FAULT_RIP 0xf7f92174ull
#define AMD64_CARGO_ENTRY_RIP 0xf7f920d0ull
#define AMD64_CARGO_START_CALL_RIP 0xf7fbfd85ull
#define AMD64_CARGO_PFWIN_WINDOW_START 0xf7f920e0ull
#define AMD64_CARGO_PFWIN_WINDOW_END 0xf7f92190ull
#define AMD64_CARGO_R12_TRACE_WINDOW_START 0xf7f92000ull
#define AMD64_CARGO_R12_TRACE_WINDOW_END 0xf7f92220ull
#define AMD64_BUSYBOX_INIT_WATCH_COUNT 32
#define AMD64_BUSYBOX_INIT_WATCH_SPAN 16

static qword_t amd64_busybox_init_watch[AMD64_BUSYBOX_INIT_WATCH_COUNT];
static unsigned amd64_busybox_init_watch_next;
static qword_t amd64_htop_watch_field_addr = AMD64_HTOP_RBX_FIELD_ABS_ADDR;
static const bool amd64_htop_legacy_trace_enabled = false;
static const bool amd64_cargo_trace_enabled = false;
static unsigned amd64_cargo_r12_trace_count;
static unsigned amd64_cargo_rdx_trace_count;
static unsigned amd64_cargo_rdi_trace_count;
static unsigned amd64_cargo_xfer_trace_count;
static unsigned amd64_cargo_start_call_trace_count;
static unsigned amd64_cc1_slot_probe_count;
static unsigned amd64_cc1_cmp_probe_count;
static unsigned amd64_cc1_je_probe_count;
static unsigned amd64_cc1_va_list_branch_probe_count;
static unsigned amd64_cc1_slot_write_probe_count;
static unsigned amd64_cc1_va_list_init_probe_count;
static unsigned amd64_cc1_xfer_probe_count;
static unsigned amd64_bash_cond_probe_count;

#define AMD64_CC1_NULL_SLOT_ADDR 0x2e416f0ull
#define AMD64_CC1_CMP_GLOBAL_ADDR 0x2e403a0ull
#define AMD64_CC1_SYSV_SLOT_ADDR 0x2e416f8ull
#define AMD64_CC1_ABI_FLAG_ADDR 0x2e6cbf0ull
#define AMD64_CC1_MS_VARIANT_ADDR 0x2e6cbccull
#define AMD64_CC1_VA_LIST_HOOK_RIP 0x11b1610ull
#define AMD64_CC1_VA_LIST_HOOK_JNE_RIP 0x11b1617ull
#define AMD64_CC1_VA_LIST_HOOK_FALLBACK_JMP_RIP 0x11b1620ull
#define AMD64_CC1_VA_LIST_COMPLEX_RIP 0x11b1628ull
#define AMD64_CC1_VA_LIST_GETTER_RIP 0x11b10d0ull
#define AMD64_CC1_VA_LIST_INIT_ENTRY_RIP 0x11b1704ull
#define AMD64_CC1_VA_LIST_INIT_SYSV_STORE_RIP 0x11b17dbull
#define AMD64_CC1_VA_LIST_INIT_ATTR_CALL_RIP 0x11b1817ull
#define AMD64_CC1_VA_LIST_INIT_ATTR_RET_RIP 0x11b181cull
#define AMD64_CC1_VA_LIST_INIT_MS_STORE_RIP 0x11b1823ull

#define AMD64_BASH_COND_UNEXP_RIP 0x20f50ull
#define AMD64_BASH_COND_BINOP_RIP 0x23967ull
#define AMD64_BASH_SYNTAXTAB_ADDR 0xbf540ull
#define AMD64_BASH_TOKEN_A_ADDR 0xc6bccull
#define AMD64_BASH_TOKEN_B_ADDR 0xc6bd0ull
#define AMD64_BASH_LINE_NUMBER_ADDR 0xc6ab4ull
#define AMD64_BASH_PARSER_STATE_ADDR 0xc6b2cull
#define AMD64_BASH_EXTENDED_GLOB_ADDR 0xcea9cull

static inline bool amd64_guest_addr_ok(qword_t guest_addr, unsigned size, guest_addr_t *addr_out);
static inline bool amd64_mem_read(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, void *out, unsigned size);

static bool amd64_trace_undefined_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_AMD64_UNDEFINED") != NULL ? 1 : 0;
    return enabled;
}

#define AMD64_CC1_TRACE_COUNT 64
struct amd64_cc1_trace {
    qword_t rip;
    qword_t rax;
    qword_t rbx;
    qword_t rcx;
    qword_t rdx;
    qword_t rsi;
    qword_t rdi;
    qword_t rsp;
    qword_t rbp;
    qword_t r8;
    qword_t r9;
    qword_t r12;
    uint8_t bytes[8];
    uint8_t byte_count;
};

static struct amd64_cc1_trace amd64_cc1_trace[AMD64_CC1_TRACE_COUNT];
static unsigned amd64_cc1_trace_next;
static pid_t_ amd64_cc1_trace_pid;

#define AMD64_AS_TRACE_COUNT 16384
struct amd64_as_trace {
    qword_t rip;
    qword_t rax;
    qword_t rbx;
    qword_t rcx;
    qword_t rdx;
    qword_t rsi;
    qword_t rdi;
    qword_t rsp;
    qword_t rbp;
    qword_t r8;
    qword_t r9;
    qword_t r12;
    uint8_t bytes[8];
    uint8_t byte_count;
};

static struct amd64_as_trace amd64_as_trace[AMD64_AS_TRACE_COUNT];
static unsigned amd64_as_trace_next;
static pid_t_ amd64_as_trace_pid;

#define AMD64_AS_EVENT_COUNT 128
#define AMD64_AS_RESET_DONE_RIP 0x7ffffdf67be1ull
#define AMD64_AS_STATE31_WRITE1_DONE_RIP 0x7ffffdf6bbbfull
#define AMD64_AS_STATE31_WRITE2_DONE_RIP 0x7ffffdf76c74ull
#define AMD64_AS_STATE31_CHECK_DONE_RIP 0x7ffffdf6f25bull
#define AMD64_AS_STATE31_ADDR 0x7ffffdfff8b1ull

enum amd64_as_event_kind {
    amd64_as_event_reset_done = 1,
    amd64_as_event_state31_write,
    amd64_as_event_state31_check,
};

struct amd64_as_event {
    qword_t rip;
    qword_t rax;
    qword_t rbx;
    qword_t rcx;
    qword_t rdx;
    qword_t rsi;
    qword_t rdi;
    qword_t r12;
    uint8_t state31;
    uint8_t kind;
};

static struct amd64_as_event amd64_as_events[AMD64_AS_EVENT_COUNT];
static unsigned amd64_as_event_next;
static pid_t_ amd64_as_event_pid;

enum amd64_as_suspect_kind {
    amd64_as_suspect_bt = 1,
    amd64_as_suspect_stack,
};

enum amd64_as_stack_op {
    amd64_as_stack_push = 1,
    amd64_as_stack_pop,
    amd64_as_stack_leave,
};

#define AMD64_AS_SUSPECT_COUNT 256
struct amd64_as_suspect {
    qword_t rip;
    qword_t lhs;
    qword_t value;
    qword_t addr;
    qword_t bit_index;
    qword_t bit;
    qword_t old_rsp;
    qword_t new_rsp;
    uint8_t kind;
    uint8_t op;
    uint8_t size;
    uint8_t aux;
};

static struct amd64_as_suspect amd64_as_suspects[AMD64_AS_SUSPECT_COUNT];
static unsigned amd64_as_suspect_next;
static pid_t_ amd64_as_suspect_pid;

#define AMD64_AS_FOCUS_COUNT 128
struct amd64_as_focus {
    qword_t rip;
    qword_t rax;
    qword_t rbx;
    qword_t rcx;
    qword_t rdx;
    qword_t rsi;
    qword_t rdi;
    qword_t rsp;
    qword_t rbp;
    qword_t r12;
};

static struct amd64_as_focus amd64_as_focus[AMD64_AS_FOCUS_COUNT];
static unsigned amd64_as_focus_next;
static pid_t_ amd64_as_focus_pid;
static pid_t_ amd64_as_template_probe_pid;
static qword_t amd64_as_template_probe_entry;
static qword_t amd64_as_template_probe_cmp;

enum amd64_as_state_region_kind {
    amd64_as_state_region_block = 1,
    amd64_as_state_region_desc,
};

#define AMD64_AS_STATE_WRITE_COUNT 256
struct amd64_as_state_write {
    qword_t rip;
    qword_t addr;
    qword_t value;
    qword_t region_base;
    qword_t snapshot_addr;
    uint8_t size;
    uint8_t region_kind;
    uint8_t region_index;
    uint8_t byte_count;
    uint8_t bytes[16];
};

static struct amd64_as_state_write amd64_as_state_writes[AMD64_AS_STATE_WRITE_COUNT];
static unsigned amd64_as_state_write_next;
static pid_t_ amd64_as_state_write_pid;

#define AMD64_AS_FOCUS_DISPATCH_RIP 0x7ffffdee87f8ull
#define AMD64_AS_FOCUS_CASE0_RIP 0x7ffffdee88ffull
#define AMD64_AS_FOCUS_SOURCE_BRANCH_RIP 0x7ffffdea6758ull
#define AMD64_AS_FOCUS_SOURCE_SET_RIP 0x7ffffdea6956ull
#define AMD64_AS_FOCUS_SOURCE_CHECK_RIP 0x7ffffdea6abeull
#define AMD64_AS_FOCUS_BUILD_RIP 0x7ffffdea6b57ull
#define AMD64_AS_FOCUS_TEMPLATE_MOFFS_RIP 0x7ffffdee9a9eull
#define AMD64_AS_FOCUS_TEMPLATE_MOFFS_CMP_RIP 0x7ffffdee9aa3ull
#define AMD64_AS_ERROR_PRINTF_ENTRY_RIP 0x7ffffdea6600ull
#define AMD64_AS_ERROR_WRAPPER_RIP 0x7ffffdee4e2dull
#define AMD64_AS_ERROR_REPORT_RIP 0x7ffffdee89d4ull
#define AMD64_AS_ERROR_PRE_COUNT 192u
#define AMD64_AS_ERROR_POST_COUNT 16u

static inline bool amd64_trace_read_guest(qword_t addr, void *out, size_t size);
static inline bool amd64_as_is_template_moffs_probe(struct cpu_state *cpu);
static inline bool amd64_trace_try_read_lock(wrlock_t *lock);
static bool amd64_trace_read_task_guest_cstring(const struct task *task, qword_t addr,
        char *buf, size_t size);
static bool amd64_resolve_task_image_base(const struct task *task, qword_t *base);

static inline bool amd64_cc1_trace_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_CC1") != NULL ? 1 : 0;
    return enabled == 1 &&
        current != NULL &&
        current->abi == GUEST_ABI_AMD64 &&
        strcmp(current->comm, "cc1") == 0;
}

static inline bool amd64_as_trace_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_AS") != NULL ? 1 : 0;
    return enabled == 1 &&
        current != NULL &&
        current->abi == GUEST_ABI_AMD64 &&
        strcmp(current->comm, "as") == 0;
}

static inline bool amd64_as_stderr_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_AS_STDERR") != NULL ? 1 : 0;
    return enabled == 1;
}

static inline bool amd64_as_alu_stderr_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_AS_ALU") != NULL ? 1 : 0;
    return enabled == 1;
}

static inline bool amd64_suspect_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_AMD64_SUSPECT") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (amd64_as_trace_enabled())
        return true;
    return current != NULL && current->abi == GUEST_ABI_AMD64;
}

static inline bool amd64_bash_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_AMD64_BASH") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    return current != NULL && current->abi == GUEST_ABI_AMD64 && strcmp(current->comm, "bash") == 0;
}

static inline bool amd64_as_is_error_path_rip(qword_t rip) {
    qword_t image_base = 0;
    if (current != NULL && current->abi == GUEST_ABI_AMD64 &&
            strcmp(current->comm, "as") == 0 &&
            amd64_resolve_task_image_base(current, &image_base)) {
        qword_t off = rip - image_base;
        if (off == 0x49a9e || off == 0x49aa3)
            return true;
    }
    switch (rip) {
    case AMD64_AS_ERROR_REPORT_RIP:
    case AMD64_AS_ERROR_WRAPPER_RIP:
    case AMD64_AS_ERROR_PRINTF_ENTRY_RIP:
    case AMD64_AS_FOCUS_DISPATCH_RIP:
    case AMD64_AS_FOCUS_CASE0_RIP:
    case AMD64_AS_FOCUS_SOURCE_BRANCH_RIP:
    case AMD64_AS_FOCUS_SOURCE_SET_RIP:
    case AMD64_AS_FOCUS_SOURCE_CHECK_RIP:
    case AMD64_AS_FOCUS_BUILD_RIP:
    case AMD64_AS_FOCUS_TEMPLATE_MOFFS_RIP:
    case AMD64_AS_FOCUS_TEMPLATE_MOFFS_CMP_RIP:
        return true;
    default:
        return false;
    }
}

static inline bool amd64_as_is_template_moffs_probe(struct cpu_state *cpu) {
    uint8_t bytes[10] = {};
    static const uint8_t entry[] = {0x89, 0xd6, 0x83, 0xce, 0x01};
    static const uint8_t cmp[] = {0x66, 0x81, 0xfe, 0xa1, 0x00};
    if (cpu == NULL || current == NULL || current->abi != GUEST_ABI_AMD64 ||
            strcmp(current->comm, "as") != 0)
        return false;
    if (amd64_as_template_probe_pid == current->pid &&
            (cpu->amd64_current_insn_rip == amd64_as_template_probe_entry ||
             cpu->amd64_current_insn_rip == amd64_as_template_probe_cmp))
        return true;
    if (!amd64_trace_read_guest(cpu->amd64_current_insn_rip, bytes, sizeof(bytes)))
        return false;
    return memcmp(bytes, entry, sizeof(entry)) == 0 ||
        memcmp(bytes, cmp, sizeof(cmp)) == 0;
}

static void amd64_as_scan_template_probe(struct cpu_state *cpu) {
    static const uint8_t pattern[] = {
        0x89, 0xd6, 0x83, 0xce, 0x01, 0x66, 0x81, 0xfe, 0xa1, 0x00,
    };

    if (!amd64_as_trace_enabled() || current == NULL || current->mem == NULL ||
            amd64_as_template_probe_pid == current->pid)
        return;

    amd64_as_template_probe_pid = current->pid;
    amd64_as_template_probe_entry = 0;
    amd64_as_template_probe_cmp = 0;

    if (!amd64_trace_try_read_lock(&current->mem->lock))
        return;
    for (page_t page = 0; page < current->mem->page_limit; mem_next_page(current->mem, &page)) {
        struct pt_entry *pt = mem_pt(current->mem, page);
        if (pt == NULL || pt->data == NULL || pt->data->data == NULL)
            continue;
        uint8_t *base = (uint8_t *) pt->data->data + pt->offset;
        for (size_t off = 0; off + sizeof(pattern) <= PAGE_SIZE; off++) {
            if (memcmp(base + off, pattern, sizeof(pattern)) != 0)
                continue;
            amd64_as_template_probe_entry = ((qword_t) page << PAGE_BITS) + off;
            amd64_as_template_probe_cmp = amd64_as_template_probe_entry + 5;
            break;
        }
        if (amd64_as_template_probe_entry != 0)
            break;
    }
    read_unlock(&current->mem->lock);

    if (amd64_as_template_probe_entry != 0 && amd64_as_stderr_enabled())
        fprintf(stderr, "amd64 as template probe: entry=%#llx cmp=%#llx current_rip=%#llx\n",
                (unsigned long long) amd64_as_template_probe_entry,
                (unsigned long long) amd64_as_template_probe_cmp,
                (unsigned long long) cpu->amd64_current_insn_rip);
}

static inline bool amd64_trace_copy_guest_locked(guest_addr_t guest_addr, void *out, size_t size) {
    uint8_t *dst = out;
    size_t copied = 0;

    while (copied < size) {
        struct pt_entry *pt = mem_pt(current->mem, PAGE(guest_addr + copied));
        if (pt == NULL || pt->data == NULL || pt->data->data == NULL)
            return false;
        size_t page_off = PGOFFSET(guest_addr + copied);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > size - copied)
            chunk = size - copied;
        memcpy(dst + copied, (uint8_t *) pt->data->data + pt->offset + page_off, chunk);
        copied += chunk;
    }

    return true;
}

static inline bool amd64_trace_try_read_lock(wrlock_t *lock) {
    return trylockr(lock) == 0;
}

static bool amd64_trace_read_current_guest(qword_t guest_addr, void *out, size_t size) {
    guest_addr_t addr;
    if (current == NULL || current->mem == NULL || out == NULL)
        return false;
    if (!amd64_guest_addr_ok(guest_addr, size, &addr))
        return false;

    bool ok;
    if (!amd64_trace_try_read_lock(&current->mem->lock))
        return false;
    ok = amd64_trace_copy_guest_locked(addr, out, size);
    read_unlock(&current->mem->lock);
    return ok;
}

static void amd64_trace_as_event(struct cpu_state *cpu, enum amd64_as_event_kind kind) {
    if (!amd64_as_trace_enabled())
        return;

    if (amd64_as_event_pid != current->pid) {
        memset(amd64_as_events, 0, sizeof(amd64_as_events));
        amd64_as_event_next = 0;
        amd64_as_event_pid = current->pid;
    }

    struct amd64_as_event *event = &amd64_as_events[amd64_as_event_next++ % AMD64_AS_EVENT_COUNT];
    memset(event, 0, sizeof(*event));
    event->kind = kind;
    event->rip = cpu->amd64_current_insn_rip;
    event->rax = cpu->amd64_regs[amd64_rax];
    event->rbx = cpu->amd64_regs[amd64_rbx];
    event->rcx = cpu->amd64_regs[amd64_rcx];
    event->rdx = cpu->amd64_regs[amd64_rdx];
    event->rsi = cpu->amd64_regs[amd64_rsi];
    event->rdi = cpu->amd64_regs[amd64_rdi];
    event->r12 = cpu->amd64_regs[amd64_r12];
    amd64_trace_read_current_guest(AMD64_AS_STATE31_ADDR, &event->state31, sizeof(event->state31));
}

static struct amd64_as_suspect *amd64_trace_as_suspect_reserve(void) {
    if (!amd64_suspect_trace_enabled())
        return NULL;

    if (amd64_as_suspect_pid != current->pid) {
        memset(amd64_as_suspects, 0, sizeof(amd64_as_suspects));
        amd64_as_suspect_next = 0;
        amd64_as_suspect_pid = current->pid;
    }

    struct amd64_as_suspect *suspect =
        &amd64_as_suspects[amd64_as_suspect_next++ % AMD64_AS_SUSPECT_COUNT];
    memset(suspect, 0, sizeof(*suspect));
    suspect->rip = current->cpu.amd64_current_insn_rip;
    return suspect;
}

static const char *amd64_as_stack_op_name(unsigned op) {
    switch (op) {
    case amd64_as_stack_push:
        return "push";
    case amd64_as_stack_pop:
        return "pop";
    case amd64_as_stack_leave:
        return "leave";
    default:
        return "unknown";
    }
}

static void amd64_dump_recent_suspects(pid_t_ pid, const char *tag) {
    if (amd64_as_suspect_pid != pid || amd64_as_suspect_next == 0)
        return;

    unsigned total = amd64_as_suspect_next;
    unsigned count = total < AMD64_AS_SUSPECT_COUNT ? total : AMD64_AS_SUSPECT_COUNT;
    unsigned start = total >= AMD64_AS_SUSPECT_COUNT ? total - AMD64_AS_SUSPECT_COUNT : 0;
    printk("[amd64-jit] %s recent-suspects pid=%d count=%u\n", tag, pid, count);
    for (unsigned i = 0; i < count; i++) {
        const struct amd64_as_suspect *suspect =
            &amd64_as_suspects[(start + i) % AMD64_AS_SUSPECT_COUNT];
        switch (suspect->kind) {
        case amd64_as_suspect_bt:
            printk("[amd64-jit]   suspect[%02u] kind=bt rip=%#llx op=%#x size=%u mem=%u index=%#llx bit=%#llx addr=%#llx lhs=%#llx value=%#llx\n",
                   i,
                   (unsigned long long) suspect->rip,
                   suspect->op,
                   suspect->size,
                   suspect->aux,
                   (unsigned long long) suspect->bit_index,
                   (unsigned long long) suspect->bit,
                   (unsigned long long) suspect->addr,
                   (unsigned long long) suspect->lhs,
                   (unsigned long long) suspect->value);
            break;
        case amd64_as_suspect_stack:
            printk("[amd64-jit]   suspect[%02u] kind=stack op=%s size=%u rip=%#llx old-rsp=%#llx new-rsp=%#llx value=%#llx\n",
                   i,
                   amd64_as_stack_op_name(suspect->op),
                   suspect->size,
                   (unsigned long long) suspect->rip,
                   (unsigned long long) suspect->old_rsp,
                   (unsigned long long) suspect->new_rsp,
                   (unsigned long long) suspect->value);
            break;
        default:
            break;
        }
    }
}

static void amd64_dump_recent_suspects_for_stack_slot(pid_t_ pid, qword_t slot_addr,
        const char *tag) {
    if (amd64_as_suspect_pid != pid || amd64_as_suspect_next == 0)
        return;

    unsigned total = amd64_as_suspect_next;
    unsigned count = total < AMD64_AS_SUSPECT_COUNT ? total : AMD64_AS_SUSPECT_COUNT;
    unsigned start = total >= AMD64_AS_SUSPECT_COUNT ? total - AMD64_AS_SUSPECT_COUNT : 0;
    unsigned matches = 0;

    printk("[amd64-jit] %s slot-suspects pid=%d slot=%#llx count=%u\n",
           tag, pid, (unsigned long long) slot_addr, count);
    for (unsigned i = 0; i < count; i++) {
        const struct amd64_as_suspect *suspect =
            &amd64_as_suspects[(start + i) % AMD64_AS_SUSPECT_COUNT];
        if (suspect->kind != amd64_as_suspect_stack)
            continue;
        if (suspect->old_rsp != slot_addr && suspect->new_rsp != slot_addr)
            continue;
        matches++;
        printk("[amd64-jit]   slot-suspect[%02u] op=%s size=%u rip=%#llx old-rsp=%#llx new-rsp=%#llx value=%#llx\n",
               i,
               amd64_as_stack_op_name(suspect->op),
               suspect->size,
               (unsigned long long) suspect->rip,
               (unsigned long long) suspect->old_rsp,
               (unsigned long long) suspect->new_rsp,
               (unsigned long long) suspect->value);
    }
    printk("[amd64-jit] %s slot-suspects-matches=%u\n", tag, matches);
}

static void amd64_trace_as_bt(struct cpu_state *cpu, uint8_t op, unsigned size,
        bool is_mem, qword_t bit_index, qword_t bit, qword_t addr,
        qword_t lhs, qword_t value) {
    struct amd64_as_suspect *suspect = amd64_trace_as_suspect_reserve();
    if (suspect == NULL)
        return;
    suspect->kind = amd64_as_suspect_bt;
    suspect->op = op;
    suspect->size = size;
    suspect->aux = is_mem ? 1 : 0;
    suspect->lhs = lhs;
    suspect->value = value;
    suspect->addr = addr;
    suspect->bit_index = bit_index;
    suspect->bit = bit;
    if (amd64_as_alu_stderr_enabled()) {
        uint8_t insn_bytes[8] = {};
        bool have_insn_bytes = amd64_trace_read_guest(cpu->amd64_current_insn_rip,
                insn_bytes, sizeof(insn_bytes));
        fprintf(stderr,
                "amd64 as bt: rip=%#llx op=%#x size=%u mem=%u index=%#llx bit=%#llx addr=%#llx lhs=%#llx value=%#llx cf=%u%s\n",
                (unsigned long long) cpu->amd64_current_insn_rip,
                op,
                size,
                is_mem ? 1u : 0u,
                (unsigned long long) bit_index,
                (unsigned long long) bit,
                (unsigned long long) addr,
                (unsigned long long) lhs,
                (unsigned long long) value,
                cpu->cf,
                have_insn_bytes ? "" : " bytes=?");
        if (have_insn_bytes) {
            fprintf(stderr,
                    "amd64 as bt: bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
                    insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
        }
    }
}

static void amd64_trace_as_stack(unsigned op, unsigned size,
        qword_t old_rsp, qword_t new_rsp, qword_t value) {
    struct amd64_as_suspect *suspect = amd64_trace_as_suspect_reserve();
    if (suspect == NULL)
        return;
    suspect->kind = amd64_as_suspect_stack;
    suspect->op = op;
    suspect->size = size;
    suspect->old_rsp = old_rsp;
    suspect->new_rsp = new_rsp;
    suspect->value = value;
}

static void amd64_dump_stack_window(struct cpu_state *cpu, struct tlb *tlb,
        qword_t center_rsp, unsigned before, unsigned after, const char *tag) {
    qword_t start = center_rsp - (qword_t) before * 8;
    qword_t end = center_rsp + (qword_t) after * 8;
    printk("[amd64-jit] %s stack-window center=%#llx range=%#llx..%#llx\n",
           tag,
           (unsigned long long) center_rsp,
           (unsigned long long) start,
           (unsigned long long) end);
    for (unsigned i = 0; i <= before + after; i++) {
        qword_t addr = start + (qword_t) i * 8;
        qword_t value = 0;
        if (amd64_mem_read(cpu, tlb, addr, &value, sizeof(value))) {
            printk("[amd64-jit]   stack[%+lld] addr=%#llx value=%#llx%s\n",
                   (long long) i - (long long) before,
                   (unsigned long long) addr,
                   (unsigned long long) value,
                   addr == center_rsp ? " <== popped target slot" : "");
        } else {
            printk("[amd64-jit]   stack[%+lld] addr=%#llx unreadable%s\n",
                   (long long) i - (long long) before,
                   (unsigned long long) addr,
                   addr == center_rsp ? " <== popped target slot" : "");
        }
    }
}

static bool amd64_mem_read_direct(qword_t guest_addr, void *out, unsigned size) {
    guest_addr_t addr;
    unsigned copied = 0;
    if (current == NULL || current->mem == NULL)
        return false;
    if (!amd64_guest_addr_ok(guest_addr, size, &addr))
        return false;
    while (copied < size) {
        guest_addr_t chunk_addr = addr + copied;
        void *ptr = mem_ptr(current->mem, chunk_addr, MEM_READ);
        unsigned chunk = PAGE_SIZE - PGOFFSET(chunk_addr);
        if (ptr == NULL)
            return false;
        if (chunk > size - copied)
            chunk = size - copied;
        memcpy((char *) out + copied, ptr, chunk);
        copied += chunk;
    }
    return true;
}

static void amd64_dump_tlb_slot(struct tlb *tlb, qword_t guest_addr, unsigned size,
        const char *tag) {
    guest_addr_t addr;
    struct tlb_entry entry;
    uint8_t cached_bytes[16] = {};
    bool have_cached_bytes = false;
    if (size > sizeof(cached_bytes))
        size = sizeof(cached_bytes);
    if (tlb == NULL || !amd64_guest_addr_ok(guest_addr, size, &addr)) {
        printk("[amd64-jit] %s tlb-slot addr=%#llx unavailable\n",
               tag,
               (unsigned long long) guest_addr);
        return;
    }
    entry = tlb->entries[TLB_INDEX(addr)];
    if (entry.page == TLB_PAGE(addr)) {
        void *ptr = (void *) (entry.data_minus_addr + addr);
        if (ptr != NULL) {
            memcpy(cached_bytes, ptr, size);
            have_cached_bytes = true;
        }
    }
    printk("[amd64-jit] %s tlb-slot addr=%#llx index=%u page=%#llx want=%#llx writable=%#llx delta=%#llx changes=%llu/%llu bytes=%s%02x %02x %02x %02x %02x %02x %02x %02x\n",
           tag,
           (unsigned long long) guest_addr,
           TLB_INDEX(addr),
           (unsigned long long) entry.page,
           (unsigned long long) TLB_PAGE(addr),
           (unsigned long long) entry.page_if_writable,
           (unsigned long long) entry.data_minus_addr,
           (unsigned long long) tlb->mem_changes,
           (unsigned long long) (tlb->mmu != NULL ? tlb->mmu->changes : 0),
           have_cached_bytes ? "" : "unreadable ",
           cached_bytes[0], cached_bytes[1], cached_bytes[2], cached_bytes[3],
           cached_bytes[4], cached_bytes[5], cached_bytes[6], cached_bytes[7]);
}

static void amd64_dump_guest_bytes(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, const char *tag) {
    uint8_t bytes[16] = {};
    if (size > sizeof(bytes))
        size = sizeof(bytes);
    if (!amd64_mem_read(cpu, tlb, guest_addr, bytes, size)) {
        printk("[amd64-jit] %s addr=%#llx unreadable size=%u\n",
               tag,
               (unsigned long long) guest_addr,
               size);
        return;
    }
    printk("[amd64-jit] %s addr=%#llx bytes=%02x %02x %02x %02x %02x %02x %02x %02x%s\n",
           tag,
           (unsigned long long) guest_addr,
           bytes[0], bytes[1], bytes[2], bytes[3],
           bytes[4], bytes[5], bytes[6], bytes[7],
           size > 8 ? " ..." : "");
}

static void amd64_trace_as_focus(struct cpu_state *cpu) {
    if (!amd64_as_trace_enabled())
        return;

    if (amd64_as_is_template_moffs_probe(cpu))
        goto record_focus;
    return;

#if 0
    qword_t image_base = 0;
    bool have_image_base = current != NULL &&
        amd64_resolve_task_image_base(current, &image_base);
    if (have_image_base) {
        qword_t off = cpu->amd64_current_insn_rip - image_base;
        if (off == 0x49a9e || off == 0x49aa3)
            goto record_focus;
    }

    switch (cpu->amd64_current_insn_rip) {
    case AMD64_AS_FOCUS_DISPATCH_RIP:
    case AMD64_AS_FOCUS_CASE0_RIP:
    case AMD64_AS_FOCUS_SOURCE_BRANCH_RIP:
    case AMD64_AS_FOCUS_SOURCE_SET_RIP:
    case AMD64_AS_FOCUS_SOURCE_CHECK_RIP:
    case AMD64_AS_FOCUS_BUILD_RIP:
    case AMD64_AS_FOCUS_TEMPLATE_MOFFS_RIP:
    case AMD64_AS_FOCUS_TEMPLATE_MOFFS_CMP_RIP:
        break;
    default:
        return;
    }

record_focus:
#endif
record_focus:
    if (amd64_as_focus_pid != current->pid) {
        memset(amd64_as_focus, 0, sizeof(amd64_as_focus));
        amd64_as_focus_next = 0;
        amd64_as_focus_pid = current->pid;
    }

    struct amd64_as_focus *focus = &amd64_as_focus[amd64_as_focus_next++ % AMD64_AS_FOCUS_COUNT];
    memset(focus, 0, sizeof(*focus));
    focus->rip = cpu->amd64_current_insn_rip;
    focus->rax = cpu->amd64_regs[amd64_rax];
    focus->rbx = cpu->amd64_regs[amd64_rbx];
    focus->rcx = cpu->amd64_regs[amd64_rcx];
    focus->rdx = cpu->amd64_regs[amd64_rdx];
    focus->rsi = cpu->amd64_regs[amd64_rsi];
    focus->rdi = cpu->amd64_regs[amd64_rdi];
    focus->rsp = cpu->amd64_regs[amd64_rsp];
    focus->rbp = cpu->amd64_regs[amd64_rbp];
    focus->r12 = cpu->amd64_regs[amd64_r12];
    if (amd64_as_stderr_enabled()) {
        static pid_t_ amd64_as_focus_image_base_pid;
        static qword_t amd64_as_focus_image_base;
        qword_t dword_at_rdx = 0;
        qword_t image_base = 0;
        uint8_t insn_bytes[8] = {};
        char rbx_text[64];
        char rsi_text[64];
        char rdi_text[64];
        if (amd64_as_focus_image_base_pid != current->pid) {
            amd64_as_focus_image_base = 0;
            if (!amd64_resolve_task_image_base(current, &amd64_as_focus_image_base))
                amd64_as_focus_image_base = 0;
            amd64_as_focus_image_base_pid = current->pid;
        }
        image_base = amd64_as_focus_image_base;
        bool have_dword_at_rdx = amd64_trace_read_guest(focus->rdx, &dword_at_rdx, sizeof(uint32_t));
        bool have_insn_bytes = amd64_trace_read_guest(focus->rip, insn_bytes, sizeof(insn_bytes));
        bool have_rbx_text = amd64_trace_read_task_guest_cstring(current, focus->rbx,
                rbx_text, sizeof(rbx_text));
        bool have_rsi_text = amd64_trace_read_task_guest_cstring(current, focus->rsi,
                rsi_text, sizeof(rsi_text));
        bool have_rdi_text = amd64_trace_read_task_guest_cstring(current, focus->rdi,
                rdi_text, sizeof(rdi_text));
        fprintf(stderr,
                "amd64 as focus: rip=%#llx off=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rsp=%#llx rbp=%#llx r12=%#llx%s%s%s%s%s\n",
                (unsigned long long) focus->rip,
                (unsigned long long) (image_base == 0 || focus->rip < image_base ? 0 : focus->rip - image_base),
                (unsigned long long) focus->rax,
                (unsigned long long) focus->rbx,
                (unsigned long long) focus->rcx,
                (unsigned long long) focus->rdx,
                (unsigned long long) focus->rsi,
                (unsigned long long) focus->rdi,
                (unsigned long long) focus->rsp,
                (unsigned long long) focus->rbp,
                (unsigned long long) focus->r12,
                have_insn_bytes ? "" : " bytes=?",
                have_dword_at_rdx ? "" : " [rdx]=?",
                have_rbx_text ? "" : " rbx_str=?",
                have_rsi_text ? "" : " rsi_str=?",
                have_rdi_text ? "" : " rdi_str=?");
        if (have_insn_bytes) {
            fprintf(stderr,
                    "amd64 as focus: bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
                    insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
        }
        if (have_dword_at_rdx)
            fprintf(stderr, "amd64 as focus: [rdx]=%#llx\n", (unsigned long long) dword_at_rdx);
        if (have_rbx_text)
            fprintf(stderr, "amd64 as focus: rbx_str=\"%s\"\n", rbx_text);
        if (have_rsi_text)
            fprintf(stderr, "amd64 as focus: rsi_str=\"%s\"\n", rsi_text);
        if (have_rdi_text)
            fprintf(stderr, "amd64 as focus: rdi_str=\"%s\"\n", rdi_text);
    }
}

static inline bool amd64_trace_copy_task_guest_locked(const struct task *task,
        guest_addr_t guest_addr, void *out, size_t size) {
    uint8_t *dst = out;
    size_t copied = 0;

    if (task == NULL || task->mem == NULL)
        return false;

    while (copied < size) {
        struct pt_entry *pt = mem_pt(task->mem, PAGE(guest_addr + copied));
        if (pt == NULL || pt->data == NULL || pt->data->data == NULL)
            return false;
        size_t page_off = PGOFFSET(guest_addr + copied);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > size - copied)
            chunk = size - copied;
        memcpy(dst + copied, (uint8_t *) pt->data->data + pt->offset + page_off, chunk);
        copied += chunk;
    }

    return true;
}

static inline bool amd64_trace_read_guest(qword_t addr, void *out, size_t size) {
    if (current == NULL || current->mem == NULL)
        return false;
    guest_addr_t guest_addr;
    if (!amd64_guest_addr_ok(addr, size, &guest_addr))
        return false;
    bool ok = false;
    if (!amd64_trace_try_read_lock(&current->mem->lock))
        return false;
    ok = amd64_trace_copy_guest_locked(guest_addr, out, size);
    read_unlock(&current->mem->lock);
    return ok;
}

static inline bool amd64_trace_read_task_guest(const struct task *task, qword_t addr,
        void *out, size_t size) {
    if (task == NULL || task->mem == NULL)
        return false;
    guest_addr_t guest_addr;
    if (!amd64_guest_addr_ok(addr, size, &guest_addr))
        return false;
    bool ok = false;
    if (!amd64_trace_try_read_lock(&task->mem->lock))
        return false;
    ok = amd64_trace_copy_task_guest_locked(task, guest_addr, out, size);
    read_unlock(&task->mem->lock);
    return ok;
}

static bool amd64_trace_read_task_guest_cstring(const struct task *task, qword_t addr,
        char *buf, size_t size) {
    if (buf == NULL || size == 0) 
        return false;
    buf[0] = '\0';
    if (task == NULL || task->mem == NULL)
        return false;

    guest_addr_t guest_addr;
    if (!amd64_guest_addr_ok(addr, 1, &guest_addr))
        return false;

    size_t i;
    for (i = 0; i + 1 < size; i++) {
        char ch;
        if (!amd64_trace_read_task_guest(task, addr + i, &ch, sizeof(ch)))
            break;
        if (ch == '\0')
            break;
        if ((unsigned char) ch < 0x20 || (unsigned char) ch > 0x7e)
            buf[i] = '.';
        else
            buf[i] = ch;
    }
    buf[i] = '\0';
    return i != 0;
}

static inline bool amd64_trace_read_guest_cstring(qword_t addr, char *buf, size_t size) {
    if (size == 0)
        return false;
    buf[0] = '\0';
    if (addr == 0)
        return false;
    for (size_t i = 0; i + 1 < size; i++) {
        uint8_t ch = 0;
        if (!amd64_trace_read_guest(addr + i, &ch, sizeof(ch)))
            return false;
        buf[i] = ch;
        if (ch == '\0')
            return true;
    }
    buf[size - 1] = '\0';
    return true;
}

static inline void amd64_trace_bash_cond_probe(struct cpu_state *cpu) {
    if (!amd64_bash_trace_enabled() || amd64_bash_cond_probe_count >= 8)
        return;

    qword_t rip = cpu->amd64_current_insn_rip;
    if (rip != AMD64_BASH_COND_UNEXP_RIP && rip != AMD64_BASH_COND_BINOP_RIP)
        return;

    dword_t line = 0;
    dword_t token_a = 0;
    dword_t token_b = 0;
    dword_t parser_state = 0;
    dword_t extended_glob = 0;
    dword_t syn_dollar = 0;
    dword_t syn_star = 0;
    dword_t syn_dash = 0;
    dword_t syn_lbrack = 0;
    dword_t syn_rbrack = 0;
    dword_t syn_i = 0;
    char token_text[64];
    char format_text[96];

    bool have_line = amd64_trace_read_guest(AMD64_BASH_LINE_NUMBER_ADDR, &line, sizeof(line));
    bool have_token_a = amd64_trace_read_guest(AMD64_BASH_TOKEN_A_ADDR, &token_a, sizeof(token_a));
    bool have_token_b = amd64_trace_read_guest(AMD64_BASH_TOKEN_B_ADDR, &token_b, sizeof(token_b));
    bool have_parser_state = amd64_trace_read_guest(AMD64_BASH_PARSER_STATE_ADDR, &parser_state, sizeof(parser_state));
    bool have_extended_glob = amd64_trace_read_guest(AMD64_BASH_EXTENDED_GLOB_ADDR, &extended_glob, sizeof(extended_glob));
    bool have_syn_dollar = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x24u, &syn_dollar, sizeof(syn_dollar));
    bool have_syn_star = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x2au, &syn_star, sizeof(syn_star));
    bool have_syn_dash = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x2du, &syn_dash, sizeof(syn_dash));
    bool have_syn_lbrack = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x5bu, &syn_lbrack, sizeof(syn_lbrack));
    bool have_syn_rbrack = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x5du, &syn_rbrack, sizeof(syn_rbrack));
    bool have_syn_i = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x69u, &syn_i, sizeof(syn_i));
    bool have_token_text = amd64_trace_read_guest_cstring(cpu->amd64_regs[amd64_rdx], token_text, sizeof(token_text));
    bool have_format_text = amd64_trace_read_guest_cstring(cpu->amd64_regs[amd64_rsi], format_text, sizeof(format_text));

    amd64_bash_cond_probe_count++;
    printk("amd64 bash cond probe: rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rbp=%#llx line=%u%s tok_a=%#x%s tok_b=%#x%s parser_state=%#x%s extglob=%#x%s\n",
           (unsigned long long) rip,
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           line, have_line ? "" : "<?>",
           token_a, have_token_a ? "" : "<?>",
           token_b, have_token_b ? "" : "<?>",
           parser_state, have_parser_state ? "" : "<?>",
           extended_glob, have_extended_glob ? "" : "<?>");
    printk("amd64 bash cond probe: format=%s%s token=%s%s syn[$]=%#x%s syn[*]=%#x%s syn[-]=%#x%s syn[[]=%#x%s syn[]]=%#x%s syn[i]=%#x%s\n",
           have_format_text ? "\"" : "<unreadable>",
           have_format_text ? format_text : "",
           have_token_text ? "\"" : "<unreadable>",
           have_token_text ? token_text : "",
           syn_dollar, have_syn_dollar ? "" : "<?>",
           syn_star, have_syn_star ? "" : "<?>",
           syn_dash, have_syn_dash ? "" : "<?>",
           syn_lbrack, have_syn_lbrack ? "" : "<?>",
           syn_rbrack, have_syn_rbrack ? "" : "<?>",
           syn_i, have_syn_i ? "" : "<?>");
}

static inline void amd64_trace_cc1_slot_probe(struct cpu_state *cpu, qword_t rip, qword_t addr, qword_t value) {
    (void) cpu;
    if (!amd64_cc1_trace_enabled() || rip != 0x1288c6dull || amd64_cc1_slot_probe_count >= 4)
        return;

    amd64_cc1_slot_probe_count++;
    uint8_t bytes[32] = {};
    size_t have_bytes = 0;
    struct pt_entry *pt = NULL;
    if (amd64_trace_try_read_lock(&current->mem->lock)) {
        guest_addr_t guest_addr;
        if (amd64_guest_addr_ok(addr, sizeof(bytes), &guest_addr)) {
            if (amd64_trace_copy_guest_locked(guest_addr, bytes, sizeof(bytes))) {
                have_bytes = sizeof(bytes);
            }
        }
        pt = mem_pt(current->mem, PAGE(addr));
        read_unlock(&current->mem->lock);
    }
    printk("amd64 cc1 slot probe: rip=%#llx addr=%#llx value=%#llx\n",
           (unsigned long long) rip,
           (unsigned long long) addr,
           (unsigned long long) value);
    if (pt == NULL) {
        printk("amd64 cc1 slot probe: page=%#llx unmapped\n",
               (unsigned long long) PAGE(addr));
    } else {
        const char *name = pt->data != NULL ? pt->data->name : "-";
        printk("amd64 cc1 slot probe: page=%#llx flags=%#x off=%#zx name=%s\n",
               (unsigned long long) PAGE(addr),
               pt->flags,
               pt->offset,
               name != NULL ? name : "-");
    }

    if (have_bytes != 0) {
        printk("amd64 cc1 slot probe bytes:");
        for (size_t i = 0; i < have_bytes; i++)
            printk(" %02x", bytes[i]);
        printk("\n");
    }
}

static inline void amd64_trace_cc1_cmp_probe(struct cpu_state *cpu, qword_t rip, qword_t addr,
        qword_t lhs, qword_t rhs, qword_t result, unsigned size) {
    if (!amd64_cc1_trace_enabled() || rip != 0x11257e0ull || amd64_cc1_cmp_probe_count >= 4)
        return;

    amd64_cc1_cmp_probe_count++;
    printk("amd64 cc1 cmp probe: rip=%#llx addr=%#llx lhs=%#llx rhs=%#llx result=%#llx size=%u zf=%d sf=%d of=%d cf=%d\n",
           (unsigned long long) rip,
           (unsigned long long) addr,
           (unsigned long long) lhs,
           (unsigned long long) rhs,
           (unsigned long long) result,
           size,
           cpu->zf,
           cpu->sf,
           cpu->of,
           cpu->cf);
}

static inline void amd64_trace_cc1_je_probe(struct cpu_state *cpu, qword_t rip, bool taken, qword_t target) {
    if (!amd64_cc1_trace_enabled() || rip != 0x11257efull || amd64_cc1_je_probe_count >= 4)
        return;

    amd64_cc1_je_probe_count++;
    printk("amd64 cc1 je probe: rip=%#llx zf=%d taken=%d target=%#llx rdi=%#llx rbp=%#llx\n",
           (unsigned long long) rip,
           cpu->zf,
           taken,
           (unsigned long long) target,
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rbp]);
}

static inline void amd64_trace_cc1_va_list_branch_probe(struct cpu_state *cpu, qword_t rip,
        bool taken, qword_t target, const char *kind) {
    if (!amd64_cc1_trace_enabled() || amd64_cc1_va_list_branch_probe_count >= 8)
        return;
    if (rip != AMD64_CC1_VA_LIST_HOOK_JNE_RIP && rip != AMD64_CC1_VA_LIST_HOOK_FALLBACK_JMP_RIP)
        return;

    qword_t abi_flags = 0;
    qword_t ms_slot = 0;
    qword_t sysv_slot = 0;
    bool have_abi_flags = false;
    bool have_ms_slot = false;
    bool have_sysv_slot = false;

    have_abi_flags = amd64_trace_read_guest(AMD64_CC1_ABI_FLAG_ADDR, &abi_flags, sizeof(abi_flags));
    have_ms_slot = amd64_trace_read_guest(AMD64_CC1_NULL_SLOT_ADDR, &ms_slot, sizeof(ms_slot));
    have_sysv_slot = amd64_trace_read_guest(AMD64_CC1_SYSV_SLOT_ADDR, &sysv_slot, sizeof(sysv_slot));

    amd64_cc1_va_list_branch_probe_count++;
    printk("amd64 cc1 va_list branch: rip=%#llx kind=%s taken=%d target=%#llx zf=%d cf=%d sf=%d of=%d rdi=%#llx rbp=%#llx flags=%#llx%s ms=%#llx%s sysv=%#llx%s\n",
           (unsigned long long) rip,
           kind,
           taken,
           (unsigned long long) target,
           cpu->zf,
           cpu->cf,
           cpu->sf,
           cpu->of,
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           (unsigned long long) abi_flags,
           have_abi_flags ? "" : "<?>",
           (unsigned long long) ms_slot,
           have_ms_slot ? "" : "<?>",
           (unsigned long long) sysv_slot,
           have_sysv_slot ? "" : "<?>");
}

static inline void amd64_trace_cc1_xfer_probe(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip, qword_t target, const char *kind) {
    uint8_t bytes[16] = {};
    bool have_bytes = false;
    guest_addr_t insn_addr;

    if (!amd64_cc1_trace_enabled() || amd64_cc1_xfer_probe_count >= 32)
        return;

    if (amd64_guest_addr_ok(saved_rip, sizeof(bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, bytes, sizeof(bytes))) {
        have_bytes = true;
    }

    amd64_cc1_xfer_probe_count++;
    printk("amd64 cc1 xfer: kind=%s from=%#llx to=%#llx rsp=%#llx rbp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n",
           kind,
           (unsigned long long) saved_rip,
           (unsigned long long) target,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_r12],
           (unsigned long long) cpu->amd64_regs[amd64_r13],
           (unsigned long long) cpu->amd64_regs[amd64_r14],
           (unsigned long long) cpu->amd64_regs[amd64_r15]);
    if (have_bytes) {
        printk("amd64 cc1 xfer bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline bool amd64_trace_range_intersects(qword_t base_a, unsigned size_a, qword_t base_b, unsigned size_b) {
    qword_t end_a = base_a + size_a;
    qword_t end_b = base_b + size_b;
    return base_a < end_b && base_b < end_a;
}

static inline void amd64_trace_cc1_slot_write_probe(struct cpu_state *cpu, qword_t guest_addr,
        const void *value, unsigned size) {
    if (!amd64_cc1_trace_enabled() || amd64_cc1_slot_write_probe_count >= 16)
        return;
    if (!amd64_trace_range_intersects(guest_addr, size, AMD64_CC1_NULL_SLOT_ADDR, 8) &&
            !amd64_trace_range_intersects(guest_addr, size, AMD64_CC1_CMP_GLOBAL_ADDR, 8))
        return;

    amd64_cc1_slot_write_probe_count++;
    qword_t observed = 0;
    memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));
    printk("amd64 cc1 slot write: rip=%#llx addr=%#llx size=%u value=%#llx rdi=%#llx rbp=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) guest_addr,
           size,
           (unsigned long long) observed,
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rbp]);
}

static inline void amd64_trace_cc1_va_list_init_probe(struct cpu_state *cpu) {
    if (!amd64_cc1_trace_enabled() || amd64_cc1_va_list_init_probe_count >= 24)
        return;

    qword_t rip = cpu->amd64_current_insn_rip;
    if (rip != AMD64_CC1_VA_LIST_HOOK_RIP &&
            rip != AMD64_CC1_VA_LIST_COMPLEX_RIP &&
            rip != AMD64_CC1_VA_LIST_GETTER_RIP &&
            rip != AMD64_CC1_VA_LIST_INIT_ENTRY_RIP &&
            rip != AMD64_CC1_VA_LIST_INIT_SYSV_STORE_RIP &&
            rip != AMD64_CC1_VA_LIST_INIT_ATTR_CALL_RIP &&
            rip != AMD64_CC1_VA_LIST_INIT_ATTR_RET_RIP &&
            rip != AMD64_CC1_VA_LIST_INIT_MS_STORE_RIP)
        return;

    qword_t ms_slot = 0;
    qword_t sysv_slot = 0;
    qword_t abi_flags = 0;
    uint32_t option = 0;
    bool have_ms_slot = false;
    bool have_sysv_slot = false;
    bool have_abi_flags = false;
    bool have_option = false;

    have_ms_slot = amd64_trace_read_guest(AMD64_CC1_NULL_SLOT_ADDR, &ms_slot, sizeof(ms_slot));
    have_sysv_slot = amd64_trace_read_guest(AMD64_CC1_SYSV_SLOT_ADDR, &sysv_slot, sizeof(sysv_slot));
    have_abi_flags = amd64_trace_read_guest(AMD64_CC1_ABI_FLAG_ADDR, &abi_flags, sizeof(abi_flags));
    have_option = amd64_trace_read_guest(AMD64_CC1_MS_VARIANT_ADDR, &option, sizeof(option));

    amd64_cc1_va_list_init_probe_count++;
    printk("amd64 cc1 va_list init: rip=%#llx rax=%#llx rbx=%#llx rdi=%#llx rsi=%#llx rbp=%#llx flags=%#llx%s ms=%#llx%s sysv=%#llx%s opt=%#x%s\n",
           (unsigned long long) rip,
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           (unsigned long long) abi_flags,
           have_abi_flags ? "" : "<?>",
           (unsigned long long) ms_slot,
           have_ms_slot ? "" : "<?>",
           (unsigned long long) sysv_slot,
           have_sysv_slot ? "" : "<?>",
           option,
           have_option ? "" : "<?>");
}

static inline void amd64_trace_cc1_step(struct cpu_state *cpu) {
    if (amd64_cc1_trace_pid != current->pid) {
        memset(amd64_cc1_trace, 0, sizeof(amd64_cc1_trace));
        amd64_cc1_trace_next = 0;
        amd64_cc1_trace_pid = current->pid;
    }

    struct amd64_cc1_trace *trace = &amd64_cc1_trace[amd64_cc1_trace_next++ % AMD64_CC1_TRACE_COUNT];
    memset(trace, 0, sizeof(*trace));
    trace->rip = cpu->amd64_current_insn_rip;
    trace->rax = cpu->amd64_regs[amd64_rax];
    trace->rbx = cpu->amd64_regs[amd64_rbx];
    trace->rcx = cpu->amd64_regs[amd64_rcx];
    trace->rdx = cpu->amd64_regs[amd64_rdx];
    trace->rsi = cpu->amd64_regs[amd64_rsi];
    trace->rdi = cpu->amd64_regs[amd64_rdi];
    trace->rsp = cpu->amd64_regs[amd64_rsp];
    trace->rbp = cpu->amd64_regs[amd64_rbp];
    trace->r8 = cpu->amd64_regs[amd64_r8];
    trace->r9 = cpu->amd64_regs[amd64_r9];
    trace->r12 = cpu->amd64_regs[amd64_r12];

    guest_addr_t guest_addr;
    if (!amd64_guest_addr_ok(trace->rip, sizeof(trace->bytes), &guest_addr) || current->mem == NULL)
        return;

    if (amd64_trace_try_read_lock(&current->mem->lock)) {
        if (amd64_trace_copy_guest_locked(guest_addr, trace->bytes, sizeof(trace->bytes))) {
            trace->byte_count = sizeof(trace->bytes);
        }
        read_unlock(&current->mem->lock);
    }

    amd64_trace_cc1_va_list_init_probe(cpu);
}

static inline void amd64_trace_as_step(struct cpu_state *cpu) {
    if (!amd64_as_trace_enabled())
        return;

    if (amd64_as_trace_pid != current->pid) {
        memset(amd64_as_trace, 0, sizeof(amd64_as_trace));
        amd64_as_trace_next = 0;
        amd64_as_trace_pid = current->pid;
        memset(amd64_as_events, 0, sizeof(amd64_as_events));
        amd64_as_event_next = 0;
        amd64_as_event_pid = current->pid;
        memset(amd64_as_suspects, 0, sizeof(amd64_as_suspects));
        amd64_as_suspect_next = 0;
        amd64_as_suspect_pid = current->pid;
        memset(amd64_as_focus, 0, sizeof(amd64_as_focus));
        amd64_as_focus_next = 0;
        amd64_as_focus_pid = current->pid;
    }

    amd64_as_scan_template_probe(cpu);

    struct amd64_as_trace *trace = &amd64_as_trace[amd64_as_trace_next++ % AMD64_AS_TRACE_COUNT];
    memset(trace, 0, sizeof(*trace));
    trace->rip = cpu->amd64_current_insn_rip;
    trace->rax = cpu->amd64_regs[amd64_rax];
    trace->rbx = cpu->amd64_regs[amd64_rbx];
    trace->rcx = cpu->amd64_regs[amd64_rcx];
    trace->rdx = cpu->amd64_regs[amd64_rdx];
    trace->rsi = cpu->amd64_regs[amd64_rsi];
    trace->rdi = cpu->amd64_regs[amd64_rdi];
    trace->rsp = cpu->amd64_regs[amd64_rsp];
    trace->rbp = cpu->amd64_regs[amd64_rbp];
    trace->r8 = cpu->amd64_regs[amd64_r8];
    trace->r9 = cpu->amd64_regs[amd64_r9];
    trace->r12 = cpu->amd64_regs[amd64_r12];

    guest_addr_t guest_addr;
    if (!amd64_guest_addr_ok(trace->rip, sizeof(trace->bytes), &guest_addr) || current->mem == NULL)
        return;

    if (amd64_trace_try_read_lock(&current->mem->lock)) {
        if (amd64_trace_copy_guest_locked(guest_addr, trace->bytes, sizeof(trace->bytes)))
            trace->byte_count = sizeof(trace->bytes);
        read_unlock(&current->mem->lock);
    }

    amd64_trace_as_focus(cpu);

    switch (trace->rip) {
    case AMD64_AS_RESET_DONE_RIP:
        amd64_trace_as_event(cpu, amd64_as_event_reset_done);
        break;
    case AMD64_AS_STATE31_WRITE1_DONE_RIP:
    case AMD64_AS_STATE31_WRITE2_DONE_RIP:
        amd64_trace_as_event(cpu, amd64_as_event_state31_write);
        break;
    case AMD64_AS_STATE31_CHECK_DONE_RIP:
        amd64_trace_as_event(cpu, amd64_as_event_state31_check);
        break;
    default:
        break;
    }

}

#define AMD64_AS_STATE_BLOCK_OFFSET 0xd3880ull
#define AMD64_AS_STATE_DUMP_SIZE 0x40u
#define AMD64_AS_STATE_TRACE_WINDOW 0x140u
#define AMD64_AS_DESCRIPTOR_SIZE 16u

static bool amd64_trace_read_task_u32(const struct task *task, qword_t addr, uint32_t *value) {
    return amd64_trace_read_task_guest(task, addr, value, sizeof(*value));
}

static bool amd64_trace_read_task_u64(const struct task *task, qword_t addr, uint64_t *value) {
    return amd64_trace_read_task_guest(task, addr, value, sizeof(*value));
}

static void amd64_dump_as_descriptor_task(const struct task *task, unsigned index, qword_t ptr,
        uint32_t slot_1c, uint32_t slot_48, uint32_t slot_88, uint32_t slot_9c) {
    if (ptr == 0 && slot_1c == 0 && slot_48 == 0 && slot_88 == 0 && slot_9c == 0)
        return;

    printk("amd64 as desc[%u]: slot_1c=%#x slot_48=%#x slot_88=%#x slot_9c=%#x ptr=%#llx",
           index,
           slot_1c,
           slot_48,
           slot_88,
           slot_9c,
           (unsigned long long) ptr);
    if (amd64_as_stderr_enabled()) {
        fprintf(stderr,
                "amd64 as desc[%u]: slot_1c=%#x slot_48=%#x slot_88=%#x slot_9c=%#x ptr=%#llx",
                index,
                slot_1c,
                slot_48,
                slot_88,
                slot_9c,
                (unsigned long long) ptr);
    }
    if (ptr == 0) {
        printk("\n");
        if (amd64_as_stderr_enabled())
            fprintf(stderr, "\n");
        return;
    }

    uint8_t desc[16] = {};
    if (!amd64_trace_read_task_guest(task, ptr, desc, sizeof(desc))) {
        printk(" unreadable\n");
        if (amd64_as_stderr_enabled())
            fprintf(stderr, " unreadable\n");
        return;
    }

    printk(" flags=%#x kind=%#x raw="
           "%02x %02x %02x %02x %02x %02x %02x %02x "
           "%02x %02x %02x %02x %02x %02x %02x %02x\n",
           desc[0xc],
           desc[0xd],
           desc[0], desc[1], desc[2], desc[3],
           desc[4], desc[5], desc[6], desc[7],
           desc[8], desc[9], desc[10], desc[11],
           desc[12], desc[13], desc[14], desc[15]);
    if (amd64_as_stderr_enabled()) {
        fprintf(stderr,
                " flags=%#x kind=%#x raw="
                "%02x %02x %02x %02x %02x %02x %02x %02x "
                "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                desc[0xc],
                desc[0xd],
                desc[0], desc[1], desc[2], desc[3],
                desc[4], desc[5], desc[6], desc[7],
                desc[8], desc[9], desc[10], desc[11],
                desc[12], desc[13], desc[14], desc[15]);
    }
}

static bool amd64_read_task_auxv64(const struct task *task, qword_t type, qword_t *value) {
    if (task == NULL || task->mm == NULL || value == NULL)
        return false;
    guest_addr_t start = task->mm->auxv_start;
    guest_addr_t end = task->mm->auxv_end;
    if (start == 0 || end <= start)
        return false;

    struct aux64_ent ent;
    for (guest_addr_t addr = start; addr + sizeof(ent) <= end; addr += sizeof(ent)) {
        if (!amd64_trace_read_task_guest(task, addr, &ent, sizeof(ent)))
            return false;
        if (ent.type == 0)
            break;
        if (ent.type == type) {
            *value = ent.value;
            return true;
        }
    }
    return false;
}

static bool amd64_resolve_task_image_base(const struct task *task, qword_t *base) {
    if (task == NULL || task->abi != GUEST_ABI_AMD64 || base == NULL)
        return false;

    qword_t phdr_addr = 0;
    qword_t phent_size = 0;
    qword_t phnum = 0;
    if (!amd64_read_task_auxv64(task, AX_PHDR, &phdr_addr) ||
            !amd64_read_task_auxv64(task, AX_PHENT, &phent_size) ||
            !amd64_read_task_auxv64(task, AX_PHNUM, &phnum))
        return false;
    if (phdr_addr == 0 || phent_size < sizeof(struct prg_header64) || phnum == 0 || phnum > 128)
        return false;

    for (qword_t i = 0; i < phnum; i++) {
        struct prg_header64 ph;
        qword_t addr = phdr_addr + i * phent_size;
        if (!amd64_trace_read_task_guest(task, addr, &ph, sizeof(ph)))
            return false;
        if (ph.type == PT_PHDR) {
            *base = phdr_addr - ph.vaddr;
            return true;
        }
    }
    return false;
}

static inline bool amd64_trace_intersects_guest_range(qword_t guest_addr, unsigned size,
        qword_t watch_addr, unsigned watch_size) {
    qword_t end = guest_addr + size;
    qword_t watch_end = watch_addr + watch_size;
    return guest_addr < watch_end && end > watch_addr;
}

static bool amd64_trace_as_state_region(qword_t guest_addr, unsigned size,
        qword_t *region_base, uint8_t *region_kind, uint8_t *region_index) {
    static pid_t_ amd64_as_image_base_pid;
    static qword_t amd64_as_image_base;

    if (!amd64_as_trace_enabled())
        return false;

    if (amd64_as_image_base_pid != current->pid) {
        amd64_as_image_base = 0;
        if (!amd64_resolve_task_image_base(current, &amd64_as_image_base))
            return false;
        amd64_as_image_base_pid = current->pid;
    }

    qword_t state_addr = amd64_as_image_base + AMD64_AS_STATE_BLOCK_OFFSET;
    if (amd64_trace_intersects_guest_range(guest_addr, size, state_addr, AMD64_AS_STATE_TRACE_WINDOW)) {
        *region_base = state_addr;
        *region_kind = amd64_as_state_region_block;
        *region_index = 0;
        return true;
    }

    for (unsigned i = 0; i <= 4; i++) {
        uint64_t ptr = 0;
        if (!amd64_trace_read_current_guest(state_addr + 0x60 + (qword_t) i * 8, &ptr, sizeof(ptr)) || ptr == 0)
            continue;
        if (!amd64_trace_intersects_guest_range(guest_addr, size, ptr, AMD64_AS_DESCRIPTOR_SIZE))
            continue;
        *region_base = ptr;
        *region_kind = amd64_as_state_region_desc;
        *region_index = i;
        return true;
    }

    return false;
}

static void amd64_trace_as_state_write(struct cpu_state *cpu, qword_t guest_addr,
        const void *value, unsigned size) {
    qword_t region_base = 0;
    uint8_t region_kind = 0;
    uint8_t region_index = 0;
    if (!amd64_trace_as_state_region(guest_addr, size, &region_base, &region_kind, &region_index))
        return;

    if (amd64_as_state_write_pid != current->pid) {
        memset(amd64_as_state_writes, 0, sizeof(amd64_as_state_writes));
        amd64_as_state_write_next = 0;
        amd64_as_state_write_pid = current->pid;
    }

    struct amd64_as_state_write *entry =
        &amd64_as_state_writes[amd64_as_state_write_next++ % AMD64_AS_STATE_WRITE_COUNT];
    memset(entry, 0, sizeof(*entry));
    entry->rip = cpu->amd64_current_insn_rip;
    entry->addr = guest_addr;
    entry->region_base = region_base;
    entry->region_kind = region_kind;
    entry->region_index = region_index;
    entry->size = size;
    memcpy(&entry->value, value, size < sizeof(entry->value) ? size : sizeof(entry->value));

    if (region_kind == amd64_as_state_region_desc) {
        entry->snapshot_addr = region_base;
    } else {
        qword_t offset = guest_addr - region_base;
        entry->snapshot_addr = region_base + (offset & ~0xfULL);
    }

    if (amd64_trace_read_current_guest(entry->snapshot_addr, entry->bytes, sizeof(entry->bytes)))
        entry->byte_count = sizeof(entry->bytes);
}

void dump_amd64_cc1_trace(const struct cpu_state *cpu) {
    (void) cpu;
    if (current == NULL || current->abi != GUEST_ABI_AMD64 || strcmp(current->comm, "cc1") != 0)
        return;
    if (amd64_cc1_trace_pid != current->pid)
        return;

    unsigned total = amd64_cc1_trace_next;
    if (total == 0)
        return;

    unsigned count = total < AMD64_CC1_TRACE_COUNT ? total : AMD64_CC1_TRACE_COUNT;
    unsigned start = total >= AMD64_CC1_TRACE_COUNT ? total - AMD64_CC1_TRACE_COUNT : 0;
    printk("amd64 cc1 trace (%u entries):\n", count);
    for (unsigned i = 0; i < count; i++) {
        const struct amd64_cc1_trace *trace = &amd64_cc1_trace[(start + i) % AMD64_CC1_TRACE_COUNT];
        printk("cc1[%02u] rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rsp=%#llx rbp=%#llx r8=%#llx r9=%#llx r12=%#llx bytes=",
               i,
               (unsigned long long) trace->rip,
               (unsigned long long) trace->rax,
               (unsigned long long) trace->rbx,
               (unsigned long long) trace->rcx,
               (unsigned long long) trace->rdx,
               (unsigned long long) trace->rsi,
               (unsigned long long) trace->rdi,
               (unsigned long long) trace->rsp,
               (unsigned long long) trace->rbp,
               (unsigned long long) trace->r8,
               (unsigned long long) trace->r9,
               (unsigned long long) trace->r12);
        for (unsigned j = 0; j < trace->byte_count; j++)
            printk("%02x%s", trace->bytes[j], j + 1 == trace->byte_count ? "" : " ");
        printk("\n");
    }
}

void dump_amd64_as_trace_task(const struct task *task) {
    if (task == NULL || task->abi != GUEST_ABI_AMD64 || strcmp(task->comm, "as") != 0)
        return;
    if (amd64_as_trace_pid != task->pid)
        return;

    if (amd64_as_state_write_pid == task->pid && amd64_as_state_write_next != 0) {
        unsigned total = amd64_as_state_write_next;
        unsigned count = total < AMD64_AS_STATE_WRITE_COUNT ? total : AMD64_AS_STATE_WRITE_COUNT;
        unsigned start = total >= AMD64_AS_STATE_WRITE_COUNT ? total - AMD64_AS_STATE_WRITE_COUNT : 0;
        printk("amd64 as state writes pid=%d (%u entries):\n", task->pid, count);
        for (unsigned i = 0; i < count; i++) {
            const struct amd64_as_state_write *entry =
                &amd64_as_state_writes[(start + i) % AMD64_AS_STATE_WRITE_COUNT];
            const char *kind = entry->region_kind == amd64_as_state_region_desc ? "desc" : "state";
            printk("as_state_write[%02u] kind=%s index=%u rip=%#llx addr=%#llx size=%u value=%#llx region=%#llx snapshot=%#llx",
                   i,
                   kind,
                   entry->region_index,
                   (unsigned long long) entry->rip,
                   (unsigned long long) entry->addr,
                   entry->size,
                   (unsigned long long) entry->value,
                   (unsigned long long) entry->region_base,
                   (unsigned long long) entry->snapshot_addr);
            if (entry->byte_count == 0) {
                printk(" bytes=?\n");
                continue;
            }
            printk(" bytes=");
            for (unsigned j = 0; j < entry->byte_count; j++)
                printk("%02x%s", entry->bytes[j], j + 1 == entry->byte_count ? "" : " ");
            printk("\n");
        }
    }

    if (amd64_as_event_pid == task->pid && amd64_as_event_next != 0) {
        unsigned total = amd64_as_event_next;
        unsigned count = total < AMD64_AS_EVENT_COUNT ? total : AMD64_AS_EVENT_COUNT;
        unsigned start = total >= AMD64_AS_EVENT_COUNT ? total - AMD64_AS_EVENT_COUNT : 0;
        printk("amd64 as state31 events pid=%d (%u entries):\n", task->pid, count);
        for (unsigned i = 0; i < count; i++) {
            const struct amd64_as_event *event = &amd64_as_events[(start + i) % AMD64_AS_EVENT_COUNT];
            const char *kind = "unknown";
            switch (event->kind) {
            case amd64_as_event_reset_done:
                kind = "reset_done";
                break;
            case amd64_as_event_state31_write:
                kind = "state31_write";
                break;
            case amd64_as_event_state31_check:
                kind = "state31_check";
                break;
            default:
                break;
            }
            printk("as_event[%02u] kind=%s rip=%#llx state31=%#x rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r12=%#llx\n",
                   i,
                   kind,
                   (unsigned long long) event->rip,
                   event->state31,
                   (unsigned long long) event->rax,
                   (unsigned long long) event->rbx,
                   (unsigned long long) event->rcx,
                   (unsigned long long) event->rdx,
                   (unsigned long long) event->rsi,
                   (unsigned long long) event->rdi,
                   (unsigned long long) event->r12);
        }
    }

    if (amd64_as_suspect_pid == task->pid && amd64_as_suspect_next != 0) {
        unsigned total = amd64_as_suspect_next;
        unsigned count = total < AMD64_AS_SUSPECT_COUNT ? total : AMD64_AS_SUSPECT_COUNT;
        unsigned start = total >= AMD64_AS_SUSPECT_COUNT ? total - AMD64_AS_SUSPECT_COUNT : 0;
        printk("amd64 as suspect ops pid=%d (%u entries):\n", task->pid, count);
        for (unsigned i = 0; i < count; i++) {
            const struct amd64_as_suspect *suspect =
                &amd64_as_suspects[(start + i) % AMD64_AS_SUSPECT_COUNT];
            switch (suspect->kind) {
            case amd64_as_suspect_bt:
                printk("as_suspect[%02u] kind=bt rip=%#llx op=%#x size=%u mem=%u index=%#llx bit=%#llx addr=%#llx lhs=%#llx value=%#llx aux=%#x\n",
                       i,
                       (unsigned long long) suspect->rip,
                       suspect->op,
                       suspect->size,
                       suspect->aux,
                       (unsigned long long) suspect->bit_index,
                       (unsigned long long) suspect->bit,
                       (unsigned long long) suspect->addr,
                       (unsigned long long) suspect->lhs,
                       (unsigned long long) suspect->value,
                       suspect->aux);
                break;
            case amd64_as_suspect_stack:
                printk("as_suspect[%02u] kind=stack rip=%#llx op=%u size=%u old_rsp=%#llx new_rsp=%#llx value=%#llx\n",
                       i,
                       (unsigned long long) suspect->rip,
                       suspect->op,
                       suspect->size,
                       (unsigned long long) suspect->old_rsp,
                       (unsigned long long) suspect->new_rsp,
                       (unsigned long long) suspect->value);
                break;
            default:
                break;
            }
        }
    }

    if (amd64_as_focus_pid == task->pid && amd64_as_focus_next != 0) {
        unsigned total = amd64_as_focus_next;
        unsigned count = total < AMD64_AS_FOCUS_COUNT ? total : AMD64_AS_FOCUS_COUNT;
        unsigned start = total >= AMD64_AS_FOCUS_COUNT ? total - AMD64_AS_FOCUS_COUNT : 0;
        printk("amd64 as focus pid=%d (%u entries):\n", task->pid, count);
        for (unsigned i = 0; i < count; i++) {
            const struct amd64_as_focus *focus =
                &amd64_as_focus[(start + i) % AMD64_AS_FOCUS_COUNT];
            qword_t dword_at_rdx = 0;
            bool have_dword_at_rdx = amd64_trace_read_task_guest(task, focus->rdx,
                    &dword_at_rdx, sizeof(uint32_t));
            char rbx_text[64];
            char rsi_text[64];
            char rdi_text[64];
            bool have_rbx_text = amd64_trace_read_task_guest_cstring(task, focus->rbx,
                    rbx_text, sizeof(rbx_text));
            bool have_rsi_text = amd64_trace_read_task_guest_cstring(task, focus->rsi,
                    rsi_text, sizeof(rsi_text));
            bool have_rdi_text = amd64_trace_read_task_guest_cstring(task, focus->rdi,
                    rdi_text, sizeof(rdi_text));
            printk("as_focus[%02u] rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rsp=%#llx rbp=%#llx r12=%#llx%s%s%s%s\n",
                   i,
                   (unsigned long long) focus->rip,
                   (unsigned long long) focus->rax,
                   (unsigned long long) focus->rbx,
                   (unsigned long long) focus->rcx,
                   (unsigned long long) focus->rdx,
                   (unsigned long long) focus->rsi,
                   (unsigned long long) focus->rdi,
                   (unsigned long long) focus->rsp,
                   (unsigned long long) focus->rbp,
                   (unsigned long long) focus->r12,
                   have_dword_at_rdx ? "" : " [rdx]=?",
                   have_rbx_text ? "" : " rbx_str=?",
                   have_rsi_text ? "" : " rsi_str=?",
                   have_rdi_text ? "" : " rdi_str=?");
            if (have_dword_at_rdx)
                printk("as_focus[%02u] [rdx]=%#llx\n", i, (unsigned long long) dword_at_rdx);
            if (have_rbx_text)
                printk("as_focus[%02u] rbx_str=\"%s\"\n", i, rbx_text);
            if (have_rsi_text)
                printk("as_focus[%02u] rsi_str=\"%s\"\n", i, rsi_text);
            if (have_rdi_text)
                printk("as_focus[%02u] rdi_str=\"%s\"\n", i, rdi_text);
        }
    }

    unsigned total = amd64_as_trace_next;
    if (total == 0)
        return;

    unsigned count = total < AMD64_AS_TRACE_COUNT ? total : AMD64_AS_TRACE_COUNT;
    unsigned start = total >= AMD64_AS_TRACE_COUNT ? total - AMD64_AS_TRACE_COUNT : 0;

    bool have_error_window = false;
    unsigned window_first = 0;
    unsigned window_count = count;
    unsigned trigger_index = 0;
    qword_t trigger_rip = 0;
    for (unsigned i = 0; i < count; i++) {
        const struct amd64_as_trace *trace = &amd64_as_trace[(start + i) % AMD64_AS_TRACE_COUNT];
        if (!amd64_as_is_error_path_rip(trace->rip))
            continue;

        unsigned pre = i < AMD64_AS_ERROR_PRE_COUNT ? i : AMD64_AS_ERROR_PRE_COUNT;
        unsigned post = count - i;
        if (post > AMD64_AS_ERROR_POST_COUNT)
            post = AMD64_AS_ERROR_POST_COUNT;
        window_first = i - pre;
        window_count = pre + post;
        trigger_index = i;
        trigger_rip = trace->rip;
        have_error_window = true;
        break;
    }

    if (have_error_window) {
        printk("amd64 as trace pid=%d (%u entries, showing %u around error path rip=%#llx at trace index=%u):\n",
               task->pid,
               count,
               window_count,
               (unsigned long long) trigger_rip,
               trigger_index);
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr,
                    "amd64 as trace pid=%d (%u entries, showing %u around error path rip=%#llx at trace index=%u):\n",
                    task->pid,
                    count,
                    window_count,
                    (unsigned long long) trigger_rip,
                    trigger_index);
        }
    } else {
        printk("amd64 as trace pid=%d (%u entries):\n", task->pid, count);
        if (amd64_as_stderr_enabled())
            fprintf(stderr, "amd64 as trace pid=%d (%u entries):\n", task->pid, count);
    }

    for (unsigned i = 0; i < window_count; i++) {
        unsigned trace_index = window_first + i;
        const struct amd64_as_trace *trace =
            &amd64_as_trace[(start + trace_index) % AMD64_AS_TRACE_COUNT];
        printk("as[%02u] rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rsp=%#llx rbp=%#llx r8=%#llx r9=%#llx r12=%#llx bytes=",
               trace_index,
               (unsigned long long) trace->rip,
               (unsigned long long) trace->rax,
               (unsigned long long) trace->rbx,
               (unsigned long long) trace->rcx,
               (unsigned long long) trace->rdx,
               (unsigned long long) trace->rsi,
               (unsigned long long) trace->rdi,
               (unsigned long long) trace->rsp,
               (unsigned long long) trace->rbp,
               (unsigned long long) trace->r8,
               (unsigned long long) trace->r9,
               (unsigned long long) trace->r12);
        for (unsigned j = 0; j < trace->byte_count; j++)
            printk("%02x%s", trace->bytes[j], j + 1 == trace->byte_count ? "" : " ");
        printk("\n");
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr,
                    "as[%02u] rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rsp=%#llx rbp=%#llx r8=%#llx r9=%#llx r12=%#llx bytes=",
                    trace_index,
                    (unsigned long long) trace->rip,
                    (unsigned long long) trace->rax,
                    (unsigned long long) trace->rbx,
                    (unsigned long long) trace->rcx,
                    (unsigned long long) trace->rdx,
                    (unsigned long long) trace->rsi,
                    (unsigned long long) trace->rdi,
                    (unsigned long long) trace->rsp,
                    (unsigned long long) trace->rbp,
                    (unsigned long long) trace->r8,
                    (unsigned long long) trace->r9,
                    (unsigned long long) trace->r12);
            for (unsigned j = 0; j < trace->byte_count; j++)
                fprintf(stderr, "%02x%s", trace->bytes[j], j + 1 == trace->byte_count ? "" : " ");
            fprintf(stderr, "\n");
        }
    }
}

void dump_amd64_as_state_task(const struct task *task) {
    if (task == NULL || task->abi != GUEST_ABI_AMD64 || strcmp(task->comm, "as") != 0)
        return;

    qword_t image_base = 0;
    if (!amd64_resolve_task_image_base(task, &image_base)) {
        printk("amd64 as state: failed to resolve image base\n");
        if (amd64_as_stderr_enabled())
            fprintf(stderr, "amd64 as state: failed to resolve image base\n");
        return;
    }

    qword_t state_addr = image_base + AMD64_AS_STATE_BLOCK_OFFSET;
    uint8_t state[AMD64_AS_STATE_DUMP_SIZE] = {};
    if (!amd64_trace_read_task_guest(task, state_addr, state, sizeof(state))) {
        printk("amd64 as state: image_base=%#llx state=%#llx unreadable\n",
               (unsigned long long) image_base,
               (unsigned long long) state_addr);
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr, "amd64 as state: image_base=%#llx state=%#llx unreadable\n",
                    (unsigned long long) image_base,
                    (unsigned long long) state_addr);
        }
        return;
    }

    uint32_t slot_f8 = 0;
    uint8_t slot_fc = 0;
    uint8_t slot_100 = 0;
    uint32_t slot_124 = 0;
    uint32_t slot_104 = 0;
    uint32_t slot_108 = 0;
    uint32_t slot_10c = 0;
    uint32_t slot_110 = 0;
    uint32_t slot_114 = 0;
    uint32_t slot_118 = 0;
    uint64_t slot_b0 = 0;
    uint64_t slot_b8 = 0;
    uint64_t slot_c0 = 0;
    uint64_t slot_c8 = 0;
    uint64_t slot_128 = 0;
    bool have_f8 = amd64_trace_read_task_guest(task, state_addr + 0xf8, &slot_f8, sizeof(slot_f8));
    bool have_fc = amd64_trace_read_task_guest(task, state_addr + 0xfc, &slot_fc, sizeof(slot_fc));
    bool have_100 = amd64_trace_read_task_guest(task, state_addr + 0x100, &slot_100, sizeof(slot_100));
    bool have_104 = amd64_trace_read_task_u32(task, state_addr + 0x104, &slot_104);
    bool have_108 = amd64_trace_read_task_u32(task, state_addr + 0x108, &slot_108);
    bool have_10c = amd64_trace_read_task_u32(task, state_addr + 0x10c, &slot_10c);
    bool have_110 = amd64_trace_read_task_u32(task, state_addr + 0x110, &slot_110);
    bool have_114 = amd64_trace_read_task_u32(task, state_addr + 0x114, &slot_114);
    bool have_118 = amd64_trace_read_task_u32(task, state_addr + 0x118, &slot_118);
    bool have_124 = amd64_trace_read_task_guest(task, state_addr + 0x124, &slot_124, sizeof(slot_124));
    bool have_b0 = amd64_trace_read_task_u64(task, state_addr + 0xb0, &slot_b0);
    bool have_b8 = amd64_trace_read_task_u64(task, state_addr + 0xb8, &slot_b8);
    bool have_c0 = amd64_trace_read_task_u64(task, state_addr + 0xc0, &slot_c0);
    bool have_c8 = amd64_trace_read_task_u64(task, state_addr + 0xc8, &slot_c8);
    bool have_128 = amd64_trace_read_task_u64(task, state_addr + 0x128, &slot_128);

    printk("amd64 as state: image_base=%#llx state=%#llx op=%02x%02x mode=%#x flags=%#x extra=%#x state31=%#x%s%s%s%s\n",
           (unsigned long long) image_base,
           (unsigned long long) state_addr,
           state[5],
           state[4],
           state[6],
           state[8],
           state[0xb],
           state[0x31],
           have_f8 ? "" : " f8=?",
           have_fc ? "" : " fc=?",
           have_100 ? "" : " 100=?",
           have_124 ? "" : " 124=?");
    if (amd64_as_stderr_enabled()) {
        fprintf(stderr,
                "amd64 as state: image_base=%#llx state=%#llx op=%02x%02x mode=%#x flags=%#x extra=%#x state31=%#x%s%s%s%s\n",
                (unsigned long long) image_base,
                (unsigned long long) state_addr,
                state[5],
                state[4],
                state[6],
                state[8],
                state[0xb],
                state[0x31],
                have_f8 ? "" : " f8=?",
                have_fc ? "" : " fc=?",
                have_100 ? "" : " 100=?",
                have_124 ? "" : " 124=?");
    }
    if (have_f8 || have_fc || have_100 || have_124) {
        printk("amd64 as state ext: slot_f8=%#x slot_fc=%#x slot_100=%#x slot_124=%#x\n",
               have_f8 ? slot_f8 : 0,
               have_fc ? slot_fc : 0,
               have_100 ? slot_100 : 0,
               have_124 ? slot_124 : 0);
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr,
                    "amd64 as state ext: slot_f8=%#x slot_fc=%#x slot_100=%#x slot_124=%#x\n",
                    have_f8 ? slot_f8 : 0,
                    have_fc ? slot_fc : 0,
                    have_100 ? slot_100 : 0,
                    have_124 ? slot_124 : 0);
        }
    }
    if (have_104 || have_108 || have_10c || have_110 || have_114 || have_118) {
        printk("amd64 as state ext2: slot_104=%#x slot_108=%#x slot_10c=%#x slot_110=%#x slot_114=%#x slot_118=%#x\n",
               have_104 ? slot_104 : 0,
               have_108 ? slot_108 : 0,
               have_10c ? slot_10c : 0,
               have_110 ? slot_110 : 0,
               have_114 ? slot_114 : 0,
               have_118 ? slot_118 : 0);
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr,
                    "amd64 as state ext2: slot_104=%#x slot_108=%#x slot_10c=%#x slot_110=%#x slot_114=%#x slot_118=%#x\n",
                    have_104 ? slot_104 : 0,
                    have_108 ? slot_108 : 0,
                    have_10c ? slot_10c : 0,
                    have_110 ? slot_110 : 0,
                    have_114 ? slot_114 : 0,
                    have_118 ? slot_118 : 0);
        }
    }
    if (have_b0 || have_b8 || have_c0 || have_c8 || have_128) {
        printk("amd64 as ptrs: slot_b0=%#llx slot_b8=%#llx slot_c0=%#llx slot_c8=%#llx slot_128=%#llx\n",
               (unsigned long long) (have_b0 ? slot_b0 : 0),
               (unsigned long long) (have_b8 ? slot_b8 : 0),
               (unsigned long long) (have_c0 ? slot_c0 : 0),
               (unsigned long long) (have_c8 ? slot_c8 : 0),
               (unsigned long long) (have_128 ? slot_128 : 0));
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr,
                    "amd64 as ptrs: slot_b0=%#llx slot_b8=%#llx slot_c0=%#llx slot_c8=%#llx slot_128=%#llx\n",
                    (unsigned long long) (have_b0 ? slot_b0 : 0),
                    (unsigned long long) (have_b8 ? slot_b8 : 0),
                    (unsigned long long) (have_c0 ? slot_c0 : 0),
                    (unsigned long long) (have_c8 ? slot_c8 : 0),
                    (unsigned long long) (have_128 ? slot_128 : 0));
        }
    }

    for (unsigned i = 0; i < sizeof(state); i += 16) {
        printk("amd64 as state[%02x]: "
               "%02x %02x %02x %02x %02x %02x %02x %02x "
               "%02x %02x %02x %02x %02x %02x %02x %02x\n",
               i,
               state[i + 0], state[i + 1], state[i + 2], state[i + 3],
               state[i + 4], state[i + 5], state[i + 6], state[i + 7],
               state[i + 8], state[i + 9], state[i + 10], state[i + 11],
               state[i + 12], state[i + 13], state[i + 14], state[i + 15]);
    }

    for (unsigned i = 0; i <= 4; i++) {
        uint32_t slot_1c = 0, slot_48 = 0, slot_88 = 0, slot_9c = 0;
        uint64_t ptr = 0;
        bool have_1c = amd64_trace_read_task_u32(task, state_addr + 0x1c + (qword_t) i * 4, &slot_1c);
        bool have_48 = amd64_trace_read_task_u32(task, state_addr + 0x48 + (qword_t) i * 4, &slot_48);
        bool have_88 = amd64_trace_read_task_u32(task, state_addr + 0x88 + (qword_t) i * 4, &slot_88);
        bool have_9c = amd64_trace_read_task_u32(task, state_addr + 0x9c + (qword_t) i * 4, &slot_9c);
        bool have_ptr = amd64_trace_read_task_u64(task, state_addr + 0x60 + (qword_t) i * 8, &ptr);
        amd64_dump_as_descriptor_task(task, i, have_ptr ? ptr : 0,
                                      have_1c ? slot_1c : 0,
                                      have_48 ? slot_48 : 0,
                                      have_88 ? slot_88 : 0,
                                      have_9c ? slot_9c : 0);
    }
}

void dump_amd64_as_stack_task(const struct task *task) {
    if (task == NULL || task->abi != GUEST_ABI_AMD64 || strcmp(task->comm, "as") != 0)
        return;

    const struct cpu_state *cpu = &task->cpu;
    qword_t rsp = cpu->amd64_regs[amd64_rsp];
    qword_t rbp = cpu->amd64_regs[amd64_rbp];

    printk("amd64 as stack pid=%d rip=%#llx rsp=%#llx rbp=%#llx\n",
           task->pid,
           (unsigned long long) cpu->amd64_rip,
           (unsigned long long) rsp,
           (unsigned long long) rbp);
    if (amd64_as_stderr_enabled()) {
        fprintf(stderr, "amd64 as stack pid=%d rip=%#llx rsp=%#llx rbp=%#llx\n",
                task->pid,
                (unsigned long long) cpu->amd64_rip,
                (unsigned long long) rsp,
                (unsigned long long) rbp);
    }

    for (unsigned i = 0; i < 12; i++) {
        qword_t addr = rsp + (qword_t) i * 8;
        qword_t value = 0;
        if (amd64_trace_read_task_guest(task, addr, &value, sizeof(value))) {
            printk("as stack[%02u] addr=%#llx value=%#llx%s\n",
                   i,
                   (unsigned long long) addr,
                   (unsigned long long) value,
                   addr == rbp ? " <rbp>" : "");
            if (amd64_as_stderr_enabled()) {
                fprintf(stderr, "as stack[%02u] addr=%#llx value=%#llx%s\n",
                        i,
                        (unsigned long long) addr,
                        (unsigned long long) value,
                        addr == rbp ? " <rbp>" : "");
            }
        } else {
            printk("as stack[%02u] addr=%#llx unreadable%s\n",
                   i,
                   (unsigned long long) addr,
                   addr == rbp ? " <rbp>" : "");
            if (amd64_as_stderr_enabled()) {
                fprintf(stderr, "as stack[%02u] addr=%#llx unreadable%s\n",
                        i,
                        (unsigned long long) addr,
                        addr == rbp ? " <rbp>" : "");
            }
        }
    }

    qword_t frame = rbp;
    for (unsigned depth = 0; depth < 4; depth++) {
        qword_t next_rbp = 0;
        qword_t return_rip = 0;
        if (!amd64_trace_read_task_guest(task, frame, &next_rbp, sizeof(next_rbp)) ||
                !amd64_trace_read_task_guest(task, frame + 8, &return_rip, sizeof(return_rip))) {
            printk("as frame[%u] rbp=%#llx unreadable\n",
                   depth, (unsigned long long) frame);
            if (amd64_as_stderr_enabled())
                fprintf(stderr, "as frame[%u] rbp=%#llx unreadable\n",
                        depth, (unsigned long long) frame);
            break;
        }
        printk("as frame[%u] rbp=%#llx next=%#llx ret=%#llx\n",
               depth,
               (unsigned long long) frame,
               (unsigned long long) next_rbp,
               (unsigned long long) return_rip);
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr, "as frame[%u] rbp=%#llx next=%#llx ret=%#llx\n",
                    depth,
                    (unsigned long long) frame,
                    (unsigned long long) next_rbp,
                    (unsigned long long) return_rip);
        }
        if (next_rbp <= frame || (next_rbp & 7) != 0)
            break;
        frame = next_rbp;
    }
}

#define AMD64_XMM_COUNT ((unsigned) (sizeof(((struct cpu_state *) 0)->xmm) / sizeof(((struct cpu_state *) 0)->xmm[0])))

static inline qword_t amd64_cvtt_scalar_to_int(double value, bool wide) {
    if (isnan(value))
        return wide ? (qword_t) INT64_MIN : (qword_t) (uint32_t) INT32_MIN;
    if (wide) {
        if (value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return (qword_t) INT64_MIN;
        return (qword_t) (sqword_t) value;
    }
    if (value < (double) INT32_MIN || value >= 2147483648.0)
        return (qword_t) (uint32_t) INT32_MIN;
    return (qword_t) (uint32_t) (int32_t) value;
}

static inline void amd64_set_fp_compare_flags(struct cpu_state *cpu, int cmp_result, bool unordered) {
    cpu->of = 0;
    cpu->sf = 0;
    cpu->af = 0;
    cpu->af_ops = 0;
    if (unordered) {
        cpu->zf = 1;
        cpu->pf = 1;
        cpu->cf = 1;
    } else if (cmp_result < 0) {
        cpu->zf = 0;
        cpu->pf = 0;
        cpu->cf = 1;
    } else if (cmp_result == 0) {
        cpu->zf = 1;
        cpu->pf = 0;
        cpu->cf = 0;
    } else {
        cpu->zf = 0;
        cpu->pf = 0;
        cpu->cf = 0;
    }
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline bool amd64_trace_intersects_busybox_slot(qword_t guest_addr, unsigned size) {
    if (size == 0)
        return false;
    qword_t start = guest_addr;
    qword_t end = guest_addr + size;
    qword_t slot_start = AMD64_BUSYBOX_INIT_SLOT;
    qword_t slot_end = slot_start + AMD64_BUSYBOX_INIT_SLOT_SIZE;
    return start < slot_end && end > slot_start;
}

static inline void amd64_busybox_watch_addr(qword_t guest_addr) {
    if (guest_addr == 0)
        return;
    for (unsigned i = 0; i < AMD64_BUSYBOX_INIT_WATCH_COUNT; i++) {
        if (amd64_busybox_init_watch[i] == guest_addr)
            return;
    }
    amd64_busybox_init_watch[amd64_busybox_init_watch_next++ % AMD64_BUSYBOX_INIT_WATCH_COUNT] = guest_addr;
}

static inline bool amd64_trace_intersects_busybox_watch(qword_t guest_addr, unsigned size,
        qword_t *base_out, qword_t *offset_out) {
    if (size == 0)
        return false;
    qword_t start = guest_addr;
    qword_t end = guest_addr + size;
    for (unsigned i = 0; i < AMD64_BUSYBOX_INIT_WATCH_COUNT; i++) {
        qword_t base = amd64_busybox_init_watch[i];
        if (base == 0)
            continue;
        qword_t watch_end = base + AMD64_BUSYBOX_INIT_WATCH_SPAN;
        if (start < watch_end && end > base) {
            if (base_out != NULL)
                *base_out = base;
            if (offset_out != NULL)
                *offset_out = start - base;
            return true;
        }
    }
    return false;
}

static inline bool amd64_guest_addr_ok(qword_t guest_addr, unsigned size, guest_addr_t *addr_out) {
    if (!guest_abi_range_valid(GUEST_ABI_AMD64, guest_addr, size))
        return false;
    *addr_out = guest_addr;
    return true;
}

static int amd64_bad_transfer_target(struct cpu_state *cpu, struct tlb *tlb,
        qword_t from, qword_t target, const char *kind) {
    (void) tlb;
    qword_t rsp = cpu->amd64_regs[amd64_rsp];
    printk("[amd64-jit] bad-%s-target from=%#llx target=%#llx rsp=%#llx\n",
           kind,
           (unsigned long long) from,
           (unsigned long long) target,
           (unsigned long long) rsp);
    if (strcmp(kind, "ret") == 0 && rsp >= 8) {
        qword_t slot_addr = rsp - 8;
        uint8_t slot_bytes[8] = {};
        bool have_slot = amd64_mem_read_direct(slot_addr, slot_bytes, sizeof(slot_bytes));
        printk("[amd64-jit] bad-ret-target-generic slot-addr=%#llx slot=%s%02x %02x %02x %02x %02x %02x %02x %02x\n",
               (unsigned long long) slot_addr,
               have_slot ? "" : "unreadable ",
               slot_bytes[0], slot_bytes[1], slot_bytes[2], slot_bytes[3],
               slot_bytes[4], slot_bytes[5], slot_bytes[6], slot_bytes[7]);
        if (current != NULL) {
            amd64_dump_recent_suspects_for_stack_slot(current->pid, slot_addr,
                    "bad-ret-target-generic");
            amd64_dump_recent_suspects(current->pid, "bad-ret-target-generic");
        }
    }
    cpu->amd64_rip = target;
    return INT_GPF;
}

static inline int amd64_validate_transfer_target(struct cpu_state *cpu, struct tlb *tlb,
        qword_t from, qword_t target, const char *kind) {
    guest_addr_t checked_target;
    if (!amd64_guest_addr_ok(target, 1, &checked_target))
        return amd64_bad_transfer_target(cpu, tlb, from, target, kind);
    return INT_NONE;
}

static inline qword_t amd64_mask(unsigned size) {
    switch (size) {
    case 8: return 0xff;
    case 16: return 0xffff;
    case 32: return 0xffffffffu;
    case 64: return ~0ull;
    default: return 0;
    }
}

static inline qword_t amd64_sign_bit(unsigned size) {
    return 1ull << (size - 1);
}

static inline qword_t amd64_trunc(qword_t value, unsigned size) {
    return value & amd64_mask(size);
}

static inline qword_t amd64_rdtsc_value(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (qword_t) now.tv_sec * 1000000000ull + (qword_t) now.tv_nsec;
}

static inline sqword_t amd64_sign_extend(qword_t value, unsigned size) {
    qword_t masked = amd64_trunc(value, size);
    if ((masked & amd64_sign_bit(size)) == 0)
        return (sqword_t) masked;
    return (sqword_t) (masked | ~amd64_mask(size));
}

static inline void amd64_sync_legacy_regs(struct cpu_state *cpu) {
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

static inline void amd64_trace_suspicious_rsp_write(struct cpu_state *cpu,
        qword_t old_rsp, qword_t new_rsp, unsigned size) {
    if (new_rsp >= 0x1000)
        return;
    printk("amd64 rsp write: rip=%#llx old=%#llx new=%#llx size=%u\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_rsp,
           (unsigned long long) new_rsp,
           size);
}

static inline void amd64_trace_cargo_r12_write(struct cpu_state *cpu,
        qword_t old_value, qword_t new_value, unsigned size, qword_t value) {
    guest_addr_t guest_addr;
    bool current_ip_in_window;
    uint8_t insn_bytes[16] = {};
    bool have_bytes = false;

    if (amd64_cargo_r12_trace_count >= 32)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;

    current_ip_in_window = cpu->amd64_current_insn_rip >= AMD64_CARGO_R12_TRACE_WINDOW_START &&
                           cpu->amd64_current_insn_rip < AMD64_CARGO_R12_TRACE_WINDOW_END;
    if (!current_ip_in_window)
        return;

    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn_bytes), &guest_addr) &&
            current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, guest_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(insn_bytes, ptr, sizeof(insn_bytes));
            have_bytes = true;
        }
    }

    amd64_cargo_r12_trace_count++;
    printk("amd64 cargo r12 write: rip=%#llx old=%#llx new=%#llx size=%u value=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_value,
           (unsigned long long) new_value,
           size,
           (unsigned long long) value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi]);
    if (have_bytes) {
        printk("amd64 cargo r12 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
               insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7],
               insn_bytes[8], insn_bytes[9], insn_bytes[10], insn_bytes[11],
               insn_bytes[12], insn_bytes[13], insn_bytes[14], insn_bytes[15]);
    }
}

static inline void amd64_trace_cargo_rdx_write(struct cpu_state *cpu,
        qword_t old_value, qword_t new_value, unsigned size, qword_t value) {
    guest_addr_t guest_addr;
    bool current_ip_in_window;
    uint8_t insn_bytes[16] = {};
    bool have_bytes = false;

    if (amd64_cargo_rdx_trace_count >= 64)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;

    current_ip_in_window = cpu->amd64_current_insn_rip >= AMD64_CARGO_R12_TRACE_WINDOW_START &&
                           cpu->amd64_current_insn_rip < AMD64_CARGO_R12_TRACE_WINDOW_END;
    if (!current_ip_in_window)
        return;

    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn_bytes), &guest_addr) &&
            current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, guest_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(insn_bytes, ptr, sizeof(insn_bytes));
            have_bytes = true;
        }
    }

    amd64_cargo_rdx_trace_count++;
    printk("amd64 cargo rdx write: rip=%#llx old=%#llx new=%#llx size=%u value=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rsi=%#llx rdi=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_value,
           (unsigned long long) new_value,
           size,
           (unsigned long long) value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi]);
    if (have_bytes) {
        printk("amd64 cargo rdx bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
               insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7],
               insn_bytes[8], insn_bytes[9], insn_bytes[10], insn_bytes[11],
               insn_bytes[12], insn_bytes[13], insn_bytes[14], insn_bytes[15]);
    }

}

static inline void amd64_trace_cargo_rdi_write(struct cpu_state *cpu,
        qword_t old_value, qword_t new_value, unsigned size, qword_t value) {
    guest_addr_t guest_addr;
    bool current_ip_in_window;
    uint8_t insn_bytes[16] = {};
    bool have_bytes = false;

    if (amd64_cargo_rdi_trace_count >= 64)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;

    current_ip_in_window = cpu->amd64_current_insn_rip >= AMD64_CARGO_R12_TRACE_WINDOW_START &&
                           cpu->amd64_current_insn_rip < AMD64_CARGO_R12_TRACE_WINDOW_END;
    if (!current_ip_in_window)
        return;

    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn_bytes), &guest_addr) &&
            current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, guest_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(insn_bytes, ptr, sizeof(insn_bytes));
            have_bytes = true;
        }
    }

    amd64_cargo_rdi_trace_count++;
    printk("amd64 cargo rdi write: rip=%#llx old=%#llx new=%#llx size=%u value=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rsi=%#llx rdx=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_value,
           (unsigned long long) new_value,
           size,
           (unsigned long long) value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdx]);
    if (have_bytes) {
        printk("amd64 cargo rdi bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
               insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7],
               insn_bytes[8], insn_bytes[9], insn_bytes[10], insn_bytes[11],
               insn_bytes[12], insn_bytes[13], insn_bytes[14], insn_bytes[15]);
    }
}

static inline void amd64_trace_htop_r13_write(struct cpu_state *cpu,
        qword_t old_value, qword_t new_value, unsigned size, qword_t value) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (cpu->amd64_current_insn_rip != AMD64_HTOP_R13_CORRUPT_WRITE_RIP)
        return;

    uint8_t insn_bytes[8] = {};
    bool have_bytes = false;
    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) cpu->amd64_current_insn_rip, MEM_READ);
        if (ptr != NULL) {
            memcpy(insn_bytes, ptr, sizeof(insn_bytes));
            have_bytes = true;
        }
    }

    printk("amd64 htop r13 write: rip=%#llx old=%#llx new=%#llx size=%u value=%#llx rsp=%#llx rbp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_value,
           (unsigned long long) new_value,
           size,
           (unsigned long long) value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx]);
    printk("amd64 htop r13 regs: rsi=%#llx rdi=%#llx r8=%#llx r9=%#llx r10=%#llx r11=%#llx r12=%#llx r14=%#llx r15=%#llx%s%s\n",
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_r8],
           (unsigned long long) cpu->amd64_regs[amd64_r9],
           (unsigned long long) cpu->amd64_regs[amd64_r10],
           (unsigned long long) cpu->amd64_regs[amd64_r11],
           (unsigned long long) cpu->amd64_regs[amd64_r12],
           (unsigned long long) cpu->amd64_regs[amd64_r14],
           (unsigned long long) cpu->amd64_regs[amd64_r15],
           have_bytes ? " bytes=" : "",
           have_bytes ? "" : "");
    if (have_bytes) {
        printk("amd64 htop r13 bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
               insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
    }
}

static inline void amd64_trace_htop_r13_source(struct cpu_state *cpu, qword_t addr, qword_t value) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (cpu->amd64_current_insn_rip != AMD64_HTOP_R13_CORRUPT_WRITE_RIP)
        return;

    qword_t base = cpu->amd64_regs[amd64_rbx];
    uint8_t bytes[32] = {};
    bool have_bytes = false;
    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) base, MEM_READ);
        if (ptr != NULL) {
            memcpy(bytes, ptr, sizeof(bytes));
            have_bytes = true;
        }
    }

    printk("amd64 htop r13 src: rip=%#llx base=%#llx addr=%#llx value=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) base,
           (unsigned long long) addr,
           (unsigned long long) value);
    if (have_bytes) {
        printk("amd64 htop r13 mem: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        printk("amd64 htop r13 mem2: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[16], bytes[17], bytes[18], bytes[19], bytes[20], bytes[21], bytes[22], bytes[23],
               bytes[24], bytes[25], bytes[26], bytes[27], bytes[28], bytes[29], bytes[30], bytes[31]);
    }
}

static inline bool amd64_trace_in_htop_window(qword_t rip) {
    if (!amd64_htop_legacy_trace_enabled)
        return false;
    return rip >= AMD64_HTOP_TRACE_WINDOW_START && rip < AMD64_HTOP_TRACE_WINDOW_END;
}

static inline bool amd64_trace_intersects_watch_addr(qword_t guest_addr, unsigned size,
        qword_t watch_addr, unsigned watch_size) {
    if (size == 0 || watch_size == 0 || watch_addr == 0)
        return false;
    qword_t start = guest_addr;
    qword_t end = guest_addr + size;
    qword_t watch_end = watch_addr + watch_size;
    return start < watch_end && end > watch_addr;
}

static inline void amd64_trace_htop_window(struct cpu_state *cpu, struct tlb *tlb) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (!amd64_trace_in_htop_window(cpu->amd64_current_insn_rip))
        return;
    if (cpu->amd64_current_insn_rip == AMD64_HTOP_RBX_LOAD_RIP && cpu->amd64_regs[amd64_rdi] != 0)
        amd64_htop_watch_field_addr = cpu->amd64_regs[amd64_rdi] + AMD64_HTOP_RBX_FIELD_OFFSET;

    uint8_t bytes[16] = {};
    bool have_bytes = false;
    guest_addr_t insn_addr;
    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, bytes, sizeof(bytes))) {
        have_bytes = true;
    }

    printk("amd64 htop win: rip=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx r12=%#llx r13=%#llx r15=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_r12],
           (unsigned long long) cpu->amd64_regs[amd64_r13],
           (unsigned long long) cpu->amd64_regs[amd64_r15]);
    if (have_bytes) {
        printk("amd64 htop win bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline bool amd64_trace_in_cargo_pf_window(qword_t rip) {
    return rip >= AMD64_CARGO_PFWIN_WINDOW_START && rip < AMD64_CARGO_PFWIN_WINDOW_END;
}

static inline void amd64_trace_cargo_pf_window(struct cpu_state *cpu, struct tlb *tlb) {
    uint8_t bytes[16] = {};
    bool have_bytes = false;
    guest_addr_t insn_addr;

    if (current == NULL)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;
    if (!amd64_trace_in_cargo_pf_window(cpu->amd64_current_insn_rip))
        return;

    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, bytes, sizeof(bytes))) {
        have_bytes = true;
    }

    printk("amd64 cargo pfwin: rip=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx r8=%#llx r9=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_r8],
           (unsigned long long) cpu->amd64_regs[amd64_r9],
           (unsigned long long) cpu->amd64_regs[amd64_r12],
           (unsigned long long) cpu->amd64_regs[amd64_r13],
           (unsigned long long) cpu->amd64_regs[amd64_r14],
           (unsigned long long) cpu->amd64_regs[amd64_r15]);
    if (have_bytes) {
        printk("amd64 cargo pfwin bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline void amd64_trace_cargo_transfer(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip, qword_t target, const char *kind) {
    uint8_t bytes[16] = {};
    bool have_bytes = false;
    guest_addr_t insn_addr;

    if (amd64_cargo_xfer_trace_count >= 32)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;
    if (target != AMD64_CARGO_ENTRY_RIP)
        return;

    if (amd64_guest_addr_ok(saved_rip, sizeof(bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, bytes, sizeof(bytes))) {
        have_bytes = true;
    }

    amd64_cargo_xfer_trace_count++;
    printk("amd64 cargo xfer: kind=%s from=%#llx to=%#llx rsp=%#llx rdi=%#llx rsi=%#llx rdx=%#llx r15=%#llx\n",
           kind,
           (unsigned long long) saved_rip,
           (unsigned long long) target,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_r15]);
    if (have_bytes) {
        printk("amd64 cargo xfer bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline void amd64_trace_cargo_predecessor(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip) {
    uint8_t bytes[16] = {};
    bool have_bytes = false;
    guest_addr_t insn_addr;

    if (amd64_cargo_xfer_trace_count >= 1)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;
    if (saved_rip == AMD64_CARGO_ENTRY_RIP || cpu->amd64_rip != AMD64_CARGO_ENTRY_RIP)
        return;

    if (amd64_guest_addr_ok(saved_rip, sizeof(bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, bytes, sizeof(bytes))) {
        have_bytes = true;
    }

    amd64_cargo_xfer_trace_count++;
    printk("amd64 cargo prev: from=%#llx to=%#llx rsp=%#llx rdi=%#llx rsi=%#llx rdx=%#llx r15=%#llx\n",
           (unsigned long long) saved_rip,
           (unsigned long long) cpu->amd64_rip,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_r15]);
    if (have_bytes) {
        printk("amd64 cargo prev bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline void amd64_trace_cargo_start_call(struct cpu_state *cpu) {
    guest_addr_t stack_addr;
    guest_addr_t bytes_addr;
    uint8_t bytes[16] = {};
    bool have_bytes = false;
    qword_t popped = 0;
    qword_t next0 = 0;
    qword_t next1 = 0;

    if (amd64_cargo_start_call_trace_count >= 1)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;
    if (cpu->amd64_current_insn_rip != AMD64_CARGO_START_CALL_RIP)
        return;
    if (current->mem == NULL)
        return;

    if (cpu->amd64_current_insn_rip >= 5 &&
            amd64_guest_addr_ok(cpu->amd64_current_insn_rip - 5, sizeof(bytes), &bytes_addr)) {
        void *ptr = mem_ptr(current->mem, bytes_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(bytes, ptr, sizeof(bytes));
            have_bytes = true;
        }
    }

    if (amd64_guest_addr_ok(cpu->amd64_regs[amd64_rsp] - 8, sizeof(popped), &stack_addr)) {
        void *ptr = mem_ptr(current->mem, stack_addr, MEM_READ);
        if (ptr != NULL)
            memcpy(&popped, ptr, sizeof(popped));
    }
    if (amd64_guest_addr_ok(cpu->amd64_regs[amd64_rsp], sizeof(next0), &stack_addr)) {
        void *ptr = mem_ptr(current->mem, stack_addr, MEM_READ);
        if (ptr != NULL)
            memcpy(&next0, ptr, sizeof(next0));
    }
    if (amd64_guest_addr_ok(cpu->amd64_regs[amd64_rsp] + 8, sizeof(next1), &stack_addr)) {
        void *ptr = mem_ptr(current->mem, stack_addr, MEM_READ);
        if (ptr != NULL)
            memcpy(&next1, ptr, sizeof(next1));
    }

    amd64_cargo_start_call_trace_count++;
    printk("amd64 cargo startcall: rip=%#llx r9=%#llx rdi=%#llx rsp=%#llx popped=%#llx next0=%#llx next1=%#llx rsi=%#llx rdx=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) cpu->amd64_regs[amd64_r9],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) popped,
           (unsigned long long) next0,
           (unsigned long long) next1,
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdx]);
    if (have_bytes) {
        printk("amd64 cargo startcall bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3],
               bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11],
               bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline void amd64_trace_htop_store_history(struct cpu_state *cpu, qword_t watch_addr) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    unsigned total = cpu->amd64_store_trace_next;
    if (total > AMD64_STORE_TRACE_COUNT)
        total = AMD64_STORE_TRACE_COUNT;

    unsigned reported = 0;
    for (unsigned i = 0; i < total && reported < 6; i++) {
        unsigned seq = cpu->amd64_store_trace_next - 1 - i;
        struct amd64_store_trace entry =
                cpu->amd64_store_trace[seq % AMD64_STORE_TRACE_COUNT];
        if (entry.addr != watch_addr)
            continue;
        printk("amd64 htop rbx store%u: rip=%#llx opcode=%#x addr=%#llx value=%#llx\n",
               reported,
               (unsigned long long) entry.rip,
               entry.opcode,
               (unsigned long long) entry.addr,
               (unsigned long long) entry.value);
        reported++;
    }
}

static inline void amd64_trace_htop_rbx_source(struct cpu_state *cpu, qword_t base, qword_t addr, qword_t value) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (cpu->amd64_current_insn_rip != AMD64_HTOP_RBX_LOAD_RIP)
        return;

    uint8_t bytes[64] = {};
    bool have_bytes = false;
    qword_t dump_addr = addr >= 0x10 ? addr - 0x10 : addr;
    uint8_t pointee[128] = {};
    bool have_pointee = false;
    qword_t pointee_addr = value >= 0x50 ? value - 0x50 : value;
    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) dump_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(bytes, ptr, sizeof(bytes));
            have_bytes = true;
        }
        if (value != 0) {
            ptr = mem_ptr(current->mem, (addr_t) pointee_addr, MEM_READ);
            if (ptr != NULL) {
                memcpy(pointee, ptr, sizeof(pointee));
                have_pointee = true;
            }
        }
    }

    printk("amd64 htop rbx src: rip=%#llx base=%#llx addr=%#llx value=%#llx rsp=%#llx r12=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) base,
           (unsigned long long) addr,
           (unsigned long long) value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_r12]);
    amd64_trace_htop_store_history(cpu, addr);
    if (have_bytes) {
        printk("amd64 htop rbx obj0: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        printk("amd64 htop rbx obj1: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[16], bytes[17], bytes[18], bytes[19], bytes[20], bytes[21], bytes[22], bytes[23],
               bytes[24], bytes[25], bytes[26], bytes[27], bytes[28], bytes[29], bytes[30], bytes[31]);
        printk("amd64 htop rbx obj2: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[32], bytes[33], bytes[34], bytes[35], bytes[36], bytes[37], bytes[38], bytes[39],
               bytes[40], bytes[41], bytes[42], bytes[43], bytes[44], bytes[45], bytes[46], bytes[47]);
        printk("amd64 htop rbx obj3: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[48], bytes[49], bytes[50], bytes[51], bytes[52], bytes[53], bytes[54], bytes[55],
               bytes[56], bytes[57], bytes[58], bytes[59], bytes[60], bytes[61], bytes[62], bytes[63]);
    }
    if (have_pointee) {
        printk("amd64 htop rbx mem0: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[0], pointee[1], pointee[2], pointee[3], pointee[4], pointee[5], pointee[6], pointee[7],
               pointee[8], pointee[9], pointee[10], pointee[11], pointee[12], pointee[13], pointee[14], pointee[15]);
        printk("amd64 htop rbx mem1: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[16], pointee[17], pointee[18], pointee[19], pointee[20], pointee[21], pointee[22], pointee[23],
               pointee[24], pointee[25], pointee[26], pointee[27], pointee[28], pointee[29], pointee[30], pointee[31]);
        printk("amd64 htop rbx mem2: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[32], pointee[33], pointee[34], pointee[35], pointee[36], pointee[37], pointee[38], pointee[39],
               pointee[40], pointee[41], pointee[42], pointee[43], pointee[44], pointee[45], pointee[46], pointee[47]);
        printk("amd64 htop rbx mem3: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[48], pointee[49], pointee[50], pointee[51], pointee[52], pointee[53], pointee[54], pointee[55],
               pointee[56], pointee[57], pointee[58], pointee[59], pointee[60], pointee[61], pointee[62], pointee[63]);
        printk("amd64 htop rbx mem4: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[64], pointee[65], pointee[66], pointee[67], pointee[68], pointee[69], pointee[70], pointee[71],
               pointee[72], pointee[73], pointee[74], pointee[75], pointee[76], pointee[77], pointee[78], pointee[79]);
        printk("amd64 htop rbx mem5: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[80], pointee[81], pointee[82], pointee[83], pointee[84], pointee[85], pointee[86], pointee[87],
               pointee[88], pointee[89], pointee[90], pointee[91], pointee[92], pointee[93], pointee[94], pointee[95]);
        printk("amd64 htop rbx mem6: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[96], pointee[97], pointee[98], pointee[99], pointee[100], pointee[101], pointee[102], pointee[103],
               pointee[104], pointee[105], pointee[106], pointee[107], pointee[108], pointee[109], pointee[110], pointee[111]);
        printk("amd64 htop rbx mem7: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[112], pointee[113], pointee[114], pointee[115], pointee[116], pointee[117], pointee[118], pointee[119],
               pointee[120], pointee[121], pointee[122], pointee[123], pointee[124], pointee[125], pointee[126], pointee[127]);
    }
}

static inline void amd64_trace_htop_rbx_base(struct cpu_state *cpu, qword_t old_value,
        qword_t new_value, unsigned size, qword_t raw_value) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (new_value != AMD64_HTOP_R13_CORRUPT_BLOCK_BASE)
        return;

    uint8_t bytes[16] = {};
    bool have_bytes = false;
    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) cpu->amd64_current_insn_rip, MEM_READ);
        if (ptr != NULL) {
            memcpy(bytes, ptr, sizeof(bytes));
            have_bytes = true;
        }
    }

    printk("amd64 htop rbx base: rip=%#llx old=%#llx new=%#llx size=%u value=%#llx rsp=%#llx rax=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_value,
           (unsigned long long) new_value,
           size,
           (unsigned long long) raw_value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi]);
    if (have_bytes) {
        printk("amd64 htop rbx bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline bool amd64_trace_intersects_htop_r13_block(qword_t guest_addr, unsigned size) {
    qword_t start = guest_addr;
    qword_t end = guest_addr + size;
    qword_t watch_start = AMD64_HTOP_R13_CORRUPT_BLOCK_BASE;
    qword_t watch_end = watch_start + AMD64_HTOP_R13_CORRUPT_BLOCK_SIZE;
    return start < watch_end && end > watch_start;
}

static inline void amd64_trace_htop_field_write(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, const void *value, unsigned size) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (!amd64_trace_intersects_watch_addr(guest_addr, size, amd64_htop_watch_field_addr, AMD64_HTOP_RBX_FIELD_SIZE))
        return;

    qword_t observed = 0;
    uint8_t field[AMD64_HTOP_RBX_FIELD_SIZE] = {};
    bool have_field = false;
    memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));

    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) amd64_htop_watch_field_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(field, ptr, sizeof(field));
            have_field = true;
        }
    }

    if (cpu->amd64_current_insn_rip == AMD64_HTOP_FIELD_FILL_RIP) {
        uint8_t insn[16] = {};
        bool have_insn = false;
        guest_addr_t insn_addr;
        if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn), &insn_addr) &&
                tlb_read(tlb, insn_addr, insn, sizeof(insn))) {
            have_insn = true;
        }
        printk("amd64 htop fill: rip=%#llx next=%#llx addr=%#llx size=%u rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n",
               (unsigned long long) cpu->amd64_current_insn_rip,
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) guest_addr,
               size,
               (unsigned long long) cpu->amd64_regs[amd64_rsp],
               (unsigned long long) cpu->amd64_regs[amd64_rax],
               (unsigned long long) cpu->amd64_regs[amd64_rbx],
               (unsigned long long) cpu->amd64_regs[amd64_rcx],
               (unsigned long long) cpu->amd64_regs[amd64_rdx],
               (unsigned long long) cpu->amd64_regs[amd64_rsi],
               (unsigned long long) cpu->amd64_regs[amd64_rdi],
               (unsigned long long) cpu->amd64_regs[amd64_r12],
               (unsigned long long) cpu->amd64_regs[amd64_r13],
               (unsigned long long) cpu->amd64_regs[amd64_r14],
               (unsigned long long) cpu->amd64_regs[amd64_r15]);
        if (have_insn) {
            printk("amd64 htop fill bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   insn[0], insn[1], insn[2], insn[3], insn[4], insn[5], insn[6], insn[7],
                   insn[8], insn[9], insn[10], insn[11], insn[12], insn[13], insn[14], insn[15]);
        }
    }

    printk("amd64 htop field write: rip=%#llx next=%#llx watch=%#llx addr=%#llx size=%u value=%#llx rsp=%#llx rdi=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) cpu->amd64_rip,
           (unsigned long long) amd64_htop_watch_field_addr,
           (unsigned long long) guest_addr,
           size,
           (unsigned long long) observed,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rdi]);
    if (have_field) {
        printk("amd64 htop field bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               field[0], field[1], field[2], field[3], field[4], field[5], field[6], field[7]);
    }
}

static inline void amd64_trace_htop_r13_block_write(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, const void *value, unsigned size) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (!amd64_trace_intersects_htop_r13_block(guest_addr, size))
        return;

    qword_t observed = 0;
    uint8_t insn_bytes[8] = {};
    bool have_insn = false;
    uint8_t block[AMD64_HTOP_R13_CORRUPT_BLOCK_SIZE] = {};
    bool have_block = false;
    memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));

    guest_addr_t insn_addr;
    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn_bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, insn_bytes, sizeof(insn_bytes))) {
        have_insn = true;
    }
    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) AMD64_HTOP_R13_CORRUPT_BLOCK_BASE, MEM_READ);
        if (ptr != NULL) {
            memcpy(block, ptr, sizeof(block));
            have_block = true;
        }
    }

    printk("amd64 htop block write: rip=%#llx next=%#llx addr=%#llx size=%u value=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx%s%s\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) cpu->amd64_rip,
           (unsigned long long) guest_addr,
           size,
           (unsigned long long) observed,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           have_insn ? " bytes=" : "",
           have_insn ? "" : "");
    if (have_insn) {
        printk("amd64 htop block bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
               insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
    }
    if (have_block) {
        printk("amd64 htop block mem: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               block[0], block[1], block[2], block[3], block[4], block[5], block[6], block[7],
               block[8], block[9], block[10], block[11], block[12], block[13], block[14], block[15]);
        printk("amd64 htop block mem2: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               block[16], block[17], block[18], block[19], block[20], block[21], block[22], block[23],
               block[24], block[25], block[26], block[27], block[28], block[29], block[30], block[31]);
    }
}

static inline qword_t amd64_reg_get(const struct cpu_state *cpu, unsigned reg, unsigned size) {
    qword_t value = cpu->amd64_regs[reg & 0xf];
    switch (size) {
    case 8: return value & 0xff;
    case 16: return value & 0xffff;
    case 32: return (uint32_t) value;
    case 64: return value;
    default: return value;
    }
}

static inline qword_t amd64_reg_get_encoded8(const struct cpu_state *cpu, unsigned reg, bool rex_present) {
    reg &= 0xf;
    if (!rex_present && reg >= 4 && reg < 8)
        return (cpu->amd64_regs[reg - 4] >> 8) & 0xff;
    return amd64_reg_get(cpu, reg, 8);
}

static inline void amd64_reg_set_encoded8(struct cpu_state *cpu, unsigned reg, bool rex_present, qword_t value) {
    reg &= 0xf;
    if (!rex_present && reg >= 4 && reg < 8) {
        unsigned base = reg - 4;
        qword_t old_value = cpu->amd64_regs[base];
        cpu->amd64_regs[base] = (cpu->amd64_regs[base] & ~0xff00ull) | ((value & 0xff) << 8);
        if (base == amd64_r13)
            amd64_trace_htop_r13_write(cpu, old_value, cpu->amd64_regs[base], 8, value & 0xff);
        return;
    }
    qword_t old_value = cpu->amd64_regs[reg];
    cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffull) | (value & 0xff);
    if (reg == amd64_r13)
        amd64_trace_htop_r13_write(cpu, old_value, cpu->amd64_regs[reg], 8, value & 0xff);
    if (reg == amd64_rbx)
        amd64_trace_htop_rbx_base(cpu, old_value, cpu->amd64_regs[reg], 8, value & 0xff);
}

static inline void amd64_reg_set(struct cpu_state *cpu, unsigned reg, unsigned size, qword_t value) {
    reg &= 0xf;
    qword_t old_value = cpu->amd64_regs[reg];
    switch (size) {
    case 8:
        cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffull) | (value & 0xff);
        break;
    case 16:
        cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffffull) | (value & 0xffff);
        break;
    case 32:
        cpu->amd64_regs[reg] = (uint32_t) value;
        break;
    case 64:
        cpu->amd64_regs[reg] = value;
        break;
    default:
        break;
    }
    if (reg == amd64_r13)
        amd64_trace_htop_r13_write(cpu, old_value, cpu->amd64_regs[reg], size, value);
    if (reg == amd64_rbx)
        amd64_trace_htop_rbx_base(cpu, old_value, cpu->amd64_regs[reg], size, value);
    if (reg == amd64_rsp)
        amd64_trace_suspicious_rsp_write(cpu, old_value, cpu->amd64_regs[reg], size);
    if (reg == amd64_rdx)
        amd64_trace_cargo_rdx_write(cpu, old_value, cpu->amd64_regs[reg], size, value);
    if (reg == amd64_rdi)
        amd64_trace_cargo_rdi_write(cpu, old_value, cpu->amd64_regs[reg], size, value);
    if (reg == amd64_r12)
        amd64_trace_cargo_r12_write(cpu, old_value, cpu->amd64_regs[reg], size, value);
}

static inline void amd64_set_logic_flags(struct cpu_state *cpu, qword_t result, unsigned size) {
    qword_t masked = amd64_trunc(result, size);
    cpu->cf = 0;
    cpu->of = 0;
    cpu->af = 0;
    cpu->af_ops = 0;
    cpu->zf = masked == 0;
    cpu->sf = (masked & amd64_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_add_flags(struct cpu_state *cpu, qword_t lhs, qword_t rhs, qword_t result, unsigned size) {
    qword_t mask = amd64_mask(size);
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t rhs_masked = amd64_trunc(rhs, size);
    qword_t res_masked = amd64_trunc(result, size);
    cpu->cf = size == 64 ? res_masked < lhs_masked : ((lhs_masked + rhs_masked) & ~mask) != 0;
    cpu->of = ((~(lhs_masked ^ rhs_masked) & (lhs_masked ^ res_masked)) & amd64_sign_bit(size)) != 0;
    cpu->af = ((lhs_masked ^ rhs_masked ^ res_masked) >> 4) & 1;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & amd64_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_sub_flags(struct cpu_state *cpu, qword_t lhs, qword_t rhs, qword_t result, unsigned size) {
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t rhs_masked = amd64_trunc(rhs, size);
    qword_t res_masked = amd64_trunc(result, size);
    cpu->cf = lhs_masked < rhs_masked;
    cpu->of = (((lhs_masked ^ rhs_masked) & (lhs_masked ^ res_masked)) & amd64_sign_bit(size)) != 0;
    cpu->af = ((lhs_masked ^ rhs_masked ^ res_masked) >> 4) & 1;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & amd64_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_adc_flags(struct cpu_state *cpu, qword_t lhs, qword_t rhs,
        unsigned carry_in, qword_t result, unsigned size) {
    qword_t mask = amd64_mask(size);
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t rhs_masked = amd64_trunc(rhs, size);
    qword_t rhs_with_carry = amd64_trunc(rhs_masked + carry_in, size);
    qword_t res_masked = amd64_trunc(result, size);
    __uint128_t full = (__uint128_t) lhs_masked + rhs_masked + carry_in;
    cpu->cf = size == 64 ? (full >> 64) != 0 : full > mask;
    cpu->of = ((~(lhs_masked ^ rhs_with_carry) & (lhs_masked ^ res_masked)) & amd64_sign_bit(size)) != 0;
    cpu->af = ((lhs_masked ^ rhs_with_carry ^ res_masked) >> 4) & 1;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & amd64_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_sbb_flags(struct cpu_state *cpu, qword_t lhs, qword_t rhs,
        unsigned carry_in, qword_t result, unsigned size) {
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t rhs_masked = amd64_trunc(rhs, size);
    qword_t rhs_with_carry = amd64_trunc(rhs_masked + carry_in, size);
    qword_t res_masked = amd64_trunc(result, size);
    __uint128_t subtrahend = (__uint128_t) rhs_masked + carry_in;
    cpu->cf = (__uint128_t) lhs_masked < subtrahend;
    cpu->of = (((lhs_masked ^ rhs_with_carry) & (lhs_masked ^ res_masked)) & amd64_sign_bit(size)) != 0;
    cpu->af = ((lhs_masked ^ rhs_with_carry ^ res_masked) >> 4) & 1;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & amd64_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_mul_flags(struct cpu_state *cpu, bool overflow) {
    cpu->cf = overflow;
    cpu->of = overflow;
    collapse_flags(cpu);
}

static inline void amd64_set_shift_flags(struct cpu_state *cpu, qword_t lhs, qword_t result,
        unsigned size, unsigned count, unsigned subop) {
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t res_masked = amd64_trunc(result, size);
    qword_t sign = amd64_sign_bit(size);
    cpu->cf = 0;
    cpu->of = 0;
    if (count != 0) {
        switch (subop) {
        case 4:
            cpu->cf = (lhs_masked >> (size - count)) & 1;
            if (count == 1)
                cpu->of = ((res_masked & sign) != 0) ^ cpu->cf;
            break;
        case 5:
            cpu->cf = (lhs_masked >> (count - 1)) & 1;
            if (count == 1)
                cpu->of = (lhs_masked & sign) != 0;
            break;
        case 7:
            cpu->cf = (lhs_masked >> (count - 1)) & 1;
            if (count == 1)
                cpu->of = 0;
            break;
        }
    }
    cpu->af = 0;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & sign) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_double_shift_flags(struct cpu_state *cpu, qword_t lhs, qword_t result,
        unsigned size, unsigned count, bool left) {
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t res_masked = amd64_trunc(result, size);
    qword_t sign = amd64_sign_bit(size);
    cpu->cf = 0;
    cpu->of = 0;
    if (count != 0) {
        if (left) {
            cpu->cf = (lhs_masked >> (size - count)) & 1;
            if (count == 1)
                cpu->of = ((res_masked & sign) != 0) ^ cpu->cf;
        } else {
            cpu->cf = (lhs_masked >> (count - 1)) & 1;
            if (count == 1)
                cpu->of = (lhs_masked & sign) != 0;
        }
    }
    cpu->af = 0;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & sign) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline qword_t amd64_rotate_value(qword_t value, unsigned size, unsigned count, unsigned subop) {
    qword_t masked = amd64_trunc(value, size);
    unsigned effective = count % size;
    if (effective == 0)
        return masked;
    if (subop == 0) {
        return amd64_trunc((masked << effective) | (masked >> (size - effective)), size);
    } else {
        return amd64_trunc((masked >> effective) | (masked << (size - effective)), size);
    }
}

static inline void amd64_set_rotate_flags(struct cpu_state *cpu, qword_t result,
        unsigned size, unsigned count, unsigned subop) {
    unsigned effective = count % size;
    if (effective == 0)
        return;
    if (subop == 0) {
        cpu->cf = result & 1;
        if (effective == 1)
            cpu->of = cpu->cf ^ ((amd64_trunc(result, size) >> (size - 1)) & 1);
    } else {
        cpu->cf = (amd64_trunc(result, size) >> (size - 1)) & 1;
        if (effective == 1)
            cpu->of = cpu->cf ^ (result & 1);
    }
    cpu->cf_bit = cpu->cf;
    cpu->of_bit = cpu->of;
}

static inline unsigned amd64_rotate_carry_count(unsigned size, unsigned count) {
    if (size == 8 || size == 16)
        return count % (size + 1);
    return count;
}

static inline qword_t amd64_rotate_carry_value(struct cpu_state *cpu, qword_t value,
        unsigned size, unsigned count, unsigned subop) {
    qword_t result = amd64_trunc(value, size);
    qword_t sign = amd64_sign_bit(size);
    qword_t mask = size == 64 ? ~(qword_t) 0 : (((qword_t) 1 << size) - 1);
    unsigned effective = amd64_rotate_carry_count(size, count);
    bool old_cf = cpu->cf != 0;

    for (unsigned i = 0; i < effective; i++) {
        bool new_cf;
        if (subop == 2) {
            new_cf = (result & sign) != 0;
            result = ((result << 1) & mask) | (old_cf ? 1 : 0);
        } else {
            new_cf = (result & 1) != 0;
            result = (result >> 1) | (old_cf ? sign : 0);
        }
        old_cf = new_cf;
    }

    if (effective != 0) {
        cpu->cf = old_cf ? 1 : 0;
        cpu->cf_bit = cpu->cf;
        if (effective == 1) {
            if (subop == 2)
                cpu->of = (((result & sign) != 0) ^ (cpu->cf != 0)) ? 1 : 0;
            else
                cpu->of = (((result & sign) != 0) ^
                        ((result & (sign >> 1)) != 0)) ? 1 : 0;
            cpu->of_bit = cpu->of;
        }
    }
    return result;
}

static inline bool amd64_fetch(struct cpu_state *cpu, struct tlb *tlb, void *out, unsigned size) {
    guest_addr_t addr;
    if (!amd64_guest_addr_ok(cpu->amd64_rip, size, &addr)) {
        cpu->segfault_addr = cpu->amd64_rip;
        cpu->segfault_was_write = false;
        return false;
    }
    if (!tlb_read(tlb, addr, out, size)) {
        cpu->segfault_addr = addr;
        cpu->segfault_was_write = false;
        return false;
    }
    cpu->amd64_rip += size;
    return true;
}

static inline bool amd64_fetch_u8(struct cpu_state *cpu, struct tlb *tlb, byte_t *out) {
    return amd64_fetch(cpu, tlb, out, sizeof(*out));
}

static inline bool amd64_fetch_u32(struct cpu_state *cpu, struct tlb *tlb, uint32_t *out) {
    return amd64_fetch(cpu, tlb, out, sizeof(*out));
}

static inline bool amd64_fetch_u64(struct cpu_state *cpu, struct tlb *tlb, uint64_t *out) {
    return amd64_fetch(cpu, tlb, out, sizeof(*out));
}

static inline bool amd64_fetch_accum_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned size, bool sign_extend_imm32, qword_t *value) {
    if (size == 8) {
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            return false;
        *value = imm8;
        return true;
    }
    if (size == 16) {
        uint16_t imm16;
        if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
            return false;
        *value = imm16;
        return true;
    }
    uint32_t imm32;
    if (!amd64_fetch_u32(cpu, tlb, &imm32))
        return false;
    *value = size == 64 && sign_extend_imm32 ? (qword_t) (sqword_t) (int32_t) imm32 : imm32;
    return true;
}

static inline bool amd64_verbose_boot_trace_enabled(void) {
    return false;
}

static inline bool amd64_mem_read(struct cpu_state *cpu, struct tlb *tlb, qword_t guest_addr, void *out, unsigned size) {
    guest_addr_t addr;
    if (!amd64_guest_addr_ok(guest_addr, size, &addr)) {
        cpu->segfault_addr = guest_addr;
        cpu->segfault_was_write = false;
        return false;
    }
    if (!tlb_read(tlb, addr, out, size)) {
        cpu->segfault_addr = addr;
        cpu->segfault_was_write = false;
        return false;
    }
    if (amd64_verbose_boot_trace_enabled() && amd64_trace_intersects_busybox_slot(guest_addr, size)) {
        qword_t observed = 0;
        memcpy(&observed, out, size < sizeof(observed) ? size : sizeof(observed));
        printk("amd64 slot read: rip=%#llx addr=%#llx size=%u value=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) guest_addr,
               size,
               (unsigned long long) observed);
    }
    return true;
}

static inline bool amd64_mem_write(struct cpu_state *cpu, struct tlb *tlb, qword_t guest_addr, const void *value, unsigned size) {
    guest_addr_t addr;
    if (!amd64_guest_addr_ok(guest_addr, size, &addr)) {
        cpu->segfault_addr = guest_addr;
        cpu->segfault_was_write = true;
        return false;
    }
    if (!tlb_write(tlb, addr, value, size)) {
        cpu->segfault_addr = addr;
        cpu->segfault_was_write = true;
        return false;
    }
    amd64_trace_cc1_slot_write_probe(cpu, guest_addr, value, size);
    amd64_trace_htop_field_write(cpu, tlb, guest_addr, value, size);
    amd64_trace_htop_r13_block_write(cpu, tlb, guest_addr, value, size);
    amd64_trace_as_state_write(cpu, guest_addr, value, size);
    if (amd64_verbose_boot_trace_enabled() && amd64_trace_intersects_busybox_slot(guest_addr, size)) {
        qword_t observed = 0;
        memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));
        printk("amd64 slot write: rip=%#llx addr=%#llx size=%u value=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) guest_addr,
               size,
               (unsigned long long) observed);
    }
    qword_t watch_base = 0, watch_offset = 0;
    if (amd64_verbose_boot_trace_enabled() &&
            amd64_trace_intersects_busybox_watch(guest_addr, size, &watch_base, &watch_offset)) {
        qword_t observed = 0;
        uint8_t insn_bytes[8] = {};
        bool have_bytes = false;
        memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));
        guest_addr_t insn_addr;
        if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn_bytes), &insn_addr) &&
                tlb_read(tlb, insn_addr, insn_bytes, sizeof(insn_bytes))) {
            have_bytes = true;
        }
        printk("amd64 init write: rip=%#llx next=%#llx base=%#llx addr=%#llx off=%#llx size=%u value=%#llx%s%s\n",
               (unsigned long long) cpu->amd64_current_insn_rip,
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) watch_base,
               (unsigned long long) guest_addr,
               (unsigned long long) watch_offset,
               size,
               (unsigned long long) observed,
               have_bytes ? " bytes=" : "",
               have_bytes ? "" : "");
        if (have_bytes) {
            printk("amd64 init write bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
                   insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
        }
        if (cpu->amd64_current_insn_rip == AMD64_BUSYBOX_INIT_CORRUPT_WRITE_RIP) {
            printk("amd64 init write regs: rax=%#llx rsp=%#llx rbp=%#llx r8=%#llx rcx=%#llx rdx=%#llx rsi=%#llx\n",
                   (unsigned long long) cpu->amd64_regs[amd64_rax],
                   (unsigned long long) cpu->amd64_regs[amd64_rsp],
                   (unsigned long long) cpu->amd64_regs[amd64_rbp],
                   (unsigned long long) cpu->amd64_regs[amd64_r8],
                   (unsigned long long) cpu->amd64_regs[amd64_rcx],
                   (unsigned long long) cpu->amd64_regs[amd64_rdx],
                   (unsigned long long) cpu->amd64_regs[amd64_rsi]);
        }
    }
    return true;
}

static inline bool amd64_push(struct cpu_state *cpu, struct tlb *tlb, qword_t value) {
    qword_t old_rsp = cpu->amd64_regs[amd64_rsp];
    qword_t rsp = old_rsp - sizeof(value);
    if (!amd64_mem_write(cpu, tlb, rsp, &value, sizeof(value)))
        return false;
    cpu->amd64_regs[amd64_rsp] = rsp;
    amd64_trace_suspicious_rsp_write(cpu, old_rsp, rsp, 64);
    amd64_trace_as_stack(amd64_as_stack_push, 64, old_rsp, rsp, value);
    return true;
}

static inline bool amd64_push_size(struct cpu_state *cpu, struct tlb *tlb, unsigned size,
        qword_t value) {
    qword_t old_rsp = cpu->amd64_regs[amd64_rsp];

    switch (size) {
    case 16: {
        uint16_t tmp = (uint16_t) value;
        qword_t rsp = old_rsp - sizeof(tmp);
        if (!amd64_mem_write(cpu, tlb, rsp, &tmp, sizeof(tmp)))
            return false;
        cpu->amd64_regs[amd64_rsp] = rsp;
        amd64_trace_suspicious_rsp_write(cpu, old_rsp, rsp, size);
        amd64_trace_as_stack(amd64_as_stack_push, size, old_rsp, rsp, value);
        return true;
    }
    case 64:
        return amd64_push(cpu, tlb, value);
    default:
        return false;
    }
}

static inline bool amd64_read_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t *value);
static inline bool amd64_write_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t value);

static inline int amd64_grp3_muldiv(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size) {
    qword_t src;
    if (!amd64_read_rm(cpu, tlb, modrm, fs_prefix, size, &src))
        return INT_PF;

    switch (modrm->reg) {
    case 2: {
        qword_t result = amd64_trunc(~src, size);
        if (!amd64_write_rm(cpu, tlb, modrm, fs_prefix, size, result))
            return INT_PF;
        return INT_NONE;
    }
    case 3: {
        qword_t result = amd64_trunc(0 - src, size);
        if (!amd64_write_rm(cpu, tlb, modrm, fs_prefix, size, result))
            return INT_PF;
        amd64_set_sub_flags(cpu, 0, src, result, size);
        return INT_NONE;
    }
    case 4:
        switch (size) {
        case 8: {
            uint16_t product = (uint8_t) amd64_reg_get(cpu, amd64_rax, 8) * (uint8_t) src;
            amd64_reg_set(cpu, amd64_rax, 16, product);
            amd64_set_mul_flags(cpu, (product >> 8) != 0);
            return INT_NONE;
        }
        case 16: {
            uint32_t product = (uint16_t) amd64_reg_get(cpu, amd64_rax, 16) * (uint16_t) src;
            amd64_reg_set(cpu, amd64_rax, 16, product);
            amd64_reg_set(cpu, amd64_rdx, 16, product >> 16);
            amd64_set_mul_flags(cpu, (product >> 16) != 0);
            return INT_NONE;
        }
        case 32: {
            uint64_t product = (uint32_t) amd64_reg_get(cpu, amd64_rax, 32) * (uint32_t) src;
            amd64_reg_set(cpu, amd64_rax, 32, product);
            amd64_reg_set(cpu, amd64_rdx, 32, product >> 32);
            amd64_set_mul_flags(cpu, (product >> 32) != 0);
            return INT_NONE;
        }
        case 64: {
            __uint128_t product = (__uint128_t) amd64_reg_get(cpu, amd64_rax, 64) * (__uint128_t) src;
            amd64_reg_set(cpu, amd64_rax, 64, (uint64_t) product);
            amd64_reg_set(cpu, amd64_rdx, 64, (uint64_t) (product >> 64));
            amd64_set_mul_flags(cpu, (product >> 64) != 0);
            return INT_NONE;
        }
        default:
            return INT_UNDEFINED;
        }
    case 5:
        switch (size) {
        case 8: {
            int16_t product = (int8_t) amd64_reg_get(cpu, amd64_rax, 8) * (int8_t) src;
            amd64_reg_set(cpu, amd64_rax, 16, (uint16_t) product);
            amd64_set_mul_flags(cpu, product != (int16_t) (int8_t) product);
            return INT_NONE;
        }
        case 16: {
            int32_t product = (int16_t) amd64_reg_get(cpu, amd64_rax, 16) * (int16_t) src;
            amd64_reg_set(cpu, amd64_rax, 16, (uint16_t) product);
            amd64_reg_set(cpu, amd64_rdx, 16, (uint16_t) ((uint32_t) product >> 16));
            amd64_set_mul_flags(cpu, product != (int32_t) (int16_t) product);
            return INT_NONE;
        }
        case 32: {
            int64_t product = (int32_t) amd64_reg_get(cpu, amd64_rax, 32) * (int32_t) src;
            amd64_reg_set(cpu, amd64_rax, 32, (uint32_t) product);
            amd64_reg_set(cpu, amd64_rdx, 32, (uint32_t) ((uint64_t) product >> 32));
            amd64_set_mul_flags(cpu, product != (int64_t) (int32_t) product);
            return INT_NONE;
        }
        case 64: {
            __int128_t product = (__int128_t) (sqword_t) amd64_reg_get(cpu, amd64_rax, 64) *
                    (__int128_t) (sqword_t) src;
            amd64_reg_set(cpu, amd64_rax, 64, (uint64_t) product);
            amd64_reg_set(cpu, amd64_rdx, 64, (uint64_t) (((__uint128_t) product) >> 64));
            amd64_set_mul_flags(cpu, product != (__int128_t) (sqword_t) (uint64_t) product);
            return INT_NONE;
        }
        default:
            return INT_UNDEFINED;
        }
    case 6:
        switch (size) {
        case 8: {
            uint8_t divisor = (uint8_t) src;
            uint16_t dividend = (uint16_t) amd64_reg_get(cpu, amd64_rax, 16);
            if (divisor == 0)
                return INT_DIV;
            uint16_t quotient = dividend / divisor;
            uint16_t remainder = dividend % divisor;
            if (quotient > 0xff)
                return INT_DIV;
            amd64_reg_set_encoded8(cpu, 0, true, quotient);
            amd64_reg_set_encoded8(cpu, 4, false, remainder);
            return INT_NONE;
        }
        case 16: {
            uint16_t divisor = (uint16_t) src;
            uint32_t dividend = ((uint32_t) amd64_reg_get(cpu, amd64_rdx, 16) << 16) |
                    (uint16_t) amd64_reg_get(cpu, amd64_rax, 16);
            if (divisor == 0)
                return INT_DIV;
            uint32_t quotient = dividend / divisor;
            uint32_t remainder = dividend % divisor;
            if (quotient > 0xffff)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 16, quotient);
            amd64_reg_set(cpu, amd64_rdx, 16, remainder);
            return INT_NONE;
        }
        case 32: {
            uint32_t divisor = (uint32_t) src;
            uint64_t dividend = ((uint64_t) amd64_reg_get(cpu, amd64_rdx, 32) << 32) |
                    (uint32_t) amd64_reg_get(cpu, amd64_rax, 32);
            if (divisor == 0)
                return INT_DIV;
            uint64_t quotient = dividend / divisor;
            uint64_t remainder = dividend % divisor;
            if (quotient > 0xffffffffu)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 32, quotient);
            amd64_reg_set(cpu, amd64_rdx, 32, remainder);
            return INT_NONE;
        }
        case 64: {
            uint64_t divisor = (uint64_t) src;
            __uint128_t dividend = ((__uint128_t) amd64_reg_get(cpu, amd64_rdx, 64) << 64) |
                    (__uint128_t) amd64_reg_get(cpu, amd64_rax, 64);
            if (divisor == 0)
                return INT_DIV;
            __uint128_t quotient = dividend / divisor;
            __uint128_t remainder = dividend % divisor;
            if ((quotient >> 64) != 0)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 64, (uint64_t) quotient);
            amd64_reg_set(cpu, amd64_rdx, 64, (uint64_t) remainder);
            return INT_NONE;
        }
        default:
            return INT_UNDEFINED;
        }
    case 7:
        switch (size) {
        case 8: {
            int8_t divisor = (int8_t) src;
            int16_t dividend = (int16_t) amd64_reg_get(cpu, amd64_rax, 16);
            if (divisor == 0)
                return INT_DIV;
            int16_t quotient = dividend / divisor;
            int16_t remainder = dividend % divisor;
            if (quotient < INT8_MIN || quotient > INT8_MAX)
                return INT_DIV;
            amd64_reg_set_encoded8(cpu, 0, true, (uint8_t) quotient);
            amd64_reg_set_encoded8(cpu, 4, false, (uint8_t) remainder);
            return INT_NONE;
        }
        case 16: {
            int16_t divisor = (int16_t) src;
            int32_t dividend = ((int32_t) (int16_t) amd64_reg_get(cpu, amd64_rdx, 16) << 16) |
                    (uint16_t) amd64_reg_get(cpu, amd64_rax, 16);
            if (divisor == 0)
                return INT_DIV;
            int32_t quotient = dividend / divisor;
            int32_t remainder = dividend % divisor;
            if (quotient < INT16_MIN || quotient > INT16_MAX)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 16, (uint16_t) quotient);
            amd64_reg_set(cpu, amd64_rdx, 16, (uint16_t) remainder);
            return INT_NONE;
        }
        case 32: {
            int32_t divisor = (int32_t) src;
            int64_t dividend = ((int64_t) (int32_t) amd64_reg_get(cpu, amd64_rdx, 32) << 32) |
                    (uint32_t) amd64_reg_get(cpu, amd64_rax, 32);
            if (divisor == 0)
                return INT_DIV;
            int64_t quotient = dividend / divisor;
            int64_t remainder = dividend % divisor;
            if (quotient < INT32_MIN || quotient > INT32_MAX)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 32, (uint32_t) quotient);
            amd64_reg_set(cpu, amd64_rdx, 32, (uint32_t) remainder);
            return INT_NONE;
        }
        case 64: {
            int64_t divisor = (int64_t) src;
            __int128_t dividend = ((__int128_t) (int64_t) amd64_reg_get(cpu, amd64_rdx, 64) << 64) |
                    (__uint128_t) amd64_reg_get(cpu, amd64_rax, 64);
            if (divisor == 0)
                return INT_DIV;
            __int128_t quotient = dividend / divisor;
            __int128_t remainder = dividend % divisor;
            if (quotient < INT64_MIN || quotient > INT64_MAX)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 64, (uint64_t) quotient);
            amd64_reg_set(cpu, amd64_rdx, 64, (uint64_t) remainder);
            return INT_NONE;
        }
        default:
            return INT_UNDEFINED;
        }
    default:
        return INT_UNDEFINED;
    }
}

static inline bool amd64_pop_size(struct cpu_state *cpu, struct tlb *tlb, unsigned size, qword_t *value) {
    qword_t rsp = cpu->amd64_regs[amd64_rsp];
    switch (size) {
    case 16: {
        uint16_t tmp;
        if (!amd64_mem_read(cpu, tlb, rsp, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        cpu->amd64_regs[amd64_rsp] = rsp + sizeof(tmp);
        amd64_trace_suspicious_rsp_write(cpu, rsp, cpu->amd64_regs[amd64_rsp], size);
        amd64_trace_as_stack(amd64_as_stack_pop, size, rsp, cpu->amd64_regs[amd64_rsp], *value);
        return true;
    }
    case 64: {
        uint64_t tmp;
        if (!amd64_mem_read(cpu, tlb, rsp, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        cpu->amd64_regs[amd64_rsp] = rsp + sizeof(tmp);
        amd64_trace_suspicious_rsp_write(cpu, rsp, cpu->amd64_regs[amd64_rsp], size);
        amd64_trace_as_stack(amd64_as_stack_pop, size, rsp, cpu->amd64_regs[amd64_rsp], *value);
        return true;
    }
    default:
        return false;
    }
}

static inline bool amd64_pop(struct cpu_state *cpu, struct tlb *tlb, qword_t *value) {
    return amd64_pop_size(cpu, tlb, 64, value);
}

static inline qword_t amd64_effective_addr(struct cpu_state *cpu, const struct amd64_modrm *modrm,
        bool fs_prefix);

static inline qword_t amd64_bt_mem_addr(qword_t addr, unsigned size, qword_t bit_index,
        bool stride_memory, bool signed_index, qword_t *bit_out) {
    qword_t truncated_index = amd64_trunc(bit_index, size);
    *bit_out = truncated_index & (size - 1);
    if (!stride_memory)
        return addr;

    sqword_t scaled_index = signed_index
            ? amd64_sign_extend(bit_index, size)
            : (sqword_t) truncated_index;
    sqword_t element_index = scaled_index >> __builtin_ctz(size);
    return addr + (qword_t) (element_index * (sqword_t) (size / 8));
}

static inline bool amd64_read_bt_operand(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t bit_index,
        bool stride_memory, bool signed_index, qword_t *value, qword_t *addr_out, qword_t *bit_out) {
    if (modrm->is_reg) {
        *addr_out = 0;
        *bit_out = bit_index & (size - 1);
        return amd64_read_rm(cpu, tlb, modrm, fs_prefix, size, value);
    }

    qword_t addr = amd64_bt_mem_addr(amd64_effective_addr(cpu, modrm, fs_prefix),
            size, bit_index, stride_memory, signed_index, bit_out);
    *addr_out = addr;
    switch (size) {
    case 16: {
        uint16_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 32: {
        uint32_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 64: {
        uint64_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    default:
        return false;
    }
}

static inline bool amd64_write_bt_operand(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t addr,
        qword_t value) {
    if (modrm->is_reg)
        return amd64_write_rm(cpu, tlb, modrm, fs_prefix, size, value);

    switch (size) {
    case 16: {
        uint16_t tmp = (uint16_t) value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    case 32: {
        uint32_t tmp = (uint32_t) value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    case 64: {
        uint64_t tmp = (uint64_t) value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    default:
        return false;
    }
}

static inline bool amd64_decode_modrm(struct cpu_state *cpu, struct tlb *tlb,
        struct amd64_rex_prefix rex, struct amd64_modrm *modrm) {
    byte_t modrm_byte;
    if (!amd64_fetch_u8(cpu, tlb, &modrm_byte))
        return false;

    unsigned mod = MOD(modrm_byte);
    modrm->rex_present = rex.present;
    modrm->reg = REG(modrm_byte) | (rex.r ? 8 : 0);
    modrm->rm = RM(modrm_byte) | (rex.b ? 8 : 0);
    modrm->is_reg = mod == 3;
    modrm->has_base = false;
    modrm->has_index = false;
    modrm->rip_relative = false;
    modrm->disp = 0;
    modrm->scale = 0;

    if (modrm->is_reg)
        return true;

    if (cpu->amd64_address_size_prefix) {
        unsigned rm_low = RM(modrm_byte);
        if (rm_low == 4) {
            byte_t sib;
            if (!amd64_fetch_u8(cpu, tlb, &sib))
                return false;
            unsigned base_low = RM(sib);
            unsigned index_low = REG(sib);
            modrm->scale = MOD(sib);
            if (index_low != 4 || rex.x) {
                modrm->has_index = true;
                modrm->index = index_low | (rex.x ? 8 : 0);
            }
            if (mod == 0 && base_low == 5 && !rex.b) {
                modrm->has_base = false;
            } else {
                modrm->has_base = true;
                modrm->base = base_low | (rex.b ? 8 : 0);
            }
        } else if (mod == 0 && rm_low == 5 && !rex.b) {
            modrm->has_base = false;
        } else {
            modrm->has_base = true;
            modrm->base = modrm->rm;
        }

        if (mod == 1) {
            int8_t disp8;
            if (!amd64_fetch(cpu, tlb, &disp8, sizeof(disp8)))
                return false;
            modrm->disp = disp8;
        } else if (mod == 2 || (mod == 0 && !modrm->has_base)) {
            int32_t disp32;
            if (!amd64_fetch(cpu, tlb, &disp32, sizeof(disp32)))
                return false;
            modrm->disp = disp32;
        }
        return true;
    }

    unsigned rm_low = RM(modrm_byte);
    if (rm_low == 4) {
        byte_t sib;
        if (!amd64_fetch_u8(cpu, tlb, &sib))
            return false;
        unsigned base_low = RM(sib);
        unsigned index_low = REG(sib);
        modrm->scale = MOD(sib);
        // In 64-bit mode, SIB index 100 means "no index" only when REX.X is clear.
        if (index_low != 4 || rex.x) {
            modrm->has_index = true;
            modrm->index = index_low | (rex.x ? 8 : 0);
        }
        if (mod == 0 && base_low == 5 && !rex.b) {
            modrm->has_base = false;
        } else {
            modrm->has_base = true;
            modrm->base = base_low | (rex.b ? 8 : 0);
        }
    } else if (mod == 0 && rm_low == 5) {
        modrm->rip_relative = true;
    } else {
        modrm->has_base = true;
        modrm->base = modrm->rm;
    }

    if (mod == 1) {
        int8_t disp8;
        if (!amd64_fetch(cpu, tlb, &disp8, sizeof(disp8)))
            return false;
        modrm->disp = disp8;
    } else if (mod == 2 || (mod == 0 && (rm_low == 5 || (rm_low == 4 && !modrm->has_base)))) {
        int32_t disp32;
        if (!amd64_fetch(cpu, tlb, &disp32, sizeof(disp32)))
            return false;
        modrm->disp = disp32;
    }
    return true;
}

static inline qword_t amd64_effective_addr(struct cpu_state *cpu, const struct amd64_modrm *modrm, bool fs_prefix) {
    if (cpu->amd64_address_size_prefix) {
        uint32_t addr32 = (uint32_t) modrm->disp;
        if (modrm->has_base)
            addr32 += (uint32_t) cpu->amd64_regs[modrm->base];
        if (modrm->has_index)
            addr32 += (uint32_t) cpu->amd64_regs[modrm->index] << modrm->scale;
        qword_t addr = addr32;
        if (fs_prefix)
            addr += cpu->tls_ptr;
        return addr;
    }

    qword_t addr = (sqword_t) modrm->disp;
    if (modrm->rip_relative)
        addr += cpu->amd64_rip;
    if (modrm->has_base)
        addr += cpu->amd64_regs[modrm->base];
    if (modrm->has_index)
        addr += cpu->amd64_regs[modrm->index] << modrm->scale;
    if (fs_prefix)
        addr += cpu->tls_ptr;
    return addr;
}

static inline bool amd64_read_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t *value) {
    if (modrm->is_reg) {
        *value = size == 8 ? amd64_reg_get_encoded8(cpu, modrm->rm, modrm->rex_present) : amd64_reg_get(cpu, modrm->rm, size);
        return true;
    }

    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    switch (size) {
    case 8: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 16: {
        uint16_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 32: {
        uint32_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 64: {
        uint64_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        if (modrm->reg == amd64_rbx && modrm->has_base)
            amd64_trace_htop_rbx_source(cpu, cpu->amd64_regs[modrm->base], addr, tmp);
        if (modrm->reg == amd64_r13)
            amd64_trace_htop_r13_source(cpu, addr, tmp);
        return true;
    }
    default:
        return false;
    }
}

static inline bool amd64_read_xmm_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, union xmm_reg *value) {
    if (modrm->reg >= AMD64_XMM_COUNT)
        return false;
    if (modrm->is_reg) {
        if (modrm->rm >= AMD64_XMM_COUNT)
            return false;
        *value = cpu->xmm[modrm->rm];
        return true;
    }
    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    return amd64_mem_read(cpu, tlb, addr, value, sizeof(*value));
}

static inline bool amd64_write_xmm_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, const union xmm_reg *value) {
    if (modrm->reg >= AMD64_XMM_COUNT)
        return false;
    if (modrm->is_reg) {
        if (modrm->rm >= AMD64_XMM_COUNT)
            return false;
        cpu->xmm[modrm->rm] = *value;
        return true;
    }
    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    return amd64_mem_write(cpu, tlb, addr, value, sizeof(*value));
}

static inline bool amd64_write_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t value) {
    if (modrm->is_reg) {
        if (size == 8)
            amd64_reg_set_encoded8(cpu, modrm->rm, modrm->rex_present, value);
        else
            amd64_reg_set(cpu, modrm->rm, size, value);
        return true;
    }

    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    switch (size) {
    case 8: {
        uint8_t tmp = value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    case 16: {
        uint16_t tmp = value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    case 32: {
        uint32_t tmp = value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    case 64: {
        uint64_t tmp = value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    default:
        return false;
    }
}

static void amd64_fill_fxsave_area(struct cpu_state *cpu, struct amd64_fxsave_area *area) {
    memset(area, 0, sizeof(*area));
    area->fcw = cpu->fcw;
    area->fsw = cpu->fsw;
    area->mxcsr = 0x1f80;
    area->mxcsr_mask = 0xffff;

    for (int i = 0; i < 8; i++) {
        const float80 value = cpu->fp[i];
        for (int j = 0; j < 4; j++)
            area->st[i].significand[j] = (word_t) (value.signif >> (j * 16));
        area->st[i].exponent = value.signExp;
        if (value.signif != 0 || value.signExp != 0)
            area->ftw |= (byte_t) (1u << i);
    }

    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 4; j++)
            area->xmm[i].element[j] = cpu->xmm[i].u32[j];
}

static void amd64_restore_fxsave_area(struct cpu_state *cpu, const struct amd64_fxsave_area *area) {
    word_t fcw = area->fcw;
    fpu_ldcw16(cpu, &fcw);
    cpu->fsw = area->fsw;

    for (int i = 0; i < 8; i++) {
        float80 value = {0};
        for (int j = 0; j < 4; j++)
            value.signif |= (uint64_t) area->st[i].significand[j] << (j * 16);
        value.signExp = area->st[i].exponent;
        cpu->fp[i] = value;
    }

    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 4; j++)
            cpu->xmm[i].u32[j] = area->xmm[i].element[j];
}

static inline int amd64_fxsave_op(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, qword_t saved_rip) {
    struct amd64_fxsave_area area;
    qword_t addr;

    if (modrm->is_reg || (modrm->reg != 0 && modrm->reg != 1))
        return INT_UNDEFINED;

    addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    if ((addr & 0xf) != 0) {
        cpu->amd64_rip = saved_rip;
        cpu->segfault_addr = addr;
        return INT_GPF;
    }

    if (modrm->reg == 0) {
        amd64_fill_fxsave_area(cpu, &area);
        if (!amd64_mem_write(cpu, tlb, addr, &area, sizeof(area))) {
            cpu->amd64_rip = saved_rip;
            return INT_PF;
        }
    } else {
        if (!amd64_mem_read(cpu, tlb, addr, &area, sizeof(area))) {
            cpu->amd64_rip = saved_rip;
            return INT_PF;
        }
        amd64_restore_fxsave_area(cpu, &area);
    }

    return INT_NONE;
}

static inline int amd64_handle_x87(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip, struct amd64_rex_prefix rex, bool fs_prefix, byte_t opcode) {
    struct amd64_modrm modrm;
    unsigned rm;
    unsigned subop;
    unsigned fullop;
    qword_t addr = 0;

    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_fpu_gpf_restore;

    rm = modrm.rm & 7;
    subop = ((unsigned) opcode << 4) | (modrm.reg & 7);
    fullop = ((unsigned) opcode << 8) | ((modrm.reg & 7) << 4) | rm;
    if (!modrm.is_reg)
        addr = amd64_effective_addr(cpu, &modrm, fs_prefix);

    if (!modrm.is_reg) {
        switch (subop) {
        case 0xd80: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_addm32(cpu, &value);
            break;
        }
        case 0xd81: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_mulm32(cpu, &value);
            break;
        }
        case 0xd82: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_comm32(cpu, &value);
            break;
        }
        case 0xd83: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_comm32(cpu, &value);
            fpu_pop(cpu);
            break;
        }
        case 0xd84: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_subm32(cpu, &value);
            break;
        }
        case 0xd85: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_subrm32(cpu, &value);
            break;
        }
        case 0xd86: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_divm32(cpu, &value);
            break;
        }
        case 0xd87: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_divrm32(cpu, &value);
            break;
        }
        case 0xd90: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ldm32(cpu, &value);
            break;
        }
        case 0xd92: {
            float value;
            fpu_stm32(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xd93: {
            float value;
            fpu_stm32(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xd94: {
            struct fpu_env32 env;
            if (!amd64_mem_read(cpu, tlb, addr, &env, sizeof(env)))
                goto amd64_fpu_gpf_restore;
            fpu_ldenv32(cpu, &env);
            break;
        }
        case 0xd95: {
            uint16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ldcw16(cpu, &value);
            break;
        }
        case 0xd96: {
            struct fpu_env32 env;
            fpu_stenv32(cpu, &env);
            if (!amd64_mem_write(cpu, tlb, addr, &env, sizeof(env)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xd97: {
            uint16_t value;
            fpu_stcw16(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xda0: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_iadd32(cpu, &value);
            break;
        }
        case 0xda1: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_imul32(cpu, &value);
            break;
        }
        case 0xda2: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_icom32(cpu, &value);
            break;
        }
        case 0xda3: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_icom32(cpu, &value);
            fpu_pop(cpu);
            break;
        }
        case 0xda4: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_isub32(cpu, &value);
            break;
        }
        case 0xda5: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_isubr32(cpu, &value);
            break;
        }
        case 0xda6: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_idiv32(cpu, &value);
            break;
        }
        case 0xda7: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_idivr32(cpu, &value);
            break;
        }
        case 0xdb0: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ild32(cpu, &value);
            break;
        }
        case 0xdb2: {
            int32_t value;
            fpu_ist32(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xdb3: {
            int32_t value;
            fpu_ist32(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdb5: {
            float80 value = {};
            if (!amd64_mem_read(cpu, tlb, addr, &value, 10))
                goto amd64_fpu_gpf_restore;
            fpu_ldm80(cpu, &value);
            break;
        }
        case 0xdb7: {
            float80 value;
            fpu_stm80(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, 10))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdc0: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_addm64(cpu, &value);
            break;
        }
        case 0xdc1: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_mulm64(cpu, &value);
            break;
        }
        case 0xdc2: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_comm64(cpu, &value);
            break;
        }
        case 0xdc3: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_comm64(cpu, &value);
            fpu_pop(cpu);
            break;
        }
        case 0xdc4: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_subm64(cpu, &value);
            break;
        }
        case 0xdc5: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_subrm64(cpu, &value);
            break;
        }
        case 0xdc6: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_divm64(cpu, &value);
            break;
        }
        case 0xdc7: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_divrm64(cpu, &value);
            break;
        }
        case 0xdd0: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ldm64(cpu, &value);
            break;
        }
        case 0xdd2: {
            double value;
            fpu_stm64(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xdd3: {
            double value;
            fpu_stm64(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdd4: {
            struct fpu_state32 state;
            if (!amd64_mem_read(cpu, tlb, addr, &state, sizeof(state)))
                goto amd64_fpu_gpf_restore;
            fpu_restore32(cpu, &state);
            break;
        }
        case 0xdd6: {
            struct fpu_state32 state;
            fpu_save32(cpu, &state);
            if (!amd64_mem_write(cpu, tlb, addr, &state, sizeof(state)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xde0: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_iadd16(cpu, &value);
            break;
        }
        case 0xde1: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_imul16(cpu, &value);
            break;
        }
        case 0xde2: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_icom16(cpu, &value);
            break;
        }
        case 0xde3: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_icom16(cpu, &value);
            fpu_pop(cpu);
            break;
        }
        case 0xde4: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_isub16(cpu, &value);
            break;
        }
        case 0xde5: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_isubr16(cpu, &value);
            break;
        }
        case 0xde6: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_idiv16(cpu, &value);
            break;
        }
        case 0xde7: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_idivr16(cpu, &value);
            break;
        }
        case 0xdf0: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ild16(cpu, &value);
            break;
        }
        case 0xdf2: {
            int16_t value;
            fpu_ist16(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xdf3: {
            int16_t value;
            fpu_ist16(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdf5: {
            int64_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ild64(cpu, &value);
            break;
        }
        case 0xdf7: {
            int64_t value;
            fpu_ist64(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        default:
            return INT_UNDEFINED;
        }
        return INT_NONE;
    }

    switch (subop) {
    case 0xd80:
        fpu_add(cpu, rm, 0);
        return INT_NONE;
    case 0xd81:
        fpu_mul(cpu, rm, 0);
        return INT_NONE;
    case 0xd82:
        fpu_com(cpu, rm);
        return INT_NONE;
    case 0xd83:
        fpu_com(cpu, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xd84:
        fpu_sub(cpu, rm, 0);
        return INT_NONE;
    case 0xd85:
        fpu_subr(cpu, rm, 0);
        return INT_NONE;
    case 0xd86:
        fpu_div(cpu, rm, 0);
        return INT_NONE;
    case 0xd87:
        fpu_divr(cpu, rm, 0);
        return INT_NONE;
    case 0xd90:
        fpu_ld(cpu, rm);
        return INT_NONE;
    case 0xd91:
        fpu_xch(cpu, rm);
        return INT_NONE;
    case 0xda0:
        fpu_cmovb(cpu, rm);
        return INT_NONE;
    case 0xda1:
        fpu_cmove(cpu, rm);
        return INT_NONE;
    case 0xda2:
        fpu_cmovbe(cpu, rm);
        return INT_NONE;
    case 0xda3:
        fpu_cmovu(cpu, rm);
        return INT_NONE;
    case 0xdb0:
        fpu_cmovnb(cpu, rm);
        return INT_NONE;
    case 0xdb1:
        fpu_cmovne(cpu, rm);
        return INT_NONE;
    case 0xdb2:
        fpu_cmovnbe(cpu, rm);
        return INT_NONE;
    case 0xdb3:
        fpu_cmovnu(cpu, rm);
        return INT_NONE;
    case 0xdb5:
        fpu_ucomi(cpu, rm);
        return INT_NONE;
    case 0xdb6:
        fpu_comi(cpu, rm);
        return INT_NONE;
    case 0xdc0:
        fpu_add(cpu, 0, rm);
        return INT_NONE;
    case 0xdc1:
        fpu_mul(cpu, 0, rm);
        return INT_NONE;
    case 0xdc4:
        fpu_subr(cpu, 0, rm);
        return INT_NONE;
    case 0xdc5:
        fpu_sub(cpu, 0, rm);
        return INT_NONE;
    case 0xdc6:
        fpu_divr(cpu, 0, rm);
        return INT_NONE;
    case 0xdc7:
        fpu_div(cpu, 0, rm);
        return INT_NONE;
    case 0xdd0:
        return INT_NONE;
    case 0xdd2:
        fpu_st(cpu, rm);
        return INT_NONE;
    case 0xdd3:
        fpu_st(cpu, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xdd4:
        fpu_ucom(cpu, rm);
        return INT_NONE;
    case 0xdd5:
        fpu_ucom(cpu, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde0:
        fpu_add(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde1:
        fpu_mul(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde4:
        fpu_subr(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde5:
        fpu_sub(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde6:
        fpu_divr(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde7:
        fpu_div(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xdf0:
        fpu_pop(cpu);
        return INT_NONE;
    case 0xdf5:
        fpu_ucomi(cpu, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xdf6:
        fpu_comi(cpu, rm);
        fpu_pop(cpu);
        return INT_NONE;
    default:
        break;
    }

    switch (fullop) {
    case 0xd940:
        fpu_chs(cpu);
        return INT_NONE;
    case 0xd941:
        fpu_abs(cpu);
        return INT_NONE;
    case 0xd944:
        fpu_tst(cpu);
        return INT_NONE;
    case 0xd945:
        fpu_xam(cpu);
        return INT_NONE;
    case 0xd950:
        fpu_ldc(cpu, fconst_one);
        return INT_NONE;
    case 0xd951:
        fpu_ldc(cpu, fconst_log2t);
        return INT_NONE;
    case 0xd952:
        fpu_ldc(cpu, fconst_log2e);
        return INT_NONE;
    case 0xd953:
        fpu_ldc(cpu, fconst_pi);
        return INT_NONE;
    case 0xd954:
        fpu_ldc(cpu, fconst_log2);
        return INT_NONE;
    case 0xd955:
        fpu_ldc(cpu, fconst_ln2);
        return INT_NONE;
    case 0xd956:
        fpu_ldc(cpu, fconst_zero);
        return INT_NONE;
    case 0xd960:
        fpu_2xm1(cpu);
        return INT_NONE;
    case 0xd961:
        fpu_yl2x(cpu);
        return INT_NONE;
    case 0xd963:
        fpu_patan(cpu);
        return INT_NONE;
    case 0xd964:
        fpu_xtract(cpu);
        return INT_NONE;
    case 0xd967:
        fpu_incstp(cpu);
        return INT_NONE;
    case 0xd970:
        fpu_prem(cpu);
        return INT_NONE;
    case 0xd972:
        fpu_sqrt(cpu);
        return INT_NONE;
    case 0xd974:
        fpu_rndint(cpu);
        return INT_NONE;
    case 0xd975:
        fpu_scale(cpu);
        return INT_NONE;
    case 0xd976:
        fpu_sin(cpu);
        return INT_NONE;
    case 0xd977:
        fpu_cos(cpu);
        return INT_NONE;
    case 0xdb42:
        fpu_clex(cpu);
        return INT_NONE;
    case 0xde31:
        fpu_com(cpu, 1);
        fpu_pop(cpu);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xdf40:
        amd64_reg_set(cpu, amd64_rax, 16, cpu->fsw);
        return INT_NONE;
    default:
        return INT_UNDEFINED;
    }

amd64_fpu_gpf_restore:
    cpu->amd64_rip = saved_rip;
    cpu->segfault_addr = saved_rip;
    return INT_GPF;
}

static inline void amd64_trace_qword_store(struct cpu_state *cpu, qword_t rip,
        byte_t opcode, qword_t addr, qword_t value) {
    unsigned slot = cpu->amd64_store_trace_next++ % AMD64_STORE_TRACE_COUNT;
    cpu->amd64_store_trace[slot] = (struct amd64_store_trace) {
        .rip = rip,
        .addr = addr,
        .value = value,
        .opcode = opcode,
    };
}

static inline bool amd64_cond_eval(struct cpu_state *cpu, unsigned cc) {
    switch (cc & 0xf) {
    case 0x0: return OF;
    case 0x1: return !OF;
    case 0x2: return CF;
    case 0x3: return !CF;
    case 0x4: return ZF;
    case 0x5: return !ZF;
    case 0x6: return CF || ZF;
    case 0x7: return !CF && !ZF;
    case 0x8: return SF;
    case 0x9: return !SF;
    case 0xa: return PF;
    case 0xb: return !PF;
    case 0xc: return SF != OF;
    case 0xd: return SF == OF;
    case 0xe: return ZF || (SF != OF);
    case 0xf: return !ZF && (SF == OF);
    default: return false;
    }
}

enum amd64_rep_mode {
    AMD64_REP_NONE,
    AMD64_REPZ,
    AMD64_REPNZ,
};

static inline void amd64_bump_string_reg(struct cpu_state *cpu, unsigned reg, unsigned size) {
    qword_t delta = size / 8;
    if (cpu->amd64_address_size_prefix) {
        uint32_t value = (uint32_t) cpu->amd64_regs[reg];
        value = !cpu->df ? value + (uint32_t) delta : value - (uint32_t) delta;
        amd64_reg_set(cpu, reg, 32, value);
        return;
    }
    if (!cpu->df)
        cpu->amd64_regs[reg] += delta;
    else
        cpu->amd64_regs[reg] -= delta;
}

static inline qword_t amd64_string_addr(const struct cpu_state *cpu, unsigned reg) {
    if (cpu->amd64_address_size_prefix)
        return (uint32_t) cpu->amd64_regs[reg];
    return cpu->amd64_regs[reg];
}

static inline int amd64_string_op(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip, byte_t opcode, unsigned size, enum amd64_rep_mode rep_mode) {
    unsigned count_size = cpu->amd64_address_size_prefix ? 32 : 64;
    qword_t count = rep_mode == AMD64_REP_NONE ? 1 : amd64_reg_get(cpu, amd64_rcx, count_size);

    while (count != 0) {
        qword_t value;
        switch (opcode) {
        case 0xa4:
        case 0xa5:
            if (!amd64_mem_read(cpu, tlb, amd64_string_addr(cpu, amd64_rsi), &value, size / 8))
                goto amd64_string_pf;
            if (!amd64_mem_write(cpu, tlb, amd64_string_addr(cpu, amd64_rdi), &value, size / 8))
                goto amd64_string_pf;
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        case 0xaa:
        case 0xab:
            value = amd64_reg_get(cpu, amd64_rax, size);
            if (!amd64_mem_write(cpu, tlb, amd64_string_addr(cpu, amd64_rdi), &value, size / 8))
                goto amd64_string_pf;
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        case 0xac:
        case 0xad:
            if (!amd64_mem_read(cpu, tlb, amd64_string_addr(cpu, amd64_rsi), &value, size / 8))
                goto amd64_string_pf;
            amd64_reg_set(cpu, amd64_rax, size, value);
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            break;
        case 0xae:
        case 0xaf: {
            qword_t lhs = amd64_reg_get(cpu, amd64_rax, size);
            qword_t rhs;
            if (!amd64_mem_read(cpu, tlb, amd64_string_addr(cpu, amd64_rdi), &rhs, size / 8))
                goto amd64_string_pf;
            amd64_set_sub_flags(cpu, lhs, rhs, lhs - rhs, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        }
        default: {
            qword_t lhs;
            qword_t rhs;
            if (!amd64_mem_read(cpu, tlb, amd64_string_addr(cpu, amd64_rsi), &lhs, size / 8))
                goto amd64_string_pf;
            if (!amd64_mem_read(cpu, tlb, amd64_string_addr(cpu, amd64_rdi), &rhs, size / 8))
                goto amd64_string_pf;
            amd64_set_sub_flags(cpu, lhs, rhs, lhs - rhs, size);
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        }
        }

        if (rep_mode != AMD64_REP_NONE) {
            count--;
            amd64_reg_set(cpu, amd64_rcx, count_size, count);
            if (opcode == 0xa6 || opcode == 0xa7 || opcode == 0xae || opcode == 0xaf) {
                if (rep_mode == AMD64_REPZ && !cpu->zf)
                    break;
                if (rep_mode == AMD64_REPNZ && cpu->zf)
                    break;
            }
        } else {
            break;
        }
    }
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_string_pf:
    cpu->amd64_rip = saved_rip;
    return INT_PF;
}

static inline int amd64_step_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    qword_t saved_rip = cpu->amd64_rip;
    cpu->amd64_current_insn_rip = saved_rip;
    if (amd64_bash_trace_enabled())
        amd64_trace_bash_cond_probe(cpu);
    if (amd64_cc1_trace_enabled())
        amd64_trace_cc1_step(cpu);
    if (amd64_as_trace_enabled())
        amd64_trace_as_step(cpu);
    cpu->amd64_address_size_prefix = false;
    if (amd64_cargo_trace_enabled)
        amd64_trace_cargo_start_call(cpu);
    if (amd64_htop_legacy_trace_enabled)
        amd64_trace_htop_window(cpu, tlb);
    if (amd64_cargo_trace_enabled)
        amd64_trace_cargo_pf_window(cpu, tlb);
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    enum amd64_rep_mode rep_mode = AMD64_REP_NONE;
    struct amd64_rex_prefix rex = {};
    byte_t opcode;

restart_prefix:
    if (!amd64_fetch_u8(cpu, tlb, &opcode)) {
        cpu->amd64_rip = saved_rip;
        cpu->segfault_addr = saved_rip;
        return INT_GPF;
    }

    if (opcode == 0x66) {
        operand_size_prefix = true;
        goto restart_prefix;
    }
    if (opcode == 0x2e || opcode == 0x3e) {
        goto restart_prefix;
    }
    if (opcode == 0x67) {
        cpu->amd64_address_size_prefix = true;
        goto restart_prefix;
    }
    if (opcode == 0x64) {
        fs_prefix = true;
        goto restart_prefix;
    }
    if (opcode == 0xf0) {
        lock_prefix = true;
        goto restart_prefix;
    }
    if (opcode == 0xf3) {
        rep_mode = AMD64_REPZ;
        goto restart_prefix;
    }
    if (opcode == 0xf2) {
        rep_mode = AMD64_REPNZ;
        goto restart_prefix;
    }
    if (opcode >= 0x40 && opcode <= 0x4f) {
        rex.present = true;
        rex.w = (opcode & 0x8) != 0;
        rex.r = (opcode & 0x4) != 0;
        rex.x = (opcode & 0x2) != 0;
        rex.b = (opcode & 0x1) != 0;
        goto restart_prefix;
    }

    unsigned op_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
    (void) lock_prefix;
    switch (opcode) {
    case 0xa4:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, 8, rep_mode);
    case 0xa5:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, op_size, rep_mode);
    case 0xa6:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, 8, rep_mode);
    case 0xa7:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, op_size, rep_mode);
    case 0xaa:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, 8, rep_mode);
    case 0xab:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, op_size, rep_mode);
    case 0xac:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, 8, rep_mode);
    case 0xad:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, op_size, rep_mode);
    case 0xae:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, 8, rep_mode);
    case 0xaf:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, op_size, rep_mode);
    case 0x0f: {
        byte_t op2;
        if (!amd64_fetch_u8(cpu, tlb, &op2)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (op2 == 0x05)
            return INT_AMD64_SYSCALL;
        if (op2 == 0x31) {
            qword_t tsc = amd64_rdtsc_value();
            amd64_reg_set(cpu, amd64_rax, 32, (dword_t) tsc);
            amd64_reg_set(cpu, amd64_rdx, 32, (dword_t) (tsc >> 32));
            break;
        }
        if (op2 == 0xa2) {
            dword_t eax = (dword_t) cpu->amd64_regs[amd64_rax];
            dword_t ebx = (dword_t) cpu->amd64_regs[amd64_rbx];
            dword_t ecx = (dword_t) cpu->amd64_regs[amd64_rcx];
            dword_t edx = (dword_t) cpu->amd64_regs[amd64_rdx];
            do_cpuid(&eax, &ebx, &ecx, &edx);
            cpu->amd64_regs[amd64_rax] = eax;
            cpu->amd64_regs[amd64_rbx] = ebx;
            cpu->amd64_regs[amd64_rcx] = ecx;
            cpu->amd64_regs[amd64_rdx] = edx;
            cpu->eax = eax;
            cpu->ebx = ebx;
            cpu->ecx = ecx;
            cpu->edx = edx;
            break;
        }
        if (op2 == 0x18) {
            struct amd64_modrm modrm;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg > 3)
                return INT_UNDEFINED;
            break;
        }
        if (op2 == 0xae) {
            struct amd64_modrm modrm;
            int interrupt;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            interrupt = amd64_fxsave_op(cpu, tlb, &modrm, fs_prefix, saved_rip);
            if (interrupt != INT_NONE)
                return interrupt;
            break;
        }
        if (op2 == 0x1e && rep_mode == AMD64_REPZ) {
            byte_t op3;
            if (!amd64_fetch_u8(cpu, tlb, &op3)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (op3 == 0xfa || op3 == 0xfb)
                break;
            return INT_UNDEFINED;
        }
        if (op2 >= 0x80 && op2 <= 0x8f) {
            int32_t rel32;
            if (!amd64_fetch(cpu, tlb, &rel32, sizeof(rel32))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            bool taken = amd64_cond_eval(cpu, op2 & 0xf);
            if (amd64_as_alu_stderr_enabled() &&
                    current != NULL &&
                    current->abi == GUEST_ABI_AMD64 &&
                    strcmp(current->comm, "as") == 0) {
                fprintf(stderr,
                        "amd64 as jcc32: rip=%#llx cc=%u taken=%u target=%#llx cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                        (unsigned long long) saved_rip,
                        op2 & 0xf,
                        taken,
                        (unsigned long long) (cpu->amd64_rip + rel32),
                        cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af);
            }
            amd64_trace_cc1_je_probe(cpu, saved_rip, taken, cpu->amd64_rip + rel32);
            if (taken) {
                qword_t target = cpu->amd64_rip + rel32;
                int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "jcc");
                if (target_interrupt != INT_NONE)
                    return target_interrupt;
                cpu->amd64_rip = target;
            }
            break;
        }
        if (op2 >= 0x40 && op2 <= 0x4f) {
            struct amd64_modrm modrm;
            qword_t src;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &src)) {
                cpu->amd64_rip = saved_rip;
                return INT_GPF;
            }
            if (amd64_cond_eval(cpu, op2 & 0xf))
                amd64_reg_set(cpu, modrm.reg, op_size, src);
            break;
        }
        if (op2 >= 0x90 && op2 <= 0x9f) {
            struct amd64_modrm modrm;
            qword_t value = amd64_cond_eval(cpu, op2 & 0xf) ? 1 : 0;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, value))
                goto amd64_gpf_restore;
            break;
        }
        if (op2 == 0xa4 || op2 == 0xa5 || op2 == 0xac || op2 == 0xad) {
            struct amd64_modrm modrm;
            qword_t lhs, rhs, result;
            unsigned count;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (op2 == 0xa4 || op2 == 0xac) {
                uint8_t imm8;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                count = imm8 & (op_size == 64 ? 0x3f : 0x1f);
            } else {
                count = amd64_reg_get(cpu, amd64_rcx, 8) & (op_size == 64 ? 0x3f : 0x1f);
            }
            if (count == 0)
                break;
            if (count > op_size)
                return INT_UNDEFINED;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            if (op2 == 0xa4 || op2 == 0xa5) {
                result = amd64_trunc((lhs << count) | (rhs >> (op_size - count)), op_size);
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                    goto amd64_gpf_restore;
                amd64_set_double_shift_flags(cpu, lhs, result, op_size, count, true);
            } else {
                result = amd64_trunc((amd64_trunc(lhs, op_size) >> count) | (rhs << (op_size - count)), op_size);
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                    goto amd64_gpf_restore;
                amd64_set_double_shift_flags(cpu, lhs, result, op_size, count, false);
            }
            break;
        }
        if (op2 == 0xbc || op2 == 0xbd) {
            struct amd64_modrm modrm;
            qword_t src;
            qword_t src_masked;
            qword_t index;
            bool count_zeroes;
            if (rep_mode != AMD64_REP_NONE && rep_mode != AMD64_REPZ)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &src))
                goto amd64_gpf_restore;
            src_masked = amd64_trunc(src, op_size);
            count_zeroes = rep_mode == AMD64_REPZ;
            collapse_flags(cpu);
            if (count_zeroes) {
                cpu->cf = src_masked == 0;
                cpu->cf_bit = cpu->cf;
                cpu->zf = 0;
            } else {
                cpu->zf = src_masked == 0;
            }
            cpu->zf_res = 0;
            if (src_masked == 0) {
                if (count_zeroes)
                    amd64_reg_set(cpu, modrm.reg, op_size, op_size);
                break;
            }
            if (op2 == 0xbc) {
                index = (op_size == 64)
                        ? (qword_t) __builtin_ctzll(src_masked)
                        : (qword_t) __builtin_ctz((uint32_t) src_masked);
            } else {
                index = (op_size == 64)
                        ? (qword_t) (63 - __builtin_clzll(src_masked))
                        : (qword_t) (31 - __builtin_clz((uint32_t) src_masked));
            }
            if (count_zeroes)
                cpu->zf = index == 0;
            amd64_reg_set(cpu, modrm.reg, op_size, index);
            break;
        }
        if (op2 == 0xb6 || op2 == 0xb7 || op2 == 0xbe || op2 == 0xbf) {
            struct amd64_modrm modrm;
            qword_t src;
            unsigned src_size = (op2 == 0xb6 || op2 == 0xbe) ? 8 : 16;
            unsigned dst_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, src_size, &src))
                goto amd64_gpf_restore;
            if (op2 == 0xbe || op2 == 0xbf)
                src = (qword_t) amd64_sign_extend(src, src_size);
            amd64_reg_set(cpu, modrm.reg, dst_size, src);
            break;
        }
        if (op2 == 0x6e) {
            struct amd64_modrm modrm;
            qword_t src_scalar;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (operand_size_prefix) {
                union xmm_reg value;
                if (modrm.reg >= AMD64_XMM_COUNT)
                    return INT_UNDEFINED;
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                    goto amd64_gpf_restore;
                value.u128 = 0;
                if (rex.w)
                    value.qw[0] = src_scalar;
                else
                    value.u32[0] = (uint32_t) src_scalar;
                cpu->xmm[modrm.reg] = value;
            } else {
                if (modrm.reg >= 8)
                    return INT_UNDEFINED;
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                    goto amd64_gpf_restore;
                cpu->mm[modrm.reg].qw = rex.w ? src_scalar : (uint32_t) src_scalar;
            }
            break;
        }
        if (op2 == 0x2c && (rep_mode == AMD64_REPZ || rep_mode == AMD64_REPNZ)) {
            struct amd64_modrm modrm;
            qword_t src_scalar;
            qword_t result;
            if (operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (rep_mode == AMD64_REPNZ) {
                double src_double;
                if (modrm.is_reg) {
                    if (modrm.rm >= AMD64_XMM_COUNT)
                        return INT_UNDEFINED;
                    src_double = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_gpf_restore;
                    src_double = *(double *) &src_scalar;
                }
                result = amd64_cvtt_scalar_to_int(src_double, rex.w);
            } else {
                float src_float;
                uint32_t src_word;
                if (modrm.is_reg) {
                    if (modrm.rm >= AMD64_XMM_COUNT)
                        return INT_UNDEFINED;
                    src_float = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_gpf_restore;
                    src_word = (uint32_t) src_scalar;
                    src_float = *(float *) &src_word;
                }
                result = amd64_cvtt_scalar_to_int((double) src_float, rex.w);
            }
            amd64_reg_set(cpu, modrm.reg, rex.w ? 64 : 32, result);
            break;
        }
        if (op2 == 0x2a && (rep_mode == AMD64_REPZ || rep_mode == AMD64_REPNZ)) {
            struct amd64_modrm modrm;
            qword_t src_scalar;
            union xmm_reg value;
            if (operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg >= AMD64_XMM_COUNT)
                return INT_UNDEFINED;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                goto amd64_gpf_restore;
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPNZ) {
                value.f64[0] = rex.w ? (double) (sqword_t) src_scalar
                                     : (double) (int32_t) src_scalar;
            } else {
                value.f32[0] = rex.w ? (float) (sqword_t) src_scalar
                                     : (float) (int32_t) src_scalar;
            }
            cpu->xmm[modrm.reg] = value;
            break;
        }
        if (op2 == 0x5a && (rep_mode == AMD64_REPZ || rep_mode == AMD64_REPNZ)) {
            struct amd64_modrm modrm;
            qword_t src_scalar;
            union xmm_reg value;
            if (operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg >= AMD64_XMM_COUNT || (modrm.is_reg && modrm.rm >= AMD64_XMM_COUNT))
                return INT_UNDEFINED;
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPNZ) {
                double src_double;
                if (modrm.is_reg) {
                    src_double = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_gpf_restore;
                    src_double = *(double *) &src_scalar;
                }
                value.f32[0] = (float) src_double;
            } else {
                float src_float;
                uint32_t src_word;
                if (modrm.is_reg) {
                    src_float = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_gpf_restore;
                    src_word = (uint32_t) src_scalar;
                    src_float = *(float *) &src_word;
                }
                value.f64[0] = (double) src_float;
            }
            cpu->xmm[modrm.reg] = value;
            break;
        }
        if ((op2 == 0x2e || op2 == 0x2f) && rep_mode == AMD64_REP_NONE) {
            struct amd64_modrm modrm;
            qword_t src_scalar;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg >= AMD64_XMM_COUNT || (modrm.is_reg && modrm.rm >= AMD64_XMM_COUNT))
                return INT_UNDEFINED;
            if (operand_size_prefix) {
                double lhs, rhs;
                lhs = cpu->xmm[modrm.reg].f64[0];
                if (modrm.is_reg) {
                    rhs = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_gpf_restore;
                    rhs = *(double *) &src_scalar;
                }
                amd64_set_fp_compare_flags(cpu, lhs < rhs ? -1 : (lhs > rhs ? 1 : 0),
                        isnan(lhs) || isnan(rhs));
            } else {
                float lhs, rhs;
                uint32_t src_word;
                lhs = cpu->xmm[modrm.reg].f32[0];
                if (modrm.is_reg) {
                    rhs = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_gpf_restore;
                    src_word = (uint32_t) src_scalar;
                    rhs = *(float *) &src_word;
                }
                amd64_set_fp_compare_flags(cpu, lhs < rhs ? -1 : (lhs > rhs ? 1 : 0),
                        isnan(lhs) || isnan(rhs));
            }
            break;
        }
        if (op2 == 0x10 || op2 == 0x11 || op2 == 0x12 || op2 == 0x13 ||
                op2 == 0x14 || op2 == 0x15 ||
                op2 == 0x16 || op2 == 0x17 ||
                op2 == 0x28 || op2 == 0x29 || op2 == 0x58 || op2 == 0x59 ||
                op2 == 0x5c || op2 == 0x5d || op2 == 0x5e || op2 == 0x54 || op2 == 0x55 ||
                op2 == 0x5f ||
                op2 == 0x56 || op2 == 0x57 || op2 == 0x60 || op2 == 0x61 ||
                op2 == 0x62 || op2 == 0x63 || op2 == 0x67 || op2 == 0x68 || op2 == 0x69 || op2 == 0x6a || op2 == 0x6b || op2 == 0x6c || op2 == 0x6d ||
                op2 == 0x6f || op2 == 0x70 || op2 == 0x7e || op2 == 0x7f ||
                op2 == 0x64 || op2 == 0x65 || op2 == 0x66 || op2 == 0x74 || op2 == 0x75 || op2 == 0x76 || op2 == 0xc2 || op2 == 0xc4 || op2 == 0xc5 || op2 == 0xc6 ||
                op2 == 0xd4 || op2 == 0xd6 || op2 == 0xd7 ||
                op2 == 0xd8 || op2 == 0xd9 || op2 == 0xda || op2 == 0xdb || op2 == 0xdc || op2 == 0xdd || op2 == 0xde || op2 == 0xdf ||
                op2 == 0xeb || op2 == 0xef ||
                op2 == 0xf4 || op2 == 0xf6 ||
                op2 == 0xf8 || op2 == 0xf9 || op2 == 0xfa || op2 == 0xfb ||
                op2 == 0xfc || op2 == 0xfd || op2 == 0xfe) {
            struct amd64_modrm modrm;
            union xmm_reg value;
            union xmm_reg src_xmm;
            qword_t src_scalar;
            uint8_t imm8;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if ((op2 != 0xc5 && modrm.reg >= AMD64_XMM_COUNT) ||
                    (modrm.is_reg && modrm.rm >= AMD64_XMM_COUNT &&
                     !(op2 == 0x7e && operand_size_prefix && rep_mode == AMD64_REP_NONE) &&
                     op2 != 0xc4 && op2 != 0xc5))
                return INT_UNDEFINED;
            if (op2 == 0x10 || op2 == 0x28 || op2 == 0x6f) {
                if (op2 == 0x6f && !(operand_size_prefix || rep_mode == AMD64_REPZ))
                    return INT_UNDEFINED;
                if (op2 == 0x10 && rep_mode == AMD64_REPZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    value = cpu->xmm[modrm.reg];
                    if (modrm.is_reg) {
                        value.u32[0] = cpu->xmm[modrm.rm].u32[0];
                    } else {
                        value.u128 = 0;
                        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                            goto amd64_gpf_restore;
                        value.u32[0] = (uint32_t) src_scalar;
                    }
                    cpu->xmm[modrm.reg] = value;
                } else if (op2 == 0x10 && rep_mode == AMD64_REPNZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    value = cpu->xmm[modrm.reg];
                    if (modrm.is_reg) {
                        value.qw[0] = cpu->xmm[modrm.rm].qw[0];
                    } else {
                        value.u128 = 0;
                        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                            goto amd64_gpf_restore;
                        value.qw[0] = src_scalar;
                    }
                    cpu->xmm[modrm.reg] = value;
                } else {
                    if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                        goto amd64_gpf_restore;
                    cpu->xmm[modrm.reg] = value;
                }
            } else if (op2 == 0x11 || op2 == 0x29 || op2 == 0x7f) {
                if (op2 == 0x7f && !(operand_size_prefix || rep_mode == AMD64_REPZ))
                    return INT_UNDEFINED;
                if (op2 == 0x11 && rep_mode == AMD64_REPZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    if (modrm.is_reg) {
                        cpu->xmm[modrm.rm].u32[0] = cpu->xmm[modrm.reg].u32[0];
                    } else if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 32,
                                   cpu->xmm[modrm.reg].u32[0])) {
                        goto amd64_gpf_restore;
                    }
                } else if (op2 == 0x11 && rep_mode == AMD64_REPNZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    if (modrm.is_reg) {
                        cpu->xmm[modrm.rm].qw[0] = cpu->xmm[modrm.reg].qw[0];
                    } else if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64,
                                   cpu->xmm[modrm.reg].qw[0])) {
                        goto amd64_gpf_restore;
                    }
                } else {
                    value = cpu->xmm[modrm.reg];
                    if (!amd64_write_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                        goto amd64_gpf_restore;
                }
            } else if (op2 == 0x12) {
                if (rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (operand_size_prefix && modrm.is_reg)
                    return INT_UNDEFINED;
                value = cpu->xmm[modrm.reg];
                if (modrm.is_reg) {
                    value.qw[0] = cpu->xmm[modrm.rm].qw[1];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_gpf_restore;
                    value.qw[0] = src_scalar;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x13) {
                if (operand_size_prefix || rep_mode != AMD64_REP_NONE || modrm.is_reg)
                    return INT_UNDEFINED;
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->xmm[modrm.reg].qw[0]))
                    goto amd64_gpf_restore;
            } else if (op2 == 0x14 || op2 == 0x15) {
                if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                if (op2 == 0x14) {
                    value.qw[1] = src_xmm.qw[0];
                } else {
                    value.qw[0] = value.qw[1];
                    value.qw[1] = src_xmm.qw[1];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x58 || op2 == 0x59 || op2 == 0x5c || op2 == 0x5d || op2 == 0x5e || op2 == 0x5f) {
                value = cpu->xmm[modrm.reg];
                if (rep_mode == AMD64_REPZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    {
                        float lhs, rhs;
                        uint32_t src_word;
                        lhs = value.f32[0];
                        if (modrm.is_reg) {
                            rhs = cpu->xmm[modrm.rm].f32[0];
                        } else {
                            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                                goto amd64_gpf_restore;
                            src_word = (uint32_t) src_scalar;
                            rhs = *(float *) &src_word;
                        }
                        switch (op2) {
                        case 0x58:
                            value.f32[0] = lhs + rhs;
                            break;
                        case 0x59:
                            value.f32[0] = lhs * rhs;
                            break;
                        case 0x5c:
                            value.f32[0] = lhs - rhs;
                            break;
                        case 0x5d:
                            value.f32[0] = lhs < rhs ? lhs : rhs;
                            break;
                        case 0x5e:
                            value.f32[0] = lhs / rhs;
                            break;
                        case 0x5f:
                            value.f32[0] = lhs > rhs ? lhs : rhs;
                            break;
                        }
                    }
                } else if (rep_mode == AMD64_REPNZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    {
                        double lhs, rhs;
                        lhs = value.f64[0];
                        if (modrm.is_reg) {
                            rhs = cpu->xmm[modrm.rm].f64[0];
                        } else {
                            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                                goto amd64_gpf_restore;
                            rhs = *(double *) &src_scalar;
                        }
                        switch (op2) {
                        case 0x58:
                            value.f64[0] = lhs + rhs;
                            break;
                        case 0x59:
                            value.f64[0] = lhs * rhs;
                            break;
                        case 0x5c:
                            value.f64[0] = lhs - rhs;
                            break;
                        case 0x5d:
                            value.f64[0] = lhs < rhs ? lhs : rhs;
                            break;
                        case 0x5e:
                            value.f64[0] = lhs / rhs;
                            break;
                        case 0x5f:
                            value.f64[0] = lhs > rhs ? lhs : rhs;
                            break;
                        }
                    }
                } else {
                    if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                        goto amd64_gpf_restore;
                    if (operand_size_prefix) {
                        switch (op2) {
                        case 0x58:
                            value.f64[0] += src_xmm.f64[0];
                            value.f64[1] += src_xmm.f64[1];
                            break;
                        case 0x59:
                            value.f64[0] *= src_xmm.f64[0];
                            value.f64[1] *= src_xmm.f64[1];
                            break;
                        case 0x5c:
                            value.f64[0] -= src_xmm.f64[0];
                            value.f64[1] -= src_xmm.f64[1];
                            break;
                        case 0x5d:
                            value.f64[0] = value.f64[0] < src_xmm.f64[0] ? value.f64[0] : src_xmm.f64[0];
                            value.f64[1] = value.f64[1] < src_xmm.f64[1] ? value.f64[1] : src_xmm.f64[1];
                            break;
                        case 0x5e:
                            value.f64[0] /= src_xmm.f64[0];
                            value.f64[1] /= src_xmm.f64[1];
                            break;
                        case 0x5f:
                            value.f64[0] = value.f64[0] > src_xmm.f64[0] ? value.f64[0] : src_xmm.f64[0];
                            value.f64[1] = value.f64[1] > src_xmm.f64[1] ? value.f64[1] : src_xmm.f64[1];
                            break;
                        }
                    } else {
                        switch (op2) {
                        case 0x58:
                            value.f32[0] += src_xmm.f32[0];
                            value.f32[1] += src_xmm.f32[1];
                            value.f32[2] += src_xmm.f32[2];
                            value.f32[3] += src_xmm.f32[3];
                            break;
                        case 0x59:
                            value.f32[0] *= src_xmm.f32[0];
                            value.f32[1] *= src_xmm.f32[1];
                            value.f32[2] *= src_xmm.f32[2];
                            value.f32[3] *= src_xmm.f32[3];
                            break;
                        case 0x5c:
                            value.f32[0] -= src_xmm.f32[0];
                            value.f32[1] -= src_xmm.f32[1];
                            value.f32[2] -= src_xmm.f32[2];
                            value.f32[3] -= src_xmm.f32[3];
                            break;
                        case 0x5d:
                            value.f32[0] = value.f32[0] < src_xmm.f32[0] ? value.f32[0] : src_xmm.f32[0];
                            value.f32[1] = value.f32[1] < src_xmm.f32[1] ? value.f32[1] : src_xmm.f32[1];
                            value.f32[2] = value.f32[2] < src_xmm.f32[2] ? value.f32[2] : src_xmm.f32[2];
                            value.f32[3] = value.f32[3] < src_xmm.f32[3] ? value.f32[3] : src_xmm.f32[3];
                            break;
                        case 0x5e:
                            value.f32[0] /= src_xmm.f32[0];
                            value.f32[1] /= src_xmm.f32[1];
                            value.f32[2] /= src_xmm.f32[2];
                            value.f32[3] /= src_xmm.f32[3];
                            break;
                        case 0x5f:
                            value.f32[0] = value.f32[0] > src_xmm.f32[0] ? value.f32[0] : src_xmm.f32[0];
                            value.f32[1] = value.f32[1] > src_xmm.f32[1] ? value.f32[1] : src_xmm.f32[1];
                            value.f32[2] = value.f32[2] > src_xmm.f32[2] ? value.f32[2] : src_xmm.f32[2];
                            value.f32[3] = value.f32[3] > src_xmm.f32[3] ? value.f32[3] : src_xmm.f32[3];
                            break;
                        }
                    }
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 >= 0x54 && op2 <= 0x57) {
                if (rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                switch (op2) {
                case 0x54:
                    value.qw[0] &= src_xmm.qw[0];
                    value.qw[1] &= src_xmm.qw[1];
                    break;
                case 0x55:
                    value.qw[0] = ~value.qw[0] & src_xmm.qw[0];
                    value.qw[1] = ~value.qw[1] & src_xmm.qw[1];
                    break;
                case 0x56:
                    value.qw[0] |= src_xmm.qw[0];
                    value.qw[1] |= src_xmm.qw[1];
                    break;
                case 0x57:
                    value.qw[0] ^= src_xmm.qw[0];
                    value.qw[1] ^= src_xmm.qw[1];
                    break;
                }
                cpu->xmm[modrm.reg] = value;
            } else if ((op2 >= 0x64 && op2 <= 0x66) || (op2 >= 0x74 && op2 <= 0x76)) {
                if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                switch (op2) {
                case 0x64:
                    for (int i = 0; i < 16; i++)
                        value.u8[i] = (int8_t) value.u8[i] > (int8_t) src_xmm.u8[i] ? 0xff : 0x00;
                    break;
                case 0x65:
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = (int16_t) value.u16[i] > (int16_t) src_xmm.u16[i] ? 0xffff : 0x0000;
                    break;
                case 0x66:
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = (int32_t) value.u32[i] > (int32_t) src_xmm.u32[i] ? 0xffffffffu : 0;
                    break;
                case 0x74:
                    for (int i = 0; i < 16; i++)
                        value.u8[i] = value.u8[i] == src_xmm.u8[i] ? 0xff : 0x00;
                    break;
                case 0x75:
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = value.u16[i] == src_xmm.u16[i] ? 0xffff : 0x0000;
                    break;
                case 0x76:
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = value.u32[i] == src_xmm.u32[i] ? 0xffffffffu : 0;
                    break;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x16) {
                if (operand_size_prefix && modrm.is_reg)
                    return INT_UNDEFINED;
                value = cpu->xmm[modrm.reg];
                if (modrm.is_reg) {
                    value.qw[1] = cpu->xmm[modrm.rm].qw[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_gpf_restore;
                    value.qw[1] = src_scalar;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x17) {
                if (operand_size_prefix || modrm.is_reg)
                    return INT_UNDEFINED;
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->xmm[modrm.reg].qw[1]))
                    goto amd64_gpf_restore;
            } else if (op2 == 0x60 || op2 == 0x61 || op2 == 0x62 ||
                       op2 == 0x68 || op2 == 0x69 || op2 == 0x6a ||
                       op2 == 0x6c || op2 == 0x6d) {
                union xmm_reg dst = cpu->xmm[modrm.reg];
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                if (op2 == 0x60) {
                    value.u8[0] = dst.u8[0];
                    value.u8[1] = src_xmm.u8[0];
                    value.u8[2] = dst.u8[1];
                    value.u8[3] = src_xmm.u8[1];
                    value.u8[4] = dst.u8[2];
                    value.u8[5] = src_xmm.u8[2];
                    value.u8[6] = dst.u8[3];
                    value.u8[7] = src_xmm.u8[3];
                    value.u8[8] = dst.u8[4];
                    value.u8[9] = src_xmm.u8[4];
                    value.u8[10] = dst.u8[5];
                    value.u8[11] = src_xmm.u8[5];
                    value.u8[12] = dst.u8[6];
                    value.u8[13] = src_xmm.u8[6];
                    value.u8[14] = dst.u8[7];
                    value.u8[15] = src_xmm.u8[7];
                } else if (op2 == 0x61) {
                    value.u16[0] = dst.u16[0];
                    value.u16[1] = src_xmm.u16[0];
                    value.u16[2] = dst.u16[1];
                    value.u16[3] = src_xmm.u16[1];
                    value.u16[4] = dst.u16[2];
                    value.u16[5] = src_xmm.u16[2];
                    value.u16[6] = dst.u16[3];
                    value.u16[7] = src_xmm.u16[3];
                } else if (op2 == 0x62) {
                    value.u32[0] = dst.u32[0];
                    value.u32[1] = src_xmm.u32[0];
                    value.u32[2] = dst.u32[1];
                    value.u32[3] = src_xmm.u32[1];
                } else if (op2 == 0x68) {
                    value.u8[0] = dst.u8[8];
                    value.u8[1] = src_xmm.u8[8];
                    value.u8[2] = dst.u8[9];
                    value.u8[3] = src_xmm.u8[9];
                    value.u8[4] = dst.u8[10];
                    value.u8[5] = src_xmm.u8[10];
                    value.u8[6] = dst.u8[11];
                    value.u8[7] = src_xmm.u8[11];
                    value.u8[8] = dst.u8[12];
                    value.u8[9] = src_xmm.u8[12];
                    value.u8[10] = dst.u8[13];
                    value.u8[11] = src_xmm.u8[13];
                    value.u8[12] = dst.u8[14];
                    value.u8[13] = src_xmm.u8[14];
                    value.u8[14] = dst.u8[15];
                    value.u8[15] = src_xmm.u8[15];
                } else if (op2 == 0x69) {
                    value.u16[0] = dst.u16[4];
                    value.u16[1] = src_xmm.u16[4];
                    value.u16[2] = dst.u16[5];
                    value.u16[3] = src_xmm.u16[5];
                    value.u16[4] = dst.u16[6];
                    value.u16[5] = src_xmm.u16[6];
                    value.u16[6] = dst.u16[7];
                    value.u16[7] = src_xmm.u16[7];
                } else if (op2 == 0x6a) {
                    value.u32[0] = dst.u32[2];
                    value.u32[1] = src_xmm.u32[2];
                    value.u32[2] = dst.u32[3];
                    value.u32[3] = src_xmm.u32[3];
                } else if (op2 == 0x6c) {
                    value = dst;
                    value.qw[1] = src_xmm.qw[0];
                } else {
                    value.qw[0] = dst.qw[1];
                    value.qw[1] = src_xmm.qw[1];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x63 || op2 == 0x67 || op2 == 0x6b) {
                union xmm_reg dst = cpu->xmm[modrm.reg];
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                if (op2 == 0x63) {
                    for (int i = 0; i < 8; i++) {
                        int16_t word = (int16_t) dst.u16[i];
                        value.u8[i] = word > INT8_MAX ? INT8_MAX : word < INT8_MIN ? INT8_MIN : (int8_t) word;
                    }
                    for (int i = 0; i < 8; i++) {
                        int16_t word = (int16_t) src_xmm.u16[i];
                        value.u8[8 + i] = word > INT8_MAX ? INT8_MAX : word < INT8_MIN ? INT8_MIN : (int8_t) word;
                    }
                } else if (op2 == 0x67) {
                    for (int i = 0; i < 8; i++) {
                        int16_t word = (int16_t) dst.u16[i];
                        value.u8[i] = word > UINT8_MAX ? UINT8_MAX : word < 0 ? 0 : (uint8_t) word;
                    }
                    for (int i = 0; i < 8; i++) {
                        int16_t word = (int16_t) src_xmm.u16[i];
                        value.u8[8 + i] = word > UINT8_MAX ? UINT8_MAX : word < 0 ? 0 : (uint8_t) word;
                    }
                } else {
                    for (int i = 0; i < 4; i++) {
                        int32_t dword = (int32_t) dst.u32[i];
                        value.u16[i] = dword > INT16_MAX ? INT16_MAX : dword < INT16_MIN ? INT16_MIN : (int16_t) dword;
                    }
                    for (int i = 0; i < 4; i++) {
                        int32_t dword = (int32_t) src_xmm.u32[i];
                        value.u16[4 + i] = dword > INT16_MAX ? INT16_MAX : dword < INT16_MIN ? INT16_MIN : (int16_t) dword;
                    }
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x7e) {
                if (rep_mode == AMD64_REPZ && !operand_size_prefix) {
                    value.u128 = 0;
                    if (modrm.is_reg) {
                        value.qw[0] = cpu->xmm[modrm.rm].qw[0];
                    } else {
                        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                            goto amd64_gpf_restore;
                        value.qw[0] = src_scalar;
                    }
                    cpu->xmm[modrm.reg] = value;
                } else if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                    qword_t scalar = rex.w ? cpu->xmm[modrm.reg].qw[0]
                                           : cpu->xmm[modrm.reg].u32[0];
                    if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, scalar))
                        goto amd64_gpf_restore;
                } else {
                    return INT_UNDEFINED;
                }
            } else if (op2 == 0x70) {
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                if (operand_size_prefix) {
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = src_xmm.u32[(imm8 >> (i * 2)) & 3];
                } else if (rep_mode == AMD64_REPNZ) {
                    value = src_xmm;
                    for (int i = 0; i < 4; i++)
                        value.u16[i] = src_xmm.u16[(imm8 >> (i * 2)) & 3];
                } else if (rep_mode == AMD64_REPZ) {
                    value = src_xmm;
                    for (int i = 0; i < 4; i++)
                        value.u16[4 + i] = src_xmm.u16[4 + ((imm8 >> (i * 2)) & 3)];
                } else {
                    return INT_UNDEFINED;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xc2) {
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                imm8 &= 7;
                if (rep_mode == AMD64_REPNZ) {
                    vec_single_fcmp64(cpu, &src_xmm.f64[0], &value, imm8);
                } else if (rep_mode == AMD64_REPZ) {
                    vec_single_fcmp32(cpu, &src_xmm.f32[0], &value, imm8);
                } else if (operand_size_prefix) {
                    vec_fcmp_p64(cpu, &src_xmm, &value, imm8);
                } else {
                    for (int i = 0; i < 4; i++) {
                        float lhs = value.f32[i];
                        float rhs = src_xmm.f32[i];
                        switch (imm8) {
                        case 0:
                            value.u32[i] = lhs == rhs ? 0xffffffffu : 0;
                            break;
                        case 1:
                            value.u32[i] = lhs < rhs ? 0xffffffffu : 0;
                            break;
                        case 2:
                            value.u32[i] = lhs <= rhs ? 0xffffffffu : 0;
                            break;
                        case 3:
                            value.u32[i] = isnan(lhs) || isnan(rhs) ? 0xffffffffu : 0;
                            break;
                        case 4:
                            value.u32[i] = lhs != rhs ? 0xffffffffu : 0;
                            break;
                        case 5:
                            value.u32[i] = !(lhs < rhs) ? 0xffffffffu : 0;
                            break;
                        case 6:
                            value.u32[i] = !(lhs <= rhs) ? 0xffffffffu : 0;
                            break;
                        case 7:
                            value.u32[i] = !(isnan(lhs) || isnan(rhs)) ? 0xffffffffu : 0;
                            break;
                        }
                    }
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xc4) {
                if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 16, &src_scalar))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.u16[imm8 & 7] = (uint16_t) src_scalar;
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xc5) {
                if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                amd64_reg_set(cpu, modrm.reg, 32, src_xmm.u16[imm8 & 7]);
            } else if (op2 == 0xc6) {
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                if (operand_size_prefix) {
                    value.qw[0] = cpu->xmm[modrm.reg].qw[(imm8 >> 0) & 1];
                    value.qw[1] = src_xmm.qw[(imm8 >> 1) & 1];
                } else {
                    value.u32[0] = cpu->xmm[modrm.reg].u32[(imm8 >> 0) & 3];
                    value.u32[1] = cpu->xmm[modrm.reg].u32[(imm8 >> 2) & 3];
                    value.u32[2] = src_xmm.u32[(imm8 >> 4) & 3];
                    value.u32[3] = src_xmm.u32[(imm8 >> 6) & 3];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xd6) {
                if (!operand_size_prefix || modrm.is_reg)
                    return INT_UNDEFINED;
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->xmm[modrm.reg].qw[0]))
                    goto amd64_gpf_restore;
            } else if (op2 == 0xd7) {
                uint32_t mask = 0;
                if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                for (int i = 0; i < 16; i++)
                    mask |= ((src_xmm.u8[i] >> 7) & 1u) << i;
                amd64_reg_set(cpu, modrm.reg, 32, mask);
            } else if (op2 == 0xd4) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] += src_xmm.qw[0];
                value.qw[1] += src_xmm.qw[1];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xf4) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] = (uint64_t) value.u32[0] * src_xmm.u32[0];
                value.qw[1] = (uint64_t) value.u32[2] * src_xmm.u32[2];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xfc || op2 == 0xfd || op2 == 0xfe) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                if (op2 == 0xfc) {
                    for (int i = 0; i < 16; i++)
                        value.u8[i] += src_xmm.u8[i];
                } else if (op2 == 0xfd) {
                    for (int i = 0; i < 8; i++)
                        value.u16[i] += src_xmm.u16[i];
                } else {
                    for (int i = 0; i < 4; i++)
                        value.u32[i] += src_xmm.u32[i];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xf6) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value.u128 = 0;
                for (int lane = 0; lane < 2; lane++) {
                    uint16_t sum = 0;
                    for (int i = 0; i < 8; i++) {
                        unsigned idx = lane * 8 + i;
                        uint8_t lhs = cpu->xmm[modrm.reg].u8[idx];
                        uint8_t rhs = src_xmm.u8[idx];
                        sum += lhs > rhs ? (uint16_t) (lhs - rhs) : (uint16_t) (rhs - lhs);
                    }
                    value.u16[lane * 4] = sum;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xf8 || op2 == 0xf9 || op2 == 0xfa || op2 == 0xfb) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                if (op2 == 0xf8) {
                    for (int i = 0; i < 16; i++)
                        value.u8[i] -= src_xmm.u8[i];
                } else if (op2 == 0xf9) {
                    for (int i = 0; i < 8; i++)
                        value.u16[i] -= src_xmm.u16[i];
                } else if (op2 == 0xfa) {
                    for (int i = 0; i < 4; i++)
                        value.u32[i] -= src_xmm.u32[i];
                } else {
                    value.qw[0] -= src_xmm.qw[0];
                    value.qw[1] -= src_xmm.qw[1];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xd8 || op2 == 0xd9 || op2 == 0xdc || op2 == 0xdd || op2 == 0xde) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                if (op2 == 0xd8) {
                    for (int i = 0; i < 16; i++)
                        value.u8[i] = value.u8[i] > src_xmm.u8[i] ? (uint8_t) (value.u8[i] - src_xmm.u8[i]) : 0;
                } else if (op2 == 0xd9) {
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = value.u16[i] > src_xmm.u16[i] ? (uint16_t) (value.u16[i] - src_xmm.u16[i]) : 0;
                } else if (op2 == 0xdc) {
                    for (int i = 0; i < 16; i++) {
                        uint16_t sum = (uint16_t) value.u8[i] + (uint16_t) src_xmm.u8[i];
                        value.u8[i] = sum > 0xff ? 0xff : (uint8_t) sum;
                    }
                } else if (op2 == 0xdd) {
                    for (int i = 0; i < 8; i++) {
                        uint32_t sum = (uint32_t) value.u16[i] + (uint32_t) src_xmm.u16[i];
                        value.u16[i] = sum > 0xffff ? 0xffff : (uint16_t) sum;
                    }
                } else {
                    for (int i = 0; i < 16; i++)
                        value.u8[i] = value.u8[i] > src_xmm.u8[i] ? value.u8[i] : src_xmm.u8[i];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xda) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                for (int i = 0; i < 16; i++)
                    value.u8[i] = value.u8[i] < src_xmm.u8[i] ? value.u8[i] : src_xmm.u8[i];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xdb) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] &= src_xmm.qw[0];
                value.qw[1] &= src_xmm.qw[1];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xdf) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] = ~value.qw[0] & src_xmm.qw[0];
                value.qw[1] = ~value.qw[1] & src_xmm.qw[1];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xeb) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] |= src_xmm.qw[0];
                value.qw[1] |= src_xmm.qw[1];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xef) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] ^= src_xmm.qw[0];
                value.qw[1] ^= src_xmm.qw[1];
                cpu->xmm[modrm.reg] = value;
            } else {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[1] = src_xmm.qw[0];
                cpu->xmm[modrm.reg] = value;
            }
            break;
        }
        if (op2 == 0x71 || op2 == 0x72 || op2 == 0x73) {
            struct amd64_modrm modrm;
            union xmm_reg value;
            uint8_t imm8;
            unsigned count;
            if (!operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!modrm.is_reg || modrm.rm >= AMD64_XMM_COUNT)
                return INT_UNDEFINED;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            value = cpu->xmm[modrm.rm];
            if (op2 == 0x71) {
                count = imm8 > 15 ? 15 : imm8;
                switch (modrm.reg) {
                case 2:
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = imm8 > 15 ? 0 : (value.u16[i] >> count);
                    break;
                case 4:
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = imm8 > 15 ? ((int16_t) value.u16[i] < 0 ? UINT16_MAX : 0)
                                                 : (uint16_t) (((int16_t) value.u16[i]) >> count);
                    break;
                case 6:
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = imm8 > 15 ? 0 : (uint16_t) (value.u16[i] << count);
                    break;
                default:
                    return INT_UNDEFINED;
                }
            } else if (op2 == 0x72) {
                count = imm8 > 31 ? 31 : imm8;
                switch (modrm.reg) {
                case 2:
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = imm8 > 31 ? 0 : (value.u32[i] >> count);
                    break;
                case 4:
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = imm8 > 31 ? ((int32_t) value.u32[i] < 0 ? UINT32_MAX : 0)
                                                 : (uint32_t) (((int32_t) value.u32[i]) >> count);
                    break;
                case 6:
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = imm8 > 31 ? 0 : (value.u32[i] << count);
                    break;
                default:
                    return INT_UNDEFINED;
                }
            } else {
                count = imm8 > 63 ? 63 : imm8;
                switch (modrm.reg) {
                case 2:
                    for (int i = 0; i < 2; i++)
                        value.qw[i] = imm8 > 63 ? 0 : (value.qw[i] >> count);
                    break;
                case 3:
                    if (imm8 >= 16)
                        value.u128 = 0;
                    else
                        value.u128 >>= imm8 * 8;
                    break;
                case 6:
                    for (int i = 0; i < 2; i++)
                        value.qw[i] = imm8 > 63 ? 0 : (value.qw[i] << count);
                    break;
                case 7:
                    if (imm8 >= 16)
                        value.u128 = 0;
                    else
                        value.u128 <<= imm8 * 8;
                    break;
                default:
                    return INT_UNDEFINED;
                }
            }
            cpu->xmm[modrm.rm] = value;
            break;
        }
        if (op2 == 0xa3) {
            struct amd64_modrm modrm;
            qword_t lhs;
            qword_t addr;
            qword_t bit;
            qword_t bit_index;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            bit_index = amd64_reg_get(cpu, modrm.reg, op_size);
            if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size,
                    bit_index, true, true, &lhs, &addr, &bit))
                goto amd64_gpf_restore;
            (void) addr;
            collapse_flags(cpu);
            cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
            amd64_trace_as_bt(cpu, op2, op_size, !modrm.is_reg, bit_index, bit, addr, lhs, lhs);
            cpu->cf_bit = cpu->cf;
            break;
        }
        if (op2 == 0xab || op2 == 0xb3 || op2 == 0xbb) {
            struct amd64_modrm modrm;
            qword_t addr;
            qword_t lhs, result;
            qword_t bit;
            qword_t bit_index;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            bit_index = amd64_reg_get(cpu, modrm.reg, op_size);
            if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size,
                    bit_index, true, true, &lhs, &addr, &bit))
                goto amd64_gpf_restore;
            collapse_flags(cpu);
            cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
            result = lhs;
            switch (op2) {
            case 0xab:
                result = amd64_trunc(lhs | (1ull << bit), op_size);
                break;
            case 0xb3:
                result = amd64_trunc(lhs & ~(1ull << bit), op_size);
                break;
            case 0xbb:
                result = amd64_trunc(lhs ^ (1ull << bit), op_size);
                break;
            }
            amd64_trace_as_bt(cpu, op2, op_size, !modrm.is_reg, bit_index, bit, addr, lhs, result);
            if (!amd64_write_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, addr, result))
                goto amd64_gpf_restore;
            if (!modrm.is_reg && op_size == 64)
                amd64_trace_qword_store(cpu, saved_rip, 0x0f, addr, result);
            cpu->cf_bit = cpu->cf;
            break;
        }
        if (op2 == 0xba) {
            struct amd64_modrm modrm;
            qword_t addr;
            qword_t lhs, result;
            qword_t bit;
            uint8_t imm8;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg < 4 || modrm.reg > 7)
                return INT_UNDEFINED;
            if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, imm8,
                    true, false, &lhs, &addr, &bit))
                goto amd64_gpf_restore;
            collapse_flags(cpu);
            cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
            result = lhs;
            switch (modrm.reg) {
            case 4:
                break;
            case 5:
                result = amd64_trunc(lhs | (1ull << bit), op_size);
                break;
            case 6:
                result = amd64_trunc(lhs & ~(1ull << bit), op_size);
                break;
            case 7:
                result = amd64_trunc(lhs ^ (1ull << bit), op_size);
                break;
            }
            amd64_trace_as_bt(cpu, op2, op_size, !modrm.is_reg, imm8, bit, addr, lhs, result);
            if (modrm.reg != 4) {
                if (!amd64_write_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, addr, result))
                    goto amd64_gpf_restore;
                if (!modrm.is_reg && op_size == 64)
                    amd64_trace_qword_store(cpu, saved_rip, 0x0f, addr, result);
            }
            cpu->cf_bit = cpu->cf;
            break;
        }
        if (op2 == 0xaf) {
            struct amd64_modrm modrm;
            qword_t rhs, lhs, result;
            sqword_t signed_result;
            bool overflow;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            signed_result = amd64_sign_extend(lhs, op_size) * amd64_sign_extend(rhs, op_size);
            result = amd64_trunc((qword_t) signed_result, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            overflow = signed_result != amd64_sign_extend(result, op_size);
            amd64_set_mul_flags(cpu, overflow);
            break;
        }
        if (op2 == 0xc0 || op2 == 0xc1) {
            struct amd64_modrm modrm;
            unsigned xadd_size = op2 == 0xc0 ? 8 : op_size;
            qword_t lhs, rhs, result;
            bool atomic_locked = false;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            atomic_locked = lock_prefix && !modrm.is_reg;
            if (atomic_locked)
                lock(&atomic_l_lock, 0);
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, xadd_size, &lhs)) {
                if (atomic_locked)
                    unlock(&atomic_l_lock);
                goto amd64_gpf_restore;
            }
            rhs = op2 == 0xc0
                    ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                    : amd64_reg_get(cpu, modrm.reg, xadd_size);
            result = amd64_trunc(lhs + rhs, xadd_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, xadd_size, result)) {
                if (atomic_locked)
                    unlock(&atomic_l_lock);
                goto amd64_gpf_restore;
            }
            if (atomic_locked)
                unlock(&atomic_l_lock);
            if (op2 == 0xc0)
                amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, lhs);
            else
                amd64_reg_set(cpu, modrm.reg, xadd_size, lhs);
            amd64_set_add_flags(cpu, lhs, rhs, result, xadd_size);
            break;
        }
        if (op2 == 0xb0 || op2 == 0xb1) {
            struct amd64_modrm modrm;
            qword_t dst, src, acc, result;
            unsigned cmpxchg_size = op2 == 0xb0 ? 8 : op_size;
            bool atomic_locked = false;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            atomic_locked = lock_prefix && !modrm.is_reg;
            if (atomic_locked)
                lock(&atomic_l_lock, 0);
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, cmpxchg_size, &dst)) {
                if (atomic_locked)
                    unlock(&atomic_l_lock);
                goto amd64_gpf_restore;
            }
            src = op2 == 0xb0
                    ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                    : amd64_reg_get(cpu, modrm.reg, cmpxchg_size);
            acc = amd64_reg_get(cpu, amd64_rax, cmpxchg_size);
            result = amd64_trunc(acc - dst, cmpxchg_size);
            amd64_set_sub_flags(cpu, acc, dst, result, cmpxchg_size);
            if (acc == dst) {
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, cmpxchg_size, src)) {
                    if (atomic_locked)
                        unlock(&atomic_l_lock);
                    goto amd64_gpf_restore;
                }
                if (!modrm.is_reg && cmpxchg_size == 64)
                    amd64_trace_qword_store(cpu, saved_rip, 0x0f, amd64_effective_addr(cpu, &modrm, fs_prefix), src);
                cpu->zf = 1;
                cpu->zf_res = 0;
            } else {
                amd64_reg_set(cpu, amd64_rax, cmpxchg_size, dst);
                cpu->zf = 0;
                cpu->zf_res = 0;
            }
            if (atomic_locked)
                unlock(&atomic_l_lock);
            break;
        }
        if (op2 == 0xc7) {
            struct amd64_modrm modrm;
            qword_t dst, expected, desired;
            bool atomic_locked = false;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg != 1 || modrm.is_reg)
                return INT_UNDEFINED;

            atomic_locked = lock_prefix;
            if (atomic_locked)
                lock(&atomic_l_lock, 0);
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &dst)) {
                if (atomic_locked)
                    unlock(&atomic_l_lock);
                goto amd64_gpf_restore;
            }

            expected = ((qword_t) (dword_t) amd64_reg_get(cpu, amd64_rdx, 32) << 32) |
                    (dword_t) amd64_reg_get(cpu, amd64_rax, 32);
            desired = ((qword_t) (dword_t) amd64_reg_get(cpu, amd64_rcx, 32) << 32) |
                    (dword_t) amd64_reg_get(cpu, amd64_rbx, 32);
            collapse_flags(cpu);
            cpu->zf = expected == dst;
            cpu->zf_res = 0;
            if (expected == dst) {
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, desired)) {
                    if (atomic_locked)
                        unlock(&atomic_l_lock);
                    goto amd64_gpf_restore;
                }
                amd64_trace_qword_store(cpu, saved_rip, 0x0f,
                        amd64_effective_addr(cpu, &modrm, fs_prefix), desired);
            } else {
                amd64_reg_set(cpu, amd64_rax, 32, (dword_t) dst);
                amd64_reg_set(cpu, amd64_rdx, 32, (dword_t) (dst >> 32));
            }
            if (atomic_locked)
                unlock(&atomic_l_lock);
            break;
        }
        if (op2 >= 0xc8 && op2 <= 0xcf) {
            unsigned reg = (op2 - 0xc8) | (rex.b ? 8 : 0);
            if (rex.w) {
                qword_t value = amd64_reg_get(cpu, reg, 64);
                amd64_reg_set(cpu, reg, 64, __builtin_bswap64(value));
            } else {
                dword_t value = (dword_t) amd64_reg_get(cpu, reg, 32);
                amd64_reg_set(cpu, reg, 32, __builtin_bswap32(value));
            }
            break;
        }
        if (op2 == 0x1f) {
            struct amd64_modrm modrm;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg != 0)
                return INT_UNDEFINED;
            break;
        }
        if (op2 == 0x0b)
            return INT_UNDEFINED;
        return INT_UNDEFINED;
    }
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
    case 0xdc:
    case 0xdd:
    case 0xde:
    case 0xdf:
        return amd64_handle_x87(cpu, tlb, saved_rip, rex, fs_prefix, opcode);
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x08:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x18:
    case 0x19:
    case 0x1a:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x09:
    case 0x0a:
    case 0x0b:
    case 0x03:
    case 0x13:
    case 0x1b:
    case 0x23:
    case 0x28:
    case 0x2a:
    case 0x2b:
    case 0x29:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x39:
    case 0x3b:
    case 0x85:
    case 0x86:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x8b:
    case 0x8d:
    case 0x63:
    case 0x69:
    case 0x6b: {
        struct amd64_modrm modrm;
        qword_t lhs, rhs, result;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        switch (opcode) {
        case 0x00:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs + rhs, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_add_flags(cpu, lhs, rhs, result, 8);
            break;
        case 0x01:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs + rhs, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_add_flags(cpu, lhs, rhs, result, op_size);
            break;
        case 0x02:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs + rhs, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_add_flags(cpu, lhs, rhs, result, 8);
            break;
        case 0x08:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs | rhs, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x10: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs + rhs + carry_in, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, 8);
            break;
        }
        case 0x11: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs + rhs + carry_in, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, op_size);
            break;
        }
        case 0x12: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs + rhs + carry_in, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, 8);
            break;
        }
        case 0x18: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs - rhs - carry_in, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, 8);
            break;
        }
        case 0x19: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs - carry_in, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, op_size);
            break;
        }
        case 0x1a: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs - rhs - carry_in, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, 8);
            break;
        }
        case 0x20:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs & rhs, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x21:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs & rhs, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x22:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs & rhs, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x09:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs | rhs, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x0a:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs | rhs, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x0b:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs | rhs, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x03:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs + rhs, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_add_flags(cpu, lhs, rhs, result, op_size);
            break;
        case 0x13: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs + rhs + carry_in, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, op_size);
            break;
        }
        case 0x1b: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs - carry_in, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, op_size);
            break;
        }
        case 0x23:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs & rhs, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x28:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs - rhs, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_sub_flags(cpu, lhs, rhs, result, 8);
            break;
        case 0x2b:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_sub_flags(cpu, lhs, rhs, result, op_size);
            break;
        case 0x29:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_sub_flags(cpu, lhs, rhs, result, op_size);
            break;
        case 0x2a:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs - rhs, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_sub_flags(cpu, lhs, rhs, result, 8);
            break;
        case 0x30:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs ^ rhs, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x31:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs ^ rhs, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x32:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs ^ rhs, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x33:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs ^ rhs, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x39:
            if (amd64_verbose_boot_trace_enabled() &&
                    saved_rip == AMD64_BUSYBOX_INIT_CMP_RIP && !modrm.is_reg) {
                amd64_busybox_watch_addr(amd64_reg_get(cpu, amd64_rax, 64));
                printk("amd64 init cmp: rip=%#llx rax=%#llx rbx=%#llx addr=%#llx\n",
                       (unsigned long long) saved_rip,
                       (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rbx, 64),
                       (unsigned long long) amd64_effective_addr(cpu, &modrm, fs_prefix));
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs, op_size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, op_size);
            if (!modrm.is_reg) {
                qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                amd64_trace_cc1_cmp_probe(cpu, saved_rip, addr, lhs, rhs, result, op_size);
            }
            break;
        case 0x3b:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs, op_size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, op_size);
            break;
        case 0x85:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            amd64_set_logic_flags(cpu, lhs & rhs, op_size);
            if (amd64_verbose_boot_trace_enabled() && saved_rip == AMD64_BUSYBOX_INIT_TEST_RIP) {
                printk("amd64 init test: rip=%#llx lhs=%#llx rhs=%#llx zf=%d rax=%#llx\n",
                       (unsigned long long) saved_rip,
                       (unsigned long long) lhs,
                       (unsigned long long) rhs,
                       cpu->zf,
                       (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64));
            }
            break;
        case 0x86:
        case 0x87: {
            unsigned xchg_size = opcode == 0x86 ? 8 : op_size;
            bool atomic_locked = !modrm.is_reg;
            if (atomic_locked)
                lock(&atomic_l_lock, 0);
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, xchg_size, &lhs)) {
                if (atomic_locked)
                    unlock(&atomic_l_lock);
                goto amd64_gpf_restore;
            }
            rhs = opcode == 0x86 ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                                 : amd64_reg_get(cpu, modrm.reg, xchg_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, xchg_size, rhs)) {
                if (atomic_locked)
                    unlock(&atomic_l_lock);
                goto amd64_gpf_restore;
            }
            if (atomic_locked)
                unlock(&atomic_l_lock);
            if (opcode == 0x86)
                amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, lhs);
            else
                amd64_reg_set(cpu, modrm.reg, xchg_size, lhs);
            break;
        }
        case 0x88:
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, rhs))
                goto amd64_gpf_restore;
            break;
        case 0x89:
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, rhs))
                goto amd64_gpf_restore;
            if (!modrm.is_reg && op_size == 64)
                amd64_trace_qword_store(cpu, saved_rip, opcode,
                        amd64_effective_addr(cpu, &modrm, fs_prefix), rhs);
            break;
        case 0x8a:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, rhs);
            break;
        case 0x8b:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            if (!modrm.is_reg && op_size == 64)
                amd64_trace_cc1_slot_probe(cpu, saved_rip, amd64_effective_addr(cpu, &modrm, fs_prefix), rhs);
            amd64_reg_set(cpu, modrm.reg, op_size, rhs);
            if (amd64_verbose_boot_trace_enabled() && saved_rip == AMD64_BUSYBOX_INIT_LOAD_RIP) {
                amd64_busybox_watch_addr(rhs);
                printk("amd64 init load: rip=%#llx dst=%u value=%#llx rax=%#llx\n",
                       (unsigned long long) saved_rip,
                       modrm.reg,
                       (unsigned long long) rhs,
                       (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64));
            }
            break;
        case 0x8d:
            if (modrm.is_reg)
                return INT_UNDEFINED;
            amd64_reg_set(cpu, modrm.reg, op_size, amd64_effective_addr(cpu, &modrm, false));
            break;
        case 0x63:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 32 : op_size, &rhs))
                goto amd64_gpf_restore;
            amd64_reg_set(cpu, modrm.reg, op_size,
                    (qword_t) amd64_sign_extend(rhs, rex.w ? 32 : op_size));
            break;
        case 0x69:
        case 0x6b: {
            sqword_t src_signed;
            sqword_t imm_signed;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            if (opcode == 0x69) {
                if (op_size == 16) {
                    int16_t imm16;
                    if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
                        cpu->amd64_rip = saved_rip;
                        cpu->segfault_addr = saved_rip;
                        return INT_GPF;
                    }
                    imm_signed = imm16;
                } else {
                    int32_t imm32;
                    if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                        cpu->amd64_rip = saved_rip;
                        cpu->segfault_addr = saved_rip;
                        return INT_GPF;
                    }
                    imm_signed = imm32;
                }
            } else {
                int8_t imm8;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                imm_signed = imm8;
            }
            src_signed = amd64_sign_extend(rhs, op_size);
            if (op_size == 64) {
                __int128_t full = (__int128_t) src_signed * (__int128_t) imm_signed;
                result = (qword_t) full;
                amd64_reg_set(cpu, modrm.reg, op_size, result);
                amd64_set_mul_flags(cpu, full != (__int128_t) (sqword_t) (uint64_t) result);
            } else {
                int64_t full = (int64_t) src_signed * (int64_t) imm_signed;
                result = amd64_trunc((qword_t) full, op_size);
                amd64_reg_set(cpu, modrm.reg, op_size, result);
                amd64_set_mul_flags(cpu, full != (int64_t) amd64_sign_extend(result, op_size));
            }
            break;
        }
        }
        break;
    }
    case 0x50 ... 0x57: {
        unsigned reg = (opcode - 0x50) | (rex.b ? 8 : 0);
        unsigned push_size = operand_size_prefix ? 16 : 64;
        qword_t value = amd64_reg_get(cpu, reg, push_size);
        if (!amd64_push_size(cpu, tlb, push_size, value))
            goto amd64_gpf_restore;
        break;
    }
    case 0x58 ... 0x5f: {
        unsigned reg = (opcode - 0x58) | (rex.b ? 8 : 0);
        unsigned pop_size = operand_size_prefix ? 16 : 64;
        qword_t value;
        if (!amd64_pop_size(cpu, tlb, pop_size, &value))
            goto amd64_gpf_restore;
        amd64_reg_set(cpu, reg, pop_size, value);
        break;
    }
    case 0x9c: {
        unsigned push_size = operand_size_prefix ? 16 : 64;
        collapse_flags(cpu);
        if (!amd64_push_size(cpu, tlb, push_size, cpu->eflags))
            goto amd64_gpf_restore;
        break;
    }
    case 0x9d: {
        unsigned pop_size = operand_size_prefix ? 16 : 64;
        qword_t value;
        if (!amd64_pop_size(cpu, tlb, pop_size, &value))
            goto amd64_gpf_restore;
        cpu->eflags = (cpu->eflags & ~0xcd5u) | ((dword_t) value & 0xcd5u);
        expand_flags(cpu);
        break;
    }
    case 0x8f: {
        struct amd64_modrm modrm;
        qword_t value;
        unsigned pop_size = operand_size_prefix ? 16 : 64;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (modrm.reg != 0)
            return INT_UNDEFINED;
        if (!amd64_pop_size(cpu, tlb, pop_size, &value))
            goto amd64_gpf_restore;
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, pop_size, value))
            goto amd64_gpf_restore;
        break;
    }
    case 0x68: {
        unsigned push_size = operand_size_prefix ? 16 : 64;
        qword_t value;
        if (push_size == 16) {
            uint16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            value = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            value = (qword_t) (sqword_t) imm32;
        }
        if (!amd64_push_size(cpu, tlb, push_size, value))
            goto amd64_gpf_restore;
        break;
    }
    case 0x6a: {
        unsigned push_size = operand_size_prefix ? 16 : 64;
        int8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!amd64_push_size(cpu, tlb, push_size, (qword_t) amd64_sign_extend((uint8_t) imm8, 8)))
            goto amd64_gpf_restore;
        break;
    }
    case 0x84: {
        struct amd64_modrm modrm;
        qword_t lhs, rhs;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
            goto amd64_gpf_restore;
        rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
        amd64_set_logic_flags(cpu, lhs & rhs, 8);
        break;
    }
    case 0x38:
    case 0x3a: {
        struct amd64_modrm modrm;
        qword_t lhs, rhs;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (opcode == 0x38) {
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
        } else {
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
        }
        amd64_set_sub_flags(cpu, lhs, rhs, amd64_trunc(lhs - rhs, 8), 8);
        break;
    }
    case 0xf6:
    case 0xf7: {
        struct amd64_modrm modrm;
        unsigned size = opcode == 0xf6 ? 8 : op_size;
        int result;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (modrm.reg == 0) {
            qword_t lhs, rhs;
            if (opcode == 0xf6) {
                uint8_t imm8;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                rhs = imm8;
            } else {
                if (size == 16) {
                    uint16_t imm16;
                    if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
                        cpu->amd64_rip = saved_rip;
                        cpu->segfault_addr = saved_rip;
                        return INT_GPF;
                    }
                    rhs = imm16;
                } else {
                    int32_t imm32;
                    if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                        cpu->amd64_rip = saved_rip;
                        cpu->segfault_addr = saved_rip;
                        return INT_GPF;
                    }
                    rhs = rex.w ? (qword_t) (sqword_t) imm32 : (uint32_t) imm32;
                }
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, size, &lhs))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, lhs & rhs, size);
            break;
        }
        result = amd64_grp3_muldiv(cpu, tlb, &modrm, fs_prefix, size);
        if (result == INT_PF)
            goto amd64_gpf_restore;
        if (result != INT_NONE)
            return result;
        break;
    }
    case 0x70 ... 0x7f: {
        int8_t rel8;
        if (!amd64_fetch(cpu, tlb, &rel8, sizeof(rel8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        bool taken = amd64_cond_eval(cpu, opcode & 0xf);
        if (amd64_as_alu_stderr_enabled() &&
                current != NULL &&
                current->abi == GUEST_ABI_AMD64 &&
                strcmp(current->comm, "as") == 0) {
            fprintf(stderr,
                    "amd64 as jcc8: rip=%#llx cc=%u taken=%u target=%#llx cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                    (unsigned long long) saved_rip,
                    opcode & 0xf,
                    taken,
                    (unsigned long long) (cpu->amd64_rip + rel8),
                    cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af);
        }
        amd64_trace_cc1_va_list_branch_probe(cpu, saved_rip, taken, cpu->amd64_rip + rel8, "jcc");
        if (amd64_verbose_boot_trace_enabled() && saved_rip == AMD64_BUSYBOX_INIT_JNE_RIP) {
            printk("amd64 init jne: rip=%#llx zf=%d taken=%d rax=%#llx target=%#llx\n",
                   (unsigned long long) saved_rip,
                   cpu->zf,
                   taken,
                   (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64),
                   (unsigned long long) (cpu->amd64_rip + rel8));
        }
        if (taken) {
            qword_t target = cpu->amd64_rip + rel8;
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "jcc");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
            amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "jcc");
            cpu->amd64_rip = target;
        }
        break;
    }
    case 0x80:
    case 0xc0:
    case 0x81:
    case 0x83:
    case 0xc1:
    case 0xc6:
    case 0xc7: {
        struct amd64_modrm modrm;
        qword_t lhs, rhs, result;
        unsigned rm_size = (opcode == 0x80 || opcode == 0xc0) ? 8 : op_size;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if ((opcode == 0xc6 || opcode == 0xc7) && modrm.reg != 0)
            return INT_UNDEFINED;
        if (opcode == 0xc6) {
            uint8_t imm8;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, imm8))
                goto amd64_gpf_restore;
            break;
        }
        if (opcode == 0xc0 || opcode == 0xc1) {
            uint8_t imm8;
            unsigned count, effective_count;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            count = imm8 & (rm_size == 64 ? 0x3f : 0x1f);
            effective_count = (modrm.reg == 0 || modrm.reg == 1) ? (count % rm_size) :
                ((modrm.reg == 2 || modrm.reg == 3) ? amd64_rotate_carry_count(rm_size, count) : count);
            if (effective_count == 0)
                break;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
                goto amd64_gpf_restore;
            switch (modrm.reg) {
            case 0:
            case 1:
                result = amd64_rotate_value(lhs, rm_size, count, modrm.reg);
                break;
            case 2:
            case 3:
                result = amd64_rotate_carry_value(cpu, lhs, rm_size, count, modrm.reg);
                break;
            case 4:
                result = amd64_trunc(lhs << count, rm_size);
                break;
            case 5:
                result = amd64_trunc(amd64_trunc(lhs, rm_size) >> count, rm_size);
                break;
            case 7:
                result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, rm_size) >> count), rm_size);
                break;
            default:
                return INT_UNDEFINED;
            }
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            if (modrm.reg == 0 || modrm.reg == 1)
                amd64_set_rotate_flags(cpu, result, rm_size, count, modrm.reg);
            else if (modrm.reg != 2 && modrm.reg != 3)
                amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
            break;
        }
        if (opcode == 0x80) {
            uint8_t imm8;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            rhs = imm8;
        } else if (opcode == 0x83) {
            int8_t imm8;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            rhs = (qword_t) amd64_sign_extend((uint8_t) imm8, 8);
        } else if (op_size == 16 && (opcode == 0x81 || opcode == 0xc7)) {
            uint16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            rhs = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            rhs = opcode == 0xc7 && !rex.w ? (uint32_t) imm32 : (qword_t) (sqword_t) imm32;
        }
        if (opcode == 0xc6 || opcode == 0xc7) {
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, rhs))
                goto amd64_gpf_restore;
            if (!modrm.is_reg && op_size == 64)
                amd64_trace_qword_store(cpu, saved_rip, opcode,
                        amd64_effective_addr(cpu, &modrm, fs_prefix), rhs);
            break;
        }

        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
            goto amd64_gpf_restore;

        bool trace_as_alu = amd64_as_alu_stderr_enabled() &&
            current != NULL &&
            current->abi == GUEST_ABI_AMD64 &&
            strcmp(current->comm, "as") == 0 &&
            (opcode == 0x81 || opcode == 0x83) &&
            modrm.is_reg &&
            rm_size == 32;
        if (trace_as_alu) {
            uint8_t insn_bytes[8] = {};
            bool have_insn_bytes = amd64_trace_read_guest(saved_rip, insn_bytes, sizeof(insn_bytes));
            fprintf(stderr,
                    "amd64 as alu: pre rip=%#llx subop=%u rm=%u lhs=%#llx rhs=%#llx cf=%u zf=%u sf=%u of=%u pf=%u af=%u%s\n",
                    (unsigned long long) saved_rip,
                    modrm.reg,
                    modrm.rm,
                    (unsigned long long) lhs,
                    (unsigned long long) rhs,
                    cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af,
                    have_insn_bytes ? "" : " bytes=?");
            if (have_insn_bytes) {
                fprintf(stderr,
                        "amd64 as alu: bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                        insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
                        insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
            }
        }

        switch (modrm.reg) {
        case 0:
            result = amd64_trunc(lhs + rhs, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_add_flags(cpu, lhs, rhs, result, rm_size);
            break;
        case 1:
            result = amd64_trunc(lhs | rhs, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, rm_size);
            break;
        case 2: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs + rhs + carry_in, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, rm_size);
            break;
        }
        case 3: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs - rhs - carry_in, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, rm_size);
            break;
        }
        case 4:
            result = amd64_trunc(lhs & rhs, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, rm_size);
            break;
        case 5:
            result = amd64_trunc(lhs - rhs, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_sub_flags(cpu, lhs, rhs, result, rm_size);
            break;
        case 6:
            result = amd64_trunc(lhs ^ rhs, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, rm_size);
            break;
        case 7:
            result = amd64_trunc(lhs - rhs, rm_size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, rm_size);
            break;
        default:
            return INT_UNDEFINED;
        }
        if (trace_as_alu) {
            fprintf(stderr,
                    "amd64 as alu: post rip=%#llx subop=%u rm=%u result=%#llx reg=%#llx cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                    (unsigned long long) saved_rip,
                    modrm.reg,
                    modrm.rm,
                    (unsigned long long) result,
                    (unsigned long long) amd64_reg_get(cpu, modrm.rm, rm_size),
                    cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af);
        }
        break;
    }
    case 0x90 ... 0x97: {
        unsigned reg = (opcode - 0x90) | (rex.b ? 8 : 0);
        if (reg != amd64_rax) {
            qword_t lhs = amd64_reg_get(cpu, amd64_rax, op_size);
            qword_t rhs = amd64_reg_get(cpu, reg, op_size);
            amd64_reg_set(cpu, amd64_rax, op_size, rhs);
            amd64_reg_set(cpu, reg, op_size, lhs);
        }
        break;
    }
    case 0x98:
        if (rex.w) {
            amd64_reg_set(cpu, amd64_rax, 64, (qword_t) (sqword_t) (int32_t) amd64_reg_get(cpu, amd64_rax, 32));
        } else if (operand_size_prefix) {
            amd64_reg_set(cpu, amd64_rax, 16, (word_t) (int16_t) amd64_reg_get(cpu, amd64_rax, 8));
        } else {
            amd64_reg_set(cpu, amd64_rax, 32, (dword_t) (int16_t) amd64_reg_get(cpu, amd64_rax, 16));
        }
        break;
    case 0x99:
        if (rex.w) {
            amd64_reg_set(cpu, amd64_rdx, 64,
                    ((sqword_t) amd64_reg_get(cpu, amd64_rax, 64) < 0) ? ~0ull : 0);
        } else if (operand_size_prefix) {
            amd64_reg_set(cpu, amd64_rdx, 16,
                    ((int16_t) amd64_reg_get(cpu, amd64_rax, 16) < 0) ? 0xffff : 0);
        } else {
            amd64_reg_set(cpu, amd64_rdx, 32,
                    ((int32_t) amd64_reg_get(cpu, amd64_rax, 32) < 0) ? 0xffffffffu : 0);
        }
        break;
    case 0x1f: {
        // Toolchains use 0f 1f /0 for alignment NOPs. Be tolerant if dispatch
        // lands on the second byte and consume the ModRM form here as well.
        struct amd64_modrm modrm;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (modrm.reg != 0)
            return INT_UNDEFINED;
        break;
    }
    case 0x04:
    case 0x05:
    case 0x0c:
    case 0x0d:
    case 0x14:
    case 0x15:
    case 0x1c:
    case 0x1d:
    case 0x24:
    case 0x25:
    case 0x2c:
    case 0x2d:
    case 0x34:
    case 0x35:
    case 0x3c:
    case 0x3d: {
        unsigned size = (opcode & 0x1) == 0 ? 8 : op_size;
        qword_t lhs = amd64_reg_get(cpu, amd64_rax, size);
        qword_t rhs;
        qword_t result;
        unsigned carry_in;
        if (!amd64_fetch_accum_imm(cpu, tlb, size, true, &rhs)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        switch (opcode) {
        case 0x04:
        case 0x05:
            result = amd64_trunc(lhs + rhs, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_add_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x0c:
        case 0x0d:
            result = amd64_trunc(lhs | rhs, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x14:
        case 0x15:
            carry_in = cpu->cf;
            result = amd64_trunc(lhs + rhs + carry_in, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        case 0x1c:
        case 0x1d:
            carry_in = cpu->cf;
            result = amd64_trunc(lhs - rhs - carry_in, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        case 0x24:
        case 0x25:
            result = amd64_trunc(lhs & rhs, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x2c:
        case 0x2d:
            result = amd64_trunc(lhs - rhs, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_sub_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x34:
        case 0x35:
            result = amd64_trunc(lhs ^ rhs, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x3c:
        case 0x3d:
            result = amd64_trunc(lhs - rhs, size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, size);
            break;
        default:
            return INT_UNDEFINED;
        }
        break;
    }
    case 0xa9: {
        qword_t lhs = amd64_reg_get(cpu, amd64_rax, op_size);
        qword_t rhs;
        if (!amd64_fetch_accum_imm(cpu, tlb, op_size, true, &rhs)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        amd64_set_logic_flags(cpu, lhs & rhs, op_size);
        break;
    }
    case 0xa8: {
        uint8_t imm8;
        qword_t lhs = amd64_reg_get(cpu, amd64_rax, 8);
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        amd64_set_logic_flags(cpu, lhs & imm8, 8);
        break;
    }
    case 0xb0 ... 0xb7: {
        unsigned reg = (opcode - 0xb0) | (rex.b ? 8 : 0);
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        amd64_reg_set_encoded8(cpu, reg, rex.present, imm8);
        break;
    }
    case 0xb8 ... 0xbf: {
        unsigned reg = (opcode - 0xb8) | (rex.b ? 8 : 0);
        if (rex.w) {
            uint64_t imm64;
            if (!amd64_fetch_u64(cpu, tlb, &imm64)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            amd64_reg_set(cpu, reg, 64, imm64);
        } else {
            uint32_t imm32;
            if (!amd64_fetch_u32(cpu, tlb, &imm32)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            amd64_reg_set(cpu, reg, 32, imm32);
        }
        break;
    }
    case 0xc2: {
        uint16_t imm16;
        qword_t target;
        if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!amd64_pop(cpu, tlb, &target))
            goto amd64_gpf_restore;
        qword_t old_rsp = cpu->amd64_regs[amd64_rsp];
        cpu->amd64_regs[amd64_rsp] = old_rsp + imm16;
        amd64_trace_suspicious_rsp_write(cpu, old_rsp, cpu->amd64_regs[amd64_rsp], 64);
        amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, target, "ret-imm");
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "ret-imm");
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "ret");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        cpu->amd64_rip = target;
        break;
    }
    case 0xc3: {
        qword_t target;
        if (!amd64_pop(cpu, tlb, &target))
            goto amd64_gpf_restore;
        amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, target, "ret");
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "ret");
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "ret");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        cpu->amd64_rip = target;
        break;
    }
    case 0xc9: {
        unsigned pop_size = operand_size_prefix ? 16 : 64;
        qword_t value;
        qword_t old_rsp = cpu->amd64_regs[amd64_rsp];
        cpu->amd64_regs[amd64_rsp] = cpu->amd64_regs[amd64_rbp];
        amd64_trace_suspicious_rsp_write(cpu, old_rsp, cpu->amd64_regs[amd64_rsp], 64);
        amd64_trace_as_stack(amd64_as_stack_leave, pop_size, old_rsp, cpu->amd64_regs[amd64_rsp],
                cpu->amd64_regs[amd64_rbp]);
        if (!amd64_pop_size(cpu, tlb, pop_size, &value))
            goto amd64_gpf_restore;
        amd64_reg_set(cpu, amd64_rbp, pop_size, value);
        break;
    }
    case 0xf4:
        return INT_PRIV;
    case 0xf5:
        cpu->cf = !cpu->cf;
        cpu->cf_bit = cpu->cf;
        break;
    case 0xf8:
        cpu->cf = 0;
        cpu->cf_bit = 0;
        break;
    case 0xf9:
        cpu->cf = 1;
        cpu->cf_bit = 1;
        break;
    case 0xfa:
    case 0xfb:
        return INT_PRIV;
    case 0xfc:
        cpu->df = 0;
        cpu->df_offset = 1;
        break;
    case 0xfd:
        cpu->df = 1;
        cpu->df_offset = -1;
        break;
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3: {
        struct amd64_modrm modrm;
        qword_t lhs, result;
        unsigned count, effective_count;
        unsigned rm_size = (opcode == 0xd0 || opcode == 0xd2) ? 8 : op_size;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
            goto amd64_gpf_restore;
        count = (opcode == 0xd0 || opcode == 0xd1) ? 1 :
            (amd64_reg_get(cpu, amd64_rcx, 8) & (rm_size == 64 ? 0x3f : 0x1f));
        effective_count = (modrm.reg == 0 || modrm.reg == 1) ? (count % rm_size) :
            ((modrm.reg == 2 || modrm.reg == 3) ? amd64_rotate_carry_count(rm_size, count) : count);
        bool trace_as_shift = amd64_as_alu_stderr_enabled() &&
            current != NULL &&
            current->abi == GUEST_ABI_AMD64 &&
            strcmp(current->comm, "as") == 0 &&
            modrm.is_reg &&
            rm_size == 32;
        if (trace_as_shift) {
            uint8_t insn_bytes[8] = {};
            bool have_insn_bytes = amd64_trace_read_guest(saved_rip, insn_bytes, sizeof(insn_bytes));
            fprintf(stderr,
                    "amd64 as shift: pre rip=%#llx subop=%u rm=%u lhs=%#llx count=%u effective=%u cf=%u zf=%u sf=%u of=%u pf=%u af=%u%s\n",
                    (unsigned long long) saved_rip,
                    modrm.reg,
                    modrm.rm,
                    (unsigned long long) lhs,
                    count,
                    effective_count,
                    cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af,
                    have_insn_bytes ? "" : " bytes=?");
            if (have_insn_bytes) {
                fprintf(stderr,
                        "amd64 as shift: bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                        insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
                        insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
            }
        }
        if (effective_count == 0)
            break;
        switch (modrm.reg) {
        case 0:
        case 1:
            result = amd64_rotate_value(lhs, rm_size, count, modrm.reg);
            break;
        case 2:
        case 3:
            result = amd64_rotate_carry_value(cpu, lhs, rm_size, count, modrm.reg);
            break;
        case 4:
            result = amd64_trunc(lhs << count, rm_size);
            break;
        case 5:
            result = amd64_trunc(amd64_trunc(lhs, rm_size) >> count, rm_size);
            break;
        case 7:
            result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, rm_size) >> count), rm_size);
            break;
        default:
            return INT_UNDEFINED;
        }
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_gpf_restore;
        if (modrm.reg == 0 || modrm.reg == 1)
            amd64_set_rotate_flags(cpu, result, rm_size, count, modrm.reg);
        else if (modrm.reg != 2 && modrm.reg != 3)
            amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
        if (trace_as_shift) {
            fprintf(stderr,
                    "amd64 as shift: post rip=%#llx subop=%u rm=%u result=%#llx reg=%#llx cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                    (unsigned long long) saved_rip,
                    modrm.reg,
                    modrm.rm,
                    (unsigned long long) result,
                    (unsigned long long) amd64_reg_get(cpu, modrm.rm, rm_size),
                    cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af);
        }
        break;
    }
    case 0xe8: {
        int32_t rel32;
        if (!amd64_fetch(cpu, tlb, &rel32, sizeof(rel32))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        qword_t return_rip = cpu->amd64_rip;
        qword_t target = return_rip + rel32;
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "call");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        if (!amd64_push(cpu, tlb, return_rip))
            goto amd64_gpf_restore;
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "call-rel32");
        cpu->amd64_rip = target;
        break;
    }
    case 0xe9: {
        int32_t rel32;
        if (!amd64_fetch(cpu, tlb, &rel32, sizeof(rel32))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        qword_t target = cpu->amd64_rip + rel32;
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "jmp");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        amd64_trace_cc1_va_list_branch_probe(cpu, saved_rip, true, target, "jmp-rel32");
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "jmp-rel32");
        cpu->amd64_rip = target;
        break;
    }
    case 0xeb: {
        int8_t rel8;
        if (!amd64_fetch(cpu, tlb, &rel8, sizeof(rel8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        qword_t target = cpu->amd64_rip + rel8;
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "jmp");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "jmp-rel8");
        cpu->amd64_rip = target;
        break;
    }
    case 0xe3: {
        int8_t rel8;
        qword_t count;
        if (!amd64_fetch(cpu, tlb, &rel8, sizeof(rel8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        count = cpu->amd64_address_size_prefix
                ? amd64_reg_get(cpu, amd64_rcx, 32)
                : amd64_reg_get(cpu, amd64_rcx, 64);
        if (count == 0) {
            qword_t target = cpu->amd64_rip + rel8;
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "jcxz");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
            cpu->amd64_rip = target;
        }
        break;
    }
    case 0xfe: {
        struct amd64_modrm modrm;
        qword_t lhs, result;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        switch (modrm.reg) {
        case 0:
        case 1: {
            bool is_inc = modrm.reg == 0;
            bool saved_cf = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            result = is_inc ? amd64_trunc(lhs + 1, 8) : amd64_trunc(lhs - 1, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            if (is_inc)
                amd64_set_add_flags(cpu, lhs, 1, result, 8);
            else
                amd64_set_sub_flags(cpu, lhs, 1, result, 8);
            cpu->cf = saved_cf;
            collapse_flags(cpu);
            break;
        }
        default:
            return INT_UNDEFINED;
        }
        break;
    }
    case 0xff: {
        struct amd64_modrm modrm;
        qword_t value, lhs, result;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        switch (modrm.reg) {
        case 0:
        case 1: {
            bool is_inc = modrm.reg == 0;
            bool saved_cf = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            result = is_inc ? amd64_trunc(lhs + 1, op_size) : amd64_trunc(lhs - 1, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            if (is_inc)
                amd64_set_add_flags(cpu, lhs, 1, result, op_size);
            else
                amd64_set_sub_flags(cpu, lhs, 1, result, op_size);
            cpu->cf = saved_cf;
            collapse_flags(cpu);
            break;
        }
        case 2: {
            qword_t return_rip = cpu->amd64_rip;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
                goto amd64_gpf_restore;
            {
                int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, value, "call-rm64");
                if (target_interrupt != INT_NONE)
                    return target_interrupt;
            }
            if (!amd64_push(cpu, tlb, return_rip))
                goto amd64_gpf_restore;
            amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, value, "call-rm64");
            amd64_trace_cargo_transfer(cpu, tlb, saved_rip, value, "call-rm64");
            cpu->amd64_rip = value;
            break;
        }
        case 4:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
                goto amd64_gpf_restore;
            {
                int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, value, "jmp-rm64");
                if (target_interrupt != INT_NONE)
                    return target_interrupt;
            }
            amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, value, "jmp-rm64");
            amd64_trace_cargo_transfer(cpu, tlb, saved_rip, value, "jmp-rm64");
            cpu->amd64_rip = value;
            break;
        case 6:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix,
                    operand_size_prefix ? 16 : 64, &value))
                goto amd64_gpf_restore;
            if (!amd64_push_size(cpu, tlb, operand_size_prefix ? 16 : 64, value))
                goto amd64_gpf_restore;
            break;
        default:
            return INT_UNDEFINED;
        }
        break;
    }
    default:
        return INT_UNDEFINED;
    }

    amd64_trace_cargo_predecessor(cpu, tlb, saved_rip);
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_gpf_restore:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_step_to_interrupt_jit(struct cpu_state *cpu, struct tlb *tlb) {
    static int debug_enabled = -1;
    qword_t before_rip = cpu->amd64_rip;
    qword_t before_rsp = cpu->amd64_regs[amd64_rsp];
    guest_addr_t checked_rip;
    if (debug_enabled == -1)
        debug_enabled = getenv("ISH_TRACE_AMD64_JIT") != NULL ? 1 : 0;
    int interrupt = amd64_step_to_interrupt(cpu, tlb);
    if (debug_enabled == 1) {
        fprintf(stderr,
                "[amd64-jit] helper result rip=%llx->%llx rsp=%llx->%llx int=%d\n",
                (unsigned long long) before_rip,
                (unsigned long long) cpu->amd64_rip,
                (unsigned long long) before_rsp,
                (unsigned long long) cpu->amd64_regs[amd64_rsp],
                interrupt);
    }
    if (interrupt != INT_NONE && !amd64_guest_addr_ok(cpu->amd64_rip, 1, &checked_rip)) {
        printk("[amd64-jit] helper bad-rip insn=%#llx rip=%#llx rsp=%#llx int=%d\n",
               (unsigned long long) cpu->amd64_current_insn_rip,
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) cpu->amd64_regs[amd64_rsp],
               interrupt);
    }
    if (interrupt != INT_NONE) {
        cpu->trapno = interrupt;
        amd64_sync_legacy_regs(cpu);
    }
    return interrupt;
}

int amd64_jit_ret(struct cpu_state *cpu, struct tlb *tlb) {
    qword_t target;
    guest_addr_t checked_target;
    qword_t saved_rip = cpu->amd64_rip;
    qword_t old_rsp = cpu->amd64_regs[amd64_rsp];
    if (!amd64_pop(cpu, tlb, &target)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    if (!amd64_guest_addr_ok(target, 1, &checked_target)) {
        uint8_t slot_bytes[8] = {};
        uint8_t direct_slot_bytes[8] = {};
        bool have_slot = amd64_mem_read(cpu, tlb, old_rsp, slot_bytes, sizeof(slot_bytes));
        bool have_direct_slot = amd64_mem_read_direct(old_rsp, direct_slot_bytes, sizeof(direct_slot_bytes));
        printk("[amd64-jit] bad-ret-target-v2 comm=%s pid=%d from=%#llx target=%#llx old-rsp=%#llx new-rsp=%#llx slot=%s%02x %02x %02x %02x %02x %02x %02x %02x\n",
               current != NULL ? current->comm : "?",
               current != NULL ? current->pid : -1,
               (unsigned long long) saved_rip,
               (unsigned long long) target,
               (unsigned long long) old_rsp,
               (unsigned long long) cpu->amd64_regs[amd64_rsp],
               have_slot ? "" : "unreadable ",
               slot_bytes[0], slot_bytes[1], slot_bytes[2], slot_bytes[3],
               slot_bytes[4], slot_bytes[5], slot_bytes[6], slot_bytes[7]);
        printk("[amd64-jit] bad-ret-target-direct addr=%#llx slot=%s%02x %02x %02x %02x %02x %02x %02x %02x\n",
               (unsigned long long) old_rsp,
               have_direct_slot ? "" : "unreadable ",
               direct_slot_bytes[0], direct_slot_bytes[1], direct_slot_bytes[2], direct_slot_bytes[3],
               direct_slot_bytes[4], direct_slot_bytes[5], direct_slot_bytes[6], direct_slot_bytes[7]);
        amd64_dump_tlb_slot(tlb, old_rsp, sizeof(slot_bytes), "bad-ret-target");
        amd64_dump_guest_bytes(cpu, tlb, saved_rip, 8, "bad-ret-target-insn");
        amd64_dump_stack_window(cpu, tlb, old_rsp, 2, 4, "bad-ret-target");
        if (current != NULL)
            amd64_dump_recent_suspects(current->pid, "bad-ret-target");
        cpu->amd64_rip = target;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    cpu->amd64_rip = target;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_push_reg(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg, unsigned long next_ip) {
    if (reg >= amd64_reg_count)
        return INT_GPF;
    qword_t saved_rip = cpu->amd64_rip;
    if (!amd64_push(cpu, tlb, cpu->amd64_regs[reg])) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_pop_reg(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg, unsigned long next_ip) {
    qword_t value;
    qword_t saved_rip = cpu->amd64_rip;
    if (reg >= amd64_reg_count)
        return INT_GPF;
    if (!amd64_pop(cpu, tlb, &value)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_regs[reg] = value;
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_pop_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    byte_t byte;
    qword_t value;
    unsigned pop_size;

    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_pop_rm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            lock_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0x8f || lock_prefix)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_pop_rm_pf;
    if (modrm.reg != 0)
        return INT_UNDEFINED;

    pop_size = operand_size_prefix ? 16 : 64;
    if (!amd64_pop_size(cpu, tlb, pop_size, &value))
        goto amd64_pop_rm_pf;
    if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, pop_size, value))
        goto amd64_pop_rm_pf;
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_pop_rm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_bswap(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg_size, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned reg = reg_size & 0xf;
    unsigned size = (reg_size >> 8) & 0xff;
    (void) tlb;
    if (reg >= amd64_reg_count || (size != 32 && size != 64))
        return INT_GPF;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    if (size == 64) {
        qword_t value = amd64_reg_get(cpu, reg, 64);
        amd64_reg_set(cpu, reg, 64, __builtin_bswap64(value));
    } else {
        dword_t value = (dword_t) amd64_reg_get(cpu, reg, 32);
        amd64_reg_set(cpu, reg, 32, __builtin_bswap32(value));
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_push_flags(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long push_size, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    if (push_size != 16 && push_size != 64)
        return INT_GPF;
    collapse_flags(cpu);
    if (!amd64_push_size(cpu, tlb, push_size, cpu->eflags)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_push_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long value, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    qword_t saved_rip = cpu->amd64_rip;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-push-imm-next from=%#llx next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    if (!amd64_push(cpu, tlb, (qword_t) value)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_xchg_rax_reg(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg_size, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned reg = reg_size & 0xf;
    unsigned size = (reg_size >> 8) & 0xff;
    qword_t lhs;
    qword_t rhs;
    (void) tlb;
    if (reg >= amd64_reg_count || (size != 32 && size != 64))
        return INT_GPF;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-xchg-next from=%#llx reg=%u size=%u next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               reg,
               size,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    if (reg != amd64_rax) {
        lhs = amd64_reg_get(cpu, amd64_rax, size);
        rhs = amd64_reg_get(cpu, reg, size);
        amd64_reg_set(cpu, amd64_rax, size, rhs);
        amd64_reg_set(cpu, reg, size, lhs);
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_xchg_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    byte_t byte;
    unsigned size;
    qword_t lhs, rhs;
    bool atomic_locked;

    if (opcode != 0x86 && opcode != 0x87)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_xchg_rm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_xchg_rm_pf;

    size = opcode == 0x86 ? 8 : (operand_size_prefix ? 16 : (rex.w ? 64 : 32));
    atomic_locked = !modrm.is_reg;
    if (atomic_locked)
        lock(&atomic_l_lock, 0);
    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, size, &lhs)) {
        if (atomic_locked)
            unlock(&atomic_l_lock);
        goto amd64_xchg_rm_pf;
    }
    rhs = opcode == 0x86
        ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
        : amd64_reg_get(cpu, modrm.reg, size);
    if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, size, rhs)) {
        if (atomic_locked)
            unlock(&atomic_l_lock);
        goto amd64_xchg_rm_pf;
    }
    if (atomic_locked)
        unlock(&atomic_l_lock);
    if (opcode == 0x86)
        amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, lhs);
    else
        amd64_reg_set(cpu, modrm.reg, size, lhs);

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_xchg_rm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_jmp_abs(struct cpu_state *cpu, struct tlb *tlb, unsigned long target) {
    guest_addr_t checked_target;
    (void) tlb;
    if (!amd64_guest_addr_ok((qword_t) target, 1, &checked_target)) {
        printk("[amd64-jit] bad-jmp-target from=%#llx target=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) target);
        cpu->amd64_rip = (qword_t) target;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    cpu->amd64_rip = (qword_t) target;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_call_abs(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long target, unsigned long next_ip) {
    guest_addr_t checked_target;
    guest_addr_t checked_next_ip;
    qword_t saved_rip = cpu->amd64_rip;
    if (!amd64_guest_addr_ok((qword_t) target, 1, &checked_target) ||
            !amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-call-target from=%#llx target=%#llx next=%#llx\n",
               (unsigned long long) saved_rip,
               (unsigned long long) target,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) target;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    if (!amd64_push(cpu, tlb, (qword_t) next_ip)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_rip = (qword_t) target;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_jcc_abs(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long cc, unsigned long target, unsigned long next_ip) {
    guest_addr_t checked_target;
    guest_addr_t checked_next_ip;
    (void) tlb;
    if (cc > 0xf)
        return INT_GPF;
    if (!amd64_guest_addr_ok((qword_t) target, 1, &checked_target) ||
            !amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-jcc-target from=%#llx cc=%lu target=%#llx next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               cc,
               (unsigned long long) target,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) target;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    cpu->amd64_rip = amd64_cond_eval(cpu, (unsigned) cc)
        ? (qword_t) target
        : (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_syscall(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    (void) tlb;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-syscall-next from=%#llx next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_AMD64_SYSCALL;
}

int amd64_jit_sign_extend(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    bool operand_size_prefix = false;
    byte_t byte;

    if (opcode != 0x98 && opcode != 0x99)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_sign_extend_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;

    if (opcode == 0x98) {
        if (rex.w) {
            amd64_reg_set(cpu, amd64_rax, 64,
                    (qword_t) (sqword_t) (int32_t) amd64_reg_get(cpu, amd64_rax, 32));
        } else if (operand_size_prefix) {
            amd64_reg_set(cpu, amd64_rax, 16,
                    (word_t) (int16_t) amd64_reg_get(cpu, amd64_rax, 8));
        } else {
            amd64_reg_set(cpu, amd64_rax, 32,
                    (dword_t) (int16_t) amd64_reg_get(cpu, amd64_rax, 16));
        }
    } else {
        if (rex.w) {
            amd64_reg_set(cpu, amd64_rdx, 64,
                    ((sqword_t) amd64_reg_get(cpu, amd64_rax, 64) < 0) ? ~0ull : 0);
        } else if (operand_size_prefix) {
            amd64_reg_set(cpu, amd64_rdx, 16,
                    ((int16_t) amd64_reg_get(cpu, amd64_rax, 16) < 0) ? 0xffff : 0);
        } else {
            amd64_reg_set(cpu, amd64_rdx, 32,
                    ((int32_t) amd64_reg_get(cpu, amd64_rax, 32) < 0) ? 0xffffffffu : 0);
        }
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_sign_extend_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_string_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    bool operand_size_prefix = false;
    enum amd64_rep_mode rep_mode = AMD64_REP_NONE;
    byte_t byte;
    unsigned size;
    int interrupt;

    if (!((opcode >= 0xa4 && opcode <= 0xa7) ||
          (opcode >= 0xaa && opcode <= 0xaf)))
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_string_op_jit_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte == 0x67) {
            cpu->amd64_address_size_prefix = true;
            continue;
        }
        if (byte == 0xf3) {
            rep_mode = AMD64_REPZ;
            continue;
        }
        if (byte == 0xf2) {
            rep_mode = AMD64_REPNZ;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;

    size = (opcode & 1) == 0 ? 8 : (rex.w ? 64 : (operand_size_prefix ? 16 : 32));
    interrupt = amd64_string_op(cpu, tlb, saved_rip, (byte_t) opcode, size, rep_mode);
    if (interrupt != INT_NONE) {
        amd64_sync_legacy_regs(cpu);
        return interrupt;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_string_op_jit_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_mov_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg_size, unsigned long value, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned reg = reg_size & 0xf;
    unsigned size = (reg_size >> 8) & 0xff;
    bool rex_present = (reg_size & (1ul << 16)) != 0;
    (void) tlb;
    if (reg >= amd64_reg_count || (size != 8 && size != 32 && size != 64))
        return INT_GPF;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-mov-imm-next from=%#llx reg=%u size=%u next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               reg,
               size,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    if (size == 8)
        amd64_reg_set_encoded8(cpu, reg, rex_present, value);
    else
        amd64_reg_set(cpu, reg, size, (qword_t) value);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_accum_imm_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    bool operand_size_prefix = false;
    byte_t byte;
    qword_t lhs, rhs, result;
    unsigned size;

    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_accum_imm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;

    size = (opcode & 1) == 0 ? 8 : (rex.w ? 64 : (operand_size_prefix ? 16 : 32));
    if (size == 8) {
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_accum_imm_pf;
        rhs = imm8;
        lhs = amd64_reg_get_encoded8(cpu, amd64_rax, rex.present);
    } else if (size == 16) {
        uint16_t imm16;
        if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
            goto amd64_accum_imm_pf;
        rhs = imm16;
        lhs = amd64_reg_get(cpu, amd64_rax, size);
    } else {
        int32_t imm32;
        if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32)))
            goto amd64_accum_imm_pf;
        rhs = rex.w ? (qword_t) (sqword_t) imm32 : (uint32_t) imm32;
        lhs = amd64_reg_get(cpu, amd64_rax, size);
    }

    switch (opcode & 0xfe) {
    case 0x04:
        result = amd64_trunc(lhs + rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_add_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x0c:
        result = amd64_trunc(lhs | rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x14: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs + rhs + carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x1c: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs - rhs - carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x24:
        result = amd64_trunc(lhs & rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x2c:
        result = amd64_trunc(lhs - rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x34:
        result = amd64_trunc(lhs ^ rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x3c:
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0xa8:
        amd64_set_logic_flags(cpu, lhs & rhs, size);
        break;
    default:
        return INT_UNDEFINED;
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_accum_imm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_reg_reg_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op_regs_size, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned opcode = op_regs_size & 0xff;
    unsigned reg = (op_regs_size >> 8) & 0xf;
    unsigned rm = (op_regs_size >> 12) & 0xf;
    unsigned size = (op_regs_size >> 16) & 0xff;
    bool rex_present = ((op_regs_size >> 24) & 1) != 0;
    qword_t lhs;
    qword_t rhs;
    qword_t result;

    (void) tlb;
    if (reg >= amd64_reg_count || rm >= amd64_reg_count ||
            (size != 8 && size != 32 && size != 64))
        return INT_GPF;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-reg-reg-next from=%#llx opcode=%#x reg=%u rm=%u size=%u next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               opcode,
               reg,
               rm,
               size,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }

    switch (opcode) {
    case 0x01:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        rhs = amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs + rhs, size);
        amd64_reg_set(cpu, rm, size, result);
        amd64_set_add_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x03:
        lhs = amd64_reg_get(cpu, reg, size);
        rhs = amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs + rhs, size);
        amd64_reg_set(cpu, reg, size, result);
        amd64_set_add_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x08:
    case 0x09:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs | rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x0b:
        lhs = amd64_reg_get(cpu, reg, size);
        rhs = amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs | rhs, size);
        amd64_reg_set(cpu, reg, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x11: {
        unsigned carry_in = cpu->cf;
        lhs = amd64_reg_get(cpu, rm, size);
        rhs = amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs + rhs + carry_in, size);
        amd64_reg_set(cpu, rm, size, result);
        amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x13: {
        unsigned carry_in = cpu->cf;
        lhs = amd64_reg_get(cpu, reg, size);
        rhs = amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs + rhs + carry_in, size);
        amd64_reg_set(cpu, reg, size, result);
        amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x19: {
        unsigned carry_in = cpu->cf;
        lhs = amd64_reg_get(cpu, rm, size);
        rhs = amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs - rhs - carry_in, size);
        amd64_reg_set(cpu, rm, size, result);
        amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x1b: {
        unsigned carry_in = cpu->cf;
        lhs = amd64_reg_get(cpu, reg, size);
        rhs = amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs - rhs - carry_in, size);
        amd64_reg_set(cpu, reg, size, result);
        amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x20:
    case 0x21:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs & rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x23:
        lhs = amd64_reg_get(cpu, reg, size);
        rhs = amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs & rhs, size);
        amd64_reg_set(cpu, reg, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x29:
        lhs = amd64_reg_get(cpu, rm, size);
        rhs = amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs - rhs, size);
        amd64_reg_set(cpu, rm, size, result);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x2b:
        lhs = amd64_reg_get(cpu, reg, size);
        rhs = amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs - rhs, size);
        amd64_reg_set(cpu, reg, size, result);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x31:
        lhs = amd64_reg_get(cpu, rm, size);
        rhs = amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs ^ rhs, size);
        amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x33:
        lhs = amd64_reg_get(cpu, reg, size);
        rhs = amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs ^ rhs, size);
        amd64_reg_set(cpu, reg, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x38:
        lhs = amd64_reg_get_encoded8(cpu, rm, rex_present);
        rhs = amd64_reg_get_encoded8(cpu, reg, rex_present);
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x39:
        lhs = amd64_reg_get(cpu, rm, size);
        rhs = amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x3a:
        lhs = amd64_reg_get_encoded8(cpu, reg, rex_present);
        rhs = amd64_reg_get_encoded8(cpu, rm, rex_present);
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x3b:
        lhs = amd64_reg_get(cpu, reg, size);
        rhs = amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x63:
        amd64_reg_set(cpu, reg, size,
                (qword_t) amd64_sign_extend(amd64_reg_get(cpu, rm, size == 64 ? 32 : size),
                        size == 64 ? 32 : size));
        break;
    case 0x84:
        lhs = amd64_reg_get_encoded8(cpu, rm, rex_present);
        rhs = amd64_reg_get_encoded8(cpu, reg, rex_present);
        amd64_set_logic_flags(cpu, lhs & rhs, size);
        break;
    case 0x85:
        lhs = amd64_reg_get(cpu, rm, size);
        rhs = amd64_reg_get(cpu, reg, size);
        amd64_set_logic_flags(cpu, lhs & rhs, size);
        break;
    case 0x88:
        amd64_reg_set_encoded8(cpu, rm, rex_present,
                amd64_reg_get_encoded8(cpu, reg, rex_present));
        break;
    case 0x89:
        amd64_reg_set(cpu, rm, size, amd64_reg_get(cpu, reg, size));
        break;
    case 0x8a:
        amd64_reg_set_encoded8(cpu, reg, rex_present,
                amd64_reg_get_encoded8(cpu, rm, rex_present));
        break;
    case 0x8b:
        amd64_reg_set(cpu, reg, size, amd64_reg_get(cpu, rm, size));
        break;
    default:
        return INT_UNDEFINED;
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_reg_imm_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op_group_rm_size, unsigned long value, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned opcode = op_group_rm_size & 0xff;
    unsigned group = (op_group_rm_size >> 8) & 0xf;
    unsigned rm = (op_group_rm_size >> 12) & 0xf;
    unsigned size = (op_group_rm_size >> 16) & 0xff;
    bool rex_present = (op_group_rm_size & (1ul << 24)) != 0;
    qword_t lhs;
    qword_t rhs = (qword_t) value;
    qword_t result;
    unsigned count;
    unsigned effective_count;

    (void) tlb;
    if (rm >= amd64_reg_count || group > 7 ||
            (size != 8 && size != 32 && size != 64))
        return INT_GPF;
    if (opcode != 0x80 && opcode != 0x81 && opcode != 0x83 &&
            opcode != 0xc0 && opcode != 0xc1 &&
            opcode != 0xc6 && opcode != 0xc7)
        return INT_UNDEFINED;
    if ((opcode == 0xc6 || opcode == 0xc7) && group != 0)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-reg-imm-next from=%#llx opcode=%#x group=%u rm=%u size=%u next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               opcode,
               group,
               rm,
               size,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }

    if (opcode == 0xc6 || opcode == 0xc7) {
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, rhs);
        else
            amd64_reg_set(cpu, rm, size, rhs);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    if (opcode == 0xc0 || opcode == 0xc1) {
        count = (unsigned) rhs & (size == 64 ? 0x3f : 0x1f);
        effective_count = (group == 0 || group == 1) ? (count % size) : count;
        if (effective_count != 0) {
            lhs = size == 8
                ? amd64_reg_get_encoded8(cpu, rm, rex_present)
                : amd64_reg_get(cpu, rm, size);
            switch (group) {
            case 0:
            case 1:
                result = amd64_rotate_value(lhs, size, count, group);
                amd64_set_rotate_flags(cpu, result, size, count, group);
                break;
            case 4:
                result = amd64_trunc(lhs << count, size);
                amd64_set_shift_flags(cpu, lhs, result, size, count, group);
                break;
            case 5:
                result = amd64_trunc(amd64_trunc(lhs, size) >> count, size);
                amd64_set_shift_flags(cpu, lhs, result, size, count, group);
                break;
            case 7:
                result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, size) >> count), size);
                amd64_set_shift_flags(cpu, lhs, result, size, count, group);
                break;
            default:
                return INT_UNDEFINED;
            }
            if (size == 8)
                amd64_reg_set_encoded8(cpu, rm, rex_present, result);
            else
                amd64_reg_set(cpu, rm, size, result);
        }
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    lhs = size == 8 ? amd64_reg_get_encoded8(cpu, rm, rex_present) :
        amd64_reg_get(cpu, rm, size);
    switch (group) {
    case 0:
        result = amd64_trunc(lhs + rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_add_flags(cpu, lhs, rhs, result, size);
        break;
    case 1:
        result = amd64_trunc(lhs | rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 2: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs + rhs + carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 3: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs - rhs - carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 4:
        result = amd64_trunc(lhs & rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 5:
        result = amd64_trunc(lhs - rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 6:
        result = amd64_trunc(lhs ^ rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 7:
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    default:
        return INT_UNDEFINED;
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_imul_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    byte_t byte;
    unsigned size;
    qword_t rhs, result;
    sqword_t src_signed;
    sqword_t imm_signed;

    if (opcode != 0x69 && opcode != 0x6b)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_imul_imm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_imul_imm_pf;

    size = operand_size_prefix ? 16 : (rex.w ? 64 : 32);
    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, size, &rhs))
        goto amd64_imul_imm_pf;
    if (opcode == 0x69) {
        if (size == 16) {
            int16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
                goto amd64_imul_imm_pf;
            imm_signed = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32)))
                goto amd64_imul_imm_pf;
            imm_signed = imm32;
        }
    } else {
        int8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_imul_imm_pf;
        imm_signed = imm8;
    }

    src_signed = amd64_sign_extend(rhs, size);
    if (size == 64) {
        __int128_t full = (__int128_t) src_signed * (__int128_t) imm_signed;
        result = (qword_t) full;
        amd64_reg_set(cpu, modrm.reg, size, result);
        amd64_set_mul_flags(cpu, full != (__int128_t) (sqword_t) (uint64_t) result);
    } else {
        int64_t full = (int64_t) src_signed * (int64_t) imm_signed;
        result = amd64_trunc((qword_t) full, size);
        amd64_reg_set(cpu, modrm.reg, size, result);
        amd64_set_mul_flags(cpu, full != (int64_t) amd64_sign_extend(result, size));
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_imul_imm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

enum amd64_jit_mem_meta {
    AMD64_JIT_MEM_OPCODE_SHIFT = 0,
    AMD64_JIT_MEM_REG_SHIFT = 8,
    AMD64_JIT_MEM_SIZE_SHIFT = 12,
    AMD64_JIT_MEM_BASE_SHIFT = 20,
    AMD64_JIT_MEM_INDEX_SHIFT = 24,
    AMD64_JIT_MEM_SCALE_SHIFT = 28,
    AMD64_JIT_MEM_HAS_BASE = 1ul << 30,
    AMD64_JIT_MEM_HAS_INDEX = 1ul << 31,
    AMD64_JIT_MEM_RIP_REL = 1ul << 32,
    AMD64_JIT_MEM_FS = 1ul << 33,
    AMD64_JIT_MEM_REX_PRESENT = 1ul << 34,
};

int amd64_jit_mem_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long meta, unsigned long disp, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned opcode = (meta >> AMD64_JIT_MEM_OPCODE_SHIFT) & 0xff;
    unsigned reg = (meta >> AMD64_JIT_MEM_REG_SHIFT) & 0xf;
    unsigned size = (meta >> AMD64_JIT_MEM_SIZE_SHIFT) & 0xff;
    unsigned base = (meta >> AMD64_JIT_MEM_BASE_SHIFT) & 0xf;
    unsigned index = (meta >> AMD64_JIT_MEM_INDEX_SHIFT) & 0xf;
    unsigned scale = (meta >> AMD64_JIT_MEM_SCALE_SHIFT) & 0x3;
    bool rex_present = (meta & AMD64_JIT_MEM_REX_PRESENT) != 0;
    qword_t addr = (qword_t) disp;
    qword_t value;

    if (reg >= amd64_reg_count || (size != 8 && size != 16 && size != 32 && size != 64))
        return INT_GPF;
    if (opcode != 0x01 && opcode != 0x03 && opcode != 0x08 && opcode != 0x09 &&
            opcode != 0x0b && opcode != 0x11 && opcode != 0x13 &&
            opcode != 0x19 && opcode != 0x1b && opcode != 0x20 && opcode != 0x21 &&
            opcode != 0x23 && opcode != 0x29 && opcode != 0x2b &&
            opcode != 0x31 && opcode != 0x33 &&
            opcode != 0x38 && opcode != 0x39 && opcode != 0x3a &&
            opcode != 0x3b && opcode != 0x84 && opcode != 0x85 &&
            opcode != 0x88 && opcode != 0x89 && opcode != 0x8a &&
            opcode != 0x8b && opcode != 0x8d && opcode != 0x63)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-mem-op-next from=%#llx opcode=%#x next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               opcode,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }

    if ((meta & AMD64_JIT_MEM_RIP_REL) != 0)
        addr += (qword_t) next_ip;
    if ((meta & AMD64_JIT_MEM_HAS_BASE) != 0)
        addr += cpu->amd64_regs[base];
    if ((meta & AMD64_JIT_MEM_HAS_INDEX) != 0)
        addr += cpu->amd64_regs[index] << scale;
    if ((meta & AMD64_JIT_MEM_FS) != 0 && opcode != 0x8d)
        addr += cpu->tls_ptr;

    switch (opcode) {
    case 0x01:
    case 0x08:
    case 0x09:
    case 0x11:
    case 0x19:
    case 0x20:
    case 0x21:
    case 0x29:
    case 0x31: {
        uint64_t tmp64;
        uint32_t tmp32;
        uint8_t tmp8;
        void *dst = size == 64 ? (void *) &tmp64 :
            (size == 8 ? (void *) &tmp8 : (void *) &tmp32);
        if (!amd64_mem_read(cpu, tlb, addr, dst, size / 8)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        qword_t lhs = size == 64 ? tmp64 : (size == 8 ? tmp8 : tmp32);
        qword_t rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        qword_t result;
        switch (opcode) {
        case 0x01:
            result = amd64_trunc(lhs + rhs, size);
            amd64_set_add_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x08:
        case 0x09:
            result = amd64_trunc(lhs | rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x11: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs + rhs + carry_in, size);
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        }
        case 0x19: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs - rhs - carry_in, size);
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        }
        case 0x20:
        case 0x21:
            result = amd64_trunc(lhs & rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x29:
            result = amd64_trunc(lhs - rhs, size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x31:
            result = amd64_trunc(lhs ^ rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        default:
            return INT_UNDEFINED;
        }
        tmp64 = result;
        tmp32 = result;
        tmp8 = result;
        if (!amd64_mem_write(cpu, tlb, addr, dst, size / 8)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        if (size == 64)
            amd64_trace_qword_store(cpu, cpu->amd64_rip, opcode, addr, result);
        break;
    }
    case 0x03:
    case 0x0b:
    case 0x13:
    case 0x1b:
    case 0x23:
    case 0x2b:
    case 0x33: {
        uint64_t tmp64;
        uint32_t tmp32;
        void *src = size == 64 ? (void *) &tmp64 : (void *) &tmp32;
        if (!amd64_mem_read(cpu, tlb, addr, src, size / 8)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        qword_t lhs = amd64_reg_get(cpu, reg, size);
        qword_t rhs = size == 64 ? tmp64 : tmp32;
        qword_t result;
        switch (opcode) {
        case 0x03:
            result = amd64_trunc(lhs + rhs, size);
            amd64_set_add_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x0b:
            result = amd64_trunc(lhs | rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x13: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs + rhs + carry_in, size);
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        }
        case 0x1b: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs - rhs - carry_in, size);
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        }
        case 0x23:
            result = amd64_trunc(lhs & rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x2b:
            result = amd64_trunc(lhs - rhs, size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x33:
            result = amd64_trunc(lhs ^ rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        default:
            return INT_UNDEFINED;
        }
        amd64_reg_set(cpu, reg, size, result);
        break;
    }
    case 0x38: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        value = tmp;
        qword_t rhs = amd64_reg_get_encoded8(cpu, reg, rex_present);
        amd64_set_sub_flags(cpu, value, rhs, amd64_trunc(value - rhs, 8), 8);
        break;
    }
    case 0x39:
        if (size == 64) {
            uint64_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        } else {
            uint32_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        }
        {
            qword_t rhs = amd64_reg_get(cpu, reg, size);
            qword_t result = amd64_trunc(value - rhs, size);
            amd64_set_sub_flags(cpu, value, rhs, result, size);
            amd64_trace_cc1_cmp_probe(cpu, cpu->amd64_rip, addr, value, rhs, result, size);
        }
        break;
    case 0x3a: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        value = amd64_reg_get_encoded8(cpu, reg, rex_present);
        qword_t rhs = tmp;
        amd64_set_sub_flags(cpu, value, rhs, amd64_trunc(value - rhs, 8), 8);
        break;
    }
    case 0x3b:
        if (size == 64) {
            uint64_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        } else {
            uint32_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        }
        {
            qword_t lhs = amd64_reg_get(cpu, reg, size);
            amd64_set_sub_flags(cpu, lhs, value, amd64_trunc(lhs - value, size), size);
        }
        break;
    case 0x84: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        amd64_set_logic_flags(cpu, tmp & amd64_reg_get_encoded8(cpu, reg, rex_present), 8);
        break;
    }
    case 0x85:
        if (size == 64) {
            uint64_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        } else {
            uint32_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        }
        amd64_set_logic_flags(cpu, value & amd64_reg_get(cpu, reg, size), size);
        break;
    case 0x88: {
        uint8_t tmp = amd64_reg_get_encoded8(cpu, reg, rex_present);
        if (!amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp))) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        break;
    }
    case 0x89: {
        value = amd64_reg_get(cpu, reg, size);
        uint64_t tmp64 = value;
        uint32_t tmp32 = value;
        const void *src = size == 64 ? (const void *) &tmp64 : (const void *) &tmp32;
        if (!amd64_mem_write(cpu, tlb, addr, src, size / 8)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        if (size == 64)
            amd64_trace_qword_store(cpu, cpu->amd64_rip, opcode, addr, value);
        break;
    }
    case 0x8a: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        amd64_reg_set_encoded8(cpu, reg, rex_present, tmp);
        break;
    }
    case 0x8b:
        if (size == 64) {
            uint64_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        } else {
            uint32_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        }
        if (size == 64)
            amd64_trace_cc1_slot_probe(cpu, cpu->amd64_rip, addr, value);
        amd64_reg_set(cpu, reg, size, value);
        break;
    case 0x8d:
        amd64_reg_set(cpu, reg, size, addr);
        break;
    case 0x63: {
        unsigned src_size = size == 64 ? 32 : size;
        if (src_size == 16) {
            uint16_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        } else {
            uint32_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        }
        amd64_reg_set(cpu, reg, size, (qword_t) amd64_sign_extend(value, src_size));
        break;
    }
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_movx(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op2, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    byte_t byte;
    qword_t value;
    unsigned src_size;
    unsigned dst_size;

    if (op2 != 0xb6 && op2 != 0xb7 && op2 != 0xbe && op2 != 0xbf)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_movx_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0x0f)
        return INT_UNDEFINED;
    if (!amd64_fetch_u8(cpu, tlb, &byte))
        goto amd64_movx_pf;
    if (byte != op2)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_movx_pf;

    src_size = (op2 == 0xb6 || op2 == 0xbe) ? 8 : 16;
    dst_size = rex.w ? 64 : 32;
    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, src_size, &value))
        goto amd64_movx_pf;
    if (op2 == 0xbe || op2 == 0xbf)
        value = (qword_t) amd64_sign_extend(value, src_size);
    amd64_reg_set(cpu, modrm.reg, dst_size, value);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_movx_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_0f_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op2, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    bool repz_prefix = false;
    byte_t byte;
    unsigned op_size;

    if ((op2 != 0x1f && op2 != 0xa3 && op2 != 0xa4 && op2 != 0xa5 &&
                op2 != 0xab && op2 != 0xac && op2 != 0xad && op2 != 0xaf &&
                op2 != 0xae &&
                op2 != 0xb0 && op2 != 0xb1 &&
                op2 != 0xb3 && op2 != 0xba && op2 != 0xbb &&
                op2 != 0xbc && op2 != 0xbd &&
                op2 != 0xc0 && op2 != 0xc1) &&
            !(op2 >= 0x40 && op2 <= 0x4f) &&
            !(op2 >= 0x90 && op2 <= 0x9f))
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_0f_rm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            lock_prefix = true;
            continue;
        }
        if (byte == 0xf3) {
            repz_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0x0f)
        return INT_UNDEFINED;
    if (!amd64_fetch_u8(cpu, tlb, &byte))
        goto amd64_0f_rm_pf;
    if (byte != op2)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_0f_rm_pf;

    op_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
    if (repz_prefix && op2 != 0xbc && op2 != 0xbd)
        return INT_UNDEFINED;
    if (op2 == 0x1f) {
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xae) {
        int interrupt = amd64_fxsave_op(cpu, tlb, &modrm, fs_prefix, saved_rip);
        if (interrupt != INT_NONE) {
            amd64_sync_legacy_regs(cpu);
            return interrupt;
        }
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 >= 0x40 && op2 <= 0x4f) {
        qword_t src;
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &src))
            goto amd64_0f_rm_pf;
        if (amd64_cond_eval(cpu, op2 & 0xf))
            amd64_reg_set(cpu, modrm.reg, op_size, src);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 >= 0x90 && op2 <= 0x9f) {
        qword_t value = amd64_cond_eval(cpu, op2 & 0xf) ? 1 : 0;
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, value))
            goto amd64_0f_rm_pf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xa4 || op2 == 0xa5 || op2 == 0xac || op2 == 0xad) {
        qword_t lhs, rhs, result;
        unsigned count;
        if (lock_prefix)
            return INT_UNDEFINED;
        if (op2 == 0xa4 || op2 == 0xac) {
            uint8_t imm8;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
                goto amd64_0f_rm_pf;
            count = imm8 & (op_size == 64 ? 0x3f : 0x1f);
        } else {
            count = amd64_reg_get(cpu, amd64_rcx, 8) & (op_size == 64 ? 0x3f : 0x1f);
        }
        if (count != 0) {
            if (count > op_size)
                return INT_UNDEFINED;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_0f_rm_pf;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            if (op2 == 0xa4 || op2 == 0xa5) {
                result = amd64_trunc((lhs << count) | (rhs >> (op_size - count)), op_size);
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                    goto amd64_0f_rm_pf;
                amd64_set_double_shift_flags(cpu, lhs, result, op_size, count, true);
            } else {
                result = amd64_trunc((amd64_trunc(lhs, op_size) >> count) |
                        (rhs << (op_size - count)), op_size);
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                    goto amd64_0f_rm_pf;
                amd64_set_double_shift_flags(cpu, lhs, result, op_size, count, false);
            }
        }
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xbc || op2 == 0xbd) {
        qword_t src;
        qword_t src_masked;
        qword_t index;
        bool count_zeroes = repz_prefix;
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &src))
            goto amd64_0f_rm_pf;
        src_masked = amd64_trunc(src, op_size);
        collapse_flags(cpu);
        if (count_zeroes) {
            cpu->cf = src_masked == 0;
            cpu->cf_bit = cpu->cf;
            cpu->zf = 0;
        } else {
            cpu->zf = src_masked == 0;
        }
        cpu->zf_res = 0;
        if (src_masked == 0) {
            if (count_zeroes)
                amd64_reg_set(cpu, modrm.reg, op_size, op_size);
            cpu->amd64_rip = (qword_t) next_ip;
            amd64_sync_legacy_regs(cpu);
            return INT_NONE;
        }
        if (op2 == 0xbc) {
            index = (op_size == 64)
                    ? (qword_t) __builtin_ctzll(src_masked)
                    : (qword_t) __builtin_ctz((uint32_t) src_masked);
        } else {
            index = (op_size == 64)
                    ? (qword_t) (63 - __builtin_clzll(src_masked))
                    : (qword_t) (31 - __builtin_clz((uint32_t) src_masked));
        }
        if (count_zeroes)
            cpu->zf = index == 0;
        amd64_reg_set(cpu, modrm.reg, op_size, index);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xa3) {
        qword_t lhs;
        qword_t addr;
        qword_t bit;
        qword_t bit_index = amd64_reg_get(cpu, modrm.reg, op_size);
        if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size,
                bit_index, true, true, &lhs, &addr, &bit))
            goto amd64_0f_rm_pf;
        (void) addr;
        collapse_flags(cpu);
        cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
        cpu->cf_bit = cpu->cf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xab || op2 == 0xb3 || op2 == 0xbb) {
        qword_t addr;
        qword_t lhs, result;
        qword_t bit;
        qword_t bit_index = amd64_reg_get(cpu, modrm.reg, op_size);
        if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size,
                bit_index, true, true, &lhs, &addr, &bit))
            goto amd64_0f_rm_pf;
        collapse_flags(cpu);
        cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
        result = lhs;
        switch (op2) {
        case 0xab:
            result = amd64_trunc(lhs | (1ull << bit), op_size);
            break;
        case 0xb3:
            result = amd64_trunc(lhs & ~(1ull << bit), op_size);
            break;
        case 0xbb:
            result = amd64_trunc(lhs ^ (1ull << bit), op_size);
            break;
        }
        if (!amd64_write_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, addr, result))
            goto amd64_0f_rm_pf;
        cpu->cf_bit = cpu->cf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xba) {
        qword_t addr;
        qword_t lhs, result;
        qword_t bit;
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_0f_rm_pf;
        if (modrm.reg < 4 || modrm.reg > 7)
            return INT_UNDEFINED;
        if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, imm8,
                true, false, &lhs, &addr, &bit))
            goto amd64_0f_rm_pf;
        collapse_flags(cpu);
        cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
        result = lhs;
        switch (modrm.reg) {
        case 4:
            break;
        case 5:
            result = amd64_trunc(lhs | (1ull << bit), op_size);
            break;
        case 6:
            result = amd64_trunc(lhs & ~(1ull << bit), op_size);
            break;
        case 7:
            result = amd64_trunc(lhs ^ (1ull << bit), op_size);
            break;
        }
        if (modrm.reg != 4) {
            if (!amd64_write_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, addr, result))
                goto amd64_0f_rm_pf;
        }
        cpu->cf_bit = cpu->cf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xaf) {
        qword_t rhs, lhs, result;
        sqword_t signed_result;
        bool overflow;
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
            goto amd64_0f_rm_pf;
        lhs = amd64_reg_get(cpu, modrm.reg, op_size);
        signed_result = amd64_sign_extend(lhs, op_size) * amd64_sign_extend(rhs, op_size);
        result = amd64_trunc((qword_t) signed_result, op_size);
        amd64_reg_set(cpu, modrm.reg, op_size, result);
        overflow = signed_result != amd64_sign_extend(result, op_size);
        amd64_set_mul_flags(cpu, overflow);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xc0 || op2 == 0xc1) {
        unsigned xadd_size = op2 == 0xc0 ? 8 : op_size;
        qword_t lhs, rhs, result;
        bool atomic_locked = lock_prefix && !modrm.is_reg;
        if (atomic_locked)
            lock(&atomic_l_lock, 0);
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, xadd_size, &lhs)) {
            if (atomic_locked)
                unlock(&atomic_l_lock);
            goto amd64_0f_rm_pf;
        }
        rhs = op2 == 0xc0
                ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                : amd64_reg_get(cpu, modrm.reg, xadd_size);
        result = amd64_trunc(lhs + rhs, xadd_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, xadd_size, result)) {
            if (atomic_locked)
                unlock(&atomic_l_lock);
            goto amd64_0f_rm_pf;
        }
        if (atomic_locked)
            unlock(&atomic_l_lock);
        if (op2 == 0xc0)
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, lhs);
        else
            amd64_reg_set(cpu, modrm.reg, xadd_size, lhs);
        amd64_set_add_flags(cpu, lhs, rhs, result, xadd_size);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xb0 || op2 == 0xb1) {
        unsigned cmpxchg_size = op2 == 0xb0 ? 8 : op_size;
        qword_t dst, src, acc, result;
        bool atomic_locked = lock_prefix && !modrm.is_reg;
        if (atomic_locked)
            lock(&atomic_l_lock, 0);
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, cmpxchg_size, &dst)) {
            if (atomic_locked)
                unlock(&atomic_l_lock);
            goto amd64_0f_rm_pf;
        }
        src = op2 == 0xb0
                ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                : amd64_reg_get(cpu, modrm.reg, cmpxchg_size);
        acc = amd64_reg_get(cpu, amd64_rax, cmpxchg_size);
        result = amd64_trunc(acc - dst, cmpxchg_size);
        amd64_set_sub_flags(cpu, acc, dst, result, cmpxchg_size);
        if (acc == dst) {
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, cmpxchg_size, src)) {
                if (atomic_locked)
                    unlock(&atomic_l_lock);
                goto amd64_0f_rm_pf;
            }
            cpu->zf = 1;
            cpu->zf_res = 0;
        } else {
            amd64_reg_set(cpu, amd64_rax, cmpxchg_size, dst);
            cpu->zf = 0;
            cpu->zf_res = 0;
        }
        if (atomic_locked)
            unlock(&atomic_l_lock);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    return INT_UNDEFINED;

amd64_0f_rm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_0f_vec_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op2, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    enum amd64_rep_mode rep_mode = AMD64_REP_NONE;
    byte_t byte;
    union xmm_reg value, src_xmm;
    qword_t src_scalar;

    if (op2 != 0x10 && op2 != 0x11 && op2 != 0x12 && op2 != 0x16 && op2 != 0x28 &&
            op2 != 0x29 && op2 != 0x6c && op2 != 0x6e &&
            op2 != 0x6f && op2 != 0xef)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_0f_vec_rm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte == 0xf2) {
            rep_mode = AMD64_REPNZ;
            continue;
        }
        if (byte == 0xf3) {
            rep_mode = AMD64_REPZ;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0x0f)
        return INT_UNDEFINED;
    if (!amd64_fetch_u8(cpu, tlb, &byte))
        goto amd64_0f_vec_rm_pf;
    if (byte != op2)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_0f_vec_rm_pf;

    if (op2 == 0x6e) {
        if (operand_size_prefix) {
            if (modrm.reg >= AMD64_XMM_COUNT)
                return INT_UNDEFINED;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                goto amd64_0f_vec_rm_pf;
            value.u128 = 0;
            if (rex.w)
                value.qw[0] = src_scalar;
            else
                value.u32[0] = (uint32_t) src_scalar;
            cpu->xmm[modrm.reg] = value;
        } else {
            if (modrm.reg >= 8)
                return INT_UNDEFINED;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                goto amd64_0f_vec_rm_pf;
            cpu->mm[modrm.reg].qw = rex.w ? src_scalar : (uint32_t) src_scalar;
        }
    } else {
        if ((modrm.reg >= AMD64_XMM_COUNT) ||
                (modrm.is_reg && modrm.rm >= AMD64_XMM_COUNT))
            return INT_UNDEFINED;
        if (op2 == 0x10 || op2 == 0x28) {
            if (op2 == 0x10 && rep_mode == AMD64_REPZ) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                value = cpu->xmm[modrm.reg];
                if (modrm.is_reg) {
                    value.u32[0] = cpu->xmm[modrm.rm].u32[0];
                } else {
                    value.u128 = 0;
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    value.u32[0] = (uint32_t) src_scalar;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x10 && rep_mode == AMD64_REPNZ) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                value = cpu->xmm[modrm.reg];
                if (modrm.is_reg) {
                    value.qw[0] = cpu->xmm[modrm.rm].qw[0];
                } else {
                    value.u128 = 0;
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    value.qw[0] = src_scalar;
                }
                cpu->xmm[modrm.reg] = value;
            } else {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                    goto amd64_0f_vec_rm_pf;
                cpu->xmm[modrm.reg] = value;
            }
        } else if (op2 == 0x12) {
            if (rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            if (operand_size_prefix && modrm.is_reg)
                return INT_UNDEFINED;
            value = cpu->xmm[modrm.reg];
            if (modrm.is_reg) {
                value.qw[0] = cpu->xmm[modrm.rm].qw[1];
            } else {
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                    goto amd64_0f_vec_rm_pf;
                value.qw[0] = src_scalar;
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0x16) {
            if (operand_size_prefix && modrm.is_reg)
                return INT_UNDEFINED;
            value = cpu->xmm[modrm.reg];
            if (modrm.is_reg) {
                value.qw[1] = cpu->xmm[modrm.rm].qw[0];
            } else {
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                    goto amd64_0f_vec_rm_pf;
                value.qw[1] = src_scalar;
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0x11 || op2 == 0x29) {
            if (op2 == 0x11 && rep_mode == AMD64_REPZ) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                if (modrm.is_reg) {
                    cpu->xmm[modrm.rm].u32[0] = cpu->xmm[modrm.reg].u32[0];
                } else if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 32,
                            cpu->xmm[modrm.reg].u32[0])) {
                    goto amd64_0f_vec_rm_pf;
                }
            } else if (op2 == 0x11 && rep_mode == AMD64_REPNZ) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                if (modrm.is_reg) {
                    cpu->xmm[modrm.rm].qw[0] = cpu->xmm[modrm.reg].qw[0];
                } else if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64,
                            cpu->xmm[modrm.reg].qw[0])) {
                    goto amd64_0f_vec_rm_pf;
                }
            } else {
                value = cpu->xmm[modrm.reg];
                if (!amd64_write_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                    goto amd64_0f_vec_rm_pf;
            }
        } else if (op2 == 0x6f) {
            if (!(operand_size_prefix || rep_mode == AMD64_REPZ))
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                goto amd64_0f_vec_rm_pf;
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0x6c) {
            if (!operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            value.qw[1] = src_xmm.qw[0];
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0xef) {
            if (!operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            value.qw[0] ^= src_xmm.qw[0];
            value.qw[1] ^= src_xmm.qw[1];
            cpu->xmm[modrm.reg] = value;
        }
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_0f_vec_rm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_grp3_test(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    byte_t byte;
    unsigned size;
    qword_t lhs, rhs;

    if (opcode != 0xf6 && opcode != 0xf7)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_grp3_test_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_grp3_test_pf;
    if (modrm.reg != 0)
        return INT_UNDEFINED;

    size = opcode == 0xf6 ? 8 : (rex.w ? 64 : (operand_size_prefix ? 16 : 32));
    if (opcode == 0xf6) {
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_grp3_test_pf;
        rhs = imm8;
    } else if (size == 16) {
        uint16_t imm16;
        if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
            goto amd64_grp3_test_pf;
        rhs = imm16;
    } else {
        int32_t imm32;
        if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32)))
            goto amd64_grp3_test_pf;
        rhs = rex.w ? (qword_t) (sqword_t) imm32 : (uint32_t) imm32;
    }

    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, size, &lhs))
        goto amd64_grp3_test_pf;
    amd64_set_logic_flags(cpu, lhs & rhs, size);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_grp3_test_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_grp3_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    byte_t byte;
    unsigned size;
    int interrupt;

    if (opcode != 0xf6 && opcode != 0xf7)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_grp3_op_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_grp3_op_pf;
    if (modrm.reg < 2)
        return INT_UNDEFINED;

    size = opcode == 0xf6 ? 8 : (operand_size_prefix ? 16 : (rex.w ? 64 : 32));
    interrupt = amd64_grp3_muldiv(cpu, tlb, &modrm, fs_prefix, size);
    if (interrupt != INT_NONE) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return interrupt;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_grp3_op_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_modrm_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    byte_t byte;
    qword_t lhs, rhs, result;
    unsigned rm_size;

    if (opcode != 0x80 && opcode != 0x81 && opcode != 0x83 &&
            opcode != 0xc0 && opcode != 0xc1 && opcode != 0xc6 &&
            opcode != 0xc7)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_modrm_imm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            lock_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_modrm_imm_pf;

    rm_size = (opcode == 0x80 || opcode == 0xc0 || opcode == 0xc6) ? 8 :
        (operand_size_prefix ? 16 : (rex.w ? 64 : 32));

    if (opcode == 0xc6) {
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_modrm_imm_pf;
        if (modrm.reg != 0 || lock_prefix)
            return INT_UNDEFINED;
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, imm8))
            goto amd64_modrm_imm_pf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    if (opcode == 0xc7) {
        if (modrm.reg != 0 || lock_prefix)
            return INT_UNDEFINED;
        if (rm_size == 16) {
            uint16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
                goto amd64_modrm_imm_pf;
            rhs = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32)))
                goto amd64_modrm_imm_pf;
            rhs = rex.w ? (qword_t) (sqword_t) imm32 : (uint32_t) imm32;
        }
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, rhs))
            goto amd64_modrm_imm_pf;
        if (rm_size == 64)
            amd64_trace_qword_store(cpu, saved_rip, opcode, amd64_effective_addr(cpu, &modrm, fs_prefix), rhs);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    if (opcode == 0xc0 || opcode == 0xc1) {
        uint8_t imm8;
        unsigned count, effective_count;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_modrm_imm_pf;
        count = imm8 & (rm_size == 64 ? 0x3f : 0x1f);
        effective_count = (modrm.reg == 0 || modrm.reg == 1) ? (count % rm_size) :
            ((modrm.reg == 2 || modrm.reg == 3) ? amd64_rotate_carry_count(rm_size, count) : count);
        if (effective_count != 0) {
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
                goto amd64_modrm_imm_pf;
            switch (modrm.reg) {
            case 0:
            case 1:
                result = amd64_rotate_value(lhs, rm_size, count, modrm.reg);
                amd64_set_rotate_flags(cpu, result, rm_size, count, modrm.reg);
                break;
            case 2:
            case 3:
                result = amd64_rotate_carry_value(cpu, lhs, rm_size, count, modrm.reg);
                break;
            case 4:
                result = amd64_trunc(lhs << count, rm_size);
                amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
                break;
            case 5:
                result = amd64_trunc(amd64_trunc(lhs, rm_size) >> count, rm_size);
                amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
                break;
            case 7:
                result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, rm_size) >> count), rm_size);
                amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
                break;
            default:
                return INT_UNDEFINED;
            }
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_modrm_imm_pf;
        }
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    if (opcode == 0x80) {
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_modrm_imm_pf;
        rhs = imm8;
    } else if (opcode == 0x83) {
        int8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_modrm_imm_pf;
        rhs = (qword_t) amd64_sign_extend((uint8_t) imm8, 8);
    } else {
        if (rm_size == 16) {
            uint16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
                goto amd64_modrm_imm_pf;
            rhs = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32)))
                goto amd64_modrm_imm_pf;
            rhs = (qword_t) (sqword_t) imm32;
        }
    }

    bool atomic_locked = lock_prefix && !modrm.is_reg && modrm.reg != 7;
    if (lock_prefix && (modrm.is_reg || modrm.reg == 7))
        return INT_UNDEFINED;
    if (atomic_locked)
        lock(&atomic_l_lock, 0);

    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
        goto amd64_modrm_imm_unlock_pf;

    switch (modrm.reg) {
    case 0:
        result = amd64_trunc(lhs + rhs, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_add_flags(cpu, lhs, rhs, result, rm_size);
        break;
    case 1:
        result = amd64_trunc(lhs | rhs, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_logic_flags(cpu, result, rm_size);
        break;
    case 2: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs + rhs + carry_in, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, rm_size);
        break;
    }
    case 3: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs - rhs - carry_in, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, rm_size);
        break;
    }
    case 4:
        result = amd64_trunc(lhs & rhs, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_logic_flags(cpu, result, rm_size);
        break;
    case 5:
        result = amd64_trunc(lhs - rhs, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_sub_flags(cpu, lhs, rhs, result, rm_size);
        break;
    case 6:
        result = amd64_trunc(lhs ^ rhs, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_logic_flags(cpu, result, rm_size);
        break;
    case 7:
        result = amd64_trunc(lhs - rhs, rm_size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, rm_size);
        break;
    default:
        return INT_UNDEFINED;
    }

    if (atomic_locked)
        unlock(&atomic_l_lock);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_modrm_imm_unlock_pf:
    if (atomic_locked)
        unlock(&atomic_l_lock);
amd64_modrm_imm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_shift(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    byte_t byte;
    qword_t lhs, result;
    unsigned rm_size;
    unsigned count;
    unsigned effective_count;

    if (opcode != 0xd0 && opcode != 0xd1 && opcode != 0xd2 && opcode != 0xd3)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_shift_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_shift_pf;

    rm_size = (opcode == 0xd0 || opcode == 0xd2) ? 8 :
        (operand_size_prefix ? 16 : (rex.w ? 64 : 32));
    count = (opcode == 0xd0 || opcode == 0xd1) ? 1 :
        ((unsigned) amd64_reg_get(cpu, amd64_rcx, 8) & (rm_size == 64 ? 0x3f : 0x1f));
    effective_count = (modrm.reg == 0 || modrm.reg == 1) ? (count % rm_size) :
        ((modrm.reg == 2 || modrm.reg == 3) ? amd64_rotate_carry_count(rm_size, count) : count);
    if (effective_count != 0) {
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
            goto amd64_shift_pf;
        switch (modrm.reg) {
        case 0:
        case 1:
            result = amd64_rotate_value(lhs, rm_size, count, modrm.reg);
            amd64_set_rotate_flags(cpu, result, rm_size, count, modrm.reg);
            break;
        case 2:
        case 3:
            result = amd64_rotate_carry_value(cpu, lhs, rm_size, count, modrm.reg);
            break;
        case 4:
            result = amd64_trunc(lhs << count, rm_size);
            amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
            break;
        case 5:
            result = amd64_trunc(amd64_trunc(lhs, rm_size) >> count, rm_size);
            amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
            break;
        case 7:
            result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, rm_size) >> count), rm_size);
            amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
            break;
        default:
            return INT_UNDEFINED;
        }
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_shift_pf;
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_shift_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_fe_group(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool lock_prefix = false;
    byte_t byte;
    qword_t lhs, result;

    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_fe_group_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            lock_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0xfe)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_fe_group_pf;
    if (modrm.reg > 1 || (lock_prefix && modrm.is_reg))
        return INT_UNDEFINED;

    bool is_inc = modrm.reg == 0;
    bool saved_cf = cpu->cf;
    bool atomic_locked = lock_prefix && !modrm.is_reg;
    if (atomic_locked)
        lock(&atomic_l_lock, 0);
    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
        goto amd64_fe_group_unlock_pf;
    result = is_inc ? amd64_trunc(lhs + 1, 8) : amd64_trunc(lhs - 1, 8);
    if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
        goto amd64_fe_group_unlock_pf;
    if (atomic_locked)
        unlock(&atomic_l_lock);
    if (is_inc)
        amd64_set_add_flags(cpu, lhs, 1, result, 8);
    else
        amd64_set_sub_flags(cpu, lhs, 1, result, 8);
    cpu->cf = saved_cf;
    collapse_flags(cpu);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_fe_group_unlock_pf:
    if (atomic_locked)
        unlock(&atomic_l_lock);
amd64_fe_group_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_ff_group(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    byte_t byte;
    qword_t value, lhs, result;
    unsigned op_size;

    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_ff_group_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            lock_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0xff)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_ff_group_pf;

    op_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
    switch (modrm.reg) {
    case 0:
    case 1: {
        bool is_inc = modrm.reg == 0;
        bool saved_cf = cpu->cf;
        bool atomic_locked = lock_prefix && !modrm.is_reg;
        if (lock_prefix && modrm.is_reg)
            return INT_UNDEFINED;
        if (atomic_locked)
            lock(&atomic_l_lock, 0);
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
            goto amd64_ff_group_unlock_pf;
        result = is_inc ? amd64_trunc(lhs + 1, op_size) : amd64_trunc(lhs - 1, op_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
            goto amd64_ff_group_unlock_pf;
        if (atomic_locked)
            unlock(&atomic_l_lock);
        if (is_inc)
            amd64_set_add_flags(cpu, lhs, 1, result, op_size);
        else
            amd64_set_sub_flags(cpu, lhs, 1, result, op_size);
        cpu->cf = saved_cf;
        collapse_flags(cpu);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
amd64_ff_group_unlock_pf:
        if (atomic_locked)
            unlock(&atomic_l_lock);
        goto amd64_ff_group_pf;
    }
    case 2: {
        qword_t return_rip = (qword_t) next_ip;
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
            goto amd64_ff_group_pf;
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, value, "call-rm64");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        if (!amd64_push(cpu, tlb, return_rip))
            goto amd64_ff_group_pf;
        amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, value, "call-rm64");
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, value, "call-rm64");
        cpu->amd64_rip = value;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    case 4:
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
            goto amd64_ff_group_pf;
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, value, "jmp-rm64");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, value, "jmp-rm64");
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, value, "jmp-rm64");
        cpu->amd64_rip = value;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    case 6:
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix,
                operand_size_prefix ? 16 : 64, &value))
            goto amd64_ff_group_pf;
        if (!amd64_push_size(cpu, tlb, operand_size_prefix ? 16 : 64, value))
            goto amd64_ff_group_pf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    default:
        return INT_UNDEFINED;
    }

amd64_ff_group_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

void amd64_jit_bridge_set_tlb(struct tlb *tlb) {
    amd64_jit_bridge_tlb = tlb;
}

int amd64_step_to_interrupt_jit_bridge(struct cpu_state *cpu) {
    if (amd64_jit_bridge_tlb == NULL)
        return INT_GPF;
    return amd64_step_to_interrupt_jit(cpu, amd64_jit_bridge_tlb);
}

int cpu_run_to_interrupt_amd64(struct cpu_state *cpu, struct tlb *tlb) {
    cpu->poked_ptr = &cpu->_poked;
    tlb_refresh(tlb, cpu->mmu);

    int steps = 0;
    guest_addr_t last_step_rip = 0;
    unsigned same_rip_steps = 0;
    static __thread guest_addr_t last_watchdog_rip;
    static __thread unsigned same_rip_timer_yields;
    while (true) {
        guest_addr_t step_rip = cpu->amd64_rip;
        if (step_rip == last_step_rip)
            same_rip_steps++;
        else {
            last_step_rip = step_rip;
            same_rip_steps = 0;
        }

        int interrupt = amd64_step_to_interrupt(cpu, tlb);
        if (interrupt == INT_UNDEFINED || interrupt == INT_PRIV) {
            if (interrupt == INT_UNDEFINED && amd64_trace_undefined_enabled()) {
                uint8_t bytes[12] = {0};
                unsigned read = 0;
                for (; read < sizeof(bytes); read++) {
                    if (!amd64_mem_read(cpu, tlb, cpu->amd64_current_insn_rip + read, &bytes[read], 1))
                        break;
                }
                printk("amd64 undefined: rip=%#llx bytes=",
                       (unsigned long long) cpu->amd64_current_insn_rip);
                for (unsigned i = 0; i < read; i++)
                    printk("%s%02x", i == 0 ? "" : " ", bytes[i]);
                printk(" rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsp=%#llx rbp=%#llx\n",
                       (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rbx, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rcx, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rdx, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rsp, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rbp, 64));
            }
            cpu->amd64_rip = cpu->amd64_current_insn_rip;
        }
        if (interrupt == INT_NONE && cpu->tf)
            interrupt = INT_DEBUG;
        if (interrupt == INT_NONE && __atomic_exchange_n(cpu->poked_ptr, false, __ATOMIC_SEQ_CST))
            interrupt = INT_TIMER;
        if (interrupt == INT_NONE && ++steps >= 1024) {
            if (same_rip_steps >= 1023) {
                if (cpu->amd64_rip == last_watchdog_rip)
                    same_rip_timer_yields++;
                else {
                    last_watchdog_rip = cpu->amd64_rip;
                    same_rip_timer_yields = 1;
                }
                if ((same_rip_timer_yields & (same_rip_timer_yields - 1)) == 0) {
                    byte_t bytes[12] = {0};
                    unsigned read = 0;
                    for (; read < sizeof(bytes); read++) {
                        if (!amd64_mem_read(cpu, tlb, cpu->amd64_rip + read, &bytes[read], 1))
                            break;
                    }
                    printk("amd64 watchdog: pid=%d comm=%s rip=%#llx repeated=%u bytes=",
                           current ? current->pid : -1,
                           current ? current->comm : "(null)",
                           (unsigned long long) cpu->amd64_rip,
                           same_rip_timer_yields);
                    for (unsigned i = 0; i < read; i++)
                        printk("%s%02x", i == 0 ? "" : " ", bytes[i]);
                    printk("\n");
                }
            } else {
                same_rip_timer_yields = 0;
            }
            steps = 0;
            interrupt = INT_TIMER;
        }
        if (interrupt != INT_NONE) {
            cpu->trapno = interrupt;
            amd64_sync_legacy_regs(cpu);
            return interrupt;
        }
    }
}
