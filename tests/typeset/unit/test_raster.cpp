// Tests for Typeset Text's drawing and Backspace (plugins/obs-typeset/src/
// text-engine.cpp): Pango and Cairo, no OBS. Built by run.sh with Pango.
#include "text-engine.hpp"

#include <cmath>
#include <cstdio>
#include <vector>
#include <string>

using namespace typeset;

static int failed = 0;

static void check(const char *what, bool ok, const std::string &detail = "")
{
	printf("%s  %s%s%s\n", ok ? "ok   " : "FAIL ", what, detail.empty() ? "" : ": ", detail.c_str());
	failed += !ok;
}

static Document text_doc(const std::string &text)
{
	Document d;
	d.text = text;
	d.style.family = "Sans";
	d.style.size = 48;
	return d;
}

static Raster draw(PangoContext *context, const Document &d, double scale)
{
	PangoLayout *layout = create_layout(context, d);
	RasterParams p;
	p.sx = p.sy = scale;
	Raster r = rasterise(layout, d, p);
	g_object_unref(layout);
	return r;
}

// Pixels with any ink (alpha above zero).
static long inked(const Raster &r)
{
	long n = 0;
	for (size_t i = 3; i < r.pixels.size(); i += 4)
		n += r.pixels[i] > 0;
	return n;
}

static size_t backspace(PangoContext *context, const std::string &text)
{
	PangoLayout *layout = create_layout(context, text_doc(text));
	const size_t at = backspace_index(layout, text.size());
	g_object_unref(layout);
	return at;
}

// A wrapped text's lines: their widths (px) and texts.
struct Lines {
	std::vector<double> widths;
	std::vector<std::string> texts;
};
static Lines lines_of(PangoContext *context, const Document &d)
{
	PangoLayout *layout = create_layout(context, d);
	Lines out;
	for (int i = 0; i < pango_layout_get_line_count(layout); i++) {
		PangoLayoutLine *line = pango_layout_get_line_readonly(layout, i);
		PangoRectangle ink, logical;
		pango_layout_line_get_extents(line, &ink, &logical);
		out.widths.push_back(logical.width / double(PANGO_SCALE));
		out.texts.push_back(d.text.substr(size_t(line->start_index), size_t(line->length)));
	}
	g_object_unref(layout);
	return out;
}

static Document boxed(const std::string &text, double width)
{
	Document d = text_doc(text);
	d.style.box = true;
	d.style.box_width = width;
	d.style.overflow = Overflow::Wrap;
	return d;
}

static size_t words(std::string s)
{
	size_t n = 0;
	bool in = false;
	for (char c : s) {
		const bool letter = c != ' ' && c != '\n';
		n += letter && !in;
		in = letter;
	}
	return n;
}

int main()
{
	PangoFontMap *map = pango_cairo_font_map_new();
	PangoContext *context = create_context(map);

	// Drawing.
	const Document plain = text_doc("Hello");
	const Raster one = draw(context, plain, 1), two = draw(context, plain, 2);
	check("a normal drawing doesn't fail", !one.failed && !two.failed && one.width > 0);
	check("drawn at twice the scale, twice the size",
	      std::abs(two.width - 2 * one.width) <= 3 && std::abs(two.height - 2 * one.height) <= 3,
	      std::to_string(one.width) + "x" + std::to_string(one.height) + " -> " + std::to_string(two.width) + "x" +
		      std::to_string(two.height));

	Document shadowed = text_doc("Il");
	shadowed.style.shadow = true;
	shadowed.style.shadow_colour = 0xFF000000;
	shadowed.style.shadow_y = 0;
	shadowed.style.shadow_blur = 0;
	shadowed.style.shadow_spread = 0;
	const Raster tight = draw(context, shadowed, 1);
	shadowed.style.shadow_spread = 10;
	const Raster spread = draw(context, shadowed, 1);
	check("spread makes the shadow reach further", inked(spread) > inked(tight) * 2,
	      std::to_string(inked(tight)) + " -> " + std::to_string(inked(spread)) + " inked pixels");
	check("and the drawing grows to hold it", spread.width >= tight.width + 18,
	      std::to_string(tight.width) + " -> " + std::to_string(spread.width));

	// Superscript: the run is raised (its ink reaches higher on the canvas).
	{
		Document line = text_doc("xx");
		const Raster flat = draw(context, line, 1);
		Span up{1, 2};
		up.script = 1;
		line.spans = {up};
		const Raster raised = draw(context, line, 1);
		auto ink_top = [](const Raster &r) {
			for (int y = 0; y < r.height; y++)
				for (int x = 0; x < r.width; x++)
					if (r.pixels[(size_t(y) * size_t(r.width) + size_t(x)) * 4 + 3] > 0)
						return r.y0 + y;
			return r.y0 + r.height;
		};
		check("a superscript is drawn higher", ink_top(raised) < ink_top(flat) - 5,
		      std::to_string(ink_top(flat)) + " -> " + std::to_string(ink_top(raised)));
	}

	// Backspace, as Pango 1.52 divides text: whole graphemes, except where
	// it says one character goes (Indic scripts).
	check("Backspace after an accented letter (e + U+0301) takes the whole letter",
	      backspace(context, "ae\xcc\x81") == 1);
	check("Backspace after a Devanagari syllable takes only its vowel sign",
	      backspace(context, "\xe0\xa4\x95\xe0\xa4\xbf") == 3);
	check("Backspace after an emoji sequence takes all of it",
	      backspace(context, "a\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7") == 1);
	check("Backspace at the start stays there", backspace(context, "") == 0);

	// Wrapping: balanced lines, no lone last word, justified.
	{
		const std::string title = "The Grove presents an evening of live music tonight";
		Document d = boxed(title, 900);
		const Lines ragged = lines_of(context, d);
		d.style.balance = true;
		const Lines even = lines_of(context, d);
		auto spread = [](const Lines &l) {
			double lo = 1e9, hi = 0;
			for (double w : l.widths) {
				lo = std::min(lo, w);
				hi = std::max(hi, w);
			}
			return hi - lo;
		};
		check("balanced lines keep their number", even.widths.size() == ragged.widths.size(),
		      std::to_string(ragged.widths.size()) + " -> " + std::to_string(even.widths.size()));
		check("balanced lines are more even", spread(even) < spread(ragged) * 0.6,
		      std::to_string(spread(ragged)) + " -> " + std::to_string(spread(even)));

		// A width that leaves "tonight" alone on the last line.
		double alone = 0;
		for (double w = 400; w < 1400 && alone == 0; w += 10) {
			const Lines l = lines_of(context, boxed(title, w));
			if (l.texts.size() > 1 && words(l.texts.back()) == 1)
				alone = w;
		}
		check("(a width found with a lone last word)", alone > 0);
		if (alone > 0) {
			Document o = boxed(title, alone);
			o.style.avoid_orphans = true;
			const Lines l = lines_of(context, o);
			check("no lone last word", words(l.texts.back()) >= 2, l.texts.back());
			check("and no more lines", l.texts.size() == lines_of(context, boxed(title, alone)).texts.size());
		}

		// A box narrower than a word: the word stays whole, unhyphenated.
		{
			const Lines l = lines_of(context, boxed("Live tonight", 60));
			bool whole = l.texts.size() == 2;
			for (const std::string &t : l.texts)
				whole = whole && t.find('-') == std::string::npos;
			check("a word is never broken or hyphenated", whole,
			      l.texts.empty() ? "" : l.texts[0] + "|" + l.texts.back());
			const double widest = widest_word(context, boxed("Live tonight", 60));
			check("the widest word is the narrowest a box can be", widest > 60 && widest < 400,
			      std::to_string(widest));
		}

		Document j = boxed(title, 700);
		j.style.align = Align::Justified;
		const Lines just = lines_of(context, j);
		check("justified lines fill the box (not the last)",
		      just.widths.size() > 1 && std::fabs(just.widths[0] - 700) < 2,
		      just.widths.empty() ? "" : std::to_string(just.widths[0]));
	}

	{
		// The coverage form, painted as the effect paints it, is the colour
		// form's picture; text it can't hold stays in colour.
		auto paint = [](const Raster &c, int x, int y, int ch) {
			const size_t i = (size_t(y) * size_t(c.width) + size_t(x)) * size_t(c.bytes);
			auto colour = [](uint32_t v, int ch) { // ch: 0 B, 1 G, 2 R, 3 A, premultiplied
				const double a = double(v >> 24) / 255;
				const int shift[] = {16, 8, 0};
				return ch == 3 ? a : double((v >> shift[ch]) & 0xFF) / 255 * a;
			};
			const double fc = double(c.pixels[i]) / 255, sc = c.bytes == 2 ? double(c.pixels[i + 1]) / 255 : 0;
			const double fa = colour(c.fill_colour, 3) * fc;
			return 255 * (colour(c.fill_colour, ch) * fc + colour(c.shadow_colour, ch) * sc * (1 - fa));
		};
		auto compare = [&](const Document &d, double scale, const char *what) {
			PangoLayout *layout = create_layout(context, d);
			RasterParams p;
			p.sx = p.sy = scale;
			p.ox = 0.25;
			const Raster colour = rasterise(layout, d, p), cov = rasterise(layout, d, p, true);
			g_object_unref(layout);
			double worst = 0;
			// (Up to 3 clear pixels wider: rows of whole 4-byte words.)
			bool same_size = cov.width >= colour.width && cov.width <= colour.width + 3 &&
					 colour.height == cov.height && cov.bytes != 4;
			for (int y = 0; same_size && y < cov.height; y++)
				for (int x = 0; x < cov.width; x++)
					for (int ch = 0; ch < 4; ch++) {
						const double want = x < colour.width
							? colour.pixels[(size_t(y) * size_t(colour.width) + size_t(x)) * 4 + size_t(ch)] : 0.0;
						worst = std::max(worst, std::fabs(paint(cov, x, y, ch) - want));
					}
			check(what, same_size && worst <= 2.0, "largest difference " + std::to_string(worst));
		};
		Document plain = text_doc("Typeset 19:30");
		plain.style.colour = 0xFF40C0F0;
		compare(plain, 1.0, "coverage form: plain text, as in colour");
		Document shadowed = plain;
		shadowed.style.shadow = true;
		shadowed.style.shadow_colour = 0xBF102030;
		shadowed.style.shadow_y = 3;
		shadowed.style.shadow_blur = 6;
		shadowed.style.shadow_spread = 2;
		compare(shadowed, 1.0, "coverage form: with a shadow, as in colour");
		compare(shadowed, 2.3, "coverage form: scaled, as in colour");
		Document seethrough = shadowed;
		seethrough.style.colour = 0x80FFFFFF;
		compare(seethrough, 1.0, "coverage form: see-through text over its shadow, as in colour");

		Document outlined = shadowed;
		outlined.style.outline = true;
		PangoLayout *layout = create_layout(context, outlined);
		check("an outlined text stays in colour", rasterise(layout, outlined, RasterParams(), true).bytes == 4);
		g_object_unref(layout);
		Document emoji = text_doc("Live \xF0\x9F\x8E\xB8");
		layout = create_layout(context, emoji);
		const Raster e = rasterise(layout, emoji, RasterParams(), true);
		g_object_unref(layout);
		check("a text with colour emoji stays in colour (if there's a colour emoji font)", e.bytes == 4 || e.bytes == 1,
		      std::to_string(e.bytes));
	}

	g_object_unref(context);
	g_object_unref(map);
	printf("%s\n", failed ? "FAILED" : "all passed");
	return failed ? 1 : 0;
}
