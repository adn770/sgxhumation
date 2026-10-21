# SPDX-License-Identifier: MIT
"""Tests for get-firmware.sh: fetch the Cedarview package, check it, cut the firmware.

Everything runs against a local HTTP server that serves tarballs made here from dummy text,
with the known hashes overridden through the environment and a fake cutter that records how
it was called. No closed bytes are involved. The one real download is opt-in:
SGX_NET_TESTS=1.
"""
import functools
import gzip
import hashlib
import http.server
import io
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import threading

import pytest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE / "get-firmware.sh"
README = HERE / "README.md"
REPO = HERE.parent.parent

INIT = b"dummy libsrv_init for the get-firmware tests\n"
UM = b"dummy libsrv_um for the get-firmware tests\n"
LICENCE = b"DUMMY LICENCE TEXT: these files are dummies.\n"
LIB = "./usr/lib/"
DOC = "./usr/share/doc/powervr/license.txt"
UBUNTU_TGZ = "cedarview-graphics-drivers_20120717.orig.tar.gz"
INTEL_TBZ = "cdv-gfx-drivers-1.0.3_bee.tar.bz2"
INNER = "cdv-gfx-drivers-1.0.3_bee/bin/cedarview-userspace-v1.0.3_bee.tar"

FAKE_CUTTER = """\
import pathlib, sys
log = pathlib.Path(sys.argv[1])
args = sys.argv[2:]
with log.open("a") as f:
    f.write(" ".join(args) + "\\n")
    for a in args:
        p = pathlib.Path(a)
        if p.is_file():
            f.write(f"{a} {p.read_bytes()!r}\\n")
out = args[args.index("-o") + 1]
pathlib.Path(out).write_bytes(b"fake firmware\\n")
"""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def tar_bytes(members: dict[str, bytes], mode: str) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode=mode) as tf:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def userspace(init: bytes = INIT) -> dict[str, bytes]:
    return {LIB + "libsrv_init.so.1.7.862890": init, LIB + "libsrv_um.so.1.7.862890": UM,
            DOC: LICENCE}


def ubuntu_package(init: bytes = INIT) -> bytes:
    return gzip.compress(tar_bytes(userspace(init), "w"), mtime=0)     # the same bytes each time


def intel_package(init: bytes = INIT) -> bytes:
    inner = tar_bytes(userspace(init), "w")
    return tar_bytes({"cdv-gfx-drivers-1.0.3_bee/LICENSE": b"top-level licences\n",
                      INNER: inner}, "w:bz2")


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


@pytest.fixture
def server(tmp_path):
    """A local HTTP server over tmp_path/www; yields (www, base URL)."""
    www = tmp_path / "www"
    www.mkdir()
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
                                            functools.partial(Quiet, directory=str(www)))
    thread = threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.05},
                              daemon=True)
    thread.start()
    yield www, f"http://127.0.0.1:{httpd.server_address[1]}"
    httpd.shutdown()
    httpd.server_close()


class Run:
    """One run of the script against the fake server, with a fake cutter."""

    def __init__(self, tmp_path, server, shell):
        self.www, self.base = server
        self.shell = shell
        self.out = tmp_path / "out"
        self.log = tmp_path / "cutter.log"
        self.tmp = tmp_path / "tmp"
        self.tmp.mkdir()
        cutter = tmp_path / "fake_cutter.py"
        cutter.write_text(FAKE_CUTTER)
        self.env = dict(os.environ,
                        TMPDIR=str(self.tmp),
                        SGX_FW_URL_UBUNTU=f"{self.base}/ubuntu/{UBUNTU_TGZ}",
                        SGX_FW_URL_ARCHIVE=f"{self.base}/archive/{INTEL_TBZ}",
                        SGX_FW_URL_WAYBACK=f"{self.base}/wayback/{INTEL_TBZ}",
                        SGX_FW_SHA256_UBUNTU=sha256(ubuntu_package()),
                        SGX_FW_SHA256_INTEL=sha256(intel_package()),
                        SGX_FW_SHA256_INIT=sha256(INIT),
                        SGX_FW_SHA256_UM=sha256(UM),
                        FWCUTTER=f"{sys.executable} {cutter} {self.log}")

    def serve(self, source: str, data: bytes):
        name = UBUNTU_TGZ if source == "ubuntu" else INTEL_TBZ
        (self.www / source).mkdir(exist_ok=True)
        (self.www / source / name).write_bytes(data)

    def __call__(self, *args, stdin: str | None = None):
        return subprocess.run([self.shell, str(SCRIPT), "-o", str(self.out), *args],
                              input=stdin if stdin is not None else "",
                              capture_output=True, text=True, env=self.env, timeout=120)

    @property
    def cutter_calls(self) -> list[str]:
        return self.log.read_text().splitlines() if self.log.exists() else []

    @property
    def fw(self) -> pathlib.Path:
        return self.out / "sgx545_ukernel.fw"


# The system sh, and dash (Debian's sh) where it is installed: the script is POSIX sh.
SHELLS = ["sh"] + (["dash"] if shutil.which("dash") else [])


@pytest.fixture(params=SHELLS)
def run(tmp_path, server, request):
    return Run(tmp_path, server, request.param)


def assert_cut(run, init_only: bool = False):
    calls = run.cutter_calls
    assert calls, "the cutter was not invoked"
    args = calls[0].split()
    assert args[0] == "cut"
    assert args[1].endswith("/libsrv_init.so.1.7.862890")
    if init_only:
        assert args[2:] == ["-o", str(run.fw)]
    else:
        assert args[2].endswith("/libsrv_um.so.1.7.862890")
        assert args[3:] == ["-o", str(run.fw)]
    assert f"{args[1]} {INIT!r}" in calls
    assert run.fw.read_bytes() == b"fake firmware\n"


# --- the happy paths ------------------------------------------------------------------------

def test_accept_license_proceeds(run):
    run.serve("ubuntu", ubuntu_package())
    r = run("--accept-license")
    assert r.returncode == 0, r.stderr
    assert LICENCE.decode().strip() in r.stdout
    assert_cut(run)
    assert sha256(b"fake firmware\n") in r.stdout


def test_interactive_yes_proceeds(run):
    run.serve("ubuntu", ubuntu_package())
    r = run(stdin="y\n")
    assert r.returncode == 0, r.stderr
    assert "[y/N]" in r.stderr
    assert_cut(run)


def test_intel_package_is_unpacked_twice(run):
    run.serve("archive", intel_package())
    r = run("--accept-license")
    assert r.returncode == 0, r.stderr
    assert LICENCE.decode().strip() in r.stdout
    assert_cut(run)


def test_init_only_leaves_libsrv_um_out(run):
    run.serve("ubuntu", ubuntu_package())
    r = run("--accept-license", "--init-only")
    assert r.returncode == 0, r.stderr
    assert_cut(run, init_only=True)


def test_temporary_files_are_removed(run):
    run.serve("ubuntu", ubuntu_package())
    assert run("--accept-license").returncode == 0
    assert list(run.tmp.iterdir()) == []
    assert sorted(p.name for p in run.out.iterdir()) == ["sgx545_ukernel.fw"]


@pytest.mark.skipif(shutil.which("wget") is None, reason="wget not installed")
def test_wget_works_too(run):
    run.serve("ubuntu", ubuntu_package())
    run.env["SGX_FW_FETCH"] = "wget"
    r = run("--accept-license")
    assert r.returncode == 0, r.stderr
    assert_cut(run)


# --- the licence ----------------------------------------------------------------------------

@pytest.mark.parametrize("answer", ["n\n", "\n", "", "maybe\n"])
def test_licence_refusal_aborts(run, answer):
    run.serve("ubuntu", ubuntu_package())
    r = run(stdin=answer)
    assert r.returncode != 0
    assert LICENCE.decode().strip() in r.stdout
    assert "not accepted" in r.stderr
    assert run.cutter_calls == [] and not run.fw.exists()
    assert list(run.tmp.iterdir()) == []


# --- sources and hashes ---------------------------------------------------------------------

def test_falls_back_past_a_missing_source(run):
    run.serve("wayback", intel_package())              # ubuntu and archive.org: 404
    r = run("--accept-license")
    assert r.returncode == 0, r.stderr
    assert "ubuntu" in r.stderr and "archive.org" in r.stderr
    assert "download failed" in r.stderr
    assert_cut(run)


def test_falls_back_past_a_bad_package(run):
    run.serve("ubuntu", ubuntu_package(b"tampered\n"))
    run.serve("archive", intel_package())
    r = run("--accept-license")
    assert r.returncode == 0, r.stderr
    assert "SHA-256 mismatch" in r.stderr
    assert_cut(run)


def test_package_hash_mismatch_fails(run):
    run.serve("ubuntu", ubuntu_package(b"tampered\n"))
    r = run("--accept-license")
    assert r.returncode != 0
    assert "SHA-256 mismatch" in r.stderr
    assert "no source" in r.stderr
    assert run.cutter_calls == [] and not run.fw.exists()


def test_library_hash_mismatch_fails(run):
    run.serve("ubuntu", ubuntu_package())
    run.env["SGX_FW_SHA256_INIT"] = "0" * 64
    r = run("--accept-license")
    assert r.returncode != 0
    assert "libsrv_init.so.1.7.862890" in r.stderr and "SHA-256 mismatch" in r.stderr
    assert run.cutter_calls == [] and not run.fw.exists()


def test_all_sources_down_is_a_clear_error(run):
    r = run("--accept-license")
    assert r.returncode != 0
    for name in ("old-releases.ubuntu.com", "archive.org", "web.archive.org"):
        assert name in r.stderr
    assert "no source" in r.stderr and "README" in r.stderr


def test_list_sources_prints_the_built_in_urls_and_hashes():
    env = {k: v for k, v in os.environ.items() if not k.startswith("SGX_FW_")}
    r = subprocess.run(["sh", str(SCRIPT), "--list-sources"], capture_output=True, text=True,
                       env=env, timeout=30)
    assert r.returncode == 0, r.stderr
    lines = r.stdout.splitlines()
    assert lines[:3] == [
        "old-releases.ubuntu.com "
        "http://old-releases.ubuntu.com/ubuntu/pool/multiverse/c/cedarview-graphics-drivers/"
        + UBUNTU_TGZ + " c23e7261631860f3bc132c294e269ea982e34e37b5e838fafea0e7070da8fd83",
        "archive.org https://archive.org/download/"
        "PowerVR-Graphics-Media-Drivers-for-Linux-Intel-GMA3600-3650-GPU/" + INTEL_TBZ
        + " 2e662b25ffb40f067ea4dabd193d1927b1090b6d4996e159653e4dd695c0317a",
        "web.archive.org http://web.archive.org/web/20190902150937id_/"
        "https://downloadmirror.intel.com/21938/eng/" + INTEL_TBZ
        + " 2e662b25ffb40f067ea4dabd193d1927b1090b6d4996e159653e4dd695c0317a",
    ]
    assert [line.split() for line in lines[3:]] == [
        ["libsrv_init.so.1.7.862890",
         "8c5a59c43f793f731199455f41ec4f065c63ecdbd36f72e4ea7501e2e327869a"],
        ["libsrv_init.so.1.7.862890",
         "4d3df850191ac363b085afd025982f5eace64977edd5483466649e9effe53b6d"],
        ["libsrv_um.so.1.7.862890",
         "2b2c09866ca372c06b9e20472caeb43edf0d776a25fa63d30979121f11988a44"],
        ["libsrv_um.so.1.7.862890",
         "e19402fce177c72e0c76f0c92247e02f83141aa753fe67b18cf896d619ad0dce"],
    ]


def test_the_library_hashes_are_fwcutters():
    import facts
    text = SCRIPT.read_text()
    for k in facts.KNOWN_INPUTS:
        assert k.sha256 in text


# --- the cache ------------------------------------------------------------------------------

def test_cache_dir_keeps_the_package_and_is_reused(run, tmp_path):
    cache = tmp_path / "cache"
    run.serve("ubuntu", ubuntu_package())
    assert run("--accept-license", "--cache-dir", str(cache)).returncode == 0
    assert (cache / UBUNTU_TGZ).read_bytes() == ubuntu_package()
    shutil.rmtree(run.www / "ubuntu")                  # every source is down now
    run.fw.unlink()
    r = run("--accept-license", "--cache-dir", str(cache))
    assert r.returncode == 0, r.stderr
    assert "cached" in r.stderr
    assert run.fw.exists()


def test_a_bad_cached_package_is_fetched_again(run, tmp_path):
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / UBUNTU_TGZ).write_bytes(b"corrupt")
    run.serve("ubuntu", ubuntu_package())
    r = run("--accept-license", "--cache-dir", str(cache))
    assert r.returncode == 0, r.stderr
    assert (cache / UBUNTU_TGZ).read_bytes() == ubuntu_package()


def test_cache_dir_inside_the_repository_is_refused(run):
    r = run("--accept-license", "--cache-dir", str(HERE / "cache-here"))
    assert r.returncode != 0
    assert "outside" in r.stderr
    assert not (HERE / "cache-here").exists()


def test_unknown_option_shows_usage(run):
    r = run("--bogus")
    assert r.returncode != 0
    assert "usage" in r.stderr.lower()


# --- the real thing (opt-in) ----------------------------------------------------------------

def documented_fw_sha256() -> str:
    m = re.search(r"`sgx545_ukernel\.fw` \(format v3\) has SHA-256\s+`([0-9a-f]{64})`",
                  README.read_text())
    assert m, "README.md does not document the cut's SHA-256"
    return m.group(1)


def test_readme_hash_is_the_cut_of_the_known_inputs():
    import fwcutter
    import fwformat as F
    init, um = fwcutter.default_input(), fwcutter.default_um_input()
    if init is None or um is None:
        pytest.skip("the Cedarview libraries are not available")
    raw = F.pack(fwcutter.cut(init.read_bytes(), um.read_bytes()))
    assert sha256(raw) == documented_fw_sha256()


@pytest.mark.skipif(os.environ.get("SGX_NET_TESTS") != "1", reason="set SGX_NET_TESTS=1")
@pytest.mark.parametrize("source", ["ubuntu", "archive", "wayback"])
def test_real_download(tmp_path, source):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("SGX_FW_") and k != "FWCUTTER"}
    env["TMPDIR"] = str(tmp_path)
    for other in {"ubuntu", "archive", "wayback"} - {source}:
        env[f"SGX_FW_URL_{other.upper()}"] = "http://127.0.0.1:9/unreachable"
    out = tmp_path / "out"
    r = subprocess.run(["sh", str(SCRIPT), "--accept-license", "-o", str(out)],
                       capture_output=True, text=True, env=env, timeout=900)
    assert r.returncode == 0, r.stderr
    assert sha256((out / "sgx545_ukernel.fw").read_bytes()) == documented_fw_sha256()
