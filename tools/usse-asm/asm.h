/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Josep Torra */
/* asm.h — the source-file driver shared by usse-asm and pds-asm. */
#ifndef ASM_H
#define ASM_H
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *name;           /* program name for messages */
    unsigned width;             /* bytes per word: 8 (USSE) or 4 (PDS) */
    const char *raw;            /* raw-word directive: ".long" or ".word" */
    int data_segment;           /* raw words before the first instruction are data,
                                 * and labels count from the first instruction */
    int (*encode)(const char *text, uint64_t *w);
    /* one listing line, as the matching disassembler prints it */
    void (*list)(FILE *f, unsigned idx, uint64_t w, int is_data);
} asm_isa;

int asm_main(int argc, char **argv, const asm_isa *isa);

#endif /* ASM_H */
