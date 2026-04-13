#ifndef EMU_GEN_H
#define EMU_GEN_H

#include <setjmp.h>
#include "jit/jit.h"
#include "emu/tlb.h"

struct gen_state {
    addr_t ip;
    addr_t orig_ip;
    unsigned long orig_ip_extra;
    bool amd64;
    bool amd64_fallback_to_interp;
    addr_t amd64_fallback_ip;
    bool amd64_compat_legacy_exec;
    uint16_t amd64_low_reg_write_mask;
    struct jit_block *block;
    unsigned size;
    unsigned capacity;
    unsigned jump_ip[2];
    unsigned block_patch_ip; // for call/call_indir gadgets
    // OOM recovery: if oom_active, gen() longjmps instead of dying
    bool oom_active;
    jmp_buf oom_recovery;
};

bool gen_start(addr_t addr, struct gen_state *state); // returns false on OOM
bool gen_start_amd64(addr_t addr, struct gen_state *state); // returns false on OOM
void gen_exit(struct gen_state *state);
void gen_end(struct gen_state *state);

int gen_step(struct gen_state *state, struct tlb *tlb);
int gen_step_amd64(struct gen_state *state, struct tlb *tlb);

#endif
