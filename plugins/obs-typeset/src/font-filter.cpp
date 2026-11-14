#include "font-filter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

namespace typeset {

// --- What a family has (font-index.hpp) ------------------------------------------------------

bool FamilyInfo::has_weight(int w) const
{
	// The same weight by name counts (a "Book" of 380 is a Regular): what's
	// asked for is a weight's look, not its exact number.
	if (variable && w >= var_lo - 50 && w <= var_hi + 50)
		return true;
	for (int x : weights)
		if (strcmp(weight_label(x), weight_label(w)) == 0)
			return true;
	return false;
}

int FamilyInfo::nearest_weight(int w) const
{
	if (variable && w >= var_lo && w <= var_hi)
		return w;
	int best = weights.empty() ? 400 : weights.front();
	for (int x : weights)
		if (std::abs(x - w) < std::abs(best - w))
			best = x;
	return best;
}

// Spaces, line breaks and other controls are no font's to draw.
static bool drawn(char32_t c)
{
	return !(c < 0x21 || (c >= 0x7F && c < 0xA0) || c == 0x200B || c == 0x200C || c == 0x200D || c == 0xFEFF);
}

bool FamilyInfo::covers(const std::u32string &text) const
{
	if (!charset)
		return true; // unknown: not hidden
	for (char32_t c : text)
		if (drawn(c) && !FcCharSetHasChar(charset.get(), FcChar32(c)))
			return false;
	return true;
}

const char *weight_label(int w)
{
	static const struct {
		int w;
		const char *name;
	} names[] = {{100, "Thin"},     {200, "ExtraLight"}, {300, "Light"},     {400, "Regular"}, {500, "Medium"},
		     {600, "SemiBold"}, {700, "Bold"},       {800, "ExtraBold"}, {900, "Black"},   {1000, "ExtraBlack"}};
	const char *best = "Regular";
	int gap = 1 << 30;
	for (const auto &n : names)
		if (std::abs(n.w - w) < gap) {
			gap = std::abs(n.w - w);
			best = n.name;
		}
	return best;
}

// --- Search ------------------------------------------------------------------------------------

std::u32string to_u32(const std::string &s)
{
	std::u32string out;
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		if (n == 0 || i + size_t(n) > s.size()) {
			i++;
			continue;
		}
		char32_t cp = n == 1 ? c : c & (0x7F >> n);
		for (int k = 1; k < n; k++)
			cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
		out.push_back(cp);
		i += size_t(n);
	}
	return out;
}

namespace {

std::string lower(const std::string &s)
{
	std::string out = s;
	for (char &c : out)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

std::vector<std::string> words(const std::string &s)
{
	std::vector<std::string> out;
	std::istringstream in(lower(s));
	for (std::string w; in >> w;)
		out.push_back(w);
	return out;
}

bool starts_word(const std::string &hay, const std::string &needle)
{
	for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1))
		if (at == 0 || hay[at - 1] == ' ' || hay[at - 1] == '-')
			return true;
	return false;
}

// The letters in order, with gaps: "plfr" in "playfair". Closer is better.
int subsequence(const std::string &hay, const std::string &needle)
{
	size_t at = 0, span_start = std::string::npos;
	for (char c : needle) {
		at = hay.find(c, at);
		if (at == std::string::npos)
			return 0;
		if (span_start == std::string::npos)
			span_start = at;
		at++;
	}
	const int span = int(at - span_start);
	return std::max(1, 100 - (span - int(needle.size())) * 5);
}

std::string without_spaces(const std::string &s)
{
	std::string out;
	for (char c : s)
		if (c != ' ' && c != '-')
			out += c;
	return out;
}

} // namespace

int search_score(const FamilyInfo &f, const std::string &text)
{
	const std::vector<std::string> query = words(text);
	if (query.empty())
		return 1;
	const std::string &name = f.search_name;
	const std::string joined = without_spaces(lower(text));
	// The whole query against the name first ("open sans", "opensans").
	if (name == lower(text))
		return 10000;
	if (name.rfind(lower(text), 0) == 0)
		return 8000;
	if (without_spaces(name).rfind(joined, 0) == 0)
		return 7000;
	// Else every word has to match something; the name counts most.
	int score = 0;
	for (const std::string &w : query) {
		int s = 0;
		if (starts_word(name, w))
			s = 600;
		else if (name.find(w) != std::string::npos)
			s = 400;
		else if (starts_word(f.search_extra, w))
			s = 250; // a category, class, feel or designer ("slab", "calm")
		else if (w.size() >= 3)
			s = subsequence(without_spaces(name), w);
		if (s == 0)
			return 0;
		score += s;
	}
	return score;
}

FontResults find_fonts(const std::vector<FamilyInfo> &families, const FontQuery &q)
{
	FontResults out;
	std::map<char32_t, int> missing_count;
	std::vector<std::pair<int, size_t>> scored;
	for (size_t i = 0; i < families.size(); i++) {
		const FamilyInfo &f = families[i];
		if (!q.categories.empty() && !q.categories.count(f.category))
			continue;
		if (q.need_weight > 0 && !f.has_weight(q.need_weight))
			continue;
		if (q.need_italic && !f.italic)
			continue;
		const int score = search_score(f, q.text);
		if (score == 0)
			continue;
		if (q.must_cover && !f.covers(q.cover_text)) {
			out.hidden++;
			// Named only past plain ASCII: a family missing ordinary letters
			// has no Latin at all (a script's own), which isn't news.
			for (char32_t c : q.cover_text)
				if (c >= 0x80 && drawn(c) && f.charset && !FcCharSetHasChar(f.charset.get(), FcChar32(c))) {
					missing_count[c]++;
					break; // the first it lacks
				}
			continue;
		}
		scored.emplace_back(score, i);
	}
	const bool searching = !words(q.text).empty();
	std::stable_sort(scored.begin(), scored.end(), [&](const auto &a, const auto &b) {
		const FamilyInfo &fa = families[a.second], &fb = families[b.second];
		if (searching && a.first != b.first)
			return a.first > b.first;
		if (q.order == FontQuery::Order::Popular || searching) {
			// Ranked families first, most used first; the rest by name.
			const int ra = fa.popularity > 0 ? fa.popularity : 1 << 30;
			const int rb = fb.popularity > 0 ? fb.popularity : 1 << 30;
			if (ra != rb)
				return ra < rb;
		}
		return fa.search_name < fb.search_name;
	});
	for (const auto &[score, i] : scored)
		out.families.push_back(i);
	std::vector<std::pair<int, char32_t>> by_count;
	for (const auto &[c, n] : missing_count)
		by_count.emplace_back(-n, c);
	std::sort(by_count.begin(), by_count.end());
	for (size_t k = 0; k < by_count.size() && k < 3; k++)
		out.missing.push_back(by_count[k].second);
	return out;
}

} // namespace typeset
