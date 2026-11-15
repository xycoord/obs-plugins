// OBS's UI thread, where scenes are taken and added (the frontend adds a new
// scene to its list as it's created, and only does that there).
#pragma once

#include <obs.hpp>

#include <functional>

namespace library {

// Runs fn there and waits for it. False, without running it, once OBS is
// closing: its UI thread may never get to it.
bool run_on_ui_thread(const std::function<void()> &fn);

// Whether OBS is closing (anything long, like copying files, stops).
bool exiting();

// The scene being edited: in Studio Mode the preview's, else the one showing.
obs_source_t *edited_scene();

// Tells the dock the library has changed.
void notify_changed();

} // namespace library
