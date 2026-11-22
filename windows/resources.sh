#!/usr/bin/env bash
# Copy Ghostty's resources from a libghostty build into a Windows package, beside ghostty.exe:
#
#   windows/resources.sh <libghostty prefix> <out dir>
#
# <libghostty prefix> is the --prefix of a libghostty build for Windows, built with
# -Demit-themes=true (themes are off by default for a Windows target), e.g.
#   zig build -Dtarget=x86_64-windows-gnu -Doptimize=ReleaseFast -Demit-themes=true --prefix <prefix>
# <out dir> is the folder ghostty.exe is in. This writes
#   <out>/share/terminfo/ghostty.terminfo      the sentinel the core looks for: found, it
#                                              takes <out>/share/ghostty as its resources dir
#   <out>/share/ghostty/themes/                for `theme = <name>`
#   <out>/share/ghostty/shell-integration/     bash, zsh, fish, elvish, nushell
# and nothing else of share/ (completions, vim files, man pages are for Unix installs).
#
# A file whose name Windows can't hold (a colon in some theme names, a reserved name like
# CON, a trailing dot or space) is left out and named on stderr, so the zip always unpacks.
set -euo pipefail
prefix=$1 out=$2
share=$prefix/share

[ -f "$share/terminfo/ghostty.terminfo" ] || {
	echo "resources.sh: $share/terminfo/ghostty.terminfo is missing: not a libghostty build for Windows" >&2
	exit 1
}
[ -d "$share/ghostty/themes" ] || {
	echo "resources.sh: $share/ghostty/themes is missing: build libghostty with -Demit-themes=true" >&2
	exit 1
}

# True when one path component is a valid Windows file name.
windows_name() {
	local n=$1 stem
	case $n in
	*[\<\>:\"\\\|\?\*]* | *[[:cntrl:]]* | *. | *' ') return 1 ;;
	esac
	stem=${n%%.*}
	case ${stem^^} in
	CON | PRN | AUX | NUL | COM[0-9] | LPT[0-9]) return 1 ;;
	esac
	return 0
}

# copy <relative path under share/>: a file or a whole directory.
copy() {
	local rel=$1 f name ok
	(cd "$share" && find "$rel" -type f) | LC_ALL=C sort | while IFS= read -r f; do
		ok=1
		IFS=/ read -r -a parts <<< "$f"
		for name in "${parts[@]}"; do windows_name "$name" || ok=0; done
		if [ $ok = 0 ]; then
			echo "resources.sh: left out share/$f: not a valid Windows file name" >&2
			continue
		fi
		mkdir -p "$out/share/$(dirname "$f")"
		cp "$share/$f" "$out/share/$f"
	done
}

rm -rf "$out/share"
copy terminfo/ghostty.terminfo
copy ghostty/themes
copy ghostty/shell-integration
shells=$(cd "$out/share/ghostty/shell-integration" && printf '%s ' *)
echo "resources: $(find "$out/share/ghostty/themes" -type f | wc -l) themes, shell integration for ${shells% } in $out/share"
