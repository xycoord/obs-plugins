// Every installed font family, for the font browser: what each has (weights,
// italics, variable ranges, the characters it covers) from fontconfig, and,
// for Google Fonts installed by tools/google-fonts.sh, Google's own
// information (category, finer classes, feel tags, popularity) from the
// typeset-catalogue.json beside them.
//
// Read once, on a thread of its own, from module load; the browser asks
// whether it's ready. Nothing here touches Qt.
#pragma once

#include <fontconfig/fontconfig.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace typeset {

struct FamilyInfo {
	std::string name;
	std::string category;             // "Sans Serif", "Serif", "Display", "Handwriting", "Monospace"; "" unknown
	std::vector<std::string> classes; // finer, strongest first ("Geometric")
	std::map<std::string, int> feel;  // "Calm" -> strength 0-100 (Google's; see feel_strong)
	std::vector<std::string> designers;
	int popularity = 0; // Google's rank, 1 the most used; 0 unknown
	// Weights it has (OpenType 100-1000): the static styles' and the variable
	// range's ends; whether a variable font covers [var_lo, var_hi].
	std::vector<int> weights;
	bool variable = false;
	int var_lo = 0, var_hi = 0;
	bool italic = false;
	std::shared_ptr<FcCharSet> charset; // every character any of its styles has
	// Lowercased, for search: the name, and the category, classes and
	// designers (feelings are matched with their strength: font-filter.cpp).
	std::string search_name, search_extra;

	bool has_weight(int w) const;
	int nearest_weight(int w) const; // the closest it has
	bool covers(const std::u32string &text) const;
};

// The weight's usual name ("Medium"), in English (for facts and tests).
const char *weight_label(int weight);

// The index, built in the background. families() is only valid once ready().
class FontIndex {
public:
	static FontIndex &get();
	void start(); // at module load
	void stop();  // at unload: waits for a build in progress
	bool ready() const;
	const std::vector<FamilyInfo> &families() const;
	const FamilyInfo *find(const std::string &name) const;

private:
	FontIndex() = default;
	struct State;
	std::shared_ptr<State> state_;
};

// Built from fontconfig's list of fonts and the catalogues beside them (for
// tests: of the fonts fontconfig knows now).
std::vector<FamilyInfo> build_font_index();

} // namespace typeset
