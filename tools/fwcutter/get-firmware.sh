#!/bin/sh
# SPDX-License-Identifier: MIT
#
# get-firmware.sh: fetch the Cedarview PowerVR package, check it, and cut
# sgx545_ukernel.fw from it locally, the way Debian's firmware-b43-installer
# runs b43-fwcutter.
#
# The project ships neither the closed package nor the cut file: Intel's
# licence allows redistribution only unmodified and with the licence. This
# script downloads the official package, checks its SHA-256 and that of the
# libraries in it, shows the licence and asks you to accept it, then runs
# fwcutter.py. The download lives in a temporary directory, removed on exit,
# unless --cache-dir names a directory (outside this repository) to keep it.
#
# Sources, tried in order:
#   1. Ubuntu 12.04 multiverse, cedarview-graphics-drivers 20120717
#      (old-releases.ubuntu.com)
#   2. archive.org copy of Intel's cdv-gfx-drivers-1.0.3_bee.tar.bz2
#   3. Wayback Machine copy of Intel Download Center #21938 (same file)
#
# For testing only, the environment can override the sources
# (SGX_FW_URL_{UBUNTU,ARCHIVE,WAYBACK}), the known hashes
# (SGX_FW_SHA256_{UBUNTU,INTEL}: one package hash; SGX_FW_SHA256_{INIT,UM}:
# a space-separated list of library hashes), the downloader
# (SGX_FW_FETCH=curl|wget) and the cutter command (FWCUTTER).

set -eu

PROG=get-firmware
HERE=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(cd "$HERE/../.." && pwd -P)

UBUNTU_FILE=cedarview-graphics-drivers_20120717.orig.tar.gz
INTEL_FILE=cdv-gfx-drivers-1.0.3_bee.tar.bz2
IA_ITEM=PowerVR-Graphics-Media-Drivers-for-Linux-Intel-GMA3600-3650-GPU

UBUNTU_POOL=http://old-releases.ubuntu.com/ubuntu/pool/multiverse/c/cedarview-graphics-drivers
WAYBACK=http://web.archive.org/web/20190902150937id_
INTEL_MIRROR=https://downloadmirror.intel.com/21938/eng
URL_UBUNTU=${SGX_FW_URL_UBUNTU:-$UBUNTU_POOL/$UBUNTU_FILE}
URL_ARCHIVE=${SGX_FW_URL_ARCHIVE:-https://archive.org/download/$IA_ITEM/$INTEL_FILE}
URL_WAYBACK=${SGX_FW_URL_WAYBACK:-$WAYBACK/$INTEL_MIRROR/$INTEL_FILE}

# The packages: Ubuntu's .orig.tar.gz; Intel's tarball (archive.org and Wayback copies).
SHA_UBUNTU=${SGX_FW_SHA256_UBUNTU:-c23e7261631860f3bc132c294e269ea982e34e37b5e838fafea0e7070da8fd83}
SHA_INTEL=${SGX_FW_SHA256_INTEL:-2e662b25ffb40f067ea4dabd193d1927b1090b6d4996e159653e4dd695c0317a}
# The libraries fwcutter knows (facts.KNOWN_INPUTS): the Intel build, then the Ubuntu one.
INIT_INTEL=8c5a59c43f793f731199455f41ec4f065c63ecdbd36f72e4ea7501e2e327869a
INIT_UBUNTU=4d3df850191ac363b085afd025982f5eace64977edd5483466649e9effe53b6d
UM_INTEL=2b2c09866ca372c06b9e20472caeb43edf0d776a25fa63d30979121f11988a44
UM_UBUNTU=e19402fce177c72e0c76f0c92247e02f83141aa753fe67b18cf896d619ad0dce
SHA_INIT=${SGX_FW_SHA256_INIT:-"$INIT_INTEL $INIT_UBUNTU"}
SHA_UM=${SGX_FW_SHA256_UM:-"$UM_INTEL $UM_UBUNTU"}

INIT_SO=libsrv_init.so.1.7.862890
UM_SO=libsrv_um.so.1.7.862890
LICENCE_TXT=./usr/share/doc/powervr/license.txt
INTEL_INNER=cdv-gfx-drivers-1.0.3_bee/bin/cedarview-userspace-v1.0.3_bee.tar

usage() {
	cat <<EOF
usage: $PROG [--accept-license] [-o DIR] [--cache-dir DIR] [--init-only]
       $PROG --list-sources

Download the Cedarview PowerVR package, verify it, show Intel's licence and,
once accepted, cut DIR/sgx545_ukernel.fw with fwcutter.py.

  --accept-license  accept the licence without asking (non-interactive use)
  -o, --output-dir DIR
                    where to write sgx545_ukernel.fw (default: .)
  --cache-dir DIR   keep the downloaded package in DIR and reuse it; DIR must
                    be outside this repository (default: a temporary directory)
  --init-only       cut from libsrv_init.so alone (format v1); by default
                    libsrv_um.so is used as well (format v3)
  --list-sources    print each source's URL and SHA-256, then the libraries'
                    known SHA-256s, and exit (for fetching by hand)
  -h, --help        show this help
EOF
}

die() {
	echo "$PROG: $*" >&2
	exit 1
}

note() {
	echo "$PROG: $*" >&2
}

ACCEPT=0
OUTDIR=.
CACHE=
INIT_ONLY=0
LIST=0
while [ $# -gt 0 ]; do
	case $1 in
	--accept-license) ACCEPT=1 ;;
	-o | --output-dir)
		[ $# -ge 2 ] || { usage >&2; exit 2; }
		OUTDIR=$2
		shift ;;
	--cache-dir)
		[ $# -ge 2 ] || { usage >&2; exit 2; }
		CACHE=$2
		shift ;;
	--init-only) INIT_ONLY=1 ;;
	--list-sources) LIST=1 ;;
	-h | --help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
	shift
done

if [ "$LIST" = 1 ]; then
	echo "old-releases.ubuntu.com $URL_UBUNTU $SHA_UBUNTU"
	echo "archive.org $URL_ARCHIVE $SHA_INTEL"
	echo "web.archive.org $URL_WAYBACK $SHA_INTEL"
	for h in $SHA_INIT; do echo "$INIT_SO $h"; done
	for h in $SHA_UM; do echo "$UM_SO $h"; done
	exit 0
fi

# --- tools ------------------------------------------------------------------

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	elif command -v openssl >/dev/null 2>&1; then
		openssl dgst -sha256 "$1" | sed 's/^.*= *//'
	else
		die "need sha256sum, shasum or openssl"
	fi
}

FETCH=${SGX_FW_FETCH:-}
if [ -z "$FETCH" ]; then
	if command -v curl >/dev/null 2>&1; then
		FETCH=curl
	elif command -v wget >/dev/null 2>&1; then
		FETCH=wget
	else
		die "need curl or wget"
	fi
fi

fetch() { # url file
	case $FETCH in
	curl) curl -fsSL --retry 2 --connect-timeout 30 -o "$2" "$1" ;;
	wget) wget -q --tries=3 --timeout=30 -O "$2" "$1" ;;
	*) die "SGX_FW_FETCH must be curl or wget" ;;
	esac
}

if [ -n "${FWCUTTER:-}" ]; then
	CUTTER=$FWCUTTER
else
	PYTHON=${PYTHON:-python3}
	command -v "$PYTHON" >/dev/null 2>&1 || die "need $PYTHON (3.10 or later) to run fwcutter.py"
	CUTTER="$PYTHON $HERE/fwcutter.py"
fi

# --- directories ------------------------------------------------------------

WORK=$(mktemp -d "${TMPDIR:-/tmp}/sgx545-fw.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

outside_repo() { # dir: the package is not ours to commit
	case $1/ in
	"$REPO"/*) die "--cache-dir must be outside this repository ($REPO)" ;;
	esac
}

if [ -n "$CACHE" ]; then
	case $CACHE in                         # before creating it, unless .. makes that unsure
	*..*) ;;
	/*) outside_repo "$CACHE" ;;
	*) outside_repo "$(pwd -P)/$CACHE" ;;
	esac
	mkdir -p "$CACHE"
	CACHE=$(cd "$CACHE" && pwd -P)
	outside_repo "$CACHE"                  # again, with symlinks resolved
	DLDIR=$CACHE
else
	DLDIR=$WORK
fi

# --- download ---------------------------------------------------------------

# get NAME URL FILE SHA: leave a verified FILE in $DLDIR, or fail.
get() {
	dest=$DLDIR/$3
	if [ -n "$CACHE" ] && [ -f "$dest" ]; then
		if [ "$(sha256 "$dest")" = "$4" ]; then
			note "$1: using the cached $dest"
			return 0
		fi
		note "$1: the cached $dest does not match its SHA-256; fetching it again"
		rm -f "$dest"
	fi
	note "$1: fetching $2"
	if ! fetch "$2" "$dest.part"; then
		rm -f "$dest.part"
		note "$1: download failed ($2)"
		return 1
	fi
	got=$(sha256 "$dest.part")
	if [ "$got" != "$4" ]; then
		rm -f "$dest.part"
		note "$1: SHA-256 mismatch for $3: got $got, expected $4"
		return 1
	fi
	mv "$dest.part" "$dest"
}

# check_lib FILE HASHES: the library must be one fwcutter knows.
check_lib() {
	got=$(sha256 "$1")
	for h in $2; do
		[ "$got" = "$h" ] && return 0
	done
	die "SHA-256 mismatch for $(basename "$1"): got $got, expected one of: $2"
}

LIBS="./usr/lib/$INIT_SO"
[ "$INIT_ONLY" = 1 ] || LIBS="$LIBS ./usr/lib/$UM_SO"
X=$WORK/x
mkdir -p "$X"

if get "old-releases.ubuntu.com" "$URL_UBUNTU" "$UBUNTU_FILE" "$SHA_UBUNTU"; then
	# shellcheck disable=SC2086
	tar -xzf "$DLDIR/$UBUNTU_FILE" -C "$X" $LIBS "$LICENCE_TXT" ||
		die "cannot extract the libraries from $UBUNTU_FILE"
elif get "archive.org" "$URL_ARCHIVE" "$INTEL_FILE" "$SHA_INTEL" ||
	get "web.archive.org" "$URL_WAYBACK" "$INTEL_FILE" "$SHA_INTEL"; then
	tar -xjf "$DLDIR/$INTEL_FILE" -C "$WORK" "$INTEL_INNER" ||
		die "cannot extract the userspace tarball from $INTEL_FILE"
	# shellcheck disable=SC2086
	tar -xf "$WORK/$INTEL_INNER" -C "$X" $LIBS "$LICENCE_TXT" ||
		die "cannot extract the libraries from $INTEL_INNER"
else
	die "no source worked (old-releases.ubuntu.com, archive.org, web.archive.org);" \
		"see \"By hand\" in $HERE/README.md"
fi

check_lib "$X/usr/lib/$INIT_SO" "$SHA_INIT"
[ "$INIT_ONLY" = 1 ] || check_lib "$X/usr/lib/$UM_SO" "$SHA_UM"

# --- licence ----------------------------------------------------------------

echo "--------------------------------------------------------------------------"
echo "The libraries are covered by this licence ($LICENCE_TXT in the package):"
echo
cat "$X/$LICENCE_TXT"
echo
echo "--------------------------------------------------------------------------"
echo "The firmware cut from them may be used, but not redistributed: the licence"
echo "allows redistribution only of the unmodified files, with this licence."
if [ "$ACCEPT" = 1 ]; then
	note "licence accepted with --accept-license"
else
	printf 'Do you accept this licence? [y/N] ' >&2
	answer=
	read -r answer || true
	case $answer in
	y | Y | yes | YES | Yes) ;;
	*) die "licence not accepted; nothing was cut" ;;
	esac
fi

# --- cut --------------------------------------------------------------------

mkdir -p "$OUTDIR"
FW=$OUTDIR/sgx545_ukernel.fw
if [ "$INIT_ONLY" = 1 ]; then
	$CUTTER cut "$X/usr/lib/$INIT_SO" -o "$FW" || die "fwcutter failed"
else
	$CUTTER cut "$X/usr/lib/$INIT_SO" "$X/usr/lib/$UM_SO" -o "$FW" || die "fwcutter failed"
fi
echo "$(sha256 "$FW")  $FW"
