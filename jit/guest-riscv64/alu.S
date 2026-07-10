// RISC-V integer ALU gadgets: OP/OP-IMM (RV64I) + M extension.
// Reg-reg code stream: [gadget][rd_off][rs1_off][rs2_off]
// Reg-imm code stream: [gadget][rd_off][rs1_off][imm] (imm sign-extended
// to 64 bits by gen). W-suffix forms compute in 32 bits and sign-extend,
// exactly the RV64 rule.

#include "gadgets.h"

// x9=rd_off, x10=rs1 value, x11=rs2 value/imm; result in x10.
.macro alu_head
    ldr x9, [_ip]
    ldr x10, [_ip, #8]
    ldr x11, [_ip, #16]
    ldr x10, [_cpu, x10]
.endm
.macro alu_rr_head
    alu_head
    ldr x11, [_cpu, x11]
.endm
.macro alu_tail
    str x10, [_cpu, x9]
    gret 3
.endm

// Two variants per op: _rr (reg-reg) and _ri (reg-imm) where the ISA has
// both. Shift immediates arrive as the raw 0-63 shamt.
.macro alu_op name, insn:vararg
.gadget \name\()_rr
    alu_rr_head
    \insn
    alu_tail
.gadget \name\()_ri
    alu_head
    \insn
    alu_tail
.endm

alu_op add,  add x10, x10, x11
alu_op and,  and x10, x10, x11
alu_op or,   orr x10, x10, x11
alu_op xor,  eor x10, x10, x11
alu_op sll,  lsl x10, x10, x11
alu_op srl,  lsr x10, x10, x11
alu_op sra,  asr x10, x10, x11
// slt/sltu spelled out: `;` inside a macro argument is a statement
// separator to gas, so the two-instruction body can't ride the vararg.
.gadget slt_rr
    alu_rr_head
    cmp x10, x11
    cset x10, lt
    alu_tail
.gadget slt_ri
    alu_head
    cmp x10, x11
    cset x10, lt
    alu_tail
.gadget sltu_rr
    alu_rr_head
    cmp x10, x11
    cset x10, lo
    alu_tail
.gadget sltu_ri
    alu_head
    cmp x10, x11
    cset x10, lo
    alu_tail

// W-suffix 32-bit forms (sign-extended results)
.macro alu_opw name, insn:vararg
.gadget \name\()w_rr
    alu_rr_head
    \insn
    sxtw x10, w10
    alu_tail
.gadget \name\()w_ri
    alu_head
    \insn
    sxtw x10, w10
    alu_tail
.endm

alu_opw add, add w10, w10, w11
alu_opw sll, lsl w10, w10, w11
alu_opw srl, lsr w10, w10, w11
alu_opw sra, asr w10, w10, w11

// sub/subw have no immediate form
.gadget sub_rr
    alu_rr_head
    sub x10, x10, x11
    alu_tail
.gadget subw_rr
    alu_rr_head
    sub w10, w10, w11
    sxtw x10, w10
    alu_tail

// ---- M extension ----------------------------------------------------------
.gadget mul_rr
    alu_rr_head
    mul x10, x10, x11
    alu_tail
.gadget mulw_rr
    alu_rr_head
    mul w10, w10, w11
    sxtw x10, w10
    alu_tail
.gadget mulh_rr
    alu_rr_head
    smulh x10, x10, x11
    alu_tail
.gadget mulhu_rr
    alu_rr_head
    umulh x10, x10, x11
    alu_tail
.gadget mulhsu_rr
    // signed(rs1) * unsigned(rs2), high half: smulh then correct — the
    // standard identity: mulhsu(a,b) = smulh(a,b) + (a<0 ? 0 : 0) ... the
    // clean form: umulh(a,b) - (a<0 ? b : 0).
    alu_rr_head
    umulh x12, x10, x11
    cmp x10, #0
    csel x13, x11, xzr, lt
    sub x10, x12, x13
    alu_tail

// RISC-V division semantics (RV spec table 7.1): x/0 = -1 (divu: all
// ones), rem by 0 = rs1, INT_MIN/-1 = INT_MIN rem 0. AArch64 sdiv already
// gives INT_MIN for the overflow case and 0 for /0, so only the /0 result
// needs fixing; rem is msub-derived which yields rs1 automatically when
// the (patched) quotient is forced to the spec value... it does NOT for
// div-by-zero (spec quotient -1 would give rem = rs1 + rs2), so rem
// branches on zero explicitly.
.gadget div_rr
    alu_rr_head
    cbz x11, 1f
    sdiv x10, x10, x11
    alu_tail
1:  mov x10, #-1
    alu_tail
.gadget divu_rr
    alu_rr_head
    cbz x11, 1f
    udiv x10, x10, x11
    alu_tail
1:  mov x10, #-1
    alu_tail
.gadget rem_rr
    alu_rr_head
    cbz x11, 1f              // rem by zero = rs1 (x10 already holds it)
    sdiv x12, x10, x11
    msub x10, x12, x11, x10
1:  alu_tail
.gadget remu_rr
    alu_rr_head
    cbz x11, 1f
    udiv x12, x10, x11
    msub x10, x12, x11, x10
1:  alu_tail
.gadget divw_rr
    alu_rr_head
    cbz w11, 1f
    sdiv w10, w10, w11
    sxtw x10, w10
    alu_tail
1:  mov x10, #-1
    alu_tail
.gadget divuw_rr
    alu_rr_head
    cbz w11, 1f
    udiv w10, w10, w11
    sxtw x10, w10
    alu_tail
1:  mov x10, #-1
    alu_tail
.gadget remw_rr
    alu_rr_head
    cbz w11, 1f
    sdiv w12, w10, w11
    msub w10, w12, w11, w10
1:  sxtw x10, w10
    alu_tail
.gadget remuw_rr
    alu_rr_head
    cbz w11, 1f
    udiv w12, w10, w11
    msub w10, w12, w11, w10
1:  sxtw x10, w10
    alu_tail

// ---- Conditional branches (fused compare+branch, the ISA's own shape) ----
// Code stream: [gadget][rs1_off][rs2_off][taken][fallthrough]; both target
// words use the tagged-chainable scheme (riscv64_branch_dispatch).
.macro branch_op name, cond
.gadget b\name
    ldr x9, [_ip]
    ldr x10, [_ip, #8]
    ldr x9, [_cpu, x9]
    ldr x10, [_cpu, x10]
    cmp x9, x10
    b.\cond 1f
    ldr _ip, [_ip, #24]      // fallthrough
    b riscv64_branch_dispatch
1:  ldr _ip, [_ip, #16]      // taken
    b riscv64_branch_dispatch
.endm

branch_op eq, eq
branch_op ne, ne
branch_op lt, lt
branch_op ge, ge
branch_op ltu, lo
branch_op geu, hs

// JALR: indirect jump, target = (rs1 + imm) & ~1. Link write (if rd!=x0)
// is a separate mov_const emitted before this. No chaining (target is
// runtime-computed). Code stream: [gadget][rs1_off][imm]
.gadget jalr
    ldr x9, [_ip]
    ldr x10, [_ip, #8]
    ldr x9, [_cpu, x9]
    add x9, x9, x10
    bic x9, x9, #1
    str x9, [_cpu, #CPU_riscv64_pc]
    b jit_ret
