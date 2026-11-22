#!/usr/bin/env bash
# Build ghostty.exe, the Windows host, against a libghostty build.
#
#   windows/build.sh <libghostty prefix> <x86_64|aarch64> <out dir> [debug|release]
#
# <libghostty prefix> is the --prefix of a libghostty build for the same
# architecture, e.g.
#   zig build -Dtarget=x86_64-windows-gnu -Doptimize=ReleaseFast --prefix <prefix>
# The host links its static library, so ghostty.exe needs no DLL of ours
# beside it. Works on Linux (cross) and on Windows (Git Bash) with zig on PATH.
# SUBSYSTEM=console keeps a console window for the logs (default: windows).
set -euo pipefail
prefix=$1 arch=$2 out=$3 mode=${4:-release}
here=$(cd "$(dirname "$0")" && pwd)

case $mode in
release) opt=(-O2 -DNDEBUG) ;;
debug) opt=(-O0 -g) ;;
*) echo "build.sh: mode is debug or release, not $mode" >&2; exit 2 ;;
esac
case ${SUBSYSTEM:-windows} in
windows) sub=(-Wl,--subsystem,windows) ;;
console) sub=() ;;
*) echo "build.sh: SUBSYSTEM is windows or console" >&2; exit 2 ;;
esac

mkdir -p "$out"
zig cc -target "$arch-windows-gnu" "${opt[@]}" -std=c11 -Wall -Wextra \
	-Wno-unused-parameter -Wno-missing-field-initializers \
	-DGHOSTTY_STATIC -I"$prefix/include" \
	-c "$here/ghostty.c" -o "$out/ghostty.obj"
# The static library has C++ in it (glslang, spirv-cross, dear imgui), so
# the link is a C++ link: zig c++ brings libc++.
zig c++ -target "$arch-windows-gnu" "${opt[@]}" \
	"$out/ghostty.obj" \
	"$prefix/lib/ghostty-internal-static.lib" \
	-lopengl32 -lgdi32 -luser32 -lshell32 -lole32 -lws2_32 -lmswsock -lntdll -ldbghelp \
	"${sub[@]}" \
	-o "$out/ghostty.exe"
rm -f "$out/ghostty.obj"
echo "built $out/ghostty.exe"
