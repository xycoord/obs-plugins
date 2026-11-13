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

// Text is laid out for its size and for editing on OBS's threads (its
// window's, its graphics thread's), all with this one font map and this one
// lock, as Pango's font maps aren't thread-safe.
std::mutex &layout_lock();
PangoContext *layout_context();

struct Caret {
	double x = 0, y = 0, height = 0; // source units
};

class TextSource {
public:
	TextSource(obs_data_t *settings, obs_source_t *source);
	~TextSource();

	// OBS callbacks.
	void update(obs_data_t *settings);
	uint32_t width() const { return width_.load(); }
	uint32_t height() const { return height_.load(); }
	void tick();
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
	Frame frame() const;
	Box box() const; // the source's own extent
	size_t hit_test(double x, double y, bool nearest_edge = true) const;
	Caret caret(size_t index) const;
	std::vector<Box> selection_boxes(size_t from, size_t to) const;
	size_t move_visually(size_t index, int direction) const;
	size_t move_by_word(size_t index, int direction) const;
	size_t move_by_line(size_t index, int direction, double x) const; // x: the column to keep to
	size_t line_edge(size_t index, int direction) const;
	size_t next_cursor(size_t index, int direction) const;
	size_t backspace_from(size_t index) const; // where Backspace at index deletes back to
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
	std::atomic<uint32_t> width_{0}, height_{0};

	// The document as it stands, for the rasteriser, with its version;
	// guarded by doc_mutex_ (taken inside the layout lock, never around it).
	std::mutex doc_mutex_;
	// An edit from the preview (on OBS's UI thread) writes the settings and
	// lays out at once; OBS's update (on its graphics thread, a frame later)
	// reads the settings and lays out. One at a time, or an update that
	// read the text just before an edit would lay out the old text after it.
	// The edit calls obs_source_update with this held, on purpose (released
	// first, an update could read the settings between the layout and the
	// write): that relies on libobs deferring a video source's update to its
	// graphics thread, so update() is never called from inside it, here.
	std::mutex content_mutex_;
	Spans superseded_; // runs the settings may still hold, moved since (being written back)
	Document pending_doc_;
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
		int idle_ticks = 0;
		int retry_ticks = 0; // after a failed drawing, frames until it's asked for again
	};
	View views_[2]; // output, windows
	uint64_t next_seq_ = 1;

	void apply(Document doc);
	void request(View &view, const RasterParams &params);
	void upload_result(View &view);
	void draw(View &view);
	void release(View &view);
	// Source units <-> layout units (Pango's), under the layout lock.
	double to_layout_x(double x) const { return (x - frame_.text_x) / frame_.text_scale; }
	double to_layout_y(double y) const { return (y - frame_.text_y) / frame_.text_scale; }
	double to_source_x(double x) const { return x * frame_.text_scale + frame_.text_x; }
	double to_source_y(double y) const { return y * frame_.text_scale + frame_.text_y; }
};

} // namespace typeset
