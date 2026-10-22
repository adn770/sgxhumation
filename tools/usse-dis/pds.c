// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* pds.c — SGX Series-5 PDS instruction decoder (see pds.h): the operand kinds
 * written as code, then the form table generated from isa/pds/ into
 * pds_table.inc (isa/README.md), which pds_decode() walks and the assembler
 * (tools/usse-asm) inverts.
 *
 * Word layout: opcode = w[31:27], predicate = w[26:24] (7 = always), and a 24-bit
 * operand body whose shape depends on the opcode group. Registers are the two
 * data-segment banks ds0/ds1 plus a few specials (ir0/ir1/ir2 input registers,
 * tim, pc, mr01/mr23). Any encoding that selects a reserved register or opcode
 * is reported as undecoded rather than guessed. */
#include "pds.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define BITS(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

static const char *const PRED[8] = { "p0", "p1", "p2", "if0", "if1", "aluz", "alun", NULL };

/* movs/movsa destinations; 10-14 are reserved. */
static const char *const MOVS_DST[16] = {
    "mr", "slc", "douti", "doutd", "doutt", "doutu", "douta", "tim",
    "doutc", "douts", NULL, NULL, NULL, NULL, NULL, "ir0",
};

/* Input/special register by index. The mov16/mov32 source fields also have pc,
 * which shifts the numbering; other groups expose only a prefix of the list. */
static const char *special(unsigned i, int with_pc) {
    static const char *const PLAIN[] = { "ir0", "ir1", "tim", "ir2" };
    static const char *const WITHPC[] = { "ir0", "ir1", "pc", "tim", "ir2" };
    if (with_pc) return i < 5 ? WITHPC[i] : NULL;
    return i < 4 ? PLAIN[i] : NULL;
}

const char *pds_pred_name(unsigned p) { return p < 8 ? PRED[p] : NULL; }
const char *pds_movs_dst_name(unsigned d) { return d < 16 ? MOVS_DST[d] : NULL; }
const char *pds_special_name(unsigned i, int with_pc) { return special(i, with_pc); }

/* ---- operand kinds ------------------------------------------------------- */

/* Predicate w[26:24] as a "(p0) " prefix; 7 (always) prints nothing. */
static int k_pred_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    const char *p = pds_pred_name(v);
    (void)k;
    if (p) snprintf(out, n, "(%s) ", p);
    else   out[0] = '\0';
    return 1;
}

static int k_port_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    const char *p = pds_movs_dst_name(v);
    (void)k;
    if (!p) return 0;
    snprintf(out, n, "%s", p);
    return 1;
}

/* movs/movsa sources, v = w[23:5]: four 2-bit selectors [7:0] (slot s at
 * 2s: bit 0 the high half, bit 1 ds1), the ds1 pair [12:8], the ds0 pair or
 * input register [17:13], and [18] which of the two that field names. Slots
 * print from 3 down to 0. */
static int k_movs_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    unsigned b1 = (v >> 8 & 31) * 2, b0 = v >> 13 & 31, ir = v >> 18 & 1;
    int uses_base = 0;
    size_t at = 0;
    (void)k;
    for (int s = 0; s < 4; s++) uses_base |= !(v >> (2 * s + 1) & 1);
    if (ir && uses_base && !pds_special_name(b0, 0)) return 0;
    out[0] = '\0';
    for (int s = 3; s >= 0; s--) {
        unsigned sel = v >> (2 * s) & 3, hi = sel & 1;
        const char *sep = s == 3 ? "" : ", ";
        if (sel & 2)  at += (size_t)snprintf(out + at, n - at, "%sds1[%u]", sep, b1 + hi);
        else if (ir)  at += (size_t)snprintf(out + at, n - at, "%s%s.%c", sep,
                                             pds_special_name(b0, 0), hi ? 'h' : 'l');
        else          at += (size_t)snprintf(out + at, n - at, "%sds0[%u]", sep, b0 * 2 + hi);
        if (at >= n) return 0;
    }
    return 1;
}

static const char *skip_ws(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    return s;
}

/* "ds<bank>[<n>]": 1 and the bank and index, else 0. */
static int parse_ds(const char *s, const char **end, unsigned *bank, unsigned *idx) {
    uint64_t x;
    if (strncmp(s, "ds", 2) || (s[2] != '0' && s[2] != '1') || s[3] != '[') return 0;
    if (!usse_parse_uint(s + 4, end, &x) || **end != ']' || x > 127) return 0;
    *bank = (unsigned)(s[2] - '0');
    *idx = (unsigned)x;
    (*end)++;
    return 1;
}

/* The inverse of k_movs_print. Fields no operand uses stay 0. */
static int k_movs_parse(const usse_kind *k, const char *s, const char **end, uint32_t *v) {
    int b0 = -1, b1 = -1, ir = -1;
    uint32_t sel = 0;
    (void)k;
    for (int i = 0; i < 4; i++) {
        unsigned bank, idx, slot = 3 - (unsigned)i, hi;
        const char *e;
        s = skip_ws(s);
        if (i) {
            if (*s != ',') return 0;
            s = skip_ws(s + 1);
        }
        if (parse_ds(s, &e, &bank, &idx)) {
            hi = idx & 1;
            idx >>= 1;
            if (idx > 31) return 0;
            if (bank) {
                if (b1 >= 0 && b1 != (int)idx) return 0;
                b1 = (int)idx;
                sel |= (2 | hi) << (2 * slot);
            } else {
                if (ir == 1 || (b0 >= 0 && b0 != (int)idx)) return 0;
                ir = 0;
                b0 = (int)idx;
                sel |= hi << (2 * slot);
            }
        } else {
            unsigned r;
            for (r = 0; r < 4; r++) {
                const char *name = pds_special_name(r, 0);
                size_t l = strlen(name);
                if (!strncmp(s, name, l) && s[l] == '.' && (s[l + 1] == 'l' || s[l + 1] == 'h')) {
                    hi = s[l + 1] == 'h';
                    e = s + l + 2;
                    break;
                }
            }
            if (r == 4 || ir == 0 || (b0 >= 0 && b0 != (int)r)) return 0;
            ir = 1;
            b0 = (int)r;
            sel |= hi << (2 * slot);
        }
        s = e;
    }
    *v = sel | (uint32_t)(b1 < 0 ? 0 : b1) << 8 | (uint32_t)(b0 < 0 ? 0 : b0) << 13 |
         (uint32_t)(ir == 1) << 18;
    *end = s;
    return 1;
}

/* 7-bit destination ds0[0-63], ds1[0-63]. */
static int k_dst7_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    snprintf(out, n, "ds%u[%u]", v >> 6, v & 63);
    return 1;
}

/* 7-bit first source: ds0[0-63] and the first arg input registers. */
static int k_src7_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    unsigned nspecial = (unsigned)(uintptr_t)k->arg;
    if (v < 64) { snprintf(out, n, "ds0[%u]", v); return 1; }
    if (v - 64 < nspecial) { snprintf(out, n, "%s", pds_special_name(v - 64, 0)); return 1; }
    return 0;
}

/* mov16 source, 9 bits in 16-bit halves: ds0, ds1, then specials with pc. */
static int k_src9h_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    char hl = (v & 1) ? 'h' : 'l';
    unsigned r = v >> 1;
    (void)k;
    if (r < 128) snprintf(out, n, "ds%u[%u].%c", r >> 6, r & 63, hl);
    else if (pds_special_name(r - 128, 1))
        snprintf(out, n, "%s.%c", pds_special_name(r - 128, 1), hl);
    else return 0;
    return 1;
}

/* mov32 source, 8 bits: ds0, ds1, then specials with pc. */
static int k_src8_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    if (v < 128) snprintf(out, n, "ds%u[%u]", v >> 6, v & 63);
    else if (pds_special_name(v - 128, 1)) snprintf(out, n, "%s", pds_special_name(v - 128, 1));
    else return 0;
    return 1;
}

/* mov64 source, 7 bits: even pairs of ds0/ds1, then mr01, mr23. */
static int k_src64_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    if (v < 64) snprintf(out, n, "ds%u[%u]", v >> 5, (v & 31) * 2);
    else if (v < 66) snprintf(out, n, "mr%s", v == 64 ? "01" : "23");
    else return 0;
    return 1;
}

/* mov128 source pair, 5 bits, below 16. */
static int k_src128_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    if (v >= 16) return 0;
    snprintf(out, n, "%u", v * 2);
    return 1;
}

/* load/store registers: v = first[3:0] (from ds48) | count[6:4] (1-8, 0 is 8). */
static int k_ldrange_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    unsigned r = 48 + (v & 15), cnt = ((v >> 4) - 1) & 7;
    (void)k;
    if (cnt) snprintf(out, n, "%u-%u]", r, r + cnt);
    else     snprintf(out, n, "%u]", r);
    return 1;
}

/* mul first source, 8 bits in halves: ds0, then ir0/ir1/tim. */
static int k_mula_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    char hl = (v & 1) ? 'h' : 'l';
    (void)k;
    if (v < 128) snprintf(out, n, "ds0[%u].%c", v >> 1, hl);
    else if ((v - 128) >> 1 < 3) snprintf(out, n, "%s.%c", pds_special_name((v - 128) >> 1, 0), hl);
    else return 0;
    return 1;
}

static int k_half_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    snprintf(out, n, "ds1[%u].%c", v >> 1, (v & 1) ? 'h' : 'l');
    return 1;
}

/* shl/shr source, 8 bits: ds0/ds1, then ir0, ir1. */
static int k_shsrc_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    if (v < 128) snprintf(out, n, "ds%u[%u]", v >> 6, v & 63);
    else if (v - 128 < 2) snprintf(out, n, "%s", pds_special_name(v - 128, 0));
    else return 0;
    return 1;
}

/* Signed 7-bit shift amount, printed as an unsigned 32-bit number. */
static int k_shamt_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    snprintf(out, n, "%u", (uint32_t)((int32_t)(v << 25) >> 25));
    return 1;
}

static int k_shamt_parse(const usse_kind *k, const char *s, const char **end, uint32_t *v) {
    uint64_t x;
    (void)k;
    if (!usse_parse_uint(s, end, &x)) return 0;
    if (x < 64) *v = (uint32_t)x;
    else if (x >= 0xffffffc0ull) *v = (uint32_t)x & 0x7f;
    else return 0;
    return 1;
}

#define KIND(name, print, parse, arg) static const usse_kind name = { print, parse, arg }
KIND(K_PRED, k_pred_print, NULL, NULL);
KIND(K_PORT, k_port_print, NULL, NULL);
KIND(K_MOVS, k_movs_print, k_movs_parse, NULL);
KIND(K_DST7, k_dst7_print, NULL, NULL);
KIND(K_SRC7_2, k_src7_print, NULL, (void *)(uintptr_t)2);
KIND(K_SRC7_3, k_src7_print, NULL, (void *)(uintptr_t)3);
KIND(K_SRC9H, k_src9h_print, NULL, NULL);
KIND(K_SRC8, k_src8_print, NULL, NULL);
KIND(K_SRC64, k_src64_print, NULL, NULL);
KIND(K_SRC128, k_src128_print, NULL, NULL);
KIND(K_LDRANGE, k_ldrange_print, NULL, NULL);
KIND(K_MULA, k_mula_print, NULL, NULL);
KIND(K_HALF, k_half_print, NULL, NULL);
KIND(K_SHSRC, k_shsrc_print, NULL, NULL);
KIND(K_SHAMT, k_shamt_print, k_shamt_parse, NULL);

/* ---- the table: data kinds and forms, generated from isa/pds/ ------------ */
#include "pds_table.inc"

int pds_decode(uint32_t w, char *text, unsigned len) {
    if (usse_form_decode_in(&pds_table, w, text, len)) return 1;
    snprintf(text, len, ".word 0x%08X", w);
    return 0;
}

pds_op_type pds_classify(uint32_t w) {
    static const pds_op_type T[32] = {
        PDS_T_MOV,  PDS_T_MOV,  PDS_T_MOV,  PDS_T_MOV,  PDS_T_MOV,  PDS_T_MOV,  PDS_T_LOAD, PDS_T_STORE,
        PDS_T_ADD,  PDS_T_SUB,  PDS_T_ADD,  PDS_T_SUB,  PDS_T_MUL,  PDS_T_ABS,  PDS_T_UNK,  PDS_T_SYNC,
        PDS_T_CMP,  PDS_T_CMP,  PDS_T_JMP,  PDS_T_CALL, PDS_T_RET,  PDS_T_HALT, PDS_T_NOP,  PDS_T_UNK,
        PDS_T_OR,   PDS_T_AND,  PDS_T_XOR,  PDS_T_NOT,  PDS_T_NOR,  PDS_T_NAND, PDS_T_SHL,  PDS_T_SHR,
    };
    unsigned op = BITS(w, 31, 27), dst = BITS(w, 3, 0);
    if (op == 0x00 || op == 0x05)               /* movs/movsa: dout* ports are I/O */
        return (dst >= 2 && dst <= 6) || dst == 8 || dst == 9 ? PDS_T_IO : PDS_T_MOV;
    return T[op];
}

int pds_predicated(uint32_t w) {
    unsigned op = BITS(w, 31, 27);
    if (op == 0x06 || op == 0x07 || op == 0x0f || op == 0x16)
        return 0;
    return BITS(w, 26, 24) != 7;
}

int pds_branch(uint32_t w, int *is_call, int *is_cond, uint32_t *target) {
    unsigned op = BITS(w, 31, 27);
    if (op != 0x12 && op != 0x13)
        return 0;
    *is_call = op == 0x13;
    *is_cond = pds_predicated(w);
    *target = BITS(w, 18, 0);
    return 1;
}
