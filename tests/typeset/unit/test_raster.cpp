// Tests for Typeset Text's drawing and Backspace (plugins/obs-typeset/src/
// text-engine.cpp): Pango and Cairo, no OBS. Built by run.sh with Pango.
#include "text-engine.hpp"

#include <obs.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
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
		// Runs that overlap one another (lines squeezed together, an italic
		// word leaning into the next, an underline through descenders): the
		// coverage form adds a run straight in only where it's drawn on clear
		// pixels, so where one lands on another it's drawn over, as in colour.
		Document overlapping = text_doc("Wolf Type jumpy fifty\nTight lines, quickly");
		overlapping.style.colour = 0xFF40C0F0;
		overlapping.style.line_spacing = 0.55;
		Span leaning(5, 9);
		leaning.italic = 1;
		Span under(10, 15);
		under.underline = 1;
		Span heavy(16, 21);
		heavy.bold = 1;
		overlapping.spans = {leaning, under, heavy};
		compare(overlapping, 1.0, "coverage form: overlapping runs and lines, as in colour");
		compare(overlapping, 2.3, "and scaled");
		overlapping.style.shadow = true;
		overlapping.style.shadow_colour = 0xBF102030;
		overlapping.style.shadow_blur = 0;
		overlapping.style.shadow_x = 1;
		compare(overlapping, 1.0, "and with a sharp shadow");


		// Letters' antialiased edges are whole in both forms: each pixel
		// as much as a drawing that adds every glyph's image whole (Cairo
		// does that into A8 with ADD; drawn any other way, it once cut off
		// a part-covered row or column at a letter's edge outside where it
		// reckoned the glyphs were). Large text, a second run (bold) and a
		// third (italic), at every eighth of a pixel each way.
		auto whole = [&](const Document &d, double scale, const char *what) {
			PangoLayout *layout = create_layout(context, d);
			const Frame f = frame_of(layout, d);
			double worst_colour = 0, worst_coverage = 0;
			bool ok = true;
			for (int i = 0; i < 8 && ok; i++)
				for (int j = 0; j < 8 && ok; j++) {
					RasterParams p;
					p.sx = p.sy = scale;
					p.ox = i / 8.0;
					p.oy = j / 8.0;
					const Raster colour = rasterise(layout, d, p), cov = rasterise(layout, d, p, true);
					cairo_surface_t *ref = cairo_image_surface_create(CAIRO_FORMAT_A8, colour.width, colour.height);
					cairo_t *cr = cairo_create(ref);
					cairo_translate(cr, p.ox - colour.x0, p.oy - colour.y0);
					cairo_scale(cr, p.sx, p.sy);
					cairo_translate(cr, f.text_x, f.text_y);
					cairo_scale(cr, f.text_scale, f.text_scale);
					cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
					pango_cairo_show_layout(cr, layout);
					cairo_destroy(cr);
					cairo_surface_flush(ref);
					const uint8_t *rd = cairo_image_surface_get_data(ref);
					const int rs = cairo_image_surface_get_stride(ref);
					ok = colour.bytes == 4 && cov.bytes == 1 && cov.x0 == colour.x0 && cov.y0 == colour.y0 &&
					     cov.height == colour.height && cov.width >= colour.width;
					for (int y = 0; ok && y < colour.height; y++)
						for (int x = 0; x < colour.width; x++) {
							const int want = rd[size_t(y) * size_t(rs) + size_t(x)];
							const int a = colour.pixels[(size_t(y) * size_t(colour.width) + size_t(x)) * 4 + 3];
							const int c = cov.pixels[size_t(y) * size_t(cov.width) + size_t(x)];
							worst_colour = std::max(worst_colour, double(std::abs(a - want)));
							worst_coverage = std::max(worst_coverage, double(std::abs(c - want)));
						}
					cairo_surface_destroy(ref);
				}
			g_object_unref(layout);
			check(what, ok && worst_colour <= 2 && worst_coverage <= 2,
			      "largest difference " + std::to_string(worst_colour) + " (colour form), " +
				      std::to_string(worst_coverage) + " (coverage form)");
		};
		Document big = text_doc("Typeset 19:30 jumpy");
		big.style.family = "DejaVu Sans";
		big.style.size = 120;
		big.style.colour = 0xFF40C0F0;
		Span bold(8, 13);
		bold.bold = 1;
		Span italic(14, 19);
		italic.italic = 1;
		big.spans = {bold, italic};
		whole(big, 1.0, "letters' edges are whole in both forms, any part of a pixel across or down");
		whole(big, 0.37, "and drawn small");
		Document small = big;
		small.style.size = 17;
		whole(small, 1.7, "and a small size scaled up");
		// A family with no bold face, which Cairo emboldens itself: its
		// letters reach past their metrics, and Cairo's own drawing cut them.
		if (font_installed(context, "IPAGothic", nullptr)) {
			Document thickened = big;
			thickened.style.family = "IPAGothic";
			thickened.style.face_style = "Bold";
			whole(thickened, 1.0, "and letters Cairo emboldens itself");
			whole(thickened, 2.3, "and those scaled");
		} else {
			printf("SKIPPED letters Cairo emboldens itself: no IPAGothic\n");
		}

		// Otherwise it's drawn as Pango's own renderer draws it: colours,
		// see-through runs, underlines, strikethrough, a second line, colour
		// emoji (where there's a font for them; Cairo puts those on whole
		// pixels). Letters spaced well apart, so that Cairo draws Pango's
		// whole too.
		{
			Document d = text_doc("Live tonight \xF0\x9F\x8E\xB8\nDoors 19:30");
			d.style.size = 40;
			d.style.letter_spacing = 60;
			d.style.colour = 0xFF40C0F0;
			Span red(0, 4);
			red.has_colour = true;
			red.colour = 0x800000FF;
			red.underline = 1;
			Span bold(18, 23);
			bold.bold = 1;
			d.spans = {red, bold};
			d.style.font_flags = OBS_FONT_STRIKEOUT;
			PangoLayout *layout = create_layout(context, d);
			PangoRectangle ink;
			pango_layout_get_pixel_extents(layout, &ink, nullptr);
			const int w = ink.x + ink.width + 20, h = ink.y + ink.height + 20;
			auto draw_with = [&](bool pango) {
				cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
				cairo_t *cr = cairo_create(s);
				cairo_translate(cr, 5.3, 6.7);
				cairo_set_source_rgba(cr, 0.94, 0.75, 0.25, 1);
				if (pango)
					pango_cairo_show_layout(cr, layout);
				else
					show_layout(cr, layout);
				cairo_destroy(cr);
				cairo_surface_flush(s);
				return s;
			};
			cairo_surface_t *a = draw_with(true), *b = draw_with(false);
			const uint8_t *ad = cairo_image_surface_get_data(a), *bd = cairo_image_surface_get_data(b);
			const int stride = cairo_image_surface_get_stride(a);
			int worst = 0;
			long inked = 0;
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w * 4; x++) {
					worst = std::max(worst, std::abs(ad[y * stride + x] - bd[y * stride + x]));
					inked += x % 4 == 3 && bd[y * stride + x] > 0;
				}
			check("drawn as Pango draws it (colours, see-through, underline, strikethrough, lines, emoji)",
			      worst <= 1 && inked > 1000, "largest difference " + std::to_string(worst));
			cairo_surface_destroy(a);
			cairo_surface_destroy(b);
			g_object_unref(layout);
		}

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
