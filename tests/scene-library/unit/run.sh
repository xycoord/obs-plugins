#!/usr/bin/env bash
# Builds and runs the Scene Library's unit tests: its changes to saved scenes,
# with libobs's data functions and no OBS running. Needs libobs's headers
# (fetched by install.sh) and the installed libobs; without them they're
# skipped, unless STRICT=1 (as in CI), when that's a failure.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../../plugins/obs-scene-library/src
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

obs_inc=$(ls -d "${XDG_CACHE_HOME:-$HOME/.cache}"/obs-plugins/obs-*/src/libobs 2>/dev/null | head -1 || true)
libobs=$(ldconfig -p 2>/dev/null | sed -n 's/^[[:space:]]*libobs\.so[.0-9]* .*=> \(.*\)$/\1/p' | head -1 || true)
if [ -z "$obs_inc" ] || [ -z "$libobs" ]; then
	echo "SKIPPED the Scene Library's unit tests: no libobs headers (run install.sh first) or no libobs"
	[ "${STRICT:-0}" = 1 ] && exit 1
	exit 0
fi
c++ -std=c++20 -Wall -Wextra -I"$src" -I"$obs_inc" -I"$(dirname "$obs_inc")/../config" \
	"$here/test_snapshot.cpp" "$src/snapshot.cpp" "$libobs" -o "$out/test_snapshot"
"$out/test_snapshot"
