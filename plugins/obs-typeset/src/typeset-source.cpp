#include "typeset-source.hpp"

#include "properties.hpp"
#include "settings.hpp"

#include <graphics/matrix4.h>
#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace typeset {

const char *const source_id = "typeset_text";

// (See the header.)
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

std::thread::id ui_thread;
std::atomic<bool> ui_thread_known{false};

bool on_ui_thread()
{
	return !ui_thread_known.load() || std::this_thread::get_id() == ui_thread;
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

// Set while an edit from the preview writes its settings (under
// content_mutex_): libobs defers a video source's update to its graphics
// thread, but if it ever called update() straight from obs_source_update,
// update() would find this set and stop, instead of waiting on the lock this
// thread holds. The edit has been laid out already, so there's nothing to do.
thread_local bool writing_edit = false;

struct WritingEdit {
	WritingEdit() { writing_edit = true; }
	~WritingEdit() { writing_edit = false; }
};

} // namespace

void set_ui_thread()
{
	ui_thread = std::this_thread::get_id();
	ui_thread_known = true;
}

TextSource::TextSource(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	// Laid out at once, on whichever thread it's made on, so it has a
	// layout and a size from the start.
	update_now(settings);
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
	if (point_sampler_)
		gs_samplerstate_destroy(point_sampler_);
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
	if (writing_edit)
		return; // called from inside an edit's own write (see writing_edit)
	if (!on_ui_thread()) {
		queue_update();
		return;
	}
	update_now(settings);
}

// Passed to the UI thread, which reads the settings as they are when it
// gets there: updates queued meanwhile come to one, and none applies
// settings older than another's.
void TextSource::queue_update()
{
	if (update_queued_.exchange(true))
		return; // one is on its way
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			auto *weak = static_cast<obs_weak_source_t *>(param);
			if (obs_source_t *src = obs_weak_source_get_source(weak)) {
				if (TextSource *text = TextSource::from(src)) {
					text->update_queued_ = false; // before reading: a later change queues again
					obs_data_t *settings = obs_source_get_settings(src);
					text->update_now(settings);
					obs_data_release(settings);
				}
				obs_source_release(src);
			}
			obs_weak_source_release(weak);
		},
		obs_source_get_weak_source(source_), false);
}

void TextSource::update_now(obs_data_t *settings)
{
	if (writing_edit)
		return;
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
	// they move with the text, and are written back, from a task of their
	// own (not from inside an update).
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
	laid_out_ns_ = os_gettime_ns();

	// Published for the rasteriser in the same order as laid out.
	std::lock_guard doc_lock(doc_mutex_);
	pending_doc_ = std::move(doc);
	size_ = uint64_t(width_.load()) << 32 | height_.load();
	pending_width_ = width_;
	pending_height_ = height_;
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
		job.source_width = pending_width_;
		job.source_height = pending_height_;
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
	if (view.parts)
		gs_vertexbuffer_destroy(view.parts);
	view.parts = nullptr;
	view.part_vertices = 0;
	view.requested = false;
	// Nothing asked for so far is wanted now: a drawing waiting to be
	// uploaded is dropped, and one still being drawn is dropped when it's
	// done, so neither sits in memory (nor shows the next time).
	const uint64_t asked = next_seq_ - 1;
	{
		std::lock_guard lock(view.slot->mutex);
		view.slot->result.reset();
		view.slot->job.reset();
		view.slot->discard_through = asked;
	}
	view.texture_seq = std::max(view.texture_seq, asked);
}

void TextSource::retry_later(View &view, const char *why, uint64_t version)
{
	// Each failure in a row waits twice as long, from 3 s to a minute (or
	// until the text changes: see tick()), and is logged once for each
	// version of the text.
	constexpr double first_wait = 3, longest_wait = 60; // seconds
	view.requested = false;
	view.retry_wait = std::min(first_wait * double(1 << std::min(view.failures, 5)), longest_wait);
	view.failures++;
	if (view.failure_logged != version) {
		view.failure_logged = version;
		blog(LOG_WARNING, "[obs-typeset] '%s': couldn't draw the text (%s); showing what was drawn before "
				  "and trying again shortly",
		     obs_source_get_name(source_), why && *why ? why : "unknown");
	}
}

uint64_t TextSource::size_for_obs() const
{
	constexpr uint64_t longest_ns = 100'000'000; // a slow or failing drawing can't hold the old size longer
	const uint64_t s = shown_size_.load();
	if (s && !live_size_ && os_gettime_ns() - laid_out_ns_.load() < longest_ns)
		return s;
	return size_.load();
}

uint32_t TextSource::width() const
{
	return uint32_t(size_for_obs() >> 32);
}

uint32_t TextSource::height() const
{
	return uint32_t(size_for_obs());
}

void TextSource::tick(float seconds)
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
		if (v.retry_wait > 0 && version != v.requested_version) {
			// Changed since the drawing that failed (a smaller size, an
			// effect turned off): drawn again at once.
			v.retry_wait = 0;
			v.failures = 0;
		}
		if (v.retry_wait > 0) {
			v.retry_wait -= seconds;
			v.want = false;
			continue;
		}
		// What the last frame drew the source at (the largest, if it was
		// drawn more than once), against what was last asked for.
		if (v.want) {
			v.want = false;
			v.idle = 0;
			const bool moved = !v.wanted.close_to(v.last_wanted);
			v.last_wanted = v.wanted;
			v.still = moved ? 0 : v.still + seconds;
			if (!v.requested || version != v.requested_version || !v.wanted.close_to(v.requested_params)) {
				// Moving or zooming (dragged, a move transition, an
				// animated scale): the drawing slides and stretches, as
				// any image does, while it's near enough the size it was
				// drawn for (a little soft, which motion hides), and is
				// drawn again for exactly where and how big it is once
				// it's been still a moment. Chasing every frame would draw
				// it every frame, each a frame late, sharp and soft by
				// turns. A changed text is drawn at once.
				const double gx = v.wanted.sx / v.requested_params.sx, gy = v.wanted.sy / v.requested_params.sy;
				const bool near = v.requested && version == v.requested_version && gx <= 1.03 &&
						  gy <= 1.03 && gx >= 0.85 && gy >= 0.85;
				constexpr double settle = 0.1; // seconds
				if (!near || v.still >= settle)
					request(v, v.wanted);
			}
		} else if (i == 0) {
			// Not shown now, but it can be cut to at any moment: kept
			// drawn for the output, so it's never blank on a cut, at the
			// scale it was last shown at (at first, its own size), or
			// larger if a window shows it larger on the canvas (the Studio
			// Mode preview, where it's set up before a cut). Only ever
			// larger: the multiview shows scenes as small thumbnails in
			// canvas units too, and a cut from one of those would start
			// soft; a drawing too large for a frame is only scaled down.
			const View &w = views_[1];
			RasterParams target = v.requested_params;
			if (w.on_canvas && (w.canvas_wanted.sx > target.sx || w.canvas_wanted.sy > target.sy)) {
				const RasterParams grown = w.canvas_wanted;
				target.sx = std::max(target.sx, grown.sx);
				target.sy = std::max(target.sy, grown.sy);
				target.ox = grown.ox;
				target.oy = grown.oy;
			}
			if (!v.requested || version != v.requested_version || !target.close_to(v.requested_params))
				request(v, target);
		} else if (i == 1 && (v.texture || v.requested) && (v.idle += seconds) > 2.0) {
			// No window has shown it for 2 s: free its raster, and any
			// drawing for it not yet uploaded or still being drawn.
			obs_enter_graphics();
			release(v);
			obs_leave_graphics();
			v.idle = 0;
		}
	}
	views_[1].on_canvas = false;
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
		retry_later(view, r.error.c_str(), r.version); // keeping what's shown
		return;
	}
	if (r.width <= 0 || r.height <= 0 || r.pixels.empty()) { // nothing to draw
		if (view.texture)
			gs_texture_destroy(view.texture);
		view.texture = nullptr;
		if (view.parts)
			gs_vertexbuffer_destroy(view.parts);
		view.parts = nullptr;
		view.part_vertices = 0;
		view.failures = 0;
		shown_now(view, r);
		view.drawn = std::move(r);
		return;
	}
	// Textures have room to spare (up to 64 pixels across, 16 down), so a
	// text that changes often (a clock, a score) mostly goes into the one
	// it has, rather than a new one each time its width changes by a pixel.
	// Only the drawing's part of the texture is drawn.
	auto padded = [](int n, int to) { return uint32_t((n + to) / to * to); };
	const uint32_t want_w = padded(r.width, 64), want_h = padded(r.height, 16);
	const uint32_t have_w = view.texture ? gs_texture_get_width(view.texture) : 0;
	const uint32_t have_h = view.texture ? gs_texture_get_height(view.texture) : 0;
	const bool fits = have_w > uint32_t(r.width) && have_h > uint32_t(r.height) && have_w <= want_w + 128 &&
			  have_h <= want_h + 32;
	if (!fits) {
		// The new texture made before the old one goes, so if it can't be
		// (out of video memory) the text still shows as it was.
		gs_texture_t *texture = gs_texture_create(want_w, want_h, GS_BGRA, 1, nullptr, GS_DYNAMIC);
		if (!texture) {
			const std::string why = "no " + std::to_string(want_w) + "x" + std::to_string(want_h) + " texture";
			retry_later(view, why.c_str(), r.version);
			return;
		}
		if (view.texture)
			gs_texture_destroy(view.texture);
		view.texture = texture;
	}
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	if (!gs_texture_map(view.texture, &ptr, &linesize)) {
		retry_later(view, "couldn't write to its texture", r.version);
		return;
	}
	const uint32_t tex_w = gs_texture_get_width(view.texture), tex_h = gs_texture_get_height(view.texture);
	const size_t row = size_t(r.width) * 4;
	for (int y = 0; y < r.height; y++) {
		uint8_t *dst = ptr + size_t(y) * linesize;
		memcpy(dst, &r.pixels[size_t(y) * row], row);
		// A clear pixel beyond its edge, where filtering a scaled drawing
		// reaches (the rest of the texture is never sampled).
		if (uint32_t(r.width) < tex_w)
			memset(dst + row, 0, 4);
	}
	if (uint32_t(r.height) < tex_h)
		memset(ptr + size_t(r.height) * linesize, 0, std::min(size_t(linesize), size_t(r.width + 1) * 4));
	gs_texture_unmap(view.texture);

	// Only its parts with ink are drawn: two triangles each, in the
	// drawing's pixels, as gs_draw_sprite would draw the whole.
	if (view.parts)
		gs_vertexbuffer_destroy(view.parts);
	view.parts = nullptr;
	view.part_vertices = 0;
	if (!r.parts.empty()) {
		const size_t n = r.parts.size() * 6;
		gs_vb_data *vbd = gs_vbdata_create();
		vbd->num = n;
		vbd->points = static_cast<vec3 *>(bmalloc(sizeof(vec3) * n));
		vbd->num_tex = 1;
		vbd->tvarray = static_cast<gs_tvertarray *>(bzalloc(sizeof(gs_tvertarray)));
		vbd->tvarray[0].width = 2;
		vbd->tvarray[0].array = bmalloc(sizeof(vec2) * n);
		vec3 *pos = vbd->points;
		vec2 *uv = static_cast<vec2 *>(vbd->tvarray[0].array);
		size_t i = 0;
		for (const Raster::Part &p : r.parts) {
			const float x0 = float(p.x), y0 = float(p.y), x1 = float(p.x + p.w), y1 = float(p.y + p.h);
			const float corners[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
			for (const auto &c : corners) {
				vec3_set(&pos[i], c[0], c[1], 0.0f);
				vec2_set(&uv[i], c[0] / float(tex_w), c[1] / float(tex_h));
				i++;
			}
		}
		view.parts = gs_vertexbuffer_create(vbd, 0);
		if (view.parts)
			view.part_vertices = n;
	}
	view.failures = 0;
	shown_now(view, r);
	r.pixels = std::vector<uint8_t>();
	view.drawn = std::move(r);
}

// The output now shows this drawing: OBS sizes the source by it (see
// width()).
void TextSource::shown_now(const View &view, const Raster &r)
{
	if (&view != &views_[0] || !r.source_width || !r.source_height)
		return;
	shown_size_ = uint64_t(r.source_width) << 32 | r.source_height;
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
	} else {
		if (axis_aligned) {
			// Unrotated and unmirrored, the raster's pixels can sit
			// exactly on the canvas's, so the texture is drawn with no
			// resampling at all.
			want.ox = double(m.t.x) - std::floor(double(m.t.x));
			want.oy = double(m.t.y) - std::floor(double(m.t.y));
		}
		if (window && want.sx > 1e-4 && want.sy > 1e-4 && std::isfinite(want.sx) && std::isfinite(want.sy)) {
			// A window showing a scene (the Studio Mode preview, the
			// multiview): the matrix is in canvas units, so this is the
			// scale the output would draw it at (see tick()).
			RasterParams &c = views_[1].canvas_wanted;
			if (!views_[1].on_canvas) {
				c = want;
				views_[1].on_canvas = true;
			} else {
				c.sx = std::max(c.sx, want.sx);
				c.sy = std::max(c.sy, want.sy);
			}
		}
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
	if (!view.requested && view.retry_wait <= 0)
		request(view, want); // the first time: don't wait a frame

	draw(view, axis_aligned ? &want : nullptr);
}

void TextSource::draw(View &view, const RasterParams *at)
{
	if (!view.texture)
		return;
	const RasterParams &p = view.drawn.params;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (at && p.close_to(*at)) {
		// Drawn for exactly where it is: each of its pixels on one of the
		// frame's, so it's copied rather than filtered (the same pixels;
		// filtering costs more, a lot more with software rendering).
		if (!point_sampler_) {
			gs_sampler_info info = {};
			info.filter = GS_FILTER_POINT;
			info.address_u = info.address_v = info.address_w = GS_ADDRESS_CLAMP;
			info.max_anisotropy = 1;
			point_sampler_ = gs_samplerstate_create(&info);
		}
		if (point_sampler_)
			gs_effect_set_next_sampler(image, point_sampler_);
	}

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_matrix_push();
	gs_matrix_translate3f(float((view.drawn.x0 - p.ox) / p.sx), float((view.drawn.y0 - p.oy) / p.sy), 0.0f);
	gs_matrix_scale3f(float(1.0 / p.sx), float(1.0 / p.sy), 1.0f);
	gs_effect_set_texture(image, view.texture);
	while (gs_effect_loop(effect, "Draw")) {
		if (view.parts) {
			gs_load_vertexbuffer(view.parts);
			gs_load_indexbuffer(nullptr);
			gs_draw(GS_TRIS, 0, uint32_t(view.part_vertices));
		} else {
			gs_draw_sprite_subregion(view.texture, 0, 0, 0, uint32_t(view.drawn.width),
						 uint32_t(view.drawn.height));
		}
	}
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
	// None of the text as it is now (a slider being dragged changes it every
	// frame): the output's latest, a step behind, as the rest of the text
	// shown at this moment is. The letters stay, and catch up a frame later.
	if (views_[0].texture) {
		draw(views_[0]);
		return true;
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

	// Still under content_mutex_ (see the header and writing_edit).

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "box_width", width);
	{
		WritingEdit writing;
		obs_source_update(source_, settings);
	}
	obs_data_release(settings);
}

double TextSource::min_box_width() const
{
	std::lock_guard lock(layout_lock());
	return widest_word(layout_context(), doc_);
}

void TextSource::set_box(int width, int height)
{
	std::lock_guard content(content_mutex_);
	Document doc;
	{
		std::lock_guard lock(layout_lock());
		doc = doc_;
	}
	width = std::clamp(width, 1, 16384);
	height = std::clamp(height, 0, 16384);
	doc.style.box = true;
	doc.style.box_width = width;
	doc.style.box_height = height;
	apply(std::move(doc));

	// Still under content_mutex_ (see the header and writing_edit).
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "box", true);
	obs_data_set_int(settings, "box_width", width);
	obs_data_set_int(settings, "box_height", height);
	{
		WritingEdit writing;
		obs_source_update(source_, settings);
	}
	obs_data_release(settings);
}

void TextSource::set_align(int align)
{
	std::lock_guard content(content_mutex_);
	Document doc;
	{
		std::lock_guard lock(layout_lock());
		doc = doc_;
	}
	align = std::clamp(align, 0, 3);
	doc.style.align = Align(align);
	apply(std::move(doc));

	// Still under content_mutex_ (see the header and writing_edit).
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "align", align);
	{
		WritingEdit writing;
		obs_source_update(source_, settings);
	}
	obs_data_release(settings);
}

void TextSource::set_line_spacing(double spacing)
{
	std::lock_guard content(content_mutex_);
	Document doc;
	{
		std::lock_guard lock(layout_lock());
		doc = doc_;
	}
	spacing = std::clamp(spacing, 0.5, 3.0);
	doc.style.line_spacing = spacing;
	apply(std::move(doc));

	// Still under content_mutex_ (see the header and writing_edit). Line
	// spacing is in the dialog's Spacing and case group, turned on for it.
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "typography", true);
	obs_data_set_double(settings, "line_spacing", spacing);
	{
		WritingEdit writing;
		obs_source_update(source_, settings);
	}
	obs_data_release(settings);
}

void TextSource::set_wrap_options(bool balance, bool avoid_orphans)
{
	std::lock_guard content(content_mutex_);
	Document doc;
	{
		std::lock_guard lock(layout_lock());
		doc = doc_;
	}
	doc.style.balance = balance;
	doc.style.avoid_orphans = avoid_orphans;
	apply(std::move(doc));

	// Still under content_mutex_ (see the header and writing_edit).
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "balance", balance);
	obs_data_set_bool(settings, "avoid_orphans", avoid_orphans);
	{
		WritingEdit writing;
		obs_source_update(source_, settings);
	}
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

	// Still under content_mutex_ (see the header and writing_edit).

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", text.c_str());
	obs_data_array_t *array = spans_array(normalised);
	obs_data_set_array(settings, "spans", array);
	obs_data_array_release(array);
	{
		WritingEdit writing;
		obs_source_update(source_, settings);
	}
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
	// Kept within the source's box (OBS's outline): a selected line's end
	// mark, or trailing letter spacing, never reaches past it.
	auto add = [&](double x0, double y, double x1, double h) {
		const double left = std::max(0.0, to_source_x(x0)), right = std::min(frame_.width, to_source_x(x1));
		if (right > left)
			boxes.push_back({left, to_source_y(y), right - left, h * s});
	};

	PangoLayoutIter *iter = pango_layout_get_iter(layout_);
	do {
		PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
		const size_t start = size_t(line->start_index), end = start + size_t(line->length);
		// A line's end counts as selected when the selection runs on past
		// it (over the line break): the line's highlight runs on a little
		// past its last letter, as in word processors.
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
		if (through_end) {
			const double mark = doc_.style.size * 0.25;
			// The piece that reaches the line's end grows; with none (the
			// selection starts at the line's very end), a piece of its own.
			const double end_x = std::min(frame_.width, to_source_x(line_x1));
			Box *last = nullptr;
			for (Box &bx : boxes)
				if (std::fabs(bx.x + bx.w - end_x) < 1e-6 && std::fabs(bx.y - to_source_y(y)) < 1e-6)
					last = &bx;
			if (last)
				last->w = std::min(last->w + mark * s, frame_.width - last->x);
			else
				add(line_x1, y, line_x1 + mark, h);
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

size_t TextSource::backspace_from(size_t index) const
{
	std::lock_guard lock(layout_lock());
	return backspace_index(layout_, std::min(index, doc_.text.size()));
}

std::vector<int> TextSource::family_weights(const std::string &family, bool italic) const
{
	std::lock_guard lock(layout_lock());
	return typeset::family_weights(layout_context(), family.empty() ? doc_.style.family : family, italic);
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

void video_tick(void *data, float seconds)
{
	static_cast<TextSource *>(data)->tick(seconds);
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
