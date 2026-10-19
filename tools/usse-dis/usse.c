/* usse.c — SGX Series-5 USSE decoder core. Field layouts derived from the
 * SGX540 oracle disassembly by bit-correlation (debug/usse/solve_bits.py). */
#include "usse.h"
#include <stdio.h>

/* Major opcode class = enc[63:59]. Base mnemonic per class (NULL = unmapped).
 * Enumerated from the 540 oracle histogram; class 0x1F needs sub-decoding. */
static const char *CLASS[32] = {
    [0]  = "fmad",   [5]  = "mov",    [7]  = "efo",
    [9]  = "and",    [10] = "and",    [11] = "xor",
    [12] = "shl",    [13] = "shr",    [20] = "ima16",
    [21] = "imae",   [29] = "ldad",   [30] = "stad",
    [31] = "special",
};

const char *usse_class_name(unsigned top5) {
    return (top5 < 32) ? CLASS[top5] : 0;
}

/* bit(enc,n) -> nth bit; field helpers keep the derived maps readable. */
static inline uint32_t bit(uint64_t e, unsigned n) { return (uint32_t)((e >> n) & 1u); }
static inline uint32_t bits(uint64_t e, unsigned hi, unsigned lo) {
    return (uint32_t)((e >> lo) & ((1ull << (hi - lo + 1)) - 1));
}

/* limm: "mov r<reg>, #0x<imm32>".  Signature enc[63:54]=0b1111110000, enc[53]=1
 * (byte7 == 0xFC).  reg = enc[25:21].  Immediate is split:
 *   imm[20:0]=enc[20:0]  imm[25:21]=enc[40:36]  imm[31:26]=enc[49:44]. */
static int decode_limm(uint64_t e, usse_insn *o) {
    if (bits(e, 63, 54) != 0x3F0u || !bit(e, 53)) return 0;
    /* Only the confirmed subset: r-bank dest (enc[52:51]==0) and no predicate
     * (enc[43:41]==0). Predicate[43:41] and dest-bank[52:51] are shared fields
     * still to be solved across the full oracle — until then, don't claim them. */
    if (bits(e, 52, 51) != 0 || bits(e, 43, 41) != 0) return 0;
    /* enc[35:26] is zero for a plain direct dest; nonzero flags indexed/special
     * dest addressing (e.g. "sa[il + #n]") we don't decode yet. */
    if (bits(e, 35, 26) != 0) return 0;
    uint32_t reg = bits(e, 25, 21);
    uint32_t imm = bits(e, 20, 0)
                 | (bits(e, 40, 36) << 21)
                 | (bits(e, 49, 44) << 26);
    snprintf(o->text, sizeof o->text, "mov r%u, #0x%08X", reg, imm);
    return 1;
}

/* Control/special sub-ops (class 0x1F) with no or trivial operands, from the
 * oracle: nop/lapc are single fixed encodings; idf/wdf are a fixed signature
 * plus a 1-bit DRC selector at enc[32]. Branches (ba/br) are deferred — the
 * oracle's targets are all tiny, so the target field past bit 2 is unexercised
 * and we won't guess it. */
static int decode_special(uint64_t e, usse_insn *o) {
    if (e == 0xF800014000000000ull) { snprintf(o->text, sizeof o->text, "nop");  return 1; }
    if (e == 0xF800008000000000ull) { snprintf(o->text, sizeof o->text, "lapc"); return 1; }
    uint64_t no_drc = e & ~(1ull << 32);        /* mask off the DRC operand bit */
    unsigned drc = (unsigned)((e >> 32) & 1u);
    if (no_drc == 0xF820000000000000ull) { snprintf(o->text, sizeof o->text, "idf drc%u, bif", drc); return 1; }
    if (no_drc == 0xF920000000000000ull) { snprintf(o->text, sizeof o->text, "wdf drc%u", drc); return 1; }
    return 0;
}

void usse_decode(uint64_t e, uint32_t pc, usse_insn *o) {
    (void)pc;
    o->enc = e;
    o->valid = 1;

    if (decode_limm(e, o)) return;
    if (decode_special(e, o)) return;

    /* Unimplemented: emit the class base mnemonic as a readable hint (operands
     * TODO) so `pd` is navigable; mark invalid so the regression harness counts
     * it as not-yet-decoded rather than a false match. */
    const char *base = usse_class_name((unsigned)(e >> 59));
    o->valid = 0;
    if (base)
        snprintf(o->text, sizeof o->text, "%-8s; TODO operands", base);
    else
        snprintf(o->text, sizeof o->text, ".long 0x%016llX", (unsigned long long)e);
}
