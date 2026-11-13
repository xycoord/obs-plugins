// Tests for Typeset Text's styled runs (plugins/obs-typeset/src/rich-text.cpp):
// plain logic, no OBS. Built and run by tests/typeset/unit/run.sh.
#include "rich-text.hpp"

#include <cstdio>
#include <string>

using namespace typeset;

static int failed = 0;

static std::string show(const Spans &spans)
{
	std::string out;
	for (const Span &s : spans) {
		out += "[" + std::to_string(s.start) + "," + std::to_string(s.end) + ")";
		if (s.bold > 0)
			out += "b";
		if (s.italic > 0)
			out += "i";
		if (s.underline > 0)
			out += "u";
		if (s.has_colour)
			out += "c";
		if (s.weight)
			out += "w" + std::to_string(s.weight);
		if (s.size > 0) {
			char size[32];
			snprintf(size, sizeof(size), "s%g", s.size);
			out += size;
		}
		if (s.script)
			out += s.script == 1 ? "^" : "_";
		out += " ";
	}
	return out.empty() ? "(none)" : out.substr(0, out.size() - 1);
}

static void expect(const char *what, const Spans &spans, const std::string &want)
{
	const std::string got = show(spans);
	if (got == want) {
		printf("ok    %s\n", what);
	} else {
		printf("FAIL  %s: got %s, want %s\n", what, got.c_str(), want.c_str());
		failed++;
	}
}

static StyleChange bold(int v = 1)
{
	StyleChange c;
	c.field = StyleChange::Bold;
	c.value = v;
	return c;
}

int main()
{
	const std::string text = "The Grove live";
	Spans s;
	apply(s, 4, 9, bold(), text);
	expect("bold on a word", s, "[4,9)b");

	Spans t = s;
	edit(t, 9, 9, 3); // typed at the end of the bold word
	expect("typing at the end of a bold word continues it", t, "[4,12)b");

	t = s;
	edit(t, 4, 4, 2); // typed at the start of it
	expect("typing just before a bold word isn't bold", t, "[6,11)b");

	t = s;
	edit(t, 6, 6, 1); // inside it
	expect("typing inside a bold word is bold", t, "[4,10)b");

	t = s;
	edit(t, 0, 3, 1); // "The" replaced by one byte before it
	expect("replacing text before a run moves it", t, "[2,7)b");

	t = s;
	edit(t, 5, 7, 0); // two bytes deleted inside
	expect("deleting inside a run shortens it", t, "[4,7)b");

	t = s;
	edit(t, 4, 9, 4); // the bold word replaced
	expect("replacing a bold word keeps it bold", t, "[4,8)b");

	t = s;
	edit(t, 2, 11, 0); // deleted across the run
	expect("deleting across a run removes it", t, "(none)");

	t = s;
	apply(t, 6, 12, bold(), text);
	expect("bold over a run and beyond merges", t, "[4,12)b");

	t = s;
	apply(t, 4, 9, bold(-1), text);
	expect("bold off removes the run", t, "(none)");

	t = s;
	StyleChange red;
	red.field = StyleChange::Colour;
	red.colour = 0xFF0000FF;
	apply(t, 7, 14, red, text);
	expect("colour over part of a bold run splits it", t, "[4,7)b [7,9)bc [9,14)c");

	if (!all_on(s, 4, 9, StyleChange::Bold) || all_on(s, 3, 9, StyleChange::Bold)) {
		printf("FAIL  all_on\n");
		failed++;
	} else {
		printf("ok    all_on\n");
	}

	t = s;
	follow_text(t, text, "The Old Grove live");
	expect("text changed elsewhere before a run moves it", t, "[8,13)b");

	t = s;
	follow_text(t, text, "The Grove is live");
	expect("text changed elsewhere after a run leaves it", t, "[4,9)b");

	// UTF-8: a run never splits a character.
	const std::string utf = "Caf\xc3\xa9 Noir"; // "Café Noir"
	Spans u;
	apply(u, 0, 4, bold(), utf); // would end inside "é"
	expect("runs end on a character boundary", u, "[0,3)b");

	// Overlapping runs from outside (a script, a hand-edited scene): the
	// first keeps its bytes; later ones start after the furthest end so far.
	{
		const std::string ten = "0123456789ab";
		Span a{0, 10};
		a.bold = 1;
		Span b{2, 4};
		b.italic = 1;
		Span c{5, 8};
		c.underline = 1;
		Spans o{a, b, c};
		normalise(o, ten);
		expect("runs inside an earlier run are dropped", o, "[0,10)b");
		Span d{8, 12};
		d.italic = 1;
		o = {a, b, d};
		normalise(o, ten);
		expect("a run reaching past an earlier one keeps the rest", o, "[0,10)b [10,12)i");
		o = {a, b, c};
		edit(o, 11, 11, 1); // through edit too
		expect("edit untangles overlaps as well", o, "[0,10)b");
	}

	// Weights: one of its own replaces bold, and bold replaces it.
	{
		const std::string w = "Light and Black";
		StyleChange weight;
		weight.field = StyleChange::Weight;
		weight.value = 900;
		Spans r;
		apply(r, 0, 5, bold(), w);
		apply(r, 0, 5, weight, w);
		expect("a weight replaces bold", r, "[0,5)w900");
		apply(r, 0, 5, bold(), w);
		expect("and bold replaces a weight", r, "[0,5)b");
		weight.value = 0;
		apply(r, 0, 5, weight, w);
		expect("weight 0 is the base's", r, "(none)");
	}
	// Superscript, sizes in fractions of a percent, and Clear.
	{
		const std::string w = "E = mc2";
		StyleChange sup;
		sup.field = StyleChange::Script;
		sup.value = 1;
		StyleChange size;
		size.field = StyleChange::Size;
		size.amount = 112.5;
		StyleChange weight;
		weight.field = StyleChange::Weight;
		weight.value = 300;
		Spans r;
		apply(r, 6, 7, sup, w);
		expect("superscript", r, "[6,7)^");
		apply(r, 4, 7, size, w);
		apply(r, 4, 7, weight, w);
		expect("a size of 112.5% and a weight over it", r, "[4,6)w300s112.5 [6,7)w300s112.5^");
		StyleChange step;
		step.field = StyleChange::Size;
		step.relative = true;
		step.amount = -12.5;
		apply(r, 4, 6, step, w);
		expect("a step back to 100% is no size of its own", r, "[4,6)w300 [6,7)w300s112.5^");
		StyleChange clear;
		clear.field = StyleChange::Clear;
		apply(r, 0, 7, clear, w);
		expect("Clear takes off weight, size and script too", r, "(none)");
	}
	// uniform(): one value over a range, gaps counting as plain text.
	{
		const std::string w = "abcdef";
		StyleChange weight;
		weight.field = StyleChange::Weight;
		weight.value = 600;
		Spans r;
		apply(r, 1, 3, weight, w);
		auto get = [](const Span &s) { return double(s.weight); };
		double v = -1;
		const bool same = uniform(r, 1, 3, get, &v) && v == 600;
		const bool mixed = !uniform(r, 0, 3, get, &v);
		const bool caret = uniform(r, 3, 3, get, &v) && v == 600; // what's typed after it
		const bool plain = uniform(r, 4, 6, get, &v) && v == 0;
		if (same && mixed && caret && plain) {
			printf("ok    uniform\n");
		} else {
			printf("FAIL  uniform: %d %d %d %d\n", same, mixed, caret, plain);
			failed++;
		}
	}

	printf("%s\n", failed ? "FAILED" : "all passed");
	return failed ? 1 : 0;
}
