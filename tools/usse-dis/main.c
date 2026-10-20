/* main.c — usse-dis CLI. Reads a raw little-endian USSE blob (e.g. the extracted
 * ukernel) and prints one decoded instruction per 8-byte word, in a format that
 * lines up with the SGX540 oracle disassembly for regression diffing:
 *
 *   <index>: 0x<ENC16>\t<mnemonic>
 *
 * Usage: usse-dis [--which] [--core 540|545] <file.bin> [max_words]
 *        usse-dis --forms     list the form table: index, mask, value, the bits
 *                             the form reads (mask and fields), pattern
 *
 * --which appends "\t#<n>", the index of the form that decoded each word.
 * --core 540 follows the SGX540's reading where it differs (default 545). */
#define _GNU_SOURCE
#include "usse.h"
#include "usse_form.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.bin> [max] | --forms\n", argv[0]); return 2; }
    if (!strcmp(argv[1], "--forms")) {
        for (size_t i = 0; i < usse_nforms; i++) {
            const usse_form *fm = &usse_forms[i];
            printf("%zu\t%016llX\t%016llX\t%016llX\t%s\n", i, (unsigned long long)fm->mask,
                   (unsigned long long)fm->val, (unsigned long long)usse_form_bits(fm),
                   fm->pat ? fm->pat : "(reject)");
        }
        return 0;
    }
    int which = 0;
    while (argc > 2 && argv[1][0] == '-' && argv[1][1] == '-') {
        if (!strcmp(argv[1], "--which")) {
            which = 1;
        } else if (!strcmp(argv[1], "--core") && argc > 3) {
            usse_set_core(atoi(argv[2]));
            argv++;
            argc--;
        } else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[1]);
            return 2;
        }
        argv++;
        argc--;
    }
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
        printf("%6u: 0x%016llX\t%s", pc, (unsigned long long)e, in.text);
        if (which) {
            char t[USSE_TEXT_MAX];
            const usse_form *fm = usse_form_decode(e, t, sizeof t);
            printf("\t#%ld", fm ? (long)(fm - usse_forms) : -1L);
        }
        putchar('\n');
        pc++;
        if (max >= 0 && (long)pc >= max) break;
    }
    fclose(f);
    return 0;
}
