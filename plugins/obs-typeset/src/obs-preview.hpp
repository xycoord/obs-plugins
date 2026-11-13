// What the editor needs from OBS's main window and scenes that OBS doesn't
// offer plugins: where the canvas is in the preview, whether the preview is
// set to Scale to Window, the preview's display (to draw the caret on), and
// OBS's hotkeys. Each is read the way OBS itself works it out, only in the OBS
// versions it's known to hold in, and fails safe (no editing, or no caret)
// where it can't be told.
#pragma once

#include "preview-geometry.hpp"

#include <graphics/matrix4.h>
#include <obs.h>

#include <QPointF>

#include <cstdint>
#include <type_traits>
#include <utility>

class QWidget;

namespace typeset {

// The OBS versions whose preview widget is laid out as preview_display()
// reads it, and whose Preview Scaling menu has the names scaling() looks for:
// 30 to 33. (The plugin needs OBS 30 or later anyway, for its sources'
// UUIDs; CI runs it on 30 and the latest.)
constexpr uint32_t known_obs_first = 30, known_obs_last = 33;

Mapping preview_mapping(QWidget *preview);
vec2 widget_to_canvas(const Mapping &m, QPointF p);
QPointF canvas_to_widget(const Mapping &m, const vec3 &p);

enum class Scaling { Window, Other, Unknown };
// Whether the preview is set to Scale to Window, from OBS's own Preview
// Scaling menu (Unknown: the menu isn't there to ask).
Scaling preview_scaling(QWidget *preview);
// Whether this OBS is one of the known versions.
bool known_obs();
// Whether text can be edited in the preview now: a known OBS, its preview
// at Scale to Window. Logs why not.
bool can_edit_in(QWidget *preview);

// The preview's obs_display_t, or null if it can't be found safely.
obs_display_t *preview_display(QWidget *preview);

// The scene the preview shows (in Studio Mode, the preview scene),
// referenced.
obs_source_t *preview_scene();

// OBS's hotkeys reading the keyboard in the background, off while typing.
void suspend_hotkeys();
void restore_hotkeys();

// A scene item's transform from its source's own units to the canvas,
// through the group it's in, if any (OBS doesn't nest groups), crops
// included.
void item_to_canvas(obs_sceneitem_t *item, obs_sceneitem_t *group, matrix4 *out);

// A scene item and the group it's in, each referenced while this holds them.
class ItemRef {
public:
	ItemRef() = default;
	ItemRef(obs_sceneitem_t *item, obs_sceneitem_t *group); // takes new references
	ItemRef(const ItemRef &o) : ItemRef(o.item_, o.group_) {}
	ItemRef(ItemRef &&o) noexcept : item_(std::exchange(o.item_, nullptr)), group_(std::exchange(o.group_, nullptr)) {}
	ItemRef &operator=(ItemRef o) noexcept
	{
		std::swap(item_, o.item_);
		std::swap(group_, o.group_);
		return *this;
	}
	~ItemRef() { reset(); }
	void reset();

	obs_sceneitem_t *item() const { return item_; }
	obs_sceneitem_t *group() const { return group_; }
	explicit operator bool() const { return item_ != nullptr; }

private:
	obs_sceneitem_t *item_ = nullptr, *group_ = nullptr;
};

// Whether a canvas point is on an item; where, in its source's units.
bool point_in_item(obs_sceneitem_t *item, obs_sceneitem_t *group, const vec2 &pos, vec3 *local = nullptr);

// Every visible item in a scene, groups opened, bottom to top.
template<typename F> void each_item(obs_scene_t *scene, F &&f)
{
	struct Walk {
		std::remove_reference_t<F> *f;
		obs_sceneitem_t *group;
	};
	auto visit = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *w = static_cast<Walk *>(param);
		if (!obs_sceneitem_visible(item))
			return true;
		if (obs_sceneitem_is_group(item)) {
			Walk inner{w->f, item};
			obs_sceneitem_group_enum_items(
				item,
				[](obs_scene_t *, obs_sceneitem_t *i, void *p) {
					auto *in = static_cast<Walk *>(p);
					if (obs_sceneitem_visible(i))
						(*in->f)(i, in->group);
					return true;
				},
				&inner);
			return true;
		}
		(*w->f)(item, w->group);
		return true;
	};
	Walk w{&f, nullptr};
	obs_scene_enum_items(scene, visit, &w);
}

// The same, over the preview's scene.
template<typename F> void each_preview_item(F &&f)
{
	obs_source_t *scene_source = preview_scene();
	if (obs_scene_t *scene = obs_scene_from_source(scene_source))
		each_item(scene, f);
	obs_source_release(scene_source);
}

// The topmost unlocked item under a canvas point in the preview's scene.
ItemRef item_at(const vec2 &canvas);

// OBS's selection in the preview's scene set to just this item.
void select_only(obs_sceneitem_t *item);

} // namespace typeset
