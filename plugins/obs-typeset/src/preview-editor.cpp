#include "preview-editor.hpp"

#include "typeset-source.hpp"

#include <graphics/matrix4.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace typeset {

namespace {

// --- Where the canvas is in OBS's preview ---------------------------------------------
//
// OBS draws the canvas into its preview widget scaled to fit, centred, inside
// a 10-pixel margin ("Scale to window", the default). This is OBS's own
// calculation (OBSBasic::ResizePreview and GetScaleAndCenterPos, the same from
// OBS 30 to 33). With the preview zoomed or set to a fixed scale it doesn't
// hold; a double-click is then checked against the item OBS itself selected
// and editing isn't started if the two disagree.

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

// The preview's obs_display_t, to draw the caret on. OBS doesn't offer it to
// plugins; the preview widget is an OBSQTDisplay, whose first member is an
// OBSDisplay (one obs_display_t pointer) straight after its QWidget base. That
// has held from OBS 27 to 33; it's checked against the display's size before
// it's used, and without it text can still be edited, only with no caret.
obs_display_t *preview_display(QWidget *preview)
{
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

// --- Finding text in a scene ----------------------------------------------------------

// A scene item's transform from its source's own units to the canvas,
// through the group it's in, if any.
void item_to_canvas(obs_sceneitem_t *item, obs_sceneitem_t *group, matrix4 *out)
{
	obs_sceneitem_get_draw_transform(item, out);
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

struct HitSearch {
	vec2 pos;
	obs_sceneitem_t *group = nullptr;
	Hit top; // the last hit in drawing order is the one on top
};

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
	return p.x >= 0.0f && p.y >= 0.0f && p.x <= float(cx) && p.y <= float(cy);
}

bool find_hit(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *search = static_cast<HitSearch *>(param);
	if (!obs_sceneitem_visible(item))
		return true;
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_t *outer = search->group;
		search->group = item;
		obs_sceneitem_group_enum_items(item, find_hit, search);
		search->group = outer;
		return true;
	}
	if (point_in_item(item, search->group, search->pos))
		search->top = {item, search->group};
	return true;
}

bool find_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *target = static_cast<std::pair<obs_sceneitem_t *, bool> *>(param);
	if (item == target->first) {
		target->second = true;
		return false;
	}
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, find_item, param);
	return !target->second;
}

// --- The editor --------------------------------------------------------------------------

struct Snapshot {
	std::string text;
	size_t caret = 0, anchor = 0;
};

enum class EditKind { None, Typing, Deleting, Other };

class PreviewEditor : public QObject {
public:
	explicit PreviewEditor(QWidget *preview);
	~PreviewEditor() override;

	bool eventFilter(QObject *obj, QEvent *event) override;
	void end_edit();

private:
	QPointer<QWidget> preview_;
	QPointer<QWindow> window_;
	QTimer check_timer_;

	// The edit under way.
	obs_sceneitem_t *item_ = nullptr; // referenced
	obs_sceneitem_t *group_ = nullptr;
	obs_source_t *source_ = nullptr; // referenced
	TextSource *text_ = nullptr;
	std::string doc_;
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

	// What the preview's draw callback draws, guarded by its lock.
	std::mutex overlay_mutex_;
	obs_display_t *display_ = nullptr;
	struct Overlay {
		bool active = false;
		obs_sceneitem_t *item = nullptr, *group = nullptr;
		Mapping mapping;
		Caret caret;
		bool show_caret = true;
		std::vector<Box> selection;
		Box box;
		uint64_t blink_from = 0;
	} overlay_;

	bool editing() const { return item_ != nullptr; }
	bool begin_edit(const Hit &hit);
	bool still_editable();
	vec3 to_source(QPointF widget_pos, bool *inside = nullptr);

	void sync_text();
	void set_caret(size_t caret, bool extend);
	void replace(size_t from, size_t to, const std::string &with, EditKind kind);
	void insert(const std::string &s, EditKind kind = EditKind::Typing) { replace_selection(s, kind); }
	void replace_selection(const std::string &s, EditKind kind);
	bool has_selection() const { return caret_ != anchor_; }
	size_t sel_from() const { return std::min(caret_, anchor_); }
	size_t sel_to() const { return std::max(caret_, anchor_); }
	void undo();
	void redo();

	bool key_press(QKeyEvent *event);
	bool mouse_event(QMouseEvent *event);
	void input_method(QInputMethodEvent *event);
	QVariant input_method_query(Qt::InputMethodQuery query);

	void attach_display();
	void detach_display();
	void refresh_overlay();
	static void draw(void *param, uint32_t cx, uint32_t cy);
	void draw_overlay();
};

PreviewEditor *editor = nullptr;

PreviewEditor::PreviewEditor(QWidget *preview) : preview_(preview)
{
	preview->installEventFilter(this);
	if (QWindow *w = preview->windowHandle()) {
		window_ = w;
		w->installEventFilter(this);
	}
	check_timer_.setInterval(250);
	QObject::connect(&check_timer_, &QTimer::timeout, this, [this] {
		if (editing() && !still_editable())
			end_edit();
	});
}

PreviewEditor::~PreviewEditor()
{
	end_edit();
	if (preview_)
		preview_->removeEventFilter(this);
	if (window_)
		window_->removeEventFilter(this);
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
	if (!preview_ || !preview_->isVisible() || !obs_sceneitem_visible(item_))
		return false;
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	std::pair<obs_sceneitem_t *, bool> target{item_, false};
	if (scene)
		obs_scene_enum_items(scene, find_item, &target);
	obs_source_release(scene_source);
	return target.second;
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
	doc_ = text_->text();
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
	preview_->setCursor(Qt::IBeamCursor);
	check_timer_.start();

	attach_display();
	blog(LOG_INFO, "[obs-typeset] editing '%s' in the preview", obs_source_get_name(source));
	return true;
}

void PreviewEditor::end_edit()
{
	if (!editing())
		return;
	check_timer_.stop();
	detach_display();
	dragging_ = false;
	if (preview_) {
		preview_->setAttribute(Qt::WA_InputMethodEnabled, saved_input_method_);
		preview_->setFocusPolicy(saved_focus_policy_);
		preview_->unsetCursor();
	}
	obs_sceneitem_release(item_);
	if (group_)
		obs_sceneitem_release(group_);
	obs_source_release(source_);
	item_ = group_ = nullptr;
	source_ = nullptr;
	text_ = nullptr;
	undo_.clear();
	redo_.clear();
}

// --- Editing ------------------------------------------------------------------------------

// The text may have been changed elsewhere (its properties) since the last
// key: take it as it is, keeping the caret where it can be.
void PreviewEditor::sync_text()
{
	std::string now = text_->text();
	if (now == doc_)
		return;
	doc_ = std::move(now);
	caret_ = std::min(caret_, doc_.size());
	anchor_ = std::min(anchor_, doc_.size());
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
}

void PreviewEditor::replace(size_t from, size_t to, const std::string &with, EditKind kind)
{
	from = std::min(from, doc_.size());
	to = std::clamp(to, from, doc_.size());
	if (from == to && with.empty())
		return;
	// Runs of typing, or of deleting, undo together.
	if (kind == EditKind::Other || kind != last_edit_ || undo_.empty())
		undo_.push_back({doc_, caret_, anchor_});
	if (undo_.size() > 500)
		undo_.erase(undo_.begin());
	redo_.clear();
	last_edit_ = kind;

	doc_.replace(from, to - from, with);
	caret_ = anchor_ = from + with.size();
	column_x_ = -1;
	text_->set_text(doc_);
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
	redo_.push_back({doc_, caret_, anchor_});
	Snapshot s = std::move(undo_.back());
	undo_.pop_back();
	doc_ = std::move(s.text);
	caret_ = s.caret;
	anchor_ = s.anchor;
	last_edit_ = EditKind::None;
	text_->set_text(doc_);
}

void PreviewEditor::redo()
{
	if (redo_.empty())
		return;
	undo_.push_back({doc_, caret_, anchor_});
	Snapshot s = std::move(redo_.back());
	redo_.pop_back();
	doc_ = std::move(s.text);
	caret_ = s.caret;
	anchor_ = s.anchor;
	last_edit_ = EditKind::None;
	text_->set_text(doc_);
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
	} else if (event->matches(QKeySequence::Paste)) {
		std::string s = QGuiApplication::clipboard()->text().toStdString();
		s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
		if (!s.empty())
			replace(sel_from(), sel_to(), s, EditKind::Other);
	} else if (event->matches(QKeySequence::Undo)) {
		undo();
	} else if (event->matches(QKeySequence::Redo)) {
		redo();
	} else {
		switch (event->key()) {
		case Qt::Key_Escape:
			end_edit();
			return true;
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
		case Qt::Key_Return:
		case Qt::Key_Enter:
			replace_selection("\n", EditKind::Other);
			break;
		case Qt::Key_Tab:
			replace_selection("\t", EditKind::Typing);
			break;
		default: {
			// Ctrl (without the Alt of AltGr) or Meta makes it a command,
			// not text.
			if ((word && !(mods & Qt::AltModifier)) || (mods & Qt::MetaModifier))
				break;
			const QString text = event->text();
			bool printable = !text.isEmpty();
			for (QChar c : text)
				printable = printable && c.isPrint();
			if (printable)
				replace_selection(text.toStdString(), EditKind::Typing);
			break;
		}
		}
	}
	refresh_overlay();
	return true;
}

bool PreviewEditor::mouse_event(QMouseEvent *event)
{
	const QEvent::Type type = event->type();
	const QPointF pos = event->position();

	if (type == QEvent::MouseButtonDblClick && event->button() == Qt::LeftButton && !editing()) {
		// Start editing the text item double-clicked on, if it's on top.
		obs_source_t *scene_source = obs_frontend_get_current_scene();
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		HitSearch search;
		search.pos = widget_to_canvas(preview_mapping(preview_), pos);
		if (scene)
			obs_scene_enum_items(scene, find_hit, &search);
		obs_source_release(scene_source);

		const Hit hit = search.top;
		if (!hit.item || !TextSource::from(obs_sceneitem_get_source(hit.item)))
			return false;
		// OBS selected what it found under the first click: if that isn't
		// this, the preview isn't laid out as assumed (zoomed, or a fixed
		// scale), and this isn't what was clicked on.
		if (!obs_sceneitem_selected(hit.item) && !(hit.group && obs_sceneitem_selected(hit.group))) {
			blog(LOG_WARNING, "[obs-typeset] the preview isn't scaled to its window; text can't be "
					  "edited in it (right-click the preview: Preview Scaling, Scale to Window)");
			return false;
		}
		if (!begin_edit(hit))
			return false;
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
		const bool triple = type == QEvent::MouseButtonPress &&
				    os_gettime_ns() - last_double_click_ <
					    uint64_t(QApplication::doubleClickInterval()) * 1000000ULL &&
				    (pos - last_double_click_pos_).manhattanLength() < 6;
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
	const std::string commit = event->commitString().toStdString();
	if (!commit.empty())
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
	case Qt::ImCursorPosition:
	case Qt::ImAnchorPosition:
		return 0;
	case Qt::ImSurroundingText:
	case Qt::ImCurrentSelection:
		return QString();
	default:
		return QVariant();
	}
}

bool PreviewEditor::eventFilter(QObject *obj, QEvent *event)
{
	// The preview's display is about to go (its native window is being
	// recreated): stop drawing on it first. This filter runs before OBS's.
	if (obj == window_ && event->type() == QEvent::PlatformSurface &&
	    static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
		    QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
		detach_display();
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
		for (int i = 0; i < 32; i++) {
			const auto q = Qt::InputMethodQuery(1 << i);
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
	case QEvent::Hide:
		if (event->type() == QEvent::Hide)
			end_edit();
		else
			refresh_overlay();
		return false;
	default:
		return false;
	}
}

// --- The caret and selection, drawn on the preview -----------------------------------------

void PreviewEditor::attach_display()
{
	// The display's own lock is held while it draws, and drawing takes the
	// overlay's: so the display is never added to or removed from with the
	// overlay's lock held.
	obs_display_t *display = preview_display(preview_);
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
	if (display)
		obs_display_add_draw_callback(display, draw, this);
	else
		blog(LOG_WARNING, "[obs-typeset] couldn't find the preview's display: no caret is shown");
}

void PreviewEditor::detach_display()
{
	obs_display_t *display;
	{
		std::lock_guard lock(overlay_mutex_);
		display = display_;
		display_ = nullptr;
		if (overlay_.item)
			obs_sceneitem_release(overlay_.item);
		if (overlay_.group)
			obs_sceneitem_release(overlay_.group);
		overlay_ = Overlay();
	}
	// Outside the lock: removing waits for a draw in progress, which takes it.
	if (display)
		obs_display_remove_draw_callback(display, draw, this);
}

void PreviewEditor::refresh_overlay()
{
	if (!editing())
		return;
	if (!display_)
		attach_display();
	Overlay o;
	o.active = true;
	o.item = item_;
	o.group = group_;
	o.mapping = preview_mapping(preview_);
	o.caret = text_->caret(caret_);
	o.show_caret = !has_selection();
	o.selection = text_->selection_boxes(sel_from(), sel_to());
	o.box = text_->box();
	o.blink_from = os_gettime_ns();

	std::lock_guard lock(overlay_mutex_);
	obs_sceneitem_addref(o.item);
	if (o.group)
		obs_sceneitem_addref(o.group);
	if (overlay_.item)
		obs_sceneitem_release(overlay_.item);
	if (overlay_.group)
		obs_sceneitem_release(overlay_.group);
	overlay_ = std::move(o);
}

void PreviewEditor::draw(void *param, uint32_t, uint32_t)
{
	static_cast<PreviewEditor *>(param)->draw_overlay();
}

struct Quad {
	vec2 p[4]; // in order round the edge
};

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

void PreviewEditor::draw_overlay()
{
	std::lock_guard lock(overlay_mutex_);
	if (!overlay_.active || !overlay_.item)
		return;
	const Overlay &o = overlay_;

	// Source units -> canvas -> the preview's pixels.
	matrix4 m;
	item_to_canvas(o.item, o.group, &m);
	auto to_display = [&](double x, double y) {
		vec3 p;
		vec3_set(&p, float(x), float(y), 0.0f);
		vec3_transform(&p, &p, &m);
		vec2 out;
		vec2_set(&out, float(o.mapping.x + p.x * o.mapping.scale), float(o.mapping.y + p.y * o.mapping.scale));
		return out;
	};
	auto box_quad = [&](const Box &b) {
		return Quad{{to_display(b.x, b.y), to_display(b.x + b.w, b.y), to_display(b.x + b.w, b.y + b.h),
			     to_display(b.x, b.y + b.h)}};
	};

	gs_matrix_push();
	gs_matrix_identity();
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	// A frame round the text box, to show it's being edited.
	const float px = float(std::max(1.0, std::round(o.mapping.dpr)));
	const Quad frame = box_quad(o.box);
	std::vector<Quad> edges;
	for (int i = 0; i < 4; i++)
		edges.push_back(line_quad(frame.p[i], frame.p[(i + 1) % 4], px));
	fill(edges, 0x4FC3F7C0);

	std::vector<Quad> selection;
	for (const Box &b : o.selection)
		selection.push_back(box_quad(b));
	fill(selection, 0x3D8EF070);

	// The caret blinks, and shows steadily for a moment after each key.
	const uint64_t since = (os_gettime_ns() - o.blink_from) / 1000000ULL;
	if (o.show_caret && (since < 500 || (since / 530) % 2 == 0)) {
		const vec2 top = to_display(o.caret.x, o.caret.y);
		const vec2 bottom = to_display(o.caret.x, o.caret.y + o.caret.height);
		fill({line_quad(top, bottom, 4.0f * px)}, 0x000000A0);
		fill({line_quad(top, bottom, 2.0f * px)}, 0xFFFFFFFF);
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

} // namespace typeset
