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

# Build info: what `ghostty.exe /buildinfo` prints, also kept as $out/BUILDINFO.txt. A release
# build passes its own lines first in $BUILDINFO_FILE (the release kit does: release, version,
# commits); this adds the source commit, the build and the core components, each with the
# version its build.zig.zon pins (a commit, a release, or the content hash when the URL has
# no version).
src=$(cd "$here/.." && pwd)
{
	[ -n "${BUILDINFO_FILE:-}" ] && cat "$BUILDINFO_FILE"
	rev=$(git -C "$src" rev-parse HEAD 2> /dev/null || echo unknown)
	git -C "$src" diff --quiet HEAD 2> /dev/null || rev="$rev (modified)"
	echo "source: $rev $(git -C "$src" rev-parse --abbrev-ref HEAD 2> /dev/null)"
	echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ), $arch-windows-gnu, host $mode, zig $(zig version)"
	# A dogfood build may draw into the windows by default (releases read frames back).
	if [ "${HOST_PRESENT_DEFAULT:-}" = swap ]; then
		echo "present: swap by default (the renderer draws into the window; GHOSTTY_PRESENT=readback for frames through the CPU)"
	fi
	# The core libraries libghostty links on Windows (not the Linux/macOS-only ones, fonts or
	# themes). `|| true`: some build.zig.zon files have no URL, and under pipefail a failing
	# grep would end the loop there.
	echo "components:"
	for z in "$src/build.zig.zon" "$src"/pkg/*/build.zig.zon; do
		grep -oE '\.url = "[^"]+"' "$z" | sed -E 's/^\.url = "//; s/"$//' || true
	done | sed -E 's|.*/||; s/\.(tar\.gz|tar\.xz|tgz|zip)$//' | sort -u |
		sed -E 's/^DearBindings_v[^_]*_ImGui_v([^-]+)-docking$/imgui-\1/' |
		grep -E '^(libxev|z2d|freetype|harfbuzz|zlib|libpng|oniguruma|glslang|spirv_cross|wuffs|highway|imgui)-' |
		sed -E 's/^(.*)-([^-]+)$/\1 \2/; s/ (1220[0-9a-f]{8})[0-9a-f]*$/ content \1/; s/ ([0-9a-f]{12})[0-9a-f]{28}$/ commit \1/' |
		awk '{ printf "  %-12s %s %s\n", $1, $2, $3 }'
} > "$out/BUILDINFO.txt"
# The same as a C string for the host.
{
	echo '#define GHOSTTY_BUILDINFO \'
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/  "/' -e 's/$/\\n" \\/' "$out/BUILDINFO.txt"
	echo '  ""'
	# The build in the window title: the release ("release: Ghostty for Windows 20261122.6" →
	# 20261122.6), else the source's short commit, "+mod" when the tree had changes.
	tag=$(sed -n 's/^release: Ghostty for Windows //p' "$out/BUILDINFO.txt" | head -1)
	if [ -z "$tag" ]; then
		tag=$(sed -n 's/^source: \([0-9a-f]\{9\}\)[0-9a-f]*\( (modified)\)\{0,1\}.*/\1\2/p' "$out/BUILDINFO.txt" | head -1)
		tag=${tag/ (modified)/+mod}
	fi
	echo "#define GHOSTTY_BUILD_TAG \"${tag:-dev}\""
	# About and What's new: the release kit passes this build's notes (WHATSNEW_FILE, UTF-8 text;
	# empty for a test build) and pys's family line (FAMILY_LINE); empty when not given.
	echo '#define GHOSTTY_WHATSNEW \'
	if [ -n "${WHATSNEW_FILE:-}" ] && [ -s "$WHATSNEW_FILE" ]; then
		sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/  "/' -e 's/$/\\n" \\/' "$WHATSNEW_FILE"
	fi
	echo '  ""'
	printf '#define GHOSTTY_FAMILY "%s"\n' "$(printf '%s' "${FAMILY_LINE:-}" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g')"
} > "$out/buildinfo.h"

# The host's modules (windows/*.c, sharing windows/host.h), one object each.
objs=()
for c in "$here"/*.c; do
	o=$out/$(basename "$c" .c).obj
	zig cc -target "$arch-windows-gnu" "${opt[@]}" -std=c11 -Wall -Wextra \
		-Wno-unused-parameter -Wno-missing-field-initializers \
		-DGHOSTTY_STATIC -I"$prefix/include" -DGHOSTTY_HAVE_BUILDINFO_H -I"$out" \
		${HOST_PRESENT_DEFAULT:+-DGHOSTTY_PRESENT_DEFAULT_$(echo "$HOST_PRESENT_DEFAULT" | tr a-z A-Z)} \
		-c "$c" -o "$o"
	objs+=("$o")
done
# The static library has C++ in it (glslang, spirv-cross, dear imgui), so
# the link is a C++ link: zig c++ brings libc++.
zig c++ -target "$arch-windows-gnu" "${opt[@]}" \
	"${objs[@]}" \
	"$prefix/lib/ghostty-internal-static.lib" \
	-lopengl32 -lgdi32 -luser32 -lshell32 -lole32 -lws2_32 -lmswsock -lntdll -ldbghelp -ladvapi32 -ldwmapi -lmsimg32 -limm32 -lshcore \
	"${sub[@]}" \
	-o "$out/ghostty.exe"
rm -f "${objs[@]}"
echo "built $out/ghostty.exe (build info: $out/BUILDINFO.txt)"
