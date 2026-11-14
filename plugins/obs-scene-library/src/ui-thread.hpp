// OBS's UI thread, where scenes are taken and added (the frontend adds a new
// scene to its list as it's created, and only does that there).
#pragma once

#include <obs.hpp>

#include <functional>

namespace library {

void run_on_ui_thread(const std::function<void()> &fn);

// The scene being edited: in Studio Mode the preview's, else the one showing.
obs_source_t *edited_scene();

// Tells the dock the library has changed.
void notify_changed();

} // namespace library
