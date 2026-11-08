#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the kmod package with the OpenWrt SDK and the test tool with its
# cross compiler; stage both in deploy/.
#
# Usage: tools/build.sh [path-to-sdk]
# Default SDK: build/openwrt-sdk-* in this repository (gitignored). The
# package build writes into the SDK, so use a copy of your own.
set -e

here=$(cd "$(dirname "$0")/.." && pwd)

sdk=${1:-$(ls -d "$here"/build/openwrt-sdk-*/ 2>/dev/null | head -1)}
sdk=${sdk%/}
if [ -z "$sdk" ] || [ ! -d "$sdk" ]; then
	echo "no SDK found; pass the path as an argument" >&2
	exit 1
fi

pkgdir="$sdk/package/kmod-lantiq-tdm-pcm"
mkdir -p "$pkgdir"
cp -a "$here/Makefile" "$pkgdir/"
rm -rf "$pkgdir/src"
cp -a "$here/src" "$pkgdir/"

(
	cd "$sdk"
	# a fresh SDK has no .config and would start menuconfig
	[ -f .config ] || make defconfig >/dev/null
	make package/kmod-lantiq-tdm-pcm/clean >/dev/null 2>&1 || true
	make package/kmod-lantiq-tdm-pcm/compile V=s
)

ko=$(ls "$sdk"/build_dir/target-*/linux-lantiq_xrx200/lantiq-tdm-pcm-*/.pkgdir/kmod-lantiq-tdm-pcm/lib/modules/*/lantiq_tdm_pcm.ko 2>/dev/null | head -1)
apk=$(ls "$sdk"/bin/targets/lantiq/xrx200/packages/kmod-lantiq-tdm-pcm-*.apk 2>/dev/null | head -1)

mkdir -p "$here/deploy"
cc=$(ls "$sdk"/staging_dir/toolchain-*/bin/*-openwrt-linux-musl-gcc | head -1)
STAGING_DIR="$sdk/staging_dir" "$cc" -O2 -Wall -Wextra \
	-I"$here/src/include/uapi" \
	-o "$here/deploy/tdm-pcm-test" "$here/tools/tdm-pcm-test.c"
[ -n "$ko" ] && cp -a "$ko" "$here/deploy/"
[ -n "$apk" ] && cp -a "$apk" "$here/deploy/"

echo "artifacts in $here/deploy:"
ls -l "$here/deploy"
