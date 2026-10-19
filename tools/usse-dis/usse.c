/* usse.c — SGX Series-5 USSE decoder core. Field layouts derived from the
 * SGX540 oracle disassembly by bit-correlation (solve.py). Each instruction
 * group is a `decode_fn` returning 1 (and writing the exact oracle mnemonic)
 * or 0 (defer); usse_decode tries them in order. Invariant: a decoder that
 * returns 1 must match the oracle — never a guess (see oracle_check.py). */
#include "usse.h"
#include <stdio.h>

/* Major opcode class = enc[63:59]. Base mnemonic per class (NULL = unmapped),
 * enumerated from the 540 oracle histogram; used only as a readable hint for
 * not-yet-decoded instructions. */
static const char *CLASS[32] = {
    [0]  = "fmad",   [5]  = "mov",    [7]  = "efo",
    [9]  = "and",    [10] = "and",    [11] = "xor",
    [12] = "shl",    [13] = "shr",    [20] = "ima16",
    [21] = "imae",   [29] = "ldad",   [30] = "stad",
    [31] = "special",
};
const char *usse_class_name(unsigned top5) { return (top5 < 32) ? CLASS[top5] : 0; }

static inline uint32_t bit(uint64_t e, unsigned n) { return (uint32_t)((e >> n) & 1u); }
static inline uint32_t bits(uint64_t e, unsigned hi, unsigned lo) {
    return (uint32_t)((e >> lo) & ((1ull << (hi - lo + 1)) - 1));
}

/* A group decoder: return 1 and write the exact mnemonic into out, or 0. */
typedef int (*decode_fn)(uint64_t e, char *out, size_t n);

/* ---- limm: "mov r<reg>, #0x<imm32>" ----------------------------------------
 * Signature enc[63:54]=0b1111110000, enc[53]=1 (byte7 0xFC). Confirmed subset
 * only: r-bank dest (enc[52:51]==0), no predicate (enc[43:41]==0), direct dest
 * (enc[35:26]==0). reg=enc[25:21]; imm=[20:0]∪([40:36]<<21)∪([49:44]<<26). */
static int decode_limm(uint64_t e, char *out, size_t n) {
    if (bits(e, 63, 54) != 0x3F0u || !bit(e, 53)) return 0;
    if (bits(e, 52, 51) != 0 || bits(e, 43, 41) != 0 || bits(e, 35, 26) != 0) return 0;
    uint32_t reg = bits(e, 25, 21);
    uint32_t imm = bits(e, 20, 0) | (bits(e, 40, 36) << 21) | (bits(e, 49, 44) << 26);
    snprintf(out, n, "mov r%u, #0x%08X", reg, imm);
    return 1;
}

/* ---- control/special no/trivial-operand ops (class 0x1F) -------------------
 * nop/lapc are single fixed encodings; idf/wdf are a fixed signature plus a
 * 1-bit DRC selector at enc[32]. Branches (ba/br) stay deferred: the oracle's
 * targets are all <=7, so the target field above bit 2 is unexercised. */
static int decode_special(uint64_t e, char *out, size_t n) {
    if (e == 0xF800014000000000ull) { snprintf(out, n, "nop");  return 1; }
    if (e == 0xF800008000000000ull) { snprintf(out, n, "lapc"); return 1; }
    uint64_t no_drc = e & ~(1ull << 32);
    unsigned drc = (unsigned)((e >> 32) & 1u);
    if (no_drc == 0xF820000000000000ull) { snprintf(out, n, "idf drc%u, bif", drc); return 1; }
    if (no_drc == 0xF920000000000000ull) { snprintf(out, n, "wdf drc%u", drc); return 1; }
    return 0;
}

/* ---- INTEGRATION POINT -----------------------------------------------------
 * Verified group decoders produced by the RE fan-out get pasted above this
 * line and listed in DECODERS below. Order matters only for disambiguation;
 * the global regression (oracle_check.py) guarantees no decoder lies. */

static const decode_fn DECODERS[] = {
    decode_limm,
    decode_special,
};

void usse_decode(uint64_t e, uint32_t pc, usse_insn *o) {
    (void)pc;
    o->enc = e;
    for (size_t i = 0; i < sizeof DECODERS / sizeof DECODERS[0]; i++) {
        if (DECODERS[i](e, o->text, sizeof o->text)) { o->valid = 1; return; }
    }
    /* Not yet decoded: readable class hint (invalid -> regression ignores it). */
    const char *base = usse_class_name((unsigned)(e >> 59));
    o->valid = 0;
    if (base) snprintf(o->text, sizeof o->text, "%-8s; TODO operands", base);
    else      snprintf(o->text, sizeof o->text, ".long 0x%016llX", (unsigned long long)e);
}
