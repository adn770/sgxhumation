# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for triopts.h: the options of tri.c for differential 3D captures.

tri.c only builds as a 32-bit Linux program against the closed GLES2 stack, so its
argument parsing and the geometry it draws live in triopts.h and are checked here through
a small host program that prints the parsed state (tri_dump) and the vertex data.
Each option changes one piece of GL state against the default triangle, so a capture with
the option can be diffed against the default capture (diff3d.py).
"""
import pathlib
import shutil
import subprocess

import pytest

HERE = pathlib.Path(__file__).resolve().parent
CC = shutil.which("cc") or shutil.which("gcc")

PROBE = r"""
#include <stdio.h>
#include "triopts.h"
int main(int argc, char **argv) {
    struct tri_opts o;
    char err[128];
    if (tri_parse(&o, argc - 1, argv + 1, err, sizeof err)) {
        printf("error %s\n", err);
        return 1;
    }
    tri_dump(stdout, &o);
    float v[TRI_MAX_VERTS * TRI_VERT_FLOATS];
    int n = tri_geometry(&o, v);
    printf("verts %d\n", n);
    for (int i = 0; i < n; i++) {
        const float *p = v + i * TRI_VERT_FLOATS;
        printf("v%d %g %g %g %g | %g %g %g %g\n", i, p[0], p[1], p[2], p[3],
               p[4], p[5], p[6], p[7]);
    }
    for (int k = 0; k < 2; k++) {
        const char *s = k ? tri_vertex_shader(&o) : tri_fragment_shader(&o);
        printf(k ? "vs " : "fs ");
        for (; *s; s++) putchar(*s == '\n' ? ' ' : *s);
        putchar('\n');
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    if not CC:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("triopts")
    (d / "probe.c").write_text(PROBE)
    exe = d / "probe"
    subprocess.run([CC, "-std=c11", "-Wall", "-Werror", "-I", str(HERE),
                    str(d / "probe.c"), "-o", str(exe)], check=True)
    return exe


def run(probe, *args, ok=True):
    r = subprocess.run([str(probe), *args], capture_output=True, text=True)
    assert (r.returncode == 0) == ok, r.stdout + r.stderr
    out = {}
    for line in r.stdout.splitlines():
        key, _, val = line.partition(" ")
        out[key] = val
    return out


def test_defaults_are_the_original_triangle(probe):
    o = run(probe)
    assert o["size"] == "64x64"
    assert o["format"] == "565"
    assert o["color"] == "1 0.5 0.25 1"
    assert o["clear"] == "0 0 1 1"
    assert o["prim"] == "0x4"            # GL_TRIANGLES
    assert o["depth"] == "off" and o["blend"] == "off" and o["cull"] == "off"
    assert o["scissor"] == "off" and o["mask"] == "1111" and o["dither"] == "1"
    assert o["viewport"] == "0 0 64 64"
    assert o["verts"] == "3"
    assert o["v0"] == "-0.5 -0.5 0 1 | 0 0 0 0"
    assert o["v2"] == "0 0.5 0 1 | 0 0 0 0"
    assert "vec4(1.0000, 0.5000, 0.2500, 1.0000)" in o["fs"]
    assert o["vs"] == "attribute vec4 pos; void main() { gl_Position = pos; } "


def test_size_moves_the_default_viewport(probe):
    o = run(probe, "size=100x60")
    assert o["size"] == "100x60" and o["viewport"] == "0 0 100 60"
    assert run(probe, "size=128x32", "viewport=8,4,16,16")["viewport"] == "8 4 16 16"


def test_colours(probe):
    o = run(probe, "color=0,1,0,0.5", "clear=0.25,0.5,0.75,1")
    assert o["color"] == "0 1 0 0.5" and o["clear"] == "0.25 0.5 0.75 1"
    assert "vec4(0.0000, 1.0000, 0.0000, 0.5000)" in o["fs"]
    assert run(probe, "noclear")["clear"] == "none"


def test_depth_options(probe):
    o = run(probe, "depth=less", "cleardepth=0.5", "z=0.25")
    assert o["depth"] == "0x201 clear=0.5 write=1"
    assert o["v1"] == "0.5 -0.5 0.25 1 | 0 0 0 0"
    assert run(probe, "depth=always", "depthmask=0")["depth"] == "0x207 clear=1 write=0"
    for name, val in (("never", 0x200), ("equal", 0x202), ("lequal", 0x203),
                      ("greater", 0x204), ("notequal", 0x205), ("gequal", 0x206)):
        assert run(probe, f"depth={name}")["depth"].startswith(f"{val:#x} ")


def test_blend_options(probe):
    assert run(probe, "blend=srcalpha,oneminussrcalpha")["blend"] == "0x302 0x303 eq=0x8006"
    assert run(probe, "blend=one,one", "blendeq=sub")["blend"] == "0x1 0x1 eq=0x800a"
    assert run(probe, "blend=dstcolor,zero", "blendeq=rsub")["blend"] == "0x306 0 eq=0x800b"


def test_cull_and_front_face(probe):
    assert run(probe, "cull=back")["cull"] == "0x405 front=0x901"
    assert run(probe, "cull=front", "frontface=cw")["cull"] == "0x404 front=0x900"
    assert run(probe, "cull=both")["cull"] == "0x408 front=0x901"


def test_mask_scissor_dither_stencil(probe):
    assert run(probe, "mask=1010")["mask"] == "1010"
    assert run(probe, "scissor=4,8,16,32")["scissor"] == "4 8 16 32"
    assert run(probe, "dither=0")["dither"] == "0"
    assert run(probe, "stencil=equal,3")["stencil"] == \
        "0x202 ref=3 mask=0xff write=0xff ops=0x1e00,0x1e00,0x1e01"
    o = run(probe, "stencil=less,5", "stencilmask=0x0f,0xf0", "stencilop=zero,incr,invert")
    assert o["stencil"] == "0x201 ref=5 mask=0xf write=0xf0 ops=0,0x1e02,0x150a"
    assert run(probe, "stencilop=decr,incrwrap,decrwrap")["stencilops"] == \
        "0x1e03 0x8507 0x8508"


def test_depth_range(probe):
    assert run(probe)["depthrange"] == "0 1"
    assert run(probe, "depthrange=0.25,0.75")["depthrange"] == "0.25 0.75"


def test_primitives(probe):
    assert run(probe, "prim=strip")["prim"] == "0x5"
    assert run(probe, "prim=strip")["verts"] == "4"
    assert run(probe, "prim=fan")["prim"] == "0x6"
    assert run(probe, "prim=two")["prim"] == "0x4"
    assert run(probe, "prim=two")["verts"] == "6"
    assert run(probe, "prim=points")["prim"] == "0x0"
    assert "gl_PointSize" in run(probe, "prim=points")["vs"]
    assert "gl_PointSize" not in run(probe)["vs"]
    assert run(probe, "prim=lines")["prim"] == "0x1"
    assert run(probe, "prim=lines")["verts"] == "4"
    assert run(probe, "prim=linestrip")["prim"] == "0x3"


def test_vertex_transform(probe):
    o = run(probe, "scale=2", "shift=0.25,-0.25")
    assert o["v0"] == "-0.75 -1.25 0 1 | 0 0 0 0"


def test_varying_colour_adds_an_attribute(probe):
    o = run(probe, "varying")
    assert o["varying"] == "1"
    assert o["v0"] == "-0.5 -0.5 0 1 | 1 0 0 1"
    assert o["v1"].endswith("| 0 1 0 1") and o["v2"].endswith("| 0 0 1 1")
    assert "varying" in o["fs"] and "gl_FragColor = col" in o["fs"]
    assert "attribute vec4 colour" in o["vs"]


def test_format_8888(probe):
    assert run(probe, "format=8888")["format"] == "8888"


def test_errors(probe):
    assert run(probe, "bogus=1", ok=False)["error"].startswith("unknown option")
    assert run(probe, "size=0x10", ok=False)["error"].startswith("bad size")
    assert run(probe, "depth=sometimes", ok=False)["error"].startswith("bad depth")
    assert run(probe, "format=444", ok=False)["error"].startswith("bad format")
