#include "typeset-source.hpp"

#include <graphics/matrix4.h>
#include <obs-module.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace typeset {

const char *const source_id = "typeset_text";

namespace {

// Layout for sizes and editing happens on whichever thread OBS updates a
// source from (usually its window's), with a font map of its own: Pango's
// aren't thread-safe, so one lock covers all of it.
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

Style read_style(obs_data_t *settings)
{
	Style style;
	obs_data_t *font = obs_data_get_obj(settings, "font");
	if (font) {
		const char *face = obs_data_get_string(font, "face");
		const char *face_style = obs_data_get_string(font, "style");
		if (face && *face)
			style.family = face;
		style.face_style = face_style ? face_style : "";
		const long long size = obs_data_get_int(font, "size");
		if (size > 0)
			style.size = double(size);
		style.font_flags = uint32_t(obs_data_get_int(font, "flags"));
		obs_data_release(font);
	}
	style.colour = uint32_t(obs_data_get_int(settings, "colour"));
	style.align = Align(std::clamp<long long>(obs_data_get_int(settings, "align"), 0, 2));
	style.line_spacing = obs_data_get_double(settings, "line_spacing");
	return style;
}

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

} // namespace

TextSource::TextSource(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	slot_ = std::make_shared<RasterSlot>();
	update(settings);
}

TextSource::~TextSource()
{
	{
		std::lock_guard lock(layout_lock());
		if (layout_)
			g_object_unref(layout_);
	}
	if (texture_) {
		obs_enter_graphics();
		gs_texture_destroy(texture_);
		obs_leave_graphics();
	}
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
	Document doc;
	const char *text = obs_data_get_string(settings, "text");
	doc.text = text ? text : "";
	doc.style = read_style(settings);
	apply(std::move(doc));
}

void TextSource::apply(Document doc)
{
	{
		std::lock_guard lock(layout_lock());
		if (layout_ && doc == doc_)
			return;
		doc_ = doc;
		if (layout_)
			g_object_unref(layout_);
		layout_ = create_layout(layout_context(), doc_);

		// OBS takes a source's size in whole pixels. An empty text keeps
		// a little width, so it can still be found and clicked on.
		const Box logical = logical_box(layout_);
		double w = logical.w;
		if (doc_.text.empty())
			w = std::max(w, doc_.style.size * 0.3);
		width_ = uint32_t(std::max(1.0, std::ceil(w - 1e-3)));
		height_ = uint32_t(std::max(1.0, std::ceil(logical.h - 1e-3)));
	}

	std::lock_guard lock(slot_->mutex);
	pending_doc_ = std::move(doc);
	doc_version_++;
}

// --- Rendering ----------------------------------------------------------------

void TextSource::request(const RasterParams &params)
{
	RasterJob job;
	{
		std::lock_guard lock(slot_->mutex);
		job.doc = pending_doc_;
		requested_version_ = doc_version_;
	}
	job.params = params;
	job.seq = next_seq_++;
	requested_ = true;
	requested_params_ = params;
	worker_submit(slot_, std::move(job));
}

void TextSource::tick()
{
	// What the last frame drew the source at (the largest, if it was drawn
	// more than once), against what was last asked for.
	if (!want_)
		return;
	want_ = false;
	uint64_t version;
	{
		std::lock_guard lock(slot_->mutex);
		version = doc_version_;
	}
	if (!requested_ || version != requested_version_ || !wanted_.close_to(requested_params_))
		request(wanted_);
}

void TextSource::upload_result()
{
	std::optional<Raster> result;
	uint64_t seq;
	{
		std::lock_guard lock(slot_->mutex);
		if (!slot_->result || slot_->result_seq <= texture_seq_)
			return;
		result.swap(slot_->result);
		seq = slot_->result_seq;
	}
	texture_seq_ = seq;

	Raster &r = *result;
	if (r.width <= 0 || r.height <= 0 || r.pixels.empty()) {
		if (texture_) {
			gs_texture_destroy(texture_);
			texture_ = nullptr;
		}
		drawn_ = std::move(r);
		return;
	}
	if (texture_ && (int(gs_texture_get_width(texture_)) != r.width ||
			 int(gs_texture_get_height(texture_)) != r.height)) {
		gs_texture_destroy(texture_);
		texture_ = nullptr;
	}
	if (texture_) {
		gs_texture_set_image(texture_, r.pixels.data(), uint32_t(r.width) * 4, false);
	} else {
		const uint8_t *data = r.pixels.data();
		texture_ = gs_texture_create(uint32_t(r.width), uint32_t(r.height), GS_BGRA, 1, &data, GS_DYNAMIC);
	}
	r.pixels.clear();
	r.pixels.shrink_to_fit();
	drawn_ = std::move(r);
}

void TextSource::render()
{
	upload_result();

	// How the source is drawn on the canvas this time: its scale along
	// each of its axes, and where its origin falls within a canvas pixel.
	struct matrix4 m;
	gs_matrix_get(&m);
	RasterParams want;
	want.sx = std::hypot(double(m.x.x), double(m.x.y));
	want.sy = std::hypot(double(m.y.x), double(m.y.y));
	if (!(want.sx > 1e-4) || !(want.sy > 1e-4) || !std::isfinite(want.sx) || !std::isfinite(want.sy))
		return;
	// Unrotated and unmirrored, the raster's pixels can sit exactly on the
	// canvas's, so the texture is drawn without any resampling at all.
	const bool axis_aligned = std::fabs(m.x.y) < 1e-6f && std::fabs(m.y.x) < 1e-6f && m.x.x > 0 && m.y.y > 0;
	if (axis_aligned) {
		want.ox = double(m.t.x) - std::floor(double(m.t.x));
		want.oy = double(m.t.y) - std::floor(double(m.t.y));
	}
	if (!want_ || want.sx * want.sy > wanted_.sx * wanted_.sy) {
		wanted_ = want;
		want_ = true;
	}
	if (!requested_)
		request(want); // the first time: don't wait a frame

	if (!texture_)
		return;

	const RasterParams &p = drawn_.params;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_matrix_push();
	gs_matrix_translate3f(float((drawn_.x0 - p.ox) / p.sx), float((drawn_.y0 - p.oy) / p.sy), 0.0f);
	gs_matrix_scale3f(float(1.0 / p.sx), float(1.0 / p.sy), 1.0f);
	gs_effect_set_texture(image, texture_);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture_, 0, uint32_t(drawn_.width), uint32_t(drawn_.height));
	gs_matrix_pop();

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
}

// --- Editing ----------------------------------------------------------------------

std::string TextSource::text() const
{
	std::lock_guard lock(layout_lock());
	return doc_.text;
}

void TextSource::set_text(const std::string &text)
{
	// OBS applies an update to a video source on its next frame; an edit
	// has to be laid out now, for the caret. The update that follows finds
	// the text already as it is.
	Document doc;
	{
		std::lock_guard lock(layout_lock());
		doc = doc_;
	}
	doc.text = text;
	apply(std::move(doc));

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", text.c_str());
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
	pango_layout_xy_to_index(layout_, int(std::lround(x * PANGO_SCALE)), int(std::lround(y * PANGO_SCALE)), &index,
				 &trailing);
	return with_trailing(doc_.text, index, nearest_edge ? trailing : 0);
}

Caret TextSource::caret(size_t index) const
{
	std::lock_guard lock(layout_lock());
	PangoRectangle strong;
	pango_layout_get_cursor_pos(layout_, int(std::min(index, doc_.text.size())), &strong, nullptr);
	return {double(strong.x) / PANGO_SCALE, double(strong.y) / PANGO_SCALE, double(strong.height) / PANGO_SCALE};
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

	PangoLayoutIter *iter = pango_layout_get_iter(layout_);
	do {
		PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
		const size_t start = size_t(line->start_index), end = start + size_t(line->length);
		// A line's end counts as selected when the selection runs on past
		// it (over the line break): shown as a little extra width.
		const bool through_end = to > end && from <= end;
		if (to < start || from > end || (from == end && !through_end))
			continue;

		PangoRectangle extents;
		pango_layout_iter_get_line_extents(iter, nullptr, &extents);
		const double y = double(extents.y) / PANGO_SCALE, h = double(extents.height) / PANGO_SCALE;
		const double line_x = double(extents.x) / PANGO_SCALE;

		int *ranges = nullptr, n = 0;
		pango_layout_line_get_x_ranges(line, int(std::max(from, start)), int(std::min(to, end)), &ranges, &n);
		for (int i = 0; i < n; i++) {
			const double x0 = line_x + double(ranges[2 * i]) / PANGO_SCALE;
			const double x1 = line_x + double(ranges[2 * i + 1]) / PANGO_SCALE;
			if (x1 > x0)
				boxes.push_back({x0, y, x1 - x0, h});
		}
		g_free(ranges);
		if (through_end) {
			const double x = line_x + double(extents.width) / PANGO_SCALE;
			boxes.push_back({x, y, doc_.style.size * 0.25, h});
		}
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
	PangoRectangle strong;
	pango_layout_get_cursor_pos(layout_, int(std::min(index, doc_.text.size())), &strong, nullptr);
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
	pango_layout_xy_to_index(layout_, int(std::lround(x * PANGO_SCALE)), (y0 + y1) / 2, &new_index, &trailing);
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
	// The end of a line that wraps is before its trailing space, so the
	// caret stays on it.
	size_t end = size_t(l->start_index + l->length);
	const bool wrapped = line + 1 < pango_layout_get_line_count(layout_) && end > 0 &&
			     doc_.text[end - 1] != '\n' && end < doc_.text.size() && doc_.text[end] != '\n';
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

// --- Registration ---------------------------------------------------------------------

namespace {

const char *get_name(void *)
{
	return obs_module_text("TypesetText");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
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
	obs_data_set_default_int(settings, "colour", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "align", int(Align::Left));
	obs_data_set_default_double(settings, "line_spacing", 1.0);
}

obs_properties_t *get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "text", obs_module_text("Text"), OBS_TEXT_MULTILINE);
	obs_property_t *p = obs_properties_add_text(props, "edit_hint", obs_module_text("EditHint"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(p, true);
	obs_properties_add_font(props, "font", obs_module_text("Font"));
	obs_properties_add_color_alpha(props, "colour", obs_module_text("Colour"));
	p = obs_properties_add_list(props, "align", obs_module_text("Align"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Align.Left"), int(Align::Left));
	obs_property_list_add_int(p, obs_module_text("Align.Centre"), int(Align::Centre));
	obs_property_list_add_int(p, obs_module_text("Align.Right"), int(Align::Right));
	p = obs_properties_add_float_slider(props, "line_spacing", obs_module_text("LineSpacing"), 0.5, 3.0, 0.05);
	obs_property_float_set_suffix(p, "×");
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
