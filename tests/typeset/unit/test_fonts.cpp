// Tests for how Typeset Text reads the font picker's style names into Pango
// (plugins/obs-typeset/src/text-engine.cpp). Built by run.sh with Pango.
#include "text-engine.hpp"

#include <obs.h>

#include <cstdio>
#include <cstring>

using namespace typeset;

static int failed = 0;

static void expect(const char *face_style, int weight, PangoStyle style, PangoStretch stretch, unsigned flags = 0)
{
	Style s;
	s.family = "Inter";
	s.face_style = face_style;
	s.font_flags = flags;
	PangoFontDescription *d = font_description(s);
	const bool ok = pango_font_description_get_weight(d) == weight && pango_font_description_get_style(d) == style &&
			pango_font_description_get_stretch(d) == stretch &&
			strcmp(pango_font_description_get_family(d), "Inter") == 0;
	printf("%s  \"%s\"%s: weight %d style %d stretch %d\n", ok ? "ok   " : "FAIL ", face_style,
	       flags ? " (with bold flag)" : "", pango_font_description_get_weight(d), pango_font_description_get_style(d),
	       pango_font_description_get_stretch(d));
	failed += !ok;
	pango_font_description_free(d);
}

int main()
{
	expect("Regular", 400, PANGO_STYLE_NORMAL, PANGO_STRETCH_NORMAL);
	expect("Bold Italic", 700, PANGO_STYLE_ITALIC, PANGO_STRETCH_NORMAL);
	expect("SemiBold Italic", 600, PANGO_STYLE_ITALIC, PANGO_STRETCH_NORMAL);
	expect("Extra Bold", 800, PANGO_STYLE_NORMAL, PANGO_STRETCH_NORMAL);
	expect("Condensed Light Oblique", 300, PANGO_STYLE_OBLIQUE, PANGO_STRETCH_CONDENSED);
	expect("Bold Display", 700, PANGO_STYLE_NORMAL, PANGO_STRETCH_NORMAL); // a word Pango doesn't know
	expect("SemiBold", 600, PANGO_STYLE_NORMAL, PANGO_STRETCH_NORMAL, OBS_FONT_BOLD); // the picker's bold flag
	expect("Regular", 700, PANGO_STYLE_NORMAL, PANGO_STRETCH_NORMAL, OBS_FONT_BOLD);
	printf("%s\n", failed ? "FAILED" : "all passed");
	return failed ? 1 : 0;
}
