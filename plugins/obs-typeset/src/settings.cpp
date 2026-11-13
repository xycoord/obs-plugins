#include "settings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>

namespace typeset {

namespace {

// The style's settings, with their types, for copying a style from one
// source to another and saving it as the default.
enum class Kind { Bool, Int, Double, Object };
struct Key {
	const char *name;
	Kind kind;
};
const Key style_keys[] = {
	{"font", Kind::Object},         {"colour", Kind::Int},          {"align", Kind::Int},
	{"box", Kind::Bool},            {"box_width", Kind::Int},       {"box_height", Kind::Int},
	{"overflow", Kind::Int},        {"valign", Kind::Int},          {"typography", Kind::Bool},
	{"line_spacing", Kind::Double}, {"letter_spacing", Kind::Int},  {"text_case", Kind::Int},
	{"tabular", Kind::Bool},        {"outline", Kind::Bool},        {"outline_colour", Kind::Int},
	{"outline_width", Kind::Int},   {"shadow", Kind::Bool},         {"shadow_colour", Kind::Int},
	{"shadow_opacity", Kind::Int},  {"shadow_x", Kind::Int},        {"shadow_y", Kind::Int},
	{"shadow_blur", Kind::Int},     {"shadow_spread", Kind::Int},   {"plate", Kind::Bool},
	{"plate_colour", Kind::Int},    {"plate_opacity", Kind::Int},   {"plate_padding", Kind::Int},
	{"plate_radius", Kind::Int},
};
constexpr const char *style_marker = "typeset_style";

// One setting copied as its kind (a value from defaults included).
void copy_key(obs_data_t *from, obs_data_t *to, const Key &k)
{
	switch (k.kind) {
	case Kind::Bool:
		obs_data_set_bool(to, k.name, obs_data_get_bool(from, k.name));
		break;
	case Kind::Int:
		obs_data_set_int(to, k.name, obs_data_get_int(from, k.name));
		break;
	case Kind::Double:
		obs_data_set_double(to, k.name, obs_data_get_double(from, k.name));
		break;
	case Kind::Object: {
		obs_data_t *o = obs_data_get_obj(from, k.name);
		obs_data_set_obj(to, k.name, o);
		obs_data_release(o);
		break;
	}
	}
}

// Whether data holds a setting as the type its kind is stored as.
bool has_as_kind(obs_data_t *data, const Key &k)
{
	if (!obs_data_has_user_value(data, k.name))
		return false;
	obs_data_item_t *item = obs_data_item_byname(data, k.name);
	const obs_data_type type = obs_data_item_gettype(item);
	obs_data_item_release(&item);
	return k.kind == Kind::Object ? type == OBS_DATA_OBJECT
	       : k.kind == Kind::Bool ? type == OBS_DATA_BOOLEAN
				      : type == OBS_DATA_NUMBER;
}

std::string default_style_path()
{
	char *path = obs_module_config_path("default-style.json");
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

} // namespace

void set_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "text", obs_module_text("DefaultText"));
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", "Sans");
	obs_data_set_string(font, "style", "Regular");
	obs_data_set_int(font, "size", 96);
	obs_data_set_int(font, "flags", 0);
	obs_data_set_default_obj(settings, "font", font);
	obs_data_release(font);
	obs_data_set_default_int(settings, "size", 96);
	obs_data_set_default_int(settings, "colour", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "align", int(Align::Left));

	obs_data_set_default_bool(settings, "box", false);
	obs_data_set_default_int(settings, "box_width", 0);
	obs_data_set_default_int(settings, "box_height", 0);
	obs_data_set_default_int(settings, "overflow", int(Overflow::Wrap));
	obs_data_set_default_int(settings, "valign", int(VAlign::Top));

	obs_data_set_default_bool(settings, "typography", false);
	obs_data_set_default_double(settings, "line_spacing", 1.0);
	obs_data_set_default_int(settings, "letter_spacing", 0);
	obs_data_set_default_int(settings, "text_case", int(Case::AsTyped));
	obs_data_set_default_bool(settings, "tabular", false);

	// Over projector visuals and video, text needs something to stand out
	// from them: a shadow is on from the start, close and fairly dense, so
	// it holds up thin and light text too.
	obs_data_set_default_bool(settings, "outline", false);
	obs_data_set_default_int(settings, "outline_colour", 0xFF000000);
	obs_data_set_default_int(settings, "outline_width", 4);
	obs_data_set_default_bool(settings, "shadow", true);
	obs_data_set_default_int(settings, "shadow_colour", 0xFF000000);
	obs_data_set_default_int(settings, "shadow_opacity", 75);
	obs_data_set_default_int(settings, "shadow_x", 0);
	obs_data_set_default_int(settings, "shadow_y", 3);
	obs_data_set_default_int(settings, "shadow_blur", 6);
	obs_data_set_default_int(settings, "shadow_spread", 2);
	obs_data_set_default_bool(settings, "plate", false);
	obs_data_set_default_int(settings, "plate_colour", 0xFF000000);
	obs_data_set_default_int(settings, "plate_opacity", 75);
	obs_data_set_default_int(settings, "plate_padding", 24);
	obs_data_set_default_int(settings, "plate_radius", 8);
}

// A colour (its own alpha ignored) at an opacity in percent.
static uint32_t with_opacity(long long colour, long long percent)
{
	const uint32_t a = uint32_t(std::clamp<long long>(percent, 0, 100) * 255 / 100);
	return (uint32_t(colour) & 0xFFFFFF) | (a << 24);
}

Style read_style(obs_data_t *settings)
{
	Style s;
	obs_data_t *font = obs_data_get_obj(settings, "font");
	if (font) {
		const char *face = obs_data_get_string(font, "face");
		const char *face_style = obs_data_get_string(font, "style");
		if (face && *face)
			s.family = face;
		s.face_style = face_style ? face_style : "";
		const long long size = obs_data_get_int(font, "size");
		if (size > 0)
			s.size = double(std::min<long long>(size, 4096));
		s.font_flags = uint32_t(obs_data_get_int(font, "flags"));
		obs_data_release(font);
	}
	s.colour = uint32_t(obs_data_get_int(settings, "colour"));
	s.align = Align(std::clamp<long long>(obs_data_get_int(settings, "align"), 0, 2));

	// Every value kept within what the dialog allows, whatever set it (a
	// pasted style, a script).
	s.box = obs_data_get_bool(settings, "box");
	s.box_width = double(std::clamp<long long>(obs_data_get_int(settings, "box_width"), 0, 16384));
	s.box_height = double(std::clamp<long long>(obs_data_get_int(settings, "box_height"), 0, 16384));
	s.overflow = Overflow(std::clamp<long long>(obs_data_get_int(settings, "overflow"), 0, 1));
	s.valign = VAlign(std::clamp<long long>(obs_data_get_int(settings, "valign"), 0, 2));

	// Turned off, a group's settings don't apply, whatever they hold.
	if (obs_data_get_bool(settings, "typography")) {
		s.line_spacing = std::clamp(obs_data_get_double(settings, "line_spacing"), 0.5, 3.0);
		s.letter_spacing = std::clamp(obs_data_get_double(settings, "letter_spacing"), -20.0, 100.0);
		s.text_case = Case(std::clamp<long long>(obs_data_get_int(settings, "text_case"), 0, 3));
		s.tabular = obs_data_get_bool(settings, "tabular");
	}
	s.outline = obs_data_get_bool(settings, "outline");
	s.outline_colour = uint32_t(obs_data_get_int(settings, "outline_colour"));
	s.outline_width = std::clamp(obs_data_get_double(settings, "outline_width"), 0.0, 64.0);
	s.shadow = obs_data_get_bool(settings, "shadow");
	s.shadow_colour = with_opacity(obs_data_get_int(settings, "shadow_colour"),
				       obs_data_get_int(settings, "shadow_opacity"));
	s.shadow_x = std::clamp(obs_data_get_double(settings, "shadow_x"), -200.0, 200.0);
	s.shadow_y = std::clamp(obs_data_get_double(settings, "shadow_y"), -200.0, 200.0);
	s.shadow_blur = std::clamp(obs_data_get_double(settings, "shadow_blur"), 0.0, 200.0);
	s.shadow_spread = std::clamp(obs_data_get_double(settings, "shadow_spread"), 0.0, 100.0);
	s.plate = obs_data_get_bool(settings, "plate");
	s.plate_colour = with_opacity(obs_data_get_int(settings, "plate_colour"),
				      obs_data_get_int(settings, "plate_opacity"));
	s.plate_padding = std::clamp(obs_data_get_double(settings, "plate_padding"), 0.0, 400.0);
	s.plate_radius = std::clamp(obs_data_get_double(settings, "plate_radius"), 0.0, 400.0);
	return s;
}

Spans read_spans(obs_data_t *settings)
{
	Spans spans;
	obs_data_array_t *array = obs_data_get_array(settings, "spans");
	const size_t n = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *o = obs_data_array_item(array, i);
		Span sp;
		sp.start = size_t(std::max<long long>(0, obs_data_get_int(o, "start")));
		sp.end = size_t(std::max<long long>(0, obs_data_get_int(o, "end")));
		// Each: true on, false off (on a base that has it), absent as the base.
		auto tri = [&](const char *k) -> int8_t {
			return obs_data_has_user_value(o, k) ? (obs_data_get_bool(o, k) ? 1 : 0) : -1;
		};
		sp.bold = tri("bold");
		sp.italic = tri("italic");
		sp.underline = tri("underline");
		sp.has_colour = obs_data_has_user_value(o, "colour");
		sp.colour = uint32_t(obs_data_get_int(o, "colour"));
		sp.size = int(std::clamp<long long>(obs_data_get_int(o, "size"), 0, 1000));
		spans.push_back(sp);
		obs_data_release(o);
	}
	obs_data_array_release(array);
	return spans;
}

obs_data_array_t *spans_array(const Spans &spans)
{
	obs_data_array_t *array = obs_data_array_create();
	for (const Span &sp : spans) {
		obs_data_t *o = obs_data_create();
		obs_data_set_int(o, "start", (long long)sp.start);
		obs_data_set_int(o, "end", (long long)sp.end);
		if (sp.bold >= 0)
			obs_data_set_bool(o, "bold", sp.bold > 0);
		if (sp.italic >= 0)
			obs_data_set_bool(o, "italic", sp.italic > 0);
		if (sp.underline >= 0)
			obs_data_set_bool(o, "underline", sp.underline > 0);
		if (sp.has_colour)
			obs_data_set_int(o, "colour", sp.colour);
		if (sp.size)
			obs_data_set_int(o, "size", sp.size);
		obs_data_array_push_back(array, o);
		obs_data_release(o);
	}
	return array;
}

void sync_size(obs_data_t *settings)
{
	obs_data_t *font = obs_data_get_obj(settings, "font");
	const long long size = font ? obs_data_get_int(font, "size") : 0;
	obs_data_release(font);
	if (size > 0 && (!obs_data_has_user_value(settings, "size") || obs_data_get_int(settings, "size") != size))
		obs_data_set_int(settings, "size", size);
}

obs_data_t *style_of(obs_data_t *settings)
{
	obs_data_t *out = obs_data_create();
	obs_data_set_bool(out, style_marker, true);
	for (const Key &k : style_keys)
		copy_key(settings, out, k);
	return out;
}

obs_data_t *pasted_style(obs_data_t *data)
{
	if (!data || !obs_data_get_bool(data, style_marker))
		return nullptr;
	obs_data_t *out = obs_data_create();
	for (const Key &k : style_keys)
		if (has_as_kind(data, k))
			copy_key(data, out, k);
	return out;
}

void apply_default_style(obs_data_t *settings)
{
	if (obs_data_get_bool(settings, "styled"))
		return;
	const std::string path = default_style_path();
	if (obs_data_t *saved = path.empty() ? nullptr : obs_data_create_from_json_file(path.c_str())) {
		for (const Key &k : style_keys)
			if (has_as_kind(saved, k) && !obs_data_has_user_value(settings, k.name))
				copy_key(saved, settings, k);
		obs_data_release(saved);
	}
	obs_data_set_bool(settings, "styled", true);
}

bool save_default_style(obs_data_t *settings)
{
	const std::string path = default_style_path();
	if (path.empty())
		return false;
	char *dir = obs_module_config_path("");
	if (dir)
		os_mkdirs(dir);
	bfree(dir);
	obs_data_t *style = style_of(settings);
	const bool saved = obs_data_save_json_safe(style, path.c_str(), "tmp", "bak");
	if (!saved)
		blog(LOG_WARNING, "[obs-typeset] couldn't save the default style to %s", path.c_str());
	obs_data_release(style);
	return saved;
}

} // namespace typeset
