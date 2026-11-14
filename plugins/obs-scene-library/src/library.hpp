// The library: a folder with a folder for each saved scene, holding
//
//   scene.json      the scene and everything in it, as OBS saves them
//   thumbnail.png   how it looked when it was saved
//   files/          copies of the images, videos and other files it uses
//
// Saving and loading are done on OBS's UI thread, except copying a scene's files
// into the library, which can take a while (write()), and is done on any thread.
#pragma once

#include <obs.hpp>

#include <QImage>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace library {

namespace fs = std::filesystem;

// --- The folder --------------------------------------------------------------

void load_config();
fs::path folder();
void set_folder(const fs::path &dir);
std::string sort_order(); // "name" or "saved"
void set_sort_order(const std::string &order);
bool dock_introduced(); // whether the dock has been shown once
void set_dock_introduced();

// --- Saved scenes ------------------------------------------------------------

struct Entry {
	fs::path dir;
	std::string name;     // what it's called in the library
	std::string scene;    // what the scene was called in OBS
	int64_t saved = 0;    // when, in seconds since 1970
	uint32_t width = 0;   // the canvas it was saved from
	uint32_t height = 0;
	int sources = 0;      // how many sources it has (the scene itself and nested scenes too)
	int files = 0;        // how many files it has copies of
	std::vector<std::string> not_copied; // files it uses that weren't copied, and why
	fs::path thumbnail() const { return dir / "thumbnail.png"; }
};

std::vector<Entry> entries();
std::optional<Entry> find(const std::string &name);

// Takes a scene as it is now, to save as `name`.
struct Snapshot {
	std::string name;
	OBSDataAutoRelease data; // scene.json, but for the list of files
	std::vector<std::string> paths;
	QImage thumbnail;
};
Snapshot snapshot(obs_source_t *scene, const std::string &name);

// Writes a snapshot into the library, copying the files it uses. A saved scene
// with the same name is replaced if `replace`, else it fails. `progress` is told
// how many bytes of how many have been copied, and stops the saving by
// returning false.
struct Saved {
	bool ok = false;
	std::string error;
	Entry entry;
};
using Progress = std::function<bool(uint64_t done, uint64_t total)>;
Saved write(Snapshot snap, bool replace, const Progress &progress = nullptr);

// Adds a saved scene to OBS's scenes, beside what's there and without changing
// any of it. In Studio Mode it's put in the preview.
struct Loaded {
	bool ok = false;
	std::string error;
	std::string scene;                       // its name in OBS
	std::vector<std::string> missing_types;  // source types not installed
	std::vector<std::string> missing_files;  // files neither where they were nor copied
	std::vector<std::string> reused;         // devices it uses as they are in OBS
	bool other_canvas = false;               // saved from a canvas of another size
};
Loaded load(const Entry &entry);

bool remove(const Entry &entry, std::string &error);
bool rename(const Entry &entry, const std::string &name, std::string &error);

// Removes what a save that never finished left behind.
void tidy();

// A folder name for a saved scene called `name`.
std::string sanitise_name(const std::string &name);

// The scene as it looks now, at its full size, over black (for the thumbnail).
QImage render(obs_source_t *scene);

} // namespace library
