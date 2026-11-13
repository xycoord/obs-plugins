// The format bar shown above the text being edited in OBS's preview: real
// Qt controls, so OBS's theme draws them as it draws the rest of its window.
// Grouped by what they act on: the selection's font (typeface, size, weight;
// bold is a weight: Ctrl+B), its style (italic, underline, superscript,
// subscript, colour) and clear formatting; then, after a line, the whole
// text's alignment. Each has a tooltip naming its shortcut.
//
// It's a small frameless window kept above OBS's (see the constructor). It
// takes the keyboard only while its size field or weight list is in use, and
// gives it back to the preview after.
#pragma once

#include <QWidget>

#include <functional>
#include <string>
#include <vector>

class QComboBox;
class QFontComboBox;
class QToolButton;

namespace typeset {

class FormatBar : public QWidget {
public:
	// What the selection (or what's typed next) has.
	struct State {
		std::string family;       // the typeface shown; empty: mixed
		std::vector<int> weights; // that typeface's weights, for the list
		int weight = 400;         // 0: mixed
		bool italic = false, underline = false;
		int script = 0;   // 0 none, 1 superscript, 2 subscript; -1 mixed
		int size_px = 0;  // in the text's own pixels; 0: mixed
		uint32_t colour = 0xFFFFFFFF; // 0xAABBGGRR
		int align = 0;
	};
	enum class Toggle { Italic, Underline, Superscript, Subscript };
	struct Actions {
		std::function<void(const std::string &)> family;
		std::function<void(int)> weight;
		std::function<void(int)> size_px;
		std::function<void(int)> step_size; // +1 bigger, -1 smaller
		std::function<void(Toggle)> toggle;
		std::function<void()> colour;
		std::function<void(int)> align;
		std::function<void()> clear;
		std::function<void()> back_to_text; // the keyboard back to the preview
	};

	FormatBar(QWidget *parent, Actions actions);
	void set_state(const State &state);
	// Where it goes, on the screen: its bottom-left corner just above (x, y)
	// (or its top just below below_y, if there's no room above), kept within
	// the preview's rectangle.
	void place(int x, int y, int below_y, const QRect &within);
	// The buttons' centres on the screen, for the tests (TYPESET_DEBUG).
	void log_geometry() const;

	bool eventFilter(QObject *obj, QEvent *event) override;

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	Actions actions_;
	QFontComboBox *family_;
	QComboBox *weight_;
	QComboBox *size_;
	QToolButton *italic_, *underline_, *super_, *sub_, *colour_, *align_, *clear_;
	State state_;
	bool updating_ = false;
};

} // namespace typeset
