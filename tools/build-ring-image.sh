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
#   --srgb    force an sRGB /product identity (pre-P3 lock-screen recovery)
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

# The complete D22AP /product node is a private local build input.  Without
# it mkadt.py deliberately emits no product node, which makes this P3
# diagnostic indistinguishable from the already-failed image.
test -s research/ipsw-x/d22ap.adt || {
	echo "missing research/ipsw-x/d22ap.adt; refusing a non-P3 ring image" >&2
	exit 1
}

qup=0
power=0
native=0
srgb=0
for arg in "$@"; do
	case $arg in
	--qup) qup=1 ;;
	--power) power=1 ;;
	--native) native=1 ;;
	--srgb) srgb=1 ;;
	*) echo "unknown option: $arg" >&2; exit 2 ;;
	esac
done
if [ "$srgb" = 1 ]; then
	export SWEET_ADT_GAMUT=sRGB
fi

# emmcrw=1 makes the HFSX root writable, cs_enforcement_disable=1 disables
# code-signing enforcement, and unentitled_ios_sim_launch=1 lets the macOS-
# config kernel exec iOS-platform binaries. Without all three, launchd is
# killed with OS_REASON_EXEC / EXIT_REASON_WRONG_PLATFORM (CS_KILLED).
common="sweetlog=1 sweetouchi2c=1 sweetouchscl=200 sweetouchfast=1 sweetouchwait=0 launchd_unsecure_cache=1 emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1"
[ "$power" = 1 ] && common="$common sweetouchpower=1"
[ "$qup" = 1 ] && common="$common sweeti2c=1"
export SWEETFB_NATIVE="$native"

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

if [ "$srgb" = 1 ]; then
	# The lock-screen recovery image reverts the 0229-0231 mode experiment,
	# so the pre-0229 driver must not print any P3 mode line.
	if grep -aq 'P3 product /' shim/out/Image; then
		echo "kernel carries the P3 mode experiment; --srgb needs the pre-0229 driver" >&2
		exit 1
	fi
else
	grep -aq 'P3 product / P3 RGB SDR' shim/out/Image || {
		echo "kernel lacks the final 0231 framebuffer mode" >&2
		exit 1
	}
fi

# Verify the exact ADT that the second pass packed, not just mkadt.py's input.
NATIVE="$native" SRGB="$srgb" python3 - <<'PY'
import os
import struct
import sys
sys.path.insert(0, 'tools')
import adt

root, _ = adt.parse(open('shim/out/whyred.adt', 'rb').read())
nodes = {dict((k, v) for k, _f, v in n.props).get('name'): n
         for n in root.children}
product = nodes.get(b'product\0')
assert product is not None, 'packed ADT has no /product'
props = {k: v for k, _f, v in product.props}
assert len(product.props) >= 60, len(product.props)
assert props.get('artwork-display-gamut') == (b'sRGB\0' if os.environ['SRGB'] == '1' else b'P3\0'), props.get('artwork-display-gamut')

arm_io = nodes[b'arm-io\0']
disp = next(n for n in arm_io.children
            if dict((k, v) for k, _f, v in n.props).get('name') == b'disp0\0')
dprops = {k: v for k, _f, v in disp.props}
expected = (1080, 2160) if os.environ['NATIVE'] == '1' else (1125, 2436)
actual = (struct.unpack('<I', dprops['canvas-width'])[0],
          struct.unpack('<I', dprops['canvas-height'])[0])
assert actual == expected, (actual, expected)
print('verified packed ADT: complete P3 product, %dx%d canvas' % actual)
PY

mkdir -p "$(dirname "$out")"
cp shim/out/shim-boot.img "$out"
verify=(--require rd=emmcroot0 --require emmcrw=1 \
	--require cs_enforcement_disable=1 \
	--require unentitled_ios_sim_launch=1 \
	--require launchd_unsecure_cache=1 \
	--require sweetlog=1 --require sweetfb_noconsole=1)
if [ "$native" = 1 ]; then
	verify+=(--require sweetfb_native=1)
else
	verify+=(--reject sweetfb_native=1)
fi
python3 tools/payload_cmdline.py --image "$out" "${verify[@]}" | tail -1
sha256sum "$out"
stat -c '%n %s bytes' "$out"
test "$(stat -c%s "$out")" -le 6291456 || {
	echo "image exceeds whyred ABL's 6 MiB download limit" >&2
	exit 1
}
