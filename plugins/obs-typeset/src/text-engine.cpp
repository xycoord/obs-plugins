#include "text-engine.hpp"

#include <obs.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace typeset {

PangoContext *create_context(PangoFontMap *map)
{
	PangoContext *context = pango_font_map_create_context(map);

	// No hinting and no rounding: glyphs keep the same positions and shapes
	// relative to each other at every size, which is what lets the text
	// scale like a vector drawing. Grey antialiasing, as for any texture
	// that is composited and scaled (subpixel colour fringes wouldn't be).
	cairo_font_options_t *options = cairo_font_options_create();
	cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
	cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
	cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
	pango_cairo_context_set_font_options(context, options);
	cairo_font_options_destroy(options);
	pango_context_set_round_glyph_positions(context, FALSE);
	return context;
}

// The font picker names a face's style the way the font does ("SemiBold
// Italic", "Extra Bold", "Condensed Light"). Pango reads most of those words;
// two-word weights it reads only written as one.
static std::string pango_style_words(const std::string &style)
{
	static const char *const prefixes[] = {"extra ", "ultra ", "semi ", "demi "};
	std::string out = style;
	for (const char *prefix : prefixes) {
		const size_t n = strlen(prefix);
		for (size_t i = 0; i + n <= out.size(); i++) {
			if (strncasecmp(out.c_str() + i, prefix, n) == 0 && (i == 0 || out[i - 1] == ' '))
				out.erase(i + n - 1, 1);
		}
	}
	return out;
}

static PangoFontDescription *font_description(const Style &style)
{
	// "Family, Style" reads the style words; the family is then set as
	// given, so words in a family's name are never taken for style.
	const std::string spec = "Sans, " + pango_style_words(style.face_style);
	PangoFontDescription *desc = pango_font_description_from_string(spec.c_str());
	pango_font_description_set_family(desc, style.family.empty() ? "Sans" : style.family.c_str());

	if ((style.font_flags & OBS_FONT_BOLD) && pango_font_description_get_weight(desc) < PANGO_WEIGHT_BOLD)
		pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
	if ((style.font_flags & OBS_FONT_ITALIC) && pango_font_description_get_style(desc) == PANGO_STYLE_NORMAL)
		pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);

	const double size = std::clamp(style.size, 1.0, 4096.0);
	pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
	return desc;
}

PangoLayout *create_layout(PangoContext *context, const Document &doc)
{
	PangoLayout *layout = pango_layout_new(context);
	const Style &style = doc.style;

	PangoFontDescription *desc = font_description(style);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	switch (style.align) {
	case Align::Centre:
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
		break;
	case Align::Right:
		pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
		break;
	default:
		pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
	}
	if (style.line_spacing > 0.0 && std::fabs(style.line_spacing - 1.0) > 1e-6)
		pango_layout_set_line_spacing(layout, float(style.line_spacing));

	if (style.font_flags & (OBS_FONT_UNDERLINE | OBS_FONT_STRIKEOUT)) {
		PangoAttrList *attrs = pango_attr_list_new();
		if (style.font_flags & OBS_FONT_UNDERLINE)
			pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
		if (style.font_flags & OBS_FONT_STRIKEOUT)
			pango_attr_list_insert(attrs, pango_attr_strikethrough_new(TRUE));
		pango_layout_set_attributes(layout, attrs);
		pango_attr_list_unref(attrs);
	}

	pango_layout_set_text(layout, doc.text.c_str(), int(doc.text.size()));
	return layout;
}

static Box to_box(const PangoRectangle &r)
{
	return {double(r.x) / PANGO_SCALE, double(r.y) / PANGO_SCALE, double(r.width) / PANGO_SCALE,
		double(r.height) / PANGO_SCALE};
}

Box logical_box(PangoLayout *layout)
{
	PangoRectangle logical;
	pango_layout_get_extents(layout, nullptr, &logical);
	return to_box(logical);
}

Box drawn_box(PangoLayout *layout)
{
	PangoRectangle ink, logical;
	pango_layout_get_extents(layout, &ink, &logical);
	const Box a = to_box(ink), b = to_box(logical);
	if (a.w <= 0 || a.h <= 0)
		return b;
	const double x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
	const double x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
	return {x0, y0, x1 - x0, y1 - y0};
}

bool RasterParams::close_to(const RasterParams &o) const
{
	// Within these, a raster made for one is indistinguishable drawn for
	// the other: under a hundredth of a pixel across an 8192-pixel texture.
	auto near_scale = [](double a, double b) { return std::fabs(a - b) <= 1e-6 * std::max(a, b); };
	auto near_offset = [](double a, double b) { return std::fabs(a - b) <= 1.0 / 128.0; };
	return near_scale(sx, o.sx) && near_scale(sy, o.sy) && near_offset(ox, o.ox) && near_offset(oy, o.oy);
}

Raster rasterise(PangoLayout *layout, const Document &doc, RasterParams params)
{
	Raster out;
	const Box box = drawn_box(layout);

	// Too big a texture: shrink the scale to fit (the text blurs a little
	// only when it is drawn larger than this on the canvas).
	const double limit = max_raster_side - 4;
	if (box.w * params.sx > limit)
		params.sx = limit / box.w;
	if (box.h * params.sy > limit)
		params.sy = limit / box.h;
	out.params = params;

	// One pixel's margin round the drawn box for the antialiasing.
	const double px0 = box.x * params.sx + params.ox, py0 = box.y * params.sy + params.oy;
	const double px1 = (box.x + box.w) * params.sx + params.ox, py1 = (box.y + box.h) * params.sy + params.oy;
	out.x0 = int(std::floor(px0)) - 1;
	out.y0 = int(std::floor(py0)) - 1;
	out.width = std::max(int(std::ceil(px1)) + 1 - out.x0, 0);
	out.height = std::max(int(std::ceil(py1)) + 1 - out.y0, 0);
	if (out.width <= 2 || out.height <= 2 || doc.text.empty()) {
		out.width = out.height = 0;
		return out;
	}

	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, out.width, out.height);
	cairo_t *cr = cairo_create(surface);
	cairo_translate(cr, params.ox - out.x0, params.oy - out.y0);
	cairo_scale(cr, params.sx, params.sy);

	const uint32_t c = doc.style.colour;
	cairo_set_source_rgba(cr, double(c & 0xFF) / 255.0, double((c >> 8) & 0xFF) / 255.0,
			      double((c >> 16) & 0xFF) / 255.0, double((c >> 24) & 0xFF) / 255.0);
	pango_cairo_show_layout(cr, layout);
	cairo_destroy(cr);

	// Cairo's ARGB32 is premultiplied BGRA in memory on little-endian
	// machines: what OBS's GS_BGRA textures take.
	cairo_surface_flush(surface);
	const int stride = cairo_image_surface_get_stride(surface);
	const uint8_t *data = cairo_image_surface_get_data(surface);
	out.pixels.resize(size_t(out.width) * out.height * 4);
	for (int y = 0; y < out.height; y++)
		memcpy(&out.pixels[size_t(y) * out.width * 4], data + size_t(y) * stride, size_t(out.width) * 4);
	cairo_surface_destroy(surface);
	return out;
}

} // namespace typeset
