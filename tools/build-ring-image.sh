#!/bin/bash
# Pack a whyred ring image with the iOS-root flags that every diagnostic
# image needs. The ad-hoc 2026-11-23 scripts omitted three of them and a
# device boot died at launchd with CS_KILLED (wrong platform).
#
#   tools/build-ring-image.sh KERNEL OUT [--qup] [--power] [--native]
#
#   --qup     append sweeti2c=1 (BLSP1 QUP1 owns the bus; 0227 writes)
#   --power   append sweetouchpower=1 (late LDO11 toggle, opt-in)
#   --native  append sweetfb_native=1 (1080x2160 canvas, red fallback clock)
#
# The default canvas is the iPhone X 1125x2436 that SweetFramebuffer crops to
# 1080x2160, so UIKit does not draw the red unsupported-screen clock.
#
# Two passes: the first builds the payload and compresses the kernel, the
# second embeds the compressed stream. zopfli is used when available because
# whyred's ABL rejects a single download over ~6 MiB.
set -euo pipefail
cd "$(dirname "$0")/.."

kernel=${1:?usage: build-ring-image.sh KERNEL OUT [--qup] [--power] [--native]}
out=${2:?usage: build-ring-image.sh KERNEL OUT [--qup] [--power] [--native]}
shift 2

qup=0
power=0
native=0
for arg in "$@"; do
	case $arg in
	--qup) qup=1 ;;
	--power) power=1 ;;
	--native) native=1 ;;
	*) echo "unknown option: $arg" >&2; exit 2 ;;
	esac
done

# emmcrw=1 makes the HFSX root writable, cs_enforcement_disable=1 disables
# code-signing enforcement, and unentitled_ios_sim_launch=1 lets the macOS-
# config kernel exec iOS-platform binaries. Without all three, launchd is
# killed with OS_REASON_EXEC / EXIT_REASON_WRONG_PLATFORM (CS_KILLED).
common="sweetlog=1 sweetouchi2c=1 sweetouchscl=200 sweetouchfast=1 sweetouchwait=0 launchd_unsecure_cache=1 emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1"
[ "$power" = 1 ] && common="$common sweetouchpower=1"
[ "$qup" = 1 ] && common="$common sweeti2c=1"
[ "$native" = 1 ] && common="$common sweetfb_native=1"

zopfli=${ZOPFLI:-}
if [ -z "$zopfli" ]; then
	zopfli=$(command -v zopfli || true)
fi

echo "kernel: $kernel"
echo "out:    $out"
echo "cmd:    $common"

echo "first pass"
rm -rf shim/out
ROOTDEV=emmcroot0 EXTRA_CMDLINE="$common" KPRINTF_SPAM= \
	KPRINTF_MUTE=1 PANEL_NOCONSOLE=1 bash shim/build.sh "$kernel"

gz=$(mktemp "${TMPDIR:-/tmp}/whyred-Image.XXXXXX.gz")
trap 'rm -f "$gz"' EXIT
if [ -n "$zopfli" ] && [ -x "$zopfli" ]; then
	echo "compress: zopfli"
	"$zopfli" --i15 -c shim/out/Image > "$gz"
else
	echo "compress: gzip -9 (zopfli not found)"
	gzip -9 -c shim/out/Image > "$gz"
fi
test -s "$gz"

echo "second pass"
ROOTDEV=emmcroot0 EXTRA_CMDLINE="$common" KPRINTF_SPAM= \
	KPRINTF_MUTE=1 PANEL_NOCONSOLE=1 \
	IMAGE_GZ="$gz" bash shim/build.sh "$kernel"

mkdir -p "$(dirname "$out")"
cp shim/out/shim-boot.img "$out"
python3 tools/payload_cmdline.py --image "$out" | tail -1
sha256sum "$out"
stat -c '%n %s bytes' "$out"
