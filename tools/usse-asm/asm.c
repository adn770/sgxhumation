/* asm.c — the source-file driver shared by usse-asm and pds-asm.
 *
 * One statement per line:
 *
 *   label:                       a label (may share the line with a statement)
 *   <instruction>                as the disassembler prints it
 *   .long 0x0123456789abcdef     a raw USSE word (.word for a PDS dword); several
 *                                values may follow, separated by commas
 *   ; comment                    to the end of the line (also C comments)
 *
 * Disassembler listings ("<index>: 0x<hex>\t<text>") are accepted too: the
 * index and hex columns are ignored, except that a PDS "data" line takes its
 * value from the hex column. In PDS mode the raw words before the first
 * instruction form the data segment, and a label's value is its instruction
 * index counted from the first instruction (as bra/call count). */
#include "asm.h"
#include "usse_form.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *text;         /* instruction text, or NULL for a raw word */
    uint64_t raw;
    unsigned line;
} item;

typedef struct {
    char *name;
    unsigned index;     /* word index */
} label;

typedef struct {
    item *items;
    size_t nitems, cap;
    label *labels;
    size_t nlabels, lcap;
    unsigned ndata;     /* data-segment words (PDS) */
    int errors;
    const char *file;
    const asm_isa *isa;
} state;

static void *grow(void *p, size_t *cap, size_t need, size_t size) {
    if (need <= *cap) return p;
    *cap = *cap ? *cap * 2 : 64;
    if (*cap < need) *cap = need;
    p = realloc(p, *cap * size);
    if (!p) { perror("realloc"); exit(2); }
    return p;
}

static void error(state *st, unsigned line, const char *fmt, const char *arg) {
    fprintf(stderr, "%s:%u: ", st->file, line);
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    st->errors++;
}

static void add(state *st, char *text, uint64_t raw, unsigned line) {
    st->items = grow(st->items, &st->cap, st->nitems + 1, sizeof *st->items);
    st->items[st->nitems++] = (item){ text, raw, line };
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int ident_start(int c) { return isalpha(c) || c == '_'; }
static int ident_char(int c) { return isalnum(c) || c == '_' || c == '.'; }

/* A disassembler listing prefix "<n>: 0x<hex>" followed by blanks: return
 * the rest of the line and set *hex. */
static char *listing(char *s, uint64_t *hex) {
    char *p = s;
    if (!isdigit((unsigned char)*p)) return NULL;
    while (isdigit((unsigned char)*p)) p++;
    if (*p++ != ':') return NULL;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) return NULL;
    char *e;
    errno = 0;
    *hex = strtoull(p + 2, &e, 16);
    if (e == p + 2 || errno || !isspace((unsigned char)*e)) return NULL;
    return trim(e);
}

/* A raw word: decimal or 0x hex, at most width bytes. */
static int parse_raw(const char *p, char **end, uint64_t *v, unsigned width) {
    int hex = p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    if (!isdigit((unsigned char)p[hex ? 2 : 0]) && !(hex && isxdigit((unsigned char)p[2])))
        return 0;
    errno = 0;
    *v = strtoull(p, end, hex ? 16 : 10);
    if (errno || ident_char((unsigned char)**end)) return 0;
    return width == 8 || *v <= 0xffffffffull;
}

/* Blank out C comments, so that a ';' inside one does not end the line. */
static void strip_c_comments(char *s) {
    for (char *c; (c = strstr(s, "/*")); ) {
        char *e = strstr(c + 2, "*/");
        e = e ? e + 2 : c + strlen(c);
        memset(c, ' ', (size_t)(e - c));
        s = e;
    }
}

static void parse_line(state *st, char *s, unsigned line) {
    char *c;
    strip_c_comments(s);
    c = strchr(s, ';');
    if (c) *c = '\0';
    s = trim(s);
    uint64_t hex;
    char *rest = listing(s, &hex);
    if (rest) {
        if (!strcmp(rest, "data")) add(st, NULL, hex, line);
        else if (*rest) add(st, strdup(rest), 0, line);
        return;
    }
    /* labels */
    while (ident_start((unsigned char)*s)) {
        char *p = s;
        while (ident_char((unsigned char)*p)) p++;
        if (*p != ':') break;
        *p = '\0';
        for (size_t i = 0; i < st->nlabels; i++)
            if (!strcmp(st->labels[i].name, s)) error(st, line, "label '%s' defined twice", s);
        st->labels = grow(st->labels, &st->lcap, st->nlabels + 1, sizeof *st->labels);
        st->labels[st->nlabels++] = (label){ strdup(s), (unsigned)st->nitems };
        s = trim(p + 1);
    }
    if (!*s) return;
    size_t rl = strlen(st->isa->raw);
    if (!strncmp(s, st->isa->raw, rl) && isspace((unsigned char)s[rl])) {
        for (char *p = s + rl; ; ) {
            char *e;
            uint64_t v;
            p = trim(p);
            if (!parse_raw(p, &e, &v, st->isa->width)) {
                error(st, line, "bad %s value", st->isa->raw);
                return;
            }
            add(st, NULL, v, line);
            p = trim(e);
            if (!*p) break;
            if (*p++ != ',') { error(st, line, "bad %s value", st->isa->raw); return; }
        }
        return;
    }
    add(st, strdup(s), 0, line);
}

static int resolve(const char *name, size_t len, uint64_t *v, void *ctx) {
    state *st = ctx;
    for (size_t i = 0; i < st->nlabels; i++) {
        if (strlen(st->labels[i].name) == len && !strncmp(st->labels[i].name, name, len)) {
            if (st->labels[i].index < st->ndata) return 0;
            *v = st->labels[i].index - st->ndata;
            return 1;
        }
    }
    return 0;
}

static int usage(const char *name) {
    fprintf(stderr, "usage: %s [-l] [-o out.bin] [--core 540|545] <source.s | ->\n", name);
    return 2;
}

int asm_main(int argc, char **argv, const asm_isa *isa) {
    const char *out = NULL, *in = NULL;
    int list = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-l")) list = 1;
        else if (!strcmp(argv[i], "--core") && i + 1 < argc) {
            extern void usse_set_core(int core);
            usse_set_core(atoi(argv[++i]));
        }
        else if (!in && (argv[i][0] != '-' || !strcmp(argv[i], "-"))) in = argv[i];
        else return usage(isa->name);
    }
    if (!in) return usage(isa->name);

    FILE *f = strcmp(in, "-") ? fopen(in, "r") : stdin;
    if (!f) { perror(in); return 2; }
    state st = { .file = strcmp(in, "-") ? in : "<stdin>", .isa = isa };
    char *buf = NULL;
    size_t cap = 0;
    unsigned line = 0;
    ssize_t n;
    while ((n = getline(&buf, &cap, f)) >= 0) {
        line++;
        parse_line(&st, buf, line);
    }
    free(buf);
    if (f != stdin) fclose(f);

    if (isa->data_segment)
        while (st.ndata < st.nitems && !st.items[st.ndata].text) st.ndata++;

    uint64_t *words = calloc(st.nitems ? st.nitems : 1, sizeof *words);
    if (!words) { perror("calloc"); return 2; }
    usse_form_set_resolver(resolve, &st);
    for (size_t i = 0; i < st.nitems; i++) {
        item *it = &st.items[i];
        if (!it->text) { words[i] = it->raw; continue; }
        if (!isa->encode(it->text, &words[i]))
            error(&st, it->line, "cannot assemble '%s'", it->text);
    }
    usse_form_set_resolver(NULL, NULL);
    if (st.errors) return 1;

    if (out) {
        FILE *o = fopen(out, "wb");
        if (!o) { perror(out); return 2; }
        for (size_t i = 0; i < st.nitems; i++)
            for (unsigned b = 0; b < isa->width; b++) fputc((int)(words[i] >> (8 * b) & 0xff), o);
        if (fclose(o)) { perror(out); return 2; }
    }
    if (list)
        for (size_t i = 0; i < st.nitems; i++)
            isa->list(stdout, (unsigned)i, words[i], i < st.ndata);
    return 0;
}
