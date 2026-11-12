// Text layout and rasterisation with Pango and Cairo.
//
// Layout happens once, in the source's own units (pixels at scale 1), with
// hinting off and glyph positions left unrounded, so it is exactly the same at
// every scale: scaling the source scales the text like a vector drawing. Only
// rasterisation depends on the scale, and it happens at the size the text is
// actually drawn at on the canvas.
#pragma once

#include <pango/pangocairo.h>

#include <cstdint>
#include <string>
#include <vector>

namespace typeset {

enum class Align { Left = 0, Centre = 1, Right = 2 };

struct Style {
	std::string family = "Sans";
	std::string face_style = "Regular"; // as the font picker names it: "SemiBold Italic"
	double size = 96.0;                 // pixels, in the source's own units
	uint32_t font_flags = 0;            // OBS_FONT_*
	uint32_t colour = 0xFFFFFFFF;       // 0xAABBGGRR, as OBS stores colours
	Align align = Align::Left;
	double line_spacing = 1.0; // a multiple of the font's own line height

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

// The layout's logical box (what its size is) and the union of that with its
// ink (everything it draws, which can reach outside: italic overhangs, accents).
Box logical_box(PangoLayout *layout);
Box drawn_box(PangoLayout *layout);

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
	int x0 = 0, y0 = 0;
	int width = 0, height = 0;
	std::vector<uint8_t> pixels; // BGRA, premultiplied, width*4 per row
};

// Largest texture side a raster may have; above it the scale is reduced.
constexpr int max_raster_side = 8192;

// Draws the layout at the given scale. A raster can come out empty (no ink).
Raster rasterise(PangoLayout *layout, const Document &doc, RasterParams params);

} // namespace typeset
