// What the editor draws on OBS's preview (never into the output): the frame
// round the text being edited, its caret and selection, a veil over the rest
// of the canvas, and placeholders for empty texts, in the theme's highlight
// colour. (The format bar is Qt's: see format-bar.hpp.) The editor describes it (an Overlay); the painter draws that on OBS's
// graphics thread, from the scene items' transforms as they are at that
// moment, so it follows the text wherever it's moved from.
#pragma once

#include "obs-preview.hpp"
#include "typeset-source.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace typeset {

// A short piece of text drawn as an image (a composition in progress, the
// placeholder's words): made on OBS's UI thread, where laying
// text out can't hold up a frame, and only uploaded on the graphics thread.
struct LabelImage {
	std::string key; // what it's of: text, size and colours
	int width = 0, height = 0;
	std::vector<uint8_t> pixels; // BGRA, premultiplied, packed rows
};
using Label = std::shared_ptr<const LabelImage>;

// A label for this text, at px pixels high, white on bg (0xRRGGBBAA; clear:
// none). UI thread only. Kept and reused while it's asked for.
Label make_label(const std::string &text, double px, uint32_t fg, uint32_t bg);


struct Overlay {
	bool editing = false;
	ItemRef target;
	Mapping mapping;
	float view_w = 0, view_h = 0; // the preview, in display pixels
	Caret caret;
	bool show_caret = true;
	std::vector<Box> selection;
	Box box;
	bool plate = false; // a background plate behind the text
	uint32_t accent = 0x2F8FFFFF; // OBS's theme's highlight, 0xRRGGBBAA
	// Words, as images: a composition in progress, and the placeholders'.
	Label preedit, placeholder;
	uint64_t blink_from = 0;
	std::vector<ItemRef> placeholders; // empty texts
};

// A composition's label, at the preview's scale (o.mapping). UI thread.
void make_labels(Overlay &o, const std::string &preedit);
// The placeholders' words, at the preview's scale. UI thread.
Label placeholder_label(const Mapping &mapping);

// Draws an Overlay. OBS's graphics thread only (it keeps textures of its
// labels).
class OverlayPainter {
public:
	void paint(const Overlay &o, uint32_t cx, uint32_t cy);
	void destroy(); // with the graphics context entered

private:
	// A label's texture, made again only when the image it's for changes;
	// one that couldn't be made isn't tried again until it does.
	struct Texture {
		gs_texture_t *texture = nullptr;
		int width = 0, height = 0;
		std::string key, failed;
		bool upload(const Label &label);
		void draw(float x, float y) const;
		void destroy();
	};
	Texture placeholder_, preedit_;

};

} // namespace typeset
