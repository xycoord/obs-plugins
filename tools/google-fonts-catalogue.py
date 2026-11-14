#!/usr/bin/env python3
"""Build typeset-catalogue.json for Typeset Text's font browser from a copy of
Google's fonts repository and (optionally) Google's public catalogue.

    google-fonts-catalogue.py REPO_DIR CATALOGUE_JSON_OR_EMPTY OUT.json

Per family (by its name as fonts list it):
  category    "Sans Serif", "Serif", "Display", "Handwriting" or "Monospace"
  classes     finer classes from the tags, strongest first, "|" between: "Geometric"
  feel        feel tags with their strength (0-100), e.g. {"Calm": 81}
  popularity  Google's rank (1 = most used), absent if unknown
  designers ("|" between), added (date), width (Google's 1-10 rating, the median style's)

Written by tools/google-fonts.sh; read only by the plugin.
"""

import csv
import json
import os
import re
import sys

CATEGORY = {"SANS_SERIF": "Sans Serif", "SERIF": "Serif", "DISPLAY": "Display",
            "HANDWRITING": "Handwriting", "MONOSPACE": "Monospace"}
# Tag groups that are finer classes of the five categories.
CLASS_GROUPS = {"Sans", "Serif", "Slab", "Script", "Monospace", "Display", "Symbol"}
CLASS_MIN = 30  # strength from which a class counts
FEEL_MIN = 20   # and a feel tag


def from_metadata(repo):
    """Name, category, designers and date from each METADATA.pb (a simple
    text-format reading: only top-level fields are wanted)."""
    out = {}
    for licence in ("ofl", "apache", "ufl"):
        root = os.path.join(repo, licence)
        if not os.path.isdir(root):
            continue
        for fam in os.listdir(root):
            path = os.path.join(root, fam, "METADATA.pb")
            if not os.path.isfile(path):
                continue
            text = open(path, encoding="utf-8", errors="replace").read()
            # Top-level fields only: drop nested blocks (fonts { ... }, axes { ... }).
            top = re.sub(r"\n\w+ \{.*?\n\}", "", "\n" + text, flags=re.S)
            name = re.search(r'^name: "(.*)"', top, re.M)
            if not name:
                continue
            entry = {}
            cat = re.search(r"^category: \"(\w+)\"", top, re.M)
            if cat and cat.group(1) in CATEGORY:
                entry["category"] = CATEGORY[cat.group(1)]
            designer = re.search(r'^designer: "(.*)"', top, re.M)
            if designer:
                entry["designers"] = [d.strip() for d in designer.group(1).split(",") if d.strip()]
            added = re.search(r'^date_added: "(.*)"', top, re.M)
            if added:
                entry["added"] = added.group(1)
            out[name.group(1)] = entry
    return out


def from_catalogue(path):
    if not path or not os.path.isfile(path) or os.path.getsize(path) == 0:
        return {}
    raw = open(path, encoding="utf-8").read()
    if raw.startswith(")]}'"):
        raw = raw[4:]
    out = {}
    for f in json.loads(raw).get("familyMetadataList", []):
        entry = {}
        if f.get("category"):
            entry["category"] = f["category"]
        if f.get("popularity"):
            entry["popularity"] = int(f["popularity"])
        if f.get("designers"):
            entry["designers"] = f["designers"]
        if f.get("dateAdded"):
            entry["added"] = f["dateAdded"]
        widths = sorted(s.get("width") for s in (f.get("fonts") or {}).values() if s.get("width"))
        if widths:
            entry["width"] = widths[len(widths) // 2]
        out[f["family"]] = entry
    return out


def from_tags(repo):
    """Finer classes and feel tags, family-wide ones only (no axis
    positions)."""
    classes, feel = {}, {}
    path = os.path.join(repo, "tags", "all", "families.csv")
    if not os.path.isfile(path):
        return classes, feel
    with open(path, encoding="utf-8", newline="") as f:
        for row in csv.reader(f):
            if len(row) < 4 or row[1].strip():
                continue
            family, tag, strength = row[0], row[2], row[3]
            try:
                strength = float(strength)
            except ValueError:
                continue
            parts = tag.strip("/").split("/")
            if len(parts) != 2:
                continue
            group, name = parts
            if group in CLASS_GROUPS and strength >= CLASS_MIN:
                classes.setdefault(family, []).append((strength, name))
            elif group == "Expressive" and strength >= FEEL_MIN:
                feel.setdefault(family, {})[name] = int(round(strength))
    return ({k: [n for _, n in sorted(v, key=lambda x: -x[0])] for k, v in classes.items()}, feel)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    repo, catalogue, out_path = sys.argv[1:]
    families = from_metadata(repo)
    online = from_catalogue(catalogue)
    classes, feel = from_tags(repo)
    for name, entry in families.items():
        entry.update({k: v for k, v in online.get(name, {}).items() if v})
        if name in classes:
            entry["classes"] = classes[name]
        if name in feel:
            entry["feel"] = feel[name]
        # Lists as one string, "|" between (OBS's JSON reading keeps only
        # objects in arrays).
        for key in ("classes", "designers"):
            if key in entry:
                entry[key] = "|".join(entry[key])
    data = {"version": 1, "families": dict(sorted(families.items()))}
    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
    os.replace(tmp, out_path)
    print("catalogue: %d families (%d with popularity, %d with classes, %d with feel tags)" % (
        len(families), sum(1 for e in families.values() if "popularity" in e),
        sum(1 for e in families.values() if "classes" in e), sum(1 for e in families.values() if "feel" in e)))


if __name__ == "__main__":
    main()
