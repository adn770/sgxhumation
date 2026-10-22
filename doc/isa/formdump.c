// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* formdump.c — the USSE and PDS form tables as data, for the ISA documents
 * (doc/isa/build.py). It reads the tables the disassemblers and assemblers use
 * (usse_table.inc and pds_table.inc in tools/usse-dis, from isa/), so the
 * documents cannot drift from the tools.
 *
 *   formdump tables              every form of both tables, as JSON
 *   formdump decode usse|pds HEX the form that decodes a word and its fields
 *   formdump encode usse|pds TXT assemble one instruction, then decode it
 *
 * Field values are printed with the field's own kind: every value for fields
 * of up to 8 bits; for wider ones the first 256 values, each single bit, and
 * three numbers (0, 1, 127) under every 7-bit "bank" prefix, which is enough
 * to describe register operands (bank above a 7-bit number) and flag runs.
 *
 * pds_forms.c (the PDS assembler) is included here rather than linked. */
#include "usse.h"
#include "usse_form.h"
#include "pds.h"
#include "../../tools/usse-asm/pds_forms.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void jstr(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static const char *kind_class(const usse_kind *k) {
    if (k->print == usse_k_enum_print) return "enum";
    if (k->print == usse_k_num_print) return "num";
    return "other";
}

static int seen(const uint32_t *list, size_t n, uint32_t v) {
    for (size_t i = 0; i < n; i++)
        if (list[i] == v) return 1;
    return 0;
}

static void dump_values(const usse_field *f) {
    unsigned w = usse_field_width(f);
    uint64_t lim = w >= 32 ? 0xffffffffull : (1ull << w) - 1;
    static uint32_t list[1 << 12];
    size_t n = 0;
    uint64_t all = w <= 8 ? lim + 1 : 256;
    for (uint64_t v = 0; v < all && v <= lim; v++) list[n++] = (uint32_t)v;
    for (unsigned i = 0; i < w && i < 32; i++)
        if (!seen(list, n, 1u << i)) list[n++] = 1u << i;
    if (w > 8 && w <= 12)
        for (uint32_t b = 0; b < 1u << (w - 7); b++) {
            static const uint32_t LO[3] = { 0, 1, 127 };
            for (int j = 0; j < 3; j++) {
                uint32_t v = b << 7 | LO[j];
                if (!seen(list, n, v)) list[n++] = v;
            }
        }
    if (!seen(list, n, (uint32_t)lim)) list[n++] = (uint32_t)lim;
    printf("\"values\":[");
    for (size_t i = 0; i < n; i++) {
        char buf[USSE_TEXT_MAX];
        printf("%s[%u,", i ? "," : "", list[i]);
        if (f->k->print(f->k, list[i], buf, sizeof buf)) jstr(buf);
        else printf("null");
        putchar(']');
    }
    putchar(']');
}

static void dump_kind(const usse_kind *k) {
    printf("\"kind\":\"%s\"", kind_class(k));
    if (k->print == usse_k_num_print) {
        const usse_num *a = k->arg;
        printf(",\"fmt\":");
        jstr(a->fmt);
        printf(",\"mul\":%u,\"add\":%u", a->mul, a->add);
    }
    printf(",\"parse\":%s", k->parse ? "true" : "false");
}

static void dump_field(const usse_field *f) {
    printf("{\"chunks\":[");
    for (int i = 0; i < USSE_MAX_CHUNKS && f->c[i].w; i++)
        printf("%s[%u,%u]", i ? "," : "", f->c[i].lo, f->c[i].w);
    printf("],\"width\":%u,", usse_field_width(f));
    dump_kind(f->k);
    putchar(',');
    dump_values(f);
    putchar('}');
}

static void dump_table(const char *name, const usse_table *t, unsigned bits) {
    printf("\"%s\":{\"bits\":%u,\"forms\":[\n", name, bits);
    for (size_t i = 0; i < t->n; i++) {
        const usse_form *fm = &t->forms[i];
        printf("%s{\"index\":%zu,\"pat\":", i ? ",\n" : "", i);
        if (fm->pat) jstr(fm->pat);
        else printf("null");
        printf(",\"mask\":\"%016llX\",\"val\":\"%016llX\",\"dflt\":\"%016llX\","
               "\"bits_read\":\"%016llX\",\"fields\":[",
               (unsigned long long)fm->mask, (unsigned long long)fm->val,
               (unsigned long long)fm->dflt, (unsigned long long)usse_form_bits(fm));
        for (int j = 0; j < USSE_MAX_FIELDS && fm->f[j].k; j++) {
            if (j) putchar(',');
            dump_field(&fm->f[j]);
        }
        printf("]}");
    }
    printf("]}");
}

static const usse_table *table_of(const char *isa) {
    if (!strcmp(isa, "usse")) return &usse_table_main;
    if (!strcmp(isa, "pds")) return &pds_table;
    return NULL;
}

/* The form that decodes w and its fields' values and texts, as JSON. */
static int decode_word(const char *isa, const usse_table *t, uint64_t w) {
    char text[USSE_TEXT_MAX];
    const usse_form *fm = NULL;
    if (!strcmp(isa, "pds")) {
        if (!pds_decode((uint32_t)w, text, sizeof text)) return 0;
        for (size_t i = 0; i < t->n && !fm; i++) {
            char again[USSE_TEXT_MAX];
            const usse_form *c = &t->forms[i];
            if (c->pat && (w & c->mask) == c->val && usse_form_render(c, w, again, sizeof again) &&
                !strcmp(again, text))
                fm = c;
        }
    } else {
        fm = usse_form_decode_in(t, w, text, sizeof text);
    }
    if (!fm) return 0;
    printf("{\"word\":\"%016llX\",\"form\":%ld,\"text\":", (unsigned long long)w,
           (long)(fm - t->forms));
    jstr(text);
    printf(",\"fields\":[");
    for (int j = 0; j < USSE_MAX_FIELDS && fm->f[j].k; j++) {
        char buf[USSE_TEXT_MAX];
        uint32_t v = usse_field_get(&fm->f[j], w);
        if (!fm->f[j].k->print(fm->f[j].k, v, buf, sizeof buf)) buf[0] = '\0';
        printf("%s[%u,", j ? "," : "", v);
        jstr(buf);
        putchar(']');
    }
    printf("]}\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "tables")) {
        printf("{");
        dump_table("usse", &usse_table_main, 64);
        printf(",\n");
        dump_table("pds", &pds_table, 32);
        printf("}\n");
        return 0;
    }
    if (argc == 4 && table_of(argv[2])) {
        const usse_table *t = table_of(argv[2]);
        uint64_t w;
        if (!strcmp(argv[1], "decode")) {
            w = strtoull(argv[3], NULL, 16);
        } else if (!strcmp(argv[1], "encode")) {
            if (!strcmp(argv[2], "pds")) {
                uint32_t p;
                if (!pds_encode(argv[3], &p)) {
                    fprintf(stderr, "formdump: cannot assemble: %s\n", argv[3]);
                    return 1;
                }
                w = p;
            } else if (!usse_form_encode(argv[3], &w)) {
                fprintf(stderr, "formdump: cannot assemble: %s\n", argv[3]);
                return 1;
            }
        } else {
            goto usage;
        }
        if (!decode_word(argv[2], t, w)) {
            fprintf(stderr, "formdump: does not decode: %016llX\n", (unsigned long long)w);
            return 1;
        }
        return 0;
    }
usage:
    fprintf(stderr, "usage: %s tables | decode usse|pds HEX | encode usse|pds TEXT\n", argv[0]);
    return 2;
}
