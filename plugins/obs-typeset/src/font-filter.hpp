// The font browser's search, filters and order: plain logic over the font
// index (font-index.hpp), so it can be tested without Qt or OBS.
#pragma once

#include "font-index.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace typeset {

// A feeling counts from this strength (Google's tags run 0-100; weaker ones
// say more about the tagging than the face).
constexpr int feel_strong = 50;

struct FontQuery {
	std::string text;                // what's typed in search
	std::set<std::string> categories; // any of these ("Sans Serif"...); empty: any
	bool must_cover = false;         // only families that have every character of cover_text
	std::u32string cover_text;
	int need_weight = 0;             // only families with this weight (0: any)
	bool need_italic = false;        // only families with an italic
	std::set<std::string> feels;     // every one of these, strongly ("Fancy"); the strongest first
	enum class Order { Popular, Name } order = Order::Popular;
};

struct FontResults {
	std::vector<size_t> families; // indices into the index, best first
	int hidden = 0;               // left out only because they can't show the text
	int unrated = 0;              // left out by a feeling for having none listed (not Google's)
	std::u32string missing;       // the characters most often missing, for saying so
};

FontResults find_fonts(const std::vector<FamilyInfo> &families, const FontQuery &query);

// How well a family matches search text: 0 not at all, else higher is better.
// A feeling typed ("fancy") matches families strongly so, the strongest best.
int search_score(const FamilyInfo &f, const std::string &text);

// A family's strong feelings, strongest first.
std::vector<std::pair<std::string, int>> strong_feels(const FamilyInfo &f);

// Every feeling the families have strongly, with how many have it.
std::map<std::string, int> feel_counts(const std::vector<FamilyInfo> &families);

// UTF-8 to code points (invalid bytes skipped).
std::u32string to_u32(const std::string &s);

// The characters a font is asked to draw in s, each once, in the order they
// first appear (no spaces or line breaks): what "covers my text" checks.
std::u32string drawn_chars(const std::string &s);

// ASCII lowercase, for search.
std::string lower(const std::string &s);

} // namespace typeset
