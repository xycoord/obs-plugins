#include "format-bar.hpp"

#include "text-engine.hpp"

#include <obs-module.h>

#include <QComboBox>
#include <QFontComboBox>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QBitmap>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QSlider>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QSignalBlocker>
#include <QTimer>
#include <QWheelEvent>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace typeset {

namespace {

// The sizes the size list offers (the text's own pixels); any other can be
// typed.
// Dense where lower thirds and set times are (24 to 72), sparse at title
// sizes; the text's own size is added (see set_state()).
constexpr int preset_sizes[] = {16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 96, 120, 144, 192, 240, 288, 384, 480};

// An item's role: the text's own size (back to it), not a size of its own.
constexpr int base_role = Qt::UserRole + 1;

// A size typed: its number, whatever's round it ("140 px"); 0 if none.
int typed_size(const QString &text)
{
	QString digits;
	for (QChar c : text)
		if (c.isDigit())
			digits += c;
	return std::clamp(digits.toInt(), 0, 4096);
}

// Icons drawn in the theme's own text colour, so they suit any OBS theme.
QIcon drawn_icon(QWidget *w, const std::function<void(QPainter &, QColor)> &draw)
{
	const qreal dpr = w->devicePixelRatioF();
	QPixmap pm(QSize(20, 20) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	draw(p, w->palette().color(QPalette::ButtonText));
	return QIcon(pm);
}

QIcon align_icon(QWidget *w, int align)
{
	return drawn_icon(w, [align](QPainter &p, QColor c) {
		p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap));
		// Ragged lines, or for justified, full lines and a shorter last one.
		const int ragged[] = {14, 9, 12, 7}, justified[] = {14, 14, 14, 8};
		for (int i = 0; i < 4; i++) {
			const int lw = align == 3 ? justified[i] : ragged[i];
			const int x = align == 1 ? (20 - lw) / 2 : align == 2 ? 17 - lw : 3;
			p.drawLine(QPointF(x, 4.5 + i * 3.7), QPointF(x + lw, 4.5 + i * 3.7));
		}
	});
}

// Each drawn as lines on a 20-pixel square, centred on (10, 10) by its own
// shape (a letter drawn as text sits where its font puts it, off centre).
QPen stroke(QColor c, qreal width = 1.7)
{
	return QPen(c, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

// An I with serifs, slanted: plainly a letter, not a slash.
QIcon italic_icon(QWidget *w)
{
	return drawn_icon(w, [](QPainter &p, QColor c) {
		p.setPen(stroke(c));
		p.drawLine(QPointF(9.5, 4), QPointF(15, 4));      // top serif
		p.drawLine(QPointF(5, 16), QPointF(10.5, 16));    // bottom serif
		p.drawLine(QPointF(12.25, 4), QPointF(7.75, 16)); // the stem
	});
}

QIcon underline_icon(QWidget *w)
{
	return drawn_icon(w, [](QPainter &p, QColor c) {
		p.setPen(stroke(c));
		QPainterPath u;
		u.moveTo(6, 3);
		u.lineTo(6, 9.5);
		u.arcTo(QRectF(6, 5.5, 8, 8), 180, 180); // the bowl, 4 across
		u.lineTo(14, 3);
		p.drawPath(u);
		p.drawLine(QPointF(5, 17), QPointF(15, 17));
	});
}

// An x with a small 2 raised or lowered, the pair centred.
QIcon script_icon(QWidget *w, bool super)
{
	return drawn_icon(w, [super](QPainter &p, QColor c) {
		p.setPen(stroke(c));
		const qreal top = super ? 7.5 : 4.5; // the x, 6 square
		p.drawLine(QPointF(3.5, top), QPointF(9.5, top + 6));
		p.drawLine(QPointF(9.5, top), QPointF(3.5, top + 6));
		// The 2, 5 wide and 6 high, beside the x's top or bottom.
		const qreal x = 11.5, y = super ? 3 : 11;
		p.setPen(stroke(c, 1.4));
		QPainterPath two;
		two.moveTo(x, y + 1.6);
		two.cubicTo(x + 0.3, y - 0.4, x + 5, y - 0.4, x + 5, y + 1.8);
		two.cubicTo(x + 5, y + 3.2, x + 1.5, y + 4.2, x, y + 6);
		two.lineTo(x + 5.2, y + 6);
		p.drawPath(two);
	});
}

// The colour: an A over a bar of it, as Word and Google Docs show it. The
// bar is thick and wider than the letter (a swatch, not an underline), with
// a faint edge so white still has one.
QIcon colour_icon(QWidget *w, uint32_t abgr)
{
	return drawn_icon(w, [abgr](QPainter &p, QColor c) {
		p.setPen(stroke(c, 1.6));
		QPainterPath a;
		a.moveTo(6.5, 12);
		a.lineTo(10, 3);
		a.lineTo(13.5, 12);
		p.drawPath(a);
		p.drawLine(QPointF(7.8, 8.8), QPointF(12.2, 8.8));
		const QColor fill(abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF);
		QColor edge = c;
		edge.setAlphaF(0.35);
		p.setPen(QPen(edge, 1));
		p.setBrush(fill);
		p.drawRoundedRect(QRectF(2.5, 14.5, 15, 4.5), 1.5, 1.5);
	});
}

// Clear formatting: an eraser at work, its rubber end solid, on a line it
// has rubbed out.
QIcon clear_icon(QWidget *w)
{
	return drawn_icon(w, [](QPainter &p, QColor c) {
		p.save();
		p.translate(10, 9.5);
		p.rotate(-45);
		// The eraser lies along x: its body, 14 long and 7 high, the left
		// third the rubber.
		const QRectF body(-7, -3.5, 14, 7);
		QPainterPath outline;
		outline.addRoundedRect(body, 1.6, 1.6);
		QPainterPath rubber;
		rubber.addRect(QRectF(-7, -3.5, 5, 7));
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawPath(outline.intersected(rubber));
		p.setPen(stroke(c, 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawPath(outline);
		p.restore();
		p.setPen(stroke(c, 1.5));
		p.drawLine(QPointF(9.5, 17.5), QPointF(17, 17.5));
	});
}

// Spacing: lines with an arrow between them, up and down.
QIcon spacing_icon(QWidget *w)
{
	return drawn_icon(w, [](QPainter &p, QColor c) {
		p.setPen(stroke(c, 1.5));
		for (int i = 0; i < 3; i++)
			p.drawLine(QPointF(9, 5 + i * 5), QPointF(17, 5 + i * 5));
		p.drawLine(QPointF(4.5, 3.5), QPointF(4.5, 16.5));
		p.drawLine(QPointF(2.5, 5.5), QPointF(4.5, 3.5));
		p.drawLine(QPointF(6.5, 5.5), QPointF(4.5, 3.5));
		p.drawLine(QPointF(2.5, 14.5), QPointF(4.5, 16.5));
		p.drawLine(QPointF(6.5, 14.5), QPointF(4.5, 16.5));
	});
}

// Letter spacing: a V and an A, an arrow apart.
QIcon tracking_icon(QWidget *w)
{
	return drawn_icon(w, [](QPainter &p, QColor c) {
		p.setPen(stroke(c, 1.5));
		p.drawLine(QPointF(2.5, 3), QPointF(5.5, 11));
		p.drawLine(QPointF(8.5, 3), QPointF(5.5, 11));
		p.drawLine(QPointF(11.5, 11), QPointF(14.5, 3));
		p.drawLine(QPointF(17.5, 11), QPointF(14.5, 3));
		p.drawLine(QPointF(12.8, 8), QPointF(16.2, 8));
		p.drawLine(QPointF(3, 16), QPointF(17, 16));
		p.drawLine(QPointF(5, 14), QPointF(3, 16));
		p.drawLine(QPointF(5, 18), QPointF(3, 16));
		p.drawLine(QPointF(15, 14), QPointF(17, 16));
		p.drawLine(QPointF(15, 18), QPointF(17, 16));
	});
}

QString tip(const char *key, const char *shortcut = nullptr)
{
	QString t = QString::fromUtf8(obs_module_text(key));
	if (shortcut)
		t += QString(" (%1)").arg(QString::fromUtf8(shortcut));
	return t;
}

QString weight_label(int w)
{
	return QString::fromUtf8(obs_module_text((std::string("Weight.") + weight_name(w)).c_str()));
}

} // namespace

FormatBar::FormatBar(QWidget *parent, Actions actions)
	: QWidget(parent, Qt::Tool | Qt::FramelessWindowHint), actions_(std::move(actions))
{
	// A small window of its own, kept above OBS's (its parent): the preview
	// is a native window OBS draws into, which covers any widget inside it,
	// and giving a widget there a native window of its own would make Qt
	// recreate the preview's, taking OBS's display with it.
	setAttribute(Qt::WA_ShowWithoutActivating);
	// Its own background is drawn (rounded, as OBS's controls are): see
	// paintEvent().
	setFocusPolicy(Qt::NoFocus);
	setObjectName("typesetFormatBar");

	auto *row = new QHBoxLayout(this);
	// One margin all round, one gap within a group, a wider one between
	// groups (and either side of the line).
	constexpr int margin = 6, gap = 4, group_gap = 8; // group_gap is added to gap
	row->setContentsMargins(margin, margin, margin, margin);
	row->setSpacing(gap);

	// The typeface: OBS's fonts, each shown in itself; type to find one.
	family_ = new QFontComboBox(this);
	family_->setToolTip(tip("Bar.Typeface"));
	family_->setFocusPolicy(Qt::ClickFocus);
	family_->setMaximumWidth(200);
	// Typed into, to find a font among hundreds (a plain list would take a
	// space in a name as choosing what's found so far).
	QObject::connect(family_, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
		if (!updating_ && actions_.family)
			actions_.family(family_->currentFont().family().toStdString());
		if (actions_.back_to_text)
			actions_.back_to_text();
	});
	row->addWidget(family_);

	weight_ = new QComboBox(this);
	weight_->setToolTip(tip("Bar.Weight", "Ctrl+B: Bold"));
	weight_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	weight_->setFocusPolicy(Qt::ClickFocus);
	// The same shape as the typeface's (a field and its arrow), though only
	// chosen from: a click anywhere on it opens the list.
	weight_->setEditable(true);
	weight_->lineEdit()->setReadOnly(true);
	weight_->lineEdit()->installEventFilter(this);
	QObject::connect(weight_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
		const int w = weight_->itemData(i).toInt();
		if (!updating_ && w > 0 && actions_.weight)
			actions_.weight(w);
		if (actions_.back_to_text)
			actions_.back_to_text();
	});

	// The size: a list like the typeface's (Word's size box). Type any size
	// and press Enter, or pick a common one, or the text's own; once clicked
	// into, Up/Down and the wheel step it a tenth, as the shortcuts do.
	// Nothing reaches the text (which may be on air) until it's committed.
	size_ = new QComboBox(this);
	size_->setEditable(true);
	size_->setInsertPolicy(QComboBox::NoInsert);
	size_->setFocusPolicy(Qt::ClickFocus);
	size_->setMaxVisibleItems(int(std::size(preset_sizes)) + 1); // all of it, no scrolling
	// A fixed width, room for "4096+": the bar doesn't shift as sizes change.
	size_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	size_->setMinimumContentsLength(5);
	auto apply_typed = [this] {
		const int px = typed_size(size_->currentText());
		if (!updating_ && px > 0 && (px != state_.size_px || state_.size_max_px != state_.size_px) &&
		    actions_.size_px)
			actions_.size_px(px);
		show_size();
		if (actions_.back_to_text)
			actions_.back_to_text();
	};
	QObject::connect(size_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
		if (!updating_ && i >= 0) {
			if (size_->itemData(i, base_role).toBool()) {
				if (actions_.size_base)
					actions_.size_base();
			} else if (actions_.size_px) {
				actions_.size_px(size_->itemData(i).toInt());
			}
		}
		show_size();
		if (actions_.back_to_text)
			actions_.back_to_text();
	});
	QObject::connect(size_->lineEdit(), &QLineEdit::returnPressed, this, apply_typed);
	size_->installEventFilter(this);
	row->addWidget(size_);
	row->addWidget(weight_); // typeface, size, then weight
	row->addSpacing(group_gap);

	auto button = [&](const QString &tooltip, bool checkable) {
		auto *b = new QToolButton(this);
		b->setAutoRaise(true);
		b->setCheckable(checkable);
		b->setFocusPolicy(Qt::NoFocus);
		b->setToolTip(tooltip);
		b->setIconSize(QSize(20, 20));
		row->addWidget(b);
		return b;
	};
	// After any button, the keyboard is the text's again (a click on the bar
	// makes its window the active one), so typing carries on.
	auto then_back = [this] {
		if (actions_.back_to_text)
			actions_.back_to_text();
	};
	auto toggle = [this, then_back](Toggle t) {
		return [this, t, then_back] {
			if (!updating_ && actions_.toggle)
				actions_.toggle(t);
			then_back();
		};
	};
	italic_ = button(tip("Bar.Italic", "Ctrl+I"), true);
	italic_->setIcon(italic_icon(this));
	QObject::connect(italic_, &QToolButton::clicked, this, toggle(Toggle::Italic));
	underline_ = button(tip("Bar.Underline", "Ctrl+U"), true);
	underline_->setIcon(underline_icon(this));
	QObject::connect(underline_, &QToolButton::clicked, this, toggle(Toggle::Underline));
	super_ = button(tip("Bar.Superscript", "Ctrl+Shift+="), true);
	super_->setIcon(script_icon(this, true));
	QObject::connect(super_, &QToolButton::clicked, this, toggle(Toggle::Superscript));
	sub_ = button(tip("Bar.Subscript", "Ctrl+="), true);
	sub_->setIcon(script_icon(this, false));
	QObject::connect(sub_, &QToolButton::clicked, this, toggle(Toggle::Subscript));
	colour_ = button(tip("Bar.Colour"), false);
	QObject::connect(colour_, &QToolButton::clicked, this, [this] {
		if (actions_.colour)
			actions_.colour();
	});
	clear_ = button(tip("Bar.Clear", "Ctrl+Space"), false);
	clear_->setIcon(clear_icon(this));
	QObject::connect(clear_, &QToolButton::clicked, this, [this, then_back] {
		if (actions_.clear)
			actions_.clear();
		then_back();
	});
	// Spacing: letters (the selection's) and lines (the text's), together.
	tracking_ = button(tip("Bar.Spacing"), false);
	tracking_->setIcon(spacing_icon(this));
	QObject::connect(tracking_, &QToolButton::clicked, this, [this] { show_popup(tracking_popup_, tracking_); });

	// Everything before acts on the selection (or what's typed next), but
	// line spacing; the paragraph, after a line, on the whole text: its icon
	// is its alignment.
	auto *line = new QFrame(this);
	line->setFrameShape(QFrame::VLine);
	line->setFrameShadow(QFrame::Plain);
	line->setFixedWidth(1);
	line->setStyleSheet("color: palette(mid);");
	// (Either side of it, the gap and a little: a line takes no room of its
	// own, so the two together are a group's gap.)
	row->addSpacing((group_gap - gap) / 2);
	row->addWidget(line);
	row->addSpacing((group_gap - gap) / 2);
	paragraph_ = button(tip("Bar.Paragraph"), false);
	QObject::connect(paragraph_, &QToolButton::clicked, this, [this] { show_popup(paragraph_popup_, paragraph_); });
	make_popups();

	// A style that's on shows in the theme's accent, as plainly as a
	// pressed button in OBS's own toolbars (the editor sets the theme's own:
	// see theme_accent()).
	set_accent(palette().color(QPalette::Highlight));

	// Every control the same height (OBS's theme sizes each kind its own
	// way), the buttons square at it, and the line a little shorter.
	int h = 0;
	for (QWidget *w : std::initializer_list<QWidget *>{family_, weight_, size_, italic_, underline_, super_, sub_,
							    colour_, clear_, tracking_, paragraph_})
		h = std::max(h, w->sizeHint().height());
	for (QWidget *w : std::initializer_list<QWidget *>{family_, weight_, size_})
		w->setFixedHeight(h);
	for (QToolButton *b : {italic_, underline_, super_, sub_, tracking_, colour_, clear_, paragraph_})
		b->setFixedSize(h, h);
	line->setFixedHeight(h - 2 * gap);

	// Keys typed into the size field or the weight list are theirs, not
	// OBS's shortcuts (Delete would remove the source).
	for (QWidget *w : findChildren<QWidget *>())
		w->installEventFilter(this);
	set_state(state_);
	hide();
}

// The bar's own shape: a rounded panel in the theme's window colour with a
// hairline edge, its corners as round as OBS's controls'. Cut to that shape
// with a mask (no compositor is needed for it).
constexpr int bar_radius = 10; // the controls' own (about 4) plus the margin: the corners nest

void FormatBar::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(QPen(palette().color(QPalette::Mid), 1));
	p.setBrush(palette().color(QPalette::Window));
	p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), bar_radius, bar_radius);
}

void FormatBar::resizeEvent(QResizeEvent *event)
{
	// The shape cut to the pixel from the same rounded rectangle that's
	// painted (a polygon of it would cut the corners off flat).
	QBitmap shape(size());
	shape.fill(Qt::color0);
	QPainter p(&shape);
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::color1);
	p.drawRoundedRect(QRectF(rect()), bar_radius, bar_radius);
	p.end();
	setMask(shape);
	QWidget::resizeEvent(event);
}

void FormatBar::set_state(const State &s)
{
	updating_ = true;
	state_ = s;

	if (!family_->hasFocus()) {
		if (s.family.empty()) {
			family_->setCurrentIndex(-1); // mixed: shown blank
			family_->lineEdit()->clear();
		} else {
			family_->setCurrentFont(QFont(QString::fromStdString(s.family)));
		}
	}
	weight_->clear();
	std::vector<int> weights = s.weights;
	// What the text has, as the list's nearest weight within a step (fonts
	// put their Book at 380, their Regular at 400: one weight to a reader),
	// or as itself if the font has nothing near (a synthesised weight).
	int shown = s.weight;
	for (int w : weights)
		if (shown > 0 && std::abs(w - shown) <= 50)
			shown = w;
	if (shown > 0 && std::find(weights.begin(), weights.end(), shown) == weights.end())
		weights.push_back(shown);
	std::sort(weights.begin(), weights.end());
	for (int w : weights)
		weight_->addItem(weight_label(w), w);
	if (shown > 0) {
		weight_->setCurrentIndex(weight_->findData(shown));
	} else {
		weight_->addItem(QString(), 0); // mixed: shown blank
		weight_->setCurrentIndex(weight_->count() - 1);
	}

	// The list: the common sizes and the text's own, in order (marked; the
	// common size itself if it's one of them).
	if (s.base_px != listed_base_) {
		listed_base_ = s.base_px;
		size_->clear();
		std::vector<int> sizes(std::begin(preset_sizes), std::end(preset_sizes));
		if (s.base_px > 0 && std::find(sizes.begin(), sizes.end(), s.base_px) == sizes.end())
			sizes.insert(std::upper_bound(sizes.begin(), sizes.end(), s.base_px), s.base_px);
		for (int px : sizes) {
			const bool base = px == s.base_px;
			size_->addItem(base ? QString("%1 \u2014 %2").arg(px).arg(QString::fromUtf8(obs_module_text("Bar.TextSize")))
					    : QString::number(px),
				       px);
			if (base)
				size_->setItemData(size_->count() - 1, true, base_role);
		}
		// The list as wide as its widest entry ("96 — current"), however
		// narrow the field.
		size_->view()->setMinimumWidth(size_->view()->sizeHintForColumn(0) + 24);
	}
	if (!size_->hasFocus() && !size_->lineEdit()->hasFocus())
		show_size();
	italic_->setChecked(s.italic);
	underline_->setChecked(s.underline);
	super_->setChecked(s.script == 1);
	sub_->setChecked(s.script == 2);
	colour_->setIcon(colour_icon(this, s.colour));
	paragraph_->setIcon(align_icon(this, s.align));
	for (int i = 0; i < 4; i++)
		aligns_[i]->setChecked(s.align == i);
	{
		const QSignalBlocker b1(balance_), b2(orphans_);
		balance_->setChecked(s.balance);
		orphans_->setChecked(s.avoid_orphans);
	}
	// Justified, balanced and lone words act only on text that wraps.
	aligns_[3]->setEnabled(s.wraps || s.align == 3);
	balance_->setEnabled(s.wraps);
	orphans_->setEnabled(s.wraps);
	wrap_note_->setVisible(!s.wraps);
	{
		const QSignalBlocker b1(tracking_slider_), b2(leading_slider_);
		tracking_slider_->setValue(int(std::lround(s.tracking)));
		leading_slider_->setValue(int(std::lround(s.line_spacing * 100)));
		if (!tracking_field_->hasFocus())
			tracking_field_->setText(s.tracking_mixed ? QString() : QString("%1 %").arg(s.tracking, 0, 'g', 4));
		if (!leading_field_->hasFocus())
			leading_field_->setText(QString("%1\u00d7").arg(s.line_spacing, 0, 'f', 2));
	}
	updating_ = false;
	adjustSize();
}

// The state's size in the field: the size, or for a mixed selection the
// smallest and a plus (as PowerPoint shows it), the range in the tooltip.
void FormatBar::set_accent(const QColor &accent)
{
	// (The theme gives lists and buttons a margin inside their own size,
	// which would draw them shorter than one another and add to the gaps
	// beside some of them only.)
	setStyleSheet(QString("QToolButton:checked { background: %1; color: palette(highlighted-text); }"
			      "QComboBox, QToolButton { margin: 0px; }")
			      .arg(accent.name()));
}

QRect FormatBar::colour_button() const
{
	return QRect(colour_->mapToGlobal(QPoint(0, 0)), colour_->size());
}

void FormatBar::show_size()
{
	const State &s = state_;
	const bool mixed = s.size_max_px != s.size_px;
	const QSignalBlocker block(size_);
	size_->setCurrentIndex(mixed ? -1 : size_->findData(s.size_px));
	size_->setEditText(s.size_px <= 0 ? QString() : mixed ? QString("%1+").arg(s.size_px) : QString::number(s.size_px));
	QString t = tip("Bar.Size", "Ctrl+Shift+> / <");
	if (mixed)
		t += "\n" + QString::fromUtf8(obs_module_text("Bar.Size.Mixed")).arg(s.size_px).arg(s.size_max_px);
	size_->setToolTip(t);
}

void FormatBar::step_size_field(int dir)
{
	if (actions_.step_size)
		actions_.step_size(dir);
	size_->lineEdit()->selectAll();
}

// The pop-ups: letter spacing for the selection; the paragraph (alignment,
// leading and wrapping) for the whole text. A slider and a field for each
// number (type a value, Enter).
void FormatBar::make_popups()
{
	auto popup = [this] {
		auto *p = new QFrame(this, Qt::Popup);
		p->setFrameShape(QFrame::StyledPanel);
		p->installEventFilter(this);
		return p;
	};
	auto number = [this](QWidget *parent, QGridLayout *grid, int r, const QIcon &icon, const char *tooltip, int min,
			     int max, QSlider **slider, QLineEdit **field,
			     std::function<void(double, bool)> FormatBar::Actions::*act, double scale) {
		auto *label = new QLabel(parent);
		label->setPixmap(icon.pixmap(QSize(20, 20)));
		label->setToolTip(tip(tooltip));
		*slider = new QSlider(Qt::Horizontal, parent);
		(*slider)->setRange(min, max);
		(*slider)->setMinimumWidth(160);
		(*slider)->setToolTip(tip(tooltip));
		*field = new QLineEdit(parent);
		(*field)->setFixedWidth(64);
		(*field)->setAlignment(Qt::AlignRight);
		(*field)->setToolTip(tip(tooltip));
		grid->addWidget(label, r, 0);
		grid->addWidget(*slider, r, 1);
		grid->addWidget(*field, r, 2);
		QSlider *sl = *slider;
		QObject::connect(sl, &QSlider::sliderPressed, this, [this] { new_step_ = true; });
		QObject::connect(sl, &QSlider::valueChanged, this, [this, sl, act, scale](int v) {
			// A drag is one step; a click on the track or a key, each its own.
			const bool step = new_step_ || !sl->isSliderDown();
			new_step_ = false;
			if (!updating_ && actions_.*act)
				(actions_.*act)(v / scale, step);
		});
		QLineEdit *f = *field;
		QObject::connect(f, &QLineEdit::returnPressed, this, [this, f, act, min, max, scale] {
			QString t = f->text();
			t.remove(QRegularExpression("[^0-9.,\\-]"));
			bool ok = false;
			const double v = t.replace(',', '.').toDouble(&ok);
			if (ok && actions_.*act)
				(actions_.*act)(std::clamp(v, min / scale, max / scale), true);
		});
		f->installEventFilter(this);
	};

	tracking_popup_ = popup();
	auto *tg = new QGridLayout(tracking_popup_);
	tg->setContentsMargins(10, 8, 10, 8);
	tg->setHorizontalSpacing(8);
	number(tracking_popup_, tg, 0, tracking_icon(this), "Bar.Tracking", -20, 100, &tracking_slider_,
	       &tracking_field_, &FormatBar::Actions::tracking, 1.0);
	number(tracking_popup_, tg, 1, spacing_icon(this), "Bar.Leading", 50, 300, &leading_slider_, &leading_field_,
	       &FormatBar::Actions::line_spacing, 100.0);

	paragraph_popup_ = popup();
	auto *pg = new QGridLayout(paragraph_popup_);
	pg->setContentsMargins(10, 8, 10, 8);
	pg->setHorizontalSpacing(8);
	pg->setVerticalSpacing(8);
	// Alignment: four buttons, the text's own lit.
	auto *aligns = new QHBoxLayout();
	aligns->setSpacing(4);
	const char *names[] = {"Align.Left", "Align.Centre", "Align.Right", "Align.Justified"};
	for (int i = 0; i < 4; i++) {
		auto *b = new QToolButton(paragraph_popup_);
		b->setAutoRaise(true);
		b->setCheckable(true);
		b->setFocusPolicy(Qt::NoFocus);
		b->setIconSize(QSize(20, 20));
		b->setIcon(align_icon(this, i));
		b->setToolTip(tip(names[i]));
		QObject::connect(b, &QToolButton::clicked, this, [this, i] {
			if (!updating_ && actions_.align)
				actions_.align(i);
			set_state(state_); // lit as the text is, whatever the click did to the button
		});
		aligns->addWidget(b);
		aligns_[i] = b;
	}
	aligns->addStretch();
	pg->addLayout(aligns, 0, 0, 1, 3);
	balance_ = new QCheckBox(tip("Box.Balance"), paragraph_popup_);
	balance_->setToolTip(tip("Box.Balance.Help"));
	orphans_ = new QCheckBox(tip("Box.AvoidOrphans"), paragraph_popup_);
	orphans_->setToolTip(tip("Box.AvoidOrphans.Help"));
	for (QCheckBox *c : {balance_, orphans_}) {
		c->setFocusPolicy(Qt::NoFocus);
		QObject::connect(c, &QCheckBox::toggled, this, [this] {
			if (!updating_ && actions_.wrap_options)
				actions_.wrap_options(balance_->isChecked(), orphans_->isChecked());
		});
	}
	pg->addWidget(balance_, 1, 0, 1, 3);
	pg->addWidget(orphans_, 2, 0, 1, 3);
	wrap_note_ = new QLabel(tip("Bar.WrapNote"), paragraph_popup_);
	wrap_note_->setWordWrap(true);
	wrap_note_->setMaximumWidth(260);
	wrap_note_->setStyleSheet("color: palette(mid);");
	pg->addWidget(wrap_note_, 3, 0, 1, 3);
	// Lit buttons in the pop-up as in the bar.
	paragraph_popup_->setStyleSheet(styleSheet());
}

void FormatBar::show_popup(QFrame *popup, QToolButton *under)
{
	popup->setPalette(palette());
	popup->setStyleSheet(styleSheet());
	popup->adjustSize();
	popup->move(under->mapToGlobal(QPoint(0, under->height() + 4)));
	popup->show();
	if (getenv("TYPESET_DEBUG")) {
		auto c = [](const QWidget *w) {
			const QPoint g = w->mapToGlobal(w->rect().center());
			return QString("%1,%2").arg(g.x()).arg(g.y());
		};
		blog(LOG_INFO, "[obs-typeset] bar popup tracking=%s leading=%s centre=%s justified=%s balance=%s",
		     c(tracking_field_).toUtf8().constData(), c(leading_field_).toUtf8().constData(),
		     c(aligns_[1]).toUtf8().constData(), c(aligns_[3]).toUtf8().constData(),
		     c(balance_).toUtf8().constData());
	}
}

void FormatBar::place(int x, int y, int below_y, const QRect &within)
{
	adjustSize();
	const int w = width(), h = height();
	int top = y - h - 6;
	if (top < within.top())
		top = below_y + 6; // no room above: below
	if (top + h > within.bottom())
		top = within.top() + 2;
	const int left = std::clamp(x, within.left() + 2, std::max(within.left() + 2, within.right() - w - 2));
	move(left, top);
	if (!isVisible())
		show();
	raise();
}

void FormatBar::log_geometry() const
{
	if (!getenv("TYPESET_DEBUG"))
		return;
	auto c = [](const QWidget *w) {
		const QPoint g = w->mapToGlobal(w->rect().center());
		return QString("%1,%2").arg(g.x()).arg(g.y());
	};
	const QString line = QString("typeface=%1 weight=%2 size=%3 italic=%4 underline=%5 super=%6 sub=%7 "
				     "colour=%8 paragraph=%9 clear=%10 tracking=%11")
				     .arg(c(family_), c(weight_), c(size_), c(italic_), c(underline_), c(super_), c(sub_),
					  c(colour_), c(paragraph_), c(clear_))
				     .arg(c(tracking_));
	blog(LOG_INFO, "[obs-typeset] bar %s", line.toUtf8().constData());
}

bool FormatBar::eventFilter(QObject *obj, QEvent *event)
{
	// (Opened on the release: opened on the press, the release would land on
	// the list and choose what's under the pointer.)
	if (obj == weight_->lineEdit() &&
	    (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick))
		return true;
	if (obj == weight_->lineEdit() && event->type() == QEvent::MouseButtonRelease) {
		weight_->showPopup();
		return true;
	}
	// (An editable list has the keyboard itself and hands keys and focus to
	// its field directly, past any filter on the field: they're caught on
	// the list.)
	QLineEdit *field = size_->lineEdit();
	const bool size_focused = size_->hasFocus() || field->hasFocus();
	if ((obj == size_ || obj == field) && event->type() == QEvent::Wheel) {
		// Only once clicked into, as OBS's own fields: a stray scroll over
		// the bar mustn't change text that may be on air.
		if (size_focused) {
			const int dy = static_cast<QWheelEvent *>(event)->angleDelta().y();
			if (dy != 0)
				step_size_field(dy > 0 ? 1 : -1);
		}
		return true;
	}
	if ((obj == size_ || obj == field) && event->type() == QEvent::KeyPress) {
		const int key = static_cast<QKeyEvent *>(event)->key();
		if (key == Qt::Key_Up || key == Qt::Key_Down) {
			step_size_field(key == Qt::Key_Up ? 1 : -1);
			return true;
		}
		if (key == Qt::Key_Escape) { // the field put back; back to the text
			show_size();
			if (actions_.back_to_text)
				actions_.back_to_text();
			return true;
		}
	}
	// Clicked into, the size is all selected, so typing replaces it (on the
	// click's release: its press places the cursor).
	if ((obj == size_ || obj == field) && event->type() == QEvent::FocusIn) {
		select_on_release_ = true;
		QTimer::singleShot(0, field, [field] { field->selectAll(); });
	}
	if (obj == field && event->type() == QEvent::MouseButtonRelease && select_on_release_) {
		select_on_release_ = false;
		QTimer::singleShot(0, field, [field] { field->selectAll(); });
	}
	if ((obj == size_ || obj == field) && event->type() == QEvent::FocusOut) // left uncommitted: put back
		QTimer::singleShot(0, this, [this] {
			if (!size_->hasFocus() && !size_->lineEdit()->hasFocus())
				show_size();
		});
	if ((obj == tracking_popup_ || obj == paragraph_popup_) && event->type() == QEvent::Hide) {
		if (actions_.back_to_text)
			actions_.back_to_text();
		return false;
	}
	if (event->type() == QEvent::ShortcutOverride) {
		event->accept();
		return false; // and the key goes to the field
	}
	return false;
}

void FormatBar::keyPressEvent(QKeyEvent *event)
{
	// Escape (or Enter) in a field: back to typing in the text.
	if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
		if (actions_.back_to_text)
			actions_.back_to_text();
		return;
	}
	QWidget::keyPressEvent(event);
}

} // namespace typeset
