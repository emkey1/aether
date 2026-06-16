#include "gadgets.h"
#include "math.h"
#include "emu/interrupt.h"

.gadget load32_addr
    mov _tmp, _addr
    gret

.gadget load16_gs
    ldrh _tmp, [_cpu, #CPU_gs]
    gret

.gadget store16_gs
    strh _tmp, [_cpu, #CPU_gs]
    gret

.gadget amd64_set_rip
    ldr x8, [_ip]
    str x8, [_cpu, CPU_amd64_rip]
    str w8, [_cpu, CPU_eip]
    gret 1

.gadget amd64_load_low8_reg_cache
    add x8, _cpu, CPU_amd64_regs
    ldr x20, [x8, 0]
    ldr x22, [x8, 8]
    ldr x23, [x8, 16]
    ldr x21, [x8, 24]
    ldr x27, [x8, 32]
    ldr x26, [x8, 40]
    ldr x24, [x8, 48]
    ldr x25, [x8, 56]
    gret

.gadget amd64_store_low8_reg_cache
    add x8, _cpu, CPU_amd64_regs
    str x20, [x8, 0]
    str x22, [x8, 8]
    str x23, [x8, 16]
    str x21, [x8, 24]
    str x27, [x8, 32]
    str x26, [x8, 40]
    str x24, [x8, 48]
    str x25, [x8, 56]
    gret

.macro amd64_cached_read id, dst
    cmp \id, 0
    b.ne .Lamd64_cached_read_1_\@
    mov \dst, x20
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_1_\@: cmp \id, 1
    b.ne .Lamd64_cached_read_2_\@
    mov \dst, x22
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_2_\@: cmp \id, 2
    b.ne .Lamd64_cached_read_3_\@
    mov \dst, x23
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_3_\@: cmp \id, 3
    b.ne .Lamd64_cached_read_4_\@
    mov \dst, x21
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_4_\@: cmp \id, 4
    b.ne .Lamd64_cached_read_5_\@
    mov \dst, x27
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_5_\@: cmp \id, 5
    b.ne .Lamd64_cached_read_6_\@
    mov \dst, x26
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_6_\@: cmp \id, 6
    b.ne .Lamd64_cached_read_7_\@
    mov \dst, x24
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_7_\@: mov \dst, x25
.Lamd64_cached_read_done_\@:
.endm

.macro amd64_cached_write_one reg
    cmp x11, 64
    b.eq .Lamd64_cached_write_one_64_\@
    cmp x11, 32
    b.eq .Lamd64_cached_write_one_32_\@
    cmp x11, 16
    b.eq .Lamd64_cached_write_one_16_\@
    bfi \reg, x12, 0, 8
    b .Lamd64_cached_write_one_done_\@
.Lamd64_cached_write_one_64_\@: mov \reg, x12
    b .Lamd64_cached_write_one_done_\@
.Lamd64_cached_write_one_32_\@: ubfx \reg, x12, 0, 32
    b .Lamd64_cached_write_one_done_\@
.Lamd64_cached_write_one_16_\@: bfi \reg, x12, 0, 16
.Lamd64_cached_write_one_done_\@:
.endm

.macro amd64_cached_write id, src, size
    mov x11, \size
    mov x12, \src
    cmp \id, 0
    b.ne .Lamd64_cached_write_1_\@
    amd64_cached_write_one x20
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_1_\@: cmp \id, 1
    b.ne .Lamd64_cached_write_2_\@
    amd64_cached_write_one x22
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_2_\@: cmp \id, 2
    b.ne .Lamd64_cached_write_3_\@
    amd64_cached_write_one x23
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_3_\@: cmp \id, 3
    b.ne .Lamd64_cached_write_4_\@
    amd64_cached_write_one x21
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_4_\@: cmp \id, 4
    b.ne .Lamd64_cached_write_5_\@
    amd64_cached_write_one x27
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_5_\@: cmp \id, 5
    b.ne .Lamd64_cached_write_6_\@
    amd64_cached_write_one x26
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_6_\@: cmp \id, 6
    b.ne .Lamd64_cached_write_7_\@
    amd64_cached_write_one x24
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_7_\@: amd64_cached_write_one x25
.Lamd64_cached_write_done_\@:
.endm

.macro amd64_read_low_or_mem id, dst
    cmp \id, 8
    b.hs .Lamd64_read_mem_\@
    amd64_cached_read \id, \dst
    b .Lamd64_read_done_\@
.Lamd64_read_mem_\@:
    add x12, _cpu, CPU_amd64_regs
    ldr \dst, [x12, \id, lsl 3]
.Lamd64_read_done_\@:
.endm

.macro amd64_write_low_or_mem id, src, size
    mov x15, \src
    cmp \id, 8
    b.hs .Lamd64_write_mem_\@
    amd64_cached_write \id, x15, \size
    b .Lamd64_write_done_\@
.Lamd64_write_mem_\@:
    add x12, _cpu, CPU_amd64_regs
    add x12, x12, \id, lsl 3
    cmp \size, 64
    b.eq .Lamd64_write_mem_64_\@
    cmp \size, 32
    b.eq .Lamd64_write_mem_32_\@
    cmp \size, 16
    b.eq .Lamd64_write_mem_16_\@
    strb w15, [x12]
    b .Lamd64_write_done_\@
.Lamd64_write_mem_16_\@: strh w15, [x12]
    b .Lamd64_write_done_\@
.Lamd64_write_mem_32_\@: ubfx x12, x15, 0, 32
    add x13, _cpu, CPU_amd64_regs
    str x12, [x13, \id, lsl 3]
    b .Lamd64_write_done_\@
.Lamd64_write_mem_64_\@: str x15, [x12]
.Lamd64_write_done_\@:
.endm

.macro amd64_cached_mask_result result, size
    cmp \size, 64
    b.eq .Lamd64_cached_mask_done_\@
    cmp \size, 32
    b.eq .Lamd64_cached_mask_32_\@
    cmp \size, 16
    b.eq .Lamd64_cached_mask_16_\@
    and \result, \result, 0xff
    b .Lamd64_cached_mask_done_\@
.Lamd64_cached_mask_16_\@: and \result, \result, 0xffff
    b .Lamd64_cached_mask_done_\@
.Lamd64_cached_mask_32_\@: mov w15, w15
.Lamd64_cached_mask_done_\@:
.endm

.macro amd64_cached_set_logic_flags result, size
    ldr w16, [_cpu, CPU_eflags]
    mov w17, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w16, w16, w17
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w17, [_cpu, CPU_flags_res]
    bic w17, w17, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w17, [_cpu, CPU_flags_res]
    cmp \result, 0
    cset w17, eq
    orr w16, w16, w17, lsl 6
    and w17, w15, 0xff
    eor w17, w17, w17, lsr 4
    eor w17, w17, w17, lsr 2
    eor w17, w17, w17, lsr 1
    mvn w17, w17
    and w17, w17, 1
    orr w16, w16, w17, lsl 2
    cmp \size, 64
    b.eq .Lamd64_cached_flags_64_\@
    cmp \size, 32
    b.eq .Lamd64_cached_flags_32_\@
    cmp \size, 16
    b.eq .Lamd64_cached_flags_16_\@
    tst \result, 0x80
    b.eq .Lamd64_cached_flags_done_\@
    orr w16, w16, SF_FLAG
    b .Lamd64_cached_flags_done_\@
.Lamd64_cached_flags_16_\@: tst \result, 0x8000
    b.eq .Lamd64_cached_flags_done_\@
    orr w16, w16, SF_FLAG
    b .Lamd64_cached_flags_done_\@
.Lamd64_cached_flags_32_\@: tbnz \result, 31, .Lamd64_cached_flags_set_sf_\@
    b .Lamd64_cached_flags_done_\@
.Lamd64_cached_flags_64_\@: tbnz \result, 63, .Lamd64_cached_flags_set_sf_\@
    b .Lamd64_cached_flags_done_\@
.Lamd64_cached_flags_set_sf_\@: orr w16, w16, SF_FLAG
.Lamd64_cached_flags_done_\@: str w16, [_cpu, CPU_eflags]
.endm

.gadget amd64_cached_mov_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    amd64_cached_read x9, x12
    amd64_cached_write x10, x12, x11
    gret 1

.gadget amd64_hybrid_mov_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    amd64_read_low_or_mem x9, x15
    amd64_write_low_or_mem x10, x15, x11
    gret 1

.gadget amd64_cached_mov_imm_reg
    ldr x8, [_ip]
    ldr x12, [_ip, 8]
    and x10, x8, 0xf
    ubfx x11, x8, 4, 7
    amd64_cached_write x10, x12, x11
    gret 2

.gadget amd64_cached_logic_reg_imm
    ldr x8, [_ip]
    ldr x13, [_ip, 8]
    ubfx x9, x8, 8, 4
    ubfx x10, x8, 12, 4
    ubfx x11, x8, 16, 7
    amd64_cached_read x10, x15
    cmp x9, 4
    b.eq 1f
    cmp x9, 6
    b.eq 2f
    orr x15, x15, x13
    b 3f
1:  and x15, x15, x13
    b 3f
2:  eor x15, x15, x13
3:  amd64_cached_mask_result x15, x11
    amd64_cached_write x10, x15, x11
    amd64_cached_set_logic_flags x15, x11
    gret 2

.macro amd64_cached_set_addsub_flags sign_bit
    strb w14, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr w17, [_cpu, CPU_eflags]
    mov x12, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w17, w17, w12
    cbz w14, .Lamd64_addsub_no_cf_\@
    orr w17, w17, 1
.Lamd64_addsub_no_cf_\@: cbz w16, .Lamd64_addsub_no_of_\@
    orr w17, w17, (1 << 11)
.Lamd64_addsub_no_of_\@: eor x12, x13, x10
    eor x12, x12, x15
    tbz x12, 4, .Lamd64_addsub_no_af_\@
    orr w17, w17, AF_FLAG
.Lamd64_addsub_no_af_\@: cmp x15, 0
    b.ne .Lamd64_addsub_no_zf_\@
    orr w17, w17, ZF_FLAG
.Lamd64_addsub_no_zf_\@: tbnz x15, \sign_bit, .Lamd64_addsub_set_sf_\@
    b .Lamd64_addsub_after_sf_\@
.Lamd64_addsub_set_sf_\@: orr w17, w17, SF_FLAG
.Lamd64_addsub_after_sf_\@: and w12, w15, 0xff
    eor w12, w12, w12, lsr 4
    eor w12, w12, w12, lsr 2
    eor w12, w12, w12, lsr 1
    mvn w12, w12
    and w12, w12, 1
    cbz w12, .Lamd64_addsub_no_pf_\@
    orr w17, w17, PF_FLAG
.Lamd64_addsub_no_pf_\@: ldrb w12, [_cpu, CPU_flags_res]
    bic w12, w12, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w12, [_cpu, CPU_flags_res]
    str w17, [_cpu, CPU_eflags]
.endm

.gadget amd64_cached_arith_reg_imm
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    mov x8, x11
    mov x0, x12
    amd64_cached_read x11, x13
    cmp x12, 64
    b.eq 1f
    mov w13, w13
    mov w10, w10
    cmp x9, 0
    b.eq 2f
    subs w15, w13, w10
    cset w14, cc
    cset w16, vs
    b 3f
2:  adds w15, w13, w10
    cset w14, cs
    cset w16, vs
3:  mov w15, w15
    amd64_cached_set_addsub_flags 31
    cmp x9, 7
    b.eq 4f
    amd64_cached_write x8, x15, x0
4:  gret 2
1:  cmp x9, 0
    b.eq 5f
    subs x15, x13, x10
    cset w14, cc
    cset w16, vs
    b 6f
5:  adds x15, x13, x10
    cset w14, cs
    cset w16, vs
6:  amd64_cached_set_addsub_flags 63
    cmp x9, 7
    b.eq 7f
    amd64_cached_write x8, x15, x0
7:  gret 2

# imul body: x13 = op1, x10 = op2, x9 = dst reg id, x12 = size (32/64).
# result = low N bits of the signed product -> dst; CF=OF = (product was truncated),
# matching amd64_set_mul_flags. SF/ZF/AF/PF are left untouched (undefined per spec;
# the prior op's lazy/eager flag state stays valid). Clobbers x14-x17.
.macro amd64_imul_body
    cmp x12, 64
    b.eq 8001f
    sxtw x13, w13
    sxtw x10, w10
    mul x14, x13, x10
    sxtw x15, w14
    cmp x15, x14
    cset w16, ne
    mov w15, w14
    b 8002f
8001:
    mul x15, x13, x10
    smulh x14, x13, x10
    asr x17, x15, 63
    cmp x14, x17
    cset w16, ne
8002:
    amd64_cached_write x9, x15, x12
    strb w16, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr w17, [_cpu, CPU_eflags]
    mov w14, (1 | (1 << 11))
    bic w17, w17, w14
    cbz w16, 8003f
    orr w17, w17, w14
8003:
    str w17, [_cpu, CPU_eflags]
.endm

# imul reg, rm, imm (69 id / 6B ib), reg form: dst <- rm * imm (signed)
.gadget amd64_imul_imm
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    amd64_cached_read x11, x13
    amd64_imul_body
    gret 2

# imul reg, rm (0F AF), reg form: dst <- dst * rm (signed)
.gadget amd64_imul_reg
    ldr x8, [_ip]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    amd64_cached_read x9, x13
    amd64_cached_read x11, x10
    amd64_imul_body
    gret 1

.gadget amd64_cached_arith_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    mov x0, x12
    tst x9, 2
    b.ne 1f
    mov x8, x11
    amd64_cached_read x11, x13
    amd64_cached_read x10, x10
    b 2f
1:  mov x8, x10
    amd64_cached_read x10, x13
    amd64_cached_read x11, x10
2:  cmp x0, 64
    b.eq 3f
    mov w13, w13
    mov w10, w10
    cmp x9, 0x28
    b.hs 4f
    adds w15, w13, w10
    cset w14, cs
    cset w16, vs
    b 5f
4:  subs w15, w13, w10
    cset w14, cc
    cset w16, vs
5:  mov w15, w15
    amd64_cached_set_addsub_flags 31
    cmp x9, 0x38
    b.hs 6f
    amd64_cached_write x8, x15, x0
6:  gret 1
3:  cmp x9, 0x28
    b.hs 7f
    adds x15, x13, x10
    cset w14, cs
    cset w16, vs
    b 8f
7:  subs x15, x13, x10
    cset w14, cc
    cset w16, vs
8:  amd64_cached_set_addsub_flags 63
    cmp x9, 0x38
    b.hs 9f
    amd64_cached_write x8, x15, x0
9:  gret 1

.macro amd64_cached_set_shift_flags sign_bit
    strb w14, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr w17, [_cpu, CPU_eflags]
    mov x12, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w17, w17, w12
    cbz w14, .Lamd64_shift_no_cf_\@
    orr w17, w17, 1
.Lamd64_shift_no_cf_\@: cbz w16, .Lamd64_shift_no_of_\@
    orr w17, w17, (1 << 11)
.Lamd64_shift_no_of_\@: cmp x15, 0
    b.ne .Lamd64_shift_no_zf_\@
    orr w17, w17, ZF_FLAG
.Lamd64_shift_no_zf_\@: tbnz x15, \sign_bit, .Lamd64_shift_set_sf_\@
    b .Lamd64_shift_after_sf_\@
.Lamd64_shift_set_sf_\@: orr w17, w17, SF_FLAG
.Lamd64_shift_after_sf_\@: and w12, w15, 0xff
    eor w12, w12, w12, lsr 4
    eor w12, w12, w12, lsr 2
    eor w12, w12, w12, lsr 1
    mvn w12, w12
    and w12, w12, 1
    cbz w12, .Lamd64_shift_no_pf_\@
    orr w17, w17, PF_FLAG
.Lamd64_shift_no_pf_\@: ldrb w12, [_cpu, CPU_flags_res]
    bic w12, w12, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w12, [_cpu, CPU_flags_res]
    str w17, [_cpu, CPU_eflags]
.endm

.gadget amd64_cached_shift_reg_count
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    tbnz x10, 63, 16f
    b 17f
16: and x10, x22, 0xff
17:
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    mov x8, x11
    mov x0, x12
    cmp x12, 64
    b.eq 1f
    and x10, x10, 0x1f
    cbz x10, 9f
    amd64_cached_read x8, x13
    mov w13, w13
    cmp x9, 4
    b.eq 2f
    cmp x9, 5
    b.eq 3f
    asr w15, w13, w10
    sub w12, w10, 1
    lsr w14, w13, w12
    b 4f
2:  lsl w15, w13, w10
    mov w14, w13
    mov w12, 32
    sub w12, w12, w10
    lsr w14, w14, w12
    b 4f
3:  lsr w15, w13, w10
    sub w12, w10, 1
    lsr w14, w13, w12
4:  and w14, w14, 1
    mov w16, 0
    cmp x10, 1
    b.ne 5f
    cmp x9, 4
    b.eq 6f
    cmp x9, 5
    b.eq 7f
    b 5f
6:  lsr w16, w15, 31
    eor w16, w16, w14
    b 5f
7:  lsr w16, w13, 31
5:  mov w15, w15
    amd64_cached_write x8, x15, x0
    amd64_cached_set_shift_flags 31
    gret 2
1:  and x10, x10, 0x3f
    cbz x10, 9f
    amd64_cached_read x8, x13
    cmp x9, 4
    b.eq 10f
    cmp x9, 5
    b.eq 11f
    asr x15, x13, x10
    sub x12, x10, 1
    lsr x14, x13, x12
    b 12f
10: lsl x15, x13, x10
    mov x14, x13
    mov x12, 64
    sub x12, x12, x10
    lsr x14, x14, x12
    b 12f
11: lsr x15, x13, x10
    sub x12, x10, 1
    lsr x14, x13, x12
12: and w14, w14, 1
    mov w16, 0
    cmp x10, 1
    b.ne 13f
    cmp x9, 4
    b.eq 14f
    cmp x9, 5
    b.eq 15f
    b 13f
14: lsr x16, x15, 63
    eor w16, w16, w14
    b 13f
15: lsr x16, x13, 63
13: amd64_cached_write x8, x15, x0
    amd64_cached_set_shift_flags 63
9:  gret 2

.gadget amd64_cached_logic_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    mov x0, x12
    tbnz x9, 1, 1f
    mov x14, x11
    mov x13, x10
    b 2f
1:  mov x14, x10
    mov x13, x11
2:  amd64_cached_read x14, x15
    amd64_cached_read x13, x16
    cmp x9, 0x20
    b.hs 3f
    orr x15, x15, x16
    b 5f
3:  cmp x9, 0x30
    b.hs 4f
    and x15, x15, x16
    b 5f
4:  eor x15, x15, x16
5:  amd64_cached_mask_result x15, x0
    amd64_cached_write x14, x15, x0
    amd64_cached_set_logic_flags x15, x0
    gret 1

.gadget amd64_hybrid_logic_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    mov x0, x12
    tbnz x9, 1, 1f
    mov x14, x11
    mov x13, x10
    b 2f
1:  mov x14, x10
    mov x13, x11
2:  amd64_read_low_or_mem x14, x15
    amd64_read_low_or_mem x13, x16
    cmp x9, 0x20
    b.hs 3f
    orr x15, x15, x16
    b 5f
3:  cmp x9, 0x30
    b.hs 4f
    and x15, x15, x16
    b 5f
4:  eor x15, x15, x16
5:  amd64_cached_mask_result x15, x0
    amd64_write_low_or_mem x14, x15, x0
    amd64_cached_set_logic_flags x15, x0
    gret 1

.gadget amd64_cached_test_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    amd64_cached_read x9, x13
    amd64_cached_read x10, x14
    and x15, x13, x14
    amd64_cached_mask_result x15, x11
    amd64_cached_set_logic_flags x15, x11
    gret 1

.gadget amd64_hybrid_test_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    amd64_read_low_or_mem x9, x13
    amd64_read_low_or_mem x10, x14
    and x15, x13, x14
    amd64_cached_mask_result x15, x11
    amd64_cached_set_logic_flags x15, x11
    gret 1

.gadget amd64_cached_test_reg_imm
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    and x9, x8, 0xf
    ubfx x11, x8, 4, 7
    amd64_cached_read x9, x13
    and x15, x13, x10
    amd64_cached_mask_result x15, x11
    amd64_cached_set_logic_flags x15, x11
    gret 2

.gadget amd64_cached_xor_zero_reg
    ldr x8, [_ip]
    and x10, x8, 0xf
    ubfx x11, x8, 4, 7
    mov x12, 0
    amd64_cached_write x10, x12, x11
    ldr w12, [_cpu, CPU_eflags]
    mov w13, (1 | PF_FLAG | AF_FLAG | SF_FLAG | (1 << 11))
    bic w12, w12, w13
    orr w12, w12, PF_FLAG
    orr w12, w12, ZF_FLAG
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w13, [_cpu, CPU_flags_res]
    bic w13, w13, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w13, [_cpu, CPU_flags_res]
    str w12, [_cpu, CPU_eflags]
    gret 1

.gadget amd64_mov64_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_amd64_regs
    ldr x12, [x11, x9, lsl 3]
    str x12, [x11, x10, lsl 3]
    gret 1

.gadget amd64_mov32_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_amd64_regs
    add x13, x11, x9, lsl 3
    ldr w12, [x13]
    str x12, [x11, x10, lsl 3]
    gret 1

.gadget amd64_mov16_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_amd64_regs
    add x13, x11, x9, lsl 3
    ldrh w12, [x13]
    add x13, x11, x10, lsl 3
    strh w12, [x13]
    gret 1

.gadget amd64_mov8_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_amd64_regs
    add x13, x11, x9, lsl 3
    ldrb w12, [x13]
    add x13, x11, x10, lsl 3
    strb w12, [x13]
    gret 1

.gadget amd64_mov_imm_reg
    ldr x8, [_ip]
    ldr x9, [_ip, 8]
    and x10, x8, 0xf
    ubfx x11, x8, 4, 7
    add x12, _cpu, CPU_amd64_regs
    add x12, x12, x10, lsl 3
    cmp x11, 64
    b.eq 1f
    cmp x11, 32
    b.eq 2f
    ldr x13, [x12]
    bfi x13, x9, 0, 16
    str x13, [x12]
    gret 2
1:
    str x9, [x12]
    gret 2
2:
    str x9, [x12]
    gret 2

.gadget amd64_mov_imm8_reg
    ldr x8, [_ip]
    ldr x9, [_ip, 8]
    and x10, x8, 0xf
    add x11, _cpu, CPU_amd64_regs
    add x11, x11, x10, lsl 3
    strb w9, [x11]
    gret 2

.gadget amd64_xchg_rax_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 7
    add x11, _cpu, CPU_amd64_regs
    ldr x12, [x11]
    ldr x13, [x11, x9, lsl 3]
    cmp x10, 64
    b.eq 1f
    cmp x10, 32
    b.eq 2f
    strh w13, [x11]
    add x14, x11, x9, lsl 3
    strh w12, [x14]
    gret 1
1:
    str x13, [x11]
    str x12, [x11, x9, lsl 3]
    gret 1
2:
    mov w12, w12
    mov w13, w13
    str x13, [x11]
    str x12, [x11, x9, lsl 3]
    gret 1

.gadget amd64_xchg_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    add x12, _cpu, CPU_amd64_regs
    add x13, x12, x9, lsl 3
    add x14, x12, x10, lsl 3
    cmp x11, 64
    b.eq 1f
    cmp x11, 32
    b.eq 2f
    cmp x11, 16
    b.eq 3f
    ldrb w15, [x13]
    ldrb w16, [x14]
    strb w16, [x13]
    strb w15, [x14]
    gret 1
1:
    ldr x15, [x13]
    ldr x16, [x14]
    str x16, [x13]
    str x15, [x14]
    gret 1
2:
    ldr w15, [x13]
    ldr w16, [x14]
    str x16, [x13]
    str x15, [x14]
    gret 1
3:
    ldrh w15, [x13]
    ldrh w16, [x14]
    strh w16, [x13]
    strh w15, [x14]
    gret 1

.gadget amd64_sign_extend
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 7
    add x11, _cpu, CPU_amd64_regs
    ldr x12, [x11]
    cmp x9, 0x99
    b.eq 3f
    cmp x10, 64
    b.eq 1f
    cmp x10, 32
    b.eq 2f
    sxtb w13, w12
    strh w13, [x11]
    gret 1
1:
    sxtw x12, w12
    str x12, [x11]
    gret 1
2:
    sxth w12, w12
    str x12, [x11]
    gret 1
3:
    cmp x10, 64
    b.eq 4f
    cmp x10, 32
    b.eq 5f
    sxth w13, w12
    asr w13, w13, 15
    strh w13, [x11, 16]
    gret 1
4:
    asr x12, x12, 63
    str x12, [x11, 16]
    gret 1
5:
    asr w12, w12, 31
    str x12, [x11, 16]
    gret 1

.gadget amd64_movx_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    add x13, _cpu, CPU_amd64_regs
    add x14, x13, x10, lsl 3
    cmp x9, 0xb7
    b.eq 2f
    cmp x9, 0xbf
    b.eq 3f
    ldrb w15, [x14]
    cmp x9, 0xbe
    b.ne 4f
    sxtb x15, w15
    b 4f
2:
    ldrh w15, [x14]
    b 4f
3:
    ldrsh x15, [x14]
4:
    cmp x12, 64
    b.eq 5f
    mov w15, w15
5:
    str x15, [x13, x11, lsl 3]
    gret 1

.gadget amd64_movsxd_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    add x12, _cpu, CPU_amd64_regs
    add x13, x12, x9, lsl 3
    cmp x11, 64
    b.eq 1f
    ldrsh w14, [x13]
    str x14, [x12, x10, lsl 3]
    gret 1
1:
    ldrsw x14, [x13]
    str x14, [x12, x10, lsl 3]
    gret 1

.gadget amd64_lea_reg_mem
    ldr x8, [_ip]
    ldr x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 8
    ubfx x12, x8, 20, 4
    add x13, _cpu, CPU_amd64_regs
    tbz x8, 30, 1f
    ldr x14, [x13, x12, lsl 3]
    add x9, x9, x14
1:
    tbz x8, 31, 2f
    ubfx x12, x8, 24, 4
    ubfx x15, x8, 28, 2
    ldr x14, [x13, x12, lsl 3]
    lsl x14, x14, x15
    add x9, x9, x14
2:
    tbz x8, 32, 3f
    ldr x14, [_ip, 16]
    add x9, x9, x14
3:
    cmp x11, 64
    b.eq 4f
    cmp x11, 32
    b.eq 5f
    add x12, x13, x10, lsl 3
    strh w9, [x12]
    gret 3
4:
    str x9, [x13, x10, lsl 3]
    gret 3
5:
    add x12, x13, x10, lsl 3
    mov w9, w9
    str x9, [x12]
    gret 3

.gadget amd64_cached_lea_reg_mem
    ldr x8, [_ip]
    ldr x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 8
    tbz x8, 30, 1f
    ubfx x0, x8, 20, 4
    amd64_read_low_or_mem x0, x14
    add x9, x9, x14
1:
    tbz x8, 31, 2f
    ubfx x0, x8, 24, 4
    ubfx x15, x8, 28, 2
    amd64_read_low_or_mem x0, x14
    lsl x14, x14, x15
    add x9, x9, x14
2:
    tbz x8, 32, 3f
    ldr x14, [_ip, 16]
    add x9, x9, x14
3:
    amd64_write_low_or_mem x10, x9, x11
    gret 3

.gadget amd64_bswap_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 7
    add x11, _cpu, CPU_amd64_regs
    ldr x12, [x11, x9, lsl 3]
    cmp x10, 64
    b.eq 1f
    rev w12, w12
    str x12, [x11, x9, lsl 3]
    gret 1
1:
    rev x12, x12
    str x12, [x11, x9, lsl 3]
    gret 1

.gadget amd64_test_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    add x12, _cpu, CPU_amd64_regs
    ldr x13, [x12, x9, lsl 3]
    ldr x14, [x12, x10, lsl 3]
    and x15, x13, x14
    cmp x11, 64
    b.eq 1f
    cmp x11, 32
    b.eq 2f
    cmp x11, 16
    b.eq 4f
    and w15, w15, 0xff
    b 3f
4:
    uxth w15, w15
    b 3f
2:
    mov w15, w15
3:
1:
    ldr w16, [_cpu, CPU_eflags]
    mov w17, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w16, w16, w17
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w17, [_cpu, CPU_flags_res]
    bic w17, w17, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w17, [_cpu, CPU_flags_res]
    cmp x15, 0
    cset w17, eq
    orr w16, w16, w17, lsl 6
    and w17, w15, 0xff
    eor w17, w17, w17, lsr 4
    eor w17, w17, w17, lsr 2
    eor w17, w17, w17, lsr 1
    mvn w17, w17
    and w17, w17, 1
    orr w16, w16, w17, lsl 2
    cmp x11, 64
    b.eq 4f
    cmp x11, 32
    b.eq 5f
    cmp x11, 16
    b.eq 8f
    tst w15, 0x80
    b.eq 6f
    orr w16, w16, SF_FLAG
    b 6f
8:
    tst w15, 0x8000
    b.eq 6f
    orr w16, w16, SF_FLAG
    b 6f
5:
    tbnz w15, 31, 7f
    b 6f
4:
    tbnz x15, 63, 7f
    b 6f
7:
    orr w16, w16, SF_FLAG
6:
    str w16, [_cpu, CPU_eflags]
    gret 1

.gadget amd64_test_reg_imm
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    and x9, x8, 0xf
    ubfx x11, x8, 4, 7
    add x12, _cpu, CPU_amd64_regs
    ldr x13, [x12, x9, lsl 3]
    and x15, x13, x10
    cmp x11, 64
    b.eq 1f
    cmp x11, 32
    b.eq 2f
    cmp x11, 16
    b.eq 4f
    and w15, w15, 0xff
    b 3f
4:
    uxth w15, w15
    b 3f
2:
    mov w15, w15
3:
1:
    ldr w16, [_cpu, CPU_eflags]
    mov w17, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w16, w16, w17
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w17, [_cpu, CPU_flags_res]
    bic w17, w17, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w17, [_cpu, CPU_flags_res]
    cmp x15, 0
    cset w17, eq
    orr w16, w16, w17, lsl 6
    and w17, w15, 0xff
    eor w17, w17, w17, lsr 4
    eor w17, w17, w17, lsr 2
    eor w17, w17, w17, lsr 1
    mvn w17, w17
    and w17, w17, 1
    orr w16, w16, w17, lsl 2
    cmp x11, 64
    b.eq 4f
    cmp x11, 32
    b.eq 5f
    cmp x11, 16
    b.eq 8f
    tst w15, 0x80
    b.eq 6f
    orr w16, w16, SF_FLAG
    b 6f
8:
    tst w15, 0x8000
    b.eq 6f
    orr w16, w16, SF_FLAG
    b 6f
5:
    tbnz w15, 31, 7f
    b 6f
4:
    tbnz x15, 63, 7f
    b 6f
7:
    orr w16, w16, SF_FLAG
6:
    str w16, [_cpu, CPU_eflags]
    gret 2

.gadget amd64_logic_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    tbnz x9, 1, 1f
    mov x14, x11
    mov x15, x10
    b 2f
1:
    mov x14, x10
    mov x15, x11
2:
    add x13, _cpu, CPU_amd64_regs
    ldr x16, [x13, x14, lsl 3]
    ldr x17, [x13, x15, lsl 3]
    cmp x9, 0x20
    b.hs 3f
    orr x15, x16, x17
    b 5f
3:
    cmp x9, 0x30
    b.hs 4f
    and x15, x16, x17
    b 5f
4:
    eor x15, x16, x17
5:
    add x14, x13, x14, lsl 3
    cmp x12, 64
    b.eq 6f
    cmp x12, 32
    b.eq 7f
    cmp x12, 16
    b.eq 8f
    strb w15, [x14]
    and w15, w15, 0xff
    b 9f
8:
    strh w15, [x14]
    uxth w15, w15
    b 9f
7:
    mov w15, w15
    str x15, [x14]
    b 9f
6:
    str x15, [x14]
9:
    ldr w16, [_cpu, CPU_eflags]
    mov w17, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w16, w16, w17
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w17, [_cpu, CPU_flags_res]
    bic w17, w17, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w17, [_cpu, CPU_flags_res]
    cmp x15, 0
    cset w17, eq
    orr w16, w16, w17, lsl 6
    and w17, w15, 0xff
    eor w17, w17, w17, lsr 4
    eor w17, w17, w17, lsr 2
    eor w17, w17, w17, lsr 1
    mvn w17, w17
    and w17, w17, 1
    orr w16, w16, w17, lsl 2
    cmp x12, 64
    b.eq 10f
    cmp x12, 32
    b.eq 11f
    cmp x12, 16
    b.eq 12f
    tst w15, 0x80
    b.eq 13f
    orr w16, w16, SF_FLAG
    b 13f
12:
    tst w15, 0x8000
    b.eq 13f
    orr w16, w16, SF_FLAG
    b 13f
11:
    tbnz w15, 31, 14f
    b 13f
10:
    tbnz x15, 63, 14f
    b 13f
14:
    orr w16, w16, SF_FLAG
13:
    str w16, [_cpu, CPU_eflags]
    gret 1

.gadget amd64_logic_reg_imm
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    add x13, _cpu, CPU_amd64_regs
    add x14, x13, x11, lsl 3
    ldr x15, [x14]
    cmp x9, 4
    b.eq 1f
    cmp x9, 6
    b.eq 2f
    orr x15, x15, x10
    b 3f
1:
    and x15, x15, x10
    b 3f
2:
    eor x15, x15, x10
3:
    cmp x12, 64
    b.eq 4f
    cmp x12, 32
    b.eq 5f
    cmp x12, 16
    b.eq 6f
    strb w15, [x14]
    and w15, w15, 0xff
    b 7f
6:
    strh w15, [x14]
    uxth w15, w15
    b 7f
5:
    mov w15, w15
    str x15, [x14]
    b 7f
4:
    str x15, [x14]
7:
    ldr w16, [_cpu, CPU_eflags]
    mov w17, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w16, w16, w17
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w17, [_cpu, CPU_flags_res]
    bic w17, w17, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w17, [_cpu, CPU_flags_res]
    cmp x15, 0
    cset w17, eq
    orr w16, w16, w17, lsl 6
    and w17, w15, 0xff
    eor w17, w17, w17, lsr 4
    eor w17, w17, w17, lsr 2
    eor w17, w17, w17, lsr 1
    mvn w17, w17
    and w17, w17, 1
    orr w16, w16, w17, lsl 2
    cmp x12, 64
    b.eq 8f
    cmp x12, 32
    b.eq 9f
    cmp x12, 16
    b.eq 10f
    tst w15, 0x80
    b.eq 11f
    orr w16, w16, SF_FLAG
    b 11f
10:
    tst w15, 0x8000
    b.eq 11f
    orr w16, w16, SF_FLAG
    b 11f
9:
    tbnz w15, 31, 12f
    b 11f
8:
    tbnz x15, 63, 12f
    b 11f
12:
    orr w16, w16, SF_FLAG
11:
    str w16, [_cpu, CPU_eflags]
    gret 2

.gadget amd64_xor_zero_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 7
    add x11, _cpu, CPU_amd64_regs
    add x11, x11, x9, lsl 3
    cmp x10, 64
    b.eq 1f
    cmp x10, 32
    b.eq 2f
    strh wzr, [x11]
    b 3f
1:
    str xzr, [x11]
    b 3f
2:
    str xzr, [x11]
3:
    ldr w12, [_cpu, CPU_eflags]
    mov w13, (1 | PF_FLAG | AF_FLAG | SF_FLAG | (1 << 11))
    bic w12, w12, w13
    orr w12, w12, PF_FLAG
    orr w12, w12, ZF_FLAG
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w13, [_cpu, CPU_flags_res]
    bic w13, w13, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w13, [_cpu, CPU_flags_res]
    str w12, [_cpu, CPU_eflags]
    gret 1

.macro mov32_reg_reg dst_name, dst, src_name, src
    .gadget mov32_reg_reg_\dst_name\()_\src_name
        mov \dst, \src
        gret
.endm

.macro mov32_reg_reg_row dst_name, dst
    .macro mov32_reg_reg_col src_name, src
        mov32_reg_reg \dst_name, \dst, \src_name, \src
    .endm
    .each_reg mov32_reg_reg_col
    .purgem mov32_reg_reg_col
.endm

.each_reg mov32_reg_reg_row

.pushsection_rodata
.global.name mov32_reg_reg_gadgets
.irp dst, REG_LIST
    .irp src, REG_LIST
        .quad NAME(gadget_mov32_reg_reg_\dst\()_\src)
    .endr
.endr
.popsection

# this would have been just a few nice compact nested loops, but gas said "nuh uh"

.macro _do_op op, arg, size, s
    .ifc \op,load
        movs _tmp, \arg, \s
        uxts _tmp, _tmp, \s
        .exitm
    .else N .ifc \op,store
        movs \arg, _tmp, \s
        .exitm
    .endif N .endif

    .ifin(\op, add,sub,adc,sbc)
        setf_a \arg, _tmp, \s
    .endifin
    .ifin(\op, and,orr,eor)
        clearf_a
        clearf_oc
    .endifin
    .ifin(\op, adc,sbc)
        ldrb w10, [_cpu, CPU_cf]
        .ifc \op,adc
            cmp w10, 1
        .else
            mvn w10, w10
            cmn w10, 1
        .endif
    .endifin

    .ifin(\op, and,orr,eor)
        \op _tmp, _tmp, \arg
    .endifin

    .ifin(\op, add,sub,adc,sbc)
        do_add \op, _tmp, \arg, \s
    .endifin

    .ifc \op,imul
        .ifnb \s
            sxt\s w10, \arg
            mul _tmp, _tmp, w10
            cmp _tmp, _tmp, sxt\s
        .else
            smull _xtmp, _tmp, \arg
            cmp _xtmp, _tmp, sxtw
        .endif
        cset w10, ne
        strb w10, [_cpu, CPU_cf]
        strb w10, [_cpu, CPU_of]
    .endif

    .ifin(\op, bsf,bsr)
        .ifnb \s
            uxt\s w10, \arg
        .else
            mov w10, \arg
        .endif
        .ifc \op,bsf
            .if \size != 32
                orr w10, w10, 1<<\size
            .endif
            rbit w10, w10
            clz w10, w10
            cmp w10, \size
        .else
            clz w10, w10
            .if \size != 32
                sub w10, w10, 32-\size
            .endif
            cmp w10, \size
            mov w9, \size-1
            sub w10, w9, w10
        .endif
        csel _tmp, w10, _tmp, ne
        cset w10, eq
        ldrb w9, [_cpu, CPU_eflags]
        bic w9, w9, ZF_FLAG
        orr w9, w9, w10, lsl 6
        strb w9, [_cpu, CPU_eflags]
        ldrb w9, [_cpu, CPU_flags_res]
        bic w9, w9, ZF_RES
        strb w9, [_cpu, CPU_flags_res]
    .endifin

    .ifc \op,xchg
        mov w9, _tmp
        mov _tmp, \arg
        movs \arg, w9, \s
    .endif

    .ifin(\op, add,sub,adc,sbc,and,orr,eor)
        setf_zsp \s
    .endifin
.endm
.macro do_op op, size, arg
    ss \size, _do_op, \op, \arg
.endm

.macro do_reg_op op, armop, size, reg
    .gadget \op\size\()_reg_\reg
        do_op \armop, \size, e\reg\()x
        gret
.endm

.macro do_hi_op op, size, reg
    ubfx w12, e\reg\()x, 8, 8
    do_op \op, \size, w12
    bfi e\reg\()x, w12, 8, 8
.endm

.macro do_op_size op, armop, size, s
    .ifnc \op,store
        .gadget \op\size\()_imm
            ldr\s w8, [_ip]
            do_op \armop, \size, w8
            gret 1
    .endif

    .ifnc \op,xchg
        .gadget \op\size\()_mem
            .ifc \op,store
                write_prep \size, \op\size\()_mem
            .else N .ifc \op,xchg
                write_prep \size, \op\size\()_mem
            .else
                read_prep \size, \op\size\()_mem
            .endif N .endif
            ldr\s w8, [_xaddr]
            do_op \armop, \size, w8
            .ifc \op,store
                str\s w8, [_xaddr]
                write_done \size, \op\size\()_mem
            .endif
            gret 1
            .ifc \op,store
                write_bullshit \size, \op\size\()_mem
            .else N .ifc \op,xchg
                write_bullshit \size, \op\size\()_mem
            .else
                read_bullshit \size, \op\size\()_mem
            .endif N .endif
    .else
        # xchg must be atomic
        .gadget \op\size\()_mem
            write_prep \size, \op\size\()_mem
            dmb ish
        1:
            ldaxr\s w8, [_xaddr]
            stlxr\s w10, _tmp, [_xaddr]
            cbnz w10, 1b
            movs _tmp, w8
            dmb ish
            write_done \size, \op\size\()_mem
            gret 1
            write_bullshit \size, \op\size\()_mem
    .endif

    .irp reg, a,b,c,d
        do_reg_op \op, \armop, \size, \reg
    .endr

    .irp reg, si,di,sp,bp
        .gadget \op\size\()_reg_\reg
            .if \size == 8
                .ifc \reg,sp N do_hi_op \armop, \size, a N .else
                .ifc \reg,bp N do_hi_op \armop, \size, c N .else
                .ifc \reg,si N do_hi_op \armop, \size, d N .else
                .ifc \reg,di N do_hi_op \armop, \size, b
                .endif N .endif N .endif N .endif
            .else
                do_op \armop, \size, e\reg
            .endif
            gret
    .endr

.endm

.irp op, load,store,xchg,add,sub,adc,sbb,and,or,xor
    .irp size, SIZE_LIST
        # a couple operations have slightly different names on arm
        .ifc \op,xor
            ss \size, do_op_size, \op, eor
        .else N .ifc \op,sbb
            ss \size, do_op_size, \op, sbc
        .else N .ifc \op,or
            ss \size, do_op_size, \op, orr
        .else
            ss \size, do_op_size, \op, \op
        .endif N .endif N .endif
    .endr
    .gadget_array \op
.endr
.irp op, imul,bsf,bsr
    .irp size, 16,32
        ss \size, do_op_size, \op, \op
    .endr
    .gadget_array \op
.endr

# atomics. oof

.macro do_op_size_atomic opname, op, size, s
    .gadget atomic_\opname\size\()_mem
        # There's so much stuff going on inside most of these operations that
        # the implementation is a compare-and-swap loop, instead of just ldaxr/stlxr
        write_prep \size, atomic_\opname\size\()_mem
        .if \size != 8
            tst _addr, (\size/8)-1
            b.eq 9f
            save_c
            mov x0, _cpu
            mov w1, _addr
            .if \size == 16
                mov w2, 3
            .else
                mov w2, 4
            .endif
            bl NAME(helper_trace_unaligned_atomic)
            restore_c
        9:
        .endif
        dmb ish
        ldr\s w12, [_xaddr]
    1:
        mov w8, w12

        # do the operation
        # dest = w8, src = _tmp
        .ifin(\op, add,sub,adc,sbc)
            setf_a src=_tmp, dst=w8, s=\s
        .endifin
        .ifin(\op, and,orr,eor)
            clearf_a
            clearf_oc
        .endifin
        .ifin(\op, adc,sbc)
            ldrb w10, [_cpu, CPU_cf]
            .ifc \op,adc
                cmp w10, 1
            .else
                mvn w10, w10
                cmn w10, 1
            .endif
        .endifin

        .ifin(\op, and,orr,eor)
            \op w8, w8, _tmp
        .endifin
        .ifin(\op, add,sub,adc,sbc)
            do_add \op, w8, _tmp, \s
        .endifin
        .ifc \op,xadd
            # exchange, then add
            mov w9, w8
            mov w8, _tmp
            setf_a src=w9, dst=w8, s=\s
            do_add add, w8, w9, \s
        .endif

        .ifin(\op, add,sub,adc,sbc,and,orr,eor,xadd)
            setf_zsp \s, val=w8
        .endifin

        .ifin(\op, inc,dec)
            mov w10, 1
            setf_a src=w10, dst=w8, s=\s
            .ifb \s
                .ifc \op,inc
                    adds w8, w8, 1
                .else
                    subs w8, w8, 1
                .endif
                cset w9, vs
            .else
                sxt\s w8, w8
                .ifc \op,inc
                    adds w8, w8, 1
                .else
                    subs w8, w8, 1
                .endif
                cmp w8, w8, sxt\s
                cset w9, ne
            .endif
            strb w9, [_cpu, CPU_of]
            setf_zsp \s, val=w8
        .endifin

    2:
        ldaxr\s w13, [_xaddr]
        cmp w12, w13
        b.ne 3f
        stlxr\s w13, w8, [_xaddr]
        cbnz w13, 2b
        .ifc \op,xadd
            mov _tmp, w9
        .endif
        dmb ish
        write_done \size, atomic_\opname\size\()_mem
        gret 1
        write_bullshit \size, atomic_\opname\size\()_mem
    3:
        dmb ish
        mov w12, w13
        b 1b
.endm

.irp op, add,sub,adc,sbb,and,or,xor,inc,dec,xadd
    .irp size, SIZE_LIST
        .ifc \op,xor
            ss \size, do_op_size_atomic, \op, eor
        .else N .ifc \op,sbb
            ss \size, do_op_size_atomic, \op, sbc
        .else N .ifc \op,or
            ss \size, do_op_size_atomic, \op, orr
        .else
            ss \size, do_op_size_atomic, \op, \op
        .endif N .endif N .endif
    .endr
    .gadget_array atomic_\op
.endr

# unary operations (well, only one explicit operand)

.macro do_inc size, s
    mov w10, 1
    setf_a w10, _tmp, \s
    .ifb \s
        adds _tmp, _tmp, 1
        cset w8, vs
    .else
        sxt\s _tmp, _tmp
        add _tmp, _tmp, 1
        cmp _tmp, _tmp, sxt\s
        cset w8, ne
    .endif
    strb w8, [_cpu, CPU_of]
    setf_zsp \s
.endm
.macro do_dec size, s
    mov w10, 1
    setf_a w10, _tmp, \s
    .ifb \s
        subs _tmp, _tmp, 1
        cset w8, vs
    .else
        sxt\s _tmp, _tmp
        sub _tmp, _tmp, 1
        cmp _tmp, _tmp, sxt\s
        cset w8, ne
    .endif
    strb w8, [_cpu, CPU_of]
    setf_zsp \s
.endm

.macro do_sign_extend size, s
    .if \size != 32
        # movs\ss\()l %tmp\s, %tmpd
        sxt\s _tmp, _tmp
    .endif
.endm
.macro do_zero_extend size, s
    .if \size != 32
        uxt\s _tmp, _tmp
    .endif
.endm
.macro do_div size, s
    .if \size == 8
        uxth w8, eax
        uxtb _tmp, _tmp
        udiv w9, w8, _tmp
        msub w10, w9, _tmp, w8
        bfi eax, w9, 0, 8
        bfi eax, w10, 8, 8
    .elseif \size == 16
        bfi w8, eax, 0, 16
        bfi w8, edx, 16, 16
        uxth _tmp, _tmp
        udiv w9, w8, _tmp
        msub w10, w9, _tmp, w8
        bfi eax, w9, 0, 16
        bfi edx, w10, 0, 16
    .elseif \size == 32
        bfi x8, xax, 0, 32
        bfi x8, xdx, 32, 32
        uxtw _xtmp, _tmp
        udiv x9, x8, _xtmp
        msub x10, x9, _xtmp, x8
        mov eax, w9
        mov edx, w10
    .endif
.endm
.macro do_idiv size, s
    # another lazy ass copy paste job
    .if \size == 8
        sxth w8, eax
        sxtb _tmp, _tmp
        sdiv w9, w8, _tmp
        msub w10, w9, _tmp, w8
        bfi eax, w9, 0, 8
        bfi eax, w10, 8, 8
    .elseif \size == 16
        bfi w8, eax, 0, 16
        bfi w8, edx, 16, 16
        sxth _tmp, _tmp
        sdiv w9, w8, _tmp
        msub w10, w9, _tmp, w8
        bfi eax, w9, 0, 16
        bfi edx, w10, 0, 16
    .elseif \size == 32
        bfi x8, xax, 0, 32
        bfi x8, xdx, 32, 32
        sxtw _xtmp, _tmp
        sdiv x9, x8, _xtmp
        msub x10, x9, _xtmp, x8
        mov eax, w9
        mov edx, w10
    .endif
.endm
.macro do_mul size, s
    .ifb \s
        umull xax, eax, _tmp
        lsr xdx, xax, 32
        cmp xax, eax, uxtw
    .else
        uxt\s w8, eax
        uxt\s _tmp, _tmp
        mul w8, w8, _tmp
        cmp w8, w8, uxt\s
        .if \size == 8
            bfxil eax, w8, 0, \size*2
        .else
            bfxil eax, w8, 0, \size
            bfxil edx, w8, \size, \size
        .endif
    .endif
    cset w8, ne
    strb w8, [_cpu, CPU_cf]
    strb w8, [_cpu, CPU_of]
.endm
.macro do_imul1 size, s
    .ifb \s
        smull xax, eax, _tmp
        lsr xdx, xax, 32
        cmp xax, eax, sxtw
    .else
        sxt\s w8, eax
        sxt\s _tmp, _tmp
        mul w8, w8, _tmp
        cmp w8, w8, sxt\s
        .if \size == 8
            bfxil eax, w8, 0, \size*2
        .else
            bfxil eax, w8, 0, \size
            bfxil edx, w8, \size, \size
        .endif
    .endif
    cset w8, ne
    strb w8, [_cpu, CPU_cf]
    strb w8, [_cpu, CPU_of]
.endm
.macro do_not size, s
    .ifb \s
        mvn _tmp, _tmp
    .else
        movs w10, _tmp, \s
        mvn w10, w10
        movs _tmp, w10, \s
    .endif
.endm

.irp op, inc,dec,sign_extend,zero_extend,div,idiv,mul,imul1,not
    .irp size, SIZE_LIST
        .gadget \op\()_\size
            ss \size, do_\op
            gret
    .endr
    .gadget_list \op, SIZE_LIST
.endr

.gadget cvt_16
    tst eax, 0x8000
    cinv w8, wzr, ne
    bfxil edx, w8, 0, 16
    gret
.gadget cvt_32
    tst eax, 0x80000000
    cinv edx, wzr, ne
    gret
.gadget_list cvt, SIZE_LIST

.gadget cvte_16
    sxtb w8, eax
    bfxil eax, w8, 0, 16
    gret
.gadget cvte_32
    sxth eax, eax
    gret
.gadget_list cvte, SIZE_LIST

# ---------------------------------------------------------------------------
# amd64 native SSE register-register gadgets.
#
# These replace the per-op C bridge (amd64_jit_0f_vec_rm) for the hottest
# register-register vector ops. They touch only cpu->xmm[] (and the operand
# word), never the GPR reg-cache (x20-x27) or _ip/_cpu/_tlb, so gen.c does NOT
# flush the reg cache around them. No memory operand => no TLB/prep => x8-x15
# are all free scratch (same as the amd64_*_reg_reg GPR gadgets above).
#
# Operand word layout (one gadget slot): src xmm id in bits 0-3, dst xmm id in
# bits 4-7. cpu->xmm[i] lives at CPU_xmm + i*16.
# ---------------------------------------------------------------------------

# movdqa/movdqu xmm1, xmm2 (66/F3 0F 6F, mod==3): cpu->xmm[dst] = cpu->xmm[src]
.gadget amd64_v_mov128_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_xmm
    add x9, x11, x9, lsl 4
    add x10, x11, x10, lsl 4
    ldp x12, x13, [x9]
    stp x12, x13, [x10]
    gret 1

# Register-register binary op over the two 64-bit lanes of an xmm:
# cpu->xmm[dst] = cpu->xmm[dst] OP cpu->xmm[src]. 64-bit-lane integer ops
# (paddq/psubq) and the 128-bit bitwise ops (pxor/pand/por/pandn) all reduce to
# two independent 64-bit ops on the lane pair. \rev swaps the operands so pandn
# (result = ~dst & src) maps to bic Rd, src, dst.
.macro amd64_v_lane64_binop name, insn, rev=0
    .gadget amd64_v_\name\()_reg
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 4, 4
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        add x10, x11, x10, lsl 4
        ldp x12, x13, [x10]
        ldp x14, x15, [x9]
        .if \rev
            \insn x12, x14, x12
            \insn x13, x15, x13
        .else
            \insn x12, x12, x14
            \insn x13, x13, x15
        .endif
        stp x12, x13, [x10]
        gret 1
.endm

# 66 0F D4 paddq
amd64_v_lane64_binop paddq, add
# 66 0F FB psubq
amd64_v_lane64_binop psubq, sub
# 0F 57 xorps / 66 0F 57 xorpd: 128-bit bitwise XOR (two 64-bit lanes)
amd64_v_lane64_binop pxor, eor

# psrlq/psllq xmm, imm8 (66 0F 73 /2,/6): shift each 64-bit lane by an imm baked
# into bits 8-15 of the operand word; rm xmm id in bits 0-3 is both source and
# dest. A shift count > 63 yields all-zero lanes (the variable lsr/lsl would
# otherwise wrap mod 64), selected branchlessly: lsr/lsl leave NZCV from the cmp
# intact, so csel zeroes the lanes when count > 63.
.macro amd64_v_shift_q_imm name, shift
    .gadget amd64_v_\name\()_imm
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 8, 8
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        ldp x12, x13, [x9]
        cmp x10, 63
        \shift x12, x12, x10
        \shift x13, x13, x10
        csel x12, xzr, x12, hi
        csel x13, xzr, x13, hi
        stp x12, x13, [x9]
        gret 1
.endm

# 66 0F 73 /2 ib psrlq
amd64_v_shift_q_imm psrlq, lsr
# 66 0F 73 /6 ib psllq
amd64_v_shift_q_imm psllq, lsl

# Scalar-double SSE2 (F2 0F 58 addsd / 59 mulsd), reg-reg: operate on the low
# 64-bit lane only, dst.f64[0] = dst.f64[0] OP src.f64[0]; dst's high 64 bits are
# preserved (the 8-byte str writes only the low lane). aarch64 C `double` math
# lowers to these same fadd/fmul, so this is bit-identical to the interpreter.
# (First NEON use in a gadget: d0/d1 are caller-saved, loaded fresh from cpu->xmm
# each time and dead after gret, and no C call is made, so they are free scratch.)
.macro amd64_v_scalar_double name, fpop
    .gadget amd64_v_\name\()_reg
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 4, 4
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        add x10, x11, x10, lsl 4
        ldr d0, [x10]
        ldr d1, [x9]
        \fpop d0, d0, d1
        str d0, [x10]
        gret 1
.endm

# F2 0F 58 addsd
amd64_v_scalar_double addsd, fadd
# F2 0F 59 mulsd
amd64_v_scalar_double mulsd, fmul

# cvtsi2sd xmm, r/m (F2 0F 2A), reg-reg: xmm[dst].f64[0] = (double) signed GPR[src].
# Operand: GPR src id bits 0-3, xmm dst id bits 4-7, REX.W in bit 8 (1=64-bit
# source, 0=32-bit). gen.c flushes the reg cache first so cpu->amd64_regs is
# current, then this reads the source straight from memory. scvtf matches the
# interp's C (double) cast bit-for-bit. Only the low 64 bits of dst are written.
.gadget amd64_v_cvtsi2sd_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x12, _cpu, CPU_amd64_regs
    add x12, x12, x9, lsl 3
    tbz x8, 8, 1f
    ldr x12, [x12]
    scvtf d0, x12
    b 2f
1:  ldr w12, [x12]
    scvtf d0, w12
2:  add x13, _cpu, CPU_xmm
    add x13, x13, x10, lsl 4
    str d0, [x13]
    gret 1

# ---------------------------------------------------------------------------
# Native amd64 SSE memory load/store.
#
# Unlike the i386 SSE path, amd64 guest addresses are 47-bit, so the 32-bit
# _addr(w3)-based read_prep/handlers cannot be used. These gadgets compute the
# full 64-bit effective address (reusing the meta/disp encoding produced by
# gen_amd64_decode_mem_meta, same bit layout the LEA gadget uses) and resolve it
# through a 64-bit TLB fast path, falling to the C slow helpers (which already
# take a 64-bit guest_addr_t) on miss/cross-page, and to a controlled INT_PF
# exit on a real fault.
#
# Contract from gen.c: the reg cache is FLUSHED first (so cpu->amd64_regs is
# current for the address read AND correct for fault re-execution, since save_regs
# only mirrors the cache to the i386 slots), and the rip is FLUSHED first (so
# cpu->amd64_rip already points at this instruction for a fault). Operands:
#   [_ip,0]=meta  [_ip,8]=disp  [_ip,16]=next_ip (rip-relative addend).  gret 3.
# Register use: x9=address (-> host ptr), x10=xmm id (survives via save_c),
# x8/x11/x12/x13/x14/x15/x0 scratch; never touches x20-x27 (the flushed cache).
# ---------------------------------------------------------------------------

# 64-bit TLB read fast path: x9 = guest addr in -> host ptr out (or miss/xpage).
.macro amd64_vread_prep size, id
    and  x8, x9, 0xfff
    cmp  x8, (0x1000-\size)
    b.hi amd64_vxpage_\id
    and  x8, x9, #0xfffffffffffff000
    ubfx x11, x9, 12, 10
    eor  x11, x11, x9, lsr 22
    and  x11, x11, 0x3ff
    mov  w12, TLB_ENTRY_SIZE
    madd x11, x11, x12, _tlb
    ldr  x13, [x11, TLB_ENTRY_page]
    cmp  x8, x13
    b.ne amd64_vmiss_\id
    ldr  x13, [x11, TLB_ENTRY_data_minus_addr]
    add  x9, x13, x9
amd64_vback_\id:
.endm

.macro amd64_vread_bullshit size, id
amd64_vmiss_\id:
    bl amd64_vmem_read_miss
    b amd64_vback_\id
amd64_vxpage_\id:
    mov x19, \size
    bl amd64_vmem_crosspage_read
    b amd64_vback_\id
.endm

.global amd64_vmem_read_miss
amd64_vmem_read_miss:
    save_c
    sub x0, _tlb, TLB_entries
    mov x1, x9
    mov x2, xzr
    bl NAME(tlb_handle_miss)
    mov x19, x0
    restore_c
    cbz x19, amd64_vmem_segfault_read
    mov x9, x19
    ret

.global amd64_vmem_crosspage_read
amd64_vmem_crosspage_read:
    save_c
    sub x0, _tlb, TLB_entries
    add x2, _cpu, LOCAL_value
    mov x1, x9
    mov x3, x19
    bl NAME(__tlb_read_cross_page)
    mov x19, x0
    restore_c
    cbz x19, amd64_vmem_segfault_read
    add x9, _cpu, LOCAL_value
    ret

.global amd64_vmem_segfault_read
amd64_vmem_segfault_read:
    ldr x8, [_tlb, (-TLB_entries+TLB_segfault_addr)]
    str x8, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    mov x0, INT_PF
    b jit_exit

# Compute the 64-bit effective address from meta(x8)/disp(x9) into x9. Reads base
# and index from cpu->amd64_regs in memory (the reg cache was flushed by gen.c).
.macro amd64_vmem_addr
    tbz  x8, 30, 7001f
    ubfx x0, x8, 20, 4
    add  x14, _cpu, CPU_amd64_regs
    ldr  x14, [x14, x0, lsl 3]
    add  x9, x9, x14
7001:
    tbz  x8, 31, 7002f
    ubfx x0, x8, 24, 4
    ubfx x15, x8, 28, 2
    add  x14, _cpu, CPU_amd64_regs
    ldr  x14, [x14, x0, lsl 3]
    lsl  x14, x14, x15
    add  x9, x9, x14
7002:
    tbz  x8, 32, 7003f
    ldr  x14, [_ip, 16]
    add  x9, x9, x14
7003:
.endm

# movaps/movups xmm, m128 (0F 28 / 0F 10): cpu->xmm[reg] = [addr]
.gadget amd64_v_load128_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 16, load128
    ldp  x12, x13, [x9]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    stp  x12, x13, [x14]
    gret 3
    amd64_vread_bullshit 16, load128

# 64-bit TLB write fast path: x9 = guest addr in -> writable host ptr out (or the
# cross-page staging buffer, flushed by amd64_vwrite_done). Mirrors the 32-bit
# write_prep: a TLB-staleness / host-page-mirroring check routes to the slow C
# resolver; the hit path checks page_if_writable and records the dirty page.
.macro amd64_vwrite_prep size, id
    and  x8, x9, 0xfff
    cmp  x8, (0x1000-\size)
    b.hi amd64_vwxpage_\id
    ldr  x11, [_tlb, (-TLB_entries+TLB_mmu)]
    ldr  x8,  [_tlb, (-TLB_entries+TLB_mem_changes)]
    ldr  x12, [x11, MMU_changes]
    cmp  x8, x12
    b.ne amd64_vwslow_\id
    ldrb w12, [x11, MMU_requires_write_revalidate]
    cbz  w12, amd64_vwfast_\id
amd64_vwslow_\id:
    bl   amd64_vmem_write_resolve
    b    amd64_vwback_\id
amd64_vwfast_\id:
    and  x8, x9, #0xfffffffffffff000
    str  x8, [_tlb, (-TLB_entries+TLB_dirty_page)]
    ubfx x11, x9, 12, 10
    eor  x11, x11, x9, lsr 22
    and  x11, x11, 0x3ff
    mov  w12, TLB_ENTRY_SIZE
    madd x11, x11, x12, _tlb
    ldr  x13, [x11, TLB_ENTRY_page_if_writable]
    cmp  x8, x13
    b.ne amd64_vwmiss_\id
    ldr  x13, [x11, TLB_ENTRY_data_minus_addr]
    add  x9, x13, x9
amd64_vwback_\id:
.endm

# After the store, if x9 was the cross-page staging buffer, flush it to memory.
.macro amd64_vwrite_done size, id
    add  x8, _cpu, LOCAL_value
    cmp  x8, x9
    b.ne amd64_vwdone_\id
    mov  x19, \size
    bl   amd64_vmem_crosspage_write
amd64_vwdone_\id:
.endm

.macro amd64_vwrite_bullshit size, id
amd64_vwmiss_\id:
    bl   amd64_vmem_write_miss
    b    amd64_vwback_\id
amd64_vwxpage_\id:
    str  x9, [_cpu, LOCAL_value_addr]
    add  x9, _cpu, LOCAL_value
    b    amd64_vwback_\id
.endm

.global amd64_vmem_write_resolve
amd64_vmem_write_resolve:
    save_c
    sub  x0, _tlb, TLB_entries
    mov  x1, x9
    bl   NAME(tlb_write_ptr_slow)
    mov  x19, x0
    restore_c
    cbz  x19, amd64_vmem_segfault_write
    mov  x9, x19
    ret

.global amd64_vmem_write_miss
amd64_vmem_write_miss:
    save_c
    sub  x0, _tlb, TLB_entries
    mov  x1, x9
    mov  x2, 1
    bl   NAME(tlb_handle_miss)
    mov  x19, x0
    restore_c
    cbz  x19, amd64_vmem_segfault_write
    mov  x9, x19
    ret

.global amd64_vmem_crosspage_write
amd64_vmem_crosspage_write:
    save_c
    sub  x0, _tlb, TLB_entries
    add  x2, _cpu, LOCAL_value
    ldr  x1, [_cpu, LOCAL_value_addr]
    mov  x3, x19
    bl   NAME(__tlb_write_cross_page)
    mov  x19, x0
    restore_c
    cbz  x19, amd64_vmem_segfault_write
    ret

.global amd64_vmem_segfault_write
amd64_vmem_segfault_write:
    ldr  x8, [_tlb, (-TLB_entries+TLB_segfault_addr)]
    str  x8, [_cpu, CPU_segfault_addr]
    mov  w8, 1
    strb w8, [_cpu, CPU_segfault_was_write]
    mov  x0, INT_PF
    b    jit_exit

# movaps/movups m128, xmm (0F 29 / 0F 11): [addr] = cpu->xmm[reg]
.gadget amd64_v_store128_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vwrite_prep 16, store128
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldp  x12, x13, [x14]
    stp  x12, x13, [x9]
    amd64_vwrite_done 16, store128
    gret 3
    amd64_vwrite_bullshit 16, store128

# amd64 push reg (0x50+r; REX.B selects r8-r15): always a 64-bit stack push in
# long mode -- the 0x66 operand-size form is excluded by gen.c. The reg cache and
# rip are flushed before this gadget, so guest registers live in CPU_amd64_regs
# (memory) and a page fault re-executes the instruction. RSP is regs[4].
# Operand 0: source register number (0-15). `push rsp` pushes the pre-decrement
# RSP because the value is read before RSP is updated.
.gadget amd64_push_reg
    ldr  x8, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14, x8, lsl 3]        /* x10 = value (survives prep via save_c) */
    ldr  x9, [x14, 4*8]               /* x9 = RSP (regs[4]) */
    sub  x9, x9, 8                    /* x9 = new RSP = store address */
    amd64_vwrite_prep 8, push_reg
    str  x10, [x9]
    amd64_vwrite_done 8, push_reg
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]               /* re-read original RSP (still unmodified) */
    sub  x8, x8, 8
    str  x8, [x14, 4*8]               /* commit RSP -= 8 only after the store */
    gret 1
    amd64_vwrite_bullshit 8, push_reg

# amd64 pop reg (0x58+r; REX.B selects r8-r15): always a 64-bit stack pop.
# Operand 0: destination register number (0-15). `pop rsp` works because regs[rsp]
# is written first and then overwritten by the popped value.
.gadget amd64_pop_reg
    ldr  x8, [_ip]
    mov  x10, x8                      /* x10 = dest reg (survives prep via save_c) */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]               /* x9 = RSP = read address */
    amd64_vread_prep 8, pop_reg
    ldr  x11, [x9]                    /* x11 = popped value */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    add  x8, x8, 8
    str  x8, [x14, 4*8]               /* regs[rsp] = rsp + 8 */
    str  x11, [x14, x10, lsl 3]       /* regs[dest] = value (pop rsp: value wins) */
    gret 1
    amd64_vread_bullshit 8, pop_reg

# amd64 push imm (0x68 imm32 / 0x6a imm8): push a sign-extended 64-bit immediate.
# gen.c has already sign-extended the immediate to 64 bits and emits it as
# operand 0, so the gadget just stores it. Same flush/fault contract as push_reg.
.gadget amd64_push_imm
    ldr  x10, [_ip]                   /* x10 = sign-extended 64-bit immediate */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]               /* x9 = RSP */
    sub  x9, x9, 8                    /* x9 = new RSP = store address */
    amd64_vwrite_prep 8, push_imm
    str  x10, [x9]
    amd64_vwrite_done 8, push_imm
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, 8
    str  x8, [x14, 4*8]
    gret 1
    amd64_vwrite_bullshit 8, push_imm

# amd64 leave (0xc9): mov rsp, rbp ; pop rbp. RBP is regs[5]. The pop reads from
# [RBP] (the new RSP), then RSP = RBP + 8 and RBP = the popped value. RSP/RBP are
# committed only after the read succeeds, so a page fault re-executes cleanly.
# 64-bit only; the 0x66 (16-bit) form stays bridged. No operands.
.gadget amd64_leave
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 5*8]               /* x9 = RBP = read address */
    amd64_vread_prep 8, leave
    ldr  x11, [x9]                    /* x11 = popped value (new RBP) */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 5*8]               /* re-read old RBP */
    add  x8, x8, 8                    /* new RSP = old RBP + 8 */
    str  x8, [x14, 4*8]              /* regs[rsp] = old_rbp + 8 */
    str  x11, [x14, 5*8]             /* regs[rbp] = popped value */
    gret 0
    amd64_vread_bullshit 8, leave

# amd64 ret (0xc3): pop the 64-bit return address into RIP and exit the block;
# the main loop then finds/runs the target block (indirect target, so no static
# link). Saves the gadget->C->gadget round-trip the bridge paid. A page fault on
# the stack read re-executes the ret (rip flushed). RSP is committed only after
# the read succeeds. No operands; ends the block via jit_ret (no gret).
.gadget amd64_ret
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]               /* x9 = RSP = read address */
    amd64_vread_prep 8, ret
    ldr  x10, [x9]                    /* x10 = return address (popped) */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    add  x8, x8, 8
    str  x8, [x14, 4*8]              /* RSP += 8 */
    str  x10, [_cpu, CPU_amd64_rip]  /* rip = target */
    str  w10, [_cpu, CPU_eip]        /* mirror low 32 bits, as amd64_jmp does */
    b    jit_ret
    amd64_vread_bullshit 8, ret

# amd64 indirect jmp, register form (0xff /4, mod==3): rip = regs[rm], exit. No
# memory access. Operand 0 = rm register number (0-15). Always 64-bit in long mode.
.gadget amd64_jmp_indir_reg
    ldr  x8, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14, x8, lsl 3]       /* target = regs[rm] */
    str  x10, [_cpu, CPU_amd64_rip]
    str  w10, [_cpu, CPU_eip]
    b    jit_ret

# amd64 indirect call, register form (0xff /2, mod==3): push the return address
# and set rip = regs[rm]. The target is read BEFORE the push (x86 order; matters
# for `call *%rsp`). Operand 0 = rm reg number, operand 1 = next_ip (return addr).
# rip flushed so a #PF on the push re-executes the call.
.gadget amd64_call_indir_reg
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [_ip]
    ldr  x10, [x14, x8, lsl 3]       /* x10 = target = regs[rm] (survives prep) */
    ldr  x9, [x14, 4*8]              /* RSP */
    sub  x9, x9, 8                   /* new RSP = store address */
    amd64_vwrite_prep 8, call_ir
    ldr  x11, [_ip, 8]               /* return address (operand 1) */
    str  x11, [x9]                   /* push return address */
    amd64_vwrite_done 8, call_ir
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, 8
    str  x8, [x14, 4*8]             /* RSP -= 8 */
    str  x10, [_cpu, CPU_amd64_rip]  /* rip = target (read before the push) */
    str  w10, [_cpu, CPU_eip]
    b    jit_ret
    amd64_vwrite_bullshit 8, call_ir

# amd64 indirect jmp, memory form (0xff /4, mod!=3): the target is the 8-byte value
# at the effective address -- this is the PLT thunk `jmp *off(%rip)` and vtable/
# function-pointer dispatch. rip = *(u64 *)addr, then exit. meta(op0)/disp(op1)
# describe the addressing mode and next_ip(op2) is the rip-relative base consumed by
# amd64_vmem_addr. rip is flushed by gen.c so a #PF on the target load re-executes.
# The FS-prefix form is excluded in gen.c (amd64_vmem_addr does not add tls_ptr).
.gadget amd64_jmp_indir_mem
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep 8, jmp_im       /* x9 = host addr of the target */
    ldr  x10, [x9]                   /* target = *(u64 *)addr */
    str  x10, [_cpu, CPU_amd64_rip]
    str  w10, [_cpu, CPU_eip]
    b    jit_ret
    amd64_vread_bullshit 8, jmp_im

# amd64 indirect call, memory form (0xff /2, mod!=3): read the 8-byte target from the
# effective address FIRST (x86 order), then push the return address and set rip. Two
# faulting accesses -- the target load (id call_im) and the push (id call_im_w). x10
# holds the target across the push's vwrite_prep (preserved by save_c). RSP is
# committed only after the push store and rip is flushed by gen.c, so a #PF on either
# access cleanly re-executes the whole instruction. FS-prefix form excluded in gen.c.
.gadget amd64_call_indir_mem
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep 8, call_im      /* x9 = host addr of the target */
    ldr  x10, [x9]                   /* x10 = target (survives the push prep) */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]              /* RSP */
    sub  x9, x9, 8                   /* new RSP = push store address */
    amd64_vwrite_prep 8, call_im_w
    ldr  x11, [_ip, 16]              /* return address (operand 2 = next_ip) */
    str  x11, [x9]                   /* push return address */
    amd64_vwrite_done 8, call_im_w
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, 8
    str  x8, [x14, 4*8]             /* RSP -= 8 (commit after the store) */
    str  x10, [_cpu, CPU_amd64_rip]  /* rip = target (read before the push) */
    str  w10, [_cpu, CPU_eip]
    b    jit_ret
    amd64_vread_bullshit 8, call_im
    amd64_vwrite_bullshit 8, call_im_w

# amd64 MOV reg<-[mem] (0x8b), mod!=3: load \vbytes from the effective address into
# the modrm.reg register with amd64 size semantics (32-bit zero-extends the upper 32,
# 16-bit merges the low 16 and preserves the rest). The reg cache is flushed by gen.c,
# so base/index come from CPU_amd64_regs and the destination is written there too (a
# #PF re-executes via the flushed rip). meta(op0)/disp(op1) describe the addressing
# mode; next_ip(op2) is amd64_vmem_addr's rip base. A per-size gadget keeps the
# vread_prep size literal (correct cross-page staging). Byte (0x8a) keeps bridging --
# its modrm.reg encodes AH/CH/DH/BH without REX (amd64_reg_get_encoded8).
.macro amd64_gen_mov_load name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x10, x8, 8, 4               /* dest reg id (survives prep via save_c) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep \vbytes, \name  /* x9 = host addr of the source */
    add  x13, _cpu, CPU_amd64_regs
    add  x13, x13, x10, lsl 3        /* &regs[dest] */
.if \vbytes == 2
    ldrh w10, [x9]
    strh w10, [x13]                  /* merge low 16, preserve upper */
.elseif \vbytes == 4
    ldr  w10, [x9]
    str  x10, [x13]                  /* 32-bit load zero-extends; full slot store */
.else
    ldr  x10, [x9]
    str  x10, [x13]
.endif
    gret 3
    amd64_vread_bullshit \vbytes, \name
.endm

# amd64 MOV [mem]<-reg (0x89), mod!=3: store the low \vbytes of the modrm.reg register
# to the effective address. Reg cache flushed by gen.c (source read from
# CPU_amd64_regs). The source value lives in x10 across vwrite_prep (only x10 survives
# a prep slow-path bl); rip flushed so a #PF on the store re-executes. Byte (0x88)
# keeps bridging (AH/CH/DH/BH encoding).
.macro amd64_gen_mov_store name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x10, x8, 8, 4               /* source reg id */
    amd64_vmem_addr                  /* x9 = effective address */
    add  x11, _cpu, CPU_amd64_regs
    ldr  x10, [x11, x10, lsl 3]      /* x10 = source value (survives prep) */
    amd64_vwrite_prep \vbytes, \name /* x9 = host write addr */
.if \vbytes == 2
    strh w10, [x9]
.elseif \vbytes == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name
    gret 3
    amd64_vwrite_bullshit \vbytes, \name
.endm

amd64_gen_mov_load  mov_load16,  2
amd64_gen_mov_load  mov_load32,  4
amd64_gen_mov_load  mov_load64,  8
amd64_gen_mov_store mov_store16, 2
amd64_gen_mov_store mov_store32, 4
amd64_gen_mov_store mov_store64, 8

# amd64 MOV reg8<-[mem] (0x8a), mod!=3: load one byte into the modrm.reg byte
# register. Byte registers alias: without REX, reg 4-7 are AH/CH/DH/BH = the HIGH
# byte (bits 8-15) of regs[reg-4]; otherwise the low byte of regs[reg]. Computing a
# BYTE POINTER up front (&regs[reg], or &regs[reg-4]+1 for the high-byte case) makes
# a single strb handle both. The byte ptr is held in x10 across the prep (only x10
# survives a slow-path bl). meta bit34 = REX_PRESENT. Cache+rip flushed by gen.c.
.gadget amd64_mov_load8
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x12, x8, 8, 4               /* reg id */
    add  x10, _cpu, CPU_amd64_regs
    add  x10, x10, x12, lsl 3        /* &regs[reg] (low-byte address) */
    tbnz x8, 34, 1f                  /* REX present -> low byte */
    cmp  x12, 4
    b.lo 1f                          /* reg 0-3 -> low byte */
    sub  x10, x10, 4*8               /* reg 4-7, no REX: &regs[reg-4]... */
    add  x10, x10, 1                 /* ...+1 = AH/CH/DH/BH high byte */
1:
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep 1, mov_load8    /* x9 = host addr of the source byte */
    ldrb w11, [x9]
    strb w11, [x10]                  /* write the byte (low or high), preserve rest */
    gret 3
    amd64_vread_bullshit 1, mov_load8

# amd64 MOV [mem]<-reg8 (0x88), mod!=3: store one byte from the modrm.reg byte
# register (AH/CH/DH/BH high-byte aliasing as above). The source byte is read through
# the same byte pointer into x10 (survives the vwrite_prep), then stored.
.gadget amd64_mov_store8
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x12, x8, 8, 4               /* reg id */
    add  x11, _cpu, CPU_amd64_regs
    add  x11, x11, x12, lsl 3        /* &regs[reg] (low-byte address) */
    tbnz x8, 34, 1f
    cmp  x12, 4
    b.lo 1f
    sub  x11, x11, 4*8
    add  x11, x11, 1                 /* AH/CH/DH/BH high byte */
1:
    ldrb w10, [x11]                  /* x10 = source byte (survives prep) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vwrite_prep 1, mov_store8  /* x9 = host write addr */
    strb w10, [x9]
    amd64_vwrite_done 1, mov_store8
    gret 3
    amd64_vwrite_bullshit 1, mov_store8

# amd64 load-op arith: reg <op>= [mem] (d=1 forms ADD 0x03 / SUB 0x2b / CMP 0x3b),
# mod!=3, sizes 32/64. The reg operand (modrm.reg) is the destination/lhs and the
# memory operand is the rhs. Mirrors gadget_amd64_cached_arith_reg_reg but reads the
# rhs from memory and runs in flush style (cache flushed by gen.c -> regs in memory,
# #PF re-executes). Sets CF/OF/AF/ZF/SF/PF eagerly via amd64_cached_set_addsub_flags
# (lhs=x13, rhs=x10, result=x15, cf=x14, of=x16; the macro touches x12/x17 only, so
# opcode (x9) and dst id (x8) survive it). CMP (opcode>=0x38) sets flags but does not
# write the destination. Byte forms (0x02/2a/3a) and ADC/SBB bridge (parity with the
# reg-reg native path); 16-bit (0x66) bridges. opcode|regid packed in x10 survives
# the prep (only x10 does). Operands: meta(0)/disp(1)/next_ip(2).
.macro amd64_gen_loadop_arith name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    and  x10, x8, 0xfff              /* opcode|regid<<8 (survives prep) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep \vbytes, \name  /* x9 = host addr of the rhs */
.if \vbytes == 4
    ldr  w11, [x9]                   /* rhs = [mem] (32-bit) */
.else
    ldr  x11, [x9]                   /* rhs = [mem] (64-bit) */
.endif
    and  x9, x10, 0xff               /* x9 = opcode */
    ubfx x8, x10, 8, 4               /* x8 = dst reg id */
    add  x13, _cpu, CPU_amd64_regs
    ldr  x13, [x13, x8, lsl 3]       /* x13 = lhs = dst reg value */
    mov  x10, x11                    /* x10 = rhs */
.if \vbytes == 4
    mov  w13, w13                    /* truncate lhs to 32 */
    mov  w10, w10                    /* truncate rhs to 32 */
    cmp  x9, 0x28
    b.hs 1f
    adds w15, w13, w10
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs w15, w13, w10
    cset w14, cc
    cset w16, vs
2:  mov  w15, w15                    /* zero-extend 32-bit result */
.else
    cmp  x9, 0x28
    b.hs 1f
    adds x15, x13, x10
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs x15, x13, x10
    cset w14, cc
    cset w16, vs
2:
.endif
    amd64_cached_set_addsub_flags \sbit
    cmp  x9, 0x38                    /* CMP (>=0x38): flags only, no write */
    b.hs 3f
    add  x13, _cpu, CPU_amd64_regs
.if \vbytes == 4
    ubfx x12, x15, 0, 32             /* 32-bit write zero-extends the upper 32 */
    str  x12, [x13, x8, lsl 3]
.else
    str  x15, [x13, x8, lsl 3]
.endif
3:  gret 3
    amd64_vread_bullshit \vbytes, \name
.endm

amd64_gen_loadop_arith loadop_arith32, 4, 31
amd64_gen_loadop_arith loadop_arith64, 8, 63

# amd64 load-op logic: reg <op>= [mem] (d=1 forms OR 0x0b / AND 0x23 / XOR 0x33),
# mod!=3, sizes 32/64. reg is dst/lhs, [mem] is rhs. Like loadop_arith but with the
# logic flag rule (CF=OF=0, AF undefined, ZF/SF/PF from the result) via
# amd64_cached_set_logic_flags (it reads the result in x15 and the size in a reg).
# Byte forms (0x0a/22/32) + TEST + 16-bit bridge. Operands: meta(0)/disp(1)/next_ip(2).
.macro amd64_gen_loadop_logic name, vbytes, ssize
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    and  x10, x8, 0xfff              /* opcode|regid<<8 (survives prep) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep \vbytes, \name  /* x9 = host addr of the rhs */
.if \vbytes == 4
    ldr  w11, [x9]                   /* rhs = [mem] (32-bit) */
.else
    ldr  x11, [x9]                   /* rhs = [mem] (64-bit) */
.endif
    and  x9, x10, 0xff               /* x9 = opcode */
    ubfx x8, x10, 8, 4               /* x8 = dst reg id */
    add  x13, _cpu, CPU_amd64_regs
    ldr  x13, [x13, x8, lsl 3]       /* x13 = lhs = dst reg value */
    cmp  x9, 0x23
    b.eq 1f                          /* AND */
    cmp  x9, 0x33
    b.eq 2f                          /* XOR */
    orr  x15, x13, x11               /* OR (0x0b) */
    b    3f
1:  and  x15, x13, x11
    b    3f
2:  eor  x15, x13, x11
3:
.if \vbytes == 4
    mov  w15, w15                    /* zero-extend 32-bit result */
.endif
    mov  x11, \ssize                 /* size in a reg for the flag macro */
    amd64_cached_set_logic_flags x15, x11
    add  x13, _cpu, CPU_amd64_regs
.if \vbytes == 4
    ubfx x12, x15, 0, 32
    str  x12, [x13, x8, lsl 3]
.else
    str  x15, [x13, x8, lsl 3]
.endif
    gret 3
    amd64_vread_bullshit \vbytes, \name
.endm

amd64_gen_loadop_logic loadop_logic32, 4, 32
amd64_gen_loadop_logic loadop_logic64, 8, 64
