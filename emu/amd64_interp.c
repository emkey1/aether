#include <limits.h>
#include <math.h>

#include "emu/cpuid.h"
#include "emu/cpu.h"
#include "emu/fpu.h"
#include "emu/memory.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
#include "emu/modrm.h"
#include "kernel/task.h"
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
    addr_t guest_addr;
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
    addr_t guest_addr;
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
    addr_t guest_addr;
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
    addr_t insn_addr;
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
    addr_t insn_addr;

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
    addr_t insn_addr;

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
    addr_t insn_addr;

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
    addr_t stack_addr;
    addr_t bytes_addr;
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
        addr_t insn_addr;
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

    addr_t insn_addr;
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
    collapse_flags(cpu);
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
    amd64_trace_htop_field_write(cpu, tlb, guest_addr, value, size);
    amd64_trace_htop_r13_block_write(cpu, tlb, guest_addr, value, size);
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
        addr_t insn_addr;
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
    return true;
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
        return true;
    }
    case 64: {
        uint64_t tmp;
        if (!amd64_mem_read(cpu, tlb, rsp, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        cpu->amd64_regs[amd64_rsp] = rsp + sizeof(tmp);
        amd64_trace_suspicious_rsp_write(cpu, rsp, cpu->amd64_regs[amd64_rsp], size);
        return true;
    }
    default:
        return false;
    }
}

static inline bool amd64_pop(struct cpu_state *cpu, struct tlb *tlb, qword_t *value) {
    return amd64_pop_size(cpu, tlb, 64, value);
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
            float80 value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ldm80(cpu, &value);
            break;
        }
        case 0xdb7: {
            float80 value;
            fpu_stm80(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
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
    case 0x0: return cpu->of;
    case 0x1: return !cpu->of;
    case 0x2: return cpu->cf;
    case 0x3: return !cpu->cf;
    case 0x4: return cpu->zf;
    case 0x5: return !cpu->zf;
    case 0x6: return cpu->cf || cpu->zf;
    case 0x7: return !cpu->cf && !cpu->zf;
    case 0x8: return cpu->sf;
    case 0x9: return !cpu->sf;
    case 0xa: return cpu->pf;
    case 0xb: return !cpu->pf;
    case 0xc: return cpu->sf != cpu->of;
    case 0xd: return cpu->sf == cpu->of;
    case 0xe: return cpu->zf || (cpu->sf != cpu->of);
    case 0xf: return !cpu->zf && (cpu->sf == cpu->of);
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
    if (!cpu->df)
        cpu->amd64_regs[reg] += delta;
    else
        cpu->amd64_regs[reg] -= delta;
}

static inline int amd64_string_op(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip, byte_t opcode, unsigned size, enum amd64_rep_mode rep_mode) {
    qword_t count = rep_mode == AMD64_REP_NONE ? 1 : amd64_reg_get(cpu, amd64_rcx, 64);

    while (count != 0) {
        qword_t value;
        switch (opcode) {
        case 0xa4:
        case 0xa5:
            if (!amd64_mem_read(cpu, tlb, cpu->amd64_regs[amd64_rsi], &value, size / 8))
                goto amd64_string_pf;
            if (!amd64_mem_write(cpu, tlb, cpu->amd64_regs[amd64_rdi], &value, size / 8))
                goto amd64_string_pf;
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        case 0xaa:
        case 0xab:
            value = amd64_reg_get(cpu, amd64_rax, size);
            if (!amd64_mem_write(cpu, tlb, cpu->amd64_regs[amd64_rdi], &value, size / 8))
                goto amd64_string_pf;
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        case 0xac:
        case 0xad:
            if (!amd64_mem_read(cpu, tlb, cpu->amd64_regs[amd64_rsi], &value, size / 8))
                goto amd64_string_pf;
            amd64_reg_set(cpu, amd64_rax, size, value);
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            break;
        case 0xae:
        case 0xaf: {
            qword_t lhs = amd64_reg_get(cpu, amd64_rax, size);
            qword_t rhs;
            if (!amd64_mem_read(cpu, tlb, cpu->amd64_regs[amd64_rdi], &rhs, size / 8))
                goto amd64_string_pf;
            amd64_set_sub_flags(cpu, lhs, rhs, lhs - rhs, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        }
        default: {
            qword_t lhs;
            qword_t rhs;
            if (!amd64_mem_read(cpu, tlb, cpu->amd64_regs[amd64_rsi], &lhs, size / 8))
                goto amd64_string_pf;
            if (!amd64_mem_read(cpu, tlb, cpu->amd64_regs[amd64_rdi], &rhs, size / 8))
                goto amd64_string_pf;
            amd64_set_sub_flags(cpu, lhs, rhs, lhs - rhs, size);
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        }
        }

        if (rep_mode != AMD64_REP_NONE) {
            count--;
            amd64_reg_set(cpu, amd64_rcx, 64, count);
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
    return INT_NONE;

amd64_string_pf:
    cpu->amd64_rip = saved_rip;
    return INT_PF;
}

static inline int amd64_step_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    qword_t saved_rip = cpu->amd64_rip;
    cpu->amd64_current_insn_rip = saved_rip;
    amd64_trace_cargo_start_call(cpu);
    amd64_trace_htop_window(cpu, tlb);
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
            if (amd64_cond_eval(cpu, op2 & 0xf))
                cpu->amd64_rip += rel32;
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
                op2 == 0x56 || op2 == 0x57 || op2 == 0x60 || op2 == 0x61 ||
                op2 == 0x62 || op2 == 0x63 || op2 == 0x67 || op2 == 0x68 || op2 == 0x69 || op2 == 0x6a || op2 == 0x6b || op2 == 0x6c ||
                op2 == 0x6f || op2 == 0x70 || op2 == 0x7e || op2 == 0x7f ||
                op2 == 0x64 || op2 == 0x65 || op2 == 0x66 || op2 == 0x74 || op2 == 0x75 || op2 == 0x76 || op2 == 0xc4 || op2 == 0xc5 || op2 == 0xc6 ||
                op2 == 0xd4 || op2 == 0xd6 || op2 == 0xd7 ||
                op2 == 0xd8 || op2 == 0xd9 || op2 == 0xda || op2 == 0xdb || op2 == 0xdc || op2 == 0xdd || op2 == 0xde || op2 == 0xdf ||
                op2 == 0xeb || op2 == 0xef ||
                op2 == 0xf6 ||
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
                if (operand_size_prefix || rep_mode != AMD64_REP_NONE)
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
            } else if (op2 == 0x58 || op2 == 0x59 || op2 == 0x5c || op2 == 0x5d || op2 == 0x5e) {
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
                if (operand_size_prefix)
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
                       op2 == 0x68 || op2 == 0x69 || op2 == 0x6a) {
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
                } else {
                    value.u32[0] = dst.u32[2];
                    value.u32[1] = src_xmm.u32[2];
                    value.u32[2] = dst.u32[3];
                    value.u32[3] = src_xmm.u32[3];
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
            qword_t bit;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            bit = amd64_reg_get(cpu, modrm.reg, op_size) & (op_size - 1);
            cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
            collapse_flags(cpu);
            break;
        }
        if (op2 == 0xab || op2 == 0xb3 || op2 == 0xbb) {
            struct amd64_modrm modrm;
            qword_t lhs, result;
            qword_t bit;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            bit = amd64_reg_get(cpu, modrm.reg, op_size) & (op_size - 1);
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
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            if (!modrm.is_reg && op_size == 64)
                amd64_trace_qword_store(cpu, saved_rip, 0x0f,
                        amd64_effective_addr(cpu, &modrm, fs_prefix), result);
            collapse_flags(cpu);
            break;
        }
        if (op2 == 0xba) {
            struct amd64_modrm modrm;
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
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            bit = imm8 & (op_size - 1);
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
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                    goto amd64_gpf_restore;
                if (!modrm.is_reg && op_size == 64)
                    amd64_trace_qword_store(cpu, saved_rip, 0x0f,
                            amd64_effective_addr(cpu, &modrm, fs_prefix), result);
            }
            collapse_flags(cpu);
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
            amd64_reg_set(cpu, modrm.reg, op_size, amd64_effective_addr(cpu, &modrm, fs_prefix));
            break;
        case 0x63:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &rhs))
                goto amd64_gpf_restore;
            amd64_reg_set(cpu, modrm.reg, 64, (qword_t) amd64_sign_extend(rhs, 32));
            break;
        case 0x69:
        case 0x6b: {
            sqword_t src_signed;
            sqword_t imm_signed;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            if (opcode == 0x69) {
                int32_t imm32;
                if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                imm_signed = imm32;
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
        if (!amd64_push(cpu, tlb, cpu->amd64_regs[reg]))
            goto amd64_gpf_restore;
        break;
    }
    case 0x58 ... 0x5f: {
        unsigned reg = (opcode - 0x58) | (rex.b ? 8 : 0);
        qword_t value;
        if (!amd64_pop(cpu, tlb, &value))
            goto amd64_gpf_restore;
        amd64_reg_set(cpu, reg, 64, value);
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
        int32_t imm32;
        if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!amd64_push(cpu, tlb, (qword_t) (sqword_t) imm32))
            goto amd64_gpf_restore;
        break;
    }
    case 0x6a: {
        int8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!amd64_push(cpu, tlb, (qword_t) (sqword_t) imm8))
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
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, size, &lhs))
                goto amd64_gpf_restore;
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
        if (amd64_verbose_boot_trace_enabled() && saved_rip == AMD64_BUSYBOX_INIT_JNE_RIP) {
            printk("amd64 init jne: rip=%#llx zf=%d taken=%d rax=%#llx target=%#llx\n",
                   (unsigned long long) saved_rip,
                   cpu->zf,
                   taken,
                   (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64),
                   (unsigned long long) (cpu->amd64_rip + rel8));
        }
        if (taken) {
            amd64_trace_cargo_transfer(cpu, tlb, saved_rip, cpu->amd64_rip + rel8, "jcc");
            cpu->amd64_rip += rel8;
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
            effective_count = (modrm.reg == 0 || modrm.reg == 1) ? (count % rm_size) : count;
            if (effective_count == 0)
                break;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
                goto amd64_gpf_restore;
            switch (modrm.reg) {
            case 0:
            case 1:
                result = amd64_rotate_value(lhs, rm_size, count, modrm.reg);
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
            else
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
    case 0xc3: {
        qword_t target;
        if (!amd64_pop(cpu, tlb, &target))
            goto amd64_gpf_restore;
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "ret");
        cpu->amd64_rip = target;
        break;
    }
    case 0xc9: {
        unsigned pop_size = operand_size_prefix ? 16 : 64;
        qword_t value;
        qword_t old_rsp = cpu->amd64_regs[amd64_rsp];
        cpu->amd64_regs[amd64_rsp] = cpu->amd64_regs[amd64_rbp];
        amd64_trace_suspicious_rsp_write(cpu, old_rsp, cpu->amd64_regs[amd64_rsp], 64);
        if (!amd64_pop_size(cpu, tlb, pop_size, &value))
            goto amd64_gpf_restore;
        amd64_reg_set(cpu, amd64_rbp, pop_size, value);
        break;
    }
    case 0xf4:
        return INT_PRIV;
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
        effective_count = (modrm.reg == 0 || modrm.reg == 1) ? (count % rm_size) : count;
        if (effective_count == 0)
            break;
        switch (modrm.reg) {
        case 0:
        case 1:
            result = amd64_rotate_value(lhs, rm_size, count, modrm.reg);
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
        else
            amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
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
        if (!amd64_push(cpu, tlb, return_rip))
            goto amd64_gpf_restore;
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, cpu->amd64_rip + rel32, "call-rel32");
        cpu->amd64_rip += rel32;
        break;
    }
    case 0xe9: {
        int32_t rel32;
        if (!amd64_fetch(cpu, tlb, &rel32, sizeof(rel32))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, cpu->amd64_rip + rel32, "jmp-rel32");
        cpu->amd64_rip += rel32;
        break;
    }
    case 0xeb: {
        int8_t rel8;
        if (!amd64_fetch(cpu, tlb, &rel8, sizeof(rel8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, cpu->amd64_rip + rel8, "jmp-rel8");
        cpu->amd64_rip += rel8;
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
            if (!amd64_push(cpu, tlb, return_rip))
                goto amd64_gpf_restore;
            amd64_trace_cargo_transfer(cpu, tlb, saved_rip, value, "call-rm64");
            cpu->amd64_rip = value;
            break;
        }
        case 4:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
                goto amd64_gpf_restore;
            amd64_trace_cargo_transfer(cpu, tlb, saved_rip, value, "jmp-rm64");
            cpu->amd64_rip = value;
            break;
        case 6:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
                goto amd64_gpf_restore;
            if (!amd64_push(cpu, tlb, value))
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

int cpu_run_to_interrupt_amd64(struct cpu_state *cpu, struct tlb *tlb) {
    cpu->poked_ptr = &cpu->_poked;
    tlb_refresh(tlb, cpu->mmu);

    int steps = 0;
    while (true) {
        int interrupt = amd64_step_to_interrupt(cpu, tlb);
        if (interrupt == INT_UNDEFINED || interrupt == INT_PRIV)
            cpu->amd64_rip = cpu->amd64_current_insn_rip;
        if (interrupt == INT_NONE && cpu->tf)
            interrupt = INT_DEBUG;
        if (interrupt == INT_NONE && __atomic_exchange_n(cpu->poked_ptr, false, __ATOMIC_SEQ_CST))
            interrupt = INT_TIMER;
        if (interrupt == INT_NONE && ++steps >= 1024) {
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
