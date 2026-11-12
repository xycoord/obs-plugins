// The Typeset text source.
//
// Its size (what OBS sees as the source's width and height) is the text's
// layout at its font size. Scaling it in a scene, by dragging its handles or
// in Edit Transform, scales the text like a vector drawing: the text is
// rasterised at the size it is actually drawn on the canvas, every time that
// changes, so it is never blurred or pixelated and never drawn at more pixels
// than it covers.
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

	// Editing, from OBS's window. Indices are byte offsets into the UTF-8
	// text; every one returned is a cursor position (between graphemes).
	static TextSource *from(obs_source_t *source); // null if not one of ours
	obs_source_t *source() const { return source_; }
	std::string text() const;
	void set_text(const std::string &text);
	Box box() const; // the source's own extent, in source units
	// The caret position nearest (x, y); or, not nearest_edge, the start of
	// the grapheme under it.
	size_t hit_test(double x, double y, bool nearest_edge = true) const;
	Caret caret(size_t index) const;
	std::vector<Box> selection_boxes(size_t from, size_t to) const;
	size_t move_visually(size_t index, int direction) const;
	size_t move_by_word(size_t index, int direction) const;
	size_t move_by_line(size_t index, int direction, double x) const; // x: the column to keep to
	size_t line_edge(size_t index, int direction) const;
	size_t next_cursor(size_t index, int direction) const;
	std::pair<size_t, size_t> word_at(size_t index) const;
	std::pair<size_t, size_t> paragraph_at(size_t index) const;

private:
	obs_source_t *source_;

	// The text as laid out for its size and for editing; guarded by the
	// one lock for all sources' layouts (see layout_lock()).
	Document doc_;
	PangoLayout *layout_ = nullptr;
	std::atomic<uint32_t> width_{0}, height_{0};

	// Rasterising. The document as it stands, for the worker, is held in
	// the slot's lock together with its version.
	std::shared_ptr<RasterSlot> slot_;
	Document pending_doc_;
	uint64_t doc_version_ = 0;

	// OBS's graphics thread only.
	gs_texture_t *texture_ = nullptr;
	Raster drawn_; // the texture's geometry (its pixels aren't kept)
	uint64_t texture_seq_ = 0;
	uint64_t next_seq_ = 1;
	bool want_ = false;
	RasterParams wanted_;
	bool requested_ = false;
	RasterParams requested_params_;
	uint64_t requested_version_ = 0;

	void apply(Document doc);
	void request(const RasterParams &params);
	void upload_result();
};

} // namespace typeset
