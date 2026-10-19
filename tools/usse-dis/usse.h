/* usse.h — SGX Series-5 USSE instruction decoder (narrow v1: ukernel subset).
 *
 * The USSE instruction word is 64-bit little-endian. The top 5 bits (enc[63:59])
 * select the major opcode class; class 0x1F (0xF8..0xFF) is the control/special
 * group with further sub-opcode bits. All field layouts in usse.c are derived by
 * bit-correlation against the SGX540 ground-truth disassembly (see
 * tools/usse-dis/README.md and debug/usse/solve_bits.py), not guessed.
 *
 * This core is standalone (no radare2 dependency) so it can be regression-tested
 * against the oracle via the usse-dis CLI, then wrapped as an r2 RArch plugin. */
#ifndef USSE_H
#define USSE_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      valid;          /* 1 if fully decoded, 0 if opcode unknown */
    uint64_t enc;            /* the raw 64-bit instruction word */
    char     text[128];      /* decoded mnemonic + operands (oracle-compatible) */
} usse_insn;

/* Decode one 64-bit USSE word. `pc` is the instruction index (for future
 * branch-target resolution). Always fills out->text (".long 0x…" if unknown). */
void usse_decode(uint64_t enc, uint32_t pc, usse_insn *out);

/* Base mnemonic for a top-5-bit opcode class, or NULL if unmapped. */
const char *usse_class_name(unsigned top5);

#endif /* USSE_H */
