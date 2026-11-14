#include "font-index.hpp"

#include <obs.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <set>
#include <thread>
#include <unordered_map>

namespace typeset {

// (What a family has, and weight_label(): font-filter.cpp, with the search.)

namespace {

std::string lower(std::string s)
{
	for (char &c : s)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// The family name in English where a font names itself in several languages.
std::string family_of(FcPattern *p)
{
	FcChar8 *name = nullptr, *lang = nullptr;
	std::string first;
	for (int i = 0; FcPatternGetString(p, FC_FAMILY, i, &name) == FcResultMatch; i++) {
		if (i == 0)
			first = reinterpret_cast<const char *>(name);
		if (FcPatternGetString(p, FC_FAMILYLANG, i, &lang) == FcResultMatch &&
		    strcmp(reinterpret_cast<const char *>(lang), "en") == 0)
			return reinterpret_cast<const char *>(name);
	}
	return first;
}

struct Catalogued {
	std::string category;
	std::vector<std::string> classes, designers;
	std::map<std::string, int> feel;
	int popularity = 0;
};

// typeset-catalogue.json (see tools/google-fonts-catalogue.py).
void read_catalogue(const std::string &path, std::unordered_map<std::string, Catalogued> &into)
{
	obs_data_t *root = obs_data_create_from_json_file(path.c_str());
	if (!root)
		return;
	obs_data_t *families = obs_data_get_obj(root, "families");
	for (obs_data_item_t *item = families ? obs_data_first(families) : nullptr; item; obs_data_item_next(&item)) {
		obs_data_t *f = obs_data_item_get_obj(item);
		if (!f)
			continue;
		Catalogued c;
		c.category = obs_data_get_string(f, "category");
		c.popularity = int(obs_data_get_int(f, "popularity"));
		// (Lists as one string, "|" between: see google-fonts-catalogue.py.)
		auto strings = [&](const char *key, std::vector<std::string> &out) {
			const std::string all = obs_data_get_string(f, key);
			for (size_t at = 0; at < all.size();) {
				const size_t bar = all.find('|', at);
				const std::string one = all.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
				if (!one.empty())
					out.push_back(one);
				if (bar == std::string::npos)
					break;
				at = bar + 1;
			}
		};
		strings("classes", c.classes);
		strings("designers", c.designers);
		if (obs_data_t *feel = obs_data_get_obj(f, "feel")) {
			for (obs_data_item_t *t = obs_data_first(feel); t; obs_data_item_next(&t))
				c.feel[obs_data_item_get_name(t)] = int(obs_data_item_get_int(t));
			obs_data_release(feel);
		}
		into[obs_data_item_get_name(item)] = std::move(c);
		obs_data_release(f);
	}
	obs_data_release(families);
	obs_data_release(root);
}

std::string dirname_of(const std::string &path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

} // namespace

std::vector<FamilyInfo> build_font_index()
{
	std::unordered_map<std::string, FamilyInfo> by_name;
	std::set<std::string> catalogue_dirs, looked;

	FcPattern *all = FcPatternCreate();
	FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, FC_FAMILYLANG, FC_WEIGHT, FC_SLANT, FC_FILE, FC_CHARSET,
					   FC_VARIABLE, FC_SPACING, nullptr);
	FcFontSet *set = FcFontList(nullptr, all, os);
	FcObjectSetDestroy(os);
	FcPatternDestroy(all);
	for (int i = 0; set && i < set->nfont; i++) {
		FcPattern *p = set->fonts[i];
		const std::string name = family_of(p);
		if (name.empty() || name[0] == '.')
			continue;
		FamilyInfo &f = by_name[name];
		f.name = name;

		FcValue v;
		if (FcPatternGet(p, FC_WEIGHT, 0, &v) == FcResultMatch) {
			if (v.type == FcTypeRange) {
				double lo = 0, hi = 0;
				FcRangeGetDouble(v.u.r, &lo, &hi);
				const int a = int(std::lround(FcWeightToOpenTypeDouble(lo)));
				const int b = int(std::lround(FcWeightToOpenTypeDouble(hi)));
				f.variable = true;
				f.var_lo = f.var_lo ? std::min(f.var_lo, a) : a;
				f.var_hi = std::max(f.var_hi, b);
				f.weights.push_back(a);
				f.weights.push_back(b);
			} else if (v.type == FcTypeDouble || v.type == FcTypeInteger) {
				const double fc = v.type == FcTypeDouble ? v.u.d : double(v.u.i);
				f.weights.push_back(int(std::lround(FcWeightToOpenTypeDouble(fc))));
			}
		}
		int slant = 0;
		if (FcPatternGetInteger(p, FC_SLANT, 0, &slant) == FcResultMatch && slant > 0)
			f.italic = true;
		int spacing = 0;
		if (FcPatternGetInteger(p, FC_SPACING, 0, &spacing) == FcResultMatch && spacing >= FC_MONO &&
		    f.category.empty())
			f.category = "Monospace";
		FcCharSet *cs = nullptr;
		if (FcPatternGetCharSet(p, FC_CHARSET, 0, &cs) == FcResultMatch && cs) {
			FcCharSet *merged = f.charset ? FcCharSetUnion(f.charset.get(), cs) : FcCharSetCopy(cs);
			f.charset.reset(merged, FcCharSetDestroy);
		}
		// A catalogue beside Google's fonts: at the top of the copy, two
		// folders up from a font (google-fonts/ofl/inter/Inter.ttf).
		FcChar8 *file = nullptr;
		if (FcPatternGetString(p, FC_FILE, 0, &file) == FcResultMatch) {
			const std::string dir = dirname_of(reinterpret_cast<const char *>(file));
			for (const std::string &d : {dir, dirname_of(dir), dirname_of(dirname_of(dir))}) {
				if (d.empty() || !looked.insert(d).second)
					continue;
				if (os_file_exists((d + "/typeset-catalogue.json").c_str()))
					catalogue_dirs.insert(d);
			}
		}
	}
	if (set)
		FcFontSetDestroy(set);

	std::unordered_map<std::string, Catalogued> catalogue;
	for (const std::string &d : catalogue_dirs)
		read_catalogue(d + "/typeset-catalogue.json", catalogue);

	std::vector<FamilyInfo> out;
	out.reserve(by_name.size());
	for (auto &[name, f] : by_name) {
		std::sort(f.weights.begin(), f.weights.end());
		f.weights.erase(std::unique(f.weights.begin(), f.weights.end()), f.weights.end());
		if (auto it = catalogue.find(name); it != catalogue.end()) {
			const Catalogued &c = it->second;
			if (!c.category.empty())
				f.category = c.category;
			f.classes = c.classes;
			f.feel = c.feel;
			f.designers = c.designers;
			f.popularity = c.popularity;
		}
		f.search_name = lower(name);
		// The category as one word ("sans", not "sans serif": searching
		// "serif" finds serifs).
		std::string extra = f.category == "Sans Serif" ? "sans" : lower(f.category);
		for (const auto &s : f.classes)
			extra += " " + lower(s);
		for (const auto &[k, v] : f.feel)
			extra += " " + lower(k);
		for (const auto &s : f.designers)
			extra += " " + lower(s);
		f.search_extra = extra;
		out.push_back(std::move(f));
	}
	std::sort(out.begin(), out.end(), [](const FamilyInfo &a, const FamilyInfo &b) {
		return a.search_name < b.search_name;
	});
	return out;
}

// --- The index, built in the background ------------------------------------------------------

struct FontIndex::State {
	std::thread thread;
	std::atomic<bool> ready{false};
	std::vector<FamilyInfo> families;
	std::unordered_map<std::string, size_t> by_name;
};

FontIndex &FontIndex::get()
{
	static FontIndex index;
	return index;
}

void FontIndex::start()
{
	if (state_)
		return;
	state_ = std::make_shared<State>();
	State *s = state_.get();
	s->thread = std::thread([s] {
		os_set_thread_name("typeset: fonts");
		const uint64_t t0 = os_gettime_ns();
		s->families = build_font_index();
		for (size_t i = 0; i < s->families.size(); i++)
			s->by_name[s->families[i].name] = i;
		size_t catalogued = 0;
		for (const FamilyInfo &f : s->families)
			catalogued += f.popularity > 0 || !f.category.empty();
		blog(LOG_INFO, "[obs-typeset] %zu font families (%zu with categories), read in %.0f ms",
		     s->families.size(), catalogued, double(os_gettime_ns() - t0) / 1e6);
		s->ready = true;
	});
}

void FontIndex::stop()
{
	if (state_ && state_->thread.joinable())
		state_->thread.join();
	state_.reset();
}

bool FontIndex::ready() const
{
	return state_ && state_->ready;
}

const std::vector<FamilyInfo> &FontIndex::families() const
{
	static const std::vector<FamilyInfo> none;
	return ready() ? state_->families : none;
}

const FamilyInfo *FontIndex::find(const std::string &name) const
{
	if (!ready())
		return nullptr;
	auto it = state_->by_name.find(name);
	return it == state_->by_name.end() ? nullptr : &state_->families[it->second];
}

} // namespace typeset
