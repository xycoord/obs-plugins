#include "obs-preview.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QAction>
#include <QGuiApplication>
#include <QMenu>
#include <QWidget>

#include <cstring>
#include <dlfcn.h>
#include <strings.h>

namespace typeset {

Mapping preview_mapping(QWidget *preview)
{
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const double dpr = preview->devicePixelRatioF();
	const QSize size = preview->size() * dpr;
	return fit_canvas(size.width(), size.height(), int(ovi.base_width), int(ovi.base_height), dpr);
}

vec2 widget_to_canvas(const Mapping &m, QPointF p)
{
	vec2 out;
	vec2_set(&out, float((p.x() * m.dpr - m.x) / m.scale), float((p.y() * m.dpr - m.y) / m.scale));
	return out;
}

QPointF canvas_to_widget(const Mapping &m, const vec3 &p)
{
	return QPointF((m.x + p.x * m.scale) / m.dpr, (m.y + p.y * m.scale) / m.dpr);
}

// OBS keeps its Preview Scaling menu's ticks up to date only when the menu
// opens, so it's asked to update them first (the menu's "about to show"
// handler, by name).
Scaling preview_scaling(QWidget *preview)
{
	QWidget *main = preview->window();
	QMenu *menu = main ? main->findChild<QMenu *>("scalingMenu") : nullptr;
	QAction *window = main ? main->findChild<QAction *>("actionScaleWindow") : nullptr;
	if (!menu || !window)
		return Scaling::Unknown;
	QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
	return window->isChecked() ? Scaling::Window : Scaling::Other;
}

bool known_obs()
{
	const uint32_t major = obs_get_version() >> 24;
	return major >= known_obs_first && major <= known_obs_last;
}

bool can_edit_in(QWidget *preview)
{
	if (!known_obs()) {
		static bool warned = false;
		if (!warned)
			blog(LOG_WARNING, "[obs-typeset] text is edited in the preview only in OBS %u to %u, whose "
					  "preview it's known to fit; edit it in its properties",
			     known_obs_first, known_obs_last);
		warned = true;
		return false;
	}
	switch (preview_scaling(preview)) {
	case Scaling::Window:
		return true;
	case Scaling::Other:
		blog(LOG_INFO, "[obs-typeset] the preview isn't set to Scale to Window, so text isn't edited in it "
			       "(right-click the preview: Preview Scaling, Scale to Window)");
		return false;
	case Scaling::Unknown:
		blog(LOG_WARNING, "[obs-typeset] couldn't read OBS's Preview Scaling menu (this OBS isn't one of "
				  "%u to %u?), so text isn't edited in the preview",
		     known_obs_first, known_obs_last);
		return false;
	}
	return false;
}

// The preview widget is an OBSQTDisplay, whose first member is an OBSDisplay
// (one obs_display_t pointer) straight after its QWidget base. That's read
// only in the known OBS versions (in any other, the editor doesn't start).
// Within them it's also checked against the display's size before use: a
// check that confirms the layout, not one that protects against a wrong one
// (reading the size goes through the pointer), so the version gate is the
// real protection.
obs_display_t *preview_display(QWidget *preview)
{
	if (!known_obs())
		return nullptr;
	bool is_display = false;
	for (const QMetaObject *mo = preview->metaObject(); mo; mo = mo->superClass()) {
		if (strcmp(mo->className(), "OBSQTDisplay") == 0) {
			is_display = true;
			break;
		}
	}
	if (!is_display)
		return nullptr;
	obs_display_t *display =
		*reinterpret_cast<obs_display_t *const *>(reinterpret_cast<const char *>(preview) + sizeof(QWidget));
	if (!display)
		return nullptr;
	uint32_t cx = 0, cy = 0;
	obs_display_size(display, &cx, &cy);
	const QSize size = preview->size() * preview->devicePixelRatioF();
	if (int(cx) != size.width() || int(cy) != size.height())
		return nullptr;
	return display;
}

obs_source_t *preview_scene()
{
	return obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
							  : obs_frontend_get_current_scene();
}

// --- Hotkeys ----------------------------------------------------------------------------------
//
// By default OBS's hotkeys work even while its window has focus: on Linux,
// libobs then reads the keyboard itself (not through Qt), so a key bound to a
// hotkey would act while typed into the text. That "background" reading is
// off while editing, and put back after as OBS sets it (OBSApp's
// ResetHotkeyState): on unless the user chose to disable hotkeys in or out of
// focus. The setting is in OBS's user config from OBS 31, the global one
// before; the user config's getter doesn't exist before 31, so it's looked up.

namespace {

config_t *hotkey_config()
{
	using getter = config_t *(*)(void);
	static const auto user = reinterpret_cast<getter>(dlsym(RTLD_DEFAULT, "obs_frontend_get_user_config"));
	static const auto global = reinterpret_cast<getter>(dlsym(RTLD_DEFAULT, "obs_frontend_get_global_config"));
	return user ? user() : global ? global() : nullptr;
}

} // namespace

void suspend_hotkeys()
{
	obs_hotkey_enable_background_press(false);
}

void restore_hotkeys()
{
	config_t *config = hotkey_config();
	const char *type = config ? config_get_string(config, "General", "HotkeyFocusType") : nullptr;
	const bool in_focus_ok = !type || strcasecmp(type, "DisableHotkeysInFocus") != 0;
	const bool out_of_focus_ok = !type || strcasecmp(type, "DisableHotkeysOutOfFocus") != 0;
	const bool in_focus = QGuiApplication::applicationState() == Qt::ApplicationActive;
	obs_hotkey_enable_background_press((in_focus && in_focus_ok) || (!in_focus && out_of_focus_ok));
}

// --- Scene items ------------------------------------------------------------------------------

void item_to_canvas(obs_sceneitem_t *item, obs_sceneitem_t *group, matrix4 *out)
{
	// A cropped item's draw transform maps what's left after the crop.
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	matrix4 draw;
	obs_sceneitem_get_draw_transform(item, &draw);
	matrix4_identity(out);
	matrix4_translate3f(out, out, -float(crop.left), -float(crop.top), 0.0f);
	matrix4_mul(out, out, &draw);
	if (group) {
		obs_sceneitem_crop gcrop;
		obs_sceneitem_get_crop(group, &gcrop);
		matrix4 g, shift;
		obs_sceneitem_get_draw_transform(group, &g);
		matrix4_identity(&shift);
		matrix4_translate3f(&shift, &shift, -float(gcrop.left), -float(gcrop.top), 0.0f);
		matrix4_mul(out, out, &shift);
		matrix4_mul(out, out, &g);
	}
}

ItemRef::ItemRef(obs_sceneitem_t *item, obs_sceneitem_t *group) : item_(item), group_(group)
{
	if (item_)
		obs_sceneitem_addref(item_);
	if (group_)
		obs_sceneitem_addref(group_);
}

void ItemRef::reset()
{
	if (item_)
		obs_sceneitem_release(item_);
	if (group_)
		obs_sceneitem_release(group_);
	item_ = group_ = nullptr;
}

bool point_in_item(obs_sceneitem_t *item, obs_sceneitem_t *group, const vec2 &pos, vec3 *local)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	const uint32_t cx = obs_source_get_width(source), cy = obs_source_get_height(source);
	if (!cx || !cy)
		return false;
	matrix4 m, inv;
	item_to_canvas(item, group, &m);
	if (!matrix4_inv(&inv, &m))
		return false;
	vec3 p;
	vec3_set(&p, pos.x, pos.y, 0.0f);
	vec3_transform(&p, &p, &inv);
	if (local)
		*local = p;
	// In a group, only what the group's own crop leaves showing.
	if (group && !point_in_item(group, nullptr, pos))
		return false;
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	return p.x >= float(crop.left) && p.y >= float(crop.top) && p.x <= float(int(cx) - crop.right) &&
	       p.y <= float(int(cy) - crop.bottom);
}

ItemRef item_at(const vec2 &canvas)
{
	ItemRef hit;
	each_preview_item([&](obs_sceneitem_t *item, obs_sceneitem_t *group) {
		// Referenced here, inside the scene's enumeration; the one it
		// replaces (lower down) let go.
		if (!obs_sceneitem_locked(item) && !(group && obs_sceneitem_locked(group)) &&
		    point_in_item(item, group, canvas))
			hit = ItemRef(item, group);
	});
	return hit;
}

void select_only(obs_sceneitem_t *target)
{
	obs_source_t *scene_source = preview_scene();
	if (obs_scene_t *scene = obs_scene_from_source(scene_source)) {
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				auto *t = static_cast<obs_sceneitem_t *>(param);
				if (obs_sceneitem_is_group(item))
					obs_sceneitem_group_enum_items(
						item,
						[](obs_scene_t *, obs_sceneitem_t *i, void *p) {
							obs_sceneitem_select(i, i == p);
							return true;
						},
						t);
				obs_sceneitem_select(item, item == t);
				return true;
			},
			target);
	}
	obs_source_release(scene_source);
}

} // namespace typeset
