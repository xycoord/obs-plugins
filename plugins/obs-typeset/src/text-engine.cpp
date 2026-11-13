#include "text-engine.hpp"

#include <obs.h>

#include <algorithm>
#include <memory>
#include <cmath>
#include <cstring>
#include <new>
#include <strings.h>

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

// The text with its outline, in the current source; for the shadow, its
// silhouette in one colour, grown by the outline and the shadow's spread.
static void draw_text(cairo_t *cr, PangoLayout *layout, const Style &s, const Frame &f, bool silhouette)
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
	if (silhouette) {
		// The shadow's shape is the outlined text's, grown by its spread
		// (so it holds up thin letters over bright backgrounds): one
		// stroke covers both.
		if (outline + s.shadow_spread > 0)
			stroke(outline + s.shadow_spread);
	} else if (outline > 0) {
		// Behind the fill, so it grows the letters outwards and never eats
		// into them.
		set_colour(cr, s.outline_colour);
		stroke(outline);
	}
	if (!silhouette)
		set_colour(cr, s.colour);
	pango_cairo_show_layout(cr, layout);
	cairo_restore(cr);
}

// Three box blurs make a close approximation of a Gaussian blur; across and
// down separately, as the text can be scaled differently each way.
static void box_blur(uint8_t *data, int w, int h, int stride, double sigma_x, double sigma_y)
{
	if (w <= 0 || h <= 0)
		return;
	auto radius = [](double sigma) {
		return sigma < 0.5 ? -1 : std::max(1, int(std::floor(sigma * 3 * std::sqrt(2 * M_PI) / 4 + 0.5))) / 2;
	};
	const int rx = radius(sigma_x), ry = radius(sigma_y);
	std::vector<uint8_t> line(size_t(std::max(w, h)));
	auto pass = [&](uint8_t *start, int n, int step, int r) {
		for (int i = 0; i < n; i++)
			line[size_t(i)] = start[size_t(i) * step];
		int sum = 0;
		for (int i = -r; i <= r; i++)
			sum += (i >= 0 && i < n) ? line[size_t(i)] : 0;
		const int width = 2 * r + 1;
		for (int i = 0; i < n; i++) {
			start[size_t(i) * step] = uint8_t(sum / width);
			const int in = i + r + 1, out = i - r;
			sum += (in < n ? line[size_t(in)] : 0) - (out >= 0 ? line[size_t(out)] : 0);
		}
	};
	for (int k = 0; k < 3; k++) {
		if (rx >= 0)
			for (int y = 0; y < h; y++)
				pass(data + size_t(y) * stride, w, 1, rx);
		if (ry >= 0)
			for (int x = 0; x < w; x++)
				pass(data + x, h, stride, ry);
	}
}

Raster rasterise(PangoLayout *layout, const Document &doc, RasterParams params)
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
			// No shadow is a wrong drawing, not a lesser one: a failure
			// here fails the whole drawing, to be tried again.
			Surface mask(cairo_image_surface_create(CAIRO_FORMAT_A8, out.width, out.height),
				     cairo_surface_destroy);
			if (cairo_surface_status(mask.get()) != CAIRO_STATUS_SUCCESS)
				return fail();
			{
				Context mcr(cairo_create(mask.get()), cairo_destroy);
				cairo_translate(mcr.get(), params.ox - out.x0, params.oy - out.y0);
				cairo_scale(mcr.get(), params.sx, params.sy);
				cairo_translate(mcr.get(), s.shadow_x, s.shadow_y);
				cairo_set_source_rgba(mcr.get(), 0, 0, 0, 1);
				draw_text(mcr.get(), layout, s, f, true);
				if (cairo_status(mcr.get()) != CAIRO_STATUS_SUCCESS)
					return fail();
			}
			cairo_surface_flush(mask.get());
			// The blur's size is in source units, like everything else;
			// half of it is the Gaussian's standard deviation.
			const double sigma = std::max(0.0, s.shadow_blur) * 0.5;
			box_blur(cairo_image_surface_get_data(mask.get()), out.width, out.height,
				 cairo_image_surface_get_stride(mask.get()), sigma * params.sx, sigma * params.sy);
			cairo_surface_mark_dirty(mask.get());

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
	return out;
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
