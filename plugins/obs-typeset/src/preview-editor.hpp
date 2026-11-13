// Editing Typeset text right in OBS's preview, the way a text box is edited in
// a slide editor: double-click the text to start (it selects the word), or
// select it and press F2 (it selects all of it), then
// type, click or drag to place the caret or select, and use the usual keys
// (arrows, Home/End, Ctrl+arrows by word, Shift to select, Ctrl+A/C/X/V,
// Ctrl+Z/Ctrl+Shift+Z). Escape, Ctrl+Return or a click outside the text
// finishes. An empty text shows where it is with a dashed frame.
//
// The caret and selection are drawn on the preview only, never into the
// output, so they don't show on the stream, recording or projectors.
#pragma once

namespace typeset {

void editor_install(); // once OBS's window is up
void editor_remove();  // before it goes
void editor_scene_changed(); // the scene shown, or the scene collection, is changing

} // namespace typeset
