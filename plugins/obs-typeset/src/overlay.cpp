#include "overlay.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace typeset {

namespace {

// Colours of the editing marks, 0xRRGGBBAA, besides the theme's highlight
// (the accent): a dark edge round it, so it shows on any background.
constexpr uint32_t accent_edge = 0x0A1830D0;
constexpr uint32_t selection_keyline = 0xFFFFFFC0;
constexpr uint32_t veil = 0x00000040; // over the rest of the canvas while editing
// Half the side of OBS's selection handles, in display pixels at a device
// pixel ratio of 1, as OBS draws them.
constexpr float handle_half = 4.0f;
// The selection: the accent nearly opaque (the selected glyphs are drawn
// again on it), or a wash of it where they can't be (a rotated source).
uint32_t with_alpha(uint32_t rgba, uint32_t a)
{
	return (rgba & 0xFFFFFF00u) | a;
}
constexpr uint32_t placeholder_line = 0xFFFFFFB0;

// How long the caret stays solid after a key or click, and its blink.
constexpr uint64_t caret_hold_ms = 500, caret_blink_ms = 530;

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

using Poly = std::vector<vec2>;

// A convex region less a convex quad (either way round), as convex pieces:
// each is the region outside one of the quad's edges and inside the edges
// before it, so they never overlap and a translucent fill is even.
std::vector<Poly> minus(const Poly &region, const Quad &q)
{
	float area = 0;
	for (int i = 0; i < 4; i++) {
		const vec2 &a = q.p[i], &b = q.p[(i + 1) % 4];
		area += a.x * b.y - b.x * a.y;
	}
	if (std::fabs(area) < 1e-3f)
		return {region};
	const float sign = area > 0 ? 1.0f : -1.0f;
	auto clip = [&](const Poly &poly, int i, bool inside) {
		const vec2 &a = q.p[i], &b = q.p[(i + 1) % 4];
		auto side = [&](const vec2 &p) {
			const float v = sign * ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x));
			return inside ? v : -v;
		};
		Poly out;
		for (size_t k = 0; k < poly.size(); k++) {
			const vec2 &p = poly[k], &n = poly[(k + 1) % poly.size()];
			const float sp = side(p), sn = side(n);
			if (sp >= 0)
				out.push_back(p);
			if ((sp >= 0) != (sn >= 0)) {
				vec2 x;
				const float t = sp / (sp - sn);
				vec2_set(&x, p.x + (n.x - p.x) * t, p.y + (n.y - p.y) * t);
				out.push_back(x);
			}
		}
		return out;
	};
	std::vector<Poly> pieces;
	for (int i = 0; i < 4; i++) {
		Poly poly = clip(region, i, false);
		for (int j = 0; j < i && poly.size() >= 3; j++)
			poly = clip(poly, j, true);
		if (poly.size() >= 3)
			pieces.push_back(std::move(poly));
	}
	return pieces;
}

// The canvas less an item's box and OBS's handles on it, as quads to fill.
// The edge is exactly OBS's outline (a rotated box too); the handles, which
// straddle it, are left clear whole.
std::vector<Quad> outside(const Quad &box, const Quad &canvas, float handle)
{
	std::vector<Quad> cuts{box};
	vec2 u, v; // OBS draws its handles upright, the box rotated or not
	vec2_set(&u, handle, 0);
	vec2_set(&v, 0, handle);
	for (int i = 0; i < 4; i++) {
		const vec2 &a = box.p[i], &b = box.p[(i + 1) % 4];
		vec2 mid;
		vec2_set(&mid, (a.x + b.x) / 2, (a.y + b.y) / 2);
		for (const vec2 &c : {a, mid}) {
			Quad sq;
			vec2_set(&sq.p[0], c.x - u.x - v.x, c.y - u.y - v.y);
			vec2_set(&sq.p[1], c.x + u.x - v.x, c.y + u.y - v.y);
			vec2_set(&sq.p[2], c.x + u.x + v.x, c.y + u.y + v.y);
			vec2_set(&sq.p[3], c.x - u.x + v.x, c.y - u.y + v.y);
			cuts.push_back(sq);
		}
	}
	auto bounds_meet = [](const Poly &p, const Quad &q) {
		float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, qx0 = 1e9f, qy0 = 1e9f, qx1 = -1e9f, qy1 = -1e9f;
		for (const vec2 &c : p) {
			x0 = std::min(x0, c.x), y0 = std::min(y0, c.y), x1 = std::max(x1, c.x), y1 = std::max(y1, c.y);
		}
		for (const vec2 &c : q.p) {
			qx0 = std::min(qx0, c.x), qy0 = std::min(qy0, c.y), qx1 = std::max(qx1, c.x), qy1 = std::max(qy1, c.y);
		}
		return x0 < qx1 && qx0 < x1 && y0 < qy1 && qy0 < y1;
	};
	std::vector<Poly> pieces{Poly(canvas.p, canvas.p + 4)};
	for (const Quad &cut : cuts) {
		std::vector<Poly> next;
		for (const Poly &p : pieces) {
			if (!bounds_meet(p, cut)) {
				next.push_back(p);
				continue;
			}
			for (Poly &r : minus(p, cut))
				next.push_back(std::move(r));
		}
		pieces = std::move(next);
	}
	std::vector<Quad> quads;
	for (const Poly &poly : pieces)
		for (size_t k = 1; k + 1 < poly.size(); k++) // a fan of triangles
			quads.push_back({{poly[0], poly[k], poly[k + 1], poly[k + 1]}});
	return quads;
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

// Made with a font map of the UI thread's own; kept while in use, so the
// same label (a glyph, the tag between keys) isn't laid out again.
Label make_label(const std::string &text, double px, uint32_t fg, uint32_t bg)
{
	char key_extra[64];
	snprintf(key_extra, sizeof(key_extra), "|%g|%08x|%08x", px, fg, bg);
	const std::string key = text + key_extra;
	static std::vector<std::weak_ptr<const LabelImage>> made;
	for (auto it = made.begin(); it != made.end();) {
		if (Label l = it->lock()) {
			if (l->key == key)
				return l;
			++it;
		} else {
			it = made.erase(it);
		}
	}

	static PangoContext *context = create_context(pango_cairo_font_map_new());
	auto image = std::make_shared<LabelImage>();
	image->key = key;
	PangoLayout *layout = pango_layout_new(context);
	PangoFontDescription *desc = pango_font_description_from_string("Sans Bold");
	pango_font_description_set_absolute_size(desc, px * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text.c_str(), -1);
	int tw = 0, th = 0;
	pango_layout_get_pixel_size(layout, &tw, &th);
	const int pad = int(px * 0.45);
	const int width = tw + 2 * pad, height = th + pad;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
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
		show_layout(cr, layout);
		cairo_destroy(cr);
		cairo_surface_flush(surface);
		const int stride = cairo_image_surface_get_stride(surface);
		const uint8_t *data = cairo_image_surface_get_data(surface);
		image->width = width;
		image->height = height;
		image->pixels.resize(size_t(width) * height * 4);
		for (int y = 0; y < height; y++)
			memcpy(&image->pixels[size_t(y) * width * 4], data + size_t(y) * stride, size_t(width) * 4);
	}
	cairo_surface_destroy(surface);
	g_object_unref(layout);
	made.push_back(image);
	return image;
}

float pixel_size(const Mapping &m)
{
	return float(std::max(1.0, std::round(m.dpr)));
}

void make_labels(Overlay &o, const std::string &preedit)
{
	o.preedit = preedit.empty() ? nullptr : make_label(preedit, 16 * pixel_size(o.mapping), 0xFFFFFFFF, o.accent);
}

Label placeholder_label(const Mapping &mapping)
{
	return make_label(obs_module_text("Placeholder"), 13 * pixel_size(mapping), 0xFFFFFFFF, 0x000000A0);
}

bool OverlayPainter::Texture::upload(const Label &label)
{
	if (!label || label->pixels.empty())
		return false;
	if (label->key == key && texture)
		return true;
	if (label->key == failed)
		return false; // not tried every frame
	const uint8_t *data = label->pixels.data();
	gs_texture_t *made = gs_texture_create(uint32_t(label->width), uint32_t(label->height), GS_BGRA, 1, &data, 0);
	if (!made) {
		failed = label->key;
		return texture != nullptr;
	}
	destroy();
	texture = made;
	width = label->width;
	height = label->height;
	key = label->key;
	return true;
}

void OverlayPainter::Texture::draw(float x, float y) const
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

void OverlayPainter::Texture::destroy()
{
	if (texture)
		gs_texture_destroy(texture);
	texture = nullptr;
	key.clear();
}

void OverlayPainter::destroy()
{
	for (Texture *t : {&placeholder_, &preedit_})
		t->destroy();
}

// --- Painting ---------------------------------------------------------------------------------

void OverlayPainter::paint(const Overlay &o, uint32_t, uint32_t cy)
{
	const uint32_t accent = o.accent;
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
		const bool words = placeholder_.upload(o.placeholder);
		std::vector<Quad> lines;
		for (const ItemRef &t : o.placeholders) {
			const Projector to_display(t, map);
			obs_source_t *s = obs_sceneitem_get_source(t.item());
			const Quad q = to_display.box({0, 0, double(obs_source_get_width(s)), double(obs_source_get_height(s))});
			outline(lines, q, px, 4 * px);
			if (words)
				placeholder_.draw((q.p[0].x + q.p[2].x) / 2 - placeholder_.width / 2.0f,
						  (q.p[0].y + q.p[2].y) / 2 - placeholder_.height / 2.0f);
		}
		fill(lines, placeholder_line);
	}

	if (o.editing && o.target) {
		const Projector to_display(o.target, map);
		// No frame of its own: OBS's selection box is the text's one outline
		// (the text stays selected while it's edited). The rest of the canvas
		// goes a little dimmer (OBS's guides with it), so it's plain which
		// text has the keyboard; the edge of the dimming is OBS's outline.
		{
			obs_video_info ovi = {};
			obs_get_video_info(&ovi);
			const Quad canvas = rect(float(map.x), float(map.y), float(map.x + ovi.base_width * map.scale),
						 float(map.y + ovi.base_height * map.scale));
			fill(outside(to_display.box(o.box), canvas, handle_half * float(map.dpr)), veil);
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
		fill(selection, with_alpha(accent, upright ? 0xE6 : 0x70));
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
					fill({q}, with_alpha(accent, 0x70));
				}
			}
			gs_set_scissor_rect(nullptr);
		}

		// A composition in progress, just above the caret.
		if (o.preedit && preedit_.upload(o.preedit)) {
			const vec2 a = to_display(o.caret.x, o.caret.y);
			preedit_.draw(a.x, a.y - preedit_.height - 2 * px);
		}

		// The caret blinks, and stays solid for a moment after each key.
		const uint64_t since = (os_gettime_ns() - o.blink_from) / 1000000ULL;
		if (o.show_caret && (since < caret_hold_ms || (since / caret_blink_ms) % 2 == 0)) {
			const vec2 a = to_display(o.caret.x, o.caret.y);
			const vec2 b = to_display(o.caret.x, o.caret.y + o.caret.height);
			// A solid white bar edged in dark: plain on any background, light
			// or dark, at any size the preview shows the text.
			fill({line_quad(a, b, 6.0f * px)}, 0x000000C8);
			fill({line_quad(a, b, 3.0f * px)}, 0xFFFFFFFF);
		}
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
	gs_matrix_pop();
}

} // namespace typeset
