// The text being edited in the preview, as plain data: the words, their
// styled runs, the caret and selection, styles chosen for what's typed next,
// and undo. No OBS, Qt or layout in it, so it's tested on its own; where the
// caret goes on an arrow key (which needs the layout) is worked out by the
// editor and handed in.
//
// Indices are byte offsets into the UTF-8 text, as everywhere in the plugin.
#pragma once

#include "rich-text.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace typeset {

enum class EditKind { None, Typing, Deleting, Stepping, Other };

// What the text's own font has (its weight and slant), for bold, italic and
// underline toggling off text that has them from its font.
struct BaseStyle {
	bool bold = false, italic = false, underline = false;
	bool has(StyleChange::Field f) const
	{
		return f == StyleChange::Bold ? bold : f == StyleChange::Italic ? italic : f == StyleChange::Underline && underline;
	}
};

// The most a paste brings in: more than any title, and well short of what
// would stall OBS while it's laid out.
constexpr size_t max_paste_chars = 20000;

// Pasted text as it's typed: every kind of line break made "\n", other
// control characters but tabs dropped (they'd be cut off or shown as boxes),
// and at most
// max_paste_chars characters, cut at a character's edge.
std::string clean_paste(std::string text, bool *cut = nullptr);

// What the editing strip changes besides the words: the whole text's
// alignment and its text box's width. In undo with the words.
struct StripLayout {
	int align = 0;
	int box_width = 0;
	double line_spacing = 1.0; // the whole text's leading
	bool balance = false, avoid_orphans = false; // its wrapping
	bool operator==(const StripLayout &) const = default;
};

class EditModel {
public:
	using Layout = StripLayout;

	// A new edit of this text.
	void start(std::string text, Spans spans, Layout layout = {});
	// The text as it now is, changed from elsewhere (its properties): the
	// caret and selection kept where they can be (cursor positions in the
	// new text, worked out by the caller); undo starts afresh.
	void take(std::string text, Spans spans, size_t caret, size_t anchor, Layout layout = {});

	const std::string &text() const { return text_; }
	const Spans &spans() const { return spans_; }
	size_t caret() const { return caret_; }
	size_t anchor() const { return anchor_; }
	bool has_selection() const { return caret_ != anchor_; }
	size_t sel_from() const { return std::min(caret_, anchor_); }
	size_t sel_to() const { return std::max(caret_, anchor_); }
	std::string selected_text() const { return text_.substr(sel_from(), sel_to() - sel_from()); }
	const Layout &layout() const { return layout_; }

	// Moving the caret (extending the selection, or not). Any of these ends
	// a run of typing for undo, and forgets styles chosen for what's typed
	// next. select() sets both ends.
	void set_caret(size_t caret, bool extend);
	void select(size_t anchor, size_t caret);
	void select_all() { select(0, text_.size()); }

	// The column Up and Down keep to (source units; negative: none yet).
	double column = -1;

	// Each returns whether the text or its runs changed (and need writing).
	bool replace(size_t from, size_t to, const std::string &with, EditKind kind);
	bool replace_selection(const std::string &with, EditKind kind);
	// Bold and so on, on the selection or, with nothing selected, on what's
	// typed next. Toggled, bold (italic, underline) goes off where all of
	// the selection has it, by a run or from the base.
	bool style(StyleChange change, bool toggle, const BaseStyle &base);
	// A tenth of the text's size bigger or smaller: each part of a
	// selection from its own size, or what's typed next.
	bool step_size(int direction);
	bool set_size_percent(double percent); // of the text's own size
	bool set_weight(int weight);           // 100 to 1000 (an explicit weight, in place of bold)
	bool set_family(const std::string &family); // a typeface for the selection; empty: the text's own
	// Letter spacing for the selection (percent of the size), and the whole
	// text's leading. A slider's drag is one undo step: new_step at its start.
	bool set_tracking(double percent, bool new_step);
	bool set_line_spacing(double spacing, bool new_step);
	bool set_wrap_options(bool balance, bool avoid_orphans);
	bool toggle_script(int which);         // 1 superscript, 2 subscript: on, or off if it has it
	// The alignment (one undo step), and the box's width (one undo step per
	// drag of its grip: begin_box_drag() marks the start).
	bool set_align(int align);
	void begin_box_drag();
	bool set_box_width(int width);
	bool undo();
	bool redo();

	// What the selection (or what's typed next) has, for the format bar.
	bool has(StyleChange::Field f, const BaseStyle &base) const;
	double size_percent() const;           // of the text's own size
	void size_range(double *smallest, double *largest) const; // the same, over a selection
	int weight(int base_weight) const;     // as drawn; 0 if mixed
	int script() const;                    // 0 none, 1 super, 2 sub; -1 if mixed
	bool family(std::string *out) const;   // the typeface ("": the text's own); false if mixed
	// The selection's letter spacing: false if mixed; *own false if it's the
	// text's own (whatever that is).
	bool tracking(double *percent, bool *own) const;
	uint32_t colour(uint32_t base) const;  // 0xAABBGGRR
	Span style_here() const;               // the run at the selection's start, or before the caret

	const std::vector<StyleChange> &pending() const { return pending_; }

private:
	struct Snapshot {
		std::string text;
		Spans spans;
		size_t caret = 0, anchor = 0;
		Layout layout;
	};
	std::string text_;
	Spans spans_;
	Layout layout_;
	size_t caret_ = 0, anchor_ = 0;
	std::vector<StyleChange> pending_; // for what's typed next, in the order chosen
	std::vector<Snapshot> undo_, redo_;
	EditKind last_edit_ = EditKind::None;

	Snapshot snapshot() const { return {text_, spans_, caret_, anchor_, layout_}; }
	void restore(Snapshot s);
	void push_undo();
	bool typing_has(StyleChange::Field f, const BaseStyle &base) const;
	Span typing_span() const;
};

} // namespace typeset
