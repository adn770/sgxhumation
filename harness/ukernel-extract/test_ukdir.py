"""Tests for ukdir.h: where fakedrm.c and loader.c find the staging directory.

fakedrm.c and loader.c only build as 32-bit Linux objects, so the path logic they share
lives in ukdir.h and is checked here through a small host program.
"""
import os
import pathlib
import shutil
import subprocess

import pytest

HERE = pathlib.Path(__file__).resolve().parent
CC = shutil.which("cc") or shutil.which("gcc")

PROBE = r"""
#include "ukdir.h"
int main(int argc, char **argv) {
    char buf[UK_PATH_MAX];
    puts(uk_path(buf, sizeof buf, argv[1]));
    return 0;
}
"""


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    if not CC:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("ukdir")
    (d / "probe.c").write_text(PROBE)
    exe = d / "probe"
    subprocess.run([CC, "-std=c11", "-Wall", "-Werror", "-I", str(HERE),
                    str(d / "probe.c"), "-o", str(exe)], check=True)
    return exe


def run(probe, rel, **env):
    base = {k: v for k, v in os.environ.items() if k not in ("SGX_UK_DIR", "HOME")}
    base.update(env)
    return subprocess.run([str(probe), rel], env=base, capture_output=True, text=True,
                          check=True).stdout.strip()


def test_env_var_names_the_directory(probe):
    assert run(probe, "drm.log", SGX_UK_DIR="/tmp/uk", HOME="/h") == "/tmp/uk/drm.log"


def test_default_is_under_home(probe):
    assert run(probe, "dump", HOME="/h") == "/h/sgxhumation-uk/dump"


def test_empty_env_var_falls_back_to_home(probe):
    assert run(probe, "lib/x.so", SGX_UK_DIR="", HOME="/h") == "/h/sgxhumation-uk/lib/x.so"


def test_no_home_uses_the_current_directory(probe):
    assert run(probe, "drm.log") == "./sgxhumation-uk/drm.log"
