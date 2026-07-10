// RISC-V GUEST gadgets (AArch64 host assembly implementing riscv64 guest
// semantics). Follows jit/guest-arm64/gadgets.h's conventions exactly —
// read that header first; its rules (x18 ban, entry.S reuse, memory-based
// guest GPR access) all apply here and are not repeated in full.
//
// Reuses jit/gadgets-aarch64/entry.S's jit_enter/gret/jit_exit unmodified,
// same as the arm64 guest engine: _cpu=x1, _tlb=x2, _ip=x28. Guest GPRs are
// memory-based (ldr/str against cpu_state per touch). Unlike guest-arm64,
// operands are addressed by BYTE OFFSET into cpu_state (computed by
// gen_step_riscv64 from the register number), so one gadget serves all 32
// registers and the x0-is-zero rule costs nothing here: gen passes
// CPU_riscv64_zero_sink as the destination offset for rd==x0 and the
// riscv64_regs[0] slot (never written, always 0) as any x0 source.

#include "../gadgets-generic.h"
#include "cpu-offsets.h"
#include "emu/interrupt.h"

_cpu    .req x1
_tlb    .req x2
_ip     .req x28

_tmp    .req x0
_tmp2   .req x8
_addr   .req x7

.extern jit_ret
.extern jit_exit

.macro .gadget name
    .global NAME(gadget_riscv64_\()\name)
    .align 4
    NAME(gadget_riscv64_\()\name) :
.endm

// Same ldar dispatch as jit/guest-arm64/gadgets.h's gret (see the rationale
// there); keep in sync if entry.S's contract changes.
.macro gret pop=0
.if \pop != 0
    add _ip, _ip, \pop*8
.endif
    ldar x9, [_ip]
    add _ip, _ip, 8
    cbnz x9, 0f
    b jit_ret
0:  br x9
.endm
