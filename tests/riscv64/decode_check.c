// Host-side check for emu/arch/riscv64/decode.h: RVC expansion and the
// I/S/B/U/J immediate reassemblers, verified word-for-word against vectors
// assembled by llvm-mc (see gen_decode_vectors.py). Runs as a plain meson
// test on any host; no emulation involved.
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "emu/arch/riscv64/decode.h"
#include "decode_vectors.h"

static int failures;

static void check_rvc(void) {
    for (size_t i = 0; i < sizeof(rvc_vectors)/sizeof(rvc_vectors[0]); i++) {
        const struct rvc_vector *v = &rvc_vectors[i];
        uint32_t got = riscv64_expand_rvc(v->c);
        if (got != v->expanded) {
            printf("FAIL rvc %-28s c=0x%04x expected 0x%08x got 0x%08x\n",
                   v->name, v->c, v->expanded, got);
            failures++;
        }
        if (riscv64_insn_length((uint16_t) v->c) != 2 ||
            riscv64_insn_length((uint16_t) v->expanded) != 4) {
            printf("FAIL length %s\n", v->name);
            failures++;
        }
    }
}

static void check_imm(void) {
    for (size_t i = 0; i < sizeof(imm_vectors)/sizeof(imm_vectors[0]); i++) {
        const struct imm_vector *v = &imm_vectors[i];
        int64_t got;
        switch (v->format) {
        case 'i': got = riscv64_imm_i(v->insn); break;
        case 's': got = riscv64_imm_s(v->insn); break;
        case 'b': got = riscv64_imm_b(v->insn); break;
        case 'u': got = riscv64_imm_u(v->insn); break;
        case 'j': got = riscv64_imm_j(v->insn); break;
        default: printf("FAIL bad format %c\n", v->format); failures++; continue;
        }
        if (got != v->imm) {
            printf("FAIL imm-%c %-24s insn=0x%08x expected %"PRId64" got %"PRId64"\n",
                   v->format, v->name, v->insn, v->imm, got);
            failures++;
        }
    }
}

static void check_encoders_roundtrip(void) {
    // The enc_b/enc_j scramblers and imm_b/imm_j reassemblers must be exact
    // inverses over the full immediate range.
    for (int64_t imm = -4096; imm <= 4094; imm += 2) {
        uint32_t w = riscv64_enc_b(RISCV64_OP_BRANCH, 0, 1, 2, imm);
        if (riscv64_imm_b(w) != imm) {
            printf("FAIL b roundtrip imm=%"PRId64"\n", imm);
            failures++;
        }
    }
    for (int64_t imm = -1048576; imm <= 1048574; imm += 2) {
        uint32_t w = riscv64_enc_j(RISCV64_OP_JAL, 1, imm);
        if (riscv64_imm_j(w) != imm) {
            printf("FAIL j roundtrip imm=%"PRId64"\n", imm);
            failures++;
        }
    }
    for (int64_t imm = -2048; imm <= 2047; imm++) {
        if (riscv64_imm_i(riscv64_enc_i(RISCV64_OP_OP_IMM, 1, 0, 2, imm)) != imm ||
            riscv64_imm_s(riscv64_enc_s(RISCV64_OP_STORE, 3, 1, 2, imm)) != imm) {
            printf("FAIL i/s roundtrip imm=%"PRId64"\n", imm);
            failures++;
        }
    }
}

static void check_reserved(void) {
    // Encodings the expander must reject (return 0).
    if (riscv64_expand_rvc(0x0000) != 0) {
        printf("FAIL all-zeros not rejected\n");
        failures++;
    }
    // c.addi4spn with nzuimm == 0 (funct3=000, quadrant 00, rd'=0): reserved
    if (riscv64_expand_rvc(0x0004) != 0) {
        printf("FAIL c.addi4spn nzuimm=0 not rejected\n");
        failures++;
    }
    // c.addiw with rd == 0 (quadrant 01, funct3=001): reserved
    if (riscv64_expand_rvc(0x2001) != 0) {
        printf("FAIL c.addiw rd=0 not rejected\n");
        failures++;
    }
    // c.lwsp with rd == 0 (quadrant 10, funct3=010): reserved
    if (riscv64_expand_rvc(0x4002) != 0) {
        printf("FAIL c.lwsp rd=0 not rejected\n");
        failures++;
    }
}

int main(void) {
    check_rvc();
    check_imm();
    check_encoders_roundtrip();
    check_reserved();
    if (failures) {
        printf("%d FAILURES\n", failures);
        return 1;
    }
    printf("riscv64 decode: all checks passed (%zu rvc vectors, %zu imm vectors, "
           "full-range b/j/i/s roundtrips)\n",
           sizeof(rvc_vectors)/sizeof(rvc_vectors[0]),
           sizeof(imm_vectors)/sizeof(imm_vectors[0]));
    return 0;
}
