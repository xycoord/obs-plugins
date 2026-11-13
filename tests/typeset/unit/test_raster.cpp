// Tests for Typeset Text's drawing and Backspace (plugins/obs-typeset/src/
// text-engine.cpp): Pango and Cairo, no OBS. Built by run.sh with Pango.
#include "text-engine.hpp"

#include <cstdio>
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

	// Backspace, as Pango 1.52 divides text: whole graphemes, except where
	// it says one character goes (Indic scripts).
	check("Backspace after an accented letter (e + U+0301) takes the whole letter",
	      backspace(context, "ae\xcc\x81") == 1);
	check("Backspace after a Devanagari syllable takes only its vowel sign",
	      backspace(context, "\xe0\xa4\x95\xe0\xa4\xbf") == 3);
	check("Backspace after an emoji sequence takes all of it",
	      backspace(context, "a\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7") == 1);
	check("Backspace at the start stays there", backspace(context, "") == 0);

	g_object_unref(context);
	g_object_unref(map);
	printf("%s\n", failed ? "FAILED" : "all passed");
	return failed ? 1 : 0;
}
