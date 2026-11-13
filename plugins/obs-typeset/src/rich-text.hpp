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
	Span() = default;
	Span(size_t start, size_t end) : start(start), end(end) {}

	size_t start = 0, end = 0;
	int8_t bold = -1, italic = -1, underline = -1; // -1: as the base; 1: on; 0: off (on a bold or italic base)
	int weight = 0; // 100 to 1000: a weight of its own, which wins over bold; 0: as the base (or bold says)
	bool has_colour = false;
	uint32_t colour = 0; // 0xAABBGGRR, as OBS stores colours
	double size = 0;     // percent of the base size; 0: as the base
	int8_t script = 0;   // 0: on the line; 1: superscript; 2: subscript
	std::string family;  // a typeface of its own; empty: the base's
	bool has_tracking = false;
	double tracking = 0; // letter spacing of its own, percent of its size (with has_tracking)

	bool same_style(const Span &o) const
	{
		return bold == o.bold && italic == o.italic && underline == o.underline && weight == o.weight &&
		       has_colour == o.has_colour && (!has_colour || colour == o.colour) && size == o.size &&
		       script == o.script && family == o.family && has_tracking == o.has_tracking &&
		       (!has_tracking || tracking == o.tracking);
	}
	bool plain() const
	{
		return bold < 0 && italic < 0 && underline < 0 && weight == 0 && !has_colour && size == 0 && script == 0 &&
		       family.empty() && !has_tracking;
	}
	bool operator==(const Span &o) const { return start == o.start && end == o.end && same_style(o); }
};

using Spans = std::vector<Span>;

// What a change sets on a range.
struct StyleChange {
	enum Field { Bold, Italic, Underline, Colour, Size, Clear, Weight, Script, Family, Tracking } field = Bold;
	// Bold, italic, underline: 1 on, 0 off, -1 as the base. Weight: 100 to
	// 1000, or 0 for the base (a weight replaces bold, and bold a weight).
	// Script: 0 on the line, 1 superscript, 2 subscript.
	int value = 0;
	double amount = 0;     // Size: percent of the base (0: as the base), or with relative a step in percent
	bool relative = false; // Size: amount is added to each part's own size
	uint32_t colour = 0;   // Colour
	bool no_colour = false;
	std::string family;    // Family: a typeface, or empty for the base's
	bool to_base = false;  // Tracking: back to the base's (amount is the percent otherwise)
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

// Whether every byte in [from, to) has bold (or italic, underline) on: by a
// run, or (with base_on) from the base where no run turns it off.
bool all_on(const Spans &spans, size_t from, size_t to, StyleChange::Field field, bool base_on = false);

// The weight a run is drawn at, over a base of base_weight (400 regular,
// 700 bold): its own weight if it has one; bold always bolder than the base
// (a SemiBold text's bold is Black); bold turned off, regular; otherwise the
// base's.
int effective_weight(const Span &span, int base_weight);

// Whether every byte in [from, to) has the same value of something (gaps
// count as plain text, Span{}); *out is that value. For an empty range, the
// style before it (what's typed there next) is asked.
bool uniform(const Spans &spans, size_t from, size_t to, double (*get)(const Span &), double *out);

// The same for the typeface: *out is the family every byte has ("" for the
// base's).
bool uniform_family(const Spans &spans, size_t from, size_t to, std::string *out);

// The runs moved to fit a text changed from outside (its properties): the
// common start and end of the old and new texts keep their runs.
void follow_text(Spans &spans, const std::string &old_text, const std::string &new_text);

} // namespace typeset
