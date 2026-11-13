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
#include <string>
#include <utility>
#include <vector>

namespace typeset {

extern const char *const source_id;

void register_source();

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
	void set_text(const std::string &text);
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
