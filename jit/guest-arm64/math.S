// AArch64 guest data-processing gadgets. Adapted from OpenMinis/ish-arm64's
// asbestos/guest-arm64/gadgets-aarch64/math.S (GPLv3, see
// /CREDITS-aarch64.md). Mechanical renames from their source: _pc -> _ip
// (this file's gadgets.h names the code-stream pointer _ip, matching
// iSH-AOK's i386-guest convention, not their _pc); gadget symbols get an
// arm64_ prefix via the .gadget macro (avoids collision with i386/amd64
// gadget names in the same final binary). Otherwise these gadget bodies
// are unchanged — the packed-parameter encoding in each comment matches
// exactly what emu/arm64_interp.c's arm64_execute() already decodes (see
// that file for the field-layout citations), so gen_step_arm64 (jit/gen.c)
// emits parameters in the same packed form these gadgets expect.

#include "gadgets.h"

.text

// Load a guest register into _tmp (x0). Code stream: [gadget addr][reg_index]
.gadget load_reg
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr _tmp, [x9, x8, lsl #3]
    gret
1:  mov _tmp, #0            // XZR
    gret

// Store _tmp (x0) to a guest register. Code stream: [gadget addr][reg_index]
.gadget store_reg
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f                 // XZR - discard
    str _tmp, [x9, x8, lsl #3]
1:  add _ip, _ip, #8
    gret

/*
 * Move wide immediate (MOVZ, MOVK, MOVN)
 * Code stream: [gadget addr][rd | hw<<8 | imm16<<16 | sf<<32]
 */

.gadget movz
    ldr x8, [_ip]
    add _ip, _ip, #8

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #2    // hw
    ubfx x11, x8, #16, #16  // imm16
    ubfx x12, x8, #32, #1   // sf

    lsl x10, x10, #4        // hw * 16
    lsl x11, x11, x10       // imm16 << (hw*16)

    cbz x12, 1f
    b 2f
1:  and x11, x11, #0xffffffff
2:
    add x13, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 3f
    str x11, [x13, x9, lsl #3]
3:  gret

.gadget movk
    ldr x8, [_ip]
    add _ip, _ip, #8

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #2    // hw
    ubfx x11, x8, #16, #16  // imm16
    ubfx x12, x8, #32, #1   // sf

    add x13, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 3f
    ldr x14, [x13, x9, lsl #3]

    cbz x12, 1f
    b 2f
1:  and x14, x14, #0xffffffff
2:
    lsl x10, x10, #4        // hw * 16
    mov x15, #0xffff
    lsl x15, x15, x10       // mask at position
    lsl x11, x11, x10       // value at position

    bic x14, x14, x15
    orr x14, x14, x11

    cbz x12, 4f
    b 5f
4:  and x14, x14, #0xffffffff
5:
    str x14, [x13, x9, lsl #3]
3:  gret

.gadget movn
    ldr x8, [_ip]
    add _ip, _ip, #8

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #2    // hw
    ubfx x11, x8, #16, #16  // imm16
    ubfx x12, x8, #32, #1   // sf

    lsl x10, x10, #4
    lsl x11, x11, x10
    mvn x11, x11            // NOT

    cbz x12, 1f
    b 2f
1:  and x11, x11, #0xffffffff
2:
    add x13, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 3f
    str x11, [x13, x9, lsl #3]
3:  gret

/*
 * ADR/ADRP - PC-relative address
 * Code stream: [gadget addr][rd(0:4) | target(8:55)]
 * Target is a 48-bit guest address packed in bits 8-55 (already computed
 * by gen_step_arm64 at compile time, since the base PC is compile-time
 * known -- see arm64_execute()'s ADR/ADRP handler in emu/arm64_interp.c
 * for the equivalent runtime computation this gadget's caller does ahead
 * of time instead).
 */
.gadget adr
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #48      // target (48-bit)
    add x11, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    str x10, [x11, x9, lsl #3]
    gret
1:  gret
