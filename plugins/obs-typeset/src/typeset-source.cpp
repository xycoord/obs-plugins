#include "typeset-source.hpp"

#include "properties.hpp"
#include "settings.hpp"

#include <graphics/matrix4.h>
#include <obs-module.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace typeset {

const char *const source_id = "typeset_text";

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

namespace {

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
	if (!obs_data_has_user_value(settings, "styled")) {
		// The dialog's Defaults button cleared "styled": set again (from
		// the UI thread), or the default style would be applied to this
		// text as if it were new when the scene is next loaded.
		obs_queue_task(
			OBS_TASK_UI,
			[](void *param) {
				auto *weak = static_cast<obs_weak_source_t *>(param);
				if (obs_source_t *src = obs_weak_source_get_source(weak)) {
					obs_data_t *data = obs_data_create();
					obs_data_set_bool(data, "styled", true);
					obs_source_update(src, data);
					obs_data_release(data);
					obs_source_release(src);
				}
				obs_weak_source_release(weak);
			},
			obs_source_get_weak_source(source_), false);
	}
	if (follow) {
		struct Task {
			obs_weak_source_t *source;
			Spans spans;
			std::string text;
			Spans replacing; // what the settings held: if they've changed since, this is out of date
		};
		auto *task = new Task{obs_source_get_weak_source(source_), doc.spans, doc.text, read_spans(settings)};
		obs_queue_task(
			OBS_TASK_UI,
			[](void *param) {
				auto *t = static_cast<Task *>(param);
				if (obs_source_t *src = obs_weak_source_get_source(t->source)) {
					obs_data_t *settings = obs_source_get_settings(src);
					// Only if the text is still the one they were moved for,
					// and no newer change to the styles has been made.
					if (t->text == obs_data_get_string(settings, "text") &&
					    read_spans(settings) == t->replacing) {
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
	// A finished drawing for the output goes to the GPU now, whether or not
	// the text is on screen, so its pixels don't wait in memory until it's
	// shown (it's kept drawn for a cut to it). Once per change, so nothing
	// at rest.
	bool finished;
	{
		std::lock_guard lock(views_[0].slot->mutex);
		finished = views_[0].slot->result && views_[0].slot->result_seq > views_[0].texture_seq;
	}
	if (finished) {
		obs_enter_graphics();
		upload_result(views_[0]);
		obs_leave_graphics();
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
		want.sx = double(viewport.cx) / width();
		want.sy = double(viewport.cy) / height();
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

	// Still under content_mutex_ (see the header): OBS defers a video
	// source's update to its graphics thread, so this doesn't call update().

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

	// Still under content_mutex_ (see the header): OBS defers a video
	// source's update to its graphics thread, so this doesn't call update().

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

size_t TextSource::backspace_from(size_t index) const
{
	std::lock_guard lock(layout_lock());
	int n = 0;
	const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(layout_, &n);
	const long i = char_offset(doc_.text, index);
	if (i <= 0)
		return 0;
	// After a character made of several (a letter and its accent, an Indic
	// or Hangul syllable), Backspace takes off only the last of them, so a
	// mistyped accent can be typed again.
	if (i < n && attrs[i].backspace_deletes_character)
		return byte_index(doc_.text, i - 1);
	long j = i;
	do
		j--;
	while (j > 0 && !attrs[j].is_cursor_position);
	return byte_index(doc_.text, j);
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
	apply_default_style(settings);
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
	info.get_defaults = set_defaults;
	info.get_properties = source_properties;
	obs_register_source(&info);
}

} // namespace typeset
