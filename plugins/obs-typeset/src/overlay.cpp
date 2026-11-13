#include "overlay.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace typeset {

namespace {

// Colours of the editing marks, 0xRRGGBBAA. The accent is a clear blue that
// isn't OBS's red selection box, so editing reads as a different state.
constexpr uint32_t accent = 0x2F8FFFFF;
constexpr uint32_t accent_edge = 0x0A1830D0; // dark edge round the accent, so it shows on blue too
constexpr uint32_t selection_keyline = 0xFFFFFFC0;
constexpr uint32_t tag_fill = 0x10141CE8;
constexpr uint32_t veil = 0x00000040; // over the rest of the canvas while editing
constexpr uint32_t selection_fill = 0x2F6FE0E6; // nearly opaque: the selected glyphs are drawn again on it
constexpr uint32_t selection_wash = 0x2F8FFF70; // when they can't be (a rotated source)
constexpr uint32_t placeholder_line = 0xFFFFFFB0;

// How long the caret stays solid after a key or click, and its blink.
constexpr uint64_t caret_hold_ms = 500, caret_blink_ms = 530;

const char *bar_glyph(BarAction a)
{
	switch (a) {
	case BarAction::Bold:
		return "B";
	case BarAction::Italic:
		return "I";
	case BarAction::Underline:
		return "U";
	case BarAction::Smaller:
		return "A−";
	case BarAction::Bigger:
		return "A+";
	case BarAction::Colour:
		return "A";
	case BarAction::Clear:
		return obs_module_text("Bar.Clear");
	default: // drawn otherwise
		return "";
	}
}

struct Quad {
	vec2 p[4]; // in order round the edge
};

Quad rect(float x0, float y0, float x1, float y1)
{
	Quad q;
	vec2_set(&q.p[0], x0, y0);
	vec2_set(&q.p[1], x1, y0);
	vec2_set(&q.p[2], x1, y1);
	vec2_set(&q.p[3], x0, y1);
	return q;
}

void fill(const std::vector<Quad> &quads, uint32_t rgba)
{
	if (quads.empty())
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colour = gs_effect_get_param_by_name(solid, "color");
	vec4 c;
	vec4_set(&c, float(rgba >> 24) / 255.0f, float((rgba >> 16) & 0xFF) / 255.0f, float((rgba >> 8) & 0xFF) / 255.0f,
		 float(rgba & 0xFF) / 255.0f);
	gs_effect_set_vec4(colour, &c);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		for (const Quad &q : quads) {
			gs_vertex2f(q.p[0].x, q.p[0].y);
			gs_vertex2f(q.p[1].x, q.p[1].y);
			gs_vertex2f(q.p[2].x, q.p[2].y);
			gs_vertex2f(q.p[0].x, q.p[0].y);
			gs_vertex2f(q.p[2].x, q.p[2].y);
			gs_vertex2f(q.p[3].x, q.p[3].y);
		}
		gs_render_stop(GS_TRIS);
	}
}

// A line from a to b, w display pixels wide.
Quad line_quad(const vec2 &a, const vec2 &b, float w)
{
	vec2 d;
	vec2_sub(&d, &b, &a);
	const float len = std::max(vec2_len(&d), 1e-3f);
	vec2 n;
	vec2_set(&n, -d.y / len * w * 0.5f, d.x / len * w * 0.5f);
	Quad q;
	vec2_add(&q.p[0], &a, &n);
	vec2_add(&q.p[1], &b, &n);
	vec2_sub(&q.p[2], &b, &n);
	vec2_sub(&q.p[3], &a, &n);
	return q;
}

// The outline of a quad, as lines w wide; dashed with dash > 0.
void outline(std::vector<Quad> &out, const Quad &q, float w, float dash = 0)
{
	for (int i = 0; i < 4; i++) {
		const vec2 &a = q.p[i], &b = q.p[(i + 1) % 4];
		if (dash <= 0) {
			out.push_back(line_quad(a, b, w));
			continue;
		}
		vec2 d;
		vec2_sub(&d, &b, &a);
		const float len = vec2_len(&d);
		for (float t = 0; t < len; t += 2 * dash) {
			vec2 s, e;
			vec2_mulf(&s, &d, t / len);
			vec2_add(&s, &s, &a);
			vec2_mulf(&e, &d, std::min(t + dash, len) / len);
			vec2_add(&e, &e, &a);
			out.push_back(line_quad(s, e, w));
		}
	}
}

// Grown outwards by d display pixels (for a quad whose corners go round
// clockwise or anticlockwise; its centre is used).
Quad grown(const Quad &q, float d)
{
	vec2 c;
	vec2_set(&c, (q.p[0].x + q.p[2].x) / 2, (q.p[0].y + q.p[2].y) / 2);
	Quad out;
	for (int i = 0; i < 4; i++) {
		vec2 v;
		vec2_sub(&v, &q.p[i], &c);
		const float len = std::max(vec2_len(&v), 1e-3f);
		vec2_mulf(&v, &v, (len + d * 1.41421f) / len);
		vec2_add(&out.p[i], &c, &v);
	}
	return out;
}

// From an item's source units to display pixels.
struct Projector {
	matrix4 m;
	Mapping map;
	Projector(const ItemRef &t, const Mapping &map) : map(map) { item_to_canvas(t.item(), t.group(), &m); }
	vec2 operator()(double x, double y) const
	{
		vec3 p;
		vec3_set(&p, float(x), float(y), 0.0f);
		vec3_transform(&p, &p, &m);
		vec2 out;
		vec2_set(&out, float(map.x + p.x * map.scale), float(map.y + p.y * map.scale));
		return out;
	}
	Quad box(const Box &b) const
	{
		return Quad{{(*this)(b.x, b.y), (*this)(b.x + b.w, b.y), (*this)(b.x + b.w, b.y + b.h), (*this)(b.x, b.y + b.h)}};
	}
};

} // namespace

// --- Labels -----------------------------------------------------------------------------------

// A short piece of text as a texture, made once (with a font map of its own
// on the graphics thread).
void OverlayPainter::Label::make(const std::string &s, double px, uint32_t fg, uint32_t bg)
{
	destroy();
	text = s;
	static PangoContext *context = create_context(pango_cairo_font_map_new());
	PangoLayout *layout = pango_layout_new(context);
	PangoFontDescription *desc = pango_font_description_from_string("Sans Bold");
	pango_font_description_set_absolute_size(desc, px * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, s.c_str(), -1);
	int tw = 0, th = 0;
	pango_layout_get_pixel_size(layout, &tw, &th);
	const int pad = int(px * 0.45);
	width = tw + 2 * pad;
	height = th + pad;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		g_object_unref(layout);
		return;
	}
	cairo_t *cr = cairo_create(surface);
	auto rgba = [&](uint32_t c) {
		cairo_set_source_rgba(cr, (c >> 24) / 255.0, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0,
				      (c & 0xFF) / 255.0);
	};
	if (bg & 0xFF) {
		const double r = height / 2.0;
		cairo_new_sub_path(cr);
		cairo_arc(cr, width - r, r, r, -M_PI / 2, M_PI / 2);
		cairo_arc(cr, r, r, r, M_PI / 2, 3 * M_PI / 2);
		cairo_close_path(cr);
		rgba(bg);
		cairo_fill(cr);
	}
	cairo_move_to(cr, pad, pad / 2.0);
	rgba(fg);
	pango_cairo_show_layout(cr, layout);
	cairo_destroy(cr);
	cairo_surface_flush(surface);
	const int stride = cairo_image_surface_get_stride(surface);
	const uint8_t *data = cairo_image_surface_get_data(surface);
	std::vector<uint8_t> pixels;
	if (stride != width * 4) { // OBS takes rows packed
		pixels.resize(size_t(width) * height * 4);
		for (int y = 0; y < height; y++)
			memcpy(&pixels[size_t(y) * width * 4], data + size_t(y) * stride, size_t(width) * 4);
		data = pixels.data();
	}
	texture = gs_texture_create(uint32_t(width), uint32_t(height), GS_BGRA, 1, &data, 0);
	cairo_surface_destroy(surface);
	g_object_unref(layout);
}

void OverlayPainter::Label::draw(float x, float y) const
{
	if (!texture)
		return;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_matrix_push();
	gs_matrix_translate3f(std::round(x), std::round(y), 0.0f);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture, 0, 0, 0);
	gs_matrix_pop();
	gs_blend_state_pop();
}

void OverlayPainter::Label::destroy()
{
	if (texture)
		gs_texture_destroy(texture);
	texture = nullptr;
}

void OverlayPainter::destroy()
{
	for (Label *l : {&tag_, &placeholder_, &preedit_, &size_})
		l->destroy();
	for (Label &l : glyphs_)
		l.destroy();
}

// --- The format bar -----------------------------------------------------------------------------

// Its buttons from (x, y), each the size of the strip, with gaps between
// the kinds (style, size, colour and alignment, clear).
void OverlayPainter::paint_bar(const Overlay &o, float x, float y, float px, std::vector<BarButton> *bar)
{
	const float size = 28 * px, gap = 2 * px;
	std::vector<BarButton> buttons;
	for (int i = 0; i < int(BarAction::Count); i++) {
		const auto a = BarAction(i);
		const float w = a == BarAction::SizeReadout || a == BarAction::Clear ? size * 1.9f : size;
		buttons.push_back({a, x, y, w, size});
		x += w + gap + (a == BarAction::Underline || a == BarAction::Bigger || a == BarAction::Align ? 3 * gap : 0);
	}

	const BarState &st = o.bar;
	auto active = [&](BarAction a) {
		return (a == BarAction::Bold && st.bold) || (a == BarAction::Italic && st.italic) ||
		       (a == BarAction::Underline && st.underline);
	};
	std::vector<Quad> fills, actives;
	for (const BarButton &b : buttons)
		(active(b.action) ? actives : fills).push_back(rect(b.x, b.y, b.x + b.w, b.y + b.h));
	fill(fills, tag_fill);
	fill(actives, accent);

	for (const BarButton &b : buttons) {
		switch (b.action) {
		case BarAction::SizeReadout:
			size_.ensure(std::to_string(st.size_px) + " px", 12 * px, 0xFFFFFFFF, 0);
			size_.draw(b.x + (b.w - size_.width) / 2, b.y + (b.h - size_.height) / 2);
			continue;
		case BarAction::Align: {
			// Three lines, aligned as the text is.
			const float widths[] = {16 * px, 10 * px, 13 * px};
			std::vector<Quad> lines;
			for (int i = 0; i < 3; i++) {
				const float lw = widths[i], ly = b.y + 9 * px + i * 5 * px;
				const float lx = st.align == int(Align::Centre)  ? b.x + (b.w - lw) / 2
						 : st.align == int(Align::Right) ? b.x + b.w - 6 * px - lw
										 : b.x + 6 * px;
				vec2 a0, a1;
				vec2_set(&a0, lx, ly);
				vec2_set(&a1, lx + lw, ly);
				lines.push_back(line_quad(a0, a1, 2 * px));
			}
			fill(lines, 0xFFFFFFFF);
			continue;
		}
		case BarAction::Colour: {
			// An "A" over a bar of the colour, as in the slide tools.
			const Quad swatch = rect(b.x + 6 * px, b.y + b.h - 8 * px, b.x + b.w - 6 * px, b.y + b.h - 4 * px);
			fill({grown(swatch, 1 * px)}, 0xFFFFFF90);
			const uint32_t c = st.colour;
			fill({swatch}, (c & 0xFF) << 24 | ((c >> 8) & 0xFF) << 16 | ((c >> 16) & 0xFF) << 8 | 0xFF);
			break;
		}
		default:
			break;
		}
		Label &l = glyphs_[int(b.action)];
		l.ensure(bar_glyph(b.action), b.action == BarAction::Clear ? 12 * px : 15 * px, 0xFFFFFFFF, 0);
		const float lift = b.action == BarAction::Colour ? 3 * px : 0;
		l.draw(b.x + (b.w - l.width) / 2, b.y + (b.h - l.height) / 2 - lift);
	}
	if (bar)
		*bar = std::move(buttons);
}

// --- Painting ---------------------------------------------------------------------------------

void OverlayPainter::paint(const Overlay &o, uint32_t, uint32_t cy, std::vector<BarButton> *bar)
{
	if (bar)
		bar->clear();
	if (!o.editing && o.placeholders.empty())
		return;
	const Mapping map = o.mapping;
	const float px = float(std::max(1.0, std::round(map.dpr)));

	gs_matrix_push();
	gs_matrix_identity();
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	// Empty texts: a dashed frame and what to do, so they can be found.
	if (!o.placeholders.empty()) {
		placeholder_.ensure(obs_module_text("Placeholder"), 13 * px, 0xFFFFFFFF, 0x000000A0);
		std::vector<Quad> lines;
		for (const ItemRef &t : o.placeholders) {
			const Projector to_display(t, map);
			obs_source_t *s = obs_sceneitem_get_source(t.item());
			const Quad q = to_display.box({0, 0, double(obs_source_get_width(s)), double(obs_source_get_height(s))});
			outline(lines, q, px, 4 * px);
			placeholder_.draw((q.p[0].x + q.p[2].x) / 2 - placeholder_.width / 2.0f,
					  (q.p[0].y + q.p[2].y) / 2 - placeholder_.height / 2.0f);
		}
		fill(lines, placeholder_line);
	}

	if (o.editing && o.target) {
		const Projector to_display(o.target, map);
		// A frame of its own, clear of OBS's red box, blue edged in dark so
		// it shows on any background.
		const Quad frame = grown(to_display.box(o.box), 8 * px);
		float top = 1e9f, left = 1e9f, bottom = -1e9f, right = -1e9f;
		for (const vec2 &p : frame.p) {
			top = std::min(top, p.y);
			left = std::min(left, p.x);
			bottom = std::max(bottom, p.y);
			right = std::max(right, p.x);
		}

		// The rest of the canvas a little dimmer while editing (OBS's
		// guides with it), so it's plain which text has the keyboard.
		{
			obs_video_info ovi = {};
			obs_get_video_info(&ovi);
			const float cx0 = float(map.x), cy0 = float(map.y);
			const float cx1 = float(map.x + ovi.base_width * map.scale), cy1 = float(map.y + ovi.base_height * map.scale);
			const float t = std::clamp(top, cy0, cy1), b = std::clamp(bottom, cy0, cy1);
			const float l = std::clamp(left, cx0, cx1), r = std::clamp(right, cx0, cx1);
			fill({rect(cx0, cy0, cx1, t), rect(cx0, b, cx1, cy1), rect(cx0, t, l, b), rect(r, t, cx1, b)}, veil);
		}

		std::vector<Quad> edges, inner;
		outline(edges, frame, 6 * px);
		outline(inner, frame, 3 * px);
		fill(edges, accent_edge);
		fill(inner, accent);

		// One strip above the frame's left end (below it when there's no
		// room above, else at the preview's top): the format bar, then the
		// tag saying what's going on. Clear of OBS's distance labels, which
		// sit over the middle of the box's edges.
		{
			const float size = 28 * px, margin = 6 * px;
			const float strip_w = size * 8.6f + 300 * px; // the buttons and room for the tag
			float y = top - margin - size;
			if (y < 2)
				y = bottom + margin;
			if (y + size > o.view_h - 2)
				y = 2;
			const float x = std::clamp(left, 2.0f, std::max(2.0f, o.view_w - strip_w));
			std::vector<BarButton> buttons;
			paint_bar(o, x, y, px, &buttons);
			const float tag_x = buttons.empty() ? x : buttons.back().x + buttons.back().w + 10 * px;
			tag_.ensure(o.tag, 14 * px, 0xFFFFFFFF, tag_fill);
			tag_.draw(tag_x, y + (size - tag_.height) / 2);
			if (bar)
				*bar = std::move(buttons);
		}

		// The text box's width grip, on the frame's right edge.
		if (o.grip) {
			const vec2 a = to_display(o.box.w, o.box.h * 0.5);
			vec2 g0, g1;
			vec2_set(&g0, a.x + 7 * px, a.y - 12 * px);
			vec2_set(&g1, a.x + 7 * px, a.y + 12 * px);
			fill({line_quad(g0, g1, 9 * px)}, accent_edge);
			fill({line_quad(g0, g1, 6 * px)}, 0xFFFFFFFF);
			fill({line_quad(g0, g1, 3 * px)}, accent);
		}

		// The selection: a solid highlight with the selected text drawn
		// again on top, clipped to it, so it keeps its own colours. A
		// rotated source can't be clipped to a box, and on a background
		// plate the plate would be drawn again over the highlight: those
		// get a wash instead.
		const matrix4 &m = to_display.m;
		const bool upright = !o.plate && std::fabs(m.x.y) < 1e-4f && std::fabs(m.y.x) < 1e-4f && m.x.x > 0 &&
				     m.y.y > 0;
		std::vector<Quad> selection;
		for (const Box &b : o.selection)
			selection.push_back(to_display.box(b));
		fill(selection, upright ? selection_fill : selection_wash);
		// A keyline round the selection as one shape: the sides of each
		// line's piece, the top of the first line and the bottom of the last.
		std::vector<Quad> keylines;
		float sel_top = 1e9f, sel_bottom = -1e9f;
		for (const Quad &q : selection) {
			sel_top = std::min(sel_top, std::min(q.p[0].y, q.p[2].y));
			sel_bottom = std::max(sel_bottom, std::max(q.p[0].y, q.p[2].y));
		}
		for (const Quad &q : selection) {
			if (!upright) {
				outline(keylines, q, 1 * px);
				continue;
			}
			keylines.push_back(line_quad(q.p[1], q.p[2], 1 * px));
			keylines.push_back(line_quad(q.p[3], q.p[0], 1 * px));
			if (std::fabs(q.p[0].y - sel_top) < 0.5f)
				keylines.push_back(line_quad(q.p[0], q.p[1], 1 * px));
			if (std::fabs(q.p[2].y - sel_bottom) < 0.5f)
				keylines.push_back(line_quad(q.p[2], q.p[3], 1 * px));
		}
		fill(keylines, selection_keyline);
		if (upright && !selection.empty()) {
			TextSource *text = TextSource::from(obs_sceneitem_get_source(o.target.item()));
			matrix4 to_display_m;
			matrix4_identity(&to_display_m);
			to_display_m.x.x = float(map.scale);
			to_display_m.y.y = float(map.scale);
			to_display_m.t.x = float(map.x);
			to_display_m.t.y = float(map.y);
			matrix4_mul(&to_display_m, &m, &to_display_m);
			const bool flip = gs_get_device_type() == GS_DEVICE_OPENGL; // GL's scissor counts from the bottom
			for (const Quad &q : selection) {
				const int x0 = int(std::floor(std::min(q.p[0].x, q.p[2].x)));
				const int x1 = int(std::ceil(std::max(q.p[0].x, q.p[2].x)));
				const int y0 = int(std::floor(std::min(q.p[0].y, q.p[2].y)));
				const int y1 = int(std::ceil(std::max(q.p[0].y, q.p[2].y)));
				gs_rect r = {x0, flip ? int(cy) - y1 : y0, x1 - x0, y1 - y0};
				if (r.cx <= 0 || r.cy <= 0)
					continue;
				gs_set_scissor_rect(&r);
				gs_matrix_push();
				gs_matrix_set(&to_display_m);
				const bool drawn = text && text->draw_raster();
				gs_matrix_pop();
				if (!drawn) { // no raster of the text as it is: a wash over it instead
					gs_set_scissor_rect(nullptr);
					fill({q}, selection_wash);
				}
			}
			gs_set_scissor_rect(nullptr);
		}

		// A composition in progress, just above the caret.
		if (!o.preedit.empty()) {
			preedit_.ensure(o.preedit, 16 * px, 0xFFFFFFFF, accent);
			const vec2 a = to_display(o.caret.x, o.caret.y);
			preedit_.draw(a.x, a.y - preedit_.height - 2 * px);
		}

		// The caret blinks, and stays solid for a moment after each key.
		const uint64_t since = (os_gettime_ns() - o.blink_from) / 1000000ULL;
		if (o.show_caret && (since < caret_hold_ms || (since / caret_blink_ms) % 2 == 0)) {
			const vec2 a = to_display(o.caret.x, o.caret.y);
			const vec2 b = to_display(o.caret.x, o.caret.y + o.caret.height);
			// Blue in white in dark: never mistaken for a letter's stem.
			fill({line_quad(a, b, 7.0f * px)}, 0x000000B0);
			fill({line_quad(a, b, 5.0f * px)}, 0xFFFFFFFF);
			fill({line_quad(a, b, 3.0f * px)}, accent);
		}
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
	gs_matrix_pop();
}

} // namespace typeset
