#!/usr/bin/env bash
# Start the installed OBS with the plugins install.sh put in place and check it
# loads every one, then load each plugin's test scene (tests/scenes/<name>.json,
# if there is one) and check OBS logs no errors for it.
#
#   tests/smoke.sh          (needs a display: run under xvfb-run -a on a server)
#
# Uses a throwaway OBS configuration: the plugins folder is linked from the
# real one, nothing else of the user's OBS setup is read or changed.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PLUGINS=${OBS_PLUGINS_DIR:-$HOME/.config/obs-studio/plugins}
SECONDS_RUN=${SMOKE_SECONDS:-20}

fake=$(mktemp -d)
trap 'rm -rf "$fake"' EXIT
cfg=$fake/.config/obs-studio
mkdir -p "$cfg/basic/scenes" "$cfg/basic/profiles/Smoke"
ln -s "$PLUGINS" "$cfg/plugins"
cat >"$cfg/global.ini" <<EOF
[General]
FirstRun=false
[Basic]
SceneCollection=Smoke
SceneCollectionFile=Smoke
Profile=Smoke
ProfileDir=Smoke
ConfigOnNewProfile=false
EOF

run_obs() {
	rm -rf "$cfg/logs"
	# OBS is stopped by a signal, which it may not exit cleanly from; that
	# isn't what's being tested.
	(cd "$fake" && HOME=$fake timeout "$SECONDS_RUN" obs --disable-shutdown-check --disable-updater >"$fake/obs.out" 2>&1) 2>/dev/null || true
	ls -t "$cfg"/logs/*.txt 2>/dev/null | head -1 || true
}

no_log() {
	echo "FAIL  OBS wrote no log; its output:"
	tail -30 "$fake/obs.out" | sed 's/^/      /'
	exit 1
}

failed=0
names=()
for dir in "$ROOT"/plugins/*/; do
	[ -f "$dir/CMakeLists.txt" ] && names+=("$(basename "$dir")")
done

# An empty scene collection: just check every plugin loads.
echo '{"name":"Smoke","sources":[]}' >"$cfg/basic/scenes/Smoke.json"
log=$(run_obs)
[ -n "$log" ] || no_log
for name in "${names[@]}"; do
	if sed -n '/Loaded Modules:/,$p' "$log" | grep -q "^[0-9:.]*: *$name\.so$"; then
		echo "ok    $name loads"
	else
		echo "FAIL  $name didn't load:"
		grep -i "$name" "$log" | sed 's/^/      /'
		failed=1
	fi
done

# Each plugin's own test scene. @DATA@ in it becomes the plugin's installed
# data folder.
for name in "${names[@]}"; do
	scene=$ROOT/tests/scenes/$name.json
	[ -f "$scene" ] || continue
	sed "s|@DATA@|$PLUGINS/$name/data|g" "$scene" >"$cfg/basic/scenes/Smoke.json"
	log=$(run_obs)
	[ -n "$log" ] || no_log
	# Lines after the scene starts loading that report an error or failure,
	# except ones from parts of OBS the test machine lacks.
	errors=$(sed -n '/Startup complete/,$p' "$log" |
		grep -i -E 'error|fail|unable|warning' |
		grep -v -i -E 'decklink|portals|dbus|v4l2|pipewire|pulse|alsa|vaapi|nvenc|qsv|amf|jack|browser' || true)
	if [ -n "$errors" ]; then
		echo "FAIL  $name's test scene:"
		printf '%s\n' "$errors" | sed 's/^/      /'
		failed=1
	else
		echo "ok    $name's test scene loads without errors"
	fi
done
exit $failed
