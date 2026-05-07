#include "gadgets.h"
#include "math.h"

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
    ldr x14, [_ip, 24]
    add x9, x9, x14
3:
    cmp x11, 64
    b.eq 4f
    cmp x11, 32
    b.eq 5f
    add x12, x13, x10, lsl 3
    strh w9, [x12]
    gret 2
4:
    str x9, [x13, x10, lsl 3]
    gret 2
5:
    add x12, x13, x10, lsl 3
    mov w9, w9
    str x9, [x12]
    gret 2

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
