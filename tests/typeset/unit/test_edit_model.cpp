// Tests for Typeset Text's editing model (plugins/obs-typeset/src/edit-model.cpp)
// and where the canvas sits in OBS's preview (preview-geometry.hpp): plain
// logic, no OBS. Built and run by tests/typeset/unit/run.sh.
#include "edit-model.hpp"
#include "preview-geometry.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace typeset;

static int failed = 0;

static void check(const char *what, bool ok, const std::string &detail = "")
{
	if (ok) {
		printf("ok    %s\n", what);
	} else {
		printf("FAIL  %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
		failed++;
	}
}

static void type(EditModel &m, const std::string &s)
{
	for (char c : s)
		m.replace_selection(std::string(1, c), EditKind::Typing);
}

static StyleChange field(StyleChange::Field f)
{
	StyleChange c;
	c.field = f;
	return c;
}

int main()
{
	const BaseStyle regular;
	BaseStyle bold_font;
	bold_font.bold = true;

	{
		EditModel m;
		m.start("", {});
		type(m, "hello world");
		check("typing", m.text() == "hello world" && m.caret() == 11);
		m.undo();
		check("undo takes off a word at a time", m.text() == "hello", m.text());
		m.undo();
		check("then the word before", m.text().empty(), m.text());
		m.redo();
		m.redo();
		check("redo puts both back", m.text() == "hello world", m.text());
	}
	{
		EditModel m;
		m.start("hello", {});
		m.set_caret(5, false);
		m.style(field(StyleChange::Bold), true, regular);
		check("Ctrl+B with nothing selected changes nothing yet", m.spans().empty());
		check("but the bar shows bold for what's typed next", m.has(StyleChange::Bold, regular));
		type(m, "!!");
		check("what's typed next is bold",
		      m.spans().size() == 1 && m.spans()[0].start == 5 && m.spans()[0].end == 7 && m.spans()[0].bold == 1);
		m.set_caret(0, false);
		check("moving the caret forgets the choice", m.pending().empty());
	}
	{
		// Bold off on a bold font: the selection is made regular explicitly.
		EditModel m;
		m.start("heavy", {});
		m.select(0, 5);
		check("a bold font's text shows bold", m.has(StyleChange::Bold, bold_font));
		m.style(field(StyleChange::Bold), true, bold_font);
		check("toggled, it's explicitly not bold",
		      m.spans().size() == 1 && m.spans()[0].bold == 0 && !m.has(StyleChange::Bold, bold_font));
		m.style(field(StyleChange::Bold), true, bold_font);
		check("toggled again, it's back to the font's own", m.spans().empty() && m.has(StyleChange::Bold, bold_font));
	}
	{
		// Half bold: toggling makes all of it bold first.
		EditModel m;
		m.start("ab", {});
		m.select(0, 1);
		m.style(field(StyleChange::Bold), true, regular);
		m.select(0, 2);
		check("half bold isn't shown as bold", !m.has(StyleChange::Bold, regular));
		m.style(field(StyleChange::Bold), true, regular);
		check("toggling makes all of it bold", m.has(StyleChange::Bold, regular));
	}
	{
		// Size steps on a selection go from each part's own size.
		EditModel m;
		m.start("abcd", {});
		m.select(0, 2);
		m.step_size(1);
		m.select(0, 4);
		m.step_size(1);
		const Spans &s = m.spans();
		check("each part steps from its own size",
		      s.size() == 2 && s[0].size == 120 && s[1].size == 110,
		      s.size() == 2 ? std::to_string(s[0].size) + "," + std::to_string(s[1].size) : "runs");
		m.select(2, 4);
		m.step_size(-1);
		check("back to 100% is no size of its own", m.spans().size() == 1 && m.spans()[0].end == 2);
	}
	{
		// Clear, then a colour: the bar shows the colour; then Clear again.
		EditModel m;
		m.start("ab", {});
		m.set_caret(2, false);
		StyleChange c = field(StyleChange::Colour);
		c.colour = 0xFF0000FF;
		m.style(c, false, regular);
		check("the colour chosen for what's typed next shows", m.colour(0xFFFFFFFF) == 0xFF0000FF);
		m.style(field(StyleChange::Clear), false, regular);
		check("cleared, the text's own colour shows", m.colour(0xFFFFFFFF) == 0xFFFFFFFF);
	}
	{
		// Changed from elsewhere: the caret kept, undo starts afresh.
		EditModel m;
		m.start("", {});
		type(m, "abc");
		m.take("abcdef", {}, 3, 3);
		check("taken from elsewhere", m.text() == "abcdef" && m.caret() == 3);
		check("undo doesn't go back past it", !m.undo());
	}
	{
		// Weights: an explicit one for the selection, shown as drawn; B
		// over it goes back to bold.
		EditModel m;
		m.start("light heavy", {});
		m.select(0, 5);
		m.set_weight(300);
		check("a weight for the selection", m.weight(400) == 300 && m.spans().size() == 1 && m.spans()[0].weight == 300);
		m.select(0, 11);
		check("mixed weights show as mixed", m.weight(400) == 0);
		m.select(6, 11);
		m.style(field(StyleChange::Bold), true, regular);
		check("bold on regular is drawn Bold", m.weight(400) == 700);
		m.set_caret(5, false);
		check("typing after a Light word is Light", m.weight(400) == 300);
	}
	{
		// Superscript and subscript: on, off, and one in place of the other.
		EditModel m;
		m.start("21st", {});
		m.select(2, 4);
		m.toggle_script(1);
		check("superscript on", m.script() == 1 && m.spans().size() == 1 && m.spans()[0].script == 1);
		m.toggle_script(2);
		check("subscript in its place", m.script() == 2);
		m.toggle_script(2);
		check("and off", m.script() == 0 && m.spans().empty());
		m.set_caret(4, false);
		m.toggle_script(1);
		check("superscript for what's typed next", m.script() == 1);
		type(m, "!");
		check("what's typed is raised", m.spans().size() == 1 && m.spans()[0].start == 4 && m.spans()[0].script == 1);
	}
	{
		// A typeface of its own for the selection; mixed shows as mixed;
		// Clear goes back to the text's own.
		EditModel m;
		m.start("band name", {});
		m.select(0, 4);
		m.set_family("DejaVu Serif");
		std::string f;
		check("a typeface for the selection", m.family(&f) && f == "DejaVu Serif");
		m.select(0, 9);
		check("mixed typefaces show as mixed", !m.family(&f));
		m.select(0, 4);
		m.style(field(StyleChange::Clear), false, regular);
		check("Clear goes back to the text's own", m.family(&f) && f.empty() && m.spans().empty());
	}
	{
		// A size typed in pixels is kept to the fraction.
		EditModel m;
		m.start("ab", {});
		m.select(0, 1);
		m.set_size_percent(100.0 * 100 / 96);
		check("100 px on 96 px text", m.spans().size() == 1 && std::fabs(m.spans()[0].size * 96 / 100 - 100) < 1e-6);
	}
	{
		// The strip's alignment and box width undo with the words.
		EditModel m;
		m.start("ab", {}, {0, 300});
		m.set_caret(2, false);
		type(m, "c");
		m.set_align(1);
		m.begin_box_drag();
		m.set_box_width(320);
		m.set_box_width(340);
		check("the strip's changes are kept", m.layout() == EditModel::Layout{1, 340});
		m.undo();
		check("one undo takes back a whole grip drag", m.layout() == EditModel::Layout{1, 300} && m.text() == "abc");
		m.undo();
		check("the next, the alignment", m.layout() == EditModel::Layout{0, 300} && m.text() == "abc");
		m.undo();
		check("then the typing", m.text() == "ab");
	}
	{
		bool cut = false;
		check("paste drops control characters", clean_paste("a\x01" "b\nc\td\x7f", &cut) == "ab\nc\td" && !cut);
		check("paste makes every line break a new line",
		      clean_paste("a\r\nb\rc\u2028" "d\u2029" "e") == "a\nb\nc\nd\ne", clean_paste("a\r\nb\rc\u2028" "d\u2029" "e"));
		std::string big;
		for (size_t i = 0; i < max_paste_chars + 10; i++)
			big += "\xC3\xA9"; // é, two bytes
		const std::string kept = clean_paste(big, &cut);
		check("a long paste is cut at a character's edge", cut && kept.size() == 2 * max_paste_chars,
		      std::to_string(kept.size()));
	}
	{
		// OBS's preview: 1920x1080 in a 1600x1000 widget, 10 px margin.
		const Mapping m = fit_canvas(1600, 1000, 1920, 1080, 1);
		check("canvas fitted to the width",
		      std::fabs(m.scale - 1580.0 / 1920) < 1e-6 && m.x == 10 && std::fabs(m.y - (490 - 444 + 10)) < 1e-9,
		      std::to_string(m.scale) + " " + std::to_string(m.x) + " " + std::to_string(m.y));
		const Mapping t = fit_canvas(800, 1000, 1080, 1920, 2);
		check("a tall canvas fitted to the height", std::fabs(t.scale - 980.0 / 1920) < 1e-6 && t.y == 10 && t.dpr == 2,
		      std::to_string(t.scale) + " " + std::to_string(t.y));
		const Mapping z = fit_canvas(15, 15, 1920, 1080, 1);
		check("a preview too small to show anything", z.scale == 1 && z.x == 0);
	}

	printf("%s\n", failed ? "FAILED" : "all passed");
	return failed ? 1 : 0;
}
