// AArch64 guest crypto-extension gadgets: AES, SHA1, SHA256, SHA512, and
// the SHA3 four-register ops. Ported from OpenMinis/ish-arm64
// gadgets-aarch64/crypto.S (GPLv3, see /CREDITS-aarch64.md); their
// bodies use raw .inst encodings — this port uses the mnemonics with
// .arch_extension. FEAT_AES/SHA1/SHA256 are v8.0 crypto, present on
// every 64-bit Apple SoC (A7+). FEAT_SHA512/SHA3 only arrived with A13,
// so: the SHA3 ops (RAX1/EOR3/BCAX/XAR) are synthesized from baseline
// NEON here, and the native SHA512 gadgets are only handed out by gen.c
// when the host has the instructions — otherwise gen.c emits sha512_soft,
// which runs the op in C (arm64_sha512_soft, emu/tlb.c).
//
// Code streams:
//   1-src:  [gadget][rd | rn<<8]
//   2-src:  [gadget][rd | rn<<8 | rm<<16]     (rd is the accumulator)
//   4-reg:  [gadget][rd | rn<<8 | rm<<16 | ra<<24]

#include "gadgets.h"

.arch_extension aes
.arch_extension sha2
.arch_extension sha3

.text

// Vd = op(Vd, Vn): AESE/AESD (state in Vd, round key in Vn).
.macro crypto_2acc name, insn
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x9, lsl #4
    ldr q0, [x13]
    add x14, x12, x10, lsl #4
    ldr q1, [x14]
    \insn
    str q0, [x13]
    gret
.endm

crypto_2acc aese,      "aese v0.16b, v1.16b"
crypto_2acc aesd,      "aesd v0.16b, v1.16b"
crypto_2acc sha1su1,   "sha1su1 v0.4s, v1.4s"
crypto_2acc sha256su0, "sha256su0 v0.4s, v1.4s"
crypto_2acc sha512su0, "sha512su0 v0.2d, v1.2d"

// Vd = op(Vn): AESMC/AESIMC/SHA1H (pure functions of Vn).
.macro crypto_1src name, insn
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x12, _cpu, #CPU_v0
    add x14, x12, x10, lsl #4
    ldr q0, [x14]
    \insn
    add x13, x12, x9, lsl #4
    str q0, [x13]
    gret
.endm

crypto_1src aesmc,  "aesmc v0.16b, v0.16b"
crypto_1src aesimc, "aesimc v0.16b, v0.16b"
crypto_1src sha1h,  "sha1h s0, s0"          // scalar result: upper bits zeroed

// Vd = op(Vd, Vn, Vm): the three-register hash steps.
.macro crypto_3acc name, insn
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x9, lsl #4
    ldr q0, [x13]
    add x14, x12, x10, lsl #4
    ldr q1, [x14]
    add x14, x12, x11, lsl #4
    ldr q2, [x14]
    \insn
    str q0, [x13]
    gret
.endm

crypto_3acc sha1c,     "sha1c q0, s1, v2.4s"
crypto_3acc sha1p,     "sha1p q0, s1, v2.4s"
crypto_3acc sha1m,     "sha1m q0, s1, v2.4s"
crypto_3acc sha1su0,   "sha1su0 v0.4s, v1.4s, v2.4s"
crypto_3acc sha256h,   "sha256h q0, q1, v2.4s"
crypto_3acc sha256h2,  "sha256h2 q0, q1, v2.4s"
crypto_3acc sha256su1, "sha256su1 v0.4s, v1.4s, v2.4s"
crypto_3acc sha512h,   "sha512h q0, q1, v2.2d"
crypto_3acc sha512h2,  "sha512h2 q0, q1, v2.2d"
crypto_3acc sha512su1, "sha512su1 v0.2d, v1.2d, v2.2d"

// RAX1 Vd.2d = Vn ^ ROL(Vm, 1) — non-accumulating. Synthesized from
// baseline NEON (shl + usra build the per-lane rotate; the shifted-in
// bits are disjoint, so accumulate == insert) so pre-FEAT_SHA3 hosts
// run it too. Verified bit-exact against native rax1 on an M-series
// host over random vectors.
// [gadget][rd | rn<<8 | rm<<16]
.gadget rax1
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    add x12, _cpu, #CPU_v0
    add x14, x12, x10, lsl #4
    ldr q1, [x14]
    add x14, x12, x11, lsl #4
    ldr q2, [x14]
    shl v0.2d, v2.2d, #1
    usra v0.2d, v2.2d, #63
    eor v0.16b, v0.16b, v1.16b
    add x13, x12, x9, lsl #4
    str q0, [x13]
    gret

// SHA3 four-register: Vd = op(Vn, Vm, Va) — not accumulating. Bodies are
// baseline NEON (EOR3 = eor+eor, BCAX = bic+eor), so no FEAT_SHA3 needed.
.macro crypto_4reg name, insn1, insn2
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x15, x8, #24, #5
    add x12, _cpu, #CPU_v0
    add x14, x12, x10, lsl #4
    ldr q1, [x14]
    add x14, x12, x11, lsl #4
    ldr q2, [x14]
    add x14, x12, x15, lsl #4
    ldr q3, [x14]
    \insn1
    \insn2
    add x13, x12, x9, lsl #4
    str q0, [x13]
    gret
.endm

crypto_4reg eor3, "eor v0.16b, v1.16b, v2.16b", "eor v0.16b, v0.16b, v3.16b"
crypto_4reg bcax, "bic v0.16b, v2.16b, v3.16b", "eor v0.16b, v0.16b, v1.16b"

// Soft SHA512 fallback for hosts without FEAT_SHA512 (pre-A13): the whole
// op runs in C (arm64_sha512_soft, emu/tlb.c), keeping the guest-visible
// feature set (AT_HWCAP SHA512, ID_AA64ISAR0.SHA2=2) identical on every
// device. op: 0=SHA512H 1=SHA512H2 2=SHA512SU1 3=SHA512SU0.
// [gadget][rd | rn<<8 | rm<<16 | op<<24]
.gadget sha512_soft
    ldr x8, [_ip]
    add _ip, _ip, #8
    save_c
    mov x0, _cpu
    mov x1, x8
    bl NAME(arm64_sha512_soft)
    restore_c
    gret

// XAR Vd.2d, Vn.2d, Vm.2d, #imm6: (Vn ^ Vm) rotated right by imm per
// 64-bit lane. The host imm form can't take a runtime amount; rotate =
// (x >> n) | (x << (64-n)) via two broadcast register shifts.
// [gadget][rd | rn<<8 | rm<<16][neg_amount][64 - amount]
.gadget xar
    ldr x8, [_ip]
    ldr x9, [_ip, #8]          // -n
    ldr x14, [_ip, #16]        // 64-n
    add _ip, _ip, #24
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    ubfx x15, x8, #16, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x11, lsl #4
    ldr q0, [x13]
    add x13, x12, x15, lsl #4
    ldr q1, [x13]
    eor v0.16b, v0.16b, v1.16b
    dup v29.2d, x9
    dup v28.2d, x14
    ushl v1.2d, v0.2d, v29.2d  // x >> n
    ushl v0.2d, v0.2d, v28.2d  // x << (64-n)
    orr v0.16b, v0.16b, v1.16b
    add x13, x12, x10, lsl #4
    str q0, [x13]
    gret

# vim: ft=gas
