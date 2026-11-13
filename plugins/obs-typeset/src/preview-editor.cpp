#include "preview-editor.hpp"

#include "edit-model.hpp"
#include "obs-preview.hpp"
#include "overlay.hpp"
#include "settings.hpp"
#include "typeset-source.hpp"

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
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace typeset {

namespace {

// Printable text from a key: every code point (not UTF-16 unit, so emoji
// count) printable, or a format character (the zero-width joiner and
// non-joiner, typed with Shift+Space on a Persian keyboard).
bool printable(const QString &text)
{
	if (text.isEmpty())
		return false;
	for (char32_t c : text.toUcs4())
		if (!QChar::isPrint(c) && QChar::category(c) != QChar::Other_Format)
			return false;
	return true;
}

// --- OBS's undo history ------------------------------------------------------------------------
//
// An edit in the preview, from starting to finishing, is one step in OBS's
// own undo history (Edit, Undo): its words, their styles, and the text box's
// width and alignment as they were before and after. A step names the source
// by its UUID, so it still finds it after a rename, and does nothing once
// it's gone.

obs_data_t *undo_state(obs_source_t *source, const Document &doc)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", doc.text.c_str());
	obs_data_array_t *spans = spans_array(doc.spans);
	obs_data_set_array(settings, "spans", spans);
	obs_data_array_release(spans);
	obs_data_set_int(settings, "box_width", (long long)std::lround(doc.style.box_width));
	obs_data_set_int(settings, "align", int(doc.style.align));
	obs_data_t *state = obs_data_create();
	obs_data_set_string(state, "uuid", obs_source_get_uuid(source));
	obs_data_set_obj(state, "settings", settings);
	obs_data_release(settings);
	return state;
}

void restore_undo_state(const char *json)
{
	obs_data_t *state = json ? obs_data_create_from_json(json) : nullptr;
	if (!state)
		return;
	if (obs_source_t *source = obs_get_source_by_uuid(obs_data_get_string(state, "uuid"))) {
		obs_data_t *settings = obs_data_get_obj(state, "settings");
		obs_source_update(source, settings);
		obs_data_release(settings);
		obs_source_release(source);
	}
	obs_data_release(state);
}

// The preview's draw callback is given this, not the editor: it outlives
// the editor (never freed), so a callback left registered by mistake finds
// no editor and draws nothing, instead of drawing with one that's gone.
class PreviewEditor;
struct DrawHook {
	std::mutex mutex;
	PreviewEditor *editor = nullptr;
};
DrawHook *draw_hook()
{
	static DrawHook *hook = new DrawHook;
	return hook;
}

// UTF-16 units (what input methods count in) to a byte index, and back.
size_t utf16_to_byte(const QString &s, int units)
{
	return size_t(s.left(std::clamp(units, 0, int(s.size()))).toUtf8().size());
}

int byte_to_utf16(const std::string &s, size_t index)
{
	return int(QString::fromStdString(s.substr(0, std::min(index, s.size()))).size());
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
	void set_collection_changing(bool changing)
	{
		collection_changing_ = changing;
		if (changing)
			scene_changed();
	}
	void clear_placeholders();

private:
	QPointer<QWidget> preview_;
	QPointer<QWindow> window_;
	QTimer timer_;
	bool warned_display_ = false;
	bool collection_changing_ = false; // its scenes are going: no new references to their items
	bool closing_ = false;             // OBS is closing: no more steps in its undo history

	// The edit under way.
	ItemRef item_;
	obs_source_t *source_ = nullptr; // referenced
	TextSource *text_ = nullptr;
	obs_data_t *before_ = nullptr; // for OBS's undo history: as it was when editing started
	std::vector<obs_source_t *> watched_scenes_; // referenced: their items' removal ends the edit
	EditModel model_;
	Document shown_; // as the overlay was last drawn for
	bool dragging_ = false;
	std::pair<size_t, size_t> drag_unit_{0, 0}; // a word or paragraph a drag started on
	enum class Unit { Grapheme, Word, Paragraph } drag_by_ = Unit::Grapheme;
	bool swallow_release_ = false;
	uint64_t last_double_click_ = 0;
	QPointF last_double_click_pos_;
	Qt::FocusPolicy saved_focus_policy_ = Qt::NoFocus;
	bool saved_input_method_ = false;
	std::string preedit_; // an input method's composition, not yet in the text
	bool committed_ = false; // an input method committed text (while it was asked to finish)
	// Dragging the text box's width grip.
	bool resizing_ = false;
	QPointF resize_from_pos_;
	double resize_from_width_ = 0;
	// What was under the pointer when the button went down (see mouse_event).
	const obs_sceneitem_t *pressed_item_ = nullptr; // only compared, never used
	bool pressed_selected_ = false;

	// What the preview's draw callback draws, and where it drew the format
	// bar, guarded by its lock.
	std::mutex overlay_mutex_;
	obs_display_t *display_ = nullptr;
	Overlay overlay_;
	std::vector<BarButton> drawn_bar_;
	OverlayPainter painter_; // graphics thread only

	bool editing() const { return text_ != nullptr; }
	static void scene_changed_signal(void *param, calldata_t *)
	{
		// From any thread: checked on the UI thread.
		QMetaObject::invokeMethod(
			static_cast<PreviewEditor *>(param),
			[e = static_cast<PreviewEditor *>(param)] {
				if (e->editing() && !e->still_editable())
					e->end_edit();
			},
			Qt::QueuedConnection);
	}
	void watch_scenes(bool on);
	void paint(uint32_t cx, uint32_t cy);
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
	bool begin_edit(const ItemRef &hit);
	bool still_editable();
	vec3 to_source(QPointF widget_pos, bool *inside = nullptr);
	void on_timer();

	void sync_text();
	void commit() { text_->set_content(model_.text(), model_.spans()); }
	BaseStyle base() const;
	void style(StyleChange change, bool toggle);
	void step_size(int direction);

	bool key_press(QKeyEvent *event);
	bool mouse_event(QMouseEvent *event);
	bool edit_selected();
	void bar_action(BarAction action);
	void pick_colour();
	bool on_grip(QPointF widget_pos);
	void input_method(QInputMethodEvent *event);
	QVariant input_method_query(Qt::InputMethodQuery query);

	void attach_display();
	void detach_display();
	void forget_display();
	void watch_window();
	void refresh_overlay();
	void refresh_placeholders();
	static void draw(void *param, uint32_t cx, uint32_t cy);
};

PreviewEditor *editor = nullptr;

PreviewEditor::PreviewEditor(QWidget *preview) : preview_(preview)
{
	{
		std::lock_guard lock(draw_hook()->mutex);
		draw_hook()->editor = this;
	}
	preview->installEventFilter(this);
	watch_window();
	// So the preview can take keys (F2) once it's clicked.
	if (preview->focusPolicy() == Qt::NoFocus)
		preview->setFocusPolicy(Qt::ClickFocus);
	// OBS turns hotkeys' background reading back on when the application
	// becomes active; while editing, it's turned off again (this runs after
	// OBS's own handler, connected first).
	QObject::connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
		if (!editing())
			return;
		if (state == Qt::ApplicationActive)
			suspend_hotkeys();
		else
			restore_hotkeys(); // in the background, hotkeys work as OBS has them
	});
	// The preview's scaling changed from its menu: editing stops if it's no
	// longer Scale to Window.
	if (QWidget *main = preview->window()) {
		for (const char *name : {"actionScaleWindow", "actionScaleCanvas", "actionScaleOutput"}) {
			if (QAction *a = main->findChild<QAction *>(name))
				QObject::connect(a, &QAction::triggered, this, [this] {
					if (editing() && preview_scaling(preview_) != Scaling::Window)
						end_edit();
				});
		}
	}
	// Twice a second: placeholders for empty texts, whether the text being
	// edited is still in the preview's scene, and whether the preview has a
	// new display. It only looks at the preview's scene; nothing is drawn
	// or laid out.
	timer_.setInterval(500);
	QObject::connect(&timer_, &QTimer::timeout, this, [this] { on_timer(); });
	timer_.start();
	attach_display();
}

PreviewEditor::~PreviewEditor()
{
	closing_ = true;
	end_edit();
	detach_display();
	{
		std::lock_guard lock(draw_hook()->mutex);
		draw_hook()->editor = nullptr;
	}
	clear_placeholders();
	obs_enter_graphics();
	painter_.destroy();
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
	if (obs_display_t *now = preview_->isVisible() ? preview_display(preview_) : nullptr; now && now != display_) {
		forget_display();
		attach_display();
	}
	if (editing()) {
		if (!still_editable()) {
			end_edit();
		} else if (!(text_->document() == shown_)) {
			// Changed from elsewhere without an update signal reaching
			// here (a belt to the signal's braces): drawn for it.
			sync_text();
			refresh_overlay();
		} else {
			// The canvas's size changed in Settings: drawn for it.
			const Mapping now = preview_mapping(preview_);
			bool moved;
			{
				std::lock_guard lock(overlay_mutex_);
				moved = now.x != overlay_.mapping.x || now.y != overlay_.mapping.y ||
					now.scale != overlay_.mapping.scale || now.dpr != overlay_.mapping.dpr;
			}
			if (moved)
				refresh_overlay();
		}
	}
	refresh_placeholders();
}

// --- Placeholders and the overlay -------------------------------------------------------------

void PreviewEditor::refresh_placeholders()
{
	std::vector<ItemRef> found;
	if (preview_ && preview_->isVisible() && !collection_changing_) {
		each_preview_item([&](obs_sceneitem_t *item, obs_sceneitem_t *group) {
			TextSource *t = TextSource::from(obs_sceneitem_get_source(item));
			if (t && item != item_.item() && t->text().empty())
				found.emplace_back(item, group); // referenced inside the enumeration
		});
	}
	const Mapping mapping = preview_ ? preview_mapping(preview_) : Mapping();
	std::vector<ItemRef> old; // let go after the lock (see attach_display)
	std::lock_guard lock(overlay_mutex_);
	old.swap(overlay_.placeholders);
	overlay_.placeholders = std::move(found);
	if (!overlay_.editing)
		overlay_.mapping = mapping;
}

void PreviewEditor::clear_placeholders()
{
	std::vector<ItemRef> old; // let go after the lock
	std::lock_guard lock(overlay_mutex_);
	old.swap(overlay_.placeholders);
}

// What the overlay shows for the edit as it now stands.
void PreviewEditor::refresh_overlay()
{
	if (!editing())
		return;
	Overlay o;
	o.editing = true;
	o.target = item_;
	o.mapping = preview_mapping(preview_);
	o.view_w = float(preview_->width() * o.mapping.dpr);
	o.view_h = float(preview_->height() * o.mapping.dpr);
	o.caret = text_->caret(model_.caret());
	o.show_caret = !model_.has_selection();
	o.selection = text_->selection_boxes(model_.sel_from(), model_.sel_to());
	o.box = text_->box();
	const Document doc = text_->document();
	o.grip = doc.style.box && doc.style.box_width > 0;
	o.plate = doc.style.plate;
	o.tag = std::string(obs_module_text("Editing")) + " “" + obs_source_get_name(source_) + "”";
	if (const Frame frame = text_->frame(); frame.text_scale < 0.995)
		o.tag += std::string(" · ") + obs_module_text("ShrunkTo") + " " +
			 std::to_string(int(std::lround(frame.text_scale * 100))) + "%";
	o.tag += std::string(" · ") + obs_module_text("EscToFinish");

	// The format bar lights up for what the text has, its own font's weight
	// and slant included (a Bold font's text shows B lit).
	const BaseStyle b = base();
	o.bar.bold = model_.has(StyleChange::Bold, b);
	o.bar.italic = model_.has(StyleChange::Italic, b);
	o.bar.underline = model_.has(StyleChange::Underline, b);
	o.bar.size_px = int(std::lround(doc.style.size * model_.size_percent() / 100.0)); // the dialog's units
	o.bar.colour = model_.colour(doc.style.colour);
	o.bar.align = int(doc.style.align);
	o.preedit = preedit_;
	o.blink_from = os_gettime_ns();
	shown_ = doc;
	// The input method follows the caret and selection.
	QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImSurroundingText | Qt::ImCursorPosition |
					       Qt::ImAnchorPosition | Qt::ImCurrentSelection);

	std::lock_guard lock(overlay_mutex_);
	o.placeholders.swap(overlay_.placeholders);
	std::swap(overlay_, o);
	// (o, now the old overlay, is let go after the lock.)
}

// --- Starting and finishing ---------------------------------------------------------------------

vec3 PreviewEditor::to_source(QPointF widget_pos, bool *inside)
{
	const Mapping map = preview_mapping(preview_);
	const vec2 canvas = widget_to_canvas(map, widget_pos);
	vec3 local;
	vec3_zero(&local);
	bool in = point_in_item(item_.item(), item_.group(), canvas, &local);
	if (!in) {
		// A margin round the text counts as the text, so a click just past a
		// line's end or below the last line places the caret there.
		matrix4 m;
		item_to_canvas(item_.item(), item_.group(), &m);
		const double scale = std::max(1e-6, double(std::hypot(m.x.x, m.x.y)));
		const double slack = 12 * map.dpr / map.scale / scale;
		const Box b = text_->box();
		in = local.x >= -slack && local.y >= -slack && local.x <= b.w + slack && local.y <= b.h + slack;
	}
	if (inside)
		*inside = in;
	return local;
}

bool PreviewEditor::still_editable()
{
	if (!preview_ || !preview_->isVisible() || !obs_sceneitem_visible(item_.item()) ||
	    (item_.group() && !obs_sceneitem_visible(item_.group())))
		return false;
	bool found = false;
	each_preview_item([&](obs_sceneitem_t *item, obs_sceneitem_t *) { found = found || item == item_.item(); });
	return found;
}

bool PreviewEditor::begin_edit(const ItemRef &hit)
{
	obs_source_t *source = obs_sceneitem_get_source(hit.item());
	TextSource *text = TextSource::from(source);
	if (!text)
		return false;
	end_edit();

	item_ = hit;
	source_ = obs_source_get_ref(source);
	text_ = text;
	resizing_ = dragging_ = false;
	preedit_.clear();
	const Document d = text_->document();
	model_.start(d.text, d.spans);
	before_ = undo_state(source_, d);
	watch_scenes(true);

	saved_focus_policy_ = preview_->focusPolicy();
	if (saved_focus_policy_ == Qt::NoFocus)
		preview_->setFocusPolicy(Qt::ClickFocus);
	saved_input_method_ = preview_->testAttribute(Qt::WA_InputMethodEnabled);
	preview_->setAttribute(Qt::WA_InputMethodEnabled, true);
	preview_->setFocus(Qt::MouseFocusReason);
	QGuiApplication::inputMethod()->update(Qt::ImEnabled);
	preview_->setCursor(Qt::IBeamCursor);

	suspend_hotkeys();
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
	// A composition in progress is kept, as slide editors keep it: the
	// input method is asked to finish it (it may commit it now, which
	// arrives as an input method event), and whatever's left is typed.
	if (!preedit_.empty()) {
		const std::string composing = preedit_;
		committed_ = false;
		QGuiApplication::inputMethod()->reset();
		if (!editing())
			return; // finished from inside the reset
		if (!committed_) { // it was dropped, not committed: typed as it stood
			sync_text();
			if (model_.replace_selection(composing, EditKind::Typing))
				commit();
		}
	}
	dragging_ = resizing_ = swallow_release_ = false;
	preedit_.clear();
	{
		Overlay old; // let go after the lock (see attach_display)
		std::lock_guard lock(overlay_mutex_);
		old.placeholders.swap(overlay_.placeholders);
		std::swap(overlay_, old);
		drawn_bar_.clear();
	}
	if (preview_) {
		preview_->setAttribute(Qt::WA_InputMethodEnabled, saved_input_method_);
		preview_->setFocusPolicy(saved_focus_policy_);
		preview_->unsetCursor();
		QGuiApplication::inputMethod()->update(Qt::ImEnabled);
	}
	restore_hotkeys();
	signal_handler_disconnect(obs_source_get_signal_handler(source_), "update", source_updated, this);
	watch_scenes(false);
	// One step in OBS's undo history for the whole edit, if it changed
	// anything.
	obs_data_t *after = undo_state(source_, text_->document());
	const std::string undo_json = obs_data_get_json(before_), redo_json = obs_data_get_json(after);
	if (undo_json != redo_json && !closing_) {
		char name[256];
		snprintf(name, sizeof(name), obs_module_text("Undo.Edit"), obs_source_get_name(source_));
		obs_frontend_add_undo_redo_action(name, restore_undo_state, restore_undo_state, undo_json.c_str(),
						  redo_json.c_str(), false);
	}
	obs_data_release(after);
	obs_data_release(before_);
	before_ = nullptr;
	item_.reset();
	obs_source_release(source_);
	source_ = nullptr;
	text_ = nullptr;
	model_.start({}, {});
	refresh_placeholders();
}

// F2 on the preview edits the selected text, with all of it selected, so
// typing replaces it.
bool PreviewEditor::edit_selected()
{
	ItemRef hit;
	int count = 0;
	each_preview_item([&](obs_sceneitem_t *item, obs_sceneitem_t *group) {
		if (obs_sceneitem_selected(item) && TextSource::from(obs_sceneitem_get_source(item))) {
			hit = ItemRef(item, group); // referenced inside the enumeration
			count++;
		}
	});
	if (count != 1 || obs_sceneitem_locked(hit.item()) || (hit.group() && obs_sceneitem_locked(hit.group())))
		return false;
	if (!can_edit_in(preview_) || !begin_edit(hit))
		return false;
	model_.select_all();
	refresh_overlay();
	return true;
}

// --- Editing --------------------------------------------------------------------------------

// The text may have been changed elsewhere (its properties) since the last
// key: take it as it is, keeping the caret where it can be, on a character.
void PreviewEditor::sync_text()
{
	Document now = text_->document();
	if (now.text == model_.text() && now.spans == model_.spans())
		return;
	const size_t caret = text_->cursor_at_or_before(model_.caret());
	const size_t anchor = text_->cursor_at_or_before(model_.anchor());
	model_.take(std::move(now.text), std::move(now.spans), caret, anchor);
}

// What the text's own font has.
BaseStyle PreviewEditor::base() const
{
	const Style st = text_->document().style;
	PangoFontDescription *desc = font_description(st);
	BaseStyle b;
	b.bold = pango_font_description_get_weight(desc) >= PANGO_WEIGHT_SEMIBOLD;
	b.italic = pango_font_description_get_style(desc) != PANGO_STYLE_NORMAL;
	b.underline = st.font_flags & OBS_FONT_UNDERLINE;
	pango_font_description_free(desc);
	return b;
}

void PreviewEditor::style(StyleChange change, bool toggle)
{
	if (model_.style(change, toggle, base()))
		commit();
}

void PreviewEditor::step_size(int direction)
{
	if (model_.step_size(direction))
		commit();
}

bool PreviewEditor::key_press(QKeyEvent *event)
{
	sync_text();
	const Qt::KeyboardModifiers mods = event->modifiers();
	const bool shift = mods & Qt::ShiftModifier;
	const bool word = mods & Qt::ControlModifier;
	bool changed = false;
	const size_t caret = model_.caret();

	if (event->matches(QKeySequence::SelectAll)) {
		model_.select_all();
	} else if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) {
		if (model_.has_selection()) {
			QGuiApplication::clipboard()->setText(QString::fromStdString(model_.selected_text()));
			if (event->matches(QKeySequence::Cut))
				changed = model_.replace_selection("", EditKind::Other);
		}
	} else if (event->matches(QKeySequence::Paste) ||
		   (event->key() == Qt::Key_V && mods == (Qt::ControlModifier | Qt::ShiftModifier))) {
		// Always as plain text.
		bool cut = false;
		const std::string s = clean_paste(QGuiApplication::clipboard()->text().toStdString(), &cut);
		if (cut)
			blog(LOG_INFO, "[obs-typeset] pasted only the first %zu characters", max_paste_chars);
		if (!s.empty())
			changed = model_.replace_selection(s, EditKind::Other);
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
		// of the text's size at a time.
		step_size(event->key() == Qt::Key_Greater || event->key() == Qt::Key_BracketRight ? 1 : -1);
	} else if (word && event->key() == Qt::Key_Space) {
		// Back to the text's own style (the selection, or what's typed next).
		StyleChange c;
		c.field = StyleChange::Clear;
		style(c, false);
	} else if (event->matches(QKeySequence::Undo)) {
		changed = model_.undo();
	} else if (event->matches(QKeySequence::Redo)) {
		changed = model_.redo();
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
			changed = model_.replace_selection("\n", EditKind::Other);
			break;
		case Qt::Key_Left:
		case Qt::Key_Right: {
			const int dir = event->key() == Qt::Key_Right ? 1 : -1;
			if (model_.has_selection() && !shift)
				model_.set_caret(dir > 0 ? model_.sel_to() : model_.sel_from(), false);
			else
				model_.set_caret(word ? text_->move_by_word(caret, dir) : text_->move_visually(caret, dir),
						 shift);
			model_.column = -1;
			break;
		}
		case Qt::Key_Up:
		case Qt::Key_Down: {
			const double column = model_.column < 0 ? text_->caret(caret).x : model_.column;
			model_.set_caret(text_->move_by_line(caret, event->key() == Qt::Key_Down ? 1 : -1, column), shift);
			model_.column = column;
			break;
		}
		case Qt::Key_Home:
		case Qt::Key_End: {
			const int dir = event->key() == Qt::Key_End ? 1 : -1;
			model_.set_caret(word ? (dir > 0 ? model_.text().size() : 0) : text_->line_edge(caret, dir), shift);
			model_.column = -1;
			break;
		}
		case Qt::Key_Backspace:
		case Qt::Key_Delete: {
			const int dir = event->key() == Qt::Key_Delete ? 1 : -1;
			if (model_.has_selection()) {
				changed = model_.replace_selection("", EditKind::Other);
				break;
			}
			// Backspace after a composed character (an accent typed
			// after its letter) takes off only the last part of it.
			const size_t to = word	    ? text_->move_by_word(caret, dir)
					  : dir > 0 ? text_->next_cursor(caret, dir)
						    : text_->backspace_from(caret);
			changed = model_.replace(std::min(caret, to), std::max(caret, to), "", EditKind::Deleting);
			break;
		}
		case Qt::Key_Tab:
			changed = model_.replace_selection("\t", EditKind::Typing);
			break;
		default:
			// Ctrl or Alt (but not both: that's AltGr on some systems) or
			// Meta makes it a command, not text.
			if ((word != bool(mods & Qt::AltModifier)) || (mods & Qt::MetaModifier))
				break;
			if (printable(event->text()))
				changed = model_.replace_selection(event->text().toStdString(), EditKind::Typing);
			break;
		}
	}
	if (changed)
		commit();
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
	case BarAction::Bigger:
		step_size(action == BarAction::Bigger ? 1 : -1);
		break;
	case BarAction::Colour:
		// The colour dialog runs an event loop of its own: opened after
		// this click has been handled, not from inside it.
		QTimer::singleShot(0, this, [this] { pick_colour(); });
		break;
	case BarAction::Align: {
		// The whole text's alignment, round left, centre, right.
		const int now = int(text_->document().style.align);
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "align", (now + 1) % 3);
		obs_source_update(source_, settings);
		obs_data_release(settings);
		break;
	}
	case BarAction::Clear:
		c.field = StyleChange::Clear;
		style(c, false);
		break;
	default: // the size readout isn't a button
		break;
	}
	refresh_overlay();
}

void PreviewEditor::pick_colour()
{
	if (!editing())
		return;
	sync_text();
	const uint32_t now = model_.colour(text_->document().style.colour);
	const QColor initial(now & 0xFF, (now >> 8) & 0xFF, (now >> 16) & 0xFF, (now >> 24) & 0xFF);
	// The colour dialog takes the focus for a while; editing carries on.
	// It runs an event loop of its own, in which anything can happen: OBS
	// can even be closing, with this editor gone, when it returns.
	QPointer<PreviewEditor> self(this);
	const QColor picked = QColorDialog::getColor(initial, preview_, obs_module_text("TextColour"),
						     QColorDialog::ShowAlphaChannel);
	if (!self)
		return;
	if (preview_)
		preview_->setFocus(Qt::OtherFocusReason);
	if (!editing() || !picked.isValid())
		return;
	sync_text(); // the text may have changed while the dialog was open
	StyleChange c;
	c.field = StyleChange::Colour;
	c.colour = uint32_t(picked.red()) | uint32_t(picked.green()) << 8 | uint32_t(picked.blue()) << 16 |
		   uint32_t(picked.alpha()) << 24;
	style(c, false);
	refresh_overlay();
}

// Whether a point is on the text box's width grip (a little generous: it's
// small).
bool PreviewEditor::on_grip(QPointF pos)
{
	const Document doc = text_->document();
	if (!doc.style.box || doc.style.box_width <= 0)
		return false;
	matrix4 m;
	item_to_canvas(item_.item(), item_.group(), &m);
	const Box b = text_->box();
	vec3 p;
	vec3_set(&p, float(b.w), float(b.h * 0.5), 0.0f);
	vec3_transform(&p, &p, &m);
	const QPointF g = canvas_to_widget(preview_mapping(preview_), p) + QPointF(7, 0);
	return std::fabs(pos.x() - g.x()) <= 8 && std::fabs(pos.y() - g.y()) <= 18;
}

bool PreviewEditor::mouse_event(QMouseEvent *event)
{
	const QEvent::Type type = event->type();
	const QPointF pos = event->position();
	const bool left = event->button() == Qt::LeftButton;

	// Whether what's under the pointer was selected before this click: OBS
	// then selects what's below it instead (clicks cycle down through
	// overlapping items), so a double-click on selected text leaves
	// something else selected by the time it arrives.
	if (type == QEvent::MouseButtonPress && left && !editing()) {
		const ItemRef hit = item_at(widget_to_canvas(preview_mapping(preview_), pos));
		pressed_item_ = hit.item();
		pressed_selected_ = hit && obs_sceneitem_selected(hit.item());
		return false;
	}

	if (type == QEvent::MouseButtonDblClick && left && !editing() && !collection_changing_) {
		// Start editing the text item double-clicked on, if it's on top
		// and not locked (OBS's own clicks skip locked items too).
		const ItemRef hit = item_at(widget_to_canvas(preview_mapping(preview_), pos));
		if (!hit || !TextSource::from(obs_sceneitem_get_source(hit.item())) || !can_edit_in(preview_))
			return false;
		// OBS selected what it found under the first click (or it was
		// selected already): if that isn't this, the preview isn't laid
		// out as assumed.
		const bool was_selected = hit.item() == pressed_item_ && pressed_selected_;
		if (!was_selected && !obs_sceneitem_selected(hit.item()) &&
		    !(hit.group() && obs_sceneitem_selected(hit.group()))) {
			blog(LOG_INFO, "[obs-typeset] the text double-clicked isn't the one OBS selected; not editing");
			return false;
		}
		if (!begin_edit(hit))
			return false;
		select_only(hit.item());
		const vec3 local = to_source(pos);
		drag_unit_ = text_->word_at(text_->hit_test(local.x, local.y, false));
		model_.select(drag_unit_.first, drag_unit_.second);
		drag_by_ = Unit::Word;
		dragging_ = swallow_release_ = true;
		last_double_click_ = os_gettime_ns();
		last_double_click_pos_ = pos;
		refresh_overlay();
		return true;
	}

	if (!editing())
		return false;

	// The format bar, where it was last drawn. (A quick second click on a
	// button is a double-click: it's a click.)
	if ((type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) && left) {
		const QPointF d = pos * preview_mapping(preview_).dpr;
		BarAction hit = BarAction::Count;
		{
			std::lock_guard lock(overlay_mutex_);
			for (const BarButton &b : drawn_bar_)
				if (b.action != BarAction::SizeReadout && b.contains(float(d.x()), float(d.y())))
					hit = b.action;
		}
		if (hit != BarAction::Count) {
			swallow_release_ = true;
			bar_action(hit);
			return true;
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
			item_to_canvas(item_.item(), item_.group(), &m);
			const double ax = m.x.x, ay = m.x.y, len2 = ax * ax + ay * ay;
			const double dx = (pos.x() - resize_from_pos_.x()) * map.dpr / map.scale;
			const double dy = (pos.y() - resize_from_pos_.y()) * map.dpr / map.scale;
			const double along = len2 > 0 ? (dx * ax + dy * ay) / len2 : 0;
			text_->set_box_width(int(std::lround(resize_from_width_ + along)));
			refresh_overlay();
			return true;
		}
		if (type == QEvent::MouseButtonRelease && left) {
			resizing_ = false;
			return true;
		}
	}
	if (type == QEvent::MouseButtonPress && left && on_grip(pos)) {
		resizing_ = true;
		resize_from_pos_ = pos;
		resize_from_width_ = text_->document().style.box_width;
		return true;
	}
	if (type == QEvent::MouseMove && !dragging_ && on_grip(pos)) {
		preview_->setCursor(Qt::SizeHorCursor);
		return true;
	}

	bool inside = false;
	const vec3 local = to_source(pos, &inside);
	switch (type) {
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonDblClick: {
		if (!left)
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
			model_.select(drag_unit_.first, drag_unit_.second);
			last_double_click_ = os_gettime_ns();
			last_double_click_pos_ = pos;
		} else if (triple) {
			drag_by_ = Unit::Paragraph;
			drag_unit_ = text_->paragraph_at(text_->hit_test(local.x, local.y, false));
			model_.select(drag_unit_.first, drag_unit_.second);
		} else {
			drag_by_ = Unit::Grapheme;
			model_.set_caret(text_->hit_test(local.x, local.y), event->modifiers() & Qt::ShiftModifier);
		}
		model_.column = -1;
		dragging_ = swallow_release_ = true;
		refresh_overlay();
		return true;
	}
	case QEvent::MouseMove:
		if (dragging_) {
			const size_t at = text_->hit_test(local.x, local.y, drag_by_ == Unit::Grapheme);
			if (drag_by_ == Unit::Grapheme) {
				model_.set_caret(at, true);
			} else {
				const auto unit = drag_by_ == Unit::Word ? text_->word_at(at) : text_->paragraph_at(at);
				if (unit.first < drag_unit_.first)
					model_.select(drag_unit_.second, unit.first);
				else
					model_.select(drag_unit_.first, std::max(unit.second, drag_unit_.second));
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
		if (swallow_release_ && left) {
			swallow_release_ = dragging_ = false;
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
	const std::string commit_text = event->commitString().toStdString();
	if (!commit_text.empty())
		committed_ = true;
	if (event->replacementLength() > 0 && !model_.has_selection()) {
		// The input method replaces text round the caret (counted in
		// UTF-16 units from it).
		const QString all = QString::fromStdString(model_.text());
		const int at = byte_to_utf16(model_.text(), model_.caret());
		const int from = std::clamp(at + event->replacementStart(), 0, int(all.size()));
		const int to = std::clamp(from + event->replacementLength(), from, int(all.size()));
		model_.select(utf16_to_byte(all, from), utf16_to_byte(all, to));
	}
	// Only a commit (or a replacement the input method asked for) changes
	// the text; a composition starting over a selection leaves it alone.
	if ((!commit_text.empty() || event->replacementLength() > 0) &&
	    model_.replace_selection(commit_text, EditKind::Typing))
		commit();
	refresh_overlay();
}

QVariant PreviewEditor::input_method_query(Qt::InputMethodQuery query)
{
	switch (query) {
	case Qt::ImEnabled:
		return true;
	case Qt::ImCursorRectangle: {
		const Caret c = text_->caret(model_.caret());
		matrix4 m;
		item_to_canvas(item_.item(), item_.group(), &m);
		const Mapping map = preview_mapping(preview_);
		auto to_widget = [&](double x, double y) {
			vec3 p;
			vec3_set(&p, float(x), float(y), 0.0f);
			vec3_transform(&p, &p, &m);
			return canvas_to_widget(map, p);
		};
		return QRectF(to_widget(c.x, c.y), to_widget(c.x, c.y + c.height)).normalized().toAlignedRect();
	}
	case Qt::ImSurroundingText:
		return QString::fromStdString(model_.text());
	case Qt::ImCursorPosition:
		return byte_to_utf16(model_.text(), model_.caret());
	case Qt::ImAnchorPosition:
		return byte_to_utf16(model_.text(), model_.anchor());
	case Qt::ImCurrentSelection:
		return QString::fromStdString(model_.selected_text());
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
	// display in its own handler, which may run before or after this: it's
	// forgotten without being touched, and found again when the preview is
	// back (on_timer).
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
		if (!editing() && !collection_changing_ && static_cast<QKeyEvent *>(event)->key() == Qt::Key_F2 &&
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

// --- The preview's display ----------------------------------------------------------------------
//
// On Linux OBS destroys the preview's display whenever the preview hides
// (OBSQTDisplay, as its window's visibility changes) and makes a new one when
// it's shown, so a display found after a hide is always a new one, without
// this callback, whatever its address.

void PreviewEditor::attach_display()
{
	// The display's own lock is held while it draws, and drawing takes the
	// overlay's: so the display is never added to or removed from with the
	// overlay's lock held. (Nor are scene items let go with it held: the
	// last reference going can take the scene's locks.)
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
		obs_display_remove_draw_callback(previous, draw, draw_hook());
	if (display) {
		obs_display_add_draw_callback(display, draw, draw_hook());
	} else if (!warned_display_ && preview_ && preview_->isVisible()) { // hidden, it has none yet
		warned_display_ = true;
		blog(LOG_WARNING, "[obs-typeset] couldn't find the preview's display: text can be edited in "
				  "the preview, but without a caret");
	}
}

void PreviewEditor::forget_display()
{
	std::lock_guard lock(overlay_mutex_);
	display_ = nullptr;
}

void PreviewEditor::detach_display()
{
	obs_display_t *display;
	{
		std::lock_guard lock(overlay_mutex_);
		display = std::exchange(display_, nullptr);
	}
	// Outside the lock: removing waits for a draw in progress, which takes it.
	if (display)
		obs_display_remove_draw_callback(display, draw, draw_hook());
}

void PreviewEditor::draw(void *param, uint32_t cx, uint32_t cy)
{
	auto *hook = static_cast<DrawHook *>(param);
	std::lock_guard lock(hook->mutex);
	if (hook->editor)
		hook->editor->paint(cx, cy);
}

void PreviewEditor::paint(uint32_t cx, uint32_t cy)
{
	std::lock_guard lock(overlay_mutex_);
	painter_.paint(overlay_, cx, cy, &drawn_bar_);
}

// The scenes the text being edited is in (and its group's, if it's in one):
// an item removed or hidden there may be it, and editing ends at once.
void PreviewEditor::watch_scenes(bool on)
{
	auto each = [&](void (*f)(signal_handler_t *, const char *, signal_callback_t, void *)) {
		for (obs_source_t *scene : watched_scenes_) {
			signal_handler_t *h = obs_source_get_signal_handler(scene);
			f(h, "item_remove", scene_changed_signal, this);
			f(h, "item_visible", scene_changed_signal, this);
			f(h, "remove", scene_changed_signal, this);
		}
	};
	if (!on) {
		each(signal_handler_disconnect);
		for (obs_source_t *scene : watched_scenes_)
			obs_source_release(scene);
		watched_scenes_.clear();
		return;
	}
	for (obs_sceneitem_t *it : {item_.item(), item_.group()}) {
		obs_scene_t *scene = it ? obs_sceneitem_get_scene(it) : nullptr;
		if (obs_source_t *s = scene ? obs_source_get_ref(obs_scene_get_source(scene)) : nullptr)
			watched_scenes_.push_back(s);
	}
	each(signal_handler_connect);
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

void editor_scene_collection(bool changing)
{
	if (editor)
		editor->set_collection_changing(changing);
}

} // namespace typeset
