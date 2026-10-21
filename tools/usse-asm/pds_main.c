// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* pds_main.c — pds-asm: assemble PDS source (a data segment, then the
 * program) into a raw little-endian blob of 32-bit words, the inverse of
 * pds-dis (see README.md). */
#include "asm.h"
#include "pds.h"
#include "pds_asm.h"

static int encode(const char *text, uint64_t *w) {
    uint32_t v;
    if (!pds_encode(text, &v)) return 0;
    *w = v;
    return 1;
}

static void list(FILE *f, unsigned idx, uint64_t w, int is_data) {
    char text[128] = "data";
    if (!is_data) pds_decode((uint32_t)w, text, sizeof text);
    fprintf(f, "%u: 0x%08X\t%s\n", idx, (unsigned)w, text);
}

int main(int argc, char **argv) {
    static const asm_isa isa = { "pds-asm", 4, ".word", 1, encode, list };
    return asm_main(argc, argv, &isa);
}
