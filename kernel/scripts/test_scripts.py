"""The test-cycle scripts take their hosts, paths and ports from the environment.

push-gma500.sh is checked in dry-run mode (SGX_DRY_RUN=1 prints the commands instead of
running them), so no build host or laptop is needed. test-session.sh only runs on the
laptop; here we check that it refuses to start without a netconsole target.
netconsole-listen.py is run for real on a free local port.
"""
from __future__ import annotations

import os
import pathlib
import socket
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
PUSH = HERE / "push-gma500.sh"
SESSION = HERE / "test-session.sh"
LISTEN = HERE / "netconsole-listen.py"


def clean_env(**extra: str) -> dict[str, str]:
    """The caller's environment without any SGX_* setting, plus extra."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("SGX_")}
    env["SGX_LOCAL_ENV"] = os.devnull       # never pick up the developer's local.env
    env.update(extra)
    return env


def push(*args: str, **extra: str) -> str:
    out = subprocess.run(["bash", str(PUSH), *args], env=clean_env(SGX_DRY_RUN="1", **extra),
                         capture_output=True, text=True, timeout=30)
    assert out.returncode == 0, out.stderr
    return out.stdout


def test_push_defaults_to_ssh_aliases():
    out = push()
    assert "ssh sgx-build " in out
    assert "sgx-target:/tmp/" in out
    assert "cd src/linux-sgx" in out
    assert "vermagic=7.3.0-sgx " in out
    assert "/usr/lib/modules/7.3.0-sgx/" in out
    assert "-j\\$(nproc)" in out or "-j$(nproc)" in out


def test_push_takes_hosts_and_paths_from_the_environment():
    out = push(SGX_BUILD_HOST="builder", SGX_TARGET_HOST="me@laptop",
               SGX_TARGET_SSH_OPTS="-o HostKeyAlias=box", SGX_KERNEL_TREE="work/linux",
               SGX_KERNEL_RELEASE="7.4.0-sgx", SGX_BUILD_JOBS="8",
               SGX_BUILD_PATH="~/.local/bin", SGX_NETCONSOLE_HOST="10.1.2.3",
               SGX_NETCONSOLE_PORT="7777")
    assert "ssh builder " in out
    assert "builder:work/linux/drivers/gpu/drm/gma500/gma500_gfx.ko" in out
    assert "-o HostKeyAlias=box me@laptop" in out
    assert "me@laptop:/tmp/" in out
    assert "cd work/linux" in out
    assert "-j8 " in out
    assert "PATH=~/.local/bin:" in out
    assert "vermagic=7.4.0-sgx " in out
    assert "depmod 7.4.0-sgx" in out
    assert "test-session.sh 10.1.2.3 7777" in out
    assert "sgx-build" not in out and "sgx-target" not in out


def test_push_netconsole_defaults_to_the_build_host_address():
    out = push(SGX_BUILD_HOST="10.9.8.7")
    assert "test-session.sh 10.9.8.7 6666" in out


def test_push_reads_local_env(tmp_path):
    env_file = tmp_path / "local.env"
    env_file.write_text("SGX_BUILD_HOST=${SGX_BUILD_HOST:-from-file}\n"
                        "SGX_TARGET_HOST=${SGX_TARGET_HOST:-laptop-from-file}\n")
    out = push(SGX_LOCAL_ENV=str(env_file))
    assert "ssh from-file " in out
    assert "laptop-from-file:/tmp/" in out
    # the environment still wins over the file
    out = push(SGX_LOCAL_ENV=str(env_file), SGX_BUILD_HOST="override")
    assert "ssh override " in out


def test_push_reads_the_example_settings():
    out = push(SGX_LOCAL_ENV=str(HERE / "local.env.example"))
    assert "-o HostKeyAlias=sgx-target user@sgx-target" in out
    assert "export PATH=~/.local/bin:$PATH;" in out     # ~ left for the build host
    assert "-j8 " in out
    assert "test-session.sh 192.0.2.10 6666" in out


def test_push_no_reboot_stops_after_install():
    out = push("--no-reboot")
    assert "mkinitcpio" in out
    assert "boot-test-kernel.sh" not in out.split("mkinitcpio", 1)[1]


def test_push_passes_the_render_gate():
    assert "boot-test-kernel.sh 0" in push(SGX_RENDER="0")
    assert "boot-test-kernel.sh 1" in push()


def test_session_needs_a_netconsole_target():
    out = subprocess.run(["bash", str(SESSION)], env=clean_env(), capture_output=True,
                         text=True, timeout=30)
    assert out.returncode != 0
    assert "usage" in out.stderr


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_listener_takes_port_and_log_from_the_environment(tmp_path):
    port, log = free_udp_port(), tmp_path / "nc.log"
    proc = subprocess.Popen([sys.executable, str(LISTEN)],
                            env=clean_env(SGX_NETCONSOLE_PORT=str(port),
                                          SGX_NETCONSOLE_LOG=str(log)))
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            for _ in range(50):
                s.sendto(b"hello sgx\n", ("127.0.0.1", port))
                time.sleep(0.1)
                if log.exists() and "hello sgx" in log.read_text():
                    break
        assert "hello sgx" in log.read_text()
    finally:
        proc.kill()
        proc.wait()
