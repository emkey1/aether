#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jit/gen.h"
#include "emu/modrm.h"
#include "emu/cpuid.h"
#include "emu/fpu.h"
#include "emu/vec.h"
#include "emu/interrupt.h"

static int gen_step32(struct gen_state *state, struct tlb *tlb);
static int gen_step16(struct gen_state *state, struct tlb *tlb);
static int gen_step64(struct gen_state *state, struct tlb *tlb);

enum amd64_jit_rep_mode {
    amd64_jit_rep_none,
    amd64_jit_repz,
    amd64_jit_repnz,
};

struct amd64_jit_rex_prefix {
    bool present;
    bool w;
    bool r;
    bool x;
    bool b;
};

struct amd64_jit_insn {
    guest_addr_t start_ip;
    guest_addr_t end_ip;
    byte_t opcode;
    byte_t op2;
    byte_t modrm;
    bool two_byte_opcode;
    bool has_modrm;
    bool operand_size_prefix;
    bool address_size_prefix;
    bool fs_prefix;
    bool lock_prefix;
    enum amd64_jit_rep_mode rep_mode;
    struct amd64_jit_rex_prefix rex;
};

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

static inline byte_t amd64_modrm_mod(byte_t modrm) {
    return (modrm >> 6) & 0x3;
}

static inline byte_t amd64_modrm_reg(byte_t modrm) {
    return (modrm >> 3) & 0x7;
}

static inline byte_t amd64_modrm_rm(byte_t modrm) {
    return modrm & 0x7;
}

static inline bool amd64_jit_ignored_segment_prefix(byte_t byte) {
    return byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e;
}

static bool amd64_opcode_needs_modrm(const struct amd64_jit_insn *insn) {
    if (insn->two_byte_opcode) {
        switch (insn->op2) {
        case 0x10:
        case 0x18:
        case 0x1f:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x2a:
        case 0x2c:
        case 0x2e:
        case 0x2f:
        case 0x28:
        case 0x29:
        case 0x50:
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
        case 0x58:
        case 0x59:
        case 0x5a:
        case 0x5c:
        case 0x5d:
        case 0x5e:
        case 0x5f:
        case 0x60:
        case 0x61:
        case 0x62:
        case 0x63:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
        case 0x68:
        case 0x69:
        case 0x6a:
        case 0x6b:
        case 0x6c:
        case 0x6d:
        case 0x6e:
        case 0x6f:
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0x74:
        case 0x75:
        case 0x76:
        case 0x7e:
        case 0x7f:
        case 0x40 ... 0x4f:
        case 0x90 ... 0x9f:
        case 0xa3:
        case 0xa4:
        case 0xa5:
        case 0xab:
        case 0xac:
        case 0xad:
        case 0xaf:
        case 0xb0:
        case 0xb1:
        case 0xb3:
        case 0xba:
        case 0xb6:
        case 0xb7:
        case 0xbe:
        case 0xbf:
        case 0xbc:
        case 0xbd:
        case 0xbb:
        case 0xc0:
        case 0xc1:
        case 0xc2:
        case 0xc4:
        case 0xc5:
        case 0xc6:
        case 0xd1:
        case 0xd2:
        case 0xd3:
        case 0xd4:
        case 0xd5:
        case 0xd6:
        case 0xd7:
        case 0xd8:
        case 0xd9:
        case 0xda:
        case 0xdb:
        case 0xdc:
        case 0xdd:
        case 0xde:
        case 0xdf:
        case 0xe0:
        case 0xe1:
        case 0xe2:
        case 0xe3:
        case 0xe4:
        case 0xe5:
        case 0xe7:
        case 0xe8:
        case 0xe9:
        case 0xea:
        case 0xeb:
        case 0xec:
        case 0xed:
        case 0xee:
        case 0xef:
        case 0xf1:
        case 0xf2:
        case 0xf3:
        case 0xf4:
        case 0xf6:
        case 0xf8:
        case 0xf9:
        case 0xfa:
        case 0xfb:
        case 0xfc:
        case 0xfd:
        case 0xfe:
            return true;
        default:
            return false;
        }
    }

    switch (insn->opcode) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0b:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x18:
    case 0x19:
    case 0x1a:
    case 0x1b:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x38:
    case 0x39:
    case 0x3a:
    case 0x3b:
    case 0x63:
    case 0x69:
    case 0x6b:
    case 0x80:
    case 0x81:
    case 0x83:
    case 0x84:
    case 0x85:
    case 0x86:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x8b:
    case 0x8d:
    case 0x8f:
    case 0xc0:
    case 0xc1:
    case 0xc6:
    case 0xc7:
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3:
    case 0xf6:
    case 0xf7:
    case 0xfe:
    case 0xff:
        return true;
    default:
        return false;
    }
}

static bool amd64_jit_debug_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_JIT") != NULL ? 1 : 0;
    return enabled == 1;
}

static void amd64_jit_debug(const char *fmt, ...) {
    if (!amd64_jit_debug_enabled())
        return;

    va_list args;
    va_start(args, fmt);
    fputs("[amd64-jit] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

int gen_step(struct gen_state *state, struct tlb *tlb) {
    state->orig_ip = state->ip;
    state->orig_ip_extra = 0;
    if (state->amd64)
        return gen_step64(state, tlb);
    return gen_step32(state, tlb);
}

static void gen(struct gen_state *state, unsigned long thing) {
    assert(state->size <= state->capacity);
    if (state->size >= state->capacity) {
        state->capacity *= 2;
        struct jit_block *bigger_block = realloc(state->block,
                sizeof(struct jit_block) + state->capacity * sizeof(unsigned long));
        if (bigger_block == NULL) {
            if (state->oom_active)
                longjmp(state->oom_recovery, 1);
            die("out of memory while jitting");
        }
        state->block = bigger_block;
    }
    assert(state->size < state->capacity);
    state->block->code[state->size++] = thing;
}

static void gen_amd64_emit_rip(struct gen_state *state, guest_addr_t rip) {
    extern void gadget_amd64_set_rip(void);
    state->amd64_deferred_rip_valid = false;
    gen(state, (unsigned long) gadget_amd64_set_rip);
    gen(state, (unsigned long) rip);
}

static void gen_amd64_flush_reg_cache(struct gen_state *state) {
#if defined(__aarch64__)
    if (state->amd64_reg_cache_valid && state->amd64_reg_cache_dirty) {
        extern void gadget_amd64_store_low8_reg_cache(void);
        gen(state, (unsigned long) gadget_amd64_store_low8_reg_cache);
    }
#endif
    state->amd64_reg_cache_valid = false;
    state->amd64_reg_cache_dirty = false;
}

__attribute__((unused)) static void gen_amd64_ensure_reg_cache(struct gen_state *state) {
#if defined(__aarch64__)
    if (!state->amd64_reg_cache_valid) {
        extern void gadget_amd64_load_low8_reg_cache(void);
        gen(state, (unsigned long) gadget_amd64_load_low8_reg_cache);
        state->amd64_reg_cache_valid = true;
        state->amd64_reg_cache_dirty = false;
    }
#else
    (void) state;
#endif
}

__attribute__((unused)) static void gen_amd64_mark_reg_cache_dirty(struct gen_state *state) {
#if defined(__aarch64__)
    state->amd64_reg_cache_dirty = true;
#else
    (void) state;
#endif
}

static void gen_amd64_defer_rip(struct gen_state *state, guest_addr_t rip) {
    state->amd64_deferred_rip = rip;
    state->amd64_deferred_rip_valid = true;
}

static void gen_amd64_flush_rip(struct gen_state *state) {
    if (state->amd64_deferred_rip_valid)
        gen_amd64_emit_rip(state, state->amd64_deferred_rip);
}

static void gen_amd64_jmp_rel(struct gen_state *state, guest_addr_t target_ip) {
    extern void gadget_amd64_jmp(void);
    gen_amd64_flush_reg_cache(state);
    state->amd64_deferred_rip_valid = false;
    gen(state, (unsigned long) gadget_amd64_jmp);
    gen(state, (unsigned long) (target_ip | (1ull << 63)));
    state->jump_ip[0] = state->size - 1;
}

// Emit a native amd64 conditional jump. The 16 x86 condition codes map onto the
// 8 do_jump base conditions (base = cc>>1) plus a swap of the two targets for the
// negated (odd) codes. The gadget evaluates the condition from the eager eflags,
// sets CPU_amd64_rip to the taken-or-else target, and exits the block (the main
// loop resolves the dynamic target, exactly as the bridge did but without the
// gadget->C->gadget round-trip). flush + invalidate the deferred rip like
// gen_amd64_jmp_rel; the gadget writes rip itself, so no static link.
static void gen_amd64_jcc(struct gen_state *state, unsigned cc,
        guest_addr_t target_ip, guest_addr_t next_ip) {
    extern void gadget_amd64_jcc_o(void), gadget_amd64_jcc_c(void),
            gadget_amd64_jcc_z(void), gadget_amd64_jcc_cz(void),
            gadget_amd64_jcc_s(void), gadget_amd64_jcc_p(void),
            gadget_amd64_jcc_sxo(void), gadget_amd64_jcc_sxoz(void);
    static void (* const gadgets[8])(void) = {
        gadget_amd64_jcc_o, gadget_amd64_jcc_c, gadget_amd64_jcc_z, gadget_amd64_jcc_cz,
        gadget_amd64_jcc_s, gadget_amd64_jcc_p, gadget_amd64_jcc_sxo, gadget_amd64_jcc_sxoz,
    };
    bool swap = cc & 1;
    gen_amd64_flush_reg_cache(state);
    state->amd64_deferred_rip_valid = false;
    gen(state, (unsigned long) gadgets[(cc >> 1) & 7]);
    gen(state, (unsigned long) (swap ? next_ip : target_ip));   // operand 0: taken
    gen(state, (unsigned long) (swap ? target_ip : next_ip));   // operand 1: else
}

__attribute__((unused)) static bool amd64_jit_low8_reg(unsigned reg) {
    return reg < 8;
}

bool gen_start(guest_addr_t addr, struct gen_state *state) {
    state->amd64 = false;
    state->amd64_fallback_to_interp = false;
    state->amd64_abort_block_to_interp = false;
    state->amd64_deferred_rip_valid = false;
    state->amd64_reg_cache_valid = false;
    state->amd64_reg_cache_dirty = false;
    state->amd64_deferred_rip = addr;
    state->amd64_fallback_ip = addr;
    state->amd64_fallback_opcode = 0;
    state->amd64_fallback_op2 = 0;
    state->amd64_fallback_flags = 0;
    state->capacity = JIT_BLOCK_INITIAL_CAPACITY;
    state->size = 0;
    state->ip = addr;
    state->amd64_ip = addr;
    state->amd64_orig_ip = addr;
    state->oom_active = false;
    for (int i = 0; i <= 1; i++) {
        state->jump_ip[i] = 0;
    }
    state->block_patch_ip = 0;

    struct jit_block *block = malloc(sizeof(struct jit_block) + state->capacity * sizeof(unsigned long));
    if (block == NULL) {
        state->block = NULL;
        return false;
    }
    state->block = block;
    block->addr = addr;
    return true;
}

bool gen_start_amd64(guest_addr_t addr, struct gen_state *state) {
    if (!gen_start(addr, state))
        return false;
    state->amd64 = true;
    return true;
}

static bool gen_fetch_amd64(struct gen_state *state, struct tlb *tlb, void *out, size_t size) {
    if (!tlb_read(tlb, state->amd64_ip, out, size))
        return false;
    state->amd64_ip += size;
    return true;
}

static bool gen_decode_amd64(struct gen_state *state, struct tlb *tlb,
        struct amd64_jit_insn *insn) {
    byte_t byte;

    insn->start_ip = state->amd64_orig_ip;
    insn->end_ip = state->amd64_orig_ip;
    insn->opcode = 0;
    insn->op2 = 0;
    insn->modrm = 0;
    insn->two_byte_opcode = false;
    insn->has_modrm = false;
    insn->operand_size_prefix = false;
    insn->address_size_prefix = false;
    insn->fs_prefix = false;
    insn->lock_prefix = false;
    insn->rep_mode = amd64_jit_rep_none;
    insn->rex = (struct amd64_jit_rex_prefix) {0};

    for (;;) {
        if (!gen_fetch_amd64(state, tlb, &byte, sizeof(byte)))
            return false;
        if (byte == 0x66) {
            insn->operand_size_prefix = true;
            continue;
        }
        if (byte == 0x67) {
            insn->address_size_prefix = true;
            continue;
        }
        if (amd64_jit_ignored_segment_prefix(byte)) {
            continue;
        }
        if (byte == 0x64) {
            insn->fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            insn->lock_prefix = true;
            continue;
        }
        if (byte == 0xf2) {
            insn->rep_mode = amd64_jit_repnz;
            continue;
        }
        if (byte == 0xf3) {
            insn->rep_mode = amd64_jit_repz;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            insn->rex.present = true;
            insn->rex.w = (byte & 8) != 0;
            insn->rex.r = (byte & 4) != 0;
            insn->rex.x = (byte & 2) != 0;
            insn->rex.b = (byte & 1) != 0;
            continue;
        }
        insn->opcode = byte;
        break;
    }

    if (insn->opcode == 0x0f) {
        if (!gen_fetch_amd64(state, tlb, &insn->op2, sizeof(insn->op2)))
            return false;
        insn->two_byte_opcode = true;
    }
    if (amd64_opcode_needs_modrm(insn)) {
        if (!tlb_read(tlb, state->amd64_ip, &insn->modrm, sizeof(insn->modrm)))
            return false;
        insn->has_modrm = true;
    }
    insn->end_ip = state->amd64_ip;
    return true;
}

static bool amd64_jit_plain_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->operand_size_prefix &&
        !insn->address_size_prefix &&
        !insn->fs_prefix &&
        !insn->lock_prefix &&
        insn->rep_mode == amd64_jit_rep_none;
}

static bool amd64_jit_one_byte_plain_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->two_byte_opcode &&
        !insn->operand_size_prefix &&
        !insn->address_size_prefix &&
        !insn->fs_prefix &&
        !insn->lock_prefix &&
        insn->rep_mode == amd64_jit_rep_none;
}

static bool amd64_jit_branch_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->operand_size_prefix &&
        !insn->address_size_prefix &&
        !insn->fs_prefix &&
        !insn->lock_prefix &&
        insn->rep_mode == amd64_jit_rep_none;
}

static bool amd64_jit_one_byte_branch_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->two_byte_opcode && amd64_jit_branch_prefixes(insn);
}

static bool amd64_jit_one_byte_rel_call_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->two_byte_opcode &&
        !insn->operand_size_prefix &&
        !insn->fs_prefix &&
        !insn->lock_prefix &&
        insn->rep_mode == amd64_jit_rep_none;
}

static bool amd64_jit_one_byte_ret_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->two_byte_opcode;
}

static bool amd64_jit_push_pop_prefixes_ok(const struct amd64_jit_insn *insn) {
    if (!amd64_jit_one_byte_plain_prefixes(insn))
        return false;
    if (!insn->rex.present)
        return true;
    return !insn->rex.w && !insn->rex.r && !insn->rex.x;
}

static bool gen_amd64_decode_mem_meta(struct gen_state *state, struct tlb *tlb,
        const struct amd64_jit_insn *insn, unsigned size,
        unsigned long *meta_out, unsigned long *disp_out,
        guest_addr_t *next_ip_out) {
    guest_addr_t ip = state->amd64_ip + 1;
    byte_t modrm = insn->modrm;
    unsigned mod = amd64_modrm_mod(modrm);
    unsigned rm_low = amd64_modrm_rm(modrm);
    unsigned reg_id = amd64_modrm_reg(modrm) | (insn->rex.r ? 8 : 0);
    unsigned base = 0;
    unsigned index = 0;
    unsigned scale = 0;
    bool has_base = false;
    bool has_index = false;
    bool rip_relative = false;
    int32_t disp = 0;

    if (mod == 3 || insn->address_size_prefix)
        return false;

    if (rm_low == 4) {
        byte_t sib;
        if (!tlb_read(tlb, ip, &sib, sizeof(sib)))
            return false;
        ip += sizeof(sib);
        unsigned base_low = amd64_modrm_rm(sib);
        unsigned index_low = amd64_modrm_reg(sib);
        scale = (sib >> 6) & 0x3;
        if (index_low != 4 || insn->rex.x) {
            has_index = true;
            index = index_low | (insn->rex.x ? 8 : 0);
        }
        // mod=00 with base=101 means no base + disp32, regardless of REX.B
        // (r13 as a base requires mod=01/10).
        if (mod == 0 && base_low == 5) {
            has_base = false;
        } else {
            has_base = true;
            base = base_low | (insn->rex.b ? 8 : 0);
        }
    } else if (mod == 0 && rm_low == 5) {
        rip_relative = true;
    } else {
        has_base = true;
        base = rm_low | (insn->rex.b ? 8 : 0);
    }

    if (mod == 1) {
        int8_t disp8;
        if (!tlb_read(tlb, ip, &disp8, sizeof(disp8)))
            return false;
        disp = disp8;
        ip += sizeof(disp8);
    } else if (mod == 2 || (mod == 0 && (rm_low == 5 ||
            (rm_low == 4 && !has_base)))) {
        if (!tlb_read(tlb, ip, &disp, sizeof(disp)))
            return false;
        ip += sizeof(disp);
    }

    *meta_out = ((unsigned long) insn->opcode << AMD64_JIT_MEM_OPCODE_SHIFT) |
        ((unsigned long) reg_id << AMD64_JIT_MEM_REG_SHIFT) |
        ((unsigned long) size << AMD64_JIT_MEM_SIZE_SHIFT) |
        ((unsigned long) base << AMD64_JIT_MEM_BASE_SHIFT) |
        ((unsigned long) index << AMD64_JIT_MEM_INDEX_SHIFT) |
        ((unsigned long) scale << AMD64_JIT_MEM_SCALE_SHIFT);
    if (has_base)
        *meta_out |= AMD64_JIT_MEM_HAS_BASE;
    if (has_index)
        *meta_out |= AMD64_JIT_MEM_HAS_INDEX;
    if (rip_relative)
        *meta_out |= AMD64_JIT_MEM_RIP_REL;
    if (insn->fs_prefix)
        *meta_out |= AMD64_JIT_MEM_FS;
    if (insn->rex.present)
        *meta_out |= AMD64_JIT_MEM_REX_PRESENT;
    *disp_out = (unsigned long) (qword_t) (sqword_t) disp;
    *next_ip_out = ip;
    return true;
}

static bool gen_amd64_decode_rm_extent(struct gen_state *state, struct tlb *tlb,
        const struct amd64_jit_insn *insn, guest_addr_t *next_ip_out) {
    guest_addr_t ip = state->amd64_ip + 1;
    byte_t modrm = insn->modrm;
    unsigned mod = amd64_modrm_mod(modrm);
    unsigned rm_low = amd64_modrm_rm(modrm);
    bool has_base = true;

    if (insn->address_size_prefix)
        return false;
    if (mod == 3) {
        *next_ip_out = ip;
        return true;
    }
    if (rm_low == 4) {
        byte_t sib;
        if (!tlb_read(tlb, ip, &sib, sizeof(sib)))
            return false;
        ip += sizeof(sib);
        unsigned base_low = amd64_modrm_rm(sib);
        has_base = !(mod == 0 && base_low == 5);
    } else if (mod == 0 && rm_low == 5) {
        has_base = false;
    }

    if (mod == 1) {
        ip += sizeof(int8_t);
    } else if (mod == 2 || (mod == 0 && (rm_low == 5 ||
            (rm_low == 4 && !has_base)))) {
        ip += sizeof(int32_t);
    }
    *next_ip_out = ip;
    return true;
}

static void gen_amd64_helper_tlb_0_retint(struct gen_state *state, void *helper) {
    extern void gadget_helper_tlb_0_retint(void);
    gen_amd64_flush_reg_cache(state);
    gen_amd64_flush_rip(state);
    gen(state, (unsigned long) gadget_helper_tlb_0_retint);
    gen(state, (unsigned long) helper);
}

static void gen_amd64_helper_tlb_1_retint(struct gen_state *state, void *helper,
        unsigned long arg0) {
    extern void gadget_helper_tlb_1_retint(void);
    gen_amd64_flush_reg_cache(state);
    gen_amd64_flush_rip(state);
    gen(state, (unsigned long) gadget_helper_tlb_1_retint);
    gen(state, (unsigned long) helper);
    gen(state, arg0);
}

static void gen_amd64_helper_tlb_2_retint(struct gen_state *state, void *helper,
        unsigned long arg0, unsigned long arg1) {
    extern void gadget_helper_tlb_2_retint(void);
    gen_amd64_flush_reg_cache(state);
    gen_amd64_flush_rip(state);
    gen(state, (unsigned long) gadget_helper_tlb_2_retint);
    gen(state, (unsigned long) helper);
    gen(state, arg0);
    gen(state, arg1);
}

static void gen_amd64_helper_tlb_3_retint(struct gen_state *state, void *helper,
        unsigned long arg0, unsigned long arg1, unsigned long arg2) {
    extern void gadget_helper_tlb_3_retint(void);
    gen_amd64_flush_reg_cache(state);
    gen_amd64_flush_rip(state);
    gen(state, (unsigned long) gadget_helper_tlb_3_retint);
    gen(state, (unsigned long) helper);
    gen(state, arg0);
    gen(state, arg1);
    gen(state, arg2);
}

int gen_step_amd64(struct gen_state *state, struct tlb *tlb) {
    return gen_step64(state, tlb);
}

static int gen_step64(struct gen_state *state, struct tlb *tlb) {
    struct amd64_jit_insn insn;
    int8_t rel8;
    int32_t rel32;
    guest_addr_t next_ip;
    guest_addr_t target_ip;
    unsigned long reg;

    state->amd64_orig_ip = state->amd64_ip;
    state->orig_ip_extra = 0;
    state->amd64_fallback_to_interp = false;
    state->amd64_fallback_ip = state->amd64_orig_ip;

    if (!gen_decode_amd64(state, tlb, &insn)) {
        amd64_jit_debug("decode fail ip=%llx",
                (unsigned long long) state->amd64_orig_ip);
        state->amd64_ip = state->amd64_orig_ip;
        state->amd64_fallback_to_interp = true;
        state->amd64_fallback_opcode = 0xff;
        state->amd64_fallback_op2 = 0;
        state->amd64_fallback_flags = 0x80;
        return false;
    }

    state->amd64_fallback_opcode = insn.opcode;
    state->amd64_fallback_op2 = insn.op2;
    state->amd64_fallback_flags =
        (insn.two_byte_opcode ? 0x01 : 0) |
        (insn.rex.present ? 0x02 : 0) |
        (insn.rex.w ? 0x04 : 0) |
        (insn.operand_size_prefix ? 0x08 : 0) |
        (insn.address_size_prefix ? 0x10 : 0) |
        (insn.fs_prefix ? 0x20 : 0) |
        (insn.rep_mode != amd64_jit_rep_none ? 0x40 : 0);

    if (amd64_jit_one_byte_ret_prefixes(&insn) && insn.opcode == 0xc3) {
        amd64_jit_debug("ret ip=%llx",
                (unsigned long long) insn.start_ip);
        // Native ret: pop the return address into rip and exit the block. The
        // gadget branches to jit_ret itself (indirect target -> no static link),
        // so no gen_exit. rip flushed so a #PF on the stack read re-executes.
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_ret(void);
        gen(state, (unsigned long) gadget_amd64_ret);
        return false;
    }

    if (amd64_jit_one_byte_ret_prefixes(&insn) && insn.opcode == 0xc2) {
        uint16_t imm16;
        if (!tlb_read(tlb, state->amd64_ip, &imm16, sizeof(imm16))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(imm16);
        state->amd64_ip = next_ip;
        amd64_jit_debug("ret-imm-helper ip=%llx imm=%u",
                (unsigned long long) insn.start_ip,
                (unsigned) imm16);
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_ret_imm,
                (unsigned long) imm16);
        gen_exit(state);
        return false;
    }

    if (amd64_jit_one_byte_ret_prefixes(&insn) && insn.opcode == 0xc9) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        if (insn.operand_size_prefix) {
            // 16-bit leave is rare; keep bridging it (ends the block).
            amd64_jit_debug("leave16-helper ip=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    (unsigned long long) next_ip);
            gen_amd64_helper_tlb_2_retint(state, amd64_jit_leave,
                    16, (unsigned long) next_ip);
            gen_exit(state);
            return false;
        }
        amd64_jit_debug("leave ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        // Native 64-bit leave continues in-block like push/pop.
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_leave(void);
        gen(state, (unsigned long) gadget_amd64_leave);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (amd64_jit_one_byte_branch_prefixes(&insn) && insn.opcode == 0xe9) {
        if (!tlb_read(tlb, state->amd64_ip, &rel32, sizeof(rel32))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel32);
        target_ip = next_ip + rel32;
        state->amd64_ip = next_ip;
        amd64_jit_debug("jmp-rel32-direct ip=%llx target=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) target_ip);
        gen_amd64_jmp_rel(state, target_ip);
        return false;
    }

    if (amd64_jit_one_byte_rel_call_prefixes(&insn) && insn.opcode == 0xe8) {
        if (!tlb_read(tlb, state->amd64_ip, &rel32, sizeof(rel32))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel32);
        target_ip = next_ip + rel32;
        state->amd64_ip = next_ip;
        amd64_jit_debug("call-rel32 ip=%llx target=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) target_ip,
                (unsigned long long) next_ip);
        // Native call: push the 64-bit return address (reusing the push-imm
        // gadget, which stores its operand verbatim), then statically link to
        // the target block exactly as jmp rel32 does. A page fault during the
        // push re-executes this instruction (rip flushed to the call's addr).
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_push_imm(void);
        gen(state, (unsigned long) gadget_amd64_push_imm);
        gen(state, (unsigned long) next_ip);
        gen_amd64_jmp_rel(state, target_ip);
        return false;
    }

    if (amd64_jit_one_byte_branch_prefixes(&insn) && insn.opcode == 0xeb) {
        if (!tlb_read(tlb, state->amd64_ip, &rel8, sizeof(rel8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel8);
        target_ip = next_ip + rel8;
        state->amd64_ip = next_ip;
        amd64_jit_debug("jmp-rel8-direct ip=%llx target=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) target_ip);
        gen_amd64_jmp_rel(state, target_ip);
        return false;
    }

    if (amd64_jit_one_byte_branch_prefixes(&insn) &&
            insn.opcode >= 0x70 && insn.opcode <= 0x7f) {
        if (!tlb_read(tlb, state->amd64_ip, &rel8, sizeof(rel8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel8);
        target_ip = next_ip + rel8;
        state->amd64_ip = next_ip;
        amd64_jit_debug("jcc-rel8 ip=%llx cc=%u target=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned) (insn.opcode & 0xf),
                (unsigned long long) target_ip,
                (unsigned long long) next_ip);
        gen_amd64_jcc(state, insn.opcode & 0xf, target_ip, next_ip);
        return false;
    }

    if (amd64_jit_branch_prefixes(&insn) && insn.two_byte_opcode &&
            insn.op2 >= 0x80 && insn.op2 <= 0x8f) {
        if (!tlb_read(tlb, state->amd64_ip, &rel32, sizeof(rel32))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel32);
        target_ip = next_ip + rel32;
        state->amd64_ip = next_ip;
        amd64_jit_debug("jcc-rel32 ip=%llx cc=%u target=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned) (insn.op2 & 0xf),
                (unsigned long long) target_ip,
                (unsigned long long) next_ip);
        gen_amd64_jcc(state, insn.op2 & 0xf, target_ip, next_ip);
        return false;
    }

    if (amd64_jit_plain_prefixes(&insn) && insn.two_byte_opcode &&
            insn.op2 == 0x05) {
        next_ip = insn.end_ip;
        amd64_jit_debug("syscall-helper ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_syscall,
                (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (amd64_jit_plain_prefixes(&insn) && insn.two_byte_opcode &&
            (insn.op2 == 0x31 || insn.op2 == 0xa2)) {
        next_ip = insn.end_ip;
        amd64_jit_debug("%s-helper ip=%llx next=%llx",
                insn.op2 == 0x31 ? "rdtsc" : "cpuid",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_1_retint(state,
                insn.op2 == 0x31 ? amd64_jit_rdtsc : amd64_jit_cpuid,
                (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.address_size_prefix && !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_repz && insn.two_byte_opcode &&
            insn.op2 == 0x1e) {
        byte_t op3;
        if (!tlb_read(tlb, state->amd64_ip, &op3, sizeof(op3))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (op3 == 0xfa || op3 == 0xfb) {
            next_ip = state->amd64_ip + sizeof(op3);
            state->amd64_ip = next_ip;
            amd64_jit_debug("endbr-nop ip=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    (unsigned long long) next_ip);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
    }

    if (!insn.address_size_prefix && !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.two_byte_opcode &&
            insn.op2 >= 0xc8 && insn.op2 <= 0xcf) {
        unsigned size = insn.rex.w ? 64 : 32;
        reg = (unsigned long) (insn.op2 - 0xc8);
        if (insn.rex.b)
            reg |= 8;
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("bswap-direct ip=%llx reg=%lu size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                size,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_bswap_reg(void);
        gen(state, (unsigned long) gadget_amd64_bswap_reg);
        gen(state, reg | ((unsigned long) size << 4));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            (insn.opcode == 0x98 || insn.opcode == 0x99)) {
        unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("sign-extend-direct ip=%llx opcode=%02x size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                size,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_sign_extend(void);
        gen(state, (unsigned long) gadget_amd64_sign_extend);
        gen(state, (unsigned long) insn.opcode | ((unsigned long) size << 8));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            (insn.opcode == 0x69 || insn.opcode == 0x6b)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        guest_addr_t imul_imm_ip = next_ip;
        next_ip += insn.opcode == 0x69
            ? (insn.operand_size_prefix ? sizeof(int16_t) : sizeof(int32_t))
            : sizeof(int8_t);
#if defined(__aarch64__)
        // imul reg, rm, imm (reg form): native multiply with overflow flags.
        if (!insn.fs_prefix && !insn.lock_prefix &&
                amd64_modrm_mod(insn.modrm) == 3) {
            unsigned size = insn.operand_size_prefix ? 16 : (insn.rex.w ? 64 : 32);
            unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
            unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            if ((size == 32 || size == 64) &&
                    amd64_jit_low8_reg(reg_id) && amd64_jit_low8_reg(rm_id)) {
                unsigned long imm_val;
                if (insn.opcode == 0x6b) {
                    int8_t i;
                    if (!tlb_read(tlb, imul_imm_ip, &i, sizeof(i))) {
                        state->amd64_ip = state->amd64_orig_ip;
                        state->amd64_fallback_to_interp = true;
                        return false;
                    }
                    imm_val = (unsigned long) (qword_t) (sqword_t) i;
                } else {
                    int32_t i;
                    if (!tlb_read(tlb, imul_imm_ip, &i, sizeof(i))) {
                        state->amd64_ip = state->amd64_orig_ip;
                        state->amd64_fallback_to_interp = true;
                        return false;
                    }
                    imm_val = (unsigned long) (qword_t) (sqword_t) i;
                }
                unsigned long packed = ((unsigned long) reg_id << 8) |
                    ((unsigned long) rm_id << 12) |
                    ((unsigned long) size << 16);
                state->amd64_ip = next_ip;
                amd64_jit_debug("imul-imm-direct ip=%llx dst=%u src=%u size=%u imm=%llx next=%llx",
                        (unsigned long long) insn.start_ip, reg_id, rm_id, size,
                        (unsigned long long) imm_val, (unsigned long long) next_ip);
                extern void gadget_amd64_imul_imm(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_imul_imm);
                gen(state, packed);
                gen(state, imm_val);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
        }
#endif
        state->amd64_ip = next_ip;
        amd64_jit_debug("imul-imm-helper ip=%llx opcode=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_imul_imm,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.opcode >= 0xa0 && insn.opcode <= 0xa3) {
        next_ip = state->amd64_ip +
            (insn.address_size_prefix ? sizeof(uint32_t) : sizeof(uint64_t));
        state->amd64_ip = next_ip;
        amd64_jit_debug("moffs-helper ip=%llx opcode=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_moffs_accum,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.fs_prefix &&
            !insn.lock_prefix &&
            ((insn.opcode >= 0xa4 && insn.opcode <= 0xa7) ||
             (insn.opcode >= 0xaa && insn.opcode <= 0xaf))) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("string-helper ip=%llx opcode=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_string_op,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            (insn.opcode == 0x86 || insn.opcode == 0x87)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (!insn.lock_prefix && amd64_modrm_mod(insn.modrm) == 3) {
            unsigned reg_raw = amd64_modrm_reg(insn.modrm);
            unsigned rm_raw = amd64_modrm_rm(insn.modrm);
            unsigned reg_id = reg_raw | (insn.rex.r ? 8 : 0);
            unsigned rm_id = rm_raw | (insn.rex.b ? 8 : 0);
            unsigned size = insn.opcode == 0x86
                ? 8
                : (insn.operand_size_prefix ? 16 : (insn.rex.w ? 64 : 32));
            if (insn.opcode == 0x86 && !insn.rex.present &&
                    (reg_raw >= 4 || rm_raw >= 4))
                goto amd64_bridge_step;
            state->amd64_ip = next_ip;
            amd64_jit_debug("xchg-reg-reg-direct ip=%llx opcode=%02x reg=%u rm=%u size=%u next=%llx",
                    (unsigned long long) insn.start_ip,
                    insn.opcode,
                    reg_id,
                    rm_id,
                    size,
                    (unsigned long long) next_ip);
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_xchg_reg_reg(void);
            gen(state, (unsigned long) gadget_amd64_xchg_reg_reg);
            gen(state, rm_id | (reg_id << 4) | ((unsigned long) size << 8));
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("xchg-rm-helper ip=%llx opcode=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_xchg_rm,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.lock_prefix && insn.rep_mode == amd64_jit_rep_none &&
            (insn.opcode == 0x04 || insn.opcode == 0x05 ||
             insn.opcode == 0x0c || insn.opcode == 0x0d ||
             insn.opcode == 0x14 || insn.opcode == 0x15 ||
             insn.opcode == 0x1c || insn.opcode == 0x1d ||
             insn.opcode == 0x24 || insn.opcode == 0x25 ||
             insn.opcode == 0x2c || insn.opcode == 0x2d ||
             insn.opcode == 0x34 || insn.opcode == 0x35 ||
             insn.opcode == 0x3c || insn.opcode == 0x3d ||
             insn.opcode == 0xa8 || insn.opcode == 0xa9)) {
        bool imm8 = (insn.opcode & 1) == 0;
        bool is_test = insn.opcode == 0xa8 || insn.opcode == 0xa9;
        unsigned size = imm8 ? 8 : (insn.operand_size_prefix ? 16 : (insn.rex.w ? 64 : 32));
        // x86 ALU op from the opcode (0=add,1=or,2=adc,3=sbb,4=and,5=sub,6=xor,7=cmp)
        unsigned op = is_test ? 0 : ((insn.opcode >> 3) & 7);
        guest_addr_t imm_ip = state->amd64_ip;
        unsigned long value;
        if (imm8) {
            int8_t i;
            if (!tlb_read(tlb, imm_ip, &i, sizeof(i))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) (qword_t) (sqword_t) i;
            next_ip = imm_ip + sizeof(i);
        } else if (size == 16) {
            uint16_t i;
            if (!tlb_read(tlb, imm_ip, &i, sizeof(i))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) i;
            next_ip = imm_ip + sizeof(i);
        } else {
            int32_t i;
            if (!tlb_read(tlb, imm_ip, &i, sizeof(i))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) (qword_t) (sqword_t) i;
            next_ip = imm_ip + sizeof(i);
        }
#if defined(__aarch64__)
        // Route 32/64-bit add/sub/cmp and and/or/xor (on the accumulator, reg 0)
        // to the same validated cached reg-imm gadgets the modrm 80/81/83 path
        // uses, eliminating the amd64_jit_accum_imm_op bridge + its re-decode.
        // adc/sbb (carry-in), 8/16-bit, and test keep bridging.
        if ((size == 32 || size == 64) && !is_test) {
            bool route_arith = (op == 0 || op == 5 || op == 7);
            bool route_logic = (op == 1 || op == 4 || op == 6);
            if (route_arith || route_logic) {
                unsigned long packed = (unsigned long) insn.opcode |
                    ((unsigned long) op << 8) |
                    (0ul << 12) |
                    ((unsigned long) size << 16);
                if (insn.rex.present)
                    packed |= 1ul << 24;
                state->amd64_ip = next_ip;
                amd64_jit_debug("accum-imm-direct ip=%llx opcode=%02x op=%u size=%u value=%llx next=%llx",
                        (unsigned long long) insn.start_ip, insn.opcode, op, size,
                        (unsigned long long) value, (unsigned long long) next_ip);
                extern void gadget_amd64_cached_arith_reg_imm(void);
                extern void gadget_amd64_cached_logic_reg_imm(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) (route_logic
                            ? (void (*)(void)) gadget_amd64_cached_logic_reg_imm
                            : (void (*)(void)) gadget_amd64_cached_arith_reg_imm));
                gen(state, packed);
                gen(state, value);
                if (op != 7) // cmp does not write back
                    gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
        }
#endif
        state->amd64_ip = next_ip;
        amd64_jit_debug("accum-imm-helper ip=%llx opcode=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_accum_imm_op,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.operand_size_prefix && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.two_byte_opcode &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) == 3 &&
            (insn.op2 == 0xb6 || insn.op2 == 0xb7 ||
             insn.op2 == 0xbe || insn.op2 == 0xbf) &&
            !((insn.op2 == 0xb6 || insn.op2 == 0xbe) &&
              !insn.rex.present && amd64_modrm_rm(insn.modrm) >= 4)) {
        unsigned src_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        unsigned dst_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned dst_size = insn.rex.w ? 64 : 32;
        next_ip = state->amd64_ip + 1;
        state->amd64_ip = next_ip;
        amd64_jit_debug("movx-reg-reg-direct ip=%llx op2=%02x src=%u dst=%u size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                insn.op2,
                src_id,
                dst_id,
                dst_size,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_movx_reg_reg(void);
        gen(state, (unsigned long) gadget_amd64_movx_reg_reg);
        gen(state, (unsigned long) insn.op2 |
                ((unsigned long) src_id << 8) |
                ((unsigned long) dst_id << 12) |
                ((unsigned long) dst_size << 16));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.operand_size_prefix && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.two_byte_opcode &&
            (insn.op2 == 0xb6 || insn.op2 == 0xb7 ||
             insn.op2 == 0xbe || insn.op2 == 0xbf)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("movx-helper ip=%llx op2=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.op2,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_movx,
                (unsigned long) insn.op2, (unsigned long) next_ip);
        return true;
    }

    if (!insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.two_byte_opcode &&
            insn.has_modrm &&
            insn.op2 == 0x18 &&
            (amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0)) <= 3) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("prefetch-nop-direct ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.two_byte_opcode &&
            insn.has_modrm &&
            insn.op2 == 0x1f &&
            amd64_modrm_reg(insn.modrm) == 0) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("nop-direct ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // imul reg, rm (0F AF) is bridged: a native gadget_amd64_imul_reg exists but
    // its low-reg form corrupts state in a specific -O1 interleaving (a low-reg
    // imul_reg followed by a hot-cache hash multiply) that isolated tests don't
    // hit; left bridged pending root-cause. The 69/6B imm form is native + proven.
    if (!insn.address_size_prefix &&
            (insn.rep_mode == amd64_jit_rep_none ||
             ((insn.op2 == 0xbc || insn.op2 == 0xbd) &&
              insn.rep_mode == amd64_jit_repz)) &&
            insn.two_byte_opcode &&
            insn.has_modrm &&
            (insn.op2 == 0x1f ||
             (insn.op2 >= 0x40 && insn.op2 <= 0x4f) ||
             (insn.op2 >= 0x90 && insn.op2 <= 0x9f) ||
             insn.op2 == 0xa3 ||
             insn.op2 == 0xa4 ||
             insn.op2 == 0xa5 ||
             insn.op2 == 0xab ||
             insn.op2 == 0xac ||
             insn.op2 == 0xad ||
             insn.op2 == 0xae ||
             insn.op2 == 0xaf ||
             insn.op2 == 0xba ||
             insn.op2 == 0xb3 ||
             insn.op2 == 0xb0 ||
             insn.op2 == 0xb1 ||
             insn.op2 == 0xbc ||
             insn.op2 == 0xbd ||
             insn.op2 == 0xbb ||
             insn.op2 == 0xc0 ||
             insn.op2 == 0xc1)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (insn.op2 == 0xa4 || insn.op2 == 0xac || insn.op2 == 0xba)
            next_ip += sizeof(uint8_t);
        state->amd64_ip = next_ip;
        amd64_jit_debug("0f-rm-helper ip=%llx op2=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.op2,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_0f_rm,
                (unsigned long) insn.op2, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

#if defined(__aarch64__)
    // Native aarch64 SSE register-register gadgets. These intercept the hottest
    // reg-reg vector ops that would otherwise compile into a per-op C bridge
    // (amd64_jit_0f_vec_rm). Only the exact reg-reg cases matched here are taken
    // natively; every other form (memory operand, MMX no-prefix, other prefixes)
    // falls through to the bridge below. The gadgets touch only cpu->xmm[], so
    // the GPR reg cache is left intact (no flush) and the rip is deferred.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            (insn.op2 == 0x6f || insn.op2 == 0xd4 || insn.op2 == 0xfb ||
             insn.op2 == 0xef)) {
        // 66 0F /r, mod==3 register-register SSE2 ops (dst=reg, src=rm):
        //   6F movdqa (128-bit copy), D4 paddq, FB psubq (packed 64-bit add/sub),
        //   EF pxor (128-bit XOR — same gadget as xorps, lane width irrelevant)
        extern void gadget_amd64_v_mov128_reg(void);
        extern void gadget_amd64_v_paddq_reg(void);
        extern void gadget_amd64_v_psubq_reg(void);
        extern void gadget_amd64_v_pxor_reg(void);
        void (*gadget)(void) = NULL;
        switch (insn.op2) {
        case 0x6f: gadget = gadget_amd64_v_mov128_reg; break;
        case 0xd4: gadget = gadget_amd64_v_paddq_reg; break;
        case 0xfb: gadget = gadget_amd64_v_psubq_reg; break;
        case 0xef: gadget = gadget_amd64_v_pxor_reg; break;
        }
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-reg op2=%02x ip=%llx src=%u dst=%u next=%llx",
                insn.op2, (unsigned long long) insn.start_ip, rm_id, reg_id,
                (unsigned long long) next_ip);
        gen(state, (unsigned long) gadget);
        gen(state, (unsigned long) (rm_id | (reg_id << 4)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 0F 57 xorps / 66 0F 57 xorpd, mod==3: a 128-bit bitwise XOR either way, so
    // no operand-size-prefix gating (unlike the SSE2 integer ops above). A rep
    // prefix is #UD — left to the bridge.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.op2 == 0x57) {
        extern void gadget_amd64_v_pxor_reg(void);
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-xor op2=57 ip=%llx src=%u dst=%u next=%llx",
                (unsigned long long) insn.start_ip, rm_id, reg_id,
                (unsigned long long) next_ip);
        gen(state, (unsigned long) gadget_amd64_v_pxor_reg);
        gen(state, (unsigned long) (rm_id | (reg_id << 4)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // F2 0F 58 addsd | 59 mulsd, mod==3: scalar-double low-lane arithmetic
    // (dst.f64[0] OP= src.f64[0], high 64 bits preserved). F2 (repnz) only; the
    // packed/single (66/F3/none) variants keep bridging.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            !insn.operand_size_prefix && insn.rep_mode == amd64_jit_repnz &&
            (insn.op2 == 0x58 || insn.op2 == 0x59)) {
        extern void gadget_amd64_v_addsd_reg(void);
        extern void gadget_amd64_v_mulsd_reg(void);
        void (*gadget)(void) = insn.op2 == 0x58
            ? (void (*)(void)) gadget_amd64_v_addsd_reg
            : (void (*)(void)) gadget_amd64_v_mulsd_reg;
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-scalard op2=%02x ip=%llx src=%u dst=%u next=%llx",
                insn.op2, (unsigned long long) insn.start_ip, rm_id, reg_id,
                (unsigned long long) next_ip);
        gen(state, (unsigned long) gadget);
        gen(state, (unsigned long) (rm_id | (reg_id << 4)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // F2 0F 2A cvtsi2sd xmm, r/m, mod==3: xmm[reg].f64[0] = (double) signed
    // GPR[rm] (REX.W -> 64-bit source, else 32-bit), high 64 bits preserved. The
    // source is a GPR, so flush the reg cache first to make cpu->amd64_regs
    // current; the gadget then reads it from memory. F2 (cvtsi2sd) only; F3
    // cvtsi2ss keeps bridging.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            !insn.operand_size_prefix && insn.rep_mode == amd64_jit_repnz &&
            insn.op2 == 0x2a) {
        extern void gadget_amd64_v_cvtsi2sd_reg(void);
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-cvtsi2sd ip=%llx gpr=%u xmm=%u w=%d next=%llx",
                (unsigned long long) insn.start_ip, rm_id, reg_id, insn.rex.w,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen(state, (unsigned long) gadget_amd64_v_cvtsi2sd_reg);
        gen(state, (unsigned long) (rm_id | (reg_id << 4) | ((insn.rex.w ? 1ul : 0ul) << 8)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 66 0F 73 /2 ib psrlq | /6 ib psllq, mod==3: shift each 64-bit lane of an
    // xmm by imm8. The /digit is the op extension; the bridge folds REX.R into
    // modrm.reg and #UDs reg>=8, so REX.R cases are left to the bridge to match.
    // Only the qword shifts (/2,/6) go native; the byte shifts /3,/7 bridge.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.rex.r &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            insn.op2 == 0x73 &&
            (amd64_modrm_reg(insn.modrm) == 2 || amd64_modrm_reg(insn.modrm) == 6)) {
        extern void gadget_amd64_v_psrlq_imm(void);
        extern void gadget_amd64_v_psllq_imm(void);
        unsigned ext = amd64_modrm_reg(insn.modrm);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        uint8_t imm8;
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (!tlb_read(tlb, next_ip, &imm8, sizeof(imm8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip += sizeof(uint8_t);
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-shiftq op2=73 /%u ip=%llx rm=%u imm=%u next=%llx",
                ext, (unsigned long long) insn.start_ip, rm_id, imm8,
                (unsigned long long) next_ip);
        gen(state, (unsigned long) (ext == 2
                    ? (void (*)(void)) gadget_amd64_v_psrlq_imm
                    : (void (*)(void)) gadget_amd64_v_psllq_imm));
        gen(state, (unsigned long) (rm_id | ((unsigned long) imm8 << 8)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 0F 28 movaps / 0F 10 movups (+66 movapd/movupd), reg<-mem, mod!=3: native
    // 128-bit xmm load through a 64-bit TLB fast path. The interp does not enforce
    // movaps alignment (reads like movups), so one gadget serves both. F2/F3
    // (movsd/movss scalar) and fs/address-size forms keep bridging. The reg cache
    // and rip are flushed so a page fault re-executes this instruction correctly.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.rep_mode == amd64_jit_rep_none &&
            (insn.op2 == 0x28 || insn.op2 == 0x10)) {
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 128, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-load128-mem ip=%llx op2=%02x meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.op2, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_v_load128_mem(void);
        gen(state, (unsigned long) gadget_amd64_v_load128_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 0F 29 movaps / 0F 11 movups (+66 movapd/movupd), reg->mem, mod!=3: native
    // 128-bit xmm store through the 64-bit TLB write path (staleness + page_if_
    // writable + cross-page staging). Same caveats as the load: alignment not
    // enforced, F2/F3 scalar + fs/address-size forms bridge, cache+rip flushed.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.rep_mode == amd64_jit_rep_none &&
            (insn.op2 == 0x29 || insn.op2 == 0x11)) {
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 128, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-store128-mem ip=%llx op2=%02x meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.op2, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_v_store128_mem(void);
        gen(state, (unsigned long) gadget_amd64_v_store128_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // movdqa (66 0F 6F load / 7F store) and movdqu (F3 0F 6F / 7F), reg<->mem,
    // mod!=3: a 128-bit move identical to movaps/movups, so it reuses the same
    // load128/store128 gadgets. NO-prefix 0F 6F/7F is MMX movq (64-bit mm regs)
    // and is excluded; F2 is not a valid movdq prefix. (Go's memmove path.)
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.rep_mode != amd64_jit_repnz &&
            (insn.operand_size_prefix || insn.rep_mode == amd64_jit_repz) &&
            (insn.op2 == 0x6f || insn.op2 == 0x7f)) {
        bool is_store = insn.op2 == 0x7f;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 128, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-movdq-mem ip=%llx op2=%02x store=%d meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.op2, is_store, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_v_load128_mem(void);
        extern void gadget_amd64_v_store128_mem(void);
        gen(state, (unsigned long) (is_store
                    ? (void (*)(void)) gadget_amd64_v_store128_mem
                    : (void (*)(void)) gadget_amd64_v_load128_mem));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }
#endif

    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            (insn.op2 == 0x10 ||
             insn.op2 == 0x2a ||
             insn.op2 == 0x2c ||
             insn.op2 == 0x2e ||
             insn.op2 == 0x2f ||
             insn.op2 == 0x11 ||
             insn.op2 == 0x12 ||
             insn.op2 == 0x13 ||
             insn.op2 == 0x14 ||
             insn.op2 == 0x15 ||
             insn.op2 == 0x16 ||
             insn.op2 == 0x17 ||
             insn.op2 == 0x28 ||
             insn.op2 == 0x29 ||
             insn.op2 == 0x50 ||
             insn.op2 == 0x54 ||
             insn.op2 == 0x55 ||
             insn.op2 == 0x56 ||
             insn.op2 == 0x57 ||
             insn.op2 == 0x58 ||
             insn.op2 == 0x59 ||
             insn.op2 == 0x5a ||
             insn.op2 == 0x5c ||
             insn.op2 == 0x5d ||
             insn.op2 == 0x5e ||
             insn.op2 == 0x5f ||
             insn.op2 == 0x60 ||
             insn.op2 == 0x61 ||
             insn.op2 == 0x62 ||
             insn.op2 == 0x63 ||
             insn.op2 == 0x64 ||
             insn.op2 == 0x65 ||
             insn.op2 == 0x66 ||
             insn.op2 == 0x67 ||
             insn.op2 == 0x68 ||
             insn.op2 == 0x69 ||
             insn.op2 == 0x6a ||
             insn.op2 == 0x6b ||
             insn.op2 == 0x6c ||
             insn.op2 == 0x6d ||
             insn.op2 == 0x6e ||
             insn.op2 == 0x6f ||
             insn.op2 == 0x70 ||
             ((insn.op2 == 0x71 || insn.op2 == 0x72 || insn.op2 == 0x73) &&
              insn.operand_size_prefix) ||
             insn.op2 == 0x74 ||
             insn.op2 == 0x75 ||
             insn.op2 == 0x76 ||
             insn.op2 == 0x7e ||
             insn.op2 == 0x7f ||
             insn.op2 == 0xc2 ||
             insn.op2 == 0xc4 ||
             insn.op2 == 0xc5 ||
             insn.op2 == 0xc6 ||
             insn.op2 == 0xd1 ||
             insn.op2 == 0xd2 ||
             insn.op2 == 0xd3 ||
             insn.op2 == 0xd4 ||
             insn.op2 == 0xd5 ||
             insn.op2 == 0xd6 ||
             insn.op2 == 0xd7 ||
             insn.op2 == 0xd8 ||
             insn.op2 == 0xd9 ||
             insn.op2 == 0xda ||
             insn.op2 == 0xdb ||
             insn.op2 == 0xdc ||
             insn.op2 == 0xdd ||
             insn.op2 == 0xde ||
             insn.op2 == 0xdf ||
             insn.op2 == 0xe0 ||
             insn.op2 == 0xe1 ||
             insn.op2 == 0xe2 ||
             insn.op2 == 0xe3 ||
             insn.op2 == 0xe4 ||
             insn.op2 == 0xe5 ||
             insn.op2 == 0xe7 ||
             insn.op2 == 0xe8 ||
             insn.op2 == 0xe9 ||
             insn.op2 == 0xea ||
             insn.op2 == 0xeb ||
             insn.op2 == 0xec ||
             insn.op2 == 0xed ||
             insn.op2 == 0xee ||
             insn.op2 == 0xf1 ||
             insn.op2 == 0xf2 ||
             insn.op2 == 0xf3 ||
             insn.op2 == 0xf4 ||
             insn.op2 == 0xf6 ||
             insn.op2 == 0xf8 ||
             insn.op2 == 0xf9 ||
             insn.op2 == 0xfa ||
             insn.op2 == 0xfb ||
             insn.op2 == 0xfc ||
             insn.op2 == 0xfd ||
             insn.op2 == 0xfe ||
             insn.op2 == 0xef)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (insn.op2 == 0x70 || insn.op2 == 0x71 || insn.op2 == 0x72 ||
                insn.op2 == 0x73 || insn.op2 == 0xc2 || insn.op2 == 0xc4 ||
                insn.op2 == 0xc5 || insn.op2 == 0xc6)
            next_ip += sizeof(uint8_t);
        state->amd64_ip = next_ip;
        amd64_jit_debug("0f-vec-rm-helper ip=%llx op2=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.op2,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_0f_vec_rm,
                (unsigned long) insn.op2, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            (insn.opcode == 0xf6 || insn.opcode == 0xf7) &&
            amd64_modrm_reg(insn.modrm) == 0) {
        size_t imm_size;
        unsigned size = insn.opcode == 0xf6
            ? 8
            : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        imm_size = insn.opcode == 0xf6
            ? sizeof(uint8_t)
            : (insn.operand_size_prefix ? sizeof(uint16_t) : sizeof(uint32_t));
        next_ip += imm_size;
        if (!insn.fs_prefix && !insn.lock_prefix &&
                amd64_modrm_mod(insn.modrm) == 3) {
            unsigned rm_raw = amd64_modrm_rm(insn.modrm);
            unsigned rm_id = rm_raw | (insn.rex.b ? 8 : 0);
            unsigned long value;
            guest_addr_t imm_ip = state->amd64_ip + 1;
            if (size == 8) {
                uint8_t imm8;
                if (!tlb_read(tlb, imm_ip, &imm8, sizeof(imm8))) {
                    state->amd64_ip = state->amd64_orig_ip;
                    state->amd64_fallback_to_interp = true;
                    return false;
                }
                value = imm8;
            } else if (size == 16) {
                uint16_t imm16;
                if (!tlb_read(tlb, imm_ip, &imm16, sizeof(imm16))) {
                    state->amd64_ip = state->amd64_orig_ip;
                    state->amd64_fallback_to_interp = true;
                    return false;
                }
                value = imm16;
            } else {
                int32_t imm32;
                if (!tlb_read(tlb, imm_ip, &imm32, sizeof(imm32))) {
                    state->amd64_ip = state->amd64_orig_ip;
                    state->amd64_fallback_to_interp = true;
                    return false;
                }
                value = insn.rex.w
                    ? (unsigned long) (qword_t) (sqword_t) imm32
                    : (unsigned long) (uint32_t) imm32;
            }
            if (!(size == 8 && !insn.rex.present && rm_raw >= 4)) {
                state->amd64_ip = next_ip;
                amd64_jit_debug("grp3-test-reg-imm-direct ip=%llx rm=%u size=%u value=%llx next=%llx",
                        (unsigned long long) insn.start_ip,
                        rm_id,
                        size,
                        (unsigned long long) value,
                        (unsigned long long) next_ip);
#if defined(__aarch64__)
                if (amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_cached_test_reg_imm(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_cached_test_reg_imm);
                    gen(state, rm_id | ((unsigned long) size << 4));
                    gen(state, value);
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
#endif
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_test_reg_imm(void);
                gen(state, (unsigned long) gadget_amd64_test_reg_imm);
                gen(state, rm_id | ((unsigned long) size << 4));
                gen(state, value);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("grp3-test-helper ip=%llx opcode=%02x modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_grp3_test,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    // The grp3 helper does not parse a LOCK prefix; route lock not/neg (legal
    // on memory operands) to the interpreter.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            (insn.opcode == 0xf6 || insn.opcode == 0xf7) &&
            amd64_modrm_reg(insn.modrm) >= 2) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("grp3-op-helper ip=%llx opcode=%02x modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_grp3_op,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            insn.opcode == 0xfe &&
            amd64_modrm_reg(insn.modrm) <= 1) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("fe-group-helper ip=%llx modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_fe_group,
                (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            insn.opcode == 0xff) {
        unsigned group = amd64_modrm_reg(insn.modrm);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        // Native memory-indirect call (/2) and jmp (/4): mod!=3, target is the
        // 8-byte value read from the effective address -- PLT stubs
        // `jmp/call *off(%rip)` and vtable/function-pointer dispatch, the common
        // real-code case. gen_amd64_decode_mem_meta needs amd64_ip still at the
        // opcode, so this runs before the advance below. 64-bit only (no 0x66);
        // FS-prefix and address-size forms keep bridging (the amd64_vmem_addr gadget
        // handles neither). rip is flushed so a #PF on the load/push re-executes.
        if ((group == 2 || group == 4) && amd64_modrm_mod(insn.modrm) != 3 &&
                !insn.lock_prefix && !insn.operand_size_prefix && !insn.fs_prefix) {
            unsigned long meta, disp;
            if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 64, &meta, &disp, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            state->amd64_ip = next_ip;
            gen_amd64_flush_reg_cache(state);
            gen_amd64_flush_rip(state);
            if (group == 4) {
                amd64_jit_debug("jmp-indir-mem ip=%llx meta=%lx disp=%lx next=%llx",
                        (unsigned long long) insn.start_ip, meta, disp,
                        (unsigned long long) next_ip);
                extern void gadget_amd64_jmp_indir_mem(void);
                gen(state, (unsigned long) gadget_amd64_jmp_indir_mem);
            } else {
                amd64_jit_debug("call-indir-mem ip=%llx meta=%lx disp=%lx next=%llx",
                        (unsigned long long) insn.start_ip, meta, disp,
                        (unsigned long long) next_ip);
                extern void gadget_amd64_call_indir_mem(void);
                gen(state, (unsigned long) gadget_amd64_call_indir_mem);
            }
            gen(state, meta);
            gen(state, disp);
            gen(state, (unsigned long) next_ip);
            return false;
        }
        state->amd64_ip = next_ip;
        // Native register-indirect call (/2) and jmp (/4): mod==3, target =
        // regs[rm]. The memory forms and the other groups (inc/dec/push/far)
        // keep bridging. Always 64-bit in long mode (require no 0x66).
        if ((group == 2 || group == 4) && amd64_modrm_mod(insn.modrm) == 3 &&
                !insn.lock_prefix && !insn.operand_size_prefix) {
            unsigned long rm = amd64_modrm_rm(insn.modrm);
            if (insn.rex.b)
                rm |= 8;
            gen_amd64_flush_reg_cache(state);
            if (group == 4) {
                amd64_jit_debug("jmp-indir-reg ip=%llx rm=%lu",
                        (unsigned long long) insn.start_ip, rm);
                state->amd64_deferred_rip_valid = false;
                extern void gadget_amd64_jmp_indir_reg(void);
                gen(state, (unsigned long) gadget_amd64_jmp_indir_reg);
                gen(state, rm);
            } else {
                amd64_jit_debug("call-indir-reg ip=%llx rm=%lu next=%llx",
                        (unsigned long long) insn.start_ip, rm,
                        (unsigned long long) next_ip);
                gen_amd64_flush_rip(state);
                extern void gadget_amd64_call_indir_reg(void);
                gen(state, (unsigned long) gadget_amd64_call_indir_reg);
                gen(state, rm);
                gen(state, (unsigned long) next_ip);
            }
            return false;
        }
        amd64_jit_debug("ff-group-helper ip=%llx modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_ff_group,
                (unsigned long) next_ip);
        if (group <= 1)
            return true;
        gen_exit(state);
        return false;
    }

    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            insn.opcode >= 0xb0 && insn.opcode <= 0xb7) {
        uint8_t imm8;
        unsigned long reg_size;
        reg = (unsigned long) (insn.opcode - 0xb0);
        if (insn.rex.b)
            reg |= 8;
        if (!tlb_read(tlb, state->amd64_ip, &imm8, sizeof(imm8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(imm8);
        state->amd64_ip = next_ip;
        reg_size = reg | (8ul << 8);
        if (insn.rex.present)
            reg_size |= 1ul << 16;
        if (insn.rex.present || insn.opcode <= 0xb3) {
            amd64_jit_debug("mov-imm8-reg-direct ip=%llx reg=%lu value=%x next=%llx",
                    (unsigned long long) insn.start_ip,
                    reg,
                    imm8,
                    (unsigned long long) next_ip);
#if defined(__aarch64__)
            if (amd64_jit_low8_reg((unsigned) reg)) {
                extern void gadget_amd64_cached_mov_imm_reg(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_mov_imm_reg);
                gen(state, reg | (8ul << 4));
                gen(state, (unsigned long) imm8);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_mov_imm8_reg(void);
            gen(state, (unsigned long) gadget_amd64_mov_imm8_reg);
            gen(state, reg);
            gen(state, (unsigned long) imm8);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        amd64_jit_debug("mov-imm-helper ip=%llx reg=%lu size=8 value=%x next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                imm8,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_3_retint(state, amd64_jit_mov_imm,
                reg_size, (unsigned long) imm8, (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.opcode >= 0xb8 && insn.opcode <= 0xbf) {
        uint64_t value;
        uint32_t imm32;
        uint16_t imm16;
        unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
        reg = (unsigned long) (insn.opcode - 0xb8);
        if (insn.rex.b)
            reg |= 8;
        if (size == 64) {
            if (!tlb_read(tlb, state->amd64_ip, &value, sizeof(value))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            next_ip = state->amd64_ip + sizeof(value);
        } else if (size == 16) {
            if (!tlb_read(tlb, state->amd64_ip, &imm16, sizeof(imm16))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = imm16;
            next_ip = state->amd64_ip + sizeof(imm16);
        } else {
            if (!tlb_read(tlb, state->amd64_ip, &imm32, sizeof(imm32))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = imm32;
            next_ip = state->amd64_ip + sizeof(imm32);
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("mov-imm-reg-direct ip=%llx reg=%lu size=%u value=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                size,
                (unsigned long long) value,
                (unsigned long long) next_ip);
#if defined(__aarch64__)
        if (amd64_jit_low8_reg((unsigned) reg)) {
            extern void gadget_amd64_cached_mov_imm_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_cached_mov_imm_reg);
            gen(state, reg | ((unsigned long) size << 4));
            gen(state, (unsigned long) value);
            gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
#endif
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_mov_imm_reg(void);
        gen(state, (unsigned long) gadget_amd64_mov_imm_reg);
        gen(state, reg | ((unsigned long) size << 4));
        gen(state, (unsigned long) value);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (amd64_jit_push_pop_prefixes_ok(&insn) &&
            insn.opcode >= 0x50 && insn.opcode <= 0x57) {
        reg = (unsigned long) (insn.opcode - 0x50);
        if (insn.rex.b)
            reg |= 8;
        next_ip = insn.end_ip;
        amd64_jit_debug("push-reg ip=%llx reg=%lu next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                (unsigned long long) next_ip);
        state->amd64_ip = next_ip;
        // Native 64-bit stack push. Flush the reg cache + rip so the gadget reads
        // guest registers from CPU_amd64_regs and a #PF re-executes this insn.
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_push_reg(void);
        gen(state, (unsigned long) gadget_amd64_push_reg);
        gen(state, reg);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (amd64_jit_push_pop_prefixes_ok(&insn) &&
            insn.opcode >= 0x58 && insn.opcode <= 0x5f) {
        reg = (unsigned long) (insn.opcode - 0x58);
        if (insn.rex.b)
            reg |= 8;
        next_ip = insn.end_ip;
        amd64_jit_debug("pop-reg ip=%llx reg=%lu next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                (unsigned long long) next_ip);
        state->amd64_ip = next_ip;
        // Native 64-bit stack pop. Same reg-cache/rip flush contract as push.
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_pop_reg(void);
        gen(state, (unsigned long) gadget_amd64_pop_reg);
        gen(state, reg);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            insn.opcode == 0x8f) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("pop-rm-helper ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_pop_rm,
                (unsigned long) next_ip);
        return true;
    }

    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            (insn.opcode == 0x68 || insn.opcode == 0x6a)) {
        unsigned long value;
        if (insn.opcode == 0x68) {
            int32_t imm32;
            if (!tlb_read(tlb, state->amd64_ip, &imm32, sizeof(imm32))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) (qword_t) (sqword_t) imm32;
            next_ip = state->amd64_ip + sizeof(imm32);
        } else {
            int8_t imm8;
            if (!tlb_read(tlb, state->amd64_ip, &imm8, sizeof(imm8))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) (qword_t) (sqword_t) imm8;
            next_ip = state->amd64_ip + sizeof(imm8);
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("push-imm ip=%llx value=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) value,
                (unsigned long long) next_ip);
        // Native push of a sign-extended 64-bit immediate (value computed above).
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_push_imm(void);
        gen(state, (unsigned long) gadget_amd64_push_imm);
        gen(state, value);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            insn.opcode == 0x9c) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("pushf-helper ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_push_flags,
                64, (unsigned long) next_ip);
        return true;
    }

    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            insn.opcode == 0x9d) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("popf-helper ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_pop_flags,
                64, (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.operand_size_prefix &&
            !insn.address_size_prefix && !insn.fs_prefix &&
            !insn.lock_prefix && !insn.rex.present &&
            insn.rep_mode == amd64_jit_repz && insn.opcode == 0x90) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("pause-direct ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.rex.present &&
            insn.rep_mode == amd64_jit_rep_none && insn.opcode == 0x90) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("nop90-direct ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.opcode >= 0x90 && insn.opcode <= 0x97) {
        unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
        reg = (unsigned long) (insn.opcode - 0x90);
        if (insn.rex.b)
            reg |= 8;
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("xchg-rax-direct ip=%llx reg=%lu size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                size,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_xchg_rax_reg(void);
        gen(state, (unsigned long) gadget_amd64_xchg_rax_reg);
        gen(state, reg | ((unsigned long) size << 4));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) != 3 &&
            insn.opcode == 0x8d) {
        unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
        unsigned long meta;
        unsigned long disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp,
                &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("lea-reg-mem-direct ip=%llx meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip,
                meta,
                disp,
                (unsigned long long) next_ip);
#if defined(__aarch64__)
        extern void gadget_amd64_cached_lea_reg_mem(void);
        gen_amd64_ensure_reg_cache(state);
        gen(state, (unsigned long) gadget_amd64_cached_lea_reg_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        if (amd64_jit_low8_reg((unsigned) ((meta >> AMD64_JIT_MEM_REG_SHIFT) & 0xf)))
            gen_amd64_mark_reg_cache_dirty(state);
        gen_amd64_defer_rip(state, next_ip);
        return true;
#else
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_lea_reg_mem(void);
        gen(state, (unsigned long) gadget_amd64_lea_reg_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
#endif
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0xd0 || insn.opcode == 0xd1 ||
             insn.opcode == 0xd2 || insn.opcode == 0xd3)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("shift-rm-helper ip=%llx opcode=%02x modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_shift,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) == 3 &&
            (insn.opcode == 0xd0 || insn.opcode == 0xd1 ||
             insn.opcode == 0xd2 || insn.opcode == 0xd3)) {
        unsigned size = (insn.opcode == 0xd0 || insn.opcode == 0xd2)
            ? 8
            : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
        unsigned group = amd64_modrm_reg(insn.modrm);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        unsigned long count = (insn.opcode == 0xd0 || insn.opcode == 0xd1) ? 1ul : (1ul << 63);
        unsigned long packed = (unsigned long) insn.opcode |
            ((unsigned long) group << 8) |
            ((unsigned long) rm_id << 12) |
            ((unsigned long) size << 16);
        if (insn.rex.present)
            packed |= 1ul << 24;
        (void) count;
        (void) packed;
        next_ip = state->amd64_ip + 1;
        state->amd64_ip = next_ip;
        if ((group == 4 || group == 5 || group == 7) &&
                (size == 32 || size == 64)) {
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_shift_reg_count(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_shift_reg_count);
                gen(state, packed);
                gen(state, count);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
        }
        amd64_jit_debug("shift-helper ip=%llx opcode=%02x group=%u rm=%u size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                group,
                rm_id,
                size,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_shift,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) == 3 &&
            (insn.opcode == 0x88 || insn.opcode == 0x8a)) {
        unsigned reg_raw = amd64_modrm_reg(insn.modrm);
        unsigned rm_raw = amd64_modrm_rm(insn.modrm);
        unsigned reg_id = reg_raw | (insn.rex.r ? 8 : 0);
        unsigned rm_id = rm_raw | (insn.rex.b ? 8 : 0);
        unsigned src_id = insn.opcode == 0x88 ? reg_id : rm_id;
        unsigned dst_id = insn.opcode == 0x88 ? rm_id : reg_id;
        unsigned long packed = src_id | (dst_id << 4);
        if (!insn.rex.present && (reg_raw >= 4 || rm_raw >= 4))
            goto amd64_bridge_step;
        next_ip = state->amd64_ip + 1;
        state->amd64_ip = next_ip;
        amd64_jit_debug("mov8-reg-reg-direct ip=%llx src=%u dst=%u next=%llx",
                (unsigned long long) insn.start_ip,
                src_id,
                dst_id,
                (unsigned long long) next_ip);
#if defined(__aarch64__)
        if (amd64_jit_low8_reg(src_id) && amd64_jit_low8_reg(dst_id)) {
            extern void gadget_amd64_cached_mov_reg_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_cached_mov_reg_reg);
            gen(state, packed | (8ul << 8));
            gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        if (amd64_jit_low8_reg(src_id) || amd64_jit_low8_reg(dst_id)) {
            extern void gadget_amd64_hybrid_mov_reg_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_hybrid_mov_reg_reg);
            gen(state, packed | (8ul << 8));
            if (amd64_jit_low8_reg(dst_id))
                gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
#endif
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_mov8_reg_reg(void);
        gen(state, (unsigned long) gadget_amd64_mov8_reg_reg);
        gen(state, packed);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Only the REX.W form goes direct: the movsxd gadgets implement 64-bit
    // semantics only. 16/32-bit movsxd falls through to the reg-reg helper.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.rex.w &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) == 3 &&
            insn.opcode == 0x63) {
        unsigned size = 64;
        unsigned src_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        unsigned dst_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        next_ip = state->amd64_ip + 1;
        state->amd64_ip = next_ip;
        amd64_jit_debug("movsxd-reg-reg-direct ip=%llx src=%u dst=%u size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                src_id,
                dst_id,
                size,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_movsxd_reg_reg(void);
        gen(state, (unsigned long) gadget_amd64_movsxd_reg_reg);
        gen(state, src_id | (dst_id << 4) | ((unsigned long) size << 8));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) == 3 &&
            (insn.opcode == 0x89 || insn.opcode == 0x8b)) {
        unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        unsigned src_id = insn.opcode == 0x89 ? reg_id : rm_id;
        unsigned dst_id = insn.opcode == 0x89 ? rm_id : reg_id;
        unsigned long packed = src_id | (dst_id << 4);
        next_ip = state->amd64_ip + 1;
        state->amd64_ip = next_ip;
        amd64_jit_debug("mov%u-reg-reg-direct ip=%llx src=%u dst=%u next=%llx",
                size,
                (unsigned long long) insn.start_ip,
                src_id,
                dst_id,
                (unsigned long long) next_ip);
#if defined(__aarch64__)
        if (amd64_jit_low8_reg(src_id) && amd64_jit_low8_reg(dst_id)) {
            extern void gadget_amd64_cached_mov_reg_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_cached_mov_reg_reg);
            gen(state, packed | ((unsigned long) size << 8));
            gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        if (amd64_jit_low8_reg(src_id) || amd64_jit_low8_reg(dst_id)) {
            extern void gadget_amd64_hybrid_mov_reg_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_hybrid_mov_reg_reg);
            gen(state, packed | ((unsigned long) size << 8));
            if (amd64_jit_low8_reg(dst_id))
                gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
#endif
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_mov16_reg_reg(void);
        extern void gadget_amd64_mov32_reg_reg(void);
        extern void gadget_amd64_mov64_reg_reg(void);
        gen(state, size == 64
                ? (unsigned long) gadget_amd64_mov64_reg_reg
                : size == 32
                ? (unsigned long) gadget_amd64_mov32_reg_reg
                : (unsigned long) gadget_amd64_mov16_reg_reg);
        gen(state, packed);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode &&
            !insn.address_size_prefix &&
            !insn.fs_prefix &&
            !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            !insn.rex.r &&
            (insn.opcode == 0x80 || insn.opcode == 0x81 ||
             insn.opcode == 0x83 || insn.opcode == 0xc0 ||
             insn.opcode == 0xc1 || insn.opcode == 0xc6 ||
             insn.opcode == 0xc7)) {
        unsigned size = (insn.opcode == 0x80 || insn.opcode == 0xc0 ||
                insn.opcode == 0xc6)
            ? 8
            : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
        unsigned group = amd64_modrm_reg(insn.modrm);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        unsigned long value;
        unsigned long packed;
        guest_addr_t imm_ip = state->amd64_ip + 1;

        if ((insn.opcode == 0xc6 || insn.opcode == 0xc7) && group != 0)
            goto amd64_bridge_step;

        if (insn.opcode == 0x80 || insn.opcode == 0xc0 ||
                insn.opcode == 0xc1 || insn.opcode == 0xc6) {
            uint8_t imm8;
            if (!tlb_read(tlb, imm_ip, &imm8, sizeof(imm8))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = imm8;
            next_ip = imm_ip + sizeof(imm8);
        } else if (insn.opcode == 0x83) {
            int8_t imm8;
            if (!tlb_read(tlb, imm_ip, &imm8, sizeof(imm8))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) (qword_t) (sqword_t) imm8;
            next_ip = imm_ip + sizeof(imm8);
        } else if (size == 16) {
            uint16_t imm16;
            if (!tlb_read(tlb, imm_ip, &imm16, sizeof(imm16))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) imm16;
            next_ip = imm_ip + sizeof(imm16);
        } else {
            int32_t imm32;
            if (!tlb_read(tlb, imm_ip, &imm32, sizeof(imm32))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            if (insn.opcode == 0xc7 && !insn.rex.w)
                value = (unsigned long) (uint32_t) imm32;
            else
                value = (unsigned long) (qword_t) (sqword_t) imm32;
            next_ip = imm_ip + sizeof(imm32);
        }

        state->amd64_ip = next_ip;
        packed = (unsigned long) insn.opcode |
            ((unsigned long) group << 8) |
            ((unsigned long) rm_id << 12) |
            ((unsigned long) size << 16);
        if (insn.rex.present)
            packed |= 1ul << 24;
        if (insn.opcode == 0xc6) {
            if (!insn.rex.present && amd64_modrm_rm(insn.modrm) >= 4)
                goto amd64_bridge_step;
            state->amd64_ip = next_ip;
            amd64_jit_debug("mov-imm8-rmreg-direct ip=%llx rm=%u value=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    rm_id,
                    (unsigned long long) value,
                    (unsigned long long) next_ip);
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_mov_imm_reg(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_mov_imm_reg);
                gen(state, rm_id | (8ul << 4));
                gen(state, value);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_mov_imm8_reg(void);
            gen(state, (unsigned long) gadget_amd64_mov_imm8_reg);
            gen(state, rm_id);
            gen(state, value);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        if (insn.opcode == 0xc7) {
            state->amd64_ip = next_ip;
            amd64_jit_debug("mov-imm-rmreg-direct ip=%llx rm=%u size=%u value=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    rm_id,
                    size,
                    (unsigned long long) value,
                    (unsigned long long) next_ip);
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_mov_imm_reg(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_mov_imm_reg);
                gen(state, rm_id | ((unsigned long) size << 4));
                gen(state, value);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_mov_imm_reg(void);
            gen(state, (unsigned long) gadget_amd64_mov_imm_reg);
            gen(state, rm_id | ((unsigned long) size << 4));
            gen(state, value);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        if ((insn.opcode == 0x80 || insn.opcode == 0x81 ||
                    insn.opcode == 0x83) &&
                (group == 0 || group == 5 || group == 7) &&
                (size == 32 || size == 64)) {
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_arith_reg_imm(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_arith_reg_imm);
                gen(state, packed);
                gen(state, value);
                if (group != 7)
                    gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
        }
        if ((insn.opcode == 0xc0 || insn.opcode == 0xc1) &&
                (group == 4 || group == 5 || group == 7) &&
                (size == 32 || size == 64)) {
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_shift_reg_count(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_shift_reg_count);
                gen(state, packed);
                gen(state, value);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
        }
        if ((insn.opcode == 0x80 || insn.opcode == 0x81 ||
                    insn.opcode == 0x83) &&
                (group == 1 || group == 4 || group == 6) &&
                !(size == 8 && !insn.rex.present &&
                    amd64_modrm_rm(insn.modrm) >= 4)) {
            amd64_jit_debug("logic-reg-imm-direct ip=%llx opcode=%02x group=%u rm=%u size=%u value=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    insn.opcode,
                    group,
                    rm_id,
                    size,
                    (unsigned long long) value,
                    (unsigned long long) next_ip);
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_logic_reg_imm(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_logic_reg_imm);
                gen(state, packed);
                gen(state, value);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_logic_reg_imm(void);
            gen(state, (unsigned long) gadget_amd64_logic_reg_imm);
            gen(state, packed);
            gen(state, value);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        amd64_jit_debug("reg-imm-helper ip=%llx opcode=%02x group=%u rm=%u size=%u value=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                group,
                rm_id,
                size,
                (unsigned long long) value,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_3_retint(state, amd64_jit_reg_imm_op,
                packed, value, (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode &&
            !insn.address_size_prefix &&
            !insn.fs_prefix &&
            !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) == 3) {
        switch (insn.opcode) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x0b:
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x18:
        case 0x19:
        case 0x1a:
        case 0x1b:
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x28:
        case 0x29:
        case 0x2a:
        case 0x2b:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x38:
        case 0x39:
        case 0x3a:
        case 0x3b:
        case 0x63:
        case 0x84:
        case 0x85:
        case 0x88:
        case 0x89:
        case 0x8a:
        case 0x8b: {
            unsigned size = ((insn.opcode & 1) == 0 ||
                    insn.opcode == 0x38 || insn.opcode == 0x3a ||
                    insn.opcode == 0x84 || insn.opcode == 0x88 ||
                    insn.opcode == 0x8a ||
                    insn.opcode == 0x08 || insn.opcode == 0x20)
                ? 8
                : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
            unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
            unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            unsigned long packed = (unsigned long) insn.opcode |
                ((unsigned long) reg_id << 8) |
                ((unsigned long) rm_id << 12) |
                ((unsigned long) size << 16);
            if (insn.rex.present)
                packed |= 1ul << 24;
            next_ip = state->amd64_ip + 1;
            state->amd64_ip = next_ip;
            if ((insn.opcode == 0x85) ||
                    (insn.opcode == 0x84 &&
                     (insn.rex.present ||
                      (amd64_modrm_reg(insn.modrm) < 4 && amd64_modrm_rm(insn.modrm) < 4)))) {
                amd64_jit_debug("test-reg-reg-direct ip=%llx reg=%u rm=%u size=%u next=%llx",
                        (unsigned long long) insn.start_ip,
                        reg_id,
                        rm_id,
                        size,
                        (unsigned long long) next_ip);
#if defined(__aarch64__)
                if (amd64_jit_low8_reg(reg_id) && amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_cached_test_reg_reg(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_cached_test_reg_reg);
                    gen(state, rm_id | (reg_id << 4) | ((unsigned long) size << 8));
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
                if (amd64_jit_low8_reg(reg_id) || amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_hybrid_test_reg_reg(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_hybrid_test_reg_reg);
                    gen(state, rm_id | (reg_id << 4) | ((unsigned long) size << 8));
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
#endif
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_test_reg_reg(void);
                gen(state, (unsigned long) gadget_amd64_test_reg_reg);
                gen(state, rm_id | (reg_id << 4) | ((unsigned long) size << 8));
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
            if ((insn.opcode == 0x31 || insn.opcode == 0x33) &&
                    reg_id == rm_id && size != 8) {
                amd64_jit_debug("xor-zero-direct ip=%llx reg=%u size=%u next=%llx",
                        (unsigned long long) insn.start_ip,
                        reg_id,
                        size,
                        (unsigned long long) next_ip);
#if defined(__aarch64__)
                if (amd64_jit_low8_reg(reg_id)) {
                    extern void gadget_amd64_cached_xor_zero_reg(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_cached_xor_zero_reg);
                    gen(state, reg_id | ((unsigned long) size << 4));
                    gen_amd64_mark_reg_cache_dirty(state);
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
#endif
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_xor_zero_reg(void);
                gen(state, (unsigned long) gadget_amd64_xor_zero_reg);
                gen(state, reg_id | ((unsigned long) size << 4));
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
            if ((insn.opcode >= 0x08 && insn.opcode <= 0x0b) ||
                    (insn.opcode >= 0x20 && insn.opcode <= 0x23) ||
                    (insn.opcode >= 0x30 && insn.opcode <= 0x33)) {
                if (size == 8 && !insn.rex.present &&
                        (amd64_modrm_reg(insn.modrm) >= 4 ||
                         amd64_modrm_rm(insn.modrm) >= 4))
                    goto amd64_bridge_step;
                amd64_jit_debug("logic-reg-reg-direct ip=%llx opcode=%02x reg=%u rm=%u size=%u next=%llx",
                        (unsigned long long) insn.start_ip,
                        insn.opcode,
                        reg_id,
                        rm_id,
                        size,
                        (unsigned long long) next_ip);
#if defined(__aarch64__)
                if (amd64_jit_low8_reg(reg_id) && amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_cached_logic_reg_reg(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_cached_logic_reg_reg);
                    gen(state, packed);
                    gen_amd64_mark_reg_cache_dirty(state);
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
                if (amd64_jit_low8_reg(reg_id) || amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_hybrid_logic_reg_reg(void);
                    unsigned dst_id = (insn.opcode & 2) == 0 ? rm_id : reg_id;
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_hybrid_logic_reg_reg);
                    gen(state, packed);
                    if (amd64_jit_low8_reg(dst_id))
                        gen_amd64_mark_reg_cache_dirty(state);
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
#endif
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_logic_reg_reg(void);
                gen(state, (unsigned long) gadget_amd64_logic_reg_reg);
                gen(state, packed);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
            if ((insn.opcode <= 0x03 ||
                     (insn.opcode >= 0x28 && insn.opcode <= 0x2b) ||
                     (insn.opcode >= 0x38 && insn.opcode <= 0x3b)) &&
                    (size == 32 || size == 64)) {
#if defined(__aarch64__)
                if (amd64_jit_low8_reg(reg_id) && amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_cached_arith_reg_reg(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_cached_arith_reg_reg);
                    gen(state, packed);
                    if (insn.opcode < 0x38)
                        gen_amd64_mark_reg_cache_dirty(state);
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
#endif
            }
            amd64_jit_debug("reg-reg-helper ip=%llx opcode=%02x reg=%u rm=%u size=%u next=%llx",
                    (unsigned long long) insn.start_ip,
                    insn.opcode,
                    reg_id,
                    rm_id,
                    size,
                    (unsigned long long) next_ip);
            gen_amd64_helper_tlb_2_retint(state, amd64_jit_reg_reg_op,
                    packed, (unsigned long) next_ip);
            return true;
        }
        default:
            break;
        }
    }

    // Native MOV reg<->mem (0x8a/0x8b load, 0x88/0x89 store), mod!=3: the most
    // common memory instruction and flag-free. Like the vector mem ops, the reg
    // cache and rip are flushed so a #PF re-executes -- base/index are read from
    // CPU_amd64_regs and the destination reg is written there too. Per-size gadget
    // keeps the vread/vwrite size literal (correct cross-page staging). The byte
    // gadgets handle the AH/CH/DH/BH high-byte aliasing (modrm.reg 4-7 without REX)
    // via meta's REX_PRESENT bit. FS-prefix and address-size forms keep bridging
    // (amd64_vmem_addr adds no tls_ptr).
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x88 || insn.opcode == 0x89 ||
             insn.opcode == 0x8a || insn.opcode == 0x8b)) {
        bool is_load = insn.opcode == 0x8a || insn.opcode == 0x8b;
        bool is_byte = insn.opcode == 0x88 || insn.opcode == 0x8a;
        unsigned size = is_byte ? 8
            : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("mov-mem ip=%llx op=%02x load=%d size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, is_load, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_mov_load8(void), gadget_amd64_mov_load16(void),
                gadget_amd64_mov_load32(void), gadget_amd64_mov_load64(void),
                gadget_amd64_mov_store8(void), gadget_amd64_mov_store16(void),
                gadget_amd64_mov_store32(void), gadget_amd64_mov_store64(void);
        void (*g)(void);
        if (is_load)
            g = size == 8 ? gadget_amd64_mov_load8
              : size == 16 ? gadget_amd64_mov_load16
              : size == 32 ? gadget_amd64_mov_load32 : gadget_amd64_mov_load64;
        else
            g = size == 8 ? gadget_amd64_mov_store8
              : size == 16 ? gadget_amd64_mov_store16
              : size == 32 ? gadget_amd64_mov_store32 : gadget_amd64_mov_store64;
        gen(state, (unsigned long) g);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native load-op arith: reg <op>= [mem] for the d=1 forms ADD (0x03), SUB (0x2b),
    // CMP (0x3b), mod!=3, 32/64-bit (no 0x66). reg is the dst/lhs, [mem] is the rhs;
    // flags are set eagerly. Mirrors the cached_arith_reg_reg coverage (ADC/SBB and
    // byte forms 0x02/2a/3a keep bridging; 16-bit bridges). Same flush+#PF-reexec
    // discipline as MOV; FS-prefix and address-size forms bridge.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x03 || insn.opcode == 0x2b || insn.opcode == 0x3b)) {
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("loadop-arith ip=%llx op=%02x size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_loadop_arith32(void), gadget_amd64_loadop_arith64(void);
        gen(state, (unsigned long) (size == 64
                    ? gadget_amd64_loadop_arith64 : gadget_amd64_loadop_arith32));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native load-op logic: reg <op>= [mem] for OR (0x0b), AND (0x23), XOR (0x33),
    // mod!=3, 32/64-bit (no 0x66). Same as load-op arith but the logic flag rule
    // (CF=OF=0, ZF/SF/PF from result). Byte forms (0x0a/22/32), TEST, 16-bit bridge.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x0b || insn.opcode == 0x23 || insn.opcode == 0x33)) {
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("loadop-logic ip=%llx op=%02x size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_loadop_logic32(void), gadget_amd64_loadop_logic64(void);
        gen(state, (unsigned long) (size == 64
                    ? gadget_amd64_loadop_logic64 : gadget_amd64_loadop_logic32));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native op-store/RMW arith: [mem] <op>= reg for ADD (0x01), SUB (0x29), CMP
    // (0x39), mod!=3, 32/64-bit (no 0x66). [mem] is both source and destination
    // (read-modify-write); modrm.reg is the rhs. Flags eager; CMP is flags-only (no
    // store). Byte forms (0x00/28/38), ADC/SBB, 16-bit, and locked forms keep
    // bridging. Same flush + #PF-reexec discipline (a fault on the store re-reads).
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x01 || insn.opcode == 0x29 || insn.opcode == 0x39)) {
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("opstore-arith ip=%llx op=%02x size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_opstore_arith32(void), gadget_amd64_opstore_arith64(void);
        gen(state, (unsigned long) (size == 64
                    ? gadget_amd64_opstore_arith64 : gadget_amd64_opstore_arith32));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native op-store/RMW logic + TEST: [mem] <op>= reg for OR (0x09), AND (0x21),
    // XOR (0x31) -- read-modify-write -- and TEST (0x85, = [mem] AND reg, flags only,
    // no store), mod!=3, 32/64-bit (no 0x66). Logic flags. Byte forms (0x08/20/30/84),
    // 16-bit, and locked forms keep bridging.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x09 || insn.opcode == 0x21 ||
             insn.opcode == 0x31 || insn.opcode == 0x85)) {
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("opstore-logic ip=%llx op=%02x size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_opstore_logic32(void), gadget_amd64_opstore_logic64(void);
        gen(state, (unsigned long) (size == 64
                    ? gadget_amd64_opstore_logic64 : gadget_amd64_opstore_logic32));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native MOVSXD reg64 <- [mem]32 (movslq, 0x63 with REX.W), mod!=3: sign-extend a
    // 32-bit memory operand into a 64-bit register, no flags. Only the REX.W form is
    // routed here (the rare non-W 0x63 32-bit form, and FS/address-size, keep bridging).
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.opcode == 0x63 && insn.rex.w) {
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 32, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("movsxd-mem ip=%llx meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_movsxd_mem(void);
        gen(state, (unsigned long) gadget_amd64_movsxd_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native adc/sbb with a memory operand, 32/64-bit (no 0x66), mod!=3 -- completes
    // the arith family. Load-op (reg <op>= [mem] + carry): ADC 0x13, SBB 0x1b. Op-
    // store/RMW ([mem] <op>= reg + carry): ADC 0x11, SBB 0x19. Carry-in = CPU_cf; the
    // gadgets use ARM adcs/sbcs and feed rhs+cf to the addsub flag macro for AF (the
    // interp computes AF/OF from rhs_with_carry). Byte (0x10/12/18/1a), 16-bit, and
    // locked forms keep bridging.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x13 || insn.opcode == 0x1b ||
             insn.opcode == 0x11 || insn.opcode == 0x19)) {
        bool is_store = insn.opcode == 0x11 || insn.opcode == 0x19;
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("addc-mem ip=%llx op=%02x store=%d size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, is_store, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_loadop_addc32(void), gadget_amd64_loadop_addc64(void),
                gadget_amd64_opstore_addc32(void), gadget_amd64_opstore_addc64(void);
        void (*g)(void);
        if (is_store)
            g = size == 64 ? gadget_amd64_opstore_addc64 : gadget_amd64_opstore_addc32;
        else
            g = size == 64 ? gadget_amd64_loadop_addc64 : gadget_amd64_loadop_addc32;
        gen(state, (unsigned long) g);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode &&
            !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x00 || insn.opcode == 0x01 ||
             insn.opcode == 0x02 || insn.opcode == 0x03 ||
             insn.opcode == 0x08 ||
             insn.opcode == 0x09 || insn.opcode == 0x0a || insn.opcode == 0x0b ||
             insn.opcode == 0x10 || insn.opcode == 0x12 ||
             insn.opcode == 0x11 || insn.opcode == 0x13 ||
             insn.opcode == 0x18 || insn.opcode == 0x1a ||
             insn.opcode == 0x19 || insn.opcode == 0x1b ||
             insn.opcode == 0x20 || insn.opcode == 0x21 || insn.opcode == 0x22 || insn.opcode == 0x23 ||
             insn.opcode == 0x28 || insn.opcode == 0x2a ||
             insn.opcode == 0x29 || insn.opcode == 0x2b ||
             insn.opcode == 0x30 || insn.opcode == 0x32 ||
             insn.opcode == 0x31 || insn.opcode == 0x33 ||
             insn.opcode == 0x38 || insn.opcode == 0x39 ||
             insn.opcode == 0x3a || insn.opcode == 0x3b ||
             insn.opcode == 0x84 || insn.opcode == 0x85 ||
             insn.opcode == 0x88 || insn.opcode == 0x89 ||
             insn.opcode == 0x8a || insn.opcode == 0x8b ||
             insn.opcode == 0x8d || insn.opcode == 0x63)) {
        unsigned size = ((insn.opcode & 1) == 0 ||
                insn.opcode == 0x38 || insn.opcode == 0x3a ||
                insn.opcode == 0x84 || insn.opcode == 0x88 ||
                insn.opcode == 0x8a ||
                insn.opcode == 0x08 || insn.opcode == 0x20)
            ? 8
            : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
        unsigned long meta;
        unsigned long disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp,
                &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("mem-op-helper ip=%llx opcode=%02x meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                meta,
                disp,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_3_retint(state, amd64_jit_mem_op,
                meta, disp, (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            (insn.opcode == 0x80 || insn.opcode == 0x81 ||
             insn.opcode == 0x83 || insn.opcode == 0xc0 ||
             insn.opcode == 0xc1 || insn.opcode == 0xc6 ||
             insn.opcode == 0xc7)) {
        if (amd64_modrm_mod(insn.modrm) != 3 &&
                (insn.opcode == 0x80 || insn.opcode == 0x81 || insn.opcode == 0x83) &&
                amd64_modrm_reg(insn.modrm) == 7) {
            // Memory-form cmp imm* drives trap-on-mismatch sequences in glibc/musl
            // startup code. Keep it on the interpreter path until the frontend/helper
            // path is proven equivalent.
            goto amd64_bridge_step;
        }
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (insn.opcode == 0x80 || insn.opcode == 0x83 ||
                insn.opcode == 0xc0 || insn.opcode == 0xc1 ||
                insn.opcode == 0xc6) {
            next_ip += sizeof(uint8_t);
        } else {
            next_ip += insn.operand_size_prefix ? sizeof(uint16_t) : sizeof(uint32_t);
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("modrm-imm-helper ip=%llx opcode=%02x modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_modrm_imm,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        return true;
    }

amd64_bridge_step:
    amd64_jit_debug("helper-step ip=%llx opcode=%02x two_byte=%d op2=%02x rex=%d%d%d%d%d opsz=%d addrsz=%d fs=%d lock=%d rep=%d has_modrm=%d modrm=%02x",
            (unsigned long long) insn.start_ip,
            insn.opcode,
            insn.two_byte_opcode,
            insn.op2,
            insn.rex.present,
            insn.rex.w,
            insn.rex.r,
            insn.rex.x,
            insn.rex.b,
            insn.operand_size_prefix,
            insn.address_size_prefix,
            insn.fs_prefix,
            insn.lock_prefix,
            insn.rep_mode,
            insn.has_modrm,
            insn.modrm);
    state->amd64_fallback_to_interp = true;
    return false;
}

void gen_end(struct gen_state *state) {
    if (state->amd64) {
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
    }
    struct jit_block *block = state->block;
    for (int i = 0; i <= 1; i++) {
        if (state->jump_ip[i] != 0) {
            block->jump_ip[i] = &block->code[state->jump_ip[i]];
            block->old_jump_ip[i] = *block->jump_ip[i];
        } else {
            block->jump_ip[i] = NULL;
        }

        list_init(&block->jumps_from[i]);
        list_init(&block->jumps_from_links[i]);
    }
    if (state->block_patch_ip != 0) {
        block->code[state->block_patch_ip] = (unsigned long) block;
    }
    if (state->amd64) {
        if (block->addr != state->amd64_ip)
            block->end_addr = state->amd64_ip - 1;
        else
            block->end_addr = block->addr;
    } else if (block->addr != state->ip)
        block->end_addr = state->ip - 1;
    else
        block->end_addr = block->addr;
    list_init(&block->chain);
    block->is_jetsam = false;
    for (int i = 0; i <= 1; i++) {
        list_init(&block->page[i]);
    }
}

void gen_exit(struct gen_state *state) {
    extern void gadget_exit(void);
    if (state->amd64) {
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
    }
    // in case the last instruction didn't end the block
    gen(state, (unsigned long) gadget_exit);
    gen(state, state->amd64 ? (unsigned long) (addr_t) state->amd64_ip : state->ip);
}

#define DECLARE_LOCALS \
    dword_t addr_offset = 0; \
    bool end_block = false; \
    bool seg_tls = false

#define FINISH \
    return !end_block

#define RESTORE_IP state->ip = state->orig_ip
#define _READIMM(name, size) do {\
    state->ip += size/8; \
    if (!tlb_read(tlb, state->ip - size/8, &name, size/8)) SEGFAULT; \
} while (0)

#define READMODRM if (!modrm_decode32(&state->ip, tlb, &modrm)) SEGFAULT
#define READADDR _READIMM(addr_offset, 32)
#define SEG_GS() seg_tls = true
#define SEG_FS() seg_tls = true

// This should stay in sync with the definition of .gadget_array in gadgets.h
enum arg {
    arg_reg_a, arg_reg_c, arg_reg_d, arg_reg_b, arg_reg_sp, arg_reg_bp, arg_reg_si, arg_reg_di,
    arg_reg_ah = arg_reg_sp, arg_reg_ch = arg_reg_bp, arg_reg_dh = arg_reg_si, arg_reg_bh = arg_reg_di,
    arg_imm, arg_mem, arg_addr, arg_gs,
    arg_count, arg_invalid,
    // the following should not be synced with the list mentioned above (no gadgets implement them)
    arg_modrm_val, arg_modrm_reg,
    arg_xmm_modrm_val, arg_xmm_modrm_reg,
    arg_mm_modrm_val, arg_mm_modrm_reg,
    arg_mem_addr, arg_1,
};

enum size {
    size_8, size_16, size_32,
    size_count,
    size_64, size_80, size_128, // bonus sizes
};

// sync with COND_LIST in control.S
enum cond {
    cond_O, cond_B, cond_E, cond_BE, cond_S, cond_P, cond_L, cond_LE,
    cond_count,
};

enum repeat {
    rep_once, rep_repz, rep_repnz,
    rep_count,
    rep_rep = rep_repz,
};

typedef void (*gadget_t)(void);

#define GEN(thing) gen(state, (unsigned long) (thing))
#define g(g) do { extern void gadget_##g(void); GEN(gadget_##g); } while (0)
#define gg(_g, a) do { g(_g); GEN(a); } while (0)
#define ggg(_g, a, b) do { g(_g); GEN(a); GEN(b); } while (0)
#define gggg(_g, a, b, c) do { g(_g); GEN(a); GEN(b); GEN(c); } while (0)
#define ggggg(_g, a, b, c, d) do { g(_g); GEN(a); GEN(b); GEN(c); GEN(d); } while (0)
#define gggggg(_g, a, b, c, d, e) do { g(_g); GEN(a); GEN(b); GEN(c); GEN(d); GEN(e); } while (0)
#define ga(g, i) do { extern gadget_t g##_gadgets[]; if (g##_gadgets[i] == NULL) UNDEFINED; GEN(g##_gadgets[i]); } while (0)
#define gag(g, i, a) do { ga(g, i); GEN(a); } while (0)
#define gagg(g, i, a, b) do { ga(g, i); GEN(a); GEN(b); } while (0)
#define gz(g, z) ga(g, sz(z))
#define h(h) gg(helper_0, h)
#define hh(h, a) ggg(helper_1, h, a)
#define hhh(h, a, b) gggg(helper_2, h, a, b)
#define ht_retint(h) gg(helper_tlb_0_retint, h)
#define h_read(h, z) do { g_addr(); ggg(helper_read##z, state->orig_ip, h##z); } while (0)
#define h_write(h, z) do { g_addr(); ggg(helper_write##z, state->orig_ip, h##z); } while (0)
#define UNDEFINED do { gggg(interrupt, INT_UNDEFINED, state->orig_ip, state->orig_ip); return false; } while (0)
#define SEGFAULT do { gggg(interrupt, INT_GPF, state->orig_ip, tlb->segfault_addr); return false; } while (0)
#define SYSCALL_AMD64 do { gggg(interrupt, INT_AMD64_SYSCALL, state->ip, 0); return false; } while (0)

static inline int sz(int size) {
    switch (size) {
        case 8: return size_8;
        case 16: return size_16;
        case 32: return size_32;
        default: return -1;
    }
}

bool gen_addr(struct gen_state *state, struct modrm *modrm, bool seg_tls) {
    if (modrm->base == reg_none)
        gg(addr_none, modrm->offset);
    else
        gag(addr, modrm->base, modrm->offset);
    if (modrm->type == modrm_mem_si)
        ga(si, modrm->index * 4 + modrm->shift);
    if (seg_tls)
        g(seg_gs);
    return true;
}
#define g_addr() gen_addr(state, &modrm, seg_tls)

// this really wants to use all the locals of the decoder, which we can do
// really nicely in gcc using nested functions, but that won't work in clang,
// so we explicitly pass 500 arguments. sorry for the mess
static inline bool gen_op(struct gen_state *state, gadget_t *gadgets, enum arg arg, struct modrm *modrm, uint64_t *imm, int size, bool seg_tls, dword_t addr_offset) {
    size = sz(size);
    gadgets = gadgets + size * arg_count;

    switch (arg) {
        case arg_modrm_reg:
            // TODO find some way to assert that this won't overflow?
            arg = modrm->reg + arg_reg_a; break;
        case arg_modrm_val:
            if (modrm->type == modrm_reg)
                arg = modrm->base + arg_reg_a;
            else
                arg = arg_mem;
            break;
        case arg_mem_addr:
            arg = arg_mem;
            modrm->type = modrm_mem;
            modrm->base = reg_none;
            modrm->offset = addr_offset;
            break;
        case arg_1:
            arg = arg_imm;
            *imm = 1;
            break;
    }
    if (arg >= arg_count || gadgets[arg] == NULL) {
        UNDEFINED;
    }
    if (arg == arg_mem || arg == arg_addr) {
        if (!gen_addr(state, modrm, seg_tls))
            return false;
    }
    GEN(gadgets[arg]);
    if (arg == arg_imm)
        GEN(*imm);
    else if (arg == arg_mem)
        GEN(state->orig_ip | state->orig_ip_extra);
    return true;
}

static inline enum arg gen_reg_arg(enum arg arg, struct modrm *modrm) {
    switch (arg) {
        case arg_modrm_reg:
            return modrm->reg + arg_reg_a;
        case arg_modrm_val:
            return modrm->type == modrm_reg ? modrm->base + arg_reg_a : arg_invalid;
        default:
            return arg >= arg_reg_a && arg <= arg_reg_di ? arg : arg_invalid;
    }
}

static inline bool gen_mov(struct gen_state *state, enum arg src, enum arg dst, struct modrm *modrm, uint64_t *imm, int size, bool seg_tls, dword_t addr_offset) {
#if defined(__aarch64__) || defined(__x86_64__)
    enum arg src_reg = gen_reg_arg(src, modrm);
    enum arg dst_reg = gen_reg_arg(dst, modrm);

    if (size == 32 && src_reg != arg_invalid && dst_reg != arg_invalid) {
        if (src_reg != dst_reg) {
            extern gadget_t mov32_reg_reg_gadgets[];
            GEN(mov32_reg_reg_gadgets[(dst_reg - arg_reg_a) * 8 + (src_reg - arg_reg_a)]);
        }
        return true;
    }
#endif

    extern gadget_t load_gadgets[];
    extern gadget_t store_gadgets[];
    return gen_op(state, load_gadgets, src, modrm, imm, size, seg_tls, addr_offset) &&
           gen_op(state, store_gadgets, dst, modrm, imm, size, seg_tls, addr_offset);
}

#define op(type, thing, z) do { \
    extern gadget_t type##_gadgets[]; \
    if (!gen_op(state, type##_gadgets, arg_##thing, &modrm, &imm, z, seg_tls, addr_offset)) return false; \
} while (0)

#define load(thing, z) op(load, thing, z)
#define store(thing, z) op(store, thing, z)
// load-op-store
#define los(o, src, dst, z) load(dst, z); op(o, src, z); store(dst, z)
#define lo(o, src, dst, z) load(dst, z); op(o, src, z)

#define MOV(src, dst,z) do { if (!gen_mov(state, arg_##src, arg_##dst, &modrm, &imm, z, seg_tls, addr_offset)) return false; } while (0)
#define MOVZX(src, dst,zs,zd) load(src, zs); gz(zero_extend, zs); store(dst, zd)
#define MOVSX(src, dst,zs,zd) load(src, zs); gz(sign_extend, zs); store(dst, zd)
// xchg must generate in this order to be atomic
#define XCHG(src, dst,z) load(src, z); op(xchg, dst, z); store(src, z)

#define ADD(src, dst,z) los(add, src, dst, z)
#define OR(src, dst,z) los(or, src, dst, z)
#define ADC(src, dst,z) los(adc, src, dst, z)
#define SBB(src, dst,z) los(sbb, src, dst, z)
#define AND(src, dst,z) los(and, src, dst, z)
#define SUB(src, dst,z) los(sub, src, dst, z)
#define XOR(src, dst,z) los(xor, src, dst, z)
#define CMP(src, dst,z) lo(sub, src, dst, z)
#define TEST(src, dst,z) lo(and, src, dst, z)
#define NOT(val,z) load(val,z); gz(not, z); store(val,z)
#define NEG(val,z) imm = 0; load(imm,z); op(sub, val,z); store(val,z)

#define POP(thing,z) \
    gg(pop, state->orig_ip); \
    state->orig_ip_extra = 1ul << 62; /* marks that on segfault the stack pointer should be adjusted */\
    store(thing, z)
#define PUSH(thing,z) load(thing, z); gg(push, state->orig_ip)

#define INC(val,z) load(val, z); gz(inc, z); store(val, z)
#define DEC(val,z) load(val, z); gz(dec, z); store(val, z)

#define fake_ip (state->ip | (1ul << 63))

#define jump_ips(off1, off2) \
    state->jump_ip[0] = state->size + off1; \
    if (off2 != 0) \
        state->jump_ip[1] = state->size + off2
#define JMP(loc) load(loc, OP_SIZE); g(jmp_indir); end_block = true
#define JMP_REL(off) gg(jmp, fake_ip + off); jump_ips(-1, 0); end_block = true
#define JCXZ_REL(off) ggg(jcxz, fake_ip + off, fake_ip); jump_ips(-2, -1); end_block = true
#define jcc(cc, to, else) gagg(jmp, cond_##cc, to, else); jump_ips(-2, -1); end_block = true
#define J_REL(cc, off)  jcc(cc, fake_ip + off, fake_ip)
#define JN_REL(cc, off) jcc(cc, fake_ip, fake_ip + off)

// state->orig_ip: for use with page fault handler;
// -1: will be patched to block address in gen_end();
// fake_ip: the first one is the return address, used for saving to stack and verifying the cached ip in return cache is correct;
// fake_ip: the second one is the return target, patchable by return chaining.
#define CALL(loc) do { \
    load(loc, OP_SIZE); \
    ggggg(call_indir, state->orig_ip, -1, fake_ip, fake_ip); \
    state->block_patch_ip = state->size - 3; \
    jump_ips(-1, 0); \
    end_block = true; \
} while (0)
// the first four arguments are the same with CALL,
// the last one is the call target, patchable by return chaining.
#define CALL_REL(off) do { \
    gggggg(call, state->orig_ip, -1, fake_ip, fake_ip, fake_ip + off); \
    state->block_patch_ip = state->size - 4; \
    jump_ips(-2, -1); \
    end_block = true; \
} while (0)
#define RET_NEAR(imm) ggg(ret, state->orig_ip, 4 + imm); end_block = true
#define INT(code) gggg(interrupt, (uint8_t) code, state->ip, 0); end_block = true

#define SET(cc, dst) ga(set, cond_##cc); store(dst, 8)
#define SETN(cc, dst) ga(setn, cond_##cc); store(dst, 8)
// wins the prize for the most annoying instruction to generate
#define CMOV(cc, src, dst,z) do { \
    gag(skipn, cond_##cc, 0); \
    int start = state->size; \
    load(src, z); store(dst, z); \
    state->block->code[start - 1] = (state->size - start) * sizeof(long); \
} while (0)
#define CMOVN(cc, src, dst,z) do { \
    gag(skip, cond_##cc, 0); \
    int start = state->size; \
    load(src, z); store(dst, z); \
    state->block->code[start - 1] = (state->size - start) * sizeof(long); \
} while (0)

#define PUSHF() g(pushf)
#define POPF() g(popf)
#define SAHF g(sahf)
#define CMC g(cmc)
#define CLC g(clc)
#define STC g(stc)
#define CLD g(cld)
#define STD g(std)

#define MUL18(val,z) MUL1(val,z)
#define MUL1(val,z) load(val, z); gz(mul, z)
#define IMUL1(val,z) load(val, z); gz(imul1, z)
#define DIV(val, z) load(val, z); gz(div, z)
#define IDIV(val, z) load(val, z); gz(idiv, z)
#define IMUL3(times, src, dst,z) load(src, z); op(imul, times, z); store(dst, z)
#define IMUL2(val, reg,z) IMUL3(val, reg, reg, z)

#define CVT ga(cvt, sz(oz))
#define CVTE ga(cvte, sz(oz))

#define ROL(count, val,z) los(rol, count, val, z)
#define ROR(count, val,z) los(ror, count, val, z)
#define RCL(count, val,z) los(rcl, count, val, z)
#define RCR(count, val,z) los(rcr, count, val, z)
#define SHL(count, val,z) los(shl, count, val, z)
#define SHR(count, val,z) los(shr, count, val, z)
#define SAR(count, val,z) los(sar, count, val, z)

#define SHLD(count, extra, dst,z) \
    load(dst,z); \
    if (arg_##count == arg_reg_c) op(shld_cl, extra,z); \
    else { op(shld_imm, extra,z); GEN(imm); } \
    store(dst,z)
#define SHRD(count, extra, dst,z) \
    load(dst,z); \
    if (arg_##count == arg_reg_c) op(shrd_cl, extra,z); \
    else { op(shrd_imm, extra,z); GEN(imm); } \
    store(dst,z)

#define BT(bit, val,z) lo(bt, val, bit, z)
#define BTC(bit, val,z) lo(btc, val, bit, z)
#define BTS(bit, val,z) lo(bts, val, bit, z)
#define BTR(bit, val,z) lo(btr, val, bit, z)
#define BSF(src, dst,z) los(bsf, src, dst, z)
#define BSR(src, dst,z) los(bsr, src, dst, z)

#define BSWAP(dst) ga(bswap, arg_##dst)

#define strop(op, rep, z) gag(op, sz(z) * size_count + rep_##rep, state->orig_ip)
#define STR(op, z) strop(op, once, z)
#define REP(op, z) strop(op, rep, z)
#define REPZ(op, z) strop(op, repz, z)
#define REPNZ(op, z) strop(op, repnz, z)

#define CMPXCHG(src, dst,z) load(src, z); op(cmpxchg, dst, z)
#define CMPXCHG8B(dst,z) g_addr(); gg(cmpxchg8b, state->orig_ip)
#define XADD(src, dst,z) XCHG(src, dst,z); ADD(src, dst,z)

void helper_rdtsc(struct cpu_state *cpu);
#define RDTSC h(helper_rdtsc)
#define CPUID() g(cpuid)

// atomic
#define atomic_op(type, src, dst,z) load(src, z); op(atomic_##type, dst, z)
#define ATOMIC_ADD(src, dst,z) atomic_op(add, src, dst, z)
#define ATOMIC_OR(src, dst,z) atomic_op(or, src, dst, z)
#define ATOMIC_ADC(src, dst,z) atomic_op(adc, src, dst, z)
#define ATOMIC_SBB(src, dst,z) atomic_op(sbb, src, dst, z)
#define ATOMIC_AND(src, dst,z) atomic_op(and, src, dst, z)
#define ATOMIC_SUB(src, dst,z) atomic_op(sub, src, dst, z)
#define ATOMIC_XOR(src, dst,z) atomic_op(xor, src, dst, z)
#define ATOMIC_INC(val,z) op(atomic_inc, val, z)
#define ATOMIC_DEC(val,z) op(atomic_dec, val, z)
#define ATOMIC_CMPXCHG(src, dst,z) atomic_op(cmpxchg, src, dst, z)
#define ATOMIC_XADD(src, dst,z) load(src, z); op(atomic_xadd, dst, z); store(src, z)
#define ATOMIC_BTC(bit, val,z) lo(atomic_btc, val, bit, z)
#define ATOMIC_BTS(bit, val,z) lo(atomic_bts, val, bit, z)
#define ATOMIC_BTR(bit, val,z) lo(atomic_btr, val, bit, z)
#define ATOMIC_CMPXCHG8B(dst,z) g_addr(); gg(atomic_cmpxchg8b, state->orig_ip)

// fpu
#define st_0 0
#define st_i modrm.rm_opcode
#define FLD() hh(fpu_ld, st_i);
#define FILD(val,z) h_read(fpu_ild, z)
#define FLDM(val,z) h_read(fpu_ldm, z)
#define FSTM(dst,z) h_write(fpu_stm, z)
#define FIST(dst,z) h_write(fpu_ist, z)
#define FXCH() hh(fpu_xch, st_i)
#define FCOM() hh(fpu_com, st_i)
#define FCOMM(val,z) h_read(fpu_comm, z)
#define FICOM(val,z) h_read(fpu_icom, z)
#define FUCOM() hh(fpu_ucom, st_i)
#define FUCOMI() hh(fpu_ucomi, st_i)
#define FCOMI() hh(fpu_comi, st_i)
#define FTST() h(fpu_tst)
#define FXAM() h(fpu_xam)
#define FST() hh(fpu_st, st_i)
#define FCHS() h(fpu_chs)
#define FABS() h(fpu_abs)
#define FLDC(what) hh(fpu_ldc, fconst_##what)
#define FPREM() h(fpu_prem)
#define FRNDINT() h(fpu_rndint)
#define FSCALE() h(fpu_scale)
#define FSQRT() h(fpu_sqrt)
#define FYL2X() h(fpu_yl2x)
#define F2XM1() h(fpu_2xm1)
#define FSTSW(dst) if (arg_##dst == arg_reg_a) g(fstsw_ax); else UNDEFINED
#define FSTCW(dst) if (arg_##dst == arg_reg_a) UNDEFINED; else h_write(fpu_stcw, 16)
#define FLDCW(dst) if (arg_##dst == arg_reg_a) UNDEFINED; else h_read(fpu_ldcw, 16)
#define FSTENV(val,z) h_write(fpu_stenv, z)
#define FLDENV(val,z) h_write(fpu_ldenv, z)
#define FSAVE(val,z) h_write(fpu_save, z)
#define FRESTORE(val,z) h_write(fpu_restore, z)
#define FCLEX() h(fpu_clex)
#define FPOP h(fpu_pop)
#define FINCSTP() h(fpu_incstp)
#define FADD(src, dst) hhh(fpu_add, src, dst)
#define FIADD(val,z) h_read(fpu_iadd, z)
#define FADDM(val,z) h_read(fpu_addm, z)
#define FSUB(src, dst) hhh(fpu_sub, src, dst)
#define FSUBM(val,z) h_read(fpu_subm, z)
#define FISUB(val,z) h_read(fpu_isub, z)
#define FISUBR(val,z) h_read(fpu_isubr, z)
#define FSUBR(src, dst) hhh(fpu_subr, src, dst)
#define FSUBRM(val,z) h_read(fpu_subrm, z)
#define FMUL(src, dst) hhh(fpu_mul, src, dst)
#define FIMUL(val,z) h_read(fpu_imul, z)
#define FMULM(val,z) h_read(fpu_mulm, z)
#define FDIV(src, dst) hhh(fpu_div, src, dst)
#define FIDIV(val,z) h_read(fpu_idiv, z)
#define FDIVM(val,z) h_read(fpu_divm, z)
#define FDIVR(src, dst) hhh(fpu_divr, src, dst)
#define FIDIVR(val,z) h_read(fpu_idivr, z)
#define FDIVRM(val,z) h_read(fpu_divrm, z)
#define FPATAN() h(fpu_patan)
#define FSIN() h(fpu_sin)
#define FCOS() h(fpu_cos)
#define FXTRACT() h(fpu_xtract)
#define FCMOVB(src) hh(fpu_cmovb, src)
#define FCMOVE(src) hh(fpu_cmove, src)
#define FCMOVBE(src) hh(fpu_cmovbe, src)
#define FCMOVU(src) hh(fpu_cmovu, src)
#define FCMOVNB(src) hh(fpu_cmovnb, src)
#define FCMOVNE(src) hh(fpu_cmovne, src)
#define FCMOVNBE(src) hh(fpu_cmovnbe, src)
#define FCMOVNU(src) hh(fpu_cmovnu, src)

// vector

static inline bool could_be_memory(enum arg arg) {
    return arg == arg_modrm_val || arg == arg_mm_modrm_val || arg == arg_xmm_modrm_val;
}

static inline uint16_t cpu_reg_offset(enum arg arg, int index) {
    if (arg == arg_xmm_modrm_reg || arg == arg_xmm_modrm_val)
        return CPU_OFFSET(xmm[index]);
    if (arg == arg_mm_modrm_reg || arg == arg_mm_modrm_val)
        return CPU_OFFSET(mm[index]);
    if (arg == arg_modrm_reg || arg == arg_modrm_val)
        return CPU_OFFSET(regs[index]);
    return 0;
}

static inline bool gen_vec(enum arg src, enum arg dst, void (*helper)(), gadget_t read_mem_gadget, gadget_t write_mem_gadget, struct gen_state *state, struct modrm *modrm, uint8_t imm, bool seg_tls, bool has_imm) {
    bool rm_is_src = !could_be_memory(dst);
    enum arg rm = rm_is_src ? src : dst;
    enum arg reg = rm_is_src ? dst : src;

    uint16_t reg_offset = cpu_reg_offset(reg, modrm->opcode);
    uint16_t rm_reg_offset = cpu_reg_offset(rm, modrm->rm_opcode);
    assert(reg_offset != 0);

    if (could_be_memory(rm) && modrm->type != modrm_reg)
        rm = arg_mem;

    uint64_t imm_arg = 0;
    if (has_imm)
        imm_arg = (uint64_t) imm << 32;

    switch (rm) {
        case arg_xmm_modrm_val:
        case arg_mm_modrm_val:
        case arg_modrm_val:
            assert(rm_reg_offset != 0);
            if (!has_imm)
                g(vec_helper_reg);
            else
                g(vec_helper_reg_imm);
            GEN(helper);
            // first byte is src, second byte is dst
            uint64_t arg;
            if (rm_is_src)
                arg = rm_reg_offset | (reg_offset << 16);
            else
                arg = reg_offset | (rm_reg_offset << 16);
            GEN(arg | imm_arg);
            break;

        case arg_mem:
            gen_addr(state, modrm, seg_tls);
            GEN(rm_is_src ? read_mem_gadget : write_mem_gadget);
            GEN(state->orig_ip);
            GEN(helper);
            GEN(reg_offset | imm_arg);
            break;

        case arg_imm:
            // TODO: support immediates and opcode
            g(vec_helper_imm);
            GEN(helper);
            // This is rm_opcode instead of opcode because PSRLQ is weird like that
            GEN(((uint16_t) imm) | (cpu_reg_offset(reg, modrm->rm_opcode) << 16));
            break;

        default:
            printk("jit: unimplemented vector op at ip %#x\n", (unsigned) state->orig_ip);
            UNDEFINED;
    }
    return true;
}

#define has_imm_ false
#define has_imm__imm true
#define _v(src, dst, helper, _imm, z) do { \
    extern void gadget_vec_helper_read##z##_imm(void); \
    extern void gadget_vec_helper_write##z##_imm(void); \
    if (!gen_vec(src, dst, (void (*)()) helper, gadget_vec_helper_read##z##_imm, gadget_vec_helper_write##z##_imm, state, &modrm, imm, seg_tls, has_imm_##_imm)) return false; \
} while (0)
#define v_(op, src, dst, _imm,z) _v(arg_##src, arg_##dst, vec_##op##z, _imm,z)
#define v(op, src, dst,z) v_(op, src, dst,,z)
#define v_imm(op, src, dst,z) v_(op, src, dst, _imm,z)

#define vec_dst_size_modrm_val 32
#define vec_dst_size_mm_modrm_val 64
#define vec_dst_size_mm_modrm_reg 64
#define vec_dst_size_xmm_modrm_val 128
#define vec_dst_size_xmm_modrm_reg 128
// you always want to merge when storing to memory
// default is to never merge otherwise
#define VMOV(src, dst, z) \
    if (could_be_memory(arg_##dst) && modrm.type != modrm_reg) { \
        v(merge, src, dst,z); \
    } else { \
        v(glue3(zero, vec_dst_size_##dst, _copy), src, dst,z); \
    }
// this will additionally merge if both src and dst are registers, e.g. movss
#define VMOV_MERGE_REG(src, dst, z) \
    if (modrm.type == modrm_reg || could_be_memory(arg_##dst)) { \
        v(merge, src, dst,z); \
    } else { \
        v(glue3(zero, vec_dst_size_##dst, _copy), src, dst,z); \
    }

#define VCOMPARE(src, dst,z) v(compare, src, dst,z)
#define V_OP(op, src, dst, z) v(op, src, dst, z)
#define V_OP_IMM(op, src, dst, z) v_imm(op, src, dst, z)

#define DECODER_RET static int
#define DECODER_NAME gen_step
#define DECODER_ARGS struct gen_state *state, struct tlb *tlb
#define DECODER_PASS_ARGS state, tlb

#define OP_SIZE 32
#include "emu/decode.h"
#undef OP_SIZE
#define OP_SIZE 16
#include "emu/decode.h"
#undef OP_SIZE
