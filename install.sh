#!/usr/bin/env bash
# Build every plugin in plugins/ against the OBS installed on this machine and
# install them into OBS's per-user plugins folder.
#
#   ./install.sh                  build and install all plugins
#   ./install.sh obs-shaderfilter build and install just these
#   ./install.sh --uninstall      remove the plugins this script installs
#
# Plugins are built against the libobs headers of the exact OBS version
# installed (fetched from obsproject's git once and cached) and linked to the
# installed libobs, so they always match the OBS that loads them. Needs git,
# cmake and a C compiler; no OBS development packages.
#
# Run it again after OBS itself is updated: plugins built for one version of
# OBS may not load in another.
#
# Environment: OBS_VERSION overrides the detected version; OBS_PLUGINS_DIR the
# folder plugins are installed into.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/obs-plugins
FLATPAK_ID=com.obsproject.Studio

say() { printf '%s\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

uninstall=false
only=()
for arg in "$@"; do
	case "$arg" in
	--uninstall) uninstall=true ;;
	-h | --help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option $arg (see --help)" ;;
	*) only+=("$arg") ;;
	esac
done

# --- Which plugins ---------------------------------------------------------
if [ -f "$ROOT/.gitmodules" ] && command -v git >/dev/null; then
	git -C "$ROOT" submodule update --init --recursive >/dev/null ||
		say "warning: couldn't update the submodules; building what's checked out"
fi
plugins=()
for dir in "$ROOT"/plugins/*/; do
	name=$(basename "$dir")
	[ -f "$dir/CMakeLists.txt" ] || continue
	if [ ${#only[@]} -gt 0 ] && [[ ! " ${only[*]} " == *" $name "* ]]; then
		continue
	fi
	plugins+=("$name")
done
for name in "${only[@]}"; do
	[ -f "$ROOT/plugins/$name/CMakeLists.txt" ] || die "no plugin called $name in plugins/"
done
[ ${#plugins[@]} -gt 0 ] || die "no plugins found in plugins/"

# --- Which OBS -------------------------------------------------------------
kind=""
version=""
libobs=""
if command -v obs >/dev/null; then
	kind=native
	version=$(obs --version 2>/dev/null | sed -n 's/.*OBS Studio - \([0-9][0-9.]*\).*/\1/p' | head -1 || true)
	# The libobs the obs program itself loads (its file name differs between
	# Ubuntu's package and OBS's own builds).
	libobs=$(ldd "$(command -v obs)" 2>/dev/null | sed -n 's/^[[:space:]]*libobs\.so[.0-9]* => \(\/[^ ]*\).*/\1/p' | head -1 || true)
	dest_default=${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins
elif command -v flatpak >/dev/null && flatpak info "$FLATPAK_ID" >/dev/null 2>&1; then
	kind=flatpak
	version=$(flatpak info "$FLATPAK_ID" | sed -n 's/^ *Version: *\([0-9][0-9.]*\).*/\1/p' | head -1 || true)
	libobs=$(ls "$(flatpak info --show-location "$FLATPAK_ID")"/files/lib/libobs.so.* 2>/dev/null | head -1 || true)
	dest_default=$HOME/.var/app/$FLATPAK_ID/config/obs-studio/plugins
else
	die "OBS Studio isn't installed (no 'obs' command and no $FLATPAK_ID Flatpak)"
fi
DEST=${OBS_PLUGINS_DIR:-$dest_default}

if $uninstall; then
	for name in "${plugins[@]}"; do
		if [ -d "$DEST/$name" ]; then
			rm -rf "${DEST:?}/$name"
			say "removed $DEST/$name"
		fi
	done
	exit 0
fi

# Releases are tagged with three numbers (30.0.2); packages may add a fourth.
version=${OBS_VERSION:-$(printf '%s' "$version" | cut -d. -f1-3)}
[ -n "$version" ] || die "couldn't tell which version of OBS is installed; set OBS_VERSION"
[ -n "$libobs" ] && [ -f "$libobs" ] || die "couldn't find the libobs that OBS uses"

for tool in git cmake make cc; do
	command -v "$tool" >/dev/null || die "$tool is needed: sudo apt install git cmake build-essential"
done

say "OBS $version ($kind), plugins go in $DEST"
if [ "$kind" = flatpak ]; then
	say "note: installing into the Flatpak is untested: a plugin built here may need a newer"
	say "      C library than the Flatpak provides, in which case OBS won't load it."
fi

# --- libobs headers for this version -----------------------------------------
sdk=$CACHE/obs-$version
if [ ! -f "$sdk/config/obsconfig.h" ]; then
	say "fetching the libobs $version headers..."
	rm -rf "$sdk"
	mkdir -p "$sdk"
	if ! err=$(git clone -q --depth 1 --branch "$version" --filter=blob:none --sparse \
		https://github.com/obsproject/obs-studio "$sdk/src" 2>&1); then
		printf '%s\n' "$err" >&2
		die "couldn't fetch OBS $version from github.com/obsproject/obs-studio (if that isn't a released version, set OBS_VERSION to the nearest one)"
	fi
	git -C "$sdk/src" sparse-checkout set libobs >/dev/null
	# obsconfig.h is generated when OBS is built; its paths and build options
	# are never used by plugins.
	mkdir -p "$sdk/config"
	sed -e 's/^#cmakedefine .*//' -e 's/@[A-Z_]*@/0/g' "$sdk/src/libobs/obsconfig.h.in" >"$sdk/config/obsconfig.h.tmp"
	mv "$sdk/config/obsconfig.h.tmp" "$sdk/config/obsconfig.h"
fi
# From OBS 32, libobs's headers use the system's SIMDe instead of their own.
if [ ! -d "$sdk/src/libobs/util/simde" ] && [ ! -f /usr/include/simde/x86/sse2.h ]; then
	die "OBS $version's headers need SIMDe: sudo apt install libsimde-dev"
fi
# The frontend API header (for plugins that add to OBS's window), from the same
# version: UI/obs-frontend-api up to OBS 30, frontend/api from 31.
if [ ! -f "$sdk/src/UI/obs-frontend-api/obs-frontend-api.h" ] && [ ! -f "$sdk/src/frontend/api/obs-frontend-api.h" ]; then
	git -C "$sdk/src" sparse-checkout add UI/obs-frontend-api frontend/api >/dev/null 2>&1 ||
		say "warning: couldn't fetch OBS $version's frontend API header"
fi
frontend_inc=$sdk/src/frontend/api
[ -f "$frontend_inc/obs-frontend-api.h" ] || frontend_inc=$sdk/src/UI/obs-frontend-api
# The frontend API library OBS itself loads.
if [ "$kind" = native ]; then
	frontend_lib=$(ldd "$(command -v obs)" 2>/dev/null | sed -n 's/^[[:space:]]*libobs-frontend-api\.so[.0-9]* => \(\/[^ ]*\).*/\1/p' | head -1 || true)
else
	frontend_lib=$(ls "$(dirname "$libobs")"/libobs-frontend-api.so.* 2>/dev/null | head -1 || true)
fi

# Written every time: the libobs in use can change without its version changing.
mkdir -p "$sdk/cmake"
cat >"$sdk/cmake/libobsConfig.cmake" <<CMAKE
# libobs as installed, with the headers of the same OBS version.
if(NOT TARGET OBS::libobs)
  add_library(OBS::libobs SHARED IMPORTED)
  set_target_properties(OBS::libobs PROPERTIES
    IMPORTED_LOCATION "$libobs"
    INTERFACE_INCLUDE_DIRECTORIES "$sdk/src/libobs;$sdk/config")
endif()
set(libobs_FOUND TRUE)
CMAKE
cat >"$sdk/cmake/obs-frontend-apiConfig.cmake" <<CMAKE
# OBS's frontend API as installed, with the header of the same OBS version.
if(NOT EXISTS "$frontend_lib" OR NOT EXISTS "$frontend_inc/obs-frontend-api.h")
  set(obs-frontend-api_FOUND FALSE)
  set(obs-frontend-api_NOT_FOUND_MESSAGE "OBS's frontend API (libobs-frontend-api and its header) wasn't found")
  return()
endif()
find_package(libobs REQUIRED)
if(NOT TARGET OBS::obs-frontend-api)
  add_library(OBS::obs-frontend-api SHARED IMPORTED)
  set_target_properties(OBS::obs-frontend-api PROPERTIES
    IMPORTED_LOCATION "$frontend_lib"
    INTERFACE_INCLUDE_DIRECTORIES "$frontend_inc"
    INTERFACE_LINK_LIBRARIES OBS::libobs)
endif()
set(obs-frontend-api_FOUND TRUE)
CMAKE

# --- Build and install ---------------------------------------------------------
if pgrep -x obs >/dev/null 2>&1; then
	say "note: OBS is running; restart it to load the new plugins."
fi
# A plugin that doesn't build is reported and skipped, and the rest are still
# installed; the script fails at the end, naming it.
failed=()
skip() {
	printf 'install.sh: %s (its log: %s)\n' "$1" "$build.log" >&2
	failed+=("$name")
}
for name in "${plugins[@]}"; do
	src=$ROOT/plugins/$name
	build=$CACHE/build/obs-$version/$name
	say "building $name..."
	mkdir -p "$build"
	# (--no-warn-unused-cli: the frontend API's folder is given to every
	# plugin, and only those that add to OBS's window use it.)
	if ! cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -Dlibobs_DIR="$sdk/cmake" \
		-Dobs-frontend-api_DIR="$sdk/cmake" --no-warn-unused-cli -Wno-dev >"$build.log" 2>&1; then
		cat "$build.log" >&2
		skip "configuring $name failed"
		continue
	fi
	if ! cmake --build "$build" -j"$(nproc)" >>"$build.log" 2>&1; then
		tail -40 "$build.log" >&2
		skip "building $name failed"
		continue
	fi
	module=$(find "$build" -maxdepth 2 -name "$name.so" | head -1)
	if [ -z "$module" ]; then
		skip "building $name made no $name.so"
		continue
	fi

	# Put the new copy together beside the old one and swap it in, so a
	# failure part-way leaves the working copy in place.
	new=$DEST/.$name.new
	rm -rf "$new" "$DEST/.$name.old"
	mkdir -p "$new/bin/64bit"
	cp "$module" "$new/bin/64bit/"
	if [ -d "$src/data" ]; then
		cp -r "$src/data" "$new/data"
	fi
	if [ -d "$DEST/$name" ]; then
		mv "$DEST/$name" "$DEST/.$name.old"
	fi
	mv "$new" "$DEST/$name"
	rm -rf "$DEST/.$name.old"
	say "installed $name"

	for sys in /usr/lib/x86_64-linux-gnu/obs-plugins /usr/lib/obs-plugins /usr/local/lib/obs-plugins; do
		if [ -f "$sys/$name.so" ]; then
			say "warning: OBS also has a system-wide $name ($sys/$name.so); remove that package"
			say "         or OBS will load two copies."
		fi
	done
done

if [ ${#failed[@]} -gt 0 ]; then
	if [ ${#failed[@]} -lt ${#plugins[@]} ]; then
		die "not installed: ${failed[*]} (see above); the others were"
	fi
	die "not installed: ${failed[*]} (see above)"
fi
