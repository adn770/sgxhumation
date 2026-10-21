// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* usse_main.c — usse-asm: assemble USSE source into a raw little-endian blob
 * of 64-bit words, the inverse of usse-dis (see README.md). */
#include "asm.h"
#include "usse.h"
#include "usse_form.h"

static int encode(const char *text, uint64_t *w) {
    return usse_form_encode(text, w);
}

static void list(FILE *f, unsigned idx, uint64_t w, int is_data) {
    usse_insn in;
    (void)is_data;
    usse_decode(w, idx, &in);
    fprintf(f, "%6u: 0x%016llX\t%s\n", idx, (unsigned long long)w, in.text);
}

int main(int argc, char **argv) {
    static const asm_isa isa = { "usse-asm", 8, ".long", 0, encode, list };
    return asm_main(argc, argv, &isa);
}
