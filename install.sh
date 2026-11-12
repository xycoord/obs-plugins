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
	git -C "$ROOT" submodule update --init --recursive >/dev/null
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
	version=$(obs --version 2>/dev/null | sed -n 's/.*OBS Studio - \([0-9][0-9.]*\).*/\1/p' | head -1)
	# The libobs the obs program itself loads (its file name differs between
	# Ubuntu's package and OBS's own builds).
	libobs=$(ldd "$(command -v obs)" 2>/dev/null | sed -n 's/^[[:space:]]*libobs\.so[.0-9]* => \(\/[^ ]*\).*/\1/p' | head -1)
	dest_default=${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins
elif command -v flatpak >/dev/null && flatpak info "$FLATPAK_ID" >/dev/null 2>&1; then
	kind=flatpak
	version=$(flatpak info "$FLATPAK_ID" | sed -n 's/^ *Version: *\([0-9][0-9.]*\).*/\1/p' | head -1)
	libobs=$(ls "$(flatpak info --show-location "$FLATPAK_ID")"/files/lib/libobs.so.* 2>/dev/null | head -1)
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

for tool in git cmake cc; do
	command -v "$tool" >/dev/null || die "$tool is needed: sudo apt install git cmake build-essential"
done

say "OBS $version ($kind), plugins go in $DEST"
if [ "$kind" = flatpak ]; then
	say "note: installing into the Flatpak is untested: a plugin built here may need a newer"
	say "      C library than the Flatpak provides, in which case OBS won't load it."
fi

# --- libobs headers for this version -----------------------------------------
sdk=$CACHE/obs-$version
if [ ! -f "$sdk/cmake/libobsConfig.cmake" ]; then
	say "fetching the libobs $version headers..."
	rm -rf "$sdk"
	mkdir -p "$sdk"
	git clone -q --depth 1 --branch "$version" --filter=blob:none --sparse \
		https://github.com/obsproject/obs-studio "$sdk/src" 2>/dev/null ||
		die "couldn't fetch OBS $version from github.com/obsproject/obs-studio"
	git -C "$sdk/src" sparse-checkout set --no-cone '/libobs/**' >/dev/null
	# obsconfig.h is generated when OBS is built; its paths and build options
	# are never used by plugins.
	mkdir -p "$sdk/config"
	sed -e 's/^#cmakedefine .*//' -e 's/@[A-Z_]*@/0/g' "$sdk/src/libobs/obsconfig.h.in" >"$sdk/config/obsconfig.h"
	mkdir -p "$sdk/cmake"
	cat >"$sdk/cmake/libobsConfig.cmake.in" <<-'EOF'
		# libobs as installed, with the headers of the same OBS version.
		if(NOT TARGET OBS::libobs)
		  add_library(OBS::libobs SHARED IMPORTED)
		  set_target_properties(OBS::libobs PROPERTIES
		    IMPORTED_LOCATION "@LIBOBS@"
		    INTERFACE_INCLUDE_DIRECTORIES "@SDK@/src/libobs;@SDK@/config")
		endif()
		set(libobs_FOUND TRUE)
	EOF
	sed -e "s|@LIBOBS@|$libobs|" -e "s|@SDK@|$sdk|g" "$sdk/cmake/libobsConfig.cmake.in" >"$sdk/cmake/libobsConfig.cmake"
fi

# --- Build and install ---------------------------------------------------------
if pgrep -x obs >/dev/null 2>&1; then
	say "note: OBS is running; restart it to load the new plugins."
fi
for name in "${plugins[@]}"; do
	src=$ROOT/plugins/$name
	build=$CACHE/build/obs-$version/$name
	say "building $name..."
	mkdir -p "$build"
	cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -Dlibobs_DIR="$sdk/cmake" \
		-Wno-dev >"$build.log" 2>&1 || { cat "$build.log" >&2; die "configuring $name failed"; }
	cmake --build "$build" -j"$(nproc)" >>"$build.log" 2>&1 || { tail -40 "$build.log" >&2; die "building $name failed"; }
	module=$(find "$build" -maxdepth 2 -name "$name.so" | head -1)
	[ -n "$module" ] || die "building $name made no $name.so"

	rm -rf "${DEST:?}/$name"
	mkdir -p "$DEST/$name/bin/64bit"
	cp "$module" "$DEST/$name/bin/64bit/"
	[ -d "$src/data" ] && cp -r "$src/data" "$DEST/$name/data"
	say "installed $name"

	for sys in /usr/lib/x86_64-linux-gnu/obs-plugins /usr/lib/obs-plugins /usr/local/lib/obs-plugins; do
		if [ -f "$sys/$name.so" ]; then
			say "warning: OBS also has a system-wide $name ($sys/$name.so); remove that package"
			say "         or OBS will load two copies."
		fi
	done
done
