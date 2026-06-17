#include "gadgets.h"
#include "math.h"

#define REP_LIST once,rep,repnz

.gadget cmc
    ldrb w8, [_cpu, CPU_cf]
    eor w8, w8, 1
    strb w8, [_cpu, CPU_cf]
    ldr w9, [_cpu, CPU_eflags]
    bic w9, w9, 1
    orr w9, w9, w8
    str w9, [_cpu, CPU_eflags]
    gret

.gadget clc
    strb wzr, [_cpu, CPU_cf]
    ldr w8, [_cpu, CPU_eflags]
    bic w8, w8, 1
    str w8, [_cpu, CPU_eflags]
    gret

.gadget stc
    mov w8, 1
    strb w8, [_cpu, CPU_cf]
    ldr w9, [_cpu, CPU_eflags]
    orr w9, w9, 1
    str w9, [_cpu, CPU_eflags]
    gret

.gadget cld
    ldr w8, [_cpu, CPU_eflags]
    bic w8, w8, DF_FLAG
    str w8, [_cpu, CPU_eflags]
    mov w8, 1
    str w8, [_cpu, CPU_df_offset]
    gret

.gadget std
    ldr w8, [_cpu, CPU_eflags]
    orr w8, w8, DF_FLAG
    str w8, [_cpu, CPU_eflags]
    mov w8, -1
    str w8, [_cpu, CPU_df_offset]
    gret

# FIXME non 32 bit
.macro do_strop op, size, rep, s=
    # repnz is only a thing for cmps and scas
    .ifc \rep,repnz
        .ifnc \op,cmps N .ifnc \op,scas
            .exitm
        .endif N .endif
    .endif

    .gadget \op\size\()_\rep
        .ifnc \rep,once
            cbz ecx, 2f
        .endif
        # Forward bulk fast path for movs/stos via rep_string_fast (jit/helpers.c):
        # resolve the host page once per page-run instead of re-hashing the TLB
        # per element. Backward (DF=1) and small counts fall through to the
        # per-element loop below; cmps/scas/lods/once are unaffected (guarded).
        .ifin(\op, movs, stos)
        .ifc \rep,rep
            ldr w8, [_cpu, CPU_eflags]
            tst w8, DF_FLAG
            b.ne 8f                     /* DF=1: backward, use per-element loop */
            cmp ecx, 8
            b.lo 8f                     /* tiny rep: per-element loop wins */
            str esi, [_cpu, CPU_esi]
            str edi, [_cpu, CPU_edi]
            str ecx, [_cpu, CPU_ecx]
            .ifc \op,stos
            str eax, [_cpu, CPU_eax]
            .endif
            save_c
            mov x0, _cpu
            sub x1, _tlb, TLB_entries
            mov w2, (\size/8)
            .ifc \op,movs
            mov w3, 1
            .else
            mov w3, 0
            .endif
            bl NAME(rep_string_fast)
            mov x19, x0
            restore_c
            ldr esi, [_cpu, CPU_esi]
            ldr edi, [_cpu, CPU_edi]
            ldr ecx, [_cpu, CPU_ecx]
            cbnz x19, 7f                /* nonzero: faulted */
            gret 1
        7:
            cmp x19, 1
            b.ne 6f                     /* x19==2: dest/write fault */
            b segfault_read             /* x19==1: source/read fault */
        6:
            b segfault_write
        8:
        .endif
        .endifin
        # df_offset = w12
        mov w12, (\size/8)
        ldr w8, [_cpu, CPU_eflags]
        tst w8, DF_FLAG
        cneg w12, w12, ne
    1:

        .ifc \op,lods
            mov _addr, esi
            read_prep \size, \op\size\()_\rep
            ldrs eax, [_xaddr], \s

        .else N .ifc \op,stos
            mov _addr, edi
            write_prep \size, \op\size\()_\rep
            str\s eax, [_xaddr]
            write_done \size, \op\size\()_\rep

        .else N .ifc \op,movs
            mov _addr, esi
            read_prep \size, \op\size\()_\rep
            ldr\s _tmp, [_xaddr]
            mov _addr, edi
            write_prep \size, \op\size\()_\rep\()2
            str\s _tmp, [_xaddr]
            write_done \size, \op\size\()_\rep\()2

        .else N .ifc \op, scas
            mov _addr, edi
            read_prep \size, \op\size\()_\rep
            ldr\s w8, [_xaddr]
            mov _tmp, eax
            setf_a src=w8, dst=_tmp, s=\s
            do_add sub, _tmp, w8, \s
            setf_zsp \s

        .else N .ifc \op, cmps
            mov _addr, esi
            read_prep \size, \op\size\()_\rep
            ldr\s _tmp, [_xaddr]
            mov _addr, edi
            read_prep \size, \op\size\()_\rep\()2
            ldr\s w8, [_xaddr]
            setf_a src=w8, dst=_tmp, s=\s
            do_add sub, _tmp, w8, \s
            setf_zsp \s
        .endif N .endif N .endif N .endif N .endif

        .ifin(\op, lods,movs,cmps)
            add esi, esi, w12
        .endifin
        .ifin(\op, movs,stos,cmps,scas)
            add edi, edi, w12
        .endifin

        .ifnc \rep,once
            subs ecx, ecx, 1
            .ifin(\op, scas,cmps)
                .ifc \rep,rep
                    cbnz _tmp, 2f
                .else N .ifc \rep,repnz
                    cbz _tmp, 2f
                .endif N .endif
            .endifin
            cbnz ecx, 1b
    2:
        .endif
        gret 1
        .ifin(\op, lods,movs,scas,cmps)
            read_bullshit \size, \op\size\()_\rep
        .endifin
        .ifc \op,stos
            write_bullshit \size, \op\size\()_\rep
        .endif
        .ifc \op,movs
            write_bullshit \size, \op\size\()_\rep\()2
        .endif
        .ifc \op,cmps
            read_bullshit \size, \op\size\()_\rep\()2
        .endif
.endm

.irp op, lods,stos,movs,scas,cmps
    .irp size, 8,16,32
        .irp rep, REP_LIST
            .if \size == 8
                do_strop \op, \size, \rep, b
            .elseif \size == 16
                do_strop \op, \size, \rep, h
            .elseif \size == 32
                do_strop \op, \size, \rep
            .endif
        .endr
    .endr
    .gadget_list_size \op, REP_LIST
.endr
# temporary
