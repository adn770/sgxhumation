# Arch Linux 32 build containers

The Cedarview laptop runs Arch Linux 32 (`pentium4`). Userspace that must link against its
libraries (IGT now, Mesa later) is built on an x86-64 build host in Docker images of the same
distribution; the x86-64 kernel runs the i686 userland natively, and the images run everything
under `setarch i686`, so build systems see an i686 host. No root on the build host is needed
beyond the `docker` group.

| file | image | what |
|------|-------|------|
| `build-rootfs.sh` | `sgxhumation/arch32:base` | `base` by `pacman --root` in `archlinux` |
| `Dockerfile.igt` | `sgxhumation/arch32:igt` | IGT's build dependencies |

```sh
# on the build host; the keyring package comes from the laptop's pacman cache
tools/arch32/build-rootfs.sh archlinux32-keyring-20251214-1.0-any.pkg.tar.zst
docker build -t sgxhumation/arch32:igt -f tools/arch32/Dockerfile.igt tools/arch32
```

Trust follows the laptop's: the Arch Linux 32 keyring, plus a local signature on the packager
key that only two master keys certify (one short of full trust), which the laptop also holds.
The install scriptlets that need `/proc` fail while the root is assembled (systemd-tmpfiles,
the journal catalog); the derived images run `systemd-tmpfiles --create` themselves.
