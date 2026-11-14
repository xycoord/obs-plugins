// The font browser's search, filters and order: plain logic over the font
// index (font-index.hpp), so it can be tested without Qt or OBS.
#pragma once

#include "font-index.hpp"

#include <set>
#include <string>
#include <vector>

namespace typeset {

struct FontQuery {
	std::string text;                // what's typed in search
	std::set<std::string> categories; // any of these ("Sans Serif"...); empty: any
	bool must_cover = false;         // only families that have every character of cover_text
	std::u32string cover_text;
	int need_weight = 0;             // only families with this weight (0: any)
	bool need_italic = false;        // only families with an italic
	enum class Order { Popular, Name } order = Order::Popular;
};

struct FontResults {
	std::vector<size_t> families; // indices into the index, best first
	int hidden = 0;               // left out only because they can't show the text
	std::u32string missing;       // the characters most often missing, for saying so
};

FontResults find_fonts(const std::vector<FamilyInfo> &families, const FontQuery &query);

// How well a family matches search text: 0 not at all, else higher is better.
int search_score(const FamilyInfo &f, const std::string &text);

// UTF-8 to code points (invalid bytes skipped).
std::u32string to_u32(const std::string &s);

} // namespace typeset
