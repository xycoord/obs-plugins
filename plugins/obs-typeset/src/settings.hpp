// A Typeset Text's settings: its style and styled runs read from them, and
// styles copied from one source to another or saved as the default for new
// ones.
#pragma once

#include "text-engine.hpp"

#include <obs.h>

#include <string>

namespace typeset {

void set_defaults(obs_data_t *settings);
Style read_style(obs_data_t *settings);
Spans read_spans(obs_data_t *settings);
obs_data_array_t *spans_array(const Spans &spans);

// The size field mirrors the font's size (which is what counts). Set when a
// source is made and when its font is picked, never from OBS's graphics
// thread (where updates run), as the dialog may be using the settings.
void sync_size(obs_data_t *settings);

// A source's style, every value as it stands (defaults included), marked as
// a style, for the clipboard or the default style's file.
obs_data_t *style_of(obs_data_t *settings);
// Just the style's settings from data that may hold anything (a paste), if
// it's marked as a style; null otherwise.
obs_data_t *pasted_style(obs_data_t *data);

// A source just added takes the style saved as the default, if any, for
// what it wasn't given (by a script, say). "styled" marks every source once
// it has had the chance, so a saved scene isn't restyled when it's loaded.
void apply_default_style(obs_data_t *settings);
bool save_default_style(obs_data_t *settings);

} // namespace typeset
