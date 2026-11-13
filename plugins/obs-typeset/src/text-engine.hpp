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

#include <pango/pangocairo.h>

#include <cstdint>
#include <string>
#include <vector>

namespace typeset {

enum class Align { Left = 0, Centre = 1, Right = 2 };
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

	// Effects.
	bool outline = false;
	uint32_t outline_colour = 0xFF000000;
	double outline_width = 4.0;
	bool shadow = false;
	uint32_t shadow_colour = 0x99000000;
	double shadow_x = 0.0, shadow_y = 4.0, shadow_blur = 8.0;
	bool plate = false;
	uint32_t plate_colour = 0xC0000000;
	double plate_padding = 24.0, plate_radius = 8.0;

	bool operator==(const Style &) const = default;
};

struct Document {
	std::string text; // UTF-8
	Style style;

	bool operator==(const Document &) const = default;
};

// A Pango context for resolution-independent layout (see above). Each thread
// that lays out text needs its own font map: Pango's aren't thread-safe.
PangoContext *create_context(PangoFontMap *map);
PangoLayout *create_layout(PangoContext *context, const Document &doc);

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
	int x0 = 0, y0 = 0;
	int width = 0, height = 0;
	std::vector<uint8_t> pixels; // BGRA, premultiplied, width*4 per row
};

// The largest a raster may be, per side and in all: above it the scale is
// reduced (the text then blurs a little, only when drawn that large).
constexpr int max_raster_side = 8192;
constexpr double max_raster_pixels = 32.0 * 1024 * 1024;

// Draws the document at the given scale. A raster can come out empty: no ink,
// or memory couldn't be had for it.
Raster rasterise(PangoLayout *layout, const Document &doc, RasterParams params);

// Whether a font family is installed, and the family Pango will use for it.
bool font_installed(PangoContext *context, const std::string &family, std::string *used);

} // namespace typeset
