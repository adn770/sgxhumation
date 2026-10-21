# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for edmtrace.py — a linear value tracer over usse-dis listings of the EDM.

Synthetic listings only: the encodings are zeros and the instructions are text we wrote,
in the format usse-dis prints. One check runs on the gitignored ukernel when it is present."""
import pathlib
import shutil
import subprocess

import pytest

import edmtrace

HERE = pathlib.Path(__file__).resolve().parent
UKERNEL = HERE / "extracted" / "sgx545_ukernel_template.bin"
USSE_DIS = HERE.parent.parent / "tools" / "usse-dis" / "usse-dis"

NAMES = {0x204: "cr_te_aa", 0x208: "cr_te_mtile_1", 0x200: "cr_vdm_start",
         0x12C: "EUR_CR_EVENT_STATUS"}


def listing(*texts, start=0):
    return [f"{start + i:6d}: 0x{0:016X}\t{t}" for i, t in enumerate(texts)]


def trace(lines, seeds=None, labels=None, start=None, end=None):
    insns = edmtrace.parse(lines)
    lo = insns[0].index if start is None else start
    hi = insns[-1].index if end is None else end
    return edmtrace.trace(insns, lo, hi, names=NAMES, seeds=seeds or {}, labels=labels or {})


def texts(events):
    return [e.text for e in events]


# --- parsing --------------------------------------------------------------------------------

def test_parse_splits_predicate_and_mnemonic():
    insns = edmtrace.parse(listing("p0 ba #0x00000010", "!p1 mov r1, #0x00000002", "nop"))
    assert [(i.index, i.pred, i.op) for i in insns] == [(0, "p0", "ba"), (1, "!p1", "mov"),
                                                         (2, None, "nop")]
    assert insns[1].args == ["r1", "#0x00000002"]


def test_parse_keeps_bracketed_operands_whole():
    (i,) = edmtrace.parse(listing("ldad.bpcache.fetch2 r4, [r2,#0++], drc0"))
    assert i.op == "ldad" and i.mods == ["bpcache", "fetch2"]
    assert i.args == ["r4", "[r2,#0++]", "drc0"]


def test_r2_labels_are_instruction_indices():
    labels = edmtrace.load_labels(["f edm.ta @ 0x8e8", "CCu a comment @ 0x8e8",
                                   "f hwperf.log @ 0xcbc8"])
    assert labels == {0x11D: "edm.ta", 0x1979: "hwperf.log"}


# --- memory operand semantics (tools/usse-dis/README.md) -------------------------------------

@pytest.mark.parametrize("mods,operand,dwords,step", [
    ([], "[r1,+#5]", [5], 0),                        # plain: b + k
    ([], "[r1,-#2]", [-2], 0),
    ([], "[r2,#1++]", [0], 1),                       # post-increment: b, then b += k
    (["fetch4"], "[r1,+#24]", [25, 26, 27, 28], 0),  # fetch: one element after the offset
    (["fetch2"], "[r2,#0++]", [0, 1], 2),            # fetch, post-increment: b += N
    (["fetch2"], "[r1,#82++]", [82, 83], 2),
])
def test_memory_operand_dwords(mods, operand, dwords, step):
    assert edmtrace.mem_dwords(mods, operand) == ("r" + operand[2], dwords, step)


def test_register_offsets_are_not_resolved():
    assert edmtrace.mem_dwords([], "[r17,+r19]") is None


# --- tracing --------------------------------------------------------------------------------

def test_mmio_write_of_a_constant_and_of_an_immediate():
    ev = trace(listing("mov r2, #0x00004000", "str #75, r2", "str #128, #1"))
    assert texts(ev) == ["W EVENT_STATUS <- 0x4000", "W cr_vdm_start <- 0x1"]
    assert [e.index for e in ev] == [1, 2]


def test_mmio_read_names_the_register():
    ev = trace(listing("ldr r4, #75, drc0", "str #128, r4"))
    assert texts(ev) == ["R EVENT_STATUS -> r4", "W cr_vdm_start <- EVENT_STATUS"]


def test_constants_fold_through_or_and_and_not():
    ev = trace(listing("mov r1, #0x00010000", "or r1, r1, #0x00000011",
                       "and r1, r1, ~#0x00000001", "str #128, r1"))
    assert texts(ev)[-1] == "W cr_vdm_start <- 0x10010"


def test_struct_fields_flow_from_loads_to_register_writes():
    # r1 = a TA command; the TA register block starts at byte 0x238 (dword 142)
    lines = listing("mov r2, #0x00000238", "imae.u r2, r2.low, #1.low, r1, u32",
                    "ldad.bpcache.fetch2 r3, [r2,#0++], drc0", "wdf drc0",
                    "str #129, r3", "str #130, r4", "ldad.bpcache r5, [r2,#1++], drc0",
                    "str #129, r5")
    ev = trace(lines, seeds={"r1": ("SGXMKIF_CMDTA", 0)})
    assert texts(ev) == [
        "ld SGXMKIF_CMDTA.ta_regs.teaa, SGXMKIF_CMDTA.ta_regs.te_mtile_1",
        "W cr_te_aa <- SGXMKIF_CMDTA.ta_regs.teaa",
        "W cr_te_mtile_1 <- SGXMKIF_CMDTA.ta_regs.te_mtile_1",
        "ld SGXMKIF_CMDTA.ta_regs.te_mtile_2",       # post-increment moved r2 by 2
        "W cr_te_aa <- SGXMKIF_CMDTA.ta_regs.te_mtile_2",
    ]


def test_memory_stores_name_both_sides():
    lines = listing("ldad.bpcache r2, [r0,+#7], drc1", "stad.bpcache [r1,+#53], r2",
                    "stad.bpcache [r1,+#52], #0")
    ev = trace(lines, seeds={"r0": ("SGXMKIF_CMDTA", 0), "r1": ("hw_render_details", 0)})
    assert texts(ev) == ["ld SGXMKIF_CMDTA.render_flags",
                         "st hw_render_details.render_flags <- "
                         "SGXMKIF_CMDTA.render_flags",
                         "st hw_render_details.sAccessDevAddr <- 0x0"]


def test_pointer_fields_can_be_followed_with_typed_seeds():
    # cmd.hw_render_details_dev_addr (dword 14) is a render-details pointer
    lines = listing("ldad.bpcache r4, [r1,+#14], drc0", "ldad.bpcache r5, [r4,+#53], drc0")
    ev = trace(lines, seeds={"r1": ("SGXMKIF_CMDTA", 0)})
    assert texts(ev)[1] == "ld hw_render_details.render_flags"


def test_unknown_structs_print_dword_indices():
    ev = trace(listing("ldad.bpcache r2, [sa0,+#25], drc0", "stad.bpcache [sa0,+#20], r2"),
               seeds={"sa0": ("TA3DCtl", 0)})
    assert texts(ev) == ["ld TA3DCtl[25]", "st TA3DCtl[20] <- TA3DCtl[25]"]


def test_other_writes_and_predicated_writes_forget_values():
    lines = listing("mov r3, #0x00000001", "shl r3, r3, #0x00000004", "str #128, r3",
                    "mov r4, #0x00000002", "p0 mov r4, #0x00000003", "str #128, r4")
    assert texts(trace(lines)) == ["W cr_vdm_start <- r3", "W cr_vdm_start <- r4"]


def test_calls_are_listed_with_labels():
    lines = listing("mov r23, pclink", "ba.savelink #0x00000004", "mov pclink, r23", "nop",
                    "lapc")
    ev = trace(lines, labels={4: "edm.helper"})
    assert texts(ev) == ["call 0x4 edm.helper"]


def test_range_limits_the_events():
    lines = listing("str #128, #1", "str #128, #2", "str #128, #3")
    assert [e.index for e in trace(lines, start=1, end=1)] == [1]


def test_format_has_the_index_and_text():
    ev = trace(listing("str #128, #1", start=0x95E))
    assert edmtrace.format_events(ev) == "0x095e  W cr_vdm_start <- 0x1"


# --- the real ukernel (optional, gitignored) -----------------------------------------------

@pytest.mark.skipif(not UKERNEL.exists() or not USSE_DIS.exists(),
                    reason="needs the captured ukernel and a built usse-dis")
def test_ta_handler_loads_the_ta_registers_from_the_command():
    out = subprocess.run([str(USSE_DIS), str(UKERNEL)], capture_output=True, text=True,
                         check=True).stdout.splitlines()
    insns = edmtrace.parse(out)
    names = {0x204: "cr_te_aa", 0x238: "cr_vdm_ctrl_stream_base",
             0xC90: "EUR_CR_BIF_TA_REQ_BASE"}
    ev = edmtrace.trace(insns, 0x95A, 0x985, names=names, labels={},
                        seeds={"r1": ("SGXMKIF_CMDTA", 0)})
    got = set(texts(ev))
    assert "W cr_te_aa <- SGXMKIF_CMDTA.ta_regs.teaa" in got
    assert "W cr_vdm_ctrl_stream_base <- SGXMKIF_CMDTA.ta_regs.vdm_ctrl_stream_base" in got
    assert "W BIF_TA_REQ_BASE <- SGXMKIF_CMDTA.ta_regs.bif_ta_req_base" in got
