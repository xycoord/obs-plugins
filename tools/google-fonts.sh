#!/usr/bin/env bash
# Install the whole Google Fonts library for this user, or update it.
#
#   tools/google-fonts.sh            install, or update what's installed
#   tools/google-fonts.sh --remove   remove it again
#
# The fonts come from Google's own repository (github.com/google/fonts): only
# its latest version, and only the font files and each family's information
# (no history), into ~/.local/share/fonts/google-fonts (GOOGLE_FONTS_DIR
# overrides it), where every program finds them. No sudo needed.
#
# Beside them it writes typeset-catalogue.json, which Typeset Text's font
# browser reads: each family's category, finer classes, feel tags and
# popularity, from the repository's tags and Google's public catalogue
# (fonts.google.com, fetched once here; without it, the categories still come
# from the repository, with no popularity).
#
# Needs git, python3 and curl, and fontconfig's fc-cache.
set -euo pipefail

DIR=${GOOGLE_FONTS_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/fonts/google-fonts}
REPO=https://github.com/google/fonts
CATALOGUE_URL=https://fonts.google.com/metadata/fonts
HERE=$(cd "$(dirname "$0")" && pwd)

say() { printf '%s\n' "$*"; }
die() { printf 'google-fonts.sh: %s\n' "$*" >&2; exit 1; }

if [ "${1:-}" = --remove ]; then
	[ -d "$DIR/.git" ] || die "nothing installed at $DIR"
	rm -rf "$DIR"
	command -v fc-cache >/dev/null && fc-cache -f >/dev/null 2>&1 || true
	say "removed $DIR"
	exit 0
elif [ $# -gt 0 ]; then
	sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
	exit 0
fi

for tool in git python3 curl fc-cache; do
	command -v "$tool" >/dev/null || die "needs $tool (sudo apt install git python3 curl fontconfig)"
done

# Only these paths are checked out: the fonts and their information files in
# each licence's folder (ofl/, apache/, ufl/), and the tags.
sparse() {
	git -C "$DIR" sparse-checkout set --no-cone \
		'/*/*/*.ttf' '/*/*/*.otf' '/*/*/METADATA.pb' '/tags/all/families.csv' '/tags/tags_metadata.csv'
}

if [ -d "$DIR/.git" ]; then
	say "updating the Google Fonts library in $DIR..."
	git -C "$DIR" fetch --depth 1 --filter=blob:none origin main
	sparse
	was=$(git -C "$DIR" rev-parse HEAD)
	git -C "$DIR" reset --hard --quiet FETCH_HEAD
	# Only the latest version kept: what it replaced goes.
	if [ "$(git -C "$DIR" rev-parse HEAD)" != "$was" ]; then
		rm -f "$DIR/.git/ORIG_HEAD"
		git -C "$DIR" reflog expire --expire=now --all
		git -C "$DIR" gc --prune=now --quiet
	fi
else
	[ -e "$DIR" ] && [ -n "$(ls -A "$DIR" 2>/dev/null)" ] && die "$DIR exists and isn't a copy made by this script"
	say "fetching the Google Fonts library into $DIR (about 1,900 families; this takes a while)..."
	mkdir -p "$(dirname "$DIR")"
	git clone --depth 1 --filter=blob:none --sparse --branch main "$REPO" "$DIR"
	sparse
fi

say "building the catalogue for Typeset Text's font browser..."
raw=$(mktemp)
trap 'rm -f "$raw"' EXIT
if ! curl -fsS --max-time 120 "$CATALOGUE_URL" -o "$raw"; then
	say "warning: couldn't fetch Google's catalogue; categories come from the repository, with no popularity"
	: >"$raw"
fi
python3 -I "$HERE/google-fonts-catalogue.py" "$DIR" "$raw" "$DIR/typeset-catalogue.json"

say "updating the font cache..."
fc-cache -f "$DIR" >/dev/null
count=$(find "$DIR" -name METADATA.pb | wc -l)
size=$(du -sh --exclude=.git "$DIR" | cut -f1)
say "done: $count families, $size, in $DIR. Restart OBS (and other programs) to see them."
