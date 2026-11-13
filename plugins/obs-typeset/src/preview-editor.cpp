#include "preview-editor.hpp"

#include "rich-text.hpp"
#include "typeset-source.hpp"

#include <graphics/matrix4.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <strings.h>
#include <util/config-file.h>
#include <mutex>
#include <string>
#include <vector>

namespace typeset {

namespace {

// Colours of the editing marks, 0xRRGGBBAA. The accent is a clear blue that
// isn't OBS's red selection box, so editing reads as a different state.
constexpr uint32_t accent = 0x2F8FFFFF;
constexpr uint32_t accent_edge = 0x0A1830D0; // dark edge round the accent, so it shows on blue too
constexpr uint32_t selection_keyline = 0xFFFFFFC0;
constexpr uint32_t tag_fill = 0x10141CE8;
constexpr uint32_t veil = 0x00000040; // over the rest of the canvas while editing
constexpr uint32_t selection_fill = 0x2F6FE0E6; // nearly opaque: the selected glyphs are drawn again on it
constexpr uint32_t selection_wash = 0x2F8FFF70; // when they can't be (a rotated source)
constexpr uint32_t placeholder_line = 0xFFFFFFB0;

// How long the caret stays solid after a key or click, and its blink.
constexpr uint64_t caret_hold_ms = 500, caret_blink_ms = 530;

// --- Where the canvas is in OBS's preview -------------------------------------------------
//
// OBS draws the canvas into its preview widget scaled to fit, centred, inside
// a 10-pixel margin ("Scale to window", the default). This is OBS's own
// calculation (OBSBasic::ResizePreview and GetScaleAndCenterPos, the same from
// OBS 30 to 33). Zoomed or at a fixed scale it doesn't hold, and editing isn't
// started: OBS's own menu says which it is (see scaled_to_window()).

constexpr int preview_edge = 10;

struct Mapping {
	double x = 0, y = 0, scale = 1; // canvas -> display pixels
	double dpr = 1;                 // display pixels per widget unit
};

Mapping preview_mapping(QWidget *preview)
{
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	Mapping m;
	m.dpr = preview->devicePixelRatioF();
	const QSize size = preview->size() * m.dpr;
	const int base_cx = int(ovi.base_width), base_cy = int(ovi.base_height);
	const int window_cx = size.width() - preview_edge * 2, window_cy = size.height() - preview_edge * 2;
	if (base_cx <= 0 || base_cy <= 0 || window_cx <= 0 || window_cy <= 0)
		return m;

	const double window_aspect = double(window_cx) / double(window_cy);
	const double base_aspect = double(base_cx) / double(base_cy);
	int new_cx, new_cy;
	float scale;
	if (window_aspect > base_aspect) {
		scale = float(window_cy) / float(base_cy);
		new_cx = int(double(window_cy) * base_aspect);
		new_cy = window_cy;
	} else {
		scale = float(window_cx) / float(base_cx);
		new_cx = window_cx;
		new_cy = int(float(window_cx) / base_aspect);
	}
	m.x = double(window_cx / 2 - new_cx / 2 + preview_edge);
	m.y = double(window_cy / 2 - new_cy / 2 + preview_edge);
	m.scale = double(scale);
	return m;
}

vec2 widget_to_canvas(const Mapping &m, QPointF p)
{
	vec2 out;
	vec2_set(&out, float((p.x() * m.dpr - m.x) / m.scale), float((p.y() * m.dpr - m.y) / m.scale));
	return out;
}

// Whether the preview is set to Scale to Window. OBS keeps its Preview
// Scaling menu's ticks up to date only when the menu opens, so it's asked to
// update them first (the menu's "about to show" handler, by name: OBS 30 to
// 33 have it). If it can't be told, text isn't edited in the preview.
bool scaled_to_window(QWidget *preview)
{
	QWidget *main = preview->window();
	QMenu *menu = main ? main->findChild<QMenu *>("scalingMenu") : nullptr;
	QAction *window = main ? main->findChild<QAction *>("actionScaleWindow") : nullptr;
	if (!menu || !window)
		return false; // can't tell: not edited, rather than edited at the wrong place
	QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
	return window->isChecked();
}

// The preview's obs_display_t, to draw on. OBS doesn't offer it to plugins;
// the preview widget is an OBSQTDisplay, whose first member is an OBSDisplay
// (one obs_display_t pointer) straight after its QWidget base. That holds in
// OBS 28 to 33 (the Qt 6 ones, which this is built for), the versions it's read in; in any other, nothing is drawn (no
// caret) rather than risk reading the wrong thing. Within those versions it's
// still checked against the display's size before use.
obs_display_t *preview_display(QWidget *preview)
{
	const uint32_t major = obs_get_version() >> 24;
	if (major < 28 || major > 33)
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

// The scene the preview shows: in Studio Mode, the preview scene.
obs_source_t *preview_scene()
{
	return obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
							  : obs_frontend_get_current_scene();
}

// --- OBS's hotkeys while typing ------------------------------------------------------------
//
// By default OBS's hotkeys work even while its window has focus: on Linux,
// libobs then reads the keyboard itself (not through Qt), so a key bound to a
// hotkey would act while typed into the text. That "background" reading is
// off while editing, and put back after as OBS sets it (OBSApp's
// ResetHotkeyState): on unless the user chose to disable hotkeys in or out of
// focus. The setting is in OBS's user config from OBS 31, the global one
// before; the user config's getter doesn't exist before 31, so it's looked up.

config_t *hotkey_config()
{
	using getter = config_t *(*)(void);
	static const auto user = reinterpret_cast<getter>(dlsym(RTLD_DEFAULT, "obs_frontend_get_user_config"));
	static const auto global = reinterpret_cast<getter>(dlsym(RTLD_DEFAULT, "obs_frontend_get_global_config"));
	return user ? user() : global ? global() : nullptr;
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

// --- Finding text in a scene ----------------------------------------------------------------

// A scene item's transform from its source's own units to the canvas,
// through the group it's in, if any (OBS doesn't nest groups).
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
		matrix4 g;
		obs_sceneitem_get_draw_transform(group, &g);
		matrix4_mul(out, out, &g);
	}
}

struct Hit {
	obs_sceneitem_t *item = nullptr;
	obs_sceneitem_t *group = nullptr;
};

// A hit's references (item and group) let go.
void release_hit(Hit &h)
{
	if (h.item)
		obs_sceneitem_release(h.item);
	if (h.group)
		obs_sceneitem_release(h.group);
	h = Hit();
}

bool point_in_item(obs_sceneitem_t *item, obs_sceneitem_t *group, const vec2 &pos, vec3 *local = nullptr)
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
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	return p.x >= float(crop.left) && p.y >= float(crop.top) && p.x <= float(int(cx) - crop.right) &&
	       p.y <= float(int(cy) - crop.bottom);
}

// Every visible item in a scene, groups opened, bottom to top.
template<typename F> void each_item(obs_scene_t *scene, F &&f)
{
	struct Walk {
		F *f;
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
				[](obs_scene_t *s, obs_sceneitem_t *i, void *p) {
					auto *in = static_cast<Walk *>(p);
					if (obs_sceneitem_visible(i))
						(*in->f)(i, in->group);
					(void)s;
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

// --- Labels drawn on the preview ----------------------------------------------------------------

// A short piece of text as a texture, made once (on OBS's graphics thread,
// with a font map of its own there).
struct Label {
	gs_texture_t *texture = nullptr;
	int width = 0, height = 0;

	void make(const char *text, double px, uint32_t fg, uint32_t bg)
	{
		static PangoContext *context = create_context(pango_cairo_font_map_new());
		PangoLayout *layout = pango_layout_new(context);
		PangoFontDescription *desc = pango_font_description_from_string("Sans Bold");
		pango_font_description_set_absolute_size(desc, px * PANGO_SCALE);
		pango_layout_set_font_description(layout, desc);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, text, -1);
		int tw = 0, th = 0;
		pango_layout_get_pixel_size(layout, &tw, &th);
		const int pad = int(px * 0.45);
		width = tw + 2 * pad;
		height = th + pad;
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
		if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(surface);
			g_object_unref(layout);
			return;
		}
		cairo_t *cr = cairo_create(surface);
		auto rgba = [&](uint32_t c) {
			cairo_set_source_rgba(cr, (c >> 24) / 255.0, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0,
					      (c & 0xFF) / 255.0);
		};
		if (bg & 0xFF) {
			const double r = height / 2.0;
			cairo_new_sub_path(cr);
			cairo_arc(cr, width - r, r, r, -M_PI / 2, M_PI / 2);
			cairo_arc(cr, r, r, r, M_PI / 2, 3 * M_PI / 2);
			cairo_close_path(cr);
			rgba(bg);
			cairo_fill(cr);
		}
		cairo_move_to(cr, pad, pad / 2.0);
		rgba(fg);
		pango_cairo_show_layout(cr, layout);
		cairo_destroy(cr);
		cairo_surface_flush(surface);
		std::vector<uint8_t> pixels(size_t(width) * height * 4);
		const int stride = cairo_image_surface_get_stride(surface);
		for (int y = 0; y < height; y++)
			memcpy(&pixels[size_t(y) * width * 4], cairo_image_surface_get_data(surface) + size_t(y) * stride,
			       size_t(width) * 4);
		cairo_surface_destroy(surface);
		g_object_unref(layout);
		const uint8_t *data = pixels.data();
		texture = gs_texture_create(uint32_t(width), uint32_t(height), GS_BGRA, 1, &data, 0);
	}

	void draw(float x, float y) const
	{
		if (!texture)
			return;
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_matrix_push();
		gs_matrix_translate3f(std::round(x), std::round(y), 0.0f);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture, 0, 0, 0);
		gs_matrix_pop();
		gs_blend_state_pop();
	}

	void destroy()
	{
		if (texture)
			gs_texture_destroy(texture);
		texture = nullptr;
	}
};

// --- The editor -----------------------------------------------------------------------------

struct Snapshot {
	std::string text;
	Spans spans;
	size_t caret = 0, anchor = 0;
};

enum class EditKind { None, Typing, Deleting, Other };

struct Quad {
	vec2 p[4]; // in order round the edge
};

// The format bar above the text being edited: bold, italic, underline,
// smaller, bigger, colour, clear. In display pixels.
enum class BarAction { Bold, Italic, Underline, Smaller, SizeReadout, Bigger, Colour, Clear };
struct BarButton {
	BarAction action;
	float x = 0, y = 0, w = 0, h = 0;
	bool active = false;
	uint32_t swatch = 0; // the colour button's colour, 0xAABBGGRR
	std::string text;    // the size readout's
};
const char *bar_glyph(BarAction a)
{
	switch (a) {
	case BarAction::Bold:
		return "B";
	case BarAction::Italic:
		return "I";
	case BarAction::Underline:
		return "U";
	case BarAction::Smaller:
		return "A\u2212";
	case BarAction::Bigger:
		return "A+";
	case BarAction::Colour:
		return "A";
	case BarAction::SizeReadout:
		return "";
	case BarAction::Clear:
		return "\u00d7";
	}
	return "";
}

class PreviewEditor : public QObject {
public:
	explicit PreviewEditor(QWidget *preview);
	~PreviewEditor() override;

	bool eventFilter(QObject *obj, QEvent *event) override;
	void end_edit();
	void scene_changed()
	{
		end_edit();
		clear_placeholders();
	}
	void clear_placeholders();

private:
	QPointer<QWidget> preview_;
	QPointer<QWindow> window_;
	QTimer timer_;
	bool warned_display_ = false;

	// The edit under way.
	obs_sceneitem_t *item_ = nullptr; // referenced
	obs_sceneitem_t *group_ = nullptr;
	obs_source_t *source_ = nullptr; // referenced
	TextSource *text_ = nullptr;
	std::string doc_;
	Spans spans_;                      // the text's styled runs
	std::vector<StyleChange> pending_; // styles for what's typed next (Ctrl+B with no selection)
	Document shown_;                   // as the overlay was last drawn for
	size_t caret_ = 0, anchor_ = 0;
	double column_x_ = -1; // the column Up and Down keep to
	bool dragging_ = false;
	std::pair<size_t, size_t> drag_unit_{0, 0}; // a word or paragraph a drag started on
	enum class Unit { Grapheme, Word, Paragraph } drag_by_ = Unit::Grapheme;
	bool swallow_release_ = false;
	uint64_t last_double_click_ = 0;
	QPointF last_double_click_pos_;
	Qt::FocusPolicy saved_focus_policy_ = Qt::NoFocus;
	bool saved_input_method_ = false;
	std::vector<Snapshot> undo_, redo_;
	EditKind last_edit_ = EditKind::None;

	// What the preview's draw callback draws, guarded by its lock. The
	// scene-item references in it are the overlay's own.
	std::mutex overlay_mutex_;
	obs_display_t *display_ = nullptr;
	obs_display_t *forgotten_ = nullptr; // let go when the preview hid; only compared
	struct Target {
		obs_sceneitem_t *item = nullptr, *group = nullptr;
	};
	struct Overlay {
		bool editing = false;
		Target target;
		Mapping mapping;
		Caret caret;
		bool show_caret = true;
		std::vector<Box> selection;
		Box box;
		bool grip = false;       // the text box's width grip
		bool plate = false;      // a background plate behind the text
		std::string tag;         // what the tag says
		std::vector<BarButton> bar;
		float tag_x = 0, strip_y = 0, strip_h = 0; // where the tag goes, after the bar
		std::string preedit;
		uint64_t blink_from = 0;
		std::vector<Target> placeholders; // empty texts
	} overlay_;
	Label editing_label_, placeholder_label_; // graphics thread only
	Label bar_labels_[8];
	Label size_label_;
	std::string size_label_text_;
	std::vector<BarButton> bar_; // as last drawn, for clicks (display pixels)
	void bar_action(BarAction action);
	std::string editing_label_text_;
	// Dragging the text box's width grip.
	bool resizing_ = false;
	QPointF resize_from_pos_;
	double resize_from_width_ = 0;
	bool on_grip(QPointF widget_pos);
	std::string preedit_; // an input method's composition, not yet in the text
	Label preedit_label_;
	std::string preedit_label_text_;

	bool editing() const { return item_ != nullptr; }
	static void source_updated(void *param, calldata_t *)
	{
		QMetaObject::invokeMethod(
			static_cast<PreviewEditor *>(param),
			[e = static_cast<PreviewEditor *>(param)] {
				if (e->editing() && !(e->text_->document() == e->shown_)) {
					e->sync_text();
					e->refresh_overlay();
				}
			},
			Qt::QueuedConnection);
	}
	bool begin_edit(const Hit &hit);
	bool still_editable();
	vec3 to_source(QPointF widget_pos, bool *inside = nullptr);
	void on_timer();

	void sync_text();
	void set_caret(size_t caret, bool extend);
	void replace(size_t from, size_t to, const std::string &with, EditKind kind);
	void replace_selection(const std::string &s, EditKind kind);
	bool has_selection() const { return caret_ != anchor_; }
	size_t sel_from() const { return std::min(caret_, anchor_); }
	size_t sel_to() const { return std::max(caret_, anchor_); }
	void undo();
	void redo();
	void restore(Snapshot s);
	void commit();
	void style(StyleChange change, bool toggle);

	Hit item_at(QPointF pos);
	void select_only(obs_sceneitem_t *item);
	const obs_sceneitem_t *pressed_item_ = nullptr; // only compared, never used
	bool pressed_selected_ = false;

	bool key_press(QKeyEvent *event);
	bool mouse_event(QMouseEvent *event);
	bool edit_selected();
	void input_method(QInputMethodEvent *event);
	QVariant input_method_query(Qt::InputMethodQuery query);

	void attach_display();
	void detach_display();
	void forget_display();
	void watch_window();
	void refresh_overlay();
	void refresh_placeholders();
	static void release(Target &t);
	static void draw(void *param, uint32_t cx, uint32_t cy);
	void draw_overlay(uint32_t cx, uint32_t cy);
};

PreviewEditor *editor = nullptr;

PreviewEditor::PreviewEditor(QWidget *preview) : preview_(preview)
{
	preview->installEventFilter(this);
	watch_window();
	// So the preview can take keys (F2) once it's clicked.
	if (preview->focusPolicy() == Qt::NoFocus)
		preview->setFocusPolicy(Qt::ClickFocus);
	// Twice a second: placeholders for empty texts, and whether the text
	// being edited is still there and unchanged from elsewhere. It only
	// looks at the preview's scene; nothing is drawn or laid out.
	// OBS turns hotkeys' background reading back on when the application
	// becomes active; while editing, it's turned off again (this runs after
	// OBS's own handler, connected first).
	QObject::connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
		if (!editing())
			return;
		if (state == Qt::ApplicationActive)
			obs_hotkey_enable_background_press(false);
		else
			restore_hotkeys(); // in the background, hotkeys work as OBS has them
	});
	timer_.setInterval(500);
	QObject::connect(&timer_, &QTimer::timeout, this, [this] { on_timer(); });
	timer_.start();
	attach_display();
}

PreviewEditor::~PreviewEditor()
{
	end_edit();
	detach_display();
	clear_placeholders();
	obs_enter_graphics();
	editing_label_.destroy();
	placeholder_label_.destroy();
	for (Label &l : bar_labels_)
		l.destroy();
	size_label_.destroy();
	preedit_label_.destroy();
	obs_leave_graphics();
	if (preview_)
		preview_->removeEventFilter(this);
	if (window_)
		window_->removeEventFilter(this);
}

// The preview's native window, whose destruction takes the display with it.
// Qt can replace it (re-docking, a screen change), so it's followed.
void PreviewEditor::watch_window()
{
	QWindow *w = preview_ ? preview_->windowHandle() : nullptr;
	if (w == window_)
		return;
	if (window_)
		window_->removeEventFilter(this);
	window_ = w;
	if (w)
		w->installEventFilter(this);
}

void PreviewEditor::on_timer()
{
	if (!preview_)
		return;
	watch_window();
	// The display can be replaced (OBS makes a new one when the preview is
	// shown again): a different pointer means the old one has gone.
	if (obs_display_t *now = preview_ && preview_->isVisible() ? preview_display(preview_) : nullptr;
	    now && now != display_) {
		forget_display();
		attach_display();
	}
	if (editing()) {
		if (!still_editable() || !scaled_to_window(preview_)) {
			end_edit();
		} else if (!(text_->document() == shown_)) {
			// Changed from its properties: draw the caret for it.
			sync_text();
			refresh_overlay();
		}
	}
	refresh_placeholders();
}

void PreviewEditor::refresh_placeholders()
{
	std::vector<Target> found;
	if (preview_ && preview_->isVisible()) {
		obs_source_t *scene_source = preview_scene();
		if (obs_scene_t *scene = obs_scene_from_source(scene_source)) {
			each_item(scene, [&](obs_sceneitem_t *item, obs_sceneitem_t *group) {
				TextSource *t = TextSource::from(obs_sceneitem_get_source(item));
				if (t && item != item_ && t->text().empty()) {
					obs_sceneitem_addref(item);
					if (group)
						obs_sceneitem_addref(group);
					found.push_back({item, group});
				}
			});
		}
		obs_source_release(scene_source);
	}
	const Mapping mapping = preview_ ? preview_mapping(preview_) : Mapping();
	std::vector<Target> old;
	{
		std::lock_guard lock(overlay_mutex_);
		old.swap(overlay_.placeholders);
		overlay_.placeholders = std::move(found);
		if (!overlay_.editing)
			overlay_.mapping = mapping;
	}
	for (Target &t : old)
		release(t);
}

void PreviewEditor::clear_placeholders()
{
	std::vector<Target> old;
	{
		std::lock_guard lock(overlay_mutex_);
		old.swap(overlay_.placeholders);
	}
	for (Target &t : old)
		release(t);
}

void PreviewEditor::release(Target &t)
{
	if (t.item)
		obs_sceneitem_release(t.item);
	if (t.group)
		obs_sceneitem_release(t.group);
	t = Target();
}

vec3 PreviewEditor::to_source(QPointF widget_pos, bool *inside)
{
	const vec2 canvas = widget_to_canvas(preview_mapping(preview_), widget_pos);
	vec3 local;
	vec3_zero(&local);
	const bool in = point_in_item(item_, group_, canvas, &local);
	if (inside)
		*inside = in;
	return local;
}

bool PreviewEditor::still_editable()
{
	if (!preview_ || !preview_->isVisible() || !obs_sceneitem_visible(item_) ||
	    (group_ && !obs_sceneitem_visible(group_)))
		return false;
	obs_source_t *scene_source = preview_scene();
	bool found = false;
	if (obs_scene_t *scene = obs_scene_from_source(scene_source))
		each_item(scene, [&](obs_sceneitem_t *item, obs_sceneitem_t *) { found = found || item == item_; });
	obs_source_release(scene_source);
	return found;
}

bool PreviewEditor::begin_edit(const Hit &hit)
{
	obs_source_t *source = obs_sceneitem_get_source(hit.item);
	TextSource *text = TextSource::from(source);
	if (!text)
		return false;
	end_edit();

	item_ = hit.item;
	obs_sceneitem_addref(item_);
	group_ = hit.group;
	if (group_)
		obs_sceneitem_addref(group_);
	source_ = obs_source_get_ref(source);
	text_ = text;
	resizing_ = dragging_ = false;
	preedit_.clear();
	const Document d = text_->document();
	doc_ = d.text;
	spans_ = d.spans;
	pending_.clear();
	caret_ = anchor_ = 0;
	column_x_ = -1;
	undo_.clear();
	redo_.clear();
	last_edit_ = EditKind::None;

	saved_focus_policy_ = preview_->focusPolicy();
	if (saved_focus_policy_ == Qt::NoFocus)
		preview_->setFocusPolicy(Qt::ClickFocus);
	saved_input_method_ = preview_->testAttribute(Qt::WA_InputMethodEnabled);
	preview_->setAttribute(Qt::WA_InputMethodEnabled, true);
	preview_->setFocus(Qt::MouseFocusReason);
	QGuiApplication::inputMethod()->update(Qt::ImEnabled);
	preview_->setCursor(Qt::IBeamCursor);

	obs_hotkey_enable_background_press(false);
	// Changes from the dialog (font, box) redraw the caret and frame at
	// once; OBS signals them on its graphics thread, so they're passed on.
	signal_handler_connect(obs_source_get_signal_handler(source_), "update", source_updated, this);
	blog(LOG_INFO, "[obs-typeset] editing '%s' in the preview", obs_source_get_name(source));
	refresh_placeholders(); // it isn't one while it's being edited
	return true;
}

void PreviewEditor::end_edit()
{
	if (!editing())
		return;
	dragging_ = false;
	resizing_ = false;
	swallow_release_ = false;
	preedit_.clear();
	Target old;
	{
		std::lock_guard lock(overlay_mutex_);
		old = overlay_.target;
		overlay_.target = Target();
		overlay_.editing = false;
		overlay_.selection.clear();
		overlay_.bar.clear();
	}
	release(old); // outside the lock (see attach_display)
	bar_.clear();
	if (preview_) {
		preview_->setAttribute(Qt::WA_InputMethodEnabled, saved_input_method_);
		preview_->setFocusPolicy(saved_focus_policy_);
		preview_->unsetCursor();
		QGuiApplication::inputMethod()->update(Qt::ImEnabled);
	}
	restore_hotkeys();
	signal_handler_disconnect(obs_source_get_signal_handler(source_), "update", source_updated, this);
	obs_sceneitem_release(item_);
	if (group_)
		obs_sceneitem_release(group_);
	obs_source_release(source_);
	item_ = group_ = nullptr;
	source_ = nullptr;
	text_ = nullptr;
	undo_.clear();
	redo_.clear();
	refresh_placeholders();
}

// --- Editing --------------------------------------------------------------------------------

// The text may have been changed elsewhere (its properties) since the last
// key: take it as it is, keeping the caret where it can be, on a character.
void PreviewEditor::sync_text()
{
	Document now = text_->document();
	if (now.text == doc_ && now.spans == spans_)
		return;
	doc_ = std::move(now.text);
	spans_ = std::move(now.spans);
	caret_ = text_->cursor_at_or_before(caret_);
	anchor_ = text_->cursor_at_or_before(anchor_);
	undo_.clear();
	redo_.clear();
	last_edit_ = EditKind::None;
}

void PreviewEditor::set_caret(size_t caret, bool extend)
{
	caret_ = std::min(caret, doc_.size());
	if (!extend)
		anchor_ = caret_;
	last_edit_ = EditKind::None;
	pending_.clear();
}

void PreviewEditor::replace(size_t from, size_t to, const std::string &with, EditKind kind)
{
	from = std::min(from, doc_.size());
	to = std::clamp(to, from, doc_.size());
	if (from == to && with.empty())
		return;
	// Runs of typing, or of deleting, undo together, a word at a time.
	const bool word_break = kind == EditKind::Typing && (with == " " || with == "\n");
	if (kind == EditKind::Other || kind != last_edit_ || undo_.empty() || word_break)
		undo_.push_back({doc_, spans_, caret_, anchor_});
	constexpr size_t undo_limit = 200;
	if (undo_.size() > undo_limit)
		undo_.erase(undo_.begin());
	redo_.clear();
	last_edit_ = kind;

	doc_.replace(from, to - from, with);
	edit(spans_, from, to, with.size());
	// Styles chosen with nothing selected go on what's typed.
	for (const StyleChange &c : pending_)
		apply(spans_, from, from + with.size(), c, doc_);
	caret_ = anchor_ = from + with.size();
	column_x_ = -1;
	commit();
}

void PreviewEditor::commit()
{
	text_->set_content(doc_, spans_);
}

void PreviewEditor::restore(Snapshot s)
{
	doc_ = std::move(s.text);
	spans_ = std::move(s.spans);
	caret_ = s.caret;
	anchor_ = s.anchor;
	last_edit_ = EditKind::None;
	pending_.clear();
	commit();
}

// Bold, italic and so on: on the selection, or, with nothing selected, on
// what's typed next. Toggled ones go off if the whole selection has them.
void PreviewEditor::style(StyleChange change, bool toggle)
{
	if (!has_selection()) {
		if (toggle) {
			auto same = std::find_if(pending_.begin(), pending_.end(),
						 [&](const StyleChange &c) { return c.field == change.field; });
			const bool on_here = all_on(spans_, caret_ > 0 ? caret_ - 1 : 0, caret_, change.field);
			if (same != pending_.end()) {
				pending_.erase(same);
				return;
			}
			change.value = on_here ? -1 : 1;
		}
		// The latest of each kind wins; clearing starts afresh.
		if (change.field == StyleChange::Clear)
			pending_.clear();
		pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
					      [&](const StyleChange &c) { return c.field == change.field; }),
			       pending_.end());
		pending_.push_back(change);
		return;
	}
	if (toggle)
		change.value = all_on(spans_, sel_from(), sel_to(), change.field) ? -1 : 1;
	undo_.push_back({doc_, spans_, caret_, anchor_});
	redo_.clear();
	last_edit_ = EditKind::Other;
	apply(spans_, sel_from(), sel_to(), change, doc_);
	commit();
}

void PreviewEditor::replace_selection(const std::string &s, EditKind kind)
{
	replace(sel_from(), sel_to(), s, has_selection() ? EditKind::Other : kind);
	last_edit_ = kind;
}

void PreviewEditor::undo()
{
	if (undo_.empty())
		return;
	redo_.push_back({doc_, spans_, caret_, anchor_});
	Snapshot s = std::move(undo_.back());
	undo_.pop_back();
	restore(std::move(s));
}

void PreviewEditor::redo()
{
	if (redo_.empty())
		return;
	undo_.push_back({doc_, spans_, caret_, anchor_});
	Snapshot s = std::move(redo_.back());
	redo_.pop_back();
	restore(std::move(s));
}

// Printable text from a key: every code point (not UTF-16 unit, so emoji
// count) printable.
bool printable(const QString &text)
{
	if (text.isEmpty())
		return false;
	for (char32_t c : text.toUcs4())
		if (!QChar::isPrint(c))
			return false;
	return true;
}

bool PreviewEditor::key_press(QKeyEvent *event)
{
	sync_text();
	const Qt::KeyboardModifiers mods = event->modifiers();
	const bool shift = mods & Qt::ShiftModifier;
	const bool word = mods & Qt::ControlModifier;

	if (event->matches(QKeySequence::SelectAll)) {
		anchor_ = 0;
		caret_ = doc_.size();
		last_edit_ = EditKind::None;
	} else if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) {
		if (has_selection()) {
			const std::string s = doc_.substr(sel_from(), sel_to() - sel_from());
			QGuiApplication::clipboard()->setText(QString::fromStdString(s));
			if (event->matches(QKeySequence::Cut))
				replace(sel_from(), sel_to(), "", EditKind::Other);
		}
	} else if (event->matches(QKeySequence::Paste) ||
		   (event->key() == Qt::Key_V && mods == (Qt::ControlModifier | Qt::ShiftModifier))) {
		// Always as plain text.
		std::string s = QGuiApplication::clipboard()->text().toStdString();
		// Control characters other than line breaks and tabs would be
		// cut off or shown as boxes.
		s.erase(std::remove_if(s.begin(), s.end(),
				       [](char c) { return (unsigned char)c < 0x20 && c != '\n' && c != '\t'; }),
			s.end());
		if (!s.empty())
			replace(sel_from(), sel_to(), s, EditKind::Other);
	} else if (event->matches(QKeySequence::Bold) || event->matches(QKeySequence::Italic) ||
		   event->matches(QKeySequence::Underline)) {
		StyleChange c;
		c.field = event->matches(QKeySequence::Bold)     ? StyleChange::Bold
			  : event->matches(QKeySequence::Italic) ? StyleChange::Italic
								 : StyleChange::Underline;
		style(c, true);
	} else if (word && (event->key() == Qt::Key_Greater || event->key() == Qt::Key_Less ||
			    event->key() == Qt::Key_BracketRight || event->key() == Qt::Key_BracketLeft)) {
		// Bigger or smaller: the selection (or what's typed next), a tenth
		// of the base size at a time.
		{
			const bool up = event->key() == Qt::Key_Greater || event->key() == Qt::Key_BracketRight;
			const Span at = style_at(spans_, has_selection() ? sel_from() : caret_ > 0 ? caret_ - 1 : 0);
			StyleChange c;
			c.field = StyleChange::Size;
			const int now = at.size ? at.size : 100;
			c.value = std::clamp(now + (up ? 10 : -10), 10, 1000);
			if (c.value == 100)
				c.value = 0;
			style(c, false);
		}
	} else if (word && event->key() == Qt::Key_Space) {
		// Back to the text's own style (the selection, or what's typed next).
		StyleChange c;
		c.field = StyleChange::Clear;
		style(c, false);
	} else if (event->matches(QKeySequence::Undo)) {
		undo();
	} else if (event->matches(QKeySequence::Redo)) {
		redo();
	} else {
		switch (event->key()) {
		case Qt::Key_Escape:
			end_edit();
			return true;
		case Qt::Key_Return:
		case Qt::Key_Enter:
			if (word) { // Ctrl+Return finishes, as in slide editors
				end_edit();
				return true;
			}
			replace_selection("\n", EditKind::Other);
			break;
		case Qt::Key_Left:
		case Qt::Key_Right: {
			const int dir = event->key() == Qt::Key_Right ? 1 : -1;
			if (has_selection() && !shift) {
				set_caret(dir > 0 ? sel_to() : sel_from(), false);
			} else {
				set_caret(word ? text_->move_by_word(caret_, dir) : text_->move_visually(caret_, dir),
					  shift);
			}
			column_x_ = -1;
			break;
		}
		case Qt::Key_Up:
		case Qt::Key_Down: {
			if (column_x_ < 0)
				column_x_ = text_->caret(caret_).x;
			const double column = column_x_;
			set_caret(text_->move_by_line(caret_, event->key() == Qt::Key_Down ? 1 : -1, column), shift);
			column_x_ = column;
			break;
		}
		case Qt::Key_Home:
		case Qt::Key_End: {
			const int dir = event->key() == Qt::Key_End ? 1 : -1;
			set_caret(word ? (dir > 0 ? doc_.size() : 0) : text_->line_edge(caret_, dir), shift);
			column_x_ = -1;
			break;
		}
		case Qt::Key_Backspace:
		case Qt::Key_Delete: {
			const int dir = event->key() == Qt::Key_Delete ? 1 : -1;
			if (has_selection()) {
				replace(sel_from(), sel_to(), "", EditKind::Other);
			} else {
				const size_t to = word ? text_->move_by_word(caret_, dir) : text_->next_cursor(caret_, dir);
				replace(std::min(caret_, to), std::max(caret_, to), "", EditKind::Deleting);
			}
			break;
		}
		case Qt::Key_Tab:
			replace_selection("\t", EditKind::Typing);
			break;
		default:
			// Ctrl (without the Alt of AltGr) or Meta makes it a command,
			// not text.
			if ((word && !(mods & Qt::AltModifier)) || (mods & Qt::MetaModifier))
				break;
			if (printable(event->text()))
				replace_selection(event->text().toStdString(), EditKind::Typing);
			break;
		}
	}
	refresh_overlay();
	return true;
}

// F2 on the preview edits the selected text, with all of it selected, so
// typing replaces it.
bool PreviewEditor::edit_selected()
{
	obs_source_t *scene_source = preview_scene();
	Hit hit;
	int count = 0;
	if (obs_scene_t *scene = obs_scene_from_source(scene_source)) {
		each_item(scene, [&](obs_sceneitem_t *item, obs_sceneitem_t *group) {
			if (obs_sceneitem_selected(item) && TextSource::from(obs_sceneitem_get_source(item))) {
				obs_sceneitem_addref(item); // inside the enumeration
				if (group)
					obs_sceneitem_addref(group);
				release_hit(hit);
				hit = {item, group};
				count++;
			}
		});
	}
	obs_source_release(scene_source);
	struct Release {
		Hit &h;
		~Release() { release_hit(h); }
	} release{hit};
	if (count != 1)
		return false;
	if (obs_sceneitem_locked(hit.item) || (hit.group && obs_sceneitem_locked(hit.group)))
		return false;
	if (!scaled_to_window(preview_)) {
		blog(LOG_INFO, "[obs-typeset] the preview isn't set to Scale to Window, so text isn't edited "
			       "in it (right-click the preview: Preview Scaling, Scale to Window)");
		return false;
	}
	if (!begin_edit(hit))
		return false;
	anchor_ = 0;
	caret_ = doc_.size();
	refresh_overlay();
	return true;
}

void PreviewEditor::bar_action(BarAction action)
{
	sync_text();
	StyleChange c;
	switch (action) {
	case BarAction::Bold:
	case BarAction::Italic:
	case BarAction::Underline:
		c.field = action == BarAction::Bold     ? StyleChange::Bold
			  : action == BarAction::Italic ? StyleChange::Italic
							: StyleChange::Underline;
		style(c, true);
		break;
	case BarAction::Smaller:
	case BarAction::Bigger: {
		const Span at = style_at(spans_, has_selection() ? sel_from() : caret_ > 0 ? caret_ - 1 : 0);
		c.field = StyleChange::Size;
		const int now = at.size ? at.size : 100;
		c.value = std::clamp(now + (action == BarAction::Bigger ? 10 : -10), 10, 1000);
		if (c.value == 100)
			c.value = 0;
		style(c, false);
		break;
	}
	case BarAction::Colour: {
		const Span at = style_at(spans_, has_selection() ? sel_from() : caret_ > 0 ? caret_ - 1 : 0);
		const uint32_t now = at.has_colour ? at.colour : text_->document().style.colour;
		const QColor initial(now & 0xFF, (now >> 8) & 0xFF, (now >> 16) & 0xFF, (now >> 24) & 0xFF);
		// The colour dialog takes the focus for a while; editing carries on.
		const QColor picked = QColorDialog::getColor(initial, preview_, obs_module_text("TextColour"),
							     QColorDialog::ShowAlphaChannel);
		if (preview_)
			preview_->setFocus(Qt::OtherFocusReason);
		if (!editing() || !picked.isValid())
			break;
		c.field = StyleChange::Colour;
		c.colour = uint32_t(picked.red()) | uint32_t(picked.green()) << 8 | uint32_t(picked.blue()) << 16 |
			   uint32_t(picked.alpha()) << 24;
		style(c, false);
		break;
	}
	case BarAction::SizeReadout:
		break;
	case BarAction::Clear:
		c.field = StyleChange::Clear;
		style(c, false);
		break;
	}
	refresh_overlay();
}

// Whether a point is on the text box's width grip (a little generous: it's
// small).
bool PreviewEditor::on_grip(QPointF pos)
{
	const Document doc = text_->document();
	if (!doc.style.box || doc.style.box_width <= 0)
		return false;
	const Mapping map = preview_mapping(preview_);
	matrix4 m;
	item_to_canvas(item_, group_, &m);
	const Box b = text_->box();
	vec3 p;
	vec3_set(&p, float(b.w), float(b.h * 0.5), 0.0f);
	vec3_transform(&p, &p, &m);
	const double gx = (map.x + p.x * map.scale) / map.dpr + 7, gy = (map.y + p.y * map.scale) / map.dpr;
	return std::fabs(pos.x() - gx) <= 8 && std::fabs(pos.y() - gy) <= 18;
}

// The topmost unlocked item under a point in the preview, referenced.
Hit PreviewEditor::item_at(QPointF pos)
{
	obs_source_t *scene_source = preview_scene();
	const vec2 canvas = widget_to_canvas(preview_mapping(preview_), pos);
	Hit hit;
	if (obs_scene_t *scene = obs_scene_from_source(scene_source)) {
		each_item(scene, [&](obs_sceneitem_t *item, obs_sceneitem_t *group) {
			if (!obs_sceneitem_locked(item) && !(group && obs_sceneitem_locked(group)) &&
			    point_in_item(item, group, canvas)) {
				// Referenced here, inside the scene's enumeration; the
				// one it replaces (lower down) let go.
				obs_sceneitem_addref(item);
				if (group)
					obs_sceneitem_addref(group);
				release_hit(hit);
				hit = {item, group};
			}
		});
	}
	obs_source_release(scene_source);
	return hit;
}

// OBS's selection set to just the text being edited.
void PreviewEditor::select_only(obs_sceneitem_t *target)
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

bool PreviewEditor::mouse_event(QMouseEvent *event)
{
	const QEvent::Type type = event->type();
	const QPointF pos = event->position();

	// Whether what's under the pointer was selected before this click: OBS
	// then selects what's below it instead (clicks cycle down through
	// overlapping items), so a double-click on selected text leaves
	// something else selected by the time it arrives.
	if (type == QEvent::MouseButtonPress && event->button() == Qt::LeftButton && !editing()) {
		Hit hit = item_at(pos);
		pressed_item_ = hit.item;
		pressed_selected_ = hit.item && obs_sceneitem_selected(hit.item);
		release_hit(hit);
		return false;
	}

	if (type == QEvent::MouseButtonDblClick && event->button() == Qt::LeftButton && !editing()) {
		// Start editing the text item double-clicked on, if it's on top
		// and not locked (OBS's own clicks skip locked items too).
		Hit hit = item_at(pos);
		struct Release {
			Hit &h;
			~Release() { release_hit(h); }
		} release{hit};
		if (!hit.item)
			return false;

		if (!TextSource::from(obs_sceneitem_get_source(hit.item)))
			return false;
		if (!scaled_to_window(preview_)) {
			blog(LOG_INFO, "[obs-typeset] the preview isn't set to Scale to Window, so text isn't edited "
				       "in it (right-click the preview: Preview Scaling, Scale to Window)");
			return false;
		}
		// OBS selected what it found under the first click (or it was
		// selected already): if that isn't this, the preview isn't laid
		// out as assumed.
		const bool was_selected = hit.item == pressed_item_ && pressed_selected_;
		if (!was_selected && !obs_sceneitem_selected(hit.item) &&
		    !(hit.group && obs_sceneitem_selected(hit.group))) {
			blog(LOG_INFO, "[obs-typeset] the text double-clicked isn't the one OBS selected; not editing");
			return false;
		}
		if (!begin_edit(hit))
			return false;
		select_only(hit.item);
		const vec3 local = to_source(pos);
		const auto w = text_->word_at(text_->hit_test(local.x, local.y, false));
		anchor_ = w.first;
		caret_ = w.second;
		drag_by_ = Unit::Word;
		drag_unit_ = w;
		dragging_ = true;
		swallow_release_ = true;
		last_double_click_ = os_gettime_ns();
		last_double_click_pos_ = pos;
		refresh_overlay();
		return true;
	}

	if (!editing())
		return false;

	bool inside = false;
	const vec3 local = to_source(pos, &inside);

	// The format bar.
	if (type == QEvent::MouseButtonPress && event->button() == Qt::LeftButton) {
		const QPointF d = pos * preview_mapping(preview_).dpr;
		for (const BarButton &b : bar_) {
			if (b.action == BarAction::SizeReadout)
				continue;
			if (d.x() >= b.x && d.x() <= b.x + b.w && d.y() >= b.y && d.y() <= b.y + b.h) {
				swallow_release_ = true;
				bar_action(b.action);
				return true;
			}
		}
	}

	// The text box's width grip.
	if (resizing_) {
		if (type == QEvent::MouseMove) {
			// How far the pointer has moved along the text's own x axis, in
			// its own units (not through its transform, which moves as the
			// box grows when the item isn't anchored at its left).
			const Mapping map = preview_mapping(preview_);
			matrix4 m;
			item_to_canvas(item_, group_, &m);
			const double ax = m.x.x, ay = m.x.y, len2 = ax * ax + ay * ay;
			const double dx = (pos.x() - resize_from_pos_.x()) * map.dpr / map.scale;
			const double dy = (pos.y() - resize_from_pos_.y()) * map.dpr / map.scale;
			const double along = len2 > 0 ? (dx * ax + dy * ay) / len2 : 0;
			text_->set_box_width(int(std::lround(resize_from_width_ + along)));
			refresh_overlay();
			return true;
		}
		if (type == QEvent::MouseButtonRelease && event->button() == Qt::LeftButton) {
			resizing_ = false;
			return true;
		}
	}
	if (type == QEvent::MouseButtonPress && event->button() == Qt::LeftButton && on_grip(pos)) {
		resizing_ = true;
		resize_from_pos_ = pos;
		resize_from_width_ = text_->document().style.box_width;
		return true;
	}
	if (type == QEvent::MouseMove && !dragging_ && on_grip(pos)) {
		preview_->setCursor(Qt::SizeHorCursor);
		return true;
	}

	switch (type) {
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonDblClick: {
		if (event->button() != Qt::LeftButton)
			return false;
		if (!inside) {
			end_edit();
			return false; // and OBS handles the click as usual
		}
		sync_text();
		constexpr int triple_click_slop = 6; // pixels the pointer may move between clicks
		const bool triple = type == QEvent::MouseButtonPress &&
				    os_gettime_ns() - last_double_click_ <
					    uint64_t(QApplication::doubleClickInterval()) * 1000000ULL &&
				    (pos - last_double_click_pos_).manhattanLength() < triple_click_slop;
		if (type == QEvent::MouseButtonDblClick) {
			drag_by_ = Unit::Word;
			drag_unit_ = text_->word_at(text_->hit_test(local.x, local.y, false));
			anchor_ = drag_unit_.first;
			caret_ = drag_unit_.second;
			last_double_click_ = os_gettime_ns();
			last_double_click_pos_ = pos;
		} else if (triple) {
			drag_by_ = Unit::Paragraph;
			drag_unit_ = text_->paragraph_at(text_->hit_test(local.x, local.y, false));
			anchor_ = drag_unit_.first;
			caret_ = drag_unit_.second;
		} else {
			drag_by_ = Unit::Grapheme;
			set_caret(text_->hit_test(local.x, local.y), event->modifiers() & Qt::ShiftModifier);
		}
		column_x_ = -1;
		last_edit_ = EditKind::None;
		dragging_ = true;
		swallow_release_ = true;
		refresh_overlay();
		return true;
	}
	case QEvent::MouseMove:
		if (dragging_) {
			const size_t at = text_->hit_test(local.x, local.y, drag_by_ == Unit::Grapheme);
			if (drag_by_ == Unit::Grapheme) {
				caret_ = at;
			} else {
				const auto unit = drag_by_ == Unit::Word ? text_->word_at(at) : text_->paragraph_at(at);
				if (unit.first < drag_unit_.first) {
					anchor_ = drag_unit_.second;
					caret_ = unit.first;
				} else {
					anchor_ = drag_unit_.first;
					caret_ = std::max(unit.second, drag_unit_.second);
				}
			}
			refresh_overlay();
			return true;
		}
		if (inside) {
			preview_->setCursor(Qt::IBeamCursor);
			return true;
		}
		preview_->unsetCursor();
		return false;
	case QEvent::MouseButtonRelease:
		if (swallow_release_ && event->button() == Qt::LeftButton) {
			swallow_release_ = false;
			dragging_ = false;
			return true;
		}
		return false;
	default:
		return false;
	}
}

void PreviewEditor::input_method(QInputMethodEvent *event)
{
	sync_text();
	// What's being composed (CJK, dead keys) is shown beside the caret in
	// the preview until it's committed.
	preedit_ = event->preeditString().toStdString();
	const std::string commit = event->commitString().toStdString();
	if (event->replacementLength() > 0 && !has_selection()) {
		// The input method replaces text round the caret (counted in
		// UTF-16 units of the text before and after it).
		const QString before = QString::fromStdString(doc_.substr(0, caret_));
		const QString after = QString::fromStdString(doc_.substr(caret_));
		const int start = event->replacementStart(), length = event->replacementLength();
		const int from = std::clamp(int(before.size()) + start, 0, int(before.size() + after.size()));
		const int to = std::clamp(from + length, from, int(before.size() + after.size()));
		const QString all = before + after;
		anchor_ = size_t(all.left(from).toUtf8().size());
		caret_ = size_t(all.left(to).toUtf8().size());
	}
	// Only a commit (or a replacement the input method asked for) changes
	// the text; a composition starting over a selection leaves it alone.
	if (!commit.empty() || event->replacementLength() > 0)
		replace_selection(commit, EditKind::Typing);
	refresh_overlay();
}

QVariant PreviewEditor::input_method_query(Qt::InputMethodQuery query)
{
	switch (query) {
	case Qt::ImEnabled:
		return true;
	case Qt::ImCursorRectangle: {
		const Caret c = text_->caret(caret_);
		matrix4 m;
		item_to_canvas(item_, group_, &m);
		const Mapping map = preview_mapping(preview_);
		auto to_widget = [&](double x, double y) {
			vec3 p;
			vec3_set(&p, float(x), float(y), 0.0f);
			vec3_transform(&p, &p, &m);
			return QPointF((map.x + p.x * map.scale) / map.dpr, (map.y + p.y * map.scale) / map.dpr);
		};
		return QRectF(to_widget(c.x, c.y), to_widget(c.x, c.y + c.height)).normalized().toAlignedRect();
	}
	case Qt::ImSurroundingText:
		return QString::fromStdString(doc_);
	case Qt::ImCursorPosition:
		return int(QString::fromStdString(doc_.substr(0, caret_)).size());
	case Qt::ImAnchorPosition:
		return int(QString::fromStdString(doc_.substr(0, anchor_)).size());
	case Qt::ImCurrentSelection:
		return QString::fromStdString(doc_.substr(sel_from(), sel_to() - sel_from()));
	default:
		return QVariant();
	}
}

bool PreviewEditor::eventFilter(QObject *obj, QEvent *event)
{
	// The preview's display is about to go (its native window is being
	// destroyed): stop drawing on it first. This filter runs before OBS's.
	if (obj == window_ && event->type() == QEvent::PlatformSurface &&
	    static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
		    QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
		detach_display();
		return false;
	}
	// Hidden (minimised, or the preview turned off), OBS destroys the
	// display on Linux in its own handler, which may run before or after
	// this: it's forgotten without being touched, and found again when
	// the preview is back (on_timer).
	if (obj == window_ && event->type() == QEvent::Hide) {
		forget_display();
		return false;
	}
	if (obj != preview_)
		return false;

	switch (event->type()) {
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonRelease:
	case QEvent::MouseButtonDblClick:
	case QEvent::MouseMove:
		return mouse_event(static_cast<QMouseEvent *>(event));
	case QEvent::Resize:
		if (!editing())
			refresh_placeholders();
		break;
	case QEvent::WinIdChange:
		detach_display();
		QTimer::singleShot(0, this, [this] {
			watch_window();
			attach_display();
		});
		return false;
	case QEvent::KeyPress:
		if (!editing() && static_cast<QKeyEvent *>(event)->key() == Qt::Key_F2 &&
		    static_cast<QKeyEvent *>(event)->modifiers() == Qt::NoModifier)
			return edit_selected();
		break;
	default:
		break;
	}
	if (!editing())
		return false;

	switch (event->type()) {
	case QEvent::ShortcutOverride:
		// Every key goes to the text while editing, not to OBS's
		// shortcuts (Delete would remove the source, Ctrl+C copy it).
		event->accept();
		return true;
	case QEvent::KeyPress:
		return key_press(static_cast<QKeyEvent *>(event));
	case QEvent::KeyRelease:
		return true;
	case QEvent::InputMethod:
		input_method(static_cast<QInputMethodEvent *>(event));
		return true;
	case QEvent::InputMethodQuery: {
		auto *query = static_cast<QInputMethodQueryEvent *>(event);
		for (unsigned bit = 0; bit < 31; bit++) {
			const auto q = Qt::InputMethodQuery(1u << bit);
			if (query->queries() & q)
				query->setValue(q, input_method_query(q));
		}
		query->accept();
		return true;
	}
	case QEvent::FocusOut: {
		// Switching windows or opening a menu keeps the edit going.
		const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
		if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason)
			end_edit();
		return false;
	}
	case QEvent::Resize:
		refresh_overlay();
		return false;
	case QEvent::Hide:
		end_edit();
		return false;
	default:
		return false;
	}
}

// --- Drawing on the preview -------------------------------------------------------------------

void PreviewEditor::attach_display()
{
	// The display's own lock is held while it draws, and drawing takes the
	// overlay's: so the display is never added to or removed from with the
	// overlay's lock held.
	obs_display_t *display = preview_ ? preview_display(preview_) : nullptr;
	obs_display_t *previous;
	{
		std::lock_guard lock(overlay_mutex_);
		previous = display_;
		display_ = display;
	}
	if (previous == display)
		return;
	if (previous)
		obs_display_remove_draw_callback(previous, draw, this);
	if (display && display == forgotten_) {
		// The same display as before the preview hid: OBS kept it, and
		// it still has this callback.
		forgotten_ = nullptr;
	} else if (display) {
		forgotten_ = nullptr;
		obs_display_add_draw_callback(display, draw, this);
	} else if (!warned_display_) {
		warned_display_ = true;
		blog(LOG_WARNING, "[obs-typeset] couldn't find the preview's display: text can be edited in "
				  "the preview, but without a caret");
	}
}

void PreviewEditor::forget_display()
{
	std::lock_guard lock(overlay_mutex_);
	if (display_)
		forgotten_ = display_;
	display_ = nullptr;
}

void PreviewEditor::detach_display()
{
	obs_display_t *display;
	{
		std::lock_guard lock(overlay_mutex_);
		display = display_;
		display_ = nullptr;
		// A display let go when the preview hid but which OBS kept (it's
		// the preview's display again): it still has the callback.
		if (!display && forgotten_ && preview_ && preview_display(preview_) == forgotten_)
			display = forgotten_;
		forgotten_ = nullptr;
	}
	// Outside the lock: removing waits for a draw in progress, which takes it.
	if (display)
		obs_display_remove_draw_callback(display, draw, this);
}

void PreviewEditor::refresh_overlay()
{
	if (!editing())
		return;
	Overlay o;
	o.editing = true;
	o.target = {item_, group_};
	o.mapping = preview_mapping(preview_);
	o.caret = text_->caret(caret_);
	o.show_caret = !has_selection();
	o.selection = text_->selection_boxes(sel_from(), sel_to());
	o.box = text_->box();
	const Document doc = text_->document();
	o.grip = doc.style.box && doc.style.box_width > 0;
	o.plate = doc.style.plate;
	o.tag = std::string(obs_module_text("Editing")) + " \u201c" + obs_source_get_name(source_) + "\u201d";
	const Frame frame = text_->frame();
	if (frame.text_scale < 0.995) {
		char shrunk[64];
		snprintf(shrunk, sizeof(shrunk), " \u00b7 %s %d%%", obs_module_text("ShrunkTo"),
			 int(std::lround(frame.text_scale * 100)));
		o.tag += shrunk;
	}
	o.tag += std::string(" \u00b7 ") + obs_module_text("EscToFinish");

	// One strip above the frame's left end (below it when there's no room
	// above): the format bar, showing what's on for the selection or what's
	// typed next, then the tag. Clear of OBS's distance labels, which sit
	// over the middle of the box's edges.
	{
		matrix4 m;
		item_to_canvas(item_, group_, &m);
		const Box b = o.box;
		float top = 1e9f, bottom = -1e9f, left = 1e9f;
		for (int i = 0; i < 4; i++) {
			vec3 p;
			vec3_set(&p, float(i == 1 || i == 2 ? b.w : 0), float(i >= 2 ? b.h : 0), 0.0f);
			vec3_transform(&p, &p, &m);
			const float x = float(o.mapping.x + p.x * o.mapping.scale), y = float(o.mapping.y + p.y * o.mapping.scale);
			top = std::min(top, y);
			bottom = std::max(bottom, y);
			left = std::min(left, x);
		}
		const float px = float(std::max(1.0, std::round(o.mapping.dpr)));
		const float size = 28 * px, gap = 2 * px, margin = 8 * px;
		// Kept within the preview: above if there's room, else below, else
		// at the preview's top.
		const float view_w = float(preview_->width() * o.mapping.dpr), view_h = float(preview_->height() * o.mapping.dpr);
		const float strip_w = 28 * px * 8.6f + 300 * px; // the buttons and room for the tag
		float y = top - margin - size - 6 * px;
		if (y < 2)
			y = bottom + margin + 6 * px;
		if (y + size > view_h - 2)
			y = 2;
		float x = std::clamp(left - margin, 2.0f, std::max(2.0f, view_w - strip_w));
		auto on = [&](StyleChange::Field f) {
			for (const StyleChange &c : pending_)
				if (c.field == f)
					return c.value > 0;
			return has_selection() ? all_on(spans_, sel_from(), sel_to(), f)
					       : caret_ > 0 && all_on(spans_, caret_ - 1, caret_, f);
		};
		const Span here = style_at(spans_, has_selection() ? sel_from() : caret_ > 0 ? caret_ - 1 : 0);
		const BarAction actions[] = {BarAction::Bold,   BarAction::Italic,      BarAction::Underline,
					     BarAction::Smaller, BarAction::SizeReadout, BarAction::Bigger,
					     BarAction::Colour,  BarAction::Clear};
		for (BarAction a : actions) {
			BarButton btn{a, x, y, a == BarAction::SizeReadout ? size * 1.6f : size, size, false, 0, {}};
			btn.active = (a == BarAction::Bold && on(StyleChange::Bold)) ||
				     (a == BarAction::Italic && on(StyleChange::Italic)) ||
				     (a == BarAction::Underline && on(StyleChange::Underline));
			if (a == BarAction::Colour)
				btn.swatch = here.has_colour ? here.colour : doc.style.colour;
			if (a == BarAction::SizeReadout)
				btn.text = std::to_string(here.size ? here.size : 100) + "%";
			o.bar.push_back(btn);
			x += btn.w + gap + (a == BarAction::Underline || a == BarAction::Bigger ? 2 * gap : 0);
		}
		o.tag_x = x + 4 * px;
		o.strip_y = y;
		o.strip_h = size;
	}
	bar_ = o.bar;
	o.preedit = preedit_;
	o.blink_from = os_gettime_ns();
	shown_ = text_->document();

	obs_sceneitem_addref(o.target.item);
	if (o.target.group)
		obs_sceneitem_addref(o.target.group);
	Target old;
	{
		std::lock_guard lock(overlay_mutex_);
		old = overlay_.target;
		o.placeholders.swap(overlay_.placeholders);
		overlay_ = std::move(o);
	}
	release(old);
}

void PreviewEditor::draw(void *param, uint32_t cx, uint32_t cy)
{
	static_cast<PreviewEditor *>(param)->draw_overlay(cx, cy);
}

void fill(const std::vector<Quad> &quads, uint32_t rgba)
{
	if (quads.empty())
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colour = gs_effect_get_param_by_name(solid, "color");
	vec4 c;
	vec4_set(&c, float(rgba >> 24) / 255.0f, float((rgba >> 16) & 0xFF) / 255.0f, float((rgba >> 8) & 0xFF) / 255.0f,
		 float(rgba & 0xFF) / 255.0f);
	gs_effect_set_vec4(colour, &c);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		for (const Quad &q : quads) {
			gs_vertex2f(q.p[0].x, q.p[0].y);
			gs_vertex2f(q.p[1].x, q.p[1].y);
			gs_vertex2f(q.p[2].x, q.p[2].y);
			gs_vertex2f(q.p[0].x, q.p[0].y);
			gs_vertex2f(q.p[2].x, q.p[2].y);
			gs_vertex2f(q.p[3].x, q.p[3].y);
		}
		gs_render_stop(GS_TRIS);
	}
}

// A line from a to b, w display pixels wide.
Quad line_quad(const vec2 &a, const vec2 &b, float w)
{
	vec2 d;
	vec2_sub(&d, &b, &a);
	const float len = std::max(vec2_len(&d), 1e-3f);
	vec2 n;
	vec2_set(&n, -d.y / len * w * 0.5f, d.x / len * w * 0.5f);
	Quad q;
	vec2_add(&q.p[0], &a, &n);
	vec2_add(&q.p[1], &b, &n);
	vec2_sub(&q.p[2], &b, &n);
	vec2_sub(&q.p[3], &a, &n);
	return q;
}

// The outline of a quad, as lines w wide; dashed with dash > 0.
void outline(std::vector<Quad> &out, const Quad &q, float w, float dash = 0)
{
	for (int i = 0; i < 4; i++) {
		const vec2 &a = q.p[i], &b = q.p[(i + 1) % 4];
		if (dash <= 0) {
			out.push_back(line_quad(a, b, w));
			continue;
		}
		vec2 d;
		vec2_sub(&d, &b, &a);
		const float len = vec2_len(&d);
		for (float t = 0; t < len; t += 2 * dash) {
			vec2 s, e;
			vec2_mulf(&s, &d, t / len);
			vec2_add(&s, &s, &a);
			vec2_mulf(&e, &d, std::min(t + dash, len) / len);
			vec2_add(&e, &e, &a);
			out.push_back(line_quad(s, e, w));
		}
	}
}

// Grown outwards by d display pixels (for a quad whose corners go round
// clockwise or anticlockwise; its centre is used).
Quad grown(const Quad &q, float d)
{
	vec2 c;
	vec2_set(&c, (q.p[0].x + q.p[2].x) / 2, (q.p[0].y + q.p[2].y) / 2);
	Quad out;
	for (int i = 0; i < 4; i++) {
		vec2 v;
		vec2_sub(&v, &q.p[i], &c);
		const float len = std::max(vec2_len(&v), 1e-3f);
		vec2_mulf(&v, &v, (len + d * 1.41421f) / len);
		vec2_add(&out.p[i], &c, &v);
	}
	return out;
}

void PreviewEditor::draw_overlay(uint32_t cx, uint32_t cy)
{
	std::lock_guard lock(overlay_mutex_);
	if (!overlay_.editing && overlay_.placeholders.empty())
		return;
	const Overlay &o = overlay_;
	const Mapping map = o.mapping;
	const float px = float(std::max(1.0, std::round(map.dpr)));

	gs_matrix_push();
	gs_matrix_identity();
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	auto projector = [&](const Target &t) {
		matrix4 m;
		item_to_canvas(t.item, t.group, &m);
		return [m, map](double x, double y) {
			vec3 p;
			vec3_set(&p, float(x), float(y), 0.0f);
			vec3_transform(&p, &p, &m);
			vec2 out;
			vec2_set(&out, float(map.x + p.x * map.scale), float(map.y + p.y * map.scale));
			return out;
		};
	};

	// Empty texts: a dashed frame and what to do, so they can be found.
	if (!o.placeholders.empty()) {
		if (!placeholder_label_.texture)
			placeholder_label_.make(obs_module_text("Placeholder"), 13 * px, 0xFFFFFFFF, 0x000000A0);
		std::vector<Quad> lines;
		for (const Target &t : o.placeholders) {
			auto to_display = projector(t);
			obs_source_t *s = obs_sceneitem_get_source(t.item);
			const double w = obs_source_get_width(s), h = obs_source_get_height(s);
			const Quad q{{to_display(0, 0), to_display(w, 0), to_display(w, h), to_display(0, h)}};
			outline(lines, q, px, 4 * px);
			const float lx = (q.p[0].x + q.p[2].x) / 2 - placeholder_label_.width / 2.0f;
			const float ly = (q.p[0].y + q.p[2].y) / 2 - placeholder_label_.height / 2.0f;
			placeholder_label_.draw(lx, ly);
		}
		fill(lines, placeholder_line);
	}

	if (o.editing && o.target.item) {
		auto to_display = projector(o.target);
		auto box_quad = [&](const Box &b) {
			return Quad{{to_display(b.x, b.y), to_display(b.x + b.w, b.y), to_display(b.x + b.w, b.y + b.h),
				     to_display(b.x, b.y + b.h)}};
		};

		// A frame of its own, clear of OBS's red box, blue edged in dark so
		// it shows on any background, and a tag saying what's going on.
		const Quad frame = grown(box_quad(o.box), 8 * px);

		// The rest of the canvas a little dimmer while editing (OBS's
		// guides with it), so it's plain which text has the keyboard.
		{
			float top = 1e9f, left = 1e9f, bottom = -1e9f, right = -1e9f;
			for (const vec2 &p : frame.p) {
				top = std::min(top, p.y);
				left = std::min(left, p.x);
				bottom = std::max(bottom, p.y);
				right = std::max(right, p.x);
			}
			obs_video_info ovi = {};
			obs_get_video_info(&ovi);
			const float cx0 = float(map.x), cy0 = float(map.y);
			const float cx1 = float(map.x + ovi.base_width * map.scale), cy1 = float(map.y + ovi.base_height * map.scale);
			auto rect = [](float x0, float y0, float x1, float y1) {
				Quad q;
				vec2_set(&q.p[0], x0, y0);
				vec2_set(&q.p[1], x1, y0);
				vec2_set(&q.p[2], x1, y1);
				vec2_set(&q.p[3], x0, y1);
				return q;
			};
			top = std::clamp(top, cy0, cy1);
			bottom = std::clamp(bottom, cy0, cy1);
			left = std::clamp(left, cx0, cx1);
			right = std::clamp(right, cx0, cx1);
			fill({rect(cx0, cy0, cx1, top), rect(cx0, bottom, cx1, cy1), rect(cx0, top, left, bottom),
			      rect(right, top, cx1, bottom)},
			     veil);
		}

		std::vector<Quad> edges, inner;
		outline(edges, frame, 6 * px);
		outline(inner, frame, 3 * px);
		fill(edges, accent_edge);
		fill(inner, accent);
		if (!editing_label_.texture || editing_label_text_ != o.tag) {
			editing_label_.destroy();
			editing_label_.make(o.tag.c_str(), 14 * px, 0xFFFFFFFF, tag_fill);
			editing_label_text_ = o.tag;
		}
		// After the format bar, in the same strip.
		editing_label_.draw(o.tag_x, o.strip_y + (o.strip_h - editing_label_.height) / 2);

		// The format bar.
		if (!o.bar.empty()) {
			std::vector<Quad> fills, actives, swatch, swatch_edge;
			for (const BarButton &b : o.bar) {
				Quad q;
				vec2_set(&q.p[0], b.x, b.y);
				vec2_set(&q.p[1], b.x + b.w, b.y);
				vec2_set(&q.p[2], b.x + b.w, b.y + b.h);
				vec2_set(&q.p[3], b.x, b.y + b.h);
				(b.active ? actives : fills).push_back(q);
				if (b.action == BarAction::Colour) {
					// An "A" over a bar of the colour, as in the slide tools.
					Quad bar;
					vec2_set(&bar.p[0], b.x + 6 * px, b.y + b.h - 8 * px);
					vec2_set(&bar.p[1], b.x + b.w - 6 * px, b.y + b.h - 8 * px);
					vec2_set(&bar.p[2], b.x + b.w - 6 * px, b.y + b.h - 4 * px);
					vec2_set(&bar.p[3], b.x + 6 * px, b.y + b.h - 4 * px);
					swatch_edge.push_back(grown(bar, 1 * px));
					swatch.push_back(bar);
				}
			}
			fill(fills, tag_fill);
			fill(actives, accent);
			fill(swatch_edge, 0xFFFFFF90);
			for (const BarButton &b : o.bar) {
				if (b.action == BarAction::Colour) {
					const uint32_t c = b.swatch;
					fill(swatch, (c & 0xFF) << 24 | ((c >> 8) & 0xFF) << 16 | ((c >> 16) & 0xFF) << 8 | 0xFF);
				}
				if (b.action == BarAction::SizeReadout) {
					if (!size_label_.texture || size_label_text_ != b.text) {
						size_label_.destroy();
						size_label_.make(b.text.c_str(), 12 * px, 0xFFFFFFFF, 0);
						size_label_text_ = b.text;
					}
					size_label_.draw(b.x + (b.w - size_label_.width) / 2, b.y + (b.h - size_label_.height) / 2);
					continue;
				}
				Label &l = bar_labels_[int(b.action)];
				if (!l.texture)
					l.make(bar_glyph(b.action), 15 * px, 0xFFFFFFFF, 0);
				const float lift = b.action == BarAction::Colour ? 3 * px : 0;
				l.draw(b.x + (b.w - l.width) / 2, b.y + (b.h - l.height) / 2 - lift);
			}
		}

		// The text box's width grip, on the frame's right edge.
		if (o.grip) {
			const vec2 a = to_display(o.box.w, o.box.h * 0.5);
			vec2 g0, g1;
			vec2_set(&g0, a.x + 7 * px, a.y - 12 * px);
			vec2_set(&g1, a.x + 7 * px, a.y + 12 * px);
			fill({line_quad(g0, g1, 9 * px)}, accent_edge);
			fill({line_quad(g0, g1, 6 * px)}, 0xFFFFFFFF);
			fill({line_quad(g0, g1, 3 * px)}, accent);
		}

		// The selection: a solid highlight with the selected text drawn
		// again on top, clipped to it, so it keeps its own colours. A
		// rotated source can't be clipped to a box, and on a background
		// plate the plate would be drawn again over the highlight: those
		// get a wash instead.
		matrix4 m;
		item_to_canvas(o.target.item, o.target.group, &m);
		const bool upright = !o.plate && std::fabs(m.x.y) < 1e-4f && std::fabs(m.y.x) < 1e-4f && m.x.x > 0 &&
				     m.y.y > 0;
		std::vector<Quad> selection;
		for (const Box &b : o.selection)
			selection.push_back(box_quad(b));
		fill(selection, upright ? selection_fill : selection_wash);
		// A keyline round the selection as one shape: the sides of each
		// line's piece, the top of the first line and the bottom of the last.
		std::vector<Quad> keylines;
		float sel_top = 1e9f, sel_bottom = -1e9f;
		for (const Quad &q : selection) {
			sel_top = std::min(sel_top, std::min(q.p[0].y, q.p[2].y));
			sel_bottom = std::max(sel_bottom, std::max(q.p[0].y, q.p[2].y));
		}
		for (const Quad &q : selection) {
			if (!upright) {
				outline(keylines, q, 1 * px);
				continue;
			}
			keylines.push_back(line_quad(q.p[1], q.p[2], 1 * px));
			keylines.push_back(line_quad(q.p[3], q.p[0], 1 * px));
			if (std::fabs(q.p[0].y - sel_top) < 0.5f)
				keylines.push_back(line_quad(q.p[0], q.p[1], 1 * px));
			if (std::fabs(q.p[2].y - sel_bottom) < 0.5f)
				keylines.push_back(line_quad(q.p[2], q.p[3], 1 * px));
		}
		fill(keylines, selection_keyline);
		if (upright && !selection.empty()) {
			TextSource *text = TextSource::from(obs_sceneitem_get_source(o.target.item));
			matrix4 to_display_m;
			matrix4_identity(&to_display_m);
			to_display_m.x.x = float(map.scale);
			to_display_m.y.y = float(map.scale);
			to_display_m.t.x = float(map.x);
			to_display_m.t.y = float(map.y);
			matrix4_mul(&to_display_m, &m, &to_display_m);
			const bool flip = gs_get_device_type() == GS_DEVICE_OPENGL; // GL's scissor counts from the bottom
			for (const Quad &q : selection) {
				const int x0 = int(std::floor(std::min(q.p[0].x, q.p[2].x)));
				const int x1 = int(std::ceil(std::max(q.p[0].x, q.p[2].x)));
				const int y0 = int(std::floor(std::min(q.p[0].y, q.p[2].y)));
				const int y1 = int(std::ceil(std::max(q.p[0].y, q.p[2].y)));
				gs_rect r = {x0, flip ? int(cy) - y1 : y0, x1 - x0, y1 - y0};
				if (r.cx <= 0 || r.cy <= 0)
					continue;
				gs_set_scissor_rect(&r);
				gs_matrix_push();
				gs_matrix_set(&to_display_m);
				const bool drawn = text && text->draw_raster();
				gs_matrix_pop();
				if (!drawn) { // no raster of the text as it is: a wash over it instead
					gs_set_scissor_rect(nullptr);
					fill({q}, selection_wash);
				}
			}
			gs_set_scissor_rect(nullptr);
		}
		(void)cx;

		// A composition in progress, just above the caret.
		if (!o.preedit.empty()) {
			if (!preedit_label_.texture || preedit_label_text_ != o.preedit) {
				preedit_label_.destroy();
				preedit_label_.make(o.preedit.c_str(), 16 * px, 0xFFFFFFFF, accent);
				preedit_label_text_ = o.preedit;
			}
			const vec2 a = to_display(o.caret.x, o.caret.y);
			preedit_label_.draw(a.x, a.y - preedit_label_.height - 2 * px);
		}

		// The caret blinks, and stays solid for a moment after each key.
		const uint64_t since = (os_gettime_ns() - o.blink_from) / 1000000ULL;
		if (o.show_caret && (since < caret_hold_ms || (since / caret_blink_ms) % 2 == 0)) {
			const vec2 a = to_display(o.caret.x, o.caret.y);
			const vec2 b = to_display(o.caret.x, o.caret.y + o.caret.height);
			// Blue in white in dark: never mistaken for a letter's stem.
			fill({line_quad(a, b, 7.0f * px)}, 0x000000B0);
			fill({line_quad(a, b, 5.0f * px)}, 0xFFFFFFFF);
			fill({line_quad(a, b, 3.0f * px)}, accent);
		}
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
	gs_matrix_pop();
}

} // namespace

void editor_install()
{
	if (editor)
		return;
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QWidget *preview = main ? main->findChild<QWidget *>("preview") : nullptr;
	if (!preview) {
		blog(LOG_WARNING, "[obs-typeset] couldn't find OBS's preview: text can't be edited in it");
		return;
	}
	editor = new PreviewEditor(preview);
}

void editor_remove()
{
	delete editor;
	editor = nullptr;
}

void editor_scene_changed()
{
	if (editor)
		editor->scene_changed();
}

} // namespace typeset
