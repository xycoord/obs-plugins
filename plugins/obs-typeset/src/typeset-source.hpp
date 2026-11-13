// The Typeset text source.
//
// Its size (what OBS sees as the source's width and height) is its text box:
// the text's own size, or the box it was given, plus any background plate's
// padding. Scaling it in a scene, by dragging its handles or in Edit
// Transform, scales the text like a vector drawing: the text is rasterised at
// the size it is actually drawn, every time that changes, so it is never
// blurred or pixelated and never drawn at more pixels than it covers.
#pragma once

#include "raster-worker.hpp"
#include "text-engine.hpp"

#include <obs.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace typeset {

extern const char *const source_id;

void register_source();

// Text is laid out for its size and for editing on OBS's UI thread (an
// update OBS makes on its graphics thread is passed there; see update()),
// with this one font map. The lock is for what other threads read of a
// layout (the editor's queries come from the UI thread too, but a source can
// be made, and so laid out, on any thread), as Pango's font maps aren't
// thread-safe. The rasteriser has a font map of its own on its own thread.
std::mutex &layout_lock();
PangoContext *layout_context();

// Says which thread is OBS's UI thread (call once, from it, at load). Until
// it's said, every thread counts as it.
void set_ui_thread();

struct Caret {
	double x = 0, y = 0, height = 0; // source units
};

class TextSource {
public:
	TextSource(obs_data_t *settings, obs_source_t *source);
	~TextSource();

	// OBS callbacks. update() on any thread but the UI thread (OBS applies a
	// video source's changes on its graphics thread) is passed to the UI
	// thread, which lays the text out from the settings as they are then,
	// so the graphics thread never waits for a layout.
	void update(obs_data_t *settings);
	// The size of the text as the output shows it: while a new version is
	// being drawn (a frame or two), the size the shown drawing was made
	// for, so the old drawing isn't placed by the new size (it would jump
	// when the item isn't anchored at its top left); the new size once
	// it's shown, or after 100 ms whatever happens.
	uint32_t width() const;
	uint32_t height() const;
	void tick(float seconds);
	void render();

	// Editing, from OBS's window. Positions are in the source's own units;
	// indices are byte offsets into the UTF-8 text, and every one returned
	// is a cursor position (between graphemes).
	static TextSource *from(obs_source_t *source); // null if not one of ours
	obs_source_t *source() const { return source_; }
	std::string text() const;
	Document document() const;
	void set_content(const std::string &text, const Spans &spans); // text and styled runs together
	void set_box_width(int width); // the text box's width, while dragging its grip
	void set_align(int align);
	void set_line_spacing(double spacing); // the whole text's leading (a multiple of the font's line height), at once     // the whole text's alignment (Align), at once
	Frame frame() const;
	Box box() const; // the source's own extent, as laid out now
	size_t hit_test(double x, double y, bool nearest_edge = true) const;
	Caret caret(size_t index) const;
	std::vector<Box> selection_boxes(size_t from, size_t to) const;
	size_t move_visually(size_t index, int direction) const;
	size_t move_by_word(size_t index, int direction) const;
	size_t move_by_line(size_t index, int direction, double x) const; // x: the column to keep to
	size_t line_edge(size_t index, int direction) const;
	size_t next_cursor(size_t index, int direction) const;
	size_t backspace_from(size_t index) const; // where Backspace at index deletes back to
	// The weights a typeface has (empty: the text's own), UI thread only.
	std::vector<int> family_weights(const std::string &family, bool italic) const;
	size_t cursor_at_or_before(size_t index) const; // the nearest cursor position not after index
	std::pair<size_t, size_t> word_at(size_t index) const;
	std::pair<size_t, size_t> paragraph_at(size_t index) const;

	// Drawing the current output raster again elsewhere (the editor draws
	// selected text over its highlight), on OBS's graphics thread, with the
	// world matrix set to map the source's units.
	bool draw_raster(); // false if no raster shows the text as it is now

private:
	obs_source_t *source_;

	// The text as laid out for its size and for editing; guarded by the
	// one lock for all sources' layouts (see layout_lock()).
	Document doc_;
	PangoLayout *layout_ = nullptr;
	Frame frame_;
	std::atomic<uint32_t> width_{0}, height_{0}; // as laid out now (for the editor)
	std::atomic<uint64_t> size_{0};              // the same, width and height in one (for OBS)
	std::atomic<uint64_t> laid_out_ns_{0};      // when
	// The size the output's drawing was made for (0: none yet).
	// Width and height in one value, so OBS never gets one drawing's width
	// with another's height.
	std::atomic<uint64_t> shown_size_{0};
	std::atomic<bool> update_queued_{false}; // an update passed to the UI thread, not yet applied

	void update_now(obs_data_t *settings);
	void queue_update();
	uint64_t size_for_obs() const;

	// The document as it stands, for the rasteriser, with its version;
	// guarded by doc_mutex_ (taken inside the layout lock, never around it).
	std::mutex doc_mutex_;
	// An edit from the preview writes the settings and lays out at once;
	// OBS's update (passed from its graphics thread to the UI thread, a
	// frame or so later) reads the settings and lays out. One at a time, or
	// an update that read the text just before an edit would lay out the
	// old text after it. (Both are on the UI thread now, but a source made
	// on another thread lays out there.) The edit calls obs_source_update
	// with this held, on purpose (released first, an update could read the
	// settings between the layout and the write). libobs defers a video
	// source's update to its graphics thread, so update() isn't called from
	// inside it; if it ever were, update() sees it's inside an edit's write
	// and returns without the lock.
	std::mutex content_mutex_;
	Spans superseded_; // runs the settings may still hold, moved since (being written back)
	Document pending_doc_;
	uint32_t pending_width_ = 0, pending_height_ = 0; // its size
	uint64_t doc_version_ = 0;

	// A source can be drawn into the output (the canvas, or a texture a
	// filter or a scene renders into) and into windows (the properties
	// dialog's preview, projectors): each gets a raster of its own, for the
	// size it's drawn at there. OBS's graphics thread only.
	struct View {
		std::shared_ptr<RasterSlot> slot = std::make_shared<RasterSlot>();
		gs_texture_t *texture = nullptr;
		Raster drawn; // the texture's geometry (its pixels aren't kept)
		uint64_t texture_seq = 0;
		bool want = false;
		RasterParams wanted;
		bool requested = false;
		RasterParams requested_params;
		uint64_t requested_version = 0;
		double idle = 0;       // seconds not shown (windows)
		double retry_wait = 0; // after a failed drawing, seconds until it's asked for again
		int failures = 0;      // failed in a row: each retry waits longer
		uint64_t failure_logged = 0; // the text's version a failure was last logged for
		// Windows only: shown this frame on a canvas (a scene in the Studio
		// Mode preview or the multiview), at this scale and offset.
		bool on_canvas = false;
		RasterParams canvas_wanted;
	};
	View views_[2]; // output, windows
	uint64_t next_seq_ = 1;

	void apply(Document doc);
	void request(View &view, const RasterParams &params);
	void upload_result(View &view);
	void draw(View &view);
	void release(View &view);
	void retry_later(View &view, const char *why, uint64_t version);
	void shown_now(const View &view, const Raster &r);
	// Source units <-> layout units (Pango's), under the layout lock.
	double to_layout_x(double x) const { return (x - frame_.text_x) / frame_.text_scale; }
	double to_layout_y(double y) const { return (y - frame_.text_y) / frame_.text_scale; }
	double to_source_x(double x) const { return x * frame_.text_scale + frame_.text_x; }
	double to_source_y(double y) const { return y * frame_.text_scale + frame_.text_y; }
};

} // namespace typeset
