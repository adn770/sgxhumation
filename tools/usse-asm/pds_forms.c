// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* pds_forms.c — the PDS assembler: text to words by inverting the PDS form
 * table (tools/usse-dis/pds.c, generated from isa/pds/), the table the decoder
 * walks, so an encoding is accepted only if the decoder prints it with the
 * form's text. */
#include "pds_asm.h"
#include "pds.h"

int pds_encode(const char *text, uint32_t *w) {
    uint64_t e;
    if (!usse_form_encode_in(&pds_table, text, &e)) return 0;
    *w = (uint32_t)e;
    return 1;
}
