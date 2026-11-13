// Styled runs within a text: a word in bold, a name in another colour.
//
// The source's own style (its properties) is the base for all of its text; a
// run holds only what it changes from that. So changing the font or colour in
// the properties changes everything except what a run overrides, and the
// properties never have to show "mixed" values.
//
// Runs are byte ranges of the UTF-8 text, kept sorted, not overlapping, and
// merged when neighbours are alike. This is plain logic, with no Pango or OBS,
// so it's tested on its own (tests/typeset/unit).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace typeset {

struct Span {
	size_t start = 0, end = 0;
	int8_t bold = -1, italic = -1, underline = -1; // -1: as the base; 1: on (0 isn't used)
	bool has_colour = false;
	uint32_t colour = 0; // 0xAABBGGRR, as OBS stores colours
	int size = 0;        // percent of the base size; 0: as the base

	bool same_style(const Span &o) const
	{
		return bold == o.bold && italic == o.italic && underline == o.underline && has_colour == o.has_colour &&
		       (!has_colour || colour == o.colour) && size == o.size;
	}
	bool plain() const { return bold < 0 && italic < 0 && underline < 0 && !has_colour && size == 0; }
	bool operator==(const Span &o) const { return start == o.start && end == o.end && same_style(o); }
};

using Spans = std::vector<Span>;

// What a change sets on a range.
struct StyleChange {
	enum Field { Bold, Italic, Underline, Colour, Size, Clear } field = Bold;
	int value = 0;       // bold/italic/underline: 1 on, -1 off; size: percent (0: as the base)
	uint32_t colour = 0; // Colour
	bool no_colour = false;
};

// Sorted, non-overlapping, merged, no plain runs, within the text, on UTF-8
// character boundaries.
void normalise(Spans &spans, const std::string &text);

// The text's [from, to) has been replaced by `inserted` bytes. Runs after it
// move; runs inside it shrink; the new text takes the style of what it
// replaced, or (typed at a point) of the character before it.
void edit(Spans &spans, size_t from, size_t to, size_t inserted);

// A change applied to [from, to).
void apply(Spans &spans, size_t from, size_t to, const StyleChange &change, const std::string &text);

// The style at a byte (a span covering just it, plain if none).
Span style_at(const Spans &spans, size_t index);

// Whether every byte in [from, to) has bold (or italic, underline) on.
bool all_on(const Spans &spans, size_t from, size_t to, StyleChange::Field field);

// The runs moved to fit a text changed from outside (its properties): the
// common start and end of the old and new texts keep their runs.
void follow_text(Spans &spans, const std::string &old_text, const std::string &new_text);

} // namespace typeset
