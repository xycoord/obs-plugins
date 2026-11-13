#include "text-engine.hpp"

#include <obs.h>

#include <algorithm>
#include <memory>
#include <cmath>
#include <cstring>
#include <new>
#include <strings.h>

#include <hb-ot.h>
#include <cairo-ft.h>
#include <cairo-pdf.h>

namespace typeset {

// Cairo (from 1.17.4) draws each glyph at its position rounded to a quarter
// pixel, but works out the area a run of glyphs covers from their positions
// rounded to whole pixels (as image surfaces ask for). The two can be half a
// pixel apart, and when Cairo draws the run through a mask of that area (it
// does whenever the glyphs' boxes overlap, which is nearly every run, unless
// it's adding to a blank A8 surface), a letter's last part-covered row or
// column at its edge falls outside it and is cut off: a hard edge where there
// should be antialiasing. Two things together make the area exactly what's
// drawn: font options that leave the positions unrounded (see create_context)
// and glyphs given to Cairo at the quarter pixel it would draw them at
// anyway (see show_layout), so the glyphs themselves are drawn as before.
// Older Cairo rounds glyphs to whole pixels for both, and is left as it is.
static bool quarter_pixel_glyphs()
{
#if CAIRO_HAS_PDF_SURFACE
	static const bool yes = cairo_version() >= CAIRO_VERSION_ENCODE(1, 17, 4);
	return yes;
#else
	return false; // no way to the unrounded options (below)
#endif
}

PangoContext *create_context(PangoFontMap *map)
{
	PangoContext *context = pango_font_map_create_context(map);

	// No hinting and no rounding: glyphs keep the same positions and shapes
	// relative to each other at every size, which is what lets the text
	// scale like a vector drawing. Grey antialiasing, as for any texture
	// that is composited and scaled (subpixel colour fringes wouldn't be).
	cairo_font_options_t *options = cairo_font_options_create();
#if CAIRO_HAS_PDF_SURFACE
	if (quarter_pixel_glyphs()) {
		// Cairo's only public way to font options that leave glyph
		// positions unrounded: a PDF surface's (one with no output, never
		// drawn on). They're otherwise the ones set below.
		cairo_surface_t *pdf = cairo_pdf_surface_create(nullptr, 1, 1);
		cairo_surface_get_font_options(pdf, options);
		cairo_surface_destroy(pdf);
	}
#endif
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

PangoFontDescription *font_description(const Style &style)
{
	// Each word of the style name is read on its own, so one Pango doesn't
	// know ("Display", "Text") doesn't lose the others ("Bold").
	PangoFontDescription *desc = pango_font_description_new();
	const std::string words = pango_style_words(style.face_style);
	size_t i = 0;
	while (i < words.size()) {
		const size_t end = std::min(words.find(' ', i), words.size());
		const std::string word = words.substr(i, end - i);
		i = end + 1;
		if (word.empty())
			continue;
		// Only what the word says: parsing fills in "normal" for the rest,
		// which mustn't undo an earlier word ("Bold Italic").
		PangoFontDescription *w = pango_font_description_from_string(("Sans, " + word).c_str());
		if (pango_font_description_get_weight(w) != PANGO_WEIGHT_NORMAL)
			pango_font_description_set_weight(desc, pango_font_description_get_weight(w));
		if (pango_font_description_get_style(w) != PANGO_STYLE_NORMAL)
			pango_font_description_set_style(desc, pango_font_description_get_style(w));
		if (pango_font_description_get_stretch(w) != PANGO_STRETCH_NORMAL)
			pango_font_description_set_stretch(desc, pango_font_description_get_stretch(w));
		if (pango_font_description_get_variant(w) != PANGO_VARIANT_NORMAL)
			pango_font_description_set_variant(desc, pango_font_description_get_variant(w));
		pango_font_description_free(w);
	}
	pango_font_description_set_family(desc, style.family.empty() ? "Sans" : style.family.c_str());

	// OBS's font picker also sets a bold flag for SemiBold and up, and an
	// italic one: they only count when the style name gave no weight or
	// slant of its own.
	if ((style.font_flags & OBS_FONT_BOLD) && pango_font_description_get_weight(desc) == PANGO_WEIGHT_NORMAL)
		pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
	if ((style.font_flags & OBS_FONT_ITALIC) && pango_font_description_get_style(desc) == PANGO_STYLE_NORMAL)
		pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);

	const double size = std::clamp(style.size, 1.0, 4096.0);
	pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
	return desc;
}

int base_weight(const Style &style)
{
	PangoFontDescription *desc = font_description(style);
	const int w = pango_font_description_get_weight(desc);
	pango_font_description_free(desc);
	return w;
}

std::vector<int> family_weights(PangoContext *context, const std::string &family, bool italic)
{
	std::vector<int> out;
	PangoFontMap *map = pango_context_get_font_map(context);
	PangoFontFamily **families = nullptr;
	int n = 0;
	pango_font_map_list_families(map, &families, &n);
	for (int i = 0; i < n; i++) {
		if (g_ascii_strcasecmp(pango_font_family_get_name(families[i]), family.c_str()) != 0)
			continue;
		PangoFontFace **faces = nullptr;
		int m = 0;
		pango_font_family_list_faces(families[i], &faces, &m);
		for (int j = 0; j < m; j++) {
			PangoFontDescription *d = pango_font_face_describe(faces[j]);
			const bool slanted = pango_font_description_get_style(d) != PANGO_STYLE_NORMAL;
			// Condensed and other widths are other designs, not weights of this one.
			if (slanted == italic && pango_font_description_get_stretch(d) == PANGO_STRETCH_NORMAL)
				out.push_back(pango_font_description_get_weight(d));
			pango_font_description_free(d);
		}
		g_free(faces);
		break;
	}
	g_free(families);
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

const char *weight_name(int weight)
{
	static const char *const names[] = {"Thin",     "ExtraLight", "Light",     "Regular", "Medium",
					    "SemiBold", "Bold",       "ExtraBold", "Black"};
	const int i = std::clamp(int(std::lround(weight / 100.0)) - 1, 0, 8);
	return names[i];
}

static bool wraps(const Style &s)
{
	return s.box && s.box_width > 0 && s.overflow == Overflow::Wrap;
}

// Whether any paragraph of more than one line ends in a line of one word.
static bool has_orphan(PangoLayout *layout, const std::string &text)
{
	const int n = pango_layout_get_line_count(layout);
	for (int i = 1; i < n; i++) {
		PangoLayoutLine *line = pango_layout_get_line_readonly(layout, i);
		PangoLayoutLine *next = i + 1 < n ? pango_layout_get_line_readonly(layout, i + 1) : nullptr;
		const bool last_of_paragraph = !next || next->is_paragraph_start;
		if (!last_of_paragraph || line->is_paragraph_start)
			continue;
		std::string words = text.substr(size_t(line->start_index), size_t(line->length));
		while (!words.empty() && (words.back() == ' ' || words.back() == '\n'))
			words.pop_back();
		while (!words.empty() && words.front() == ' ')
			words.erase(words.begin());
		if (!words.empty() && words.find(' ') == std::string::npos)
			return true;
	}
	return false;
}

// The wrapping width narrowed within the box, keeping the number of lines:
// as narrow as it goes (balanced lines), and/or until no paragraph ends in
// one word alone. frame_of() places the narrower block in the box as the
// alignment says. A few layouts of a title's few words: well under a
// millisecond.
// The widest word's width, in Pango units, laid out as the layout is.
static int widest_word_units(PangoLayout *layout)
{
	const int width = pango_layout_get_width(layout);
	pango_layout_set_width(layout, 1); // every word a line of its own
	PangoRectangle logical;
	pango_layout_get_extents(layout, nullptr, &logical);
	pango_layout_set_width(layout, width);
	return logical.width;
}

static void tune_wrap(PangoLayout *layout, const Document &doc)
{
	const Style &s = doc.style;
	const int full = int(std::lround(s.box_width * PANGO_SCALE));
	auto lines_at = [&](int w) {
		pango_layout_set_width(layout, w);
		return pango_layout_get_line_count(layout);
	};
	const int n = lines_at(full);
	if (n < 2)
		return;
	int width = full;
	if (s.balance) {
		// The narrowest width with no more lines: lines as even as they go.
		// (Never narrower than the widest word: it would stick out.)
		int lo = std::max(full / (n + 1), widest_word_units(layout)), hi = full;
		for (int i = 0; i < 14 && hi - lo > PANGO_SCALE / 2; i++) {
			const int mid = lo + (hi - lo) / 2;
			if (lines_at(mid) <= n)
				hi = mid;
			else
				lo = mid;
		}
		width = hi;
	}
	if (s.avoid_orphans) {
		lines_at(width);
		if (has_orphan(layout, doc.text)) {
			// Narrower a little at a time, until a word comes down to join
			// the last one, the lines no more than before.
			const int floor = std::max(full / 2, widest_word_units(layout));
			for (int w = width - full / 50; w >= floor; w -= full / 50) {
				if (lines_at(w) > n)
					break;
				if (!has_orphan(layout, doc.text)) {
					width = w;
					break;
				}
			}
		}
	}
	pango_layout_set_width(layout, width);
}

PangoLayout *create_layout(PangoContext *context, const Document &doc)
{
	PangoLayout *layout = pango_layout_new(context);
	const Style &style = doc.style;

	PangoFontDescription *desc = font_description(style);
	pango_layout_set_font_description(layout, desc);
	const int base = pango_font_description_get_weight(desc);
	pango_font_description_free(desc);

	switch (style.align) {
	case Align::Centre:
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
		break;
	case Align::Right:
		pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
		break;
	case Align::Justified: // each wrapped line to the box's full width (not a paragraph's last)
		pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
		pango_layout_set_justify(layout, TRUE);
		break;
	default:
		pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
	}
	if (style.line_spacing > 0.0 && std::fabs(style.line_spacing - 1.0) > 1e-6)
		pango_layout_set_line_spacing(layout, float(style.line_spacing));
	if (wraps(style)) {
		pango_layout_set_width(layout, int(std::lround(style.box_width * PANGO_SCALE)));
		// At spaces only: a word too wide for the box stays whole (and the
		// box's handles stop at the widest: see widest_word()), never broken
		// or hyphenated, as display type is set.
		pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
	}

	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_insert_hyphens_new(FALSE));
	if (style.font_flags & OBS_FONT_UNDERLINE)
		pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
	if (style.font_flags & OBS_FONT_STRIKEOUT)
		pango_attr_list_insert(attrs, pango_attr_strikethrough_new(TRUE));
	if (std::fabs(style.letter_spacing) > 1e-6)
		pango_attr_list_insert(attrs, pango_attr_letter_spacing_new(int(std::lround(
						      style.size * style.letter_spacing / 100.0 * PANGO_SCALE))));
	// The case is how the text is drawn, not what it is: the text stays as
	// typed, and editing works on it as typed.
	switch (style.text_case) {
	case Case::Upper:
		pango_attr_list_insert(attrs, pango_attr_text_transform_new(PANGO_TEXT_TRANSFORM_UPPERCASE));
		break;
	case Case::Lower:
		pango_attr_list_insert(attrs, pango_attr_text_transform_new(PANGO_TEXT_TRANSFORM_LOWERCASE));
		break;
	case Case::Title:
		pango_attr_list_insert(attrs, pango_attr_text_transform_new(PANGO_TEXT_TRANSFORM_CAPITALIZE));
		break;
	default:
		break;
	}
	if (style.tabular)
		pango_attr_list_insert(attrs, pango_attr_font_features_new("tnum 1"));

	// Styled runs, over the base.
	auto add = [&](PangoAttribute *a, const Span &sp) {
		a->start_index = guint(sp.start);
		a->end_index = guint(sp.end);
		pango_attr_list_insert(attrs, a);
	};
	for (const Span &sp : doc.spans) {
		if (sp.start >= sp.end || sp.end > doc.text.size())
			continue;
		if (!sp.family.empty())
			add(pango_attr_family_new(sp.family.c_str()), sp);
		if (const int w = effective_weight(sp, base); w != base)
			add(pango_attr_weight_new(PangoWeight(w)), sp);
		if (sp.italic > 0)
			add(pango_attr_style_new(PANGO_STYLE_ITALIC), sp);
		else if (sp.italic == 0)
			add(pango_attr_style_new(PANGO_STYLE_NORMAL), sp);
		if (sp.underline > 0)
			add(pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), sp);
		else if (sp.underline == 0)
			add(pango_attr_underline_new(PANGO_UNDERLINE_NONE), sp);
		if (sp.has_colour) {
			const uint32_t c = sp.colour;
			add(pango_attr_foreground_new(guint16((c & 0xFF) * 257), guint16(((c >> 8) & 0xFF) * 257),
						      guint16(((c >> 16) & 0xFF) * 257)),
			    sp);
			add(pango_attr_foreground_alpha_new(guint16(((c >> 24) & 0xFF) * 257)), sp);
		}
		if (sp.size > 0)
			add(pango_attr_size_new_absolute(int(std::lround(std::clamp(style.size, 1.0, 4096.0) * sp.size /
									    100.0 * PANGO_SCALE))),
			    sp);
		if (sp.has_tracking) // a percent of the run's own size
			add(pango_attr_letter_spacing_new(int(std::lround(std::clamp(style.size, 1.0, 4096.0) *
									   (sp.size > 0 ? sp.size / 100.0 : 1.0) *
									   sp.tracking / 100.0 * PANGO_SCALE))),
			    sp);
		// Raised or lowered, and made smaller, as the font itself says
		// (its superscript metrics), so it sits right in any font.
		if (sp.script == 1 || sp.script == 2) {
			const bool super = sp.script == 1;
			add(pango_attr_baseline_shift_new(super ? PANGO_BASELINE_SHIFT_SUPERSCRIPT
								: PANGO_BASELINE_SHIFT_SUBSCRIPT),
			    sp);
			add(pango_attr_font_scale_new(super ? PANGO_FONT_SCALE_SUPERSCRIPT : PANGO_FONT_SCALE_SUBSCRIPT), sp);
		}
	}
	pango_layout_set_attributes(layout, attrs);
	pango_attr_list_unref(attrs);

	pango_layout_set_text(layout, doc.text.c_str(), int(doc.text.size()));
	if (wraps(style) && (style.balance || style.avoid_orphans))
		tune_wrap(layout, doc);
	return layout;
}

static Box to_box(const PangoRectangle &r)
{
	return {double(r.x) / PANGO_SCALE, double(r.y) / PANGO_SCALE, double(r.width) / PANGO_SCALE,
		double(r.height) / PANGO_SCALE};
}

static Box unite(const Box &a, const Box &b)
{
	if (a.w <= 0 || a.h <= 0)
		return b;
	if (b.w <= 0 || b.h <= 0)
		return a;
	const double x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
	const double x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
	return {x0, y0, x1 - x0, y1 - y0};
}

static Box grow(const Box &b, double by)
{
	return {b.x - by, b.y - by, b.w + 2 * by, b.h + 2 * by};
}

static double fraction(Align a)
{
	return a == Align::Centre ? 0.5 : a == Align::Right ? 1.0 : 0.0;
}

static double fraction(VAlign a)
{
	return a == VAlign::Middle ? 0.5 : a == VAlign::Bottom ? 1.0 : 0.0;
}

Frame frame_of(PangoLayout *layout, const Document &doc)
{
	const Style &s = doc.style;
	PangoRectangle logical_r;
	pango_layout_get_extents(layout, nullptr, &logical_r);
	const Box logical = to_box(logical_r);

	Frame f;
	const bool fixed_w = s.box && s.box_width > 0;
	const bool fixed_h = s.box && s.box_height > 0;
	if (fixed_w && s.overflow == Overflow::Shrink) {
		if (logical.w > s.box_width)
			f.text_scale = s.box_width / logical.w;
		if (fixed_h && logical.h * f.text_scale > s.box_height)
			f.text_scale = s.box_height / logical.h;
	}
	const double block_w = logical.w * f.text_scale, block_h = logical.h * f.text_scale;

	// An empty text keeps a little width, so it can still be found. A box
	// is never narrower than its text: a word too wide for it (it's never
	// broken) widens it, so OBS's box goes round everything shown.
	double inner_w = fixed_w ? std::max(s.box_width, block_w) : block_w;
	if (!fixed_w && doc.text.empty())
		inner_w = std::max(inner_w, s.size * 0.3);
	const double inner_h = fixed_h ? s.box_height : block_h;

	// Wrapped, Pango places each line within its wrapping width itself; that
	// width is the box's, or narrower (balanced lines), placed in the box as
	// the alignment says. A line wider than it (a word too wide) Pango puts
	// partly before 0; the box widened for it, which brings it back inside.
	if (wraps(s))
		f.text_x = (inner_w - double(pango_layout_get_width(layout)) / PANGO_SCALE) * fraction(s.align);
	else
		f.text_x = (inner_w - block_w) * fraction(s.align) - logical.x * f.text_scale;
	f.text_y = (fixed_h ? (inner_h - block_h) * fraction(s.valign) : 0.0) - logical.y * f.text_scale;

	const double pad = s.plate ? std::max(0.0, s.plate_padding) : 0.0;
	f.text_x += pad;
	f.text_y += pad;
	f.width = inner_w + 2 * pad;
	f.height = inner_h + 2 * pad;
	if (s.plate)
		f.plate = {0, 0, f.width, f.height};
	return f;
}

Box drawn_box(PangoLayout *layout, const Document &doc, const Frame &f)
{
	const Style &s = doc.style;
	PangoRectangle ink_r, logical_r;
	pango_layout_get_extents(layout, &ink_r, &logical_r);
	Box text = unite(to_box(ink_r), to_box(logical_r));
	text = {f.text_x + text.x * f.text_scale, f.text_y + text.y * f.text_scale, text.w * f.text_scale,
		text.h * f.text_scale};
	if (s.outline)
		text = grow(text, std::max(0.0, s.outline_width));

	Box out = unite(text, f.plate);
	if (s.shadow) {
		Box shadow = grow(text, std::max(0.0, s.shadow_blur) * 1.5 + std::max(0.0, s.shadow_spread));
		shadow.x += s.shadow_x;
		shadow.y += s.shadow_y;
		out = unite(out, shadow);
	}
	return out;
}

bool RasterParams::close_to(const RasterParams &o) const
{
	// Within these, a raster made for one is indistinguishable drawn for
	// the other: under a hundredth of a pixel across an 8192-pixel texture.
	auto near_scale = [](double a, double b) { return std::fabs(a - b) <= 1e-6 * std::max(a, b); };
	auto near_offset = [](double a, double b) { return std::fabs(a - b) <= 1.0 / 128.0; };
	return near_scale(sx, o.sx) && near_scale(sy, o.sy) && near_offset(ox, o.ox) && near_offset(oy, o.oy);
}

static void set_colour(cairo_t *cr, uint32_t c)
{
	cairo_set_source_rgba(cr, double(c & 0xFF) / 255.0, double((c >> 8) & 0xFF) / 255.0,
			      double((c >> 16) & 0xFF) / 255.0, double((c >> 24) & 0xFF) / 255.0);
}

static void rounded_rect(cairo_t *cr, const Box &b, double r)
{
	r = std::clamp(r, 0.0, std::min(b.w, b.h) / 2);
	if (r <= 0) {
		cairo_rectangle(cr, b.x, b.y, b.w, b.h);
		return;
	}
	cairo_new_sub_path(cr);
	cairo_arc(cr, b.x + b.w - r, b.y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, b.x + b.w - r, b.y + b.h - r, r, 0, M_PI / 2);
	cairo_arc(cr, b.x + r, b.y + b.h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, b.x + r, b.y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

// Whether a font has colour glyphs (emoji): layers, bitmaps, SVG or paint.
static bool colour_font(PangoFont *font)
{
	hb_font_t *hb = font ? pango_font_get_hb_font(font) : nullptr;
	hb_face_t *face = hb ? hb_font_get_face(hb) : nullptr;
	return face && (hb_ot_color_has_layers(face) || hb_ot_color_has_png(face) || hb_ot_color_has_svg(face)
#if HB_VERSION_ATLEAST(7, 0, 0)
			|| hb_ot_color_has_paint(face)
#endif
		       );
}

// show_layout()'s renderer: Pango's own Cairo renderer's drawing (in
// pangocairo-render.c), but with each glyph given to Cairo at the quarter
// pixel Cairo draws it at (see quarter_pixel_glyphs). Pango works out the
// lines, runs, colours and decorations; this only draws them.
namespace {
struct Renderer {
	PangoRenderer parent;
	cairo_t *cr;
	double x0, y0; // where the layout's origin is, in user space
	bool snap;
	bool blank; // nothing is drawn on the target yet
};
struct RendererClass {
	PangoRendererClass parent;
};
} // namespace

// A part's colour as Pango's renderer sets it: the run's colour and/or
// alpha, the rest from the current source.
static void set_part_colour(Renderer *r, PangoRenderPart part)
{
	PangoRenderer *renderer = &r->parent;
	const PangoColor *colour = pango_renderer_get_color(renderer, part);
	const guint16 a = pango_renderer_get_alpha(renderer, part);
	if (!colour && !a)
		return;
	double red = 0, green = 0, blue = 0, alpha = 1;
	if (colour) {
		red = colour->red / 65535.0;
		green = colour->green / 65535.0;
		blue = colour->blue / 65535.0;
	} else if (cairo_pattern_t *p = cairo_get_source(r->cr); p && cairo_pattern_get_type(p) == CAIRO_PATTERN_TYPE_SOLID) {
		cairo_pattern_get_rgba(p, &red, &green, &blue, &alpha);
	}
	if (a)
		alpha = a / 65535.0;
	cairo_set_source_rgba(r->cr, red, green, blue, alpha);
}

// Whether any of n bytes is set, eight at a time.
static bool any_set(const uint8_t *p, size_t n)
{
	uint64_t acc = 0;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t v;
		memcpy(&v, p + i, 8);
		acc |= v;
	}
	for (; i < n; i++)
		acc |= p[i];
	return acc != 0;
}

// Where a run of glyphs reaches on its image surface, in pixels, and a
// pixel more: from the glyphs' metrics, or (outlines) from their outlines,
// which go further than a face's metrics when Cairo emboldens it itself.
namespace {
struct Area {
	int left = 0, top = 0, right = 0, bottom = 0;
	bool empty() const { return right <= left || bottom <= top; }
};
} // namespace

static Area area_of(cairo_t *cr, const std::vector<cairo_glyph_t> &glyphs, bool outlines)
{
	if (glyphs.empty())
		return {};
	double x0, y0, x1, y1;
	if (outlines) {
		cairo_new_path(cr);
		cairo_glyph_path(cr, glyphs.data(), int(glyphs.size()));
		cairo_path_extents(cr, &x0, &y0, &x1, &y1);
		cairo_new_path(cr);
	} else {
		cairo_text_extents_t e; // measured from the first glyph
		cairo_glyph_extents(cr, glyphs.data(), int(glyphs.size()), &e);
		x0 = glyphs[0].x + e.x_bearing, y0 = glyphs[0].y + e.y_bearing;
		x1 = x0 + e.width, y1 = y0 + e.height;
	}
	if (x1 <= x0 || y1 <= y0)
		return {}; // no ink
	double xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
	x0 = y0 = HUGE_VAL;
	x1 = y1 = -HUGE_VAL;
	for (int i = 0; i < 4; i++) {
		cairo_user_to_device(cr, &xs[i], &ys[i]);
		x0 = std::min(x0, xs[i]), y0 = std::min(y0, ys[i]), x1 = std::max(x1, xs[i]), y1 = std::max(y1, ys[i]);
	}
	cairo_surface_t *target = cairo_get_target(cr);
	return {std::max(0, int(std::floor(x0)) - 1), std::max(0, int(std::floor(y0)) - 1),
		std::min(cairo_image_surface_get_width(target), int(std::ceil(x1)) + 1),
		std::min(cairo_image_surface_get_height(target), int(std::ceil(y1)) + 1)};
}

// Whether every pixel of an area of an image surface is clear.
static bool clear(cairo_surface_t *target, const Area &a)
{
	if (a.empty())
		return true;
	cairo_surface_flush(target);
	const uint8_t *data = cairo_image_surface_get_data(target);
	const int stride = cairo_image_surface_get_stride(target);
	for (int row = a.top; row < a.bottom; row++)
		if (any_set(data + size_t(row) * size_t(stride) + a.left, size_t(a.right - a.left)))
			return false;
	return true;
}

// Whether glyphs drawn over an A8 surface (the coverage form, a shadow) in
// an opaque colour can be added in instead: the same pixels wherever the
// pixels under them are still clear. Cairo draws a run over through a mask
// of the glyphs added together, which into A8 takes pixman's slow general
// path; added straight in, the glyphs take a fast one. Cairo adds them
// itself only into a surface it made and hasn't drawn on: the first run, of
// a raster drawn in its own memory, and no other.
static bool can_add(cairo_t *cr)
{
	cairo_surface_t *target = cairo_get_target(cr);
	double red, green, blue, alpha;
	return cairo_get_operator(cr) == CAIRO_OPERATOR_OVER && cairo_surface_get_type(target) == CAIRO_SURFACE_TYPE_IMAGE &&
	       cairo_image_surface_get_format(target) == CAIRO_FORMAT_A8 &&
	       cairo_pattern_get_rgba(cairo_get_source(cr), &red, &green, &blue, &alpha) == CAIRO_STATUS_SUCCESS &&
	       alpha >= 1.0;
}

// Whether Cairo emboldens a font's letters itself (a family with no bold
// face): its glyph metrics then fall short of their outlines, by up to a
// pixel and more at 140 px, and Cairo's mask for a run, cut to the metrics,
// cuts the letters' edges with it.
static bool emboldened(cairo_scaled_font_t *scaled)
{
	cairo_font_face_t *face = cairo_scaled_font_get_font_face(scaled);
	return cairo_font_face_get_type(face) == CAIRO_FONT_TYPE_FT &&
	       (cairo_ft_font_face_get_synthesize(face) & CAIRO_FT_SYNTHESIZE_BOLD);
}

// Draws a run as Cairo draws one, in the current source over the target
// through a mask of its glyphs added together, but with the mask the size
// of the glyphs' outlines, so nothing is cut. False if it can't (not into
// an image surface, or no memory): the run is then Cairo's to draw.
static bool show_through_mask(cairo_t *cr, const std::vector<cairo_glyph_t> &glyphs, bool blank)
{
	cairo_surface_t *target = cairo_get_target(cr);
	double sx = 1, sy = 1, dx = 0, dy = 0;
	if (cairo_surface_get_type(target) != CAIRO_SURFACE_TYPE_IMAGE ||
	    cairo_pattern_get_type(cairo_get_source(cr)) != CAIRO_PATTERN_TYPE_SOLID)
		return false;
	cairo_surface_get_device_scale(target, &sx, &sy);
	if (sx != 1 || sy != 1)
		return false;
	const Area a = area_of(cr, glyphs, true);
	if (a.empty())
		return true;
	cairo_surface_get_device_offset(target, &dx, &dy);
	cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, a.right - a.left, a.bottom - a.top);
	cairo_t *m = cairo_create(mask);
	cairo_matrix_t ctm;
	cairo_get_matrix(cr, &ctm);
	ctm.x0 += dx - a.left;
	ctm.y0 += dy - a.top;
	cairo_set_matrix(m, &ctm);
	cairo_set_scaled_font(m, cairo_get_scaled_font(cr));
	cairo_set_operator(m, CAIRO_OPERATOR_ADD); // every glyph whole, with no mask of Cairo's own
	cairo_set_source_rgba(m, 0, 0, 0, 1);
	cairo_show_glyphs(m, glyphs.data(), int(glyphs.size()));
	const bool ok = cairo_status(m) == CAIRO_STATUS_SUCCESS;
	cairo_destroy(m);
	if (ok) {
		cairo_save(cr);
		if (can_add(cr) && (blank || clear(target, a)))
			cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
		cairo_identity_matrix(cr);
		cairo_mask_surface(cr, mask, a.left - dx, a.top - dy);
		cairo_restore(cr);
	}
	cairo_surface_destroy(mask);
	return ok;
}

static void render_glyphs(PangoRenderer *renderer, PangoFont *font, PangoGlyphString *glyphs, int x, int y)
{
	Renderer *r = reinterpret_cast<Renderer *>(renderer);
	cairo_t *cr = r->cr;
	const double base_x = r->x0 + double(x) / PANGO_SCALE, base_y = r->y0 + double(y) / PANGO_SCALE;
	cairo_save(cr);
	set_part_colour(r, PANGO_RENDER_PART_FOREGROUND);
	// Pango draws what a font can't (boxes for a missing letter, or for any
	// letter of a font that won't load): those are left to it.
	auto pango_draws = [&](PangoGlyphInfo *glyph, int n, double gx, double gy) {
		PangoGlyphString string{n, glyph, nullptr, 0};
		cairo_move_to(cr, gx, gy);
		pango_cairo_show_glyph_string(cr, font, &string);
		r->blank = false;
	};
	cairo_scaled_font_t *scaled = font && PANGO_IS_CAIRO_FONT(font)
					      ? pango_cairo_font_get_scaled_font(PANGO_CAIRO_FONT(font))
					      : nullptr;
	if (!scaled || cairo_scaled_font_status(scaled) != CAIRO_STATUS_SUCCESS) {
		pango_draws(glyphs->glyphs, glyphs->num_glyphs, base_x, base_y);
		cairo_restore(cr);
		return;
	}
	cairo_set_scaled_font(cr, scaled);
	// A colour font's glyphs Cairo draws one at a time, each at a whole
	// pixel and with no mask: they're never cut, and are left where they
	// are, and drawn as they always were.
	const bool colour = colour_font(font);
	const bool snap = r->snap && !colour;

	std::vector<cairo_glyph_t> out;
	out.reserve(size_t(glyphs->num_glyphs));
	int advance = 0;
	for (int i = 0; i < glyphs->num_glyphs; i++) {
		PangoGlyphInfo *gi = &glyphs->glyphs[i];
		const int at = advance;
		advance += gi->geometry.width;
		if (gi->glyph == PANGO_GLYPH_EMPTY)
			continue;
		double gx = base_x + double(at + gi->geometry.x_offset) / PANGO_SCALE;
		double gy = gi->geometry.y_offset == 0 ? base_y : base_y + double(gi->geometry.y_offset) / PANGO_SCALE;
		if (gi->glyph & PANGO_GLYPH_UNKNOWN_FLAG) {
			PangoGlyphInfo alone = *gi;
			alone.geometry.x_offset = alone.geometry.y_offset = 0;
			pango_draws(&alone, 1, gx, gy);
			continue;
		}
		if (snap) {
			// Rounded as Cairo rounds it: to the nearest quarter pixel.
			cairo_user_to_device(cr, &gx, &gy);
			gx = std::floor(gx * 4 + 0.5) / 4;
			gy = std::floor(gy * 4 + 0.5) / 4;
			cairo_device_to_user(cr, &gx, &gy);
		}
		out.push_back({gi->glyph, gx, gy});
	}
	if (out.empty() || colour || !emboldened(scaled) || !show_through_mask(cr, out, r->blank)) {
		if (!out.empty() && !colour && can_add(cr) && (r->blank || clear(cairo_get_target(cr), area_of(cr, out, false))))
			cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
		cairo_show_glyphs(cr, out.data(), int(out.size()));
	}
	r->blank = r->blank && out.empty();
	cairo_restore(cr);
}

static void render_rectangle(PangoRenderer *renderer, PangoRenderPart part, int x, int y, int width, int height)
{
	Renderer *r = reinterpret_cast<Renderer *>(renderer);
	cairo_save(r->cr);
	set_part_colour(r, part);
	cairo_rectangle(r->cr, r->x0 + double(x) / PANGO_SCALE, r->y0 + double(y) / PANGO_SCALE,
			double(width) / PANGO_SCALE, double(height) / PANGO_SCALE);
	cairo_fill(r->cr);
	cairo_restore(r->cr);
	r->blank = false;
}

// In device space, from the layout's origin (Pango draws decorations this
// way when its context has a matrix).
static void render_trapezoid(PangoRenderer *renderer, PangoRenderPart part, double y1, double x11, double x21,
			     double y2, double x12, double x22)
{
	Renderer *r = reinterpret_cast<Renderer *>(renderer);
	cairo_t *cr = r->cr;
	cairo_save(cr);
	set_part_colour(r, part);
	double x = r->x0, y = r->y0;
	cairo_user_to_device_distance(cr, &x, &y);
	cairo_identity_matrix(cr);
	cairo_translate(cr, x, y);
	cairo_move_to(cr, x11, y1);
	cairo_line_to(cr, x21, y1);
	cairo_line_to(cr, x22, y2);
	cairo_line_to(cr, x12, y2);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_restore(cr);
	r->blank = false;
}

// (Shapes and squiggly error underlines, which Pango's renderer also draws,
// aren't in any layout made here.)
static GType renderer_type()
{
	static gsize type = 0;
	if (g_once_init_enter(&type)) {
		const GType t = g_type_register_static_simple(
			PANGO_TYPE_RENDERER, g_intern_static_string("TypesetCairoRenderer"), sizeof(RendererClass),
			[](gpointer klass, gpointer) {
				PangoRendererClass *c = PANGO_RENDERER_CLASS(klass);
				c->draw_glyphs = render_glyphs;
				c->draw_rectangle = render_rectangle;
				c->draw_trapezoid = render_trapezoid;
			},
			sizeof(Renderer), nullptr, GTypeFlags(0));
		g_once_init_leave(&type, t);
	}
	return GType(type);
}

void show_layout(cairo_t *cr, PangoLayout *layout, bool blank)
{
	Renderer *r = static_cast<Renderer *>(g_object_new(renderer_type(), nullptr));
	r->cr = cr;
	r->snap = quarter_pixel_glyphs();
	r->blank = blank;
	// At the current point, as pango_cairo_show_layout() draws, which it
	// leaves where it was.
	const bool had_point = cairo_has_current_point(cr);
	cairo_get_current_point(cr, &r->x0, &r->y0);
	pango_renderer_draw_layout(&r->parent, layout, 0, 0);
	if (had_point)
		cairo_move_to(cr, r->x0, r->y0);
	else
		cairo_new_sub_path(cr);
	g_object_unref(r);
}

// The text with its outline, in the current source; for the shadow, its
// silhouette in one colour, grown by the outline and by spread, drawn into
// a new, clear surface.
static void draw_text(cairo_t *cr, PangoLayout *layout, const Style &s, const Frame &f, bool silhouette,
		      double spread = 0)
{
	cairo_save(cr);
	cairo_translate(cr, f.text_x, f.text_y);
	cairo_scale(cr, f.text_scale, f.text_scale);
	// Strokes are centred on the letters' edges, so half of each grows
	// them: round joins, so corners don't spike.
	auto stroke = [&](double grow_by) {
		pango_cairo_layout_path(cr, layout);
		cairo_set_line_width(cr, 2 * grow_by / f.text_scale);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_stroke(cr);
	};
	const double outline = s.outline && s.outline_width > 0 ? s.outline_width : 0.0;
	bool blank = silhouette;
	if (silhouette) {
		// The shadow's shape is the outlined text's, grown by its spread
		// (so it holds up thin letters over bright backgrounds): one
		// stroke covers both.
		if (outline + spread > 0) {
			stroke(outline + spread);
			blank = false;
		}
	} else if (outline > 0) {
		// Behind the fill, so it grows the letters outwards and never eats
		// into them.
		set_colour(cr, s.outline_colour);
		stroke(outline);
	}
	if (!silhouette)
		set_colour(cr, s.colour);
	show_layout(cr, layout, blank);
	cairo_restore(cr);
}

// Three box blurs make a close approximation of a Gaussian blur; across and
// down separately, as the text can be scaled differently each way. Each
// pass is a running sum (so any radius costs the same), along rows: down
// the image too, a row of sums at a time, which keeps to memory in order.
static void box_blur(uint8_t *data, int w, int h, int stride, double sigma_x, double sigma_y)
{
	if (w <= 0 || h <= 0)
		return;
	auto radius = [](double sigma) {
		return sigma < 0.5 ? -1 : std::max(1, int(std::floor(sigma * 3 * std::sqrt(2 * M_PI) / 4 + 0.5))) / 2;
	};
	const int rx = radius(sigma_x), ry = radius(sigma_y);
	// Dividing by the box's width: multiplying by its reciprocal, in 16.16.
	auto reciprocal = [](int r) { return uint32_t((65536 + r) / (2 * r + 1)); };
	const size_t W = size_t(w);

	if (rx >= 0) {
		const uint32_t mul = reciprocal(rx);
		std::vector<uint8_t> a(W), b(W);
		for (int y = 0; y < h; y++) {
			uint8_t *row = data + size_t(y) * size_t(stride);
			const uint8_t *in = row;
			for (int k = 0; k < 3; k++) {
				uint8_t *out = k == 2 ? row : (k == 0 ? a.data() : b.data());
				uint32_t sum = 0;
				for (int i = 0; i <= rx && i < w; i++)
					sum += in[i];
				for (int i = 0; i < w; i++) {
					out[i] = uint8_t((sum * mul + 32768) >> 16);
					const int add = i + rx + 1, sub = i - rx;
					sum += (add < w ? in[add] : 0) - (sub >= 0 ? in[sub] : 0);
				}
				in = out;
			}
		}
	}
	if (ry >= 0) {
		const uint32_t mul = reciprocal(ry);
		std::vector<uint8_t> tmp(W * size_t(h));
		std::vector<uint32_t> sums(W);
		auto row_of = [&](uint8_t *base, size_t pitch, int y) { return base + size_t(y) * pitch; };
		for (int k = 0; k < 3; k++) {
			// Ping-pong between the image and a copy: data -> tmp -> data -> tmp, then back.
			uint8_t *src = (k % 2 == 0) ? data : tmp.data();
			uint8_t *dst = (k % 2 == 0) ? tmp.data() : data;
			const size_t sp = (k % 2 == 0) ? size_t(stride) : W, dp = (k % 2 == 0) ? W : size_t(stride);
			std::fill(sums.begin(), sums.end(), 0);
			for (int i = 0; i <= ry && i < h; i++) {
				const uint8_t *r = row_of(src, sp, i);
				for (size_t x = 0; x < W; x++)
					sums[x] += r[x];
			}
			for (int y = 0; y < h; y++) {
				uint8_t *out = row_of(dst, dp, y);
				for (size_t x = 0; x < W; x++)
					out[x] = uint8_t((sums[x] * mul + 32768) >> 16);
				const int add = y + ry + 1, sub = y - ry;
				if (add < h) {
					const uint8_t *r = row_of(src, sp, add);
					for (size_t x = 0; x < W; x++)
						sums[x] += r[x];
				}
				if (sub >= 0) {
					const uint8_t *r = row_of(src, sp, sub);
					for (size_t x = 0; x < W; x++)
						sums[x] -= r[x];
				}
			}
		}
		// Three passes end in tmp: back into the image.
		for (int y = 0; y < h; y++)
			memcpy(data + size_t(y) * size_t(stride), tmp.data() + size_t(y) * W, W);
	}
}

// Grows a mask by an ellipse of radii rx and ry whole pixels (each pixel
// takes the largest value within it).
static void dilate_whole(uint8_t *data, int w, int h, int stride, int K, int R, double rx, double ry)
{
	if ((R <= 0 && K <= 0) || w <= 0 || h <= 0)
		return;
	const size_t W = size_t(w);
	// How far across each row of the ellipse reaches.
	std::vector<int> half(size_t(2 * R + 1));
	for (int dy = -R; dy <= R; dy++) {
		const double t = R > 0 ? double(dy) / ry : 0.0;
		half[size_t(dy + R)] = t * t > 1.0 + 1e-9 ? -1 : std::min(K, int(std::floor(rx * std::sqrt(std::max(0.0, 1.0 - t * t)) + 0.5)));
	}
	// For the rows in reach, each grown across by 0 to K pixels: a ring of them.
	const size_t rows = size_t(2 * R + 1);
	std::vector<uint8_t> ring(rows * size_t(K + 1) * W);
	auto slot = [&](int row, int k) { return ring.data() + ((size_t(row) % rows) * size_t(K + 1) + size_t(k)) * W; };
	auto build = [&](int row) {
		uint8_t *prev = slot(row, 0);
		memcpy(prev, data + size_t(row) * size_t(stride), W);
		for (int k = 1; k <= K; k++) {
			uint8_t *cur = slot(row, k);
			// Each pixel and its neighbours' largest (no branches inside,
			// so it's done many pixels at a time).
			cur[0] = W > 1 ? std::max(prev[0], prev[1]) : prev[0];
			for (size_t x = 1; x + 1 < W; x++)
				cur[x] = std::max(std::max(prev[x - 1], prev[x]), prev[x + 1]);
			if (W > 1)
				cur[W - 1] = std::max(prev[W - 2], prev[W - 1]);
			prev = cur;
		}
	};
	for (int j = 0; j < R && j < h; j++)
		build(j);
	std::vector<uint8_t> out(W);
	for (int y = 0; y < h; y++) {
		if (y + R < h)
			build(y + R); // before row y is written over (it may be this one)
		std::fill(out.begin(), out.end(), 0);
		for (int dy = -R; dy <= R; dy++) {
			const int j = y + dy, k = half[size_t(dy + R)];
			if (j < 0 || j >= h || k < 0)
				continue;
			const uint8_t *src = slot(j, k);
			for (size_t x = 0; x < W; x++)
				out[x] = std::max(out[x], src[x]);
		}
		memcpy(data + size_t(y) * size_t(stride), out.data(), W);
	}
}

// The shadow's spread: the mask grown by an ellipse of radii rx and ry pixels,
// as a stroke round every letter would grow it, at a small fraction of the
// cost. A radius part-way between whole pixels is a mix of the two either
// side, so the shadow grows smoothly with the text's scale.
static void dilate(uint8_t *data, int w, int h, int stride, double rx, double ry)
{
	if (w <= 0 || h <= 0 || (rx <= 0 && ry <= 0))
		return;
	const int kx = int(std::floor(rx)), ky = int(std::floor(ry));
	const double t = ((rx - kx) + (ry - ky)) / 2;
	if (t < 1.0 / 64) {
		dilate_whole(data, w, h, stride, kx, ky, std::max(rx, 1e-9), std::max(ry, 1e-9));
		return;
	}
	const size_t W = size_t(w);
	std::vector<uint8_t> more(W * size_t(h));
	for (int y = 0; y < h; y++)
		memcpy(&more[size_t(y) * W], data + size_t(y) * size_t(stride), W);
	dilate_whole(data, w, h, stride, kx, ky, std::max(double(kx), 1e-9), std::max(double(ky), 1e-9));
	dilate_whole(more.data(), w, h, w, kx + 1, ky + 1, double(kx + 1), double(ky + 1));
	const uint32_t f = uint32_t(std::lround(t * 256));
	for (int y = 0; y < h; y++) {
		uint8_t *a = data + size_t(y) * size_t(stride);
		const uint8_t *b = &more[size_t(y) * W];
		for (size_t x = 0; x < W; x++)
			a[x] = uint8_t((a[x] * (256 - f) + b[x] * f + 128) >> 8);
	}
}

// Whether any of the layout's letters come from a colour font (emoji):
// those keep their own colours, so the text isn't one colour.
static bool has_colour_glyphs(PangoLayout *layout)
{
	bool colour = false;
	PangoLayoutIter *it = pango_layout_get_iter(layout);
	do {
		PangoLayoutRun *run = pango_layout_iter_get_run_readonly(it);
		if (!run || !run->item || !run->item->analysis.font)
			continue;
		colour = colour_font(run->item->analysis.font);
	} while (!colour && pango_layout_iter_next_run(it));
	pango_layout_iter_free(it);
	return colour;
}

// Text that's one colour (with, at most, a shadow of another): it can be
// drawn in the coverage form.
static bool one_colour(PangoLayout *layout, const Document &doc)
{
	const Style &s = doc.style;
	if (s.plate || (s.outline && s.outline_width > 0))
		return false;
	for (const Span &sp : doc.spans)
		if (sp.has_colour)
			return false;
	return !has_colour_glyphs(layout);
}

Raster rasterise(PangoLayout *layout, const Document &doc, RasterParams params, bool coverage)
{
	Raster out;
	const Style &s = doc.style;
	const Frame f = frame_of(layout, doc);
	const Box box = drawn_box(layout, doc, f);
	if (doc.text.empty() && !s.plate)
		return out;

	// Too big a texture: shrink the scale to fit.
	const double limit = max_raster_side - 4;
	if (box.w * params.sx > limit)
		params.sx = limit / box.w;
	if (box.h * params.sy > limit)
		params.sy = limit / box.h;
	const double area = box.w * params.sx * box.h * params.sy;
	if (area > max_raster_pixels) {
		const double k = std::sqrt(max_raster_pixels / area);
		params.sx *= k;
		params.sy *= k;
	}
	out.params = params;

	// One pixel's margin round the drawn box for the antialiasing.
	const double px0 = box.x * params.sx + params.ox, py0 = box.y * params.sy + params.oy;
	const double px1 = (box.x + box.w) * params.sx + params.ox, py1 = (box.y + box.h) * params.sy + params.oy;
	out.x0 = int(std::floor(px0)) - 1;
	out.y0 = int(std::floor(py0)) - 1;
	out.width = std::max(int(std::ceil(px1)) + 1 - out.x0, 0);
	out.height = std::max(int(std::ceil(py1)) + 1 - out.y0, 0);
	if (out.width <= 2 || out.height <= 2) {
		out.width = out.height = 0;
		return out;
	}

	// Drawn straight into the raster's own pixels when Cairo can use rows
	// packed edge to edge (it always can for ARGB32), so there's no second
	// full-size copy; otherwise into a surface of its own and copied.
	// Cairo's ARGB32 is premultiplied BGRA in memory on little-endian
	// machines: what OBS's GS_BGRA textures take. Cairo's objects are owned
	// here, so an exception (an allocation in the blur) can't leak them.
	using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
	using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
	auto fail = [&out] {
		out.width = out.height = 0;
		out.pixels = std::vector<uint8_t>();
		out.failed = true; // out of memory: try again later
		return out;
	};
	// The shadow's shape, blurred: a mask of how much each pixel has, in
	// the raster's pixels. No shadow is a wrong drawing, not a lesser one:
	// a failure here fails the whole drawing, to be tried again.
	auto shadow_mask = [&]() -> Surface {
		// The blur's size is in source units, like everything else; half
		// of it is the Gaussian's standard deviation. The spread grows the
		// silhouette's pixels (fast) where the blur is wide enough to hide
		// the difference; otherwise it's a stroke round each letter (exact).
		const double sigma = std::max(0.0, s.shadow_blur) * 0.5;
		const double spread = std::max(0.0, s.shadow_spread);
		const double blur_px = sigma * std::min(params.sx, params.sy);
		const double spread_px = spread * std::max(params.sx, params.sy);
		const bool grow_after = spread > 0 && blur_px >= 1.0 && blur_px >= spread_px / 2;
		Surface mask(cairo_image_surface_create(CAIRO_FORMAT_A8, out.width, out.height), cairo_surface_destroy);
		if (cairo_surface_status(mask.get()) != CAIRO_STATUS_SUCCESS)
			return Surface(nullptr, cairo_surface_destroy);
		{
			Context mcr(cairo_create(mask.get()), cairo_destroy);
			cairo_translate(mcr.get(), params.ox - out.x0, params.oy - out.y0);
			cairo_scale(mcr.get(), params.sx, params.sy);
			cairo_translate(mcr.get(), s.shadow_x, s.shadow_y);
			cairo_set_source_rgba(mcr.get(), 0, 0, 0, 1);
			draw_text(mcr.get(), layout, s, f, true, grow_after ? 0.0 : spread);
			if (cairo_status(mcr.get()) != CAIRO_STATUS_SUCCESS)
				return Surface(nullptr, cairo_surface_destroy);
		}
		cairo_surface_flush(mask.get());
		uint8_t *mask_data = cairo_image_surface_get_data(mask.get());
		const int mask_stride = cairo_image_surface_get_stride(mask.get());
		if (grow_after)
			dilate(mask_data, out.width, out.height, mask_stride, spread * params.sx, spread * params.sy);
		box_blur(mask_data, out.width, out.height, mask_stride, sigma * params.sx, sigma * params.sy);
		cairo_surface_mark_dirty(mask.get());
		return mask;
	};

	if (coverage && one_colour(layout, doc)) {
		// The text's coverage, and its shadow's, side by side in each pixel.
		// With no shadow, drawn straight into the raster's pixels: rows of
		// a whole number of 4 bytes (as Cairo's A8 rows are), the few
		// pixels added on the right clear.
		if (!s.shadow)
			out.width = (out.width + 3) / 4 * 4;
		std::vector<uint8_t> direct_pixels;
		if (!s.shadow) {
			try {
				direct_pixels.assign(size_t(out.width) * size_t(out.height), 0);
			} catch (const std::bad_alloc &) {
				return fail();
			}
		}
		Surface fill(s.shadow ? cairo_image_surface_create(CAIRO_FORMAT_A8, out.width, out.height)
				      : cairo_image_surface_create_for_data(direct_pixels.data(), CAIRO_FORMAT_A8,
									    out.width, out.height, out.width),
			     cairo_surface_destroy);
		if (cairo_surface_status(fill.get()) != CAIRO_STATUS_SUCCESS)
			return fail();
		{
			Context cr(cairo_create(fill.get()), cairo_destroy);
			cairo_translate(cr.get(), params.ox - out.x0, params.oy - out.y0);
			cairo_scale(cr.get(), params.sx, params.sy);
			cairo_set_source_rgba(cr.get(), 0, 0, 0, 1);
			draw_text(cr.get(), layout, s, f, true); // the letters alone (no outline: see one_colour)
			if (cairo_status(cr.get()) != CAIRO_STATUS_SUCCESS)
				return fail();
		}
		cairo_surface_flush(fill.get());
		Surface shadow(nullptr, cairo_surface_destroy);
		if (s.shadow) {
			shadow = shadow_mask();
			if (!shadow)
				return fail();
		}
		out.bytes = shadow ? 2 : 1;
		out.fill_colour = s.colour;
		out.shadow_colour = s.shadow_colour;
		if (!shadow) {
			fill.reset(); // before its pixels move
			out.pixels = std::move(direct_pixels);
			find_parts(out);
			return out;
		}
		try {
			out.pixels.assign(size_t(out.width) * size_t(out.height) * size_t(out.bytes), 0);
		} catch (const std::bad_alloc &) {
			return fail();
		}
		const uint8_t *fd = cairo_image_surface_get_data(fill.get());
		const int fs = cairo_image_surface_get_stride(fill.get());
		{
			const uint8_t *sd = cairo_image_surface_get_data(shadow.get());
			const int ss = cairo_image_surface_get_stride(shadow.get());
			for (int y = 0; y < out.height; y++) {
				uint8_t *o = &out.pixels[size_t(y) * size_t(out.width) * 2];
				const uint8_t *a = fd + size_t(y) * size_t(fs), *b = sd + size_t(y) * size_t(ss);
				for (int x = 0; x < out.width; x++) {
					o[2 * x] = a[x];
					o[2 * x + 1] = b[x];
				}
			}
		}
		find_parts(out);
		return out;
	}

	const int packed = out.width * 4;
	const bool direct = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, out.width) == packed;
	try {
		out.pixels.assign(size_t(packed) * size_t(out.height), 0);
	} catch (const std::bad_alloc &) {
		return fail();
	}
	{
		Surface surface(direct ? cairo_image_surface_create_for_data(out.pixels.data(), CAIRO_FORMAT_ARGB32,
									    out.width, out.height, packed)
				       : cairo_image_surface_create(CAIRO_FORMAT_ARGB32, out.width, out.height),
				cairo_surface_destroy);
		if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS)
			return fail();
		Context cr(cairo_create(surface.get()), cairo_destroy);
		cairo_translate(cr.get(), params.ox - out.x0, params.oy - out.y0);
		cairo_scale(cr.get(), params.sx, params.sy);

		if (s.plate) {
			rounded_rect(cr.get(), f.plate, s.plate_radius);
			set_colour(cr.get(), s.plate_colour);
			cairo_fill(cr.get());
		}

		if (s.shadow) {
			Surface mask = shadow_mask();
			if (!mask)
				return fail();
			cairo_save(cr.get());
			cairo_identity_matrix(cr.get());
			set_colour(cr.get(), s.shadow_colour);
			cairo_mask_surface(cr.get(), mask.get(), 0, 0);
			cairo_restore(cr.get());
		}

		draw_text(cr.get(), layout, s, f, false);
		if (cairo_status(cr.get()) != CAIRO_STATUS_SUCCESS)
			return fail();
		cr.reset();
		cairo_surface_flush(surface.get());
		if (!direct) {
			const int stride = cairo_image_surface_get_stride(surface.get());
			const uint8_t *data = cairo_image_surface_get_data(surface.get());
			for (int y = 0; y < out.height; y++)
				memcpy(&out.pixels[size_t(y) * size_t(packed)], data + size_t(y) * stride, size_t(packed));
		}
	} // the surface (which may use out.pixels) goes before out is returned
	find_parts(out);
	return out;
}

void find_parts(Raster &r)
{
	r.parts.clear();
	if (r.width <= 0 || r.height <= 0 || r.pixels.empty())
		return;
	// A tile counts if anything within a pixel of it has ink: filtering a
	// scaled drawing reaches that far, so a tile left out is one whose
	// every drawn pixel would have been clear.
	constexpr int tile = 16;
	const int tx = (r.width + tile - 1) / tile, ty = (r.height + tile - 1) / tile;
	std::vector<uint8_t> inked(size_t(tx) * size_t(ty), 0);
	const size_t bytes = size_t(r.bytes);
	for (int y = 0; y < r.height; y++) {
		const uint8_t *row = &r.pixels[size_t(y) * size_t(r.width) * bytes];
		const int t0 = std::max(0, (y - 1) / tile), t1 = std::min(ty - 1, (y + 1) / tile);
		auto mark = [&](int u) {
			for (int t = t0; t <= t1; t++)
				inked[size_t(t) * size_t(tx) + size_t(u)] = 1;
		};
		for (int u = 0; u < tx; u++) {
			const int x0 = u * tile, x1 = std::min(r.width, x0 + tile);
			const uint8_t *chunk = row + size_t(x0) * bytes;
			const size_t n = size_t(x1 - x0) * bytes;
			if (!any_set(chunk, n))
				continue;
			mark(u);
			// Ink at a tile's edge reaches the tile beside it.
			if (u > 0 && any_set(chunk, bytes))
				mark(u - 1);
			if (u + 1 < tx && any_set(chunk + n - bytes, bytes))
				mark(u + 1);
		}
	}
	long drawn = 0;
	for (int t = 0; t < ty; t++) {
		for (int u = 0; u < tx; u++) {
			if (!inked[size_t(t) * size_t(tx) + size_t(u)])
				continue;
			int end = u;
			while (end + 1 < tx && inked[size_t(t) * size_t(tx) + size_t(end + 1)])
				end++;
			const int x = u * tile, y = t * tile;
			Raster::Part p{x, y, std::min(r.width, (end + 1) * tile) - x, std::min(r.height, y + tile) - y};
			drawn += long(p.w) * p.h;
			r.parts.push_back(p);
			u = end;
		}
	}
	// Nearly all of it: one part is as good, and cheaper to set up.
	if (drawn * 10 > long(r.width) * r.height * 9)
		r.parts.clear();
	else if (r.parts.empty())
		r.parts.push_back({0, 0, 1, 1}); // all clear: a pixel, so it's not taken as "all of it"
}

size_t backspace_index(PangoLayout *layout, size_t index)
{
	const char *text = pango_layout_get_text(layout);
	const size_t size = strlen(text);
	index = std::min(index, size);
	const long i = g_utf8_pointer_to_offset(text, text + index);
	if (i <= 0)
		return 0;
	int n = 0;
	const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(layout, &n);
	long j = i - 1;
	if (!(i < n && attrs[i].backspace_deletes_character))
		while (j > 0 && !attrs[j].is_cursor_position)
			j--;
	return size_t(g_utf8_offset_to_pointer(text, j) - text);
}

bool font_installed(PangoContext *context, const std::string &family, std::string *used)
{
	static const char *const generic[] = {"sans", "sans-serif", "serif", "monospace", "mono", "cursive",
					      "fantasy", "system-ui", "emoji", "math"};
	bool found = false;
	for (const char *g : generic)
		found = found || strcasecmp(family.c_str(), g) == 0;

	if (!found) {
		PangoFontFamily **families = nullptr;
		int n = 0;
		pango_font_map_list_families(pango_context_get_font_map(context), &families, &n);
		for (int i = 0; i < n && !found; i++)
			found = strcasecmp(pango_font_family_get_name(families[i]), family.c_str()) == 0;
		g_free(families);
	}
	if (used) {
		PangoFontDescription *desc = pango_font_description_from_string("Sans 12px");
		pango_font_description_set_family(desc, family.c_str());
		PangoFont *font = pango_context_load_font(context, desc);
		pango_font_description_free(desc);
		used->clear();
		if (font) {
			PangoFontDescription *d = pango_font_describe(font);
			if (const char *name = pango_font_description_get_family(d))
				*used = name;
			pango_font_description_free(d);
			g_object_unref(font);
		}
	}
	return found;
}

double widest_word(PangoContext *context, const Document &doc)
{
	PangoLayout *layout = create_layout(context, doc);
	const int w = widest_word_units(layout);
	g_object_unref(layout);
	return double(w) / PANGO_SCALE;
}

} // namespace typeset
