#include "typeset-source.hpp"

#include <graphics/matrix4.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace typeset {

const char *const source_id = "typeset_text";

namespace {

// Text is laid out (for its size, and for editing) on whichever thread OBS
// updates a source from: its window's, or its graphics thread, where OBS
// applies changes to video sources. Pango's font maps aren't thread-safe, so
// that layout has a font map of its own and one lock covers all of it. The
// rasteriser has its own font map on its own thread.
std::mutex &layout_lock()
{
	static std::mutex mutex;
	return mutex;
}

PangoContext *layout_context()
{
	static PangoContext *context = create_context(pango_cairo_font_map_new());
	return context;
}

// --- Settings ---------------------------------------------------------------------------

// The style's settings, with their types, for copying a style from one
// source to another and saving it as the default.
enum class Kind { Bool, Int, Double, Object };
struct Key {
	const char *name;
	Kind kind;
};
const Key style_keys[] = {
	{"font", Kind::Object},          {"colour", Kind::Int},           {"align", Kind::Int},
	{"box", Kind::Bool},             {"box_width", Kind::Int},        {"box_height", Kind::Int},
	{"overflow", Kind::Int},         {"valign", Kind::Int},           {"typography", Kind::Bool},
	{"line_spacing", Kind::Double},  {"letter_spacing", Kind::Int},   {"text_case", Kind::Int},
	{"tabular", Kind::Bool},         {"outline", Kind::Bool},         {"outline_colour", Kind::Int},
	{"outline_width", Kind::Int},    {"shadow", Kind::Bool},          {"shadow_colour", Kind::Int},
	{"shadow_opacity", Kind::Int},   {"shadow_x", Kind::Int},         {"shadow_y", Kind::Int},
	{"shadow_blur", Kind::Int},      {"plate", Kind::Bool},           {"plate_colour", Kind::Int},
	{"plate_opacity", Kind::Int},    {"plate_padding", Kind::Int},    {"plate_radius", Kind::Int},
};
constexpr const char *style_marker = "typeset_style";

// A colour (its own alpha ignored) at an opacity in percent.
uint32_t with_opacity(long long colour, long long percent)
{
	const uint32_t a = uint32_t(std::clamp<long long>(percent, 0, 100) * 255 / 100);
	return (uint32_t(colour) & 0xFFFFFF) | (a << 24);
}

Style read_style(obs_data_t *settings)
{
	Style s;
	obs_data_t *font = obs_data_get_obj(settings, "font");
	if (font) {
		const char *face = obs_data_get_string(font, "face");
		const char *face_style = obs_data_get_string(font, "style");
		if (face && *face)
			s.family = face;
		s.face_style = face_style ? face_style : "";
		const long long size = obs_data_get_int(font, "size");
		if (size > 0)
			s.size = double(std::min<long long>(size, 4096));
		s.font_flags = uint32_t(obs_data_get_int(font, "flags"));
		obs_data_release(font);
	}
	s.colour = uint32_t(obs_data_get_int(settings, "colour"));
	s.align = Align(std::clamp<long long>(obs_data_get_int(settings, "align"), 0, 2));

	// Every value kept within what the dialog allows, whatever set it (a
	// pasted style, a script).
	s.box = obs_data_get_bool(settings, "box");
	s.box_width = double(std::clamp<long long>(obs_data_get_int(settings, "box_width"), 0, 16384));
	s.box_height = double(std::clamp<long long>(obs_data_get_int(settings, "box_height"), 0, 16384));
	s.overflow = Overflow(std::clamp<long long>(obs_data_get_int(settings, "overflow"), 0, 1));
	s.valign = VAlign(std::clamp<long long>(obs_data_get_int(settings, "valign"), 0, 2));

	// Turned off, a group's settings don't apply, whatever they hold.
	if (obs_data_get_bool(settings, "typography")) {
		s.line_spacing = std::clamp(obs_data_get_double(settings, "line_spacing"), 0.5, 3.0);
		s.letter_spacing = std::clamp(obs_data_get_double(settings, "letter_spacing"), -20.0, 100.0);
		s.text_case = Case(std::clamp<long long>(obs_data_get_int(settings, "text_case"), 0, 3));
		s.tabular = obs_data_get_bool(settings, "tabular");
	}
	s.outline = obs_data_get_bool(settings, "outline");
	s.outline_colour = uint32_t(obs_data_get_int(settings, "outline_colour"));
	s.outline_width = std::clamp(obs_data_get_double(settings, "outline_width"), 0.0, 64.0);
	s.shadow = obs_data_get_bool(settings, "shadow");
	s.shadow_colour = with_opacity(obs_data_get_int(settings, "shadow_colour"),
				       obs_data_get_int(settings, "shadow_opacity"));
	s.shadow_x = std::clamp(obs_data_get_double(settings, "shadow_x"), -200.0, 200.0);
	s.shadow_y = std::clamp(obs_data_get_double(settings, "shadow_y"), -200.0, 200.0);
	s.shadow_blur = std::clamp(obs_data_get_double(settings, "shadow_blur"), 0.0, 200.0);
	s.plate = obs_data_get_bool(settings, "plate");
	s.plate_colour = with_opacity(obs_data_get_int(settings, "plate_colour"),
				      obs_data_get_int(settings, "plate_opacity"));
	s.plate_padding = std::clamp(obs_data_get_double(settings, "plate_padding"), 0.0, 400.0);
	s.plate_radius = std::clamp(obs_data_get_double(settings, "plate_radius"), 0.0, 400.0);
	return s;
}

// A source's style, every value as it stands (defaults included).
obs_data_t *style_of(obs_data_t *settings)
{
	obs_data_t *out = obs_data_create();
	obs_data_set_bool(out, style_marker, true);
	for (const Key &k : style_keys) {
		switch (k.kind) {
		case Kind::Bool:
			obs_data_set_bool(out, k.name, obs_data_get_bool(settings, k.name));
			break;
		case Kind::Int:
			obs_data_set_int(out, k.name, obs_data_get_int(settings, k.name));
			break;
		case Kind::Double:
			obs_data_set_double(out, k.name, obs_data_get_double(settings, k.name));
			break;
		case Kind::Object: {
			obs_data_t *o = obs_data_get_obj(settings, k.name);
			obs_data_set_obj(out, k.name, o);
			obs_data_release(o);
			break;
		}
		}
	}
	return out;
}

// Only the style's own settings from data that may hold anything (pasted).
obs_data_t *only_style(obs_data_t *data)
{
	obs_data_t *out = obs_data_create();
	for (const Key &k : style_keys) {
		if (!obs_data_has_user_value(data, k.name))
			continue;
		obs_data_item_t *item = obs_data_item_byname(data, k.name);
		const obs_data_type type = obs_data_item_gettype(item);
		const bool right_type = k.kind == Kind::Object ? type == OBS_DATA_OBJECT
					: k.kind == Kind::Bool ? type == OBS_DATA_BOOLEAN
							       : type == OBS_DATA_NUMBER;
		obs_data_item_release(&item);
		if (!right_type)
			continue;
		switch (k.kind) {
		case Kind::Bool:
			obs_data_set_bool(out, k.name, obs_data_get_bool(data, k.name));
			break;
		case Kind::Int:
			obs_data_set_int(out, k.name, obs_data_get_int(data, k.name));
			break;
		case Kind::Double:
			obs_data_set_double(out, k.name, obs_data_get_double(data, k.name));
			break;
		case Kind::Object: {
			obs_data_t *o = obs_data_get_obj(data, k.name);
			obs_data_set_obj(out, k.name, o);
			obs_data_release(o);
			break;
		}
		}
	}
	return out;
}

Spans read_spans(obs_data_t *settings)
{
	Spans spans;
	obs_data_array_t *array = obs_data_get_array(settings, "spans");
	const size_t n = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *o = obs_data_array_item(array, i);
		Span sp;
		sp.start = size_t(std::max<long long>(0, obs_data_get_int(o, "start")));
		sp.end = size_t(std::max<long long>(0, obs_data_get_int(o, "end")));
		sp.bold = obs_data_get_bool(o, "bold") ? 1 : -1;
		sp.italic = obs_data_get_bool(o, "italic") ? 1 : -1;
		sp.underline = obs_data_get_bool(o, "underline") ? 1 : -1;
		sp.has_colour = obs_data_has_user_value(o, "colour");
		sp.colour = uint32_t(obs_data_get_int(o, "colour"));
		sp.size = int(std::clamp<long long>(obs_data_get_int(o, "size"), 0, 1000));
		spans.push_back(sp);
		obs_data_release(o);
	}
	obs_data_array_release(array);
	return spans;
}

obs_data_array_t *spans_array(const Spans &spans)
{
	obs_data_array_t *array = obs_data_array_create();
	for (const Span &sp : spans) {
		obs_data_t *o = obs_data_create();
		obs_data_set_int(o, "start", (long long)sp.start);
		obs_data_set_int(o, "end", (long long)sp.end);
		if (sp.bold > 0)
			obs_data_set_bool(o, "bold", true);
		if (sp.italic > 0)
			obs_data_set_bool(o, "italic", true);
		if (sp.underline > 0)
			obs_data_set_bool(o, "underline", true);
		if (sp.has_colour)
			obs_data_set_int(o, "colour", sp.colour);
		if (sp.size)
			obs_data_set_int(o, "size", sp.size);
		obs_data_array_push_back(array, o);
		obs_data_release(o);
	}
	return array;
}

std::string default_style_path()
{
	char *path = obs_module_config_path("default-style.json");
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

// --- Text indices --------------------------------------------------------------------------

// Character offsets and byte indices, for Pango's per-character attributes.
long char_offset(const std::string &text, size_t index)
{
	index = std::min(index, text.size());
	return g_utf8_pointer_to_offset(text.c_str(), text.c_str() + index);
}

size_t byte_index(const std::string &text, long offset)
{
	return size_t(g_utf8_offset_to_pointer(text.c_str(), offset) - text.c_str());
}

// Pango reports positions as an index plus how many characters on from it
// ("trailing"), for the far side of a grapheme.
size_t with_trailing(const std::string &text, int index, int trailing)
{
	const char *p = text.c_str() + std::clamp<int>(index, 0, int(text.size()));
	const char *end = text.c_str() + text.size();
	while (trailing-- > 0 && p < end)
		p = g_utf8_next_char(p);
	return size_t(std::min(p, end) - text.c_str());
}

// Pango units for a position, clamped so a pointer far outside a tiny text
// can't overflow them.
int pango_units(double v)
{
	return int(std::lround(std::clamp(v, -1e6, 1e6) * PANGO_SCALE));
}

} // namespace

TextSource::TextSource(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	update(settings);
}

TextSource::~TextSource()
{
	{
		std::lock_guard lock(layout_lock());
		if (layout_)
			g_object_unref(layout_);
	}
	obs_enter_graphics();
	for (View &v : views_)
		release(v);
	obs_leave_graphics();
}

TextSource *TextSource::from(obs_source_t *source)
{
	if (!source)
		return nullptr;
	const char *id = obs_source_get_unversioned_id(source);
	if (!id || strcmp(id, source_id) != 0)
		return nullptr;
	return static_cast<TextSource *>(obs_obj_get_data(source));
}

// The size field mirrors the font's size (which is what counts). Set when a
// source is made and when its font is picked (never from OBS's graphics
// thread, where updates run, as the dialog may be using the settings).
void sync_size(obs_data_t *settings)
{
	obs_data_t *font = obs_data_get_obj(settings, "font");
	const long long size = font ? obs_data_get_int(font, "size") : 0;
	obs_data_release(font);
	if (size > 0 && (!obs_data_has_user_value(settings, "size") || obs_data_get_int(settings, "size") != size))
		obs_data_set_int(settings, "size", size);
}

void TextSource::update(obs_data_t *settings)
{
	std::lock_guard content(content_mutex_);
	Document doc;
	const char *text = obs_data_get_string(settings, "text");
	// Made valid UTF-8, which editing's index arithmetic relies on.
	gchar *valid = g_utf8_make_valid(text ? text : "", -1);
	doc.text = valid;
	g_free(valid);
	doc.style = read_style(settings);
	doc.spans = read_spans(settings);

	// The text changed from outside (its properties) and its runs didn't:
	// they move with the text, and are written back from OBS's UI thread
	// (this may be its graphics thread, which mustn't write the settings).
	bool follow = false;
	{
		std::lock_guard lock(layout_lock());
		// (Until a move is written back, the settings still hold the runs
		// from before it: those count as unchanged too, and the runs as
		// moved are kept.)
		const bool stale = !superseded_.empty() && doc.spans == superseded_;
		const bool runs_unchanged = doc.spans == doc_.spans || stale;
		if (layout_ && doc.text != doc_.text && runs_unchanged && !doc_.spans.empty()) {
			superseded_ = doc.spans; // what the settings hold until this move is written back
			doc.spans = doc_.spans;
			follow_text(doc.spans, doc_.text, doc.text);
			follow = true;
		} else if (stale) {
			// Still the old runs in the settings: keep the current ones,
			// and make sure they're written back.
			doc.spans = doc_.spans;
			follow = true;
		} else {
			superseded_.clear();
		}
	}
	normalise(doc.spans, doc.text);
	if (follow) {
		struct Task {
			obs_weak_source_t *source;
			Spans spans;
			std::string text;
		};
		auto *task = new Task{obs_source_get_weak_source(source_), doc.spans, doc.text};
		obs_queue_task(
			OBS_TASK_UI,
			[](void *param) {
				auto *t = static_cast<Task *>(param);
				if (obs_source_t *src = obs_weak_source_get_source(t->source)) {
					obs_data_t *settings = obs_source_get_settings(src);
					// Only if the text is still the one they were moved for.
					if (t->text == obs_data_get_string(settings, "text")) {
						obs_data_t *data = obs_data_create();
						obs_data_array_t *array = spans_array(t->spans);
						obs_data_set_array(data, "spans", array);
						obs_data_array_release(array);
						obs_source_update(src, data);
						obs_data_release(data);
					}
					obs_data_release(settings);
					obs_source_release(src);
				}
				obs_weak_source_release(t->source);
				delete t;
			},
			task, false);
	}
	apply(std::move(doc));
}

void TextSource::apply(Document doc)
{
	std::lock_guard lock(layout_lock());
	// An edit in the preview has been applied already; the update OBS then
	// makes from the settings finds the same document and stops here.
	if (layout_ && doc == doc_)
		return;
	doc_ = doc;
	if (layout_)
		g_object_unref(layout_);
	layout_ = create_layout(layout_context(), doc_);
	frame_ = frame_of(layout_, doc_);
	// OBS takes a source's size in whole pixels.
	width_ = uint32_t(std::max(1.0, std::ceil(frame_.width - 1e-3)));
	height_ = uint32_t(std::max(1.0, std::ceil(frame_.height - 1e-3)));

	// Published for the rasteriser in the same order as laid out.
	std::lock_guard doc_lock(doc_mutex_);
	pending_doc_ = std::move(doc);
	doc_version_++;
}

// --- Rendering ----------------------------------------------------------------------------

void TextSource::request(View &view, const RasterParams &params)
{
	RasterJob job;
	{
		std::lock_guard lock(doc_mutex_);
		job.doc = pending_doc_;
		view.requested_version = doc_version_;
		job.version = doc_version_;
	}
	job.params = params;
	job.seq = next_seq_++;
	view.requested = true;
	view.requested_params = params;
	worker_submit(view.slot, std::move(job));
}

void TextSource::release(View &view)
{
	if (view.texture)
		gs_texture_destroy(view.texture);
	view.texture = nullptr;
	view.requested = false;
	view.texture_seq = 0;
}

void TextSource::tick()
{
	uint64_t version;
	{
		std::lock_guard lock(doc_mutex_);
		version = doc_version_;
	}
	for (int i = 0; i < 2; i++) {
		View &v = views_[i];
		if (v.retry_ticks > 0) {
			v.retry_ticks--;
			v.want = false;
			continue;
		}
		// What the last frame drew the source at (the largest, if it was
		// drawn more than once), against what was last asked for.
		if (v.want) {
			v.want = false;
			v.idle_ticks = 0;
			if (!v.requested || version != v.requested_version || !v.wanted.close_to(v.requested_params))
				request(v, v.wanted);
		} else if (i == 0 && (!v.requested || version != v.requested_version)) {
			// Not shown now, but it can be cut to at any moment: kept
			// drawn for the output, at the scale it was last shown at
			// (at first, its own size), so it's never blank on a cut.
			request(v, v.requested ? v.requested_params : RasterParams());
		} else if (i == 1 && v.texture && ++v.idle_ticks > 120) {
			// No window has shown it for a while: free its raster.
			obs_enter_graphics();
			release(v);
			obs_leave_graphics();
		}
	}
}

void TextSource::upload_result(View &view)
{
	std::optional<Raster> result;
	uint64_t seq;
	{
		std::lock_guard lock(view.slot->mutex);
		if (!view.slot->result || view.slot->result_seq <= view.texture_seq)
			return;
		result.swap(view.slot->result);
		seq = view.slot->result_seq;
	}
	view.texture_seq = seq;

	Raster &r = *result;
	if (r.failed) {
		// Keep what's shown; ask again in a few seconds.
		view.requested = false;
		view.retry_ticks = 180;
		return;
	}
	if (r.width <= 0 || r.height <= 0 || r.pixels.empty()) {
		if (view.texture)
			gs_texture_destroy(view.texture);
		view.texture = nullptr;
		view.drawn = std::move(r);
		return;
	}
	if (view.texture && (int(gs_texture_get_width(view.texture)) != r.width ||
			     int(gs_texture_get_height(view.texture)) != r.height)) {
		gs_texture_destroy(view.texture);
		view.texture = nullptr;
	}
	if (view.texture) {
		gs_texture_set_image(view.texture, r.pixels.data(), uint32_t(r.width) * 4, false);
	} else {
		const uint8_t *data = r.pixels.data();
		view.texture =
			gs_texture_create(uint32_t(r.width), uint32_t(r.height), GS_BGRA, 1, &data, GS_DYNAMIC);
		if (!view.texture)
			blog(LOG_WARNING, "[obs-typeset] '%s': couldn't make a %dx%d texture",
			     obs_source_get_name(source_), r.width, r.height);
	}
	r.pixels = std::vector<uint8_t>();
	view.drawn = std::move(r);
}

void TextSource::render()
{
	// Into the output, or into a window (OBS draws windows straight to their
	// swap chain, with no render target set).
	const bool window = gs_get_render_target() == nullptr;
	View &view = views_[window ? 1 : 0];
	upload_result(view);

	// How the source is drawn this time: its scale along each of its axes,
	// and where its origin falls within a pixel.
	struct matrix4 m;
	gs_matrix_get(&m);
	RasterParams want;
	want.sx = std::hypot(double(m.x.x), double(m.x.y));
	want.sy = std::hypot(double(m.y.x), double(m.y.y));
	const bool axis_aligned = std::fabs(m.x.y) < 1e-6f && std::fabs(m.y.x) < 1e-6f && m.x.x > 0 && m.y.y > 0;
	gs_rect viewport = {};
	if (window)
		gs_get_viewport(&viewport);
	const bool same_aspect = viewport.cx > 0 && viewport.cy > 0 && width() > 0 && height() > 0 &&
				 std::fabs(double(viewport.cx) / viewport.cy - double(width()) / height()) <
					 0.02 * double(width()) / height();
	if (window && axis_aligned && same_aspect && std::fabs(want.sx - 1) < 1e-4 && std::fabs(want.sy - 1) < 1e-4 &&
	    std::fabs(m.t.x) < 1e-3f && std::fabs(m.t.y) < 1e-3f) {
		// The source on its own, fitted to a window (the properties
		// dialog's preview, a source projector): OBS projects its own
		// size onto a viewport of the same shape, so the viewport says
		// how big it is. (A scene item at the origin, unscaled, in the
		// Studio Mode preview has the same matrix, but the canvas's
		// shape.)
		{
			want.sx = double(viewport.cx) / width();
			want.sy = double(viewport.cy) / height();
		}
	} else if (!window && axis_aligned) {
		// Unrotated and unmirrored, the raster's pixels can sit exactly on
		// the canvas's, so the texture is drawn with no resampling at all.
		want.ox = double(m.t.x) - std::floor(double(m.t.x));
		want.oy = double(m.t.y) - std::floor(double(m.t.y));
	}
	if (!(want.sx > 1e-4) || !(want.sy > 1e-4) || !std::isfinite(want.sx) || !std::isfinite(want.sy))
		return;
	if (!view.want) {
		view.wanted = want;
		view.want = true;
	} else {
		// Drawn more than once this frame: big enough for the largest.
		view.wanted.sx = std::max(view.wanted.sx, want.sx);
		view.wanted.sy = std::max(view.wanted.sy, want.sy);
	}
	if (!view.requested && view.retry_ticks == 0)
		request(view, want); // the first time: don't wait a frame

	draw(view);
}

void TextSource::draw(View &view)
{
	if (!view.texture)
		return;
	const RasterParams &p = view.drawn.params;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_matrix_push();
	gs_matrix_translate3f(float((view.drawn.x0 - p.ox) / p.sx), float((view.drawn.y0 - p.oy) / p.sy), 0.0f);
	gs_matrix_scale3f(float(1.0 / p.sx), float(1.0 / p.sy), 1.0f);
	gs_effect_set_texture(image, view.texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(view.texture, 0, uint32_t(view.drawn.width), uint32_t(view.drawn.height));
	gs_matrix_pop();

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
}

bool TextSource::draw_raster()
{
	// The output's raster, or the window's (the Studio Mode preview) if
	// that one shows the text as it is now and the output's doesn't.
	uint64_t version;
	{
		std::lock_guard lock(doc_mutex_);
		version = doc_version_;
	}
	for (View &v : views_) {
		if (v.texture && v.drawn.version == version) {
			draw(v);
			return true;
		}
	}
	return false;
}

// --- Editing --------------------------------------------------------------------------------

std::string TextSource::text() const
{
	std::lock_guard lock(layout_lock());
	return doc_.text;
}

Document TextSource::document() const
{
	std::lock_guard lock(layout_lock());
	return doc_;
}


void TextSource::set_box_width(int width)
{
	std::lock_guard content(content_mutex_);
	Document doc;
	{
		std::lock_guard lock(layout_lock());
		doc = doc_;
	}
	width = std::clamp(width, 1, 16384);
	doc.style.box_width = width;
	apply(std::move(doc));

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "box_width", width);
	obs_source_update(source_, settings);
	obs_data_release(settings);
}

Frame TextSource::frame() const
{
	std::lock_guard lock(layout_lock());
	return frame_;
}

void TextSource::set_content(const std::string &text, const Spans &spans)
{
	std::lock_guard content(content_mutex_);
	Document doc;
	{
		std::lock_guard lock(layout_lock());
		doc = doc_;
	}
	doc.text = text;
	doc.spans = spans;
	normalise(doc.spans, doc.text);
	const Spans normalised = doc.spans;
	apply(std::move(doc));

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", text.c_str());
	obs_data_array_t *array = spans_array(normalised);
	obs_data_set_array(settings, "spans", array);
	obs_data_array_release(array);
	obs_source_update(source_, settings);
	obs_data_release(settings);
}

Box TextSource::box() const
{
	return {0, 0, double(width_.load()), double(height_.load())};
}

size_t TextSource::hit_test(double x, double y, bool nearest_edge) const
{
	std::lock_guard lock(layout_lock());
	int index = 0, trailing = 0;
	pango_layout_xy_to_index(layout_, pango_units(to_layout_x(x)), pango_units(to_layout_y(y)), &index, &trailing);
	return with_trailing(doc_.text, index, nearest_edge ? trailing : 0);
}

Caret TextSource::caret(size_t index) const
{
	std::lock_guard lock(layout_lock());
	PangoRectangle strong;
	pango_layout_get_cursor_pos(layout_, int(std::min(index, doc_.text.size())), &strong, nullptr);
	return {to_source_x(double(strong.x) / PANGO_SCALE), to_source_y(double(strong.y) / PANGO_SCALE),
		double(strong.height) / PANGO_SCALE * frame_.text_scale};
}

std::vector<Box> TextSource::selection_boxes(size_t from, size_t to) const
{
	std::vector<Box> boxes;
	if (from > to)
		std::swap(from, to);
	std::lock_guard lock(layout_lock());
	to = std::min(to, doc_.text.size());
	if (from >= to)
		return boxes;
	const double s = frame_.text_scale;
	auto add = [&](double x0, double y, double x1, double h) {
		if (x1 > x0)
			boxes.push_back({to_source_x(x0), to_source_y(y), (x1 - x0) * s, h * s});
	};

	PangoLayoutIter *iter = pango_layout_get_iter(layout_);
	do {
		PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
		const size_t start = size_t(line->start_index), end = start + size_t(line->length);
		// A line's end counts as selected when the selection runs on past
		// it (over the line break): shown as a narrow mark after the line.
		const bool through_end = to > end && from <= end;
		if (to < start || from > end || (from == end && !through_end))
			continue;

		PangoRectangle extents;
		pango_layout_iter_get_line_extents(iter, nullptr, &extents);
		const double y = double(extents.y) / PANGO_SCALE, h = double(extents.height) / PANGO_SCALE;
		const double line_x0 = double(extents.x) / PANGO_SCALE;
		const double line_x1 = line_x0 + double(extents.width) / PANGO_SCALE;

		// Pango's ranges are in layout coordinates already; a range that
		// runs to the line's end reaches the layout's edge, so it's kept
		// to the line itself.
		int *ranges = nullptr, n = 0;
		pango_layout_line_get_x_ranges(line, int(std::max(from, start)), int(std::min(to, end)), &ranges, &n);
		for (int i = 0; i < n; i++) {
			const double x0 = std::max(line_x0, double(ranges[2 * i]) / PANGO_SCALE);
			const double x1 = std::min(line_x1, double(ranges[2 * i + 1]) / PANGO_SCALE);
			add(x0, y, x1, h);
		}
		g_free(ranges);
		if (through_end)
			add(line_x1, y, line_x1 + doc_.style.size * 0.12, h);
	} while (pango_layout_iter_next_line(iter));
	pango_layout_iter_free(iter);
	return boxes;
}

size_t TextSource::move_visually(size_t index, int direction) const
{
	std::lock_guard lock(layout_lock());
	int new_index = 0, trailing = 0;
	pango_layout_move_cursor_visually(layout_, TRUE, int(std::min(index, doc_.text.size())), 0, direction,
					  &new_index, &trailing);
	if (new_index < 0)
		return 0;
	if (new_index >= G_MAXINT)
		return doc_.text.size();
	return with_trailing(doc_.text, new_index, trailing);
}

size_t TextSource::next_cursor(size_t index, int direction) const
{
	std::lock_guard lock(layout_lock());
	int n = 0;
	const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(layout_, &n);
	long i = char_offset(doc_.text, index);
	do
		i += direction > 0 ? 1 : -1;
	while (i > 0 && i < n - 1 && !attrs[i].is_cursor_position);
	return byte_index(doc_.text, std::clamp<long>(i, 0, n - 1));
}

size_t TextSource::cursor_at_or_before(size_t index) const
{
	std::lock_guard lock(layout_lock());
	const std::string &t = doc_.text;
	index = std::min(index, t.size());
	// Back to the start of a UTF-8 character, then of a grapheme.
	while (index > 0 && index < t.size() && (uint8_t(t[index]) & 0xC0) == 0x80)
		index--;
	int n = 0;
	const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(layout_, &n);
	long i = char_offset(t, index);
	while (i > 0 && i < n && !attrs[i].is_cursor_position)
		i--;
	return byte_index(t, std::clamp<long>(i, 0, std::max(n - 1, 0)));
}

size_t TextSource::move_by_word(size_t index, int direction) const
{
	std::lock_guard lock(layout_lock());
	int n = 0;
	const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(layout_, &n);
	long i = char_offset(doc_.text, index);
	if (direction > 0) {
		do
			i++;
		while (i < n - 1 && !attrs[i].is_word_end);
	} else {
		do
			i--;
		while (i > 0 && !attrs[i].is_word_start);
	}
	return byte_index(doc_.text, std::clamp<long>(i, 0, n - 1));
}

size_t TextSource::move_by_line(size_t index, int direction, double x) const
{
	std::lock_guard lock(layout_lock());
	int line = 0, x_pos = 0;
	pango_layout_index_to_line_x(layout_, int(std::min(index, doc_.text.size())), FALSE, &line, &x_pos);
	const int lines = pango_layout_get_line_count(layout_);
	if (direction < 0 && line == 0)
		return 0;
	if (direction > 0 && line >= lines - 1)
		return doc_.text.size();

	// Into the middle of the line above or below, at the same column.
	PangoLayoutIter *iter = pango_layout_get_iter(layout_);
	for (int i = 0; i < line + direction; i++)
		pango_layout_iter_next_line(iter);
	int y0 = 0, y1 = 0;
	pango_layout_iter_get_line_yrange(iter, &y0, &y1);
	pango_layout_iter_free(iter);

	int new_index = 0, trailing = 0;
	pango_layout_xy_to_index(layout_, pango_units(to_layout_x(x)), (y0 + y1) / 2, &new_index, &trailing);
	return with_trailing(doc_.text, new_index, trailing);
}

size_t TextSource::line_edge(size_t index, int direction) const
{
	std::lock_guard lock(layout_lock());
	int line = 0, x_pos = 0;
	pango_layout_index_to_line_x(layout_, int(std::min(index, doc_.text.size())), FALSE, &line, &x_pos);
	PangoLayoutLine *l = pango_layout_get_line_readonly(layout_, line);
	if (!l)
		return index;
	if (direction < 0)
		return size_t(l->start_index);
	// The end of a line that wraps is before the space it wraps at, so the
	// caret stays on that line.
	// (A line's length leaves out the line break that ends it, if any.)
	size_t end = size_t(l->start_index + l->length);
	const bool wrapped = line + 1 < pango_layout_get_line_count(layout_) && end > 0 && end < doc_.text.size() &&
			     doc_.text[end] != '\n';
	if (wrapped && doc_.text[end - 1] == ' ')
		end--;
	return end;
}

std::pair<size_t, size_t> TextSource::word_at(size_t index) const
{
	std::lock_guard lock(layout_lock());
	int n = 0;
	const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(layout_, &n);
	const long i = char_offset(doc_.text, index);
	// The nearest word boundary at or before the character: if it's a
	// word's start, the character is in that word.
	long start = i;
	while (start > 0 && !attrs[start].is_word_start && !attrs[start].is_word_end)
		start--;
	long end = i + 1;
	if (i < n - 1 && attrs[start].is_word_start) {
		while (end < n - 1 && !attrs[end].is_word_end)
			end++;
	} else {
		// A space or punctuation: just that grapheme.
		start = i;
		while (end < n - 1 && !attrs[end].is_cursor_position)
			end++;
	}
	return {byte_index(doc_.text, std::clamp<long>(start, 0, n - 1)),
		byte_index(doc_.text, std::clamp<long>(end, 0, n - 1))};
}

std::pair<size_t, size_t> TextSource::paragraph_at(size_t index) const
{
	std::lock_guard lock(layout_lock());
	const std::string &t = doc_.text;
	index = std::min(index, t.size());
	const size_t start = index == 0 ? 0 : t.rfind('\n', index - 1);
	const size_t end = t.find('\n', index);
	return {start == std::string::npos || index == 0 ? 0 : start + 1, end == std::string::npos ? t.size() : end};
}

// --- Registration -------------------------------------------------------------------------

namespace {

const char *get_name(void *)
{
	return obs_module_text("TypesetText");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	// A source just added takes the style saved as the default, if any.
	// "styled" marks every source once it has had the chance, so a saved
	// scene isn't restyled when it's loaded again.
	if (!obs_data_get_bool(settings, "styled")) {
		const std::string path = default_style_path();
		obs_data_t *saved = path.empty() ? nullptr : obs_data_create_from_json_file(path.c_str());
		if (saved) {
			// Only what the source wasn't given (by a script, say).
			obs_data_t *style = only_style(saved);
			for (obs_data_item_t *item = obs_data_first(style); item; obs_data_item_next(&item)) {
				const char *name = obs_data_item_get_name(item);
				if (obs_data_has_user_value(settings, name))
					continue;
				obs_data_item_t *copy = obs_data_item_byname(style, name);
				switch (obs_data_item_gettype(copy)) {
				case OBS_DATA_BOOLEAN:
					obs_data_set_bool(settings, name, obs_data_item_get_bool(copy));
					break;
				case OBS_DATA_NUMBER:
					if (obs_data_item_numtype(copy) == OBS_DATA_NUM_INT)
						obs_data_set_int(settings, name, obs_data_item_get_int(copy));
					else
						obs_data_set_double(settings, name, obs_data_item_get_double(copy));
					break;
				case OBS_DATA_OBJECT: {
					obs_data_t *o = obs_data_item_get_obj(copy);
					obs_data_set_obj(settings, name, o);
					obs_data_release(o);
					break;
				}
				default:
					break;
				}
				obs_data_item_release(&copy);
			}
			obs_data_release(style);
			obs_data_release(saved);
		}
		obs_data_set_bool(settings, "styled", true);
	}
	sync_size(settings);
	return new TextSource(settings, source);
}

void destroy(void *data)
{
	delete static_cast<TextSource *>(data);
}

void update(void *data, obs_data_t *settings)
{
	static_cast<TextSource *>(data)->update(settings);
}

uint32_t get_width(void *data)
{
	return static_cast<TextSource *>(data)->width();
}

uint32_t get_height(void *data)
{
	return static_cast<TextSource *>(data)->height();
}

void video_tick(void *data, float)
{
	static_cast<TextSource *>(data)->tick();
}

void video_render(void *data, gs_effect_t *)
{
	static_cast<TextSource *>(data)->render();
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "text", obs_module_text("DefaultText"));
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", "Sans");
	obs_data_set_string(font, "style", "Regular");
	obs_data_set_int(font, "size", 96);
	obs_data_set_int(font, "flags", 0);
	obs_data_set_default_obj(settings, "font", font);
	obs_data_release(font);
	obs_data_set_default_int(settings, "size", 96);
	obs_data_set_default_int(settings, "colour", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "align", int(Align::Left));

	obs_data_set_default_bool(settings, "box", false);
	obs_data_set_default_int(settings, "box_width", 0);
	obs_data_set_default_int(settings, "box_height", 0);
	obs_data_set_default_int(settings, "overflow", int(Overflow::Wrap));
	obs_data_set_default_int(settings, "valign", int(VAlign::Top));

	obs_data_set_default_bool(settings, "typography", false);
	obs_data_set_default_double(settings, "line_spacing", 1.0);
	obs_data_set_default_int(settings, "letter_spacing", 0);
	obs_data_set_default_int(settings, "text_case", int(Case::AsTyped));
	obs_data_set_default_bool(settings, "tabular", false);

	// Over projector visuals and video, text needs something to stand out
	// from them: a shadow is on from the start, close and fairly dense, so
	// it holds up thin and light text too.
	obs_data_set_default_bool(settings, "outline", false);
	obs_data_set_default_int(settings, "outline_colour", 0xFF000000);
	obs_data_set_default_int(settings, "outline_width", 4);
	obs_data_set_default_bool(settings, "shadow", true);
	obs_data_set_default_int(settings, "shadow_colour", 0xFF000000);
	obs_data_set_default_int(settings, "shadow_opacity", 75);
	obs_data_set_default_int(settings, "shadow_x", 0);
	obs_data_set_default_int(settings, "shadow_y", 3);
	obs_data_set_default_int(settings, "shadow_blur", 6);
	obs_data_set_default_bool(settings, "plate", false);
	obs_data_set_default_int(settings, "plate_colour", 0xFF000000);
	obs_data_set_default_int(settings, "plate_opacity", 75);
	obs_data_set_default_int(settings, "plate_padding", 24);
	obs_data_set_default_int(settings, "plate_radius", 8);
}

// A checkable group holds its settings only while it's ticked, so a feature
// not in use takes one line in the dialog. OBS calls these when the dialog
// opens (before it builds it) and when a group is ticked (then rebuilds it).
void fill_box(obs_properties_t *g, obs_data_t *settings)
{
	obs_property_t *p = obs_properties_add_int(g, "box_width", obs_module_text("Box.Width"), 1, 16384, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(g, "box_height", obs_module_text("Box.Height"), 0, 16384, 1);
	obs_property_int_set_suffix(p, " px");
	obs_property_set_long_description(p, obs_module_text("Box.Height.Help"));
	obs_property_set_modified_callback(p, [](obs_properties_t *props, obs_property_t *, obs_data_t *s) {
		obs_property_t *v = obs_properties_get(props, "valign");
		const bool show = obs_data_get_int(s, "box_height") > 0;
		if (!v || obs_property_visible(v) == show)
			return false;
		obs_property_set_visible(v, show);
		return true;
	});
	p = obs_properties_add_list(g, "overflow", obs_module_text("Box.Overflow"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Box.Overflow.Wrap"), int(Overflow::Wrap));
	obs_property_list_add_int(p, obs_module_text("Box.Overflow.Shrink"), int(Overflow::Shrink));
	p = obs_properties_add_list(g, "valign", obs_module_text("Box.VAlign"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Box.VAlign.Top"), int(VAlign::Top));
	obs_property_list_add_int(p, obs_module_text("Box.VAlign.Middle"), int(VAlign::Middle));
	obs_property_list_add_int(p, obs_module_text("Box.VAlign.Bottom"), int(VAlign::Bottom));
	obs_property_set_visible(p, settings && obs_data_get_int(settings, "box_height") > 0);
}

obs_property_t *px_slider(obs_properties_t *g, const char *name, const char *label, int min, int max)
{
	obs_property_t *p = obs_properties_add_int_slider(g, name, obs_module_text(label), min, max, 1);
	obs_property_int_set_suffix(p, " px");
	return p;
}

obs_property_t *percent_slider(obs_properties_t *g, const char *name, const char *label, int min, int max)
{
	obs_property_t *p = obs_properties_add_int_slider(g, name, obs_module_text(label), min, max, 1);
	obs_property_int_set_suffix(p, "%");
	return p;
}

void fill_plate(obs_properties_t *g, obs_data_t *)
{
	obs_properties_add_color(g, "plate_colour", obs_module_text("Colour"));
	percent_slider(g, "plate_opacity", "Opacity", 0, 100);
	px_slider(g, "plate_padding", "Plate.Padding", 0, 400);
	px_slider(g, "plate_radius", "Plate.Radius", 0, 400);
}

void fill_outline(obs_properties_t *g, obs_data_t *)
{
	obs_properties_add_color_alpha(g, "outline_colour", obs_module_text("Colour"));
	px_slider(g, "outline_width", "Outline.Width", 1, 64);
}

void fill_shadow(obs_properties_t *g, obs_data_t *)
{
	obs_properties_add_color(g, "shadow_colour", obs_module_text("Colour"));
	percent_slider(g, "shadow_opacity", "Opacity", 0, 100);
	px_slider(g, "shadow_x", "Shadow.X", -200, 200);
	px_slider(g, "shadow_y", "Shadow.Y", -200, 200);
	px_slider(g, "shadow_blur", "Shadow.Blur", 0, 200);
}

void fill_typography(obs_properties_t *g, obs_data_t *)
{
	obs_property_t *p =
		obs_properties_add_float_slider(g, "line_spacing", obs_module_text("LineSpacing"), 0.5, 3.0, 0.05);
	obs_property_float_set_suffix(p, "×");
	p = percent_slider(g, "letter_spacing", "LetterSpacing", -20, 100);
	obs_property_set_long_description(p, obs_module_text("LetterSpacing.Help"));
	p = obs_properties_add_list(g, "text_case", obs_module_text("Case"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Case.AsTyped"), int(Case::AsTyped));
	obs_property_list_add_int(p, obs_module_text("Case.Upper"), int(Case::Upper));
	obs_property_list_add_int(p, obs_module_text("Case.Lower"), int(Case::Lower));
	obs_property_list_add_int(p, obs_module_text("Case.Title"), int(Case::Title));
	p = obs_properties_add_bool(g, "tabular", obs_module_text("Tabular"));
	obs_property_set_long_description(p, obs_module_text("Tabular.Help"));
}

struct Group {
	const char *name, *label, *help, *about;
	void (*fill)(obs_properties_t *, obs_data_t *);
};
// In the order a venue's jobs use them most.
const Group groups[] = {
	{"plate", "Plate", nullptr, "Plate.About", fill_plate},
	{"shadow", "Shadow", "Shadow.Help", "Shadow.About", fill_shadow},
	{"outline", "Outline", nullptr, "Outline.About", fill_outline},
	{"box", "Box", "Box.Help", "Box.About", fill_box},
	{"typography", "Typography", nullptr, "Typography.About", fill_typography},
};

// Each feature is a checkable group (OBS's own: the switch is the box's
// title). Off, it holds one line saying what it does; on, its settings.
// (The council compared this with checkbox rows that reveal a box: this
// read clearest, with on/off and its settings plainly one thing.) OBS calls
// this when the dialog opens, before building it, and when a group is
// ticked, then rebuilds it.
bool group_toggled(obs_properties_t *props, obs_property_t *group, obs_data_t *settings)
{
	const char *name = obs_property_name(group);
	const bool on = obs_data_get_bool(settings, name);
	obs_properties_t *content = obs_property_group_content(group);
	const std::string about = std::string(name) + "_about";
	const bool showing_about = obs_properties_get(content, about.c_str()) != nullptr;
	if (obs_properties_first(content) && on != showing_about)
		return false; // already as it should be

	std::vector<std::string> names;
	for (obs_property_t *p = obs_properties_first(content); p; obs_property_next(&p))
		names.push_back(obs_property_name(p));
	for (const std::string &n : names)
		obs_properties_remove_by_name(content, n.c_str());

	const Group *g = nullptr;
	for (const Group &each : groups)
		if (strcmp(each.name, name) == 0)
			g = &each;
	if (!g)
		return true;
	if (on) {
		// A box starts as wide as the text is now, so ticking it changes
		// nothing until its width is changed.
		if (strcmp(name, "box") == 0 && obs_data_get_int(settings, "box_width") <= 0) {
			auto *text = static_cast<TextSource *>(obs_properties_get_param(props));
			if (text) {
				const Document doc = text->document();
				const double pad = doc.style.plate ? doc.style.plate_padding : 0;
				obs_data_set_int(settings, "box_width",
						 std::max(1LL, (long long)std::lround(text->width() - 2 * pad)));
			}
		}
		g->fill(content, settings);
	} else {
		obs_property_t *p = obs_properties_add_text(content, about.c_str(), obs_module_text(g->about), OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(p, true);
	}
	return true;
}


// The font picker holds the size, but the size is what's changed most, so it
// has a field of its own too; the two are kept the same.
bool font_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool refresh = false;
	obs_data_t *font = obs_data_get_obj(settings, "font");
	const long long size = font ? obs_data_get_int(font, "size") : 0;
	const char *face = font ? obs_data_get_string(font, "face") : "";
	const std::string family = face && *face ? face : "Sans";
	obs_data_release(font);
	if (size > 0 && size != obs_data_get_int(settings, "size")) {
		obs_data_set_int(settings, "size", size);
		refresh = true;
	}

	// Warn when the font isn't installed (a scene collection from
	// another computer), and say what's shown instead.
	if (obs_property_t *warning = obs_properties_get(props, "font_missing")) {
		std::string used;
		bool installed;
		{
			std::lock_guard lock(layout_lock());
			installed = font_installed(layout_context(), family, &used);
		}
		const std::string message = installed ? std::string()
						      : std::string(obs_module_text("FontMissing.1")) + " \u201c" +
								family + "\u201d " + obs_module_text("FontMissing.2") +
								" \u201c" + used + "\u201d.";
		if (obs_property_visible(warning) != !installed ||
		    message != (obs_property_description(warning) ? obs_property_description(warning) : "")) {
			obs_property_set_description(warning, message.c_str());
			obs_property_set_visible(warning, !installed);
			refresh = true;
		}
	}
	return refresh;
}

bool size_changed(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const long long size = obs_data_get_int(settings, "size");
	obs_data_t *font = obs_data_get_obj(settings, "font");
	if (font && size > 0 && obs_data_get_int(font, "size") != size) {
		obs_data_t *copy = obs_data_create();
		obs_data_apply(copy, font);
		obs_data_set_int(copy, "size", size);
		obs_data_set_obj(settings, "font", copy);
		obs_data_release(copy);
	}
	obs_data_release(font);
	return false;
}

// The clipboard is Qt's, usable only on its UI thread (obs-websocket can
// press a button from another).
bool on_ui_thread()
{
	return qApp && QThread::currentThread() == qApp->thread();
}

bool copy_style(obs_properties_t *, obs_property_t *, void *data)
{
	auto *text = static_cast<TextSource *>(data);
	if (!text || !on_ui_thread())
		return false;
	obs_data_t *settings = obs_source_get_settings(text->source());
	obs_data_t *style = style_of(settings);
	QGuiApplication::clipboard()->setText(QString::fromUtf8(obs_data_get_json_pretty(style)));
	obs_data_release(style);
	obs_data_release(settings);
	return false;
}


bool paste_style(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *text = static_cast<TextSource *>(data);
	if (!text || !on_ui_thread())
		return false;
	const QByteArray json = QGuiApplication::clipboard()->text().toUtf8();
	obs_data_t *pasted = json.isEmpty() ? nullptr : obs_data_create_from_json(json.constData());
	if (!pasted || !obs_data_get_bool(pasted, style_marker)) {
		obs_data_release(pasted);
		return false;
	}
	obs_data_t *style = only_style(pasted);
	obs_source_update(text->source(), style);
	obs_data_release(style);
	obs_data_release(pasted);
	// OBS rebuilds the dialog without asking the groups again: they're
	// filled (or emptied) for the pasted style here.
	obs_data_t *settings = obs_source_get_settings(text->source());
	for (const Group &g : groups)
		if (obs_property_t *p = obs_properties_get(props, g.name))
			group_toggled(props, p, settings);
	obs_data_release(settings);
	return true;
}

bool save_default_style(obs_properties_t *, obs_property_t *, void *data)
{
	auto *text = static_cast<TextSource *>(data);
	if (!text)
		return false;
	const std::string path = default_style_path();
	if (path.empty())
		return false;
	char *dir = obs_module_config_path("");
	if (dir)
		os_mkdirs(dir);
	bfree(dir);
	obs_data_t *settings = obs_source_get_settings(text->source());
	obs_data_t *style = style_of(settings);
	if (!obs_data_save_json_safe(style, path.c_str(), "tmp", "bak"))
		blog(LOG_WARNING, "[obs-typeset] couldn't save the default style to %s", path.c_str());
	obs_data_release(style);
	obs_data_release(settings);
	return false;
}

obs_properties_t *get_properties(void *data)
{
	auto *text = static_cast<TextSource *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, text, nullptr);
	obs_property_t *p;

	obs_properties_add_text(props, "text", obs_module_text("Text"), OBS_TEXT_MULTILINE);
	p = obs_properties_add_text(props, "edit_hint", obs_module_text("EditHint"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(p, true);
	p = obs_properties_add_font(props, "font", obs_module_text("Font"));
	obs_property_set_modified_callback(p, font_changed);
	p = obs_properties_add_int_slider(props, "size", obs_module_text("Size"), 4, 4096, 1);
	obs_property_int_set_suffix(p, " px");
	obs_property_set_long_description(p, obs_module_text("Size.Help"));
	obs_property_set_modified_callback(p, size_changed);
	obs_properties_add_color_alpha(props, "colour", obs_module_text("Colour"));
	p = obs_properties_add_list(props, "align", obs_module_text("Align"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Align.Left"), int(Align::Left));
	obs_property_list_add_int(p, obs_module_text("Align.Centre"), int(Align::Centre));
	obs_property_list_add_int(p, obs_module_text("Align.Right"), int(Align::Right));

	// Shown only when the font isn't on this computer (see font_changed).
	p = obs_properties_add_text(props, "font_missing", "", OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);
	obs_property_text_set_info_word_wrap(p, true);
	obs_property_set_visible(p, false);

	// Only when some of the text has a style of its own (set while editing
	// in the preview).
	const bool styled = text && !text->document().spans.empty();
	p = obs_properties_add_text(props, "word_styles", obs_module_text("WordStyles"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(p, true);
	obs_property_set_visible(p, styled);
	p = obs_properties_add_button2(
		props, "clear_word_styles", obs_module_text("ClearWordStyles"),
		[](obs_properties_t *props, obs_property_t *, void *data) {
			auto *t = static_cast<TextSource *>(data);
			if (!t)
				return false;
			t->set_content(t->text(), {});
			obs_property_set_visible(obs_properties_get(props, "word_styles"), false);
			obs_property_set_visible(obs_properties_get(props, "clear_word_styles"), false);
			return true;
		},
		text);
	obs_property_set_visible(p, styled);

	for (const Group &g : groups) {
		p = obs_properties_add_group(props, g.name, obs_module_text(g.label), OBS_GROUP_CHECKABLE,
					     obs_properties_create());
		if (g.help)
			obs_property_set_long_description(p, obs_module_text(g.help));
		obs_property_set_modified_callback(p, group_toggled);
	}

	obs_properties_t *style = obs_properties_create();
	obs_properties_add_button2(style, "copy_style", obs_module_text("CopyStyle"), copy_style, text);
	obs_properties_add_button2(style, "paste_style", obs_module_text("PasteStyle"), paste_style, text);
	obs_properties_add_button2(style, "default_style", obs_module_text("DefaultStyle"), save_default_style, text);
	p = obs_properties_add_group(props, "style", obs_module_text("Style"), OBS_GROUP_NORMAL, style);
	obs_property_set_long_description(p, obs_module_text("Style.Help"));

	return props;
}

} // namespace

void register_source()
{
	struct obs_source_info info = {};
	info.id = source_id;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	info.get_name = get_name;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_width = get_width;
	info.get_height = get_height;
	info.video_tick = video_tick;
	info.video_render = video_render;
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	obs_register_source(&info);
}

} // namespace typeset
