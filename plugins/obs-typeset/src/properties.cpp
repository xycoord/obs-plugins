#include "properties.hpp"

#include "settings.hpp"
#include "typeset-source.hpp"

#include <obs-module.h>

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QThread>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace typeset {

namespace {

// Vertical alignment means something only in a box with a height.
bool valign_shown(obs_data_t *settings)
{
	return obs_data_get_bool(settings, "box") && obs_data_get_int(settings, "box_height") > 0;
}

// Balancing and lone last words mean something only for text that wraps.
bool wrap_options_shown(obs_data_t *settings)
{
	return obs_data_get_bool(settings, "box") && obs_data_get_int(settings, "overflow") == int(Overflow::Wrap);
}

void fill_box(obs_properties_t *g, obs_data_t *settings)
{
	obs_property_t *p = obs_properties_add_int(g, "box_width", obs_module_text("Box.Width"), 1, 16384, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(g, "box_height", obs_module_text("Box.Height"), 0, 16384, 1);
	obs_property_int_set_suffix(p, " px");
	obs_property_set_long_description(p, obs_module_text("Box.Height.Help"));
	obs_property_set_modified_callback(p, [](obs_properties_t *props, obs_property_t *, obs_data_t *s) {
		obs_property_t *v = obs_properties_get(props, "valign");
		const bool show = valign_shown(s);
		if (!v || obs_property_visible(v) == show)
			return false;
		obs_property_set_visible(v, show);
		return true;
	});
	p = obs_properties_add_list(g, "overflow", obs_module_text("Box.Overflow"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Box.Overflow.Wrap"), int(Overflow::Wrap));
	obs_property_list_add_int(p, obs_module_text("Box.Overflow.Shrink"), int(Overflow::Shrink));
	obs_property_set_modified_callback(p, [](obs_properties_t *props, obs_property_t *, obs_data_t *s) {
		bool changed = false;
		for (const char *name : {"balance", "avoid_orphans"}) {
			obs_property_t *o = obs_properties_get(props, name);
			if (o && obs_property_visible(o) != wrap_options_shown(s)) {
				obs_property_set_visible(o, wrap_options_shown(s));
				changed = true;
			}
		}
		return changed;
	});
	p = obs_properties_add_bool(g, "balance", obs_module_text("Box.Balance"));
	obs_property_set_long_description(p, obs_module_text("Box.Balance.Help"));
	obs_property_set_visible(p, settings && wrap_options_shown(settings));
	p = obs_properties_add_bool(g, "avoid_orphans", obs_module_text("Box.AvoidOrphans"));
	obs_property_set_long_description(p, obs_module_text("Box.AvoidOrphans.Help"));
	obs_property_set_visible(p, settings && wrap_options_shown(settings));
	p = obs_properties_add_list(g, "valign", obs_module_text("Box.VAlign"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Box.VAlign.Top"), int(VAlign::Top));
	obs_property_list_add_int(p, obs_module_text("Box.VAlign.Middle"), int(VAlign::Middle));
	obs_property_list_add_int(p, obs_module_text("Box.VAlign.Bottom"), int(VAlign::Bottom));
	obs_property_set_visible(p, settings && valign_shown(settings));
}

obs_property_t *px_slider(obs_properties_t *g, const char *name, const char *label, int min, int max)
{
	obs_property_t *p = obs_properties_add_int_slider(g, name, obs_module_text(label), min, max, 1);
	obs_property_int_set_suffix(p, " px");
	return p;
}

obs_property_t *percent_slider(obs_properties_t *g, const char *name, const char *label, int min, int max)
{
	obs_property_t *p = obs_properties_add_int_slider(g, name, obs_module_text(label), min, max, 1);
	obs_property_int_set_suffix(p, "%");
	return p;
}

void fill_plate(obs_properties_t *g, obs_data_t *)
{
	obs_properties_add_color(g, "plate_colour", obs_module_text("Colour"));
	percent_slider(g, "plate_opacity", "Opacity", 0, 100);
	px_slider(g, "plate_padding", "Plate.Padding", 0, 400);
	px_slider(g, "plate_radius", "Plate.Radius", 0, 400);
}

void fill_outline(obs_properties_t *g, obs_data_t *)
{
	obs_properties_add_color_alpha(g, "outline_colour", obs_module_text("Colour"));
	px_slider(g, "outline_width", "Outline.Width", 1, 64);
}

void fill_shadow(obs_properties_t *g, obs_data_t *)
{
	obs_properties_add_color(g, "shadow_colour", obs_module_text("Colour"));
	percent_slider(g, "shadow_opacity", "Opacity", 0, 100);
	px_slider(g, "shadow_x", "Shadow.X", -200, 200);
	px_slider(g, "shadow_y", "Shadow.Y", -200, 200);
	px_slider(g, "shadow_blur", "Shadow.Blur", 0, 200);
	obs_property_t *p = px_slider(g, "shadow_spread", "Shadow.Spread", 0, 100);
	obs_property_set_long_description(p, obs_module_text("Shadow.Spread.Help"));
}

void fill_typography(obs_properties_t *g, obs_data_t *)
{
	obs_property_t *p =
		obs_properties_add_float_slider(g, "line_spacing", obs_module_text("LineSpacing"), 0.5, 3.0, 0.05);
	obs_property_float_set_suffix(p, "×");
	p = percent_slider(g, "letter_spacing", "LetterSpacing", -20, 100);
	obs_property_set_long_description(p, obs_module_text("LetterSpacing.Help"));
	p = obs_properties_add_list(g, "text_case", obs_module_text("Case"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Case.AsTyped"), int(Case::AsTyped));
	obs_property_list_add_int(p, obs_module_text("Case.Upper"), int(Case::Upper));
	obs_property_list_add_int(p, obs_module_text("Case.Lower"), int(Case::Lower));
	obs_property_list_add_int(p, obs_module_text("Case.Title"), int(Case::Title));
	p = obs_properties_add_bool(g, "tabular", obs_module_text("Tabular"));
	obs_property_set_long_description(p, obs_module_text("Tabular.Help"));
}

struct Group {
	const char *name, *label, *help, *about;
	void (*fill)(obs_properties_t *, obs_data_t *);
};
// In the order a venue's jobs use them most.
const Group groups[] = {
	{"plate", "Plate", nullptr, "Plate.About", fill_plate},
	{"shadow", "Shadow", "Shadow.Help", "Shadow.About", fill_shadow},
	{"outline", "Outline", nullptr, "Outline.About", fill_outline},
	{"box", "Box", "Box.Help", "Box.About", fill_box},
	{"typography", "Typography", nullptr, "Typography.About", fill_typography},
};

// Each feature is a checkable group (OBS's own: the switch is the box's
// title), so on or off and its settings read plainly as one thing. Off, it
// shows one line saying what it does; on, its settings. Both are made once
// with the dialog and only shown or hidden: OBS's dialog keeps pointers to
// the properties it was built from until it rebuilds. Returns whether
// anything was shown or hidden.
bool show_group(obs_property_t *group, obs_data_t *settings)
{
	const char *name = obs_property_name(group);
	const bool on = obs_data_get_bool(settings, name);
	const std::string about = std::string(name) + "_about";
	bool changed = false;
	for (obs_property_t *p = obs_properties_first(obs_property_group_content(group)); p; obs_property_next(&p)) {
		const char *pname = obs_property_name(p);
		bool show = about == pname ? !on : on;
		if (strcmp(pname, "valign") == 0)
			show = valign_shown(settings);
		if (strcmp(pname, "balance") == 0 || strcmp(pname, "avoid_orphans") == 0)
			show = wrap_options_shown(settings);
		if (obs_property_visible(p) != show) {
			obs_property_set_visible(p, show);
			changed = true;
		}
	}
	return changed;
}

// OBS calls this when the dialog opens (before building it) and when a
// group is ticked or unticked.
bool group_toggled(obs_properties_t *props, obs_property_t *group, obs_data_t *settings)
{
	// A box starts as wide as the text is now, so ticking it changes
	// nothing until its width is changed.
	if (strcmp(obs_property_name(group), "box") == 0 && obs_data_get_bool(settings, "box") &&
	    obs_data_get_int(settings, "box_width") <= 0) {
		if (auto *text = static_cast<TextSource *>(obs_properties_get_param(props))) {
			const Document doc = text->document();
			const double pad = doc.style.plate ? doc.style.plate_padding : 0;
			obs_data_set_int(settings, "box_width", std::max(1LL, (long long)std::lround(text->width() - 2 * pad)));
		}
	}
	return show_group(group, settings);
}

// The font picker holds the size, but the size is what's changed most, so it
// has a field of its own too; the two are kept the same.
bool font_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool refresh = false;
	obs_data_t *font = obs_data_get_obj(settings, "font");
	const long long size = font ? obs_data_get_int(font, "size") : 0;
	const char *face = font ? obs_data_get_string(font, "face") : "";
	const std::string family = face && *face ? face : "Sans";
	obs_data_release(font);
	if (size > 0 && size != obs_data_get_int(settings, "size")) {
		obs_data_set_int(settings, "size", size);
		refresh = true;
	}

	// Warn when the font isn't installed (a scene collection from
	// another computer), and say what's shown instead.
	if (obs_property_t *warning = obs_properties_get(props, "font_missing")) {
		std::string used;
		bool installed;
		{
			// A font map of its own, so listing every installed family
			// (slow the first time) never holds the layout lock, which
			// a text made on another thread may be waiting on.
			static std::mutex mutex;
			std::lock_guard lock(mutex);
			static PangoContext *context = create_context(pango_cairo_font_map_new());
			installed = font_installed(context, family, &used);
		}
		const std::string message = installed ? std::string()
						      : std::string(obs_module_text("FontMissing.1")) + " \u201c" +
								family + "\u201d " + obs_module_text("FontMissing.2") +
								" \u201c" + used + "\u201d.";
		if (obs_property_visible(warning) != !installed ||
		    message != (obs_property_description(warning) ? obs_property_description(warning) : "")) {
			obs_property_set_description(warning, message.c_str());
			obs_property_set_visible(warning, !installed);
			refresh = true;
		}
	}
	return refresh;
}

bool size_changed(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const long long size = obs_data_get_int(settings, "size");
	obs_data_t *font = obs_data_get_obj(settings, "font");
	if (font && size > 0 && obs_data_get_int(font, "size") != size) {
		obs_data_t *copy = obs_data_create();
		obs_data_apply(copy, font);
		obs_data_set_int(copy, "size", size);
		obs_data_set_obj(settings, "font", copy);
		obs_data_release(copy);
	}
	obs_data_release(font);
	return false;
}

// The clipboard is Qt's, usable only on its UI thread (obs-websocket can
// press a button from another).
bool on_ui_thread()
{
	return qApp && QThread::currentThread() == qApp->thread();
}

bool copy_style(obs_properties_t *, obs_property_t *, void *data)
{
	auto *text = static_cast<TextSource *>(data);
	if (!text || !on_ui_thread())
		return false;
	obs_data_t *settings = obs_source_get_settings(text->source());
	obs_data_t *style = style_of(settings);
	QGuiApplication::clipboard()->setText(QString::fromUtf8(obs_data_get_json_pretty(style)));
	obs_data_release(style);
	obs_data_release(settings);
	return false;
}

bool paste_style(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *text = static_cast<TextSource *>(data);
	if (!text || !on_ui_thread())
		return false;
	const QByteArray json = QGuiApplication::clipboard()->text().toUtf8();
	obs_data_t *pasted = json.isEmpty() ? nullptr : obs_data_create_from_json(json.constData());
	obs_data_t *style = pasted_style(pasted);
	obs_data_release(pasted);
	if (!style)
		return false;
	obs_source_update(text->source(), style);
	obs_data_release(style);
	// OBS rebuilds the dialog without asking the groups again: they're
	// shown on or off for the pasted style here, and the size field is set
	// to the pasted font's size.
	obs_data_t *settings = obs_source_get_settings(text->source());
	sync_size(settings);
	for (const Group &g : groups)
		if (obs_property_t *p = obs_properties_get(props, g.name))
			group_toggled(props, p, settings);
	obs_data_release(settings);
	return true;
}

bool save_default(obs_properties_t *, obs_property_t *, void *data)
{
	auto *text = static_cast<TextSource *>(data);
	if (!text)
		return false;
	obs_data_t *settings = obs_source_get_settings(text->source());
	save_default_style(settings);
	obs_data_release(settings);
	return false;
}

} // namespace

obs_properties_t *source_properties(void *data)
{
	auto *text = static_cast<TextSource *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, text, nullptr);
	obs_property_t *p;

	obs_properties_add_text(props, "text", obs_module_text("Text"), OBS_TEXT_MULTILINE);
	p = obs_properties_add_text(props, "edit_hint", obs_module_text("EditHint"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(p, true);
	p = obs_properties_add_font(props, "font", obs_module_text("Font"));
	obs_property_set_modified_callback(p, font_changed);
	p = obs_properties_add_int_slider(props, "size", obs_module_text("Size"), 4, 4096, 1);
	obs_property_int_set_suffix(p, " px");
	obs_property_set_long_description(p, obs_module_text("Size.Help"));
	obs_property_set_modified_callback(p, size_changed);
	obs_properties_add_color_alpha(props, "colour", obs_module_text("Colour"));
	p = obs_properties_add_list(props, "align", obs_module_text("Align"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Align.Left"), int(Align::Left));
	obs_property_list_add_int(p, obs_module_text("Align.Centre"), int(Align::Centre));
	obs_property_list_add_int(p, obs_module_text("Align.Right"), int(Align::Right));
	obs_property_list_add_int(p, obs_module_text("Align.Justified"), int(Align::Justified));

	// Shown only when the font isn't on this computer (see font_changed).
	p = obs_properties_add_text(props, "font_missing", "", OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);
	obs_property_text_set_info_word_wrap(p, true);
	obs_property_set_visible(p, false);

	// Only when some of the text has a style of its own (set while editing
	// in the preview).
	const bool styled = text && !text->document().spans.empty();
	p = obs_properties_add_text(props, "word_styles", obs_module_text("WordStyles"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(p, true);
	obs_property_set_visible(p, styled);
	p = obs_properties_add_button2(
		props, "clear_word_styles", obs_module_text("ClearWordStyles"),
		[](obs_properties_t *props, obs_property_t *, void *data) {
			auto *t = static_cast<TextSource *>(data);
			if (!t)
				return false;
			t->set_content(t->text(), {});
			obs_property_set_visible(obs_properties_get(props, "word_styles"), false);
			obs_property_set_visible(obs_properties_get(props, "clear_word_styles"), false);
			return true;
		},
		text);
	obs_property_set_visible(p, styled);

	// (Without a source, as a new one would be.)
	obs_data_t *settings = text ? obs_source_get_settings(text->source()) : obs_get_source_defaults(source_id);
	for (const Group &g : groups) {
		obs_properties_t *content = obs_properties_create();
		g.fill(content, settings);
		const std::string about = std::string(g.name) + "_about";
		obs_property_t *line = obs_properties_add_text(content, about.c_str(), obs_module_text(g.about), OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(line, true);
		p = obs_properties_add_group(props, g.name, obs_module_text(g.label), OBS_GROUP_CHECKABLE, content);
		if (g.help)
			obs_property_set_long_description(p, obs_module_text(g.help));
		obs_property_set_modified_callback(p, group_toggled);
		if (settings)
			show_group(p, settings);
	}
	obs_data_release(settings);

	obs_properties_t *style = obs_properties_create();
	obs_properties_add_button2(style, "copy_style", obs_module_text("CopyStyle"), copy_style, text);
	obs_properties_add_button2(style, "paste_style", obs_module_text("PasteStyle"), paste_style, text);
	p = obs_properties_add_button2(style, "default_style", obs_module_text("DefaultStyle"), save_default, text);
	obs_property_set_long_description(p, obs_module_text("DefaultStyle.Help"));
	p = obs_properties_add_group(props, "style", obs_module_text("Style"), OBS_GROUP_NORMAL, style);
	obs_property_set_long_description(p, obs_module_text("Style.Help"));

	return props;
}

} // namespace typeset
