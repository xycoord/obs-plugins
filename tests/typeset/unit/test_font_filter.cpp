// Tests for the font browser's search, filters and order
// (plugins/obs-typeset/src/font-filter.cpp): plain logic over made-up
// families. Built and run by tests/typeset/unit/run.sh.
#include "font-filter.hpp"

#include <cstdio>
#include <string>

using namespace typeset;

static int failed = 0;

static void check(const char *what, bool ok, const std::string &detail = "")
{
	printf("%s  %s%s%s\n", ok ? "ok   " : "FAIL ", what, detail.empty() ? "" : ": ", detail.c_str());
	failed += !ok;
}

static FamilyInfo family(const std::string &name, const std::string &category, int popularity,
			 std::vector<int> weights, bool italic, const std::string &extra = "")
{
	FamilyInfo f;
	f.name = name;
	f.category = category;
	f.popularity = popularity;
	f.weights = weights;
	f.italic = italic;
	f.search_name = name;
	for (char &c : f.search_name)
		c = char(tolower(static_cast<unsigned char>(c)));
	f.search_extra = extra;
	// Basic Latin, and an ö only where asked for (below).
	FcCharSet *cs = FcCharSetCreate();
	for (FcChar32 c = 0x20; c < 0x7F; c++)
		FcCharSetAddChar(cs, c);
	f.charset.reset(cs, FcCharSetDestroy);
	return f;
}

static std::string names(const std::vector<FamilyInfo> &all, const FontResults &r)
{
	std::string out;
	for (size_t i : r.families)
		out += (out.empty() ? "" : ", ") + all[i].name;
	return out;
}

int main()
{
	std::vector<FamilyInfo> all = {
		family("Playfair Display", "Serif", 22, {400, 900}, true, "serif didone calm claus eggers"),
		family("Play", "Sans Serif", 300, {400, 700}, false, "sans"),
		family("Playball", "Handwriting", 900, {400}, false, "handwriting informal"),
		family("Inter", "Sans Serif", 5, {}, true, "sans neo grotesque calm competent"),
		family("Roboto Slab", "Serif", 30, {100, 400, 900}, false, "serif slab geometric"),
		family("Space Mono", "Monospace", 200, {400, 700}, true, "monospace"),
		family("Local Font", "", 0, {400}, false, ""),
	};
	all[3].variable = true; // Inter: 100-900, variable
	all[3].var_lo = 100;
	all[3].var_hi = 900;
	FcCharSetAddChar(all[0].charset.get(), 0xF6); // Playfair has ö
	FcCharSetAddChar(all[3].charset.get(), 0xF6); // and Inter

	FontQuery q;
	FontResults r = find_fonts(all, q);
	check("nothing typed: every family, the most used first, unranked last by name",
	      names(all, r) == "Inter, Playfair Display, Roboto Slab, Space Mono, Play, Playball, Local Font",
	      names(all, r));

	q.order = FontQuery::Order::Name;
	r = find_fonts(all, q);
	check("or A-Z", names(all, r).rfind("Inter, Local Font, Play,", 0) == 0, names(all, r));
	q.order = FontQuery::Order::Popular;

	q.text = "pla";
	r = find_fonts(all, q);
	check("a name's start: those first, the most used first",
	      names(all, r).rfind("Playfair Display, Play, Playball", 0) == 0, names(all, r));
	q.text = "playfair display";
	r = find_fonts(all, q);
	check("the whole name first", !r.families.empty() && all[r.families[0]].name == "Playfair Display",
	      names(all, r));
	q.text = "robotoslab";
	r = find_fonts(all, q);
	check("spaces left out still finds it", names(all, r) == "Roboto Slab", names(all, r));
	q.text = "slab";
	r = find_fonts(all, q);
	check("a word in a name before a class", names(all, r) == "Roboto Slab", names(all, r));
	q.text = "calm";
	r = find_fonts(all, q);
	check("feel words find families", names(all, r) == "Inter, Playfair Display", names(all, r));
	q.text = "geometric";
	r = find_fonts(all, q);
	check("classes find families", names(all, r) == "Roboto Slab", names(all, r));
	q.text = "eggers";
	r = find_fonts(all, q);
	check("designers find families", names(all, r) == "Playfair Display", names(all, r));
	q.text = "plyfr";
	r = find_fonts(all, q);
	check("letters in order, with gaps", names(all, r) == "Playfair Display", names(all, r));
	q.text = "serif calm";
	r = find_fonts(all, q);
	check("every word has to match", names(all, r) == "Playfair Display", names(all, r));
	q.text = "zzz";
	check("no match, nothing", find_fonts(all, q).families.empty());
	q.text.clear();

	q.categories = {"Serif", "Monospace"};
	r = find_fonts(all, q);
	check("categories: any of those chosen", names(all, r) == "Playfair Display, Roboto Slab, Space Mono",
	      names(all, r));
	q.categories.clear();

	q.need_weight = 900;
	r = find_fonts(all, q);
	check("has the weight (a variable font's range counts)",
	      names(all, r) == "Inter, Playfair Display, Roboto Slab", names(all, r));
	q.need_weight = 0;
	q.need_italic = true;
	r = find_fonts(all, q);
	check("has an italic", names(all, r) == "Inter, Playfair Display, Space Mono", names(all, r));
	q.need_italic = false;

	q.must_cover = true;
	q.cover_text = to_u32("Ödön");
	r = find_fonts(all, q);
	check("covers the text: none has Ö, so all are hidden", r.families.empty() && r.hidden == 7 && r.missing == U"Ö",
	      names(all, r) + " hidden " + std::to_string(r.hidden));
	q.cover_text = to_u32("dön");
	r = find_fonts(all, q);
	check("covers the text: those without ö hidden, and counted", names(all, r) == "Inter, Playfair Display" &&
										   r.hidden == 5,
	      names(all, r) + " hidden " + std::to_string(r.hidden));
	check("the missing character named", r.missing == U"ö");
	q.cover_text = to_u32("Grove\n  live");
	r = find_fonts(all, q);
	check("spaces and line breaks aren't asked of a font", r.hidden == 0, std::to_string(r.hidden));

	check("the characters checked: each once, in order, no spaces or line breaks",
	      drawn_chars("Ödön\nÖdön  död") == U"Ödön");
	q.cover_text = drawn_chars("dön dön dön");
	r = find_fonts(all, q);
	check("and checking them finds what the whole text would", names(all, r) == "Inter, Playfair Display" &&
									   r.missing == U"ö",
	      names(all, r));

	// Weights by name: DejaVu's Book (380) is a Regular.
	FamilyInfo book = family("Book", "", 0, {380, 700}, false);
	check("a weight of the same name counts as had", book.has_weight(400) && !book.has_weight(500));
	check("the nearest weight", book.nearest_weight(600) == 700 && book.nearest_weight(300) == 380);
	check("a weight's name", std::string(weight_label(380)) == "Regular" &&
					 std::string(weight_label(500)) == "Medium");

	printf("%s\n", failed ? "FAILED" : "all passed");
	return failed ? 1 : 0;
}
