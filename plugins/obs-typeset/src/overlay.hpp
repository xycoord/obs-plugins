// What the editor draws on OBS's preview (never into the output): the frame
// round the text being edited, its caret and selection, the format bar and
// tag above it, a veil over the rest of the canvas, and placeholders for empty
// texts. The editor describes it (an Overlay); the painter draws that on OBS's
// graphics thread, from the scene items' transforms as they are at that
// moment, so it follows the text wherever it's moved from.
#pragma once

#include "obs-preview.hpp"
#include "typeset-source.hpp"

#include <cstdint>
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
	std::string tag;    // what the tag says
	BarState bar;
	std::string preedit; // an input method's composition
	uint64_t blink_from = 0;
	std::vector<ItemRef> placeholders; // empty texts
};

// Draws an Overlay. OBS's graphics thread only (it keeps textures of its
// labels).
class OverlayPainter {
public:
	// Draws it, and says where the format bar's buttons went (for clicks).
	void paint(const Overlay &o, uint32_t cx, uint32_t cy, std::vector<BarButton> *bar);
	void destroy(); // with the graphics context entered

private:
	struct Label {
		gs_texture_t *texture = nullptr;
		int width = 0, height = 0;
		std::string text;
		void make(const std::string &text, double px, uint32_t fg, uint32_t bg);
		void ensure(const std::string &text, double px, uint32_t fg, uint32_t bg)
		{
			if (!texture || text != this->text)
				make(text, px, fg, bg);
		}
		void draw(float x, float y) const;
		void destroy();
	};
	Label tag_, placeholder_, preedit_, size_;
	Label glyphs_[int(BarAction::Count)];

	void paint_bar(const Overlay &o, float x, float y, float px, std::vector<BarButton> *bar);
};

} // namespace typeset
