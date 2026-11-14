// What a saved scene is made of, and how it's changed to be loaded back: plain
// changes to the data OBS saves sources as, with no OBS running needed (the
// unit tests run them on their own).
#pragma once

#include <obs.hpp>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace library {

// Calls fn for every string set in a tree of settings (into objects and arrays),
// with the key it's under; fn returns a replacement, or nullptr to leave it.
using StringFn = std::function<const std::string *(const char *key, const char *value)>;
void rewrite_strings(obs_data_t *settings, const StringFn &fn);

// The settings a saved source has that can name files or other sources: its own
// and its filters' (a scene's own hold its items, with their show and hide
// transitions).
void for_each_settings(obs_data_t *source, const std::function<void(obs_data_t *)> &fn);

// Every file or folder that the saved sources' settings name by its full path,
// and that exists.
std::vector<std::string> referenced_paths(obs_data_array_t *sources);

// Whether a string looks like a full path (no check it exists).
bool looks_like_path(const char *s);

// What there is in OBS already, for loading a saved scene beside it.
struct Existing {
	// Whether a source or scene already has this name.
	std::function<bool(const std::string &name)> name_taken;
	// Whether sources of a type capture something outside OBS (a camera, a
	// microphone, the screen): a second copy of one would fight the first for
	// the device, or double its sound, so the one in OBS is used instead.
	std::function<bool(const std::string &id)> captures;
	// The uuid of the source called `name`, if it's of this type, else "".
	std::function<std::string(const std::string &name, const std::string &id)> device;
	// A new uuid.
	std::function<std::string()> new_uuid;
};

struct Prepared {
	OBSDataArrayAutoRelease sources;           // ready for obs_load_sources
	std::string root;                          // the scene's name once loaded
	std::map<std::string, std::string> names;  // every saved name, to its name once loaded
	std::vector<std::string> reused;           // devices the scene uses as they are in OBS
};

// Makes a saved scene's sources ready to load beside what's in OBS, without
// touching anything there: every source gets a new uuid, and a name of its own
// (its saved one if that's free, else with " 2", " 3" added); the scene is called
// `root_name` (or that with a number). References between them follow (a scene's
// items, and any setting that names one of them, except in keys that hold text
// to show). Settings naming a file in `paths` are pointed where it is now.
// Capture sources already in OBS (same name and type) are used as they are,
// not copied. Hotkeys aren't kept: a copy shouldn't answer to its original's
// keys.
Prepared prepare_for_load(obs_data_array_t *saved, const std::string &root_saved_name, const std::string &root_name,
			  const Existing &existing, const std::map<std::string, std::string> &paths);

// The source types used by saved sources and their filters (versioned ids).
std::set<std::string> source_types(obs_data_array_t *sources);

} // namespace library
