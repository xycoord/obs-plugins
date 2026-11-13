// What the editor draws on OBS's preview (never into the output): the frame
// round the text being edited, its caret and selection, the format bar and
// tag above it, a veil over the rest of the canvas, and placeholders for empty
// texts. The editor describes it (an Overlay); the painter draws that on OBS's
// graphics thread, from the scene items' transforms as they are at that
// moment, so it follows the text wherever it's moved from.
#pragma once

#include "obs-preview.hpp"
#include "typeset-source.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace typeset {

// The format bar's buttons, left to right.
enum class BarAction { Bold, Italic, Underline, Smaller, SizeReadout, Bigger, Colour, Align, Clear, Count };

// What the bar shows: what's on for the selection, or for what's typed next.
struct BarState {
	bool bold = false, italic = false, underline = false;
	int size_px = 0;     // the size readout, in the text's own pixels
	uint32_t colour = 0; // the colour button's bar, 0xAABBGGRR
	int align = 0;       // the whole text's alignment (Align)
};

// A button as drawn, in display pixels.
struct BarButton {
	BarAction action = BarAction::Bold;
	float x = 0, y = 0, w = 0, h = 0;
	bool contains(float px, float py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
};

// A short piece of text drawn as an image (the tag, the bar's glyphs and
// size, the placeholder's words): made on OBS's UI thread, where laying
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
	bool grip = false;  // the text box's width grip
	bool plate = false; // a background plate behind the text
	BarState bar;
	// Its words, as images: the tag, the bar's glyphs and size readout, a
	// composition in progress, and the placeholders' words.
	Label tag, size, preedit, placeholder;
	std::array<Label, size_t(BarAction::Count)> glyphs;
	uint64_t blink_from = 0;
	std::vector<ItemRef> placeholders; // empty texts
};

// The overlay's labels, for its tag and composition (and its bar's glyphs
// and size, from o.bar), at the preview's scale (o.mapping). UI thread.
void make_labels(Overlay &o, const std::string &tag, const std::string &preedit);
// The placeholders' words, at the preview's scale. UI thread.
Label placeholder_label(const Mapping &mapping);

// Draws an Overlay. OBS's graphics thread only (it keeps textures of its
// labels).
class OverlayPainter {
public:
	// Draws it, and says where the format bar's buttons went (for clicks).
	void paint(const Overlay &o, uint32_t cx, uint32_t cy, std::vector<BarButton> *bar);
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
	Texture tag_, placeholder_, preedit_, size_;
	Texture glyphs_[int(BarAction::Count)];

	void paint_bar(const Overlay &o, float x, float y, float px, std::vector<BarButton> *bar);
};

} // namespace typeset
