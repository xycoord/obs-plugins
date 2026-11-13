// Tests for how Typeset Text reads the font picker's style names into Pango
// (plugins/obs-typeset/src/text-engine.cpp). Built by run.sh with Pango.
#include "text-engine.hpp"

#include <obs.h>

#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
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

	// The weight a run is drawn at.
	auto weight = [](int8_t bold, int own, int base) {
		Span s;
		s.bold = bold;
		s.weight = own;
		return effective_weight(s, base);
	};
	auto check = [](const char *what, bool ok) {
		printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
		failed += !ok;
	};
	check("bold on Regular is Bold", weight(1, 0, 400) == 700);
	check("bold on SemiBold is Black", weight(1, 0, 600) == 900);
	check("bold off on Bold is Regular", weight(0, 0, 700) == 400);
	check("a weight of its own wins over bold", weight(1, 300, 400) == 300);
	check("no style is the base's weight", weight(-1, 0, 500) == 500);
	check("weight names", std::string(weight_name(400)) == "Regular" && std::string(weight_name(900)) == "Black" &&
				      std::string(weight_name(650)) == "Bold" && std::string(weight_name(100)) == "Thin");

	// The weights a family has (DejaVu Sans, which CI installs: Book and Bold
	// upright, and an ExtraLight in some packagings).
	PangoFontMap *map = pango_cairo_font_map_new();
	PangoContext *context = create_context(map);
	const std::vector<int> dejavu = family_weights(context, "DejaVu Sans", false);
	std::string listed;
	for (int w : dejavu)
		listed += std::to_string(w) + " ";
	const bool has_regular = std::find(dejavu.begin(), dejavu.end(), 400) != dejavu.end();
	const bool has_bold = std::find(dejavu.begin(), dejavu.end(), 700) != dejavu.end();
	printf("%s  DejaVu Sans has Regular and Bold, in order: %s\n",
	       has_regular && has_bold && std::is_sorted(dejavu.begin(), dejavu.end()) ? "ok   " : "FAIL ", listed.c_str());
	failed += !(has_regular && has_bold && std::is_sorted(dejavu.begin(), dejavu.end()));
	check("a family that isn't installed has no weights", family_weights(context, "No Such Family 123", false).empty());
	g_object_unref(context);
	g_object_unref(map);
	printf("%s\n", failed ? "FAILED" : "all passed");
	return failed ? 1 : 0;
}
