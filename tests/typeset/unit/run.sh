#!/usr/bin/env bash
# Builds and runs Typeset Text's unit tests (plain logic, no OBS needed).
# The font tests need Pango and libobs's headers; without them they're
# skipped, unless STRICT=1 (as in CI), when that's a failure.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../../plugins/obs-typeset/src
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

c++ -std=c++20 -Wall -Wextra -I"$src" "$here/test_rich_text.cpp" "$src/rich-text.cpp" -o "$out/test_rich_text"

c++ -std=c++20 -Wall -Wextra -I"$src" "$here/test_edit_model.cpp" "$src/edit-model.cpp" "$src/rich-text.cpp" \
	-o "$out/test_edit_model"
"$out/test_edit_model"
"$out/test_rich_text"

# The font name reading and drawing need Pango, and libobs's header for OBS_FONT_*
# (fetched by install.sh); it calls nothing in libobs.
obs_inc=$(ls -d "${XDG_CACHE_HOME:-$HOME/.cache}"/obs-plugins/obs-*/src/libobs 2>/dev/null | head -1 || true)
if [ -z "$obs_inc" ] || ! pkg-config --exists pangocairo; then
	echo "SKIPPED the font tests: no libobs headers (run install.sh first) or no Pango"
	[ "${STRICT:-0}" = 1 ] && exit 1
	exit 0
fi
c++ -std=c++20 -Wall -Wextra -I"$src" -I"$obs_inc" -I"$(dirname "$obs_inc")/../config" "$here/test_fonts.cpp" \
	"$src/text-engine.cpp" "$src/rich-text.cpp" $(pkg-config --cflags --libs pangocairo) -o "$out/test_fonts"
"$out/test_fonts"
c++ -std=c++20 -Wall -Wextra -I"$src" -I"$obs_inc" -I"$(dirname "$obs_inc")/../config" "$here/test_raster.cpp" \
	"$src/text-engine.cpp" "$src/rich-text.cpp" $(pkg-config --cflags --libs pangocairo) -o "$out/test_raster"
"$out/test_raster"
