// Text layout and rasterisation with Pango and Cairo.
//
// Layout happens once, in the source's own units (pixels at scale 1), with
// hinting off and glyph positions left unrounded, so it is exactly the same at
// every scale: scaling the source scales the text like a vector drawing. Only
// rasterisation depends on the scale, and it happens at the size the text is
// actually drawn at on the canvas. Every length in a style (box, outline,
// shadow, padding) is in the same units as the font size, so the whole design
// scales together.
#pragma once

#include "rich-text.hpp"

#include <pango/pangocairo.h>

#include <cstdint>
#include <string>
#include <vector>

namespace typeset {

enum class Align { Left = 0, Centre = 1, Right = 2, Justified = 3 };
enum class VAlign { Top = 0, Middle = 1, Bottom = 2 };
enum class Overflow { Wrap = 0, Shrink = 1 };
enum class Case { AsTyped = 0, Upper = 1, Lower = 2, Title = 3 };

struct Style {
	std::string family = "Sans";
	std::string face_style = "Regular"; // as the font picker names it: "SemiBold Italic"
	double size = 96.0;                 // pixels, in the source's own units
	uint32_t font_flags = 0;            // OBS_FONT_*
	uint32_t colour = 0xFFFFFFFF;       // 0xAABBGGRR, as OBS stores colours
	Align align = Align::Left;

	// Text box: off, the source is the size of its text.
	bool box = false;
	double box_width = 0, box_height = 0; // height 0: as tall as the text
	Overflow overflow = Overflow::Wrap;
	VAlign valign = VAlign::Top;

	// Typography.
	double line_spacing = 1.0;   // a multiple of the font's own line height
	double letter_spacing = 0.0; // percent of the font size
	Case text_case = Case::AsTyped;
	bool tabular = false; // figures all the same width

	// Wrapping in a text box: lines made as even in length as they can be
	// (as CSS's text-wrap: balance), and a paragraph's last line kept from
	// being one word alone.
	bool balance = false;
	bool avoid_orphans = false;

	// Effects.
	bool outline = false;
	uint32_t outline_colour = 0xFF000000;
	double outline_width = 4.0;
	bool shadow = false;
	uint32_t shadow_colour = 0x99000000;
	double shadow_x = 0.0, shadow_y = 4.0, shadow_blur = 8.0;
	double shadow_spread = 0.0; // the shadow grown round the letters before it's blurred: a denser halo
	bool plate = false;
	uint32_t plate_colour = 0xC0000000;
	double plate_padding = 24.0, plate_radius = 8.0;

	bool operator==(const Style &) const = default;
};

struct Document {
	std::string text; // UTF-8
	Style style;
	Spans spans; // parts styled differently from the rest (see rich-text.hpp)

	bool operator==(const Document &) const = default;
};

// A Pango context for resolution-independent layout (see above). Each thread
// that lays out text needs its own font map: Pango's aren't thread-safe.
PangoContext *create_context(PangoFontMap *map);
PangoLayout *create_layout(PangoContext *context, const Document &doc);
PangoFontDescription *font_description(const Style &style); // the base font, for a style
int base_weight(const Style &style);                         // its weight (400 regular, 700 bold)

// The weights a family has faces for, upright (or italic), lowest first: for
// a variable font, its named instances. Empty if it isn't installed.
std::vector<int> family_weights(PangoContext *context, const std::string &family, bool italic);

// A weight's usual name ("Thin" to "Black"), for the nearest hundred.
const char *weight_name(int weight);

struct Box {
	double x = 0, y = 0, w = 0, h = 0;
};

// Where everything goes in the source, in its own units: its size, and the
// layout placed at (text_x, text_y) and drawn at text_scale (less than 1 when
// shrunk to fit its box). Source = layout * text_scale + text origin.
struct Frame {
	double width = 0, height = 0;
	double text_x = 0, text_y = 0, text_scale = 1;
	Box plate; // the background plate, when there is one
};

Frame frame_of(PangoLayout *layout, const Document &doc);

// Everything a document draws, in source units, including outline, shadow
// and plate (which can reach outside the source's own size).
Box drawn_box(PangoLayout *layout, const Document &doc, const Frame &frame);

// Where a raster's pixels sit: its top-left pixel's corner is at canvas pixel
// (x0, y0) relative to the canvas pixel grid the source was placed on, for a
// source drawn at scale (sx, sy) whose origin falls (ox, oy) of a pixel past a
// pixel corner.
struct RasterParams {
	double sx = 1, sy = 1;
	double ox = 0, oy = 0;

	bool close_to(const RasterParams &o) const;
};

struct Raster {
	RasterParams params;
	uint64_t version = 0; // of the document drawn (set by whoever asked for it)
	uint32_t source_width = 0, source_height = 0; // the source's size for that document (likewise)
	bool failed = false;  // couldn't be drawn (out of memory): try again later
	std::string error;    // why, when it failed
	int x0 = 0, y0 = 0;
	int width = 0, height = 0;
	// Its pixels, row after row with no gaps, in one of two forms:
	// - colour (bytes 4): BGRA, premultiplied;
	// - coverage (bytes 1 or 2): how much of each pixel the text covers,
	//   and with 2, how much its shadow does, to be drawn in fill_colour
	//   and shadow_colour (0xAABBGGRR), the text over its shadow. The colour
	//   form's picture (but for a part-covered row at a letter's edge, which
	//   Cairo draws a little differently into each) from a quarter or half
	//   the data, for text in one colour with no outline or plate.
	int bytes = 4;
	uint32_t fill_colour = 0, shadow_colour = 0;
	std::vector<uint8_t> pixels;
	// The parts with anything in them, in its pixels: runs of tiles along
	// each row of tiles, everything else clear. Only these are drawn. Empty
	// when it's all to be drawn (it's nearly all ink).
	struct Part {
		int x, y, w, h;
	};
	std::vector<Part> parts;
};

// Tiles of a raster with ink, merged along rows (Raster::parts).
void find_parts(Raster &r);

// The largest a raster may be, per side and in all: above it the scale is
// reduced (the text then blurs a little, only when drawn that large).
constexpr int max_raster_side = 8192;
constexpr double max_raster_pixels = 32.0 * 1024 * 1024;

// Draws the document at the given scale. A raster comes out empty when
// there's no ink, and failed when memory couldn't be had for it.
// coverage: in the coverage form where the text allows (see Raster).
Raster rasterise(PangoLayout *layout, const Document &doc, RasterParams params, bool coverage = false);

// Where Backspace at a byte index of a layout's text deletes back to: the
// whole grapheme before it, as Pango divides graphemes (an emoji sequence, a
// flag, a letter with its accent, a Hangul syllable), except where Pango says
// one character is deleted (Indic scripts: क + ि loses only the vowel sign).
size_t backspace_index(PangoLayout *layout, size_t index);

// The widest word (or other unbreakable run) of a text, as laid out: the
// narrowest its text box can be.
double widest_word(PangoContext *context, const Document &doc);

// Whether a font family is installed, and the family Pango will use for it.
bool font_installed(PangoContext *context, const std::string &family, std::string *used);

} // namespace typeset
