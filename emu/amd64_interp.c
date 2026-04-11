#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
#include "emu/modrm.h"
#include "kernel/task.h"

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

#define AMD64_BUSYBOX_INIT_SLOT 0x5661a6d8ull
#define AMD64_BUSYBOX_INIT_SLOT_SIZE 8

static inline bool amd64_trace_intersects_busybox_slot(qword_t guest_addr, unsigned size) {
    if (size == 0)
        return false;
    qword_t start = guest_addr;
    qword_t end = guest_addr + size;
    qword_t slot_start = AMD64_BUSYBOX_INIT_SLOT;
    qword_t slot_end = slot_start + AMD64_BUSYBOX_INIT_SLOT_SIZE;
    return start < slot_end && end > slot_start;
}

static inline bool amd64_guest_addr_ok(qword_t guest_addr, unsigned size, addr_t *addr_out) {
    addr_t addr = (addr_t) guest_addr;
    qword_t zero_extended = (qword_t) addr;
    qword_t sign_extended = (qword_t) (sqword_t) (int32_t) addr;
    if (guest_addr != zero_extended && guest_addr != sign_extended)
        return false;
    if (size != 0 && addr + size - 1 < addr)
        return false;
    *addr_out = addr;
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
        cpu->amd64_regs[base] = (cpu->amd64_regs[base] & ~0xff00ull) | ((value & 0xff) << 8);
        return;
    }
    cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffull) | (value & 0xff);
}

static inline void amd64_reg_set(struct cpu_state *cpu, unsigned reg, unsigned size, qword_t value) {
    reg &= 0xf;
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
    addr_t addr;
    if (!amd64_guest_addr_ok(cpu->amd64_rip, size, &addr))
        return false;
    if (!tlb_read(tlb, addr, out, size))
        return false;
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

static inline bool amd64_mem_read(struct cpu_state *cpu, struct tlb *tlb, qword_t guest_addr, void *out, unsigned size) {
    addr_t addr;
    if (!amd64_guest_addr_ok(guest_addr, size, &addr)) {
        cpu->segfault_addr = (addr_t) guest_addr;
        return false;
    }
    if (!tlb_read(tlb, addr, out, size)) {
        cpu->segfault_addr = addr;
        return false;
    }
    if (amd64_trace_intersects_busybox_slot(guest_addr, size)) {
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
    addr_t addr;
    if (!amd64_guest_addr_ok(guest_addr, size, &addr)) {
        cpu->segfault_addr = (addr_t) guest_addr;
        return false;
    }
    if (!tlb_write(tlb, addr, value, size)) {
        cpu->segfault_addr = addr;
        return false;
    }
    if (amd64_trace_intersects_busybox_slot(guest_addr, size)) {
        qword_t observed = 0;
        memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));
        printk("amd64 slot write: rip=%#llx addr=%#llx size=%u value=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) guest_addr,
               size,
               (unsigned long long) observed);
    }
    return true;
}

static inline bool amd64_push(struct cpu_state *cpu, struct tlb *tlb, qword_t value) {
    qword_t rsp = cpu->amd64_regs[amd64_rsp] - sizeof(value);
    if (!amd64_mem_write(cpu, tlb, rsp, &value, sizeof(value)))
        return false;
    cpu->amd64_regs[amd64_rsp] = rsp;
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
        return INT_GPF;

    switch (modrm->reg) {
    case 2: {
        qword_t result = amd64_trunc(~src, size);
        if (!amd64_write_rm(cpu, tlb, modrm, fs_prefix, size, result))
            return INT_GPF;
        return INT_NONE;
    }
    case 3: {
        qword_t result = amd64_trunc(0 - src, size);
        if (!amd64_write_rm(cpu, tlb, modrm, fs_prefix, size, result))
            return INT_GPF;
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

static inline bool amd64_pop(struct cpu_state *cpu, struct tlb *tlb, qword_t *value) {
    qword_t rsp = cpu->amd64_regs[amd64_rsp];
    if (!amd64_mem_read(cpu, tlb, rsp, value, sizeof(*value)))
        return false;
    cpu->amd64_regs[amd64_rsp] = rsp + sizeof(*value);
    return true;
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
        if (mod == 0 && base_low == 5) {
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
        return true;
    }
    default:
        return false;
    }
}

static inline bool amd64_read_xmm_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, union xmm_reg *value) {
    if (modrm->reg >= 8)
        return false;
    if (modrm->is_reg) {
        if (modrm->rm >= 8)
            return false;
        *value = cpu->xmm[modrm->rm];
        return true;
    }
    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    return amd64_mem_read(cpu, tlb, addr, value, sizeof(*value));
}

static inline bool amd64_write_xmm_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, const union xmm_reg *value) {
    if (modrm->reg >= 8)
        return false;
    if (modrm->is_reg) {
        if (modrm->rm >= 8)
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
        byte_t opcode, unsigned size, enum amd64_rep_mode rep_mode) {
    qword_t count = rep_mode == AMD64_REP_NONE ? 1 : amd64_reg_get(cpu, amd64_rcx, 64);

    while (count != 0) {
        qword_t value;
        switch (opcode) {
        case 0xa4:
        case 0xa5:
            if (!amd64_mem_read(cpu, tlb, cpu->amd64_regs[amd64_rsi], &value, size / 8))
                return INT_GPF;
            if (!amd64_mem_write(cpu, tlb, cpu->amd64_regs[amd64_rdi], &value, size / 8))
                return INT_GPF;
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        case 0xaa:
        case 0xab:
            value = amd64_reg_get(cpu, amd64_rax, size);
            if (!amd64_mem_write(cpu, tlb, cpu->amd64_regs[amd64_rdi], &value, size / 8))
                return INT_GPF;
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        case 0xac:
        case 0xad:
            if (!amd64_mem_read(cpu, tlb, cpu->amd64_regs[amd64_rsi], &value, size / 8))
                return INT_GPF;
            amd64_reg_set(cpu, amd64_rax, size, value);
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            break;
        case 0xae:
        case 0xaf: {
            qword_t lhs = amd64_reg_get(cpu, amd64_rax, size);
            qword_t rhs;
            if (!amd64_mem_read(cpu, tlb, cpu->amd64_regs[amd64_rdi], &rhs, size / 8))
                return INT_GPF;
            amd64_set_sub_flags(cpu, lhs, rhs, lhs - rhs, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        }
        default: {
            qword_t lhs;
            qword_t rhs;
            if (!amd64_mem_read(cpu, tlb, cpu->amd64_regs[amd64_rsi], &lhs, size / 8))
                return INT_GPF;
            if (!amd64_mem_read(cpu, tlb, cpu->amd64_regs[amd64_rdi], &rhs, size / 8))
                return INT_GPF;
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
}

static inline int amd64_step_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    qword_t saved_rip = cpu->amd64_rip;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    enum amd64_rep_mode rep_mode = AMD64_REP_NONE;
    struct amd64_rex_prefix rex = {};
    byte_t opcode;

restart_prefix:
    if (!amd64_fetch_u8(cpu, tlb, &opcode)) {
        cpu->amd64_rip = saved_rip;
        cpu->segfault_addr = (addr_t) saved_rip;
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
        return amd64_string_op(cpu, tlb, opcode, 8, rep_mode);
    case 0xa5:
        return amd64_string_op(cpu, tlb, opcode, op_size, rep_mode);
    case 0xa6:
        return amd64_string_op(cpu, tlb, opcode, 8, rep_mode);
    case 0xa7:
        return amd64_string_op(cpu, tlb, opcode, op_size, rep_mode);
    case 0xaa:
        return amd64_string_op(cpu, tlb, opcode, 8, rep_mode);
    case 0xab:
        return amd64_string_op(cpu, tlb, opcode, op_size, rep_mode);
    case 0xac:
        return amd64_string_op(cpu, tlb, opcode, 8, rep_mode);
    case 0xad:
        return amd64_string_op(cpu, tlb, opcode, op_size, rep_mode);
    case 0xae:
        return amd64_string_op(cpu, tlb, opcode, 8, rep_mode);
    case 0xaf:
        return amd64_string_op(cpu, tlb, opcode, op_size, rep_mode);
    case 0x0f: {
        byte_t op2;
        if (!amd64_fetch_u8(cpu, tlb, &op2)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = (addr_t) saved_rip;
            return INT_GPF;
        }
        if (op2 == 0x05)
            return INT_AMD64_SYSCALL;
        if (op2 >= 0x80 && op2 <= 0x8f) {
            int32_t rel32;
            if (!amd64_fetch(cpu, tlb, &rel32, sizeof(rel32))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
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
                cpu->segfault_addr = (addr_t) saved_rip;
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
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, value))
                goto amd64_gpf_restore;
            break;
        }
        if (op2 == 0xb6 || op2 == 0xb7 || op2 == 0xbe || op2 == 0xbf) {
            struct amd64_modrm modrm;
            qword_t src;
            unsigned src_size = (op2 == 0xb6 || op2 == 0xbe) ? 8 : 16;
            unsigned dst_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
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
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            if (operand_size_prefix) {
                union xmm_reg value;
                if (modrm.reg >= 8)
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
        if (op2 == 0x10 || op2 == 0x11 || op2 == 0x16 || op2 == 0x17 || op2 == 0x28 || op2 == 0x29 || op2 == 0x62 || op2 == 0x6c || op2 == 0x6f || op2 == 0x70 || op2 == 0x7e || op2 == 0x7f || op2 == 0xc6 || op2 == 0xd6 || op2 == 0xeb || op2 == 0xef) {
            struct amd64_modrm modrm;
            union xmm_reg value;
            union xmm_reg src_xmm;
            qword_t src_scalar;
            uint8_t imm8;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            if (modrm.reg >= 8 || (modrm.is_reg && modrm.rm >= 8))
                return INT_UNDEFINED;
            if (op2 == 0x10 || op2 == 0x28 || op2 == 0x6f) {
                if (op2 == 0x6f && !(operand_size_prefix || rep_mode == AMD64_REPZ))
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                    goto amd64_gpf_restore;
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x11 || op2 == 0x29 || op2 == 0x7f) {
                if (op2 == 0x7f && !(operand_size_prefix || rep_mode == AMD64_REPZ))
                    return INT_UNDEFINED;
                value = cpu->xmm[modrm.reg];
                if (!amd64_write_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                    goto amd64_gpf_restore;
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
            } else if (op2 == 0x62) {
                union xmm_reg dst = cpu->xmm[modrm.reg];
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value.u32[0] = dst.u32[0];
                value.u32[1] = src_xmm.u32[0];
                value.u32[2] = dst.u32[1];
                value.u32[3] = src_xmm.u32[1];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x7e) {
                if (rep_mode != AMD64_REPZ)
                    return INT_UNDEFINED;
                value.u128 = 0;
                if (modrm.is_reg) {
                    value.qw[0] = cpu->xmm[modrm.rm].qw[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_gpf_restore;
                    value.qw[0] = src_scalar;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x70) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = (addr_t) saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                for (int i = 0; i < 4; i++)
                    value.u32[i] = src_xmm.u32[(imm8 >> (i * 2)) & 3];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xc6) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = (addr_t) saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.u32[0] = cpu->xmm[modrm.reg].u32[(imm8 >> 0) & 3];
                value.u32[1] = cpu->xmm[modrm.reg].u32[(imm8 >> 2) & 3];
                value.u32[2] = src_xmm.u32[(imm8 >> 4) & 3];
                value.u32[3] = src_xmm.u32[(imm8 >> 6) & 3];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xd6) {
                if (!operand_size_prefix || modrm.is_reg)
                    return INT_UNDEFINED;
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->xmm[modrm.reg].qw[0]))
                    goto amd64_gpf_restore;
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
        if (op2 == 0x72) {
            struct amd64_modrm modrm;
            union xmm_reg value;
            uint8_t imm8;
            unsigned count;
            if (!operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            if (!modrm.is_reg || modrm.rm >= 8)
                return INT_UNDEFINED;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            count = imm8 > 31 ? 31 : imm8;
            value = cpu->xmm[modrm.rm];
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
            cpu->xmm[modrm.rm] = value;
            break;
        }
        if (op2 == 0xa3) {
            struct amd64_modrm modrm;
            qword_t lhs;
            qword_t bit;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            bit = amd64_reg_get(cpu, modrm.reg, op_size) & (op_size - 1);
            cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
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
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
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
                cpu->segfault_addr = (addr_t) saved_rip;
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
        if (op2 == 0xb1) {
            struct amd64_modrm modrm;
            qword_t dst, src, acc, result;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &dst))
                goto amd64_gpf_restore;
            src = amd64_reg_get(cpu, modrm.reg, op_size);
            acc = amd64_reg_get(cpu, amd64_rax, op_size);
            result = amd64_trunc(acc - dst, op_size);
            amd64_set_sub_flags(cpu, acc, dst, result, op_size);
            if (acc == dst) {
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, src))
                    goto amd64_gpf_restore;
                if (!modrm.is_reg && op_size == 64)
                    amd64_trace_qword_store(cpu, saved_rip, 0x0f, amd64_effective_addr(cpu, &modrm, fs_prefix), src);
                cpu->zf = 1;
                cpu->zf_res = 0;
            } else {
                amd64_reg_set(cpu, amd64_rax, op_size, dst);
                cpu->zf = 0;
                cpu->zf_res = 0;
            }
            break;
        }
        if (op2 == 0x1f) {
            struct amd64_modrm modrm;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
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
            cpu->segfault_addr = (addr_t) saved_rip;
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
            break;
        case 0x86:
        case 0x87: {
            unsigned xchg_size = opcode == 0x86 ? 8 : op_size;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, xchg_size, &lhs))
                goto amd64_gpf_restore;
            rhs = opcode == 0x86 ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                                 : amd64_reg_get(cpu, modrm.reg, xchg_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, xchg_size, rhs))
                goto amd64_gpf_restore;
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
                    cpu->segfault_addr = (addr_t) saved_rip;
                    return INT_GPF;
                }
                imm_signed = imm32;
            } else {
                int8_t imm8;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = (addr_t) saved_rip;
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
    case 0x68: {
        int32_t imm32;
        if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = (addr_t) saved_rip;
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
            cpu->segfault_addr = (addr_t) saved_rip;
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
            cpu->segfault_addr = (addr_t) saved_rip;
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
            cpu->segfault_addr = (addr_t) saved_rip;
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
            cpu->segfault_addr = (addr_t) saved_rip;
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
                    cpu->segfault_addr = (addr_t) saved_rip;
                    return INT_GPF;
                }
                rhs = imm8;
            } else {
                int32_t imm32;
                if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = (addr_t) saved_rip;
                    return INT_GPF;
                }
                rhs = rex.w ? (qword_t) (sqword_t) imm32 : (uint32_t) imm32;
            }
            amd64_set_logic_flags(cpu, lhs & rhs, size);
            break;
        }
        result = amd64_grp3_muldiv(cpu, tlb, &modrm, fs_prefix, size);
        if (result == INT_GPF)
            goto amd64_gpf_restore;
        if (result != INT_NONE)
            return result;
        break;
    }
    case 0x70 ... 0x7f: {
        int8_t rel8;
        if (!amd64_fetch(cpu, tlb, &rel8, sizeof(rel8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = (addr_t) saved_rip;
            return INT_GPF;
        }
        if (amd64_cond_eval(cpu, opcode & 0xf))
            cpu->amd64_rip += rel8;
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
            cpu->segfault_addr = (addr_t) saved_rip;
            return INT_GPF;
        }
        if ((opcode == 0xc6 || opcode == 0xc7) && modrm.reg != 0)
            return INT_UNDEFINED;
        if (opcode == 0xc6) {
            uint8_t imm8;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
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
                cpu->segfault_addr = (addr_t) saved_rip;
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
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            rhs = imm8;
        } else if (opcode == 0x83) {
            int8_t imm8;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            rhs = (qword_t) amd64_sign_extend((uint8_t) imm8, 8);
        } else if (opcode == 0xc7 && op_size == 16) {
            uint16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            rhs = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            rhs = opcode == 0xc7 && !rex.w ? (uint32_t) imm32 : (qword_t) (sqword_t) imm32;
        }
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
            goto amd64_gpf_restore;

        if (opcode == 0xc7) {
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, rhs))
                goto amd64_gpf_restore;
            if (!modrm.is_reg && op_size == 64)
                amd64_trace_qword_store(cpu, saved_rip, opcode,
                        amd64_effective_addr(cpu, &modrm, fs_prefix), rhs);
            break;
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
            cpu->segfault_addr = (addr_t) saved_rip;
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
            cpu->segfault_addr = (addr_t) saved_rip;
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
            cpu->segfault_addr = (addr_t) saved_rip;
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
            cpu->segfault_addr = (addr_t) saved_rip;
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
                cpu->segfault_addr = (addr_t) saved_rip;
                return INT_GPF;
            }
            amd64_reg_set(cpu, reg, 64, imm64);
        } else {
            uint32_t imm32;
            if (!amd64_fetch_u32(cpu, tlb, &imm32)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = (addr_t) saved_rip;
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
        cpu->amd64_rip = target;
        break;
    }
    case 0xd1:
    case 0xd3: {
        struct amd64_modrm modrm;
        qword_t lhs, result;
        unsigned count, effective_count;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = (addr_t) saved_rip;
            return INT_GPF;
        }
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
            goto amd64_gpf_restore;
        count = opcode == 0xd1 ? 1 : (amd64_reg_get(cpu, amd64_rcx, 8) & (op_size == 64 ? 0x3f : 0x1f));
        effective_count = (modrm.reg == 0 || modrm.reg == 1) ? (count % op_size) : count;
        if (effective_count == 0)
            break;
        switch (modrm.reg) {
        case 0:
        case 1:
            result = amd64_rotate_value(lhs, op_size, count, modrm.reg);
            break;
        case 4:
            result = amd64_trunc(lhs << count, op_size);
            break;
        case 5:
            result = amd64_trunc(amd64_trunc(lhs, op_size) >> count, op_size);
            break;
        case 7:
            result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, op_size) >> count), op_size);
            break;
        default:
            return INT_UNDEFINED;
        }
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
            goto amd64_gpf_restore;
        if (modrm.reg == 0 || modrm.reg == 1)
            amd64_set_rotate_flags(cpu, result, op_size, count, modrm.reg);
        else
            amd64_set_shift_flags(cpu, lhs, result, op_size, count, modrm.reg);
        break;
    }
    case 0xe8: {
        int32_t rel32;
        if (!amd64_fetch(cpu, tlb, &rel32, sizeof(rel32))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = (addr_t) saved_rip;
            return INT_GPF;
        }
        qword_t return_rip = cpu->amd64_rip;
        if (!amd64_push(cpu, tlb, return_rip))
            goto amd64_gpf_restore;
        cpu->amd64_rip += rel32;
        break;
    }
    case 0xe9: {
        int32_t rel32;
        if (!amd64_fetch(cpu, tlb, &rel32, sizeof(rel32))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = (addr_t) saved_rip;
            return INT_GPF;
        }
        cpu->amd64_rip += rel32;
        break;
    }
    case 0xeb: {
        int8_t rel8;
        if (!amd64_fetch(cpu, tlb, &rel8, sizeof(rel8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = (addr_t) saved_rip;
            return INT_GPF;
        }
        cpu->amd64_rip += rel8;
        break;
    }
    case 0xfe: {
        struct amd64_modrm modrm;
        qword_t lhs, result;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = (addr_t) saved_rip;
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
            cpu->segfault_addr = (addr_t) saved_rip;
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
            cpu->amd64_rip = value;
            break;
        }
        case 4:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
                goto amd64_gpf_restore;
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

    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_gpf_restore:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_GPF;
}

int cpu_run_to_interrupt_amd64(struct cpu_state *cpu, struct tlb *tlb) {
    cpu->poked_ptr = &cpu->_poked;
    tlb_refresh(tlb, cpu->mmu);

    int steps = 0;
    while (true) {
        int interrupt = amd64_step_to_interrupt(cpu, tlb);
        if (interrupt == INT_UNDEFINED)
            cpu->amd64_rip = cpu->eip;
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
