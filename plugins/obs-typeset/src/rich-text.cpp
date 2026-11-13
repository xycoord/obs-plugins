#include "rich-text.hpp"

#include <algorithm>

namespace typeset {

namespace {

// Back to the start of a UTF-8 character.
size_t char_start(const std::string &text, size_t i)
{
	i = std::min(i, text.size());
	while (i > 0 && i < text.size() && (uint8_t(text[i]) & 0xC0) == 0x80)
		i--;
	return i;
}

void merge(Spans &spans)
{
	Spans out;
	for (const Span &s : spans) {
		if (s.start >= s.end || s.plain())
			continue;
		if (!out.empty() && out.back().end == s.start && out.back().same_style(s))
			out.back().end = s.end;
		else
			out.push_back(s);
	}
	spans.swap(out);
}

// Sorted by start, and overlaps taken out: where runs overlap, the one that
// starts first keeps the bytes, and a later one starts after the furthest end
// so far (one wholly inside an earlier run goes; merge drops it).
void untangle(Spans &spans)
{
	std::stable_sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) { return a.start < b.start; });
	size_t furthest = 0;
	for (Span &s : spans) {
		s.start = std::max(s.start, furthest);
		furthest = std::max(furthest, s.end);
	}
}

// Split runs so none crosses `at`.
void split_at(Spans &spans, size_t at)
{
	for (size_t i = 0; i < spans.size(); i++) {
		if (spans[i].start < at && at < spans[i].end) {
			Span right = spans[i];
			right.start = at;
			spans[i].end = at;
			spans.insert(spans.begin() + long(i) + 1, right);
			return;
		}
	}
}

void change(Span &s, const StyleChange &c)
{
	switch (c.field) {
	case StyleChange::Bold:
		s.bold = int8_t(std::clamp(c.value, -1, 1));
		break;
	case StyleChange::Italic:
		s.italic = int8_t(std::clamp(c.value, -1, 1));
		break;
	case StyleChange::Underline:
		s.underline = int8_t(std::clamp(c.value, -1, 1));
		break;
	case StyleChange::Colour:
		s.has_colour = !c.no_colour;
		s.colour = c.colour;
		break;
	case StyleChange::Size:
		if (c.relative) {
			const int now = s.size ? s.size : 100;
			s.size = std::clamp(now + c.value, 10, 1000);
			if (s.size == 100)
				s.size = 0;
		} else {
			s.size = std::clamp(c.value, 0, 1000);
		}
		break;
	case StyleChange::Clear:
		s = Span{s.start, s.end};
		break;
	}
}

} // namespace

void normalise(Spans &spans, const std::string &text)
{
	for (Span &s : spans) {
		s.start = char_start(text, std::min(s.start, text.size()));
		s.end = char_start(text, std::min(s.end, text.size()));
		if (s.size < 0)
			s.size = 0;
	}
	untangle(spans);
	merge(spans);
}

void edit(Spans &spans, size_t from, size_t to, size_t inserted)
{
	if (to < from)
		std::swap(from, to);
	// The new text's style: that of what it replaced, or (typed at a
	// point) of the character before it.
	Span inherit;
	if (from < to)
		inherit = style_at(spans, from);
	else if (from > 0)
		inherit = style_at(spans, from - 1);

	Spans out;
	for (const Span &s : spans) {
		// The replaced bytes go: positions in them close up to `from`.
		auto removed = [&](size_t p) { return p <= from ? p : p >= to ? p - (to - from) : from; };
		// The new bytes come in at `from`: a run ending there doesn't
		// take them, one going on past it does.
		auto added = [&](size_t p, bool end) { return p > from || (p == from && !end) ? p + inserted : p; };
		Span t = s;
		t.start = added(removed(s.start), false);
		t.end = added(removed(s.end), true);
		out.push_back(t);
	}
	if (inserted > 0 && !inherit.plain()) {
		inherit.start = from;
		inherit.end = from + inserted;
		out.push_back(inherit);
	}
	untangle(out);
	spans.swap(out);
	merge(spans);
}

void apply(Spans &spans, size_t from, size_t to, const StyleChange &c, const std::string &text)
{
	from = char_start(text, from);
	to = char_start(text, std::min(to, text.size()));
	if (from >= to)
		return;
	split_at(spans, from);
	split_at(spans, to);
	// Fill the gaps in [from, to) with plain runs, so every byte has one.
	Spans filled;
	size_t at = from;
	for (const Span &s : spans) {
		if (s.end <= from || s.start >= to) {
			filled.push_back(s);
			continue;
		}
		if (s.start > at)
			filled.push_back(Span{at, s.start});
		filled.push_back(s);
		at = s.end;
	}
	if (at < to)
		filled.push_back(Span{at, to});
	std::stable_sort(filled.begin(), filled.end(), [](const Span &a, const Span &b) { return a.start < b.start; });
	for (Span &s : filled)
		if (s.start >= from && s.end <= to)
			change(s, c);
	spans.swap(filled);
	merge(spans);
}

Span style_at(const Spans &spans, size_t index)
{
	for (const Span &s : spans)
		if (s.start <= index && index < s.end)
			return s;
	return Span{index, index + 1};
}

bool all_on(const Spans &spans, size_t from, size_t to, StyleChange::Field field, bool base_on)
{
	if (from >= to)
		return false;
	size_t at = from;
	for (const Span &s : spans) {
		if (s.end <= at || s.start >= to)
			continue;
		if (s.start > at && !base_on)
			return false; // a gap: as the base
		const int8_t v = field == StyleChange::Bold     ? s.bold
				 : field == StyleChange::Italic ? s.italic
								: s.underline;
		if (v == 0 || (v < 0 && !base_on))
			return false;
		at = s.end;
		if (at >= to)
			return true;
	}
	return at >= to || base_on;
}

void follow_text(Spans &spans, const std::string &old_text, const std::string &new_text)
{
	if (old_text == new_text || spans.empty())
		return;
	size_t prefix = 0;
	const size_t n = std::min(old_text.size(), new_text.size());
	while (prefix < n && old_text[prefix] == new_text[prefix])
		prefix++;
	size_t suffix = 0;
	while (suffix < n - prefix && old_text[old_text.size() - 1 - suffix] == new_text[new_text.size() - 1 - suffix])
		suffix++;
	edit(spans, prefix, old_text.size() - suffix, new_text.size() - suffix - prefix);
	normalise(spans, new_text);
}

} // namespace typeset
