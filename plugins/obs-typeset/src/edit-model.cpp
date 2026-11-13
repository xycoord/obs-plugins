#include "edit-model.hpp"

#include <algorithm>

namespace typeset {

namespace {

void replace_all(std::string &s, const std::string &from, const std::string &to)
{
	for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
		s.replace(at, from.size(), to);
}

} // namespace

std::string clean_paste(std::string s, bool *cut)
{
	// Line breaks of every kind (Windows', old Macs', Unicode's line and
	// paragraph separators) as the one the text uses.
	replace_all(s, "\r\n", "\n");
	replace_all(s, "\r", "\n");
	replace_all(s, "\u2028", "\n");
	replace_all(s, "\u2029", "\n");
	s.erase(std::remove_if(s.begin(), s.end(),
			       [](char c) {
				       const auto u = static_cast<unsigned char>(c);
				       return (u < 0x20 && c != '\n' && c != '\t') || u == 0x7F;
			       }),
		s.end());
	// Characters counted by their first bytes (anything but 10xxxxxx).
	size_t chars = 0, at = 0;
	for (; at < s.size(); at++) {
		if ((static_cast<unsigned char>(s[at]) & 0xC0) != 0x80 && chars++ == max_paste_chars)
			break;
	}
	if (cut)
		*cut = at < s.size();
	s.resize(at);
	return s;
}

void EditModel::start(std::string text, Spans spans)
{
	text_ = std::move(text);
	spans_ = std::move(spans);
	caret_ = anchor_ = 0;
	column = -1;
	pending_.clear();
	undo_.clear();
	redo_.clear();
	last_edit_ = EditKind::None;
}

void EditModel::take(std::string text, Spans spans, size_t caret, size_t anchor)
{
	text_ = std::move(text);
	spans_ = std::move(spans);
	caret_ = std::min(caret, text_.size());
	anchor_ = std::min(anchor, text_.size());
	undo_.clear();
	redo_.clear();
	last_edit_ = EditKind::None;
}

void EditModel::set_caret(size_t caret, bool extend)
{
	caret_ = std::min(caret, text_.size());
	if (!extend)
		anchor_ = caret_;
	last_edit_ = EditKind::None;
	pending_.clear();
}

void EditModel::select(size_t anchor, size_t caret)
{
	anchor_ = std::min(anchor, text_.size());
	set_caret(caret, true);
}

void EditModel::push_undo()
{
	undo_.push_back(snapshot());
	constexpr size_t undo_limit = 200;
	if (undo_.size() > undo_limit)
		undo_.erase(undo_.begin());
	redo_.clear();
}

bool EditModel::replace(size_t from, size_t to, const std::string &with, EditKind kind)
{
	from = std::min(from, text_.size());
	to = std::clamp(to, from, text_.size());
	if (from == to && with.empty())
		return false;
	// Runs of typing, or of deleting, undo together, a word at a time.
	const bool word_break = kind == EditKind::Typing && (with == " " || with == "\n");
	if (kind == EditKind::Other || kind != last_edit_ || undo_.empty() || word_break)
		push_undo();
	else
		redo_.clear();
	last_edit_ = kind;

	text_.replace(from, to - from, with);
	edit(spans_, from, to, with.size());
	// Styles chosen with nothing selected go on what's typed.
	for (const StyleChange &c : pending_)
		apply(spans_, from, from + with.size(), c, text_);
	caret_ = anchor_ = from + with.size();
	column = -1;
	return true;
}

bool EditModel::replace_selection(const std::string &with, EditKind kind)
{
	const bool changed = replace(sel_from(), sel_to(), with, has_selection() ? EditKind::Other : kind);
	last_edit_ = kind;
	return changed;
}

void EditModel::restore(Snapshot s)
{
	text_ = std::move(s.text);
	spans_ = std::move(s.spans);
	caret_ = s.caret;
	anchor_ = s.anchor;
	last_edit_ = EditKind::None;
	pending_.clear();
}

bool EditModel::undo()
{
	if (undo_.empty())
		return false;
	redo_.push_back(snapshot());
	Snapshot s = std::move(undo_.back());
	undo_.pop_back();
	restore(std::move(s));
	return true;
}

bool EditModel::redo()
{
	if (redo_.empty())
		return false;
	undo_.push_back(snapshot());
	Snapshot s = std::move(redo_.back());
	redo_.pop_back();
	restore(std::move(s));
	return true;
}

Span EditModel::style_here() const
{
	return style_at(spans_, has_selection() ? sel_from() : caret_ > 0 ? caret_ - 1 : 0);
}

// What's typed next: as last chosen for it (a Clear before it meaning "as
// the base"), or else as the character before the caret.
bool EditModel::typing_has(StyleChange::Field f, const BaseStyle &base) const
{
	bool cleared = false;
	int chosen = -2; // none chosen
	for (const StyleChange &c : pending_) {
		if (c.field == StyleChange::Clear) {
			cleared = true;
			chosen = -2;
		} else if (c.field == f) {
			chosen = c.value;
		}
	}
	if (chosen == 1)
		return true;
	if (chosen == 0)
		return false;
	if (chosen == -1 || cleared || caret_ == 0)
		return base.has(f);
	return all_on(spans_, caret_ - 1, caret_, f, base.has(f));
}

bool EditModel::has(StyleChange::Field f, const BaseStyle &base) const
{
	return has_selection() ? all_on(spans_, sel_from(), sel_to(), f, base.has(f)) : typing_has(f, base);
}

int EditModel::size_percent() const
{
	if (!has_selection()) {
		bool cleared = false;
		int chosen = -1;
		for (const StyleChange &c : pending_) {
			if (c.field == StyleChange::Clear) {
				cleared = true;
				chosen = -1;
			} else if (c.field == StyleChange::Size) {
				chosen = c.value;
			}
		}
		if (chosen >= 0)
			return chosen ? chosen : 100;
		if (cleared)
			return 100;
	}
	const Span at = style_here();
	return at.size ? at.size : 100;
}

uint32_t EditModel::colour(uint32_t base) const
{
	const Span here = style_here();
	uint32_t c = here.has_colour ? here.colour : base;
	if (!has_selection()) {
		for (const StyleChange &p : pending_) {
			if (p.field == StyleChange::Clear)
				c = base;
			else if (p.field == StyleChange::Colour)
				c = p.no_colour ? base : p.colour;
		}
	}
	return c;
}

bool EditModel::style(StyleChange change, bool toggle, const BaseStyle &base)
{
	// Toggled off: back to the base where the base hasn't it, or explicitly
	// off where it has (a bold font's text made regular).
	const bool from_base = toggle && base.has(change.field);
	if (toggle)
		change.value = has(change.field, base) ? (from_base ? 0 : -1) : (from_base ? -1 : 1);
	if (!has_selection()) {
		// The latest of each kind wins; clearing starts afresh.
		if (change.field == StyleChange::Clear)
			pending_.clear();
		std::erase_if(pending_, [&](const StyleChange &c) { return c.field == change.field; });
		pending_.push_back(change);
		return false;
	}
	push_undo();
	last_edit_ = EditKind::Other;
	apply(spans_, sel_from(), sel_to(), change, text_);
	return true;
}

bool EditModel::step_size(int direction)
{
	StyleChange c;
	c.field = StyleChange::Size;
	if (has_selection()) {
		c.relative = true;
		c.value = 10 * direction;
	} else {
		c.value = std::clamp(size_percent() + 10 * direction, 10, 1000);
		if (c.value == 100)
			c.value = 0;
	}
	return style(c, false, {});
}

} // namespace typeset
