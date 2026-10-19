/* main.c — usse-dis CLI. Reads a raw little-endian USSE blob (e.g. the extracted
 * ukernel) and prints one decoded instruction per 8-byte word, in a format that
 * lines up with the SGX540 oracle disassembly for regression diffing:
 *
 *   <index>: 0x<ENC16>\t<mnemonic>
 *
 * Usage: usse-dis <file.bin> [max_words] */
#define _GNU_SOURCE
#include "usse.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.bin> [max]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    long max = (argc > 2) ? strtol(argv[2], 0, 0) : -1;

    unsigned char b[8];
    uint32_t pc = 0;
    while (fread(b, 1, 8, f) == 8) {
        uint64_t e = 0;
        for (int i = 7; i >= 0; i--) e = (e << 8) | b[i];   /* little-endian */
        usse_insn in;
        usse_decode(e, pc, &in);
        printf("%6u: 0x%016llX\t%s\n", pc, (unsigned long long)e, in.text);
        pc++;
        if (max >= 0 && (long)pc >= max) break;
    }
    fclose(f);
    return 0;
}
