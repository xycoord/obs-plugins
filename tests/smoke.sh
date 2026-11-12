#!/usr/bin/env bash
# Start the installed OBS with the plugins install.sh put in place and check it
# loads every one, then load each plugin's test scene (tests/scenes/<name>.json,
# if there is one) and check that all of its filters were created and OBS
# logged no errors for them.
#
#   tests/smoke.sh          (needs a display: run under xvfb-run -a on a server)
#
# In a scene file, @DATA@ stands for the plugin's installed data folder and
# @TESTS@ for this tests folder (for images in tests/fixtures).
#
# Uses a throwaway OBS configuration: the plugins folder is linked from the
# real one, nothing else of the user's OBS setup is read or changed. Native OBS
# only, not the Flatpak.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TESTS=$ROOT/tests
PLUGINS=${OBS_PLUGINS_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins}
SECONDS_RUN=${SMOKE_SECONDS:-20}

fake=$(mktemp -d)
trap 'rm -rf "$fake"' EXIT
cfg=$fake/.config/obs-studio
mkdir -p "$cfg/basic/scenes" "$cfg/basic/profiles/Smoke"
ln -s "$PLUGINS" "$cfg/plugins"
# OBS 30 reads these from global.ini, OBS 31 and later from user.ini.
for ini in global.ini user.ini; do
	cat >"$cfg/$ini" <<-EOF
		[General]
		FirstRun=false
		[Basic]
		SceneCollection=Smoke
		SceneCollectionFile=Smoke
		Profile=Smoke
		ProfileDir=Smoke
		ConfigOnNewProfile=false
	EOF
done

failed=0
fail() {
	echo "FAIL  $1"
	failed=1
}

# Runs OBS and sets $log to the log it wrote; fails and returns 1 if OBS didn't
# get through startup.
run_obs() {
	rm -rf "$cfg/logs"
	# OBS is stopped by a signal, which it may not exit cleanly from; that
	# isn't what's being tested. --kill-after: in case it doesn't stop.
	# --multi: no "already running" dialog if the real OBS is open. HOME is
	# moved, so X needs telling where its cookie is.
	(cd "$fake" && XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority} HOME=$fake XDG_CONFIG_HOME=$fake/.config \
		timeout --kill-after=5s "$SECONDS_RUN" obs --multi --disable-shutdown-check --disable-updater \
		>"$fake/obs.out" 2>&1) 2>/dev/null || true
	log=$(ls -t "$cfg"/logs/*.txt 2>/dev/null | head -1 || true)
	if [ -z "$log" ] || ! grep -q 'Startup complete' "$log"; then
		fail "OBS didn't finish starting ($1); its output:"
		tail -30 "$fake/obs.out" | sed 's/^/      /'
		return 1
	fi
}

names=()
for dir in "$ROOT"/plugins/*/; do
	[ -f "$dir/CMakeLists.txt" ] && names+=("$(basename "$dir")")
done

# Problems OBS reports that come from parts of OBS a test machine lacks.
not_ours='decklink|portals|dbus|v4l2|pipewire|pulse|alsa|vaapi|nvenc|qsv|amf|jack|browser|rtmp-services'

# Errors and failures OBS reports once a scene has loaded, without timestamps.
errors_after_startup() {
	sed -n '/Startup complete/,$p' "$log" | sed 's/^[0-9:.]*: //' |
		grep -i -E 'error|fail|unable|warning|not found' | grep -v -i -E "$not_ours" | sort -u || true
}

# A control scene with only OBS's own sources and filter: check every plugin
# loads, and that nothing about it went wrong while modules loaded (a missing
# locale, for one). What OBS reports here after startup is OBS's own noise (it
# varies by version: OBS 32 logs Qt messages once a scene has sources) and
# isn't held against the plugins' scenes.
cat >"$cfg/basic/scenes/Smoke.json" <<EOF
{"name": "Smoke", "current_scene": "Scene", "current_program_scene": "Scene",
 "scene_order": [{"name": "Scene"}],
 "sources": [
  {"id": "scene", "versioned_id": "scene", "name": "Scene", "settings": {"id_counter": 2,
   "items": [{"name": "Image", "visible": true, "id": 1}, {"name": "Colour", "visible": true, "id": 2}]}},
  {"id": "image_source", "versioned_id": "image_source", "name": "Image",
   "settings": {"file": "$TESTS/fixtures/logo.jpg"},
   "filters": [{"id": "color_filter", "versioned_id": "color_filter_v2", "name": "Colour Correction",
                "enabled": true, "settings": {"gamma": 0.1}}]},
  {"id": "color_source", "versioned_id": "color_source_v3", "name": "Colour",
   "settings": {"color": 4282417407, "width": 64, "height": 64}}]}
EOF
baseline=""
if run_obs "control scene"; then
	baseline=$(errors_after_startup)
	for name in "${names[@]}"; do
		if sed -n '/Loaded Modules:/,$p' "$log" | grep -q "^[0-9:.]*: *$name\.so$"; then
			echo "ok    $name loads"
		else
			fail "$name didn't load:"
			grep -i "$name" "$log" | sed 's/^/      /'
		fi
		problems=$(grep -i -F "$name" "$log" | grep -i -E 'error|fail|unable|warning' || true)
		if [ -n "$problems" ]; then
			fail "$name reported problems while loading:"
			printf '%s\n' "$problems" | sed 's/^/      /'
		fi
	done
fi

for name in "${names[@]}"; do
	scene=$TESTS/scenes/$name.json
	[ -f "$scene" ] || continue
	sed -e "s|@DATA@|$PLUGINS/$name/data|g" -e "s|@TESTS@|$TESTS|g" "$scene" >"$cfg/basic/scenes/Smoke.json"
	run_obs "$name's test scene" || continue

	# Every filter in the scene has to have been created.
	missing=$(python3 - "$cfg/basic/scenes/Smoke.json" "$log" <<-'EOF'
		import json, sys
		scene = json.load(open(sys.argv[1]))
		log = open(sys.argv[2], errors="replace").read()
		for src in scene.get("sources", []):
		    for f in src.get("filters", []):
		        if f"- filter: '{f['name']}' ({f['id']})" not in log:
		            print(f"{src['name']}: {f['name']} ({f['id']})")
	EOF
	)
	if [ -n "$missing" ]; then
		fail "$name's test scene: filters not created:"
		printf '%s\n' "$missing" | sed 's/^/      /'
	fi
	# Errors or failures reported once the scene loads, other than ones OBS
	# reports with no scene at all.
	errors=$(comm -23 <(errors_after_startup) <(printf '%s\n' "$baseline" | sort -u) | sed '/^$/d')
	if [ -n "$errors" ]; then
		fail "$name's test scene:"
		printf '%s\n' "$errors" | sed 's/^/      /'
	elif [ -z "$missing" ]; then
		echo "ok    $name's test scene loads without errors"
	fi
done
exit $failed
