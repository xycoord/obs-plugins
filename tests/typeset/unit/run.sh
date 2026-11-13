#!/usr/bin/env bash
# Builds and runs Typeset Text's unit tests (plain logic, no OBS needed).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../../plugins/obs-typeset/src
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
c++ -std=c++20 -Wall -Wextra -I"$src" "$here/test_rich_text.cpp" "$src/rich-text.cpp" -o "$out/test_rich_text"
# The font name reading needs Pango (and libobs's header, for OBS_FONT_*).
obs_inc=$(ls -d "${XDG_CACHE_HOME:-$HOME/.cache}"/obs-plugins/obs-*/src/libobs 2>/dev/null | head -1)
if [ -n "$obs_inc" ] && pkg-config --exists pangocairo; then
	c++ -std=c++20 -Wall -Wextra -I"$src" -I"$obs_inc" -I"$(dirname "$obs_inc")/../config" "$here/test_fonts.cpp" \
		"$src/text-engine.cpp" "$src/rich-text.cpp" $(pkg-config --cflags --libs pangocairo) -o "$out/test_fonts" \
		-Wl,--unresolved-symbols=ignore-all
	"$out/test_fonts"
else
	echo "skipped the font tests: no libobs headers (run install.sh first) or no Pango"
fi
"$out/test_rich_text"
# The font name reading needs Pango (and libobs's header, for OBS_FONT_*).
obs_inc=$(ls -d "${XDG_CACHE_HOME:-$HOME/.cache}"/obs-plugins/obs-*/src/libobs 2>/dev/null | head -1)
if [ -n "$obs_inc" ] && pkg-config --exists pangocairo; then
	c++ -std=c++20 -Wall -Wextra -I"$src" -I"$obs_inc" -I"$(dirname "$obs_inc")/../config" "$here/test_fonts.cpp" \
		"$src/text-engine.cpp" "$src/rich-text.cpp" $(pkg-config --cflags --libs pangocairo) -o "$out/test_fonts" \
		-Wl,--unresolved-symbols=ignore-all
	"$out/test_fonts"
else
	echo "skipped the font tests: no libobs headers (run install.sh first) or no Pango"
fi
