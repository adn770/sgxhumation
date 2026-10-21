// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* pdsmain.c — pds-dis CLI. Disassembles a raw PDS program: little-endian 32-bit
 * words, the first <data_dwords> of which are the data segment. Output lines up
 * with refdisasm/refpds545 for regression diffing:
 *
 *   <index>: 0x<HEX8>\t<text>
 *
 * Usage: pds-dis <program.bin> <data_dwords> */
#include "pds.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <program.bin> <data_dwords>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    unsigned data = (unsigned)strtoul(argv[2], 0, 0), i = 0;
    unsigned char b[4];
    while (fread(b, 1, 4, f) == 4) {
        uint32_t w = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
        char txt[128] = "data";
        if (i >= data) pds_decode(w, txt, sizeof txt);
        printf("%u: 0x%08X\t%s\n", i, w, txt);
        i++;
    }
    fclose(f);
    return 0;
}
