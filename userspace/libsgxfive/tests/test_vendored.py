"""The copy of libsgxfive in userspace/igt/lib/sgxfive/ is current.

The IGT tests build our ioctl layer, transfer engine and assembled programs from that copy
(userspace/igt/sync-libsgxfive.sh writes it). A change here without a re-sync would test
stale code on the hardware.
"""
import pathlib

HERE = pathlib.Path(__file__).resolve().parent.parent
COPY = HERE.parent / "igt" / "lib" / "sgxfive"
FILES = ["include/sgxfive.h", "include/sgxfive_xfer.h", "src/dev.c", "src/xfer.c",
         "src/xfer_job.c", "src/prog_asm.c", "src/prog_asm_gen.h"]


def test_igt_copy_is_current():
    stale = [f for f in FILES
             if (COPY / pathlib.Path(f).name).read_bytes() != (HERE / f).read_bytes()]
    assert not stale, f"run userspace/igt/sync-libsgxfive.sh: {stale}"
