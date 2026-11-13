#include "format-bar.hpp"

#include "text-engine.hpp"

#include <obs-module.h>

#include <QComboBox>
#include <QFontComboBox>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QFrame>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QSpinBox>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace typeset {

namespace {

// Steps of a tenth of the size (at least a pixel), as the shortcuts step.
class SizeBox : public QSpinBox {
public:
	std::function<void(int)> stepped;
	using QSpinBox::QSpinBox;
	void stepBy(int steps) override
	{
		if (stepped)
			stepped(steps > 0 ? 1 : -1);
	}

protected:
	StepEnabled stepEnabled() const override { return StepUpEnabled | StepDownEnabled; }
};

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
		const int widths[] = {14, 9, 12, 7};
		for (int i = 0; i < 4; i++) {
			const int lw = widths[i];
			const int x = align == 1 ? (20 - lw) / 2 : align == 2 ? 17 - lw : 3;
			p.drawLine(QPointF(x, 4.5 + i * 3.7), QPointF(x + lw, 4.5 + i * 3.7));
		}
	});
}

QIcon text_icon(QWidget *w, const QString &text, bool bold, bool italic, bool underline)
{
	return drawn_icon(w, [&](QPainter &p, QColor c) {
		QFont f = w->font();
		f.setPixelSize(14);
		f.setBold(bold);
		f.setItalic(italic);
		f.setUnderline(underline);
		p.setFont(f);
		p.setPen(c);
		p.drawText(QRect(0, 0, 20, 20), Qt::AlignCenter, text);
	});
}

// x with a small 2 raised or lowered.
QIcon script_icon(QWidget *w, bool super)
{
	return drawn_icon(w, [&](QPainter &p, QColor c) {
		QFont f = w->font();
		f.setPixelSize(13);
		p.setFont(f);
		p.setPen(c);
		p.drawText(QRect(1, 2, 12, 17), Qt::AlignCenter, "x");
		f.setPixelSize(9);
		p.setFont(f);
		p.drawText(QRect(11, super ? 1 : 9, 8, 10), Qt::AlignCenter, "2");
	});
}

// The colour: an A over a bar of it, as slide tools show it.
QIcon colour_icon(QWidget *w, uint32_t abgr)
{
	return drawn_icon(w, [&](QPainter &p, QColor c) {
		QFont f = w->font();
		f.setPixelSize(13);
		f.setBold(true);
		p.setFont(f);
		p.setPen(c);
		p.drawText(QRect(0, 0, 20, 15), Qt::AlignCenter, "A");
		const QColor fill(abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF);
		p.setPen(QPen(c, 0.8));
		p.setBrush(fill);
		p.drawRect(QRectF(3.5, 15, 13, 3.5));
	});
}

// Clear formatting: a T with a small cross, the usual sign for it.
QIcon clear_icon(QWidget *w)
{
	return drawn_icon(w, [](QPainter &p, QColor c) {
		p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(4, 4), QPointF(13, 4));
		p.drawLine(QPointF(8.5, 4), QPointF(8.5, 16));
		p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(12.5, 12), QPointF(17.5, 17));
		p.drawLine(QPointF(17.5, 12), QPointF(12.5, 17));
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

	auto *size = new SizeBox(this);
	size_ = size;
	size->setRange(1, 4096);
	size->setSuffix(" px");
	size->setToolTip(tip("Bar.Size", "Ctrl+Shift+> / <"));
	size->setFocusPolicy(Qt::ClickFocus);
	size->setKeyboardTracking(false); // typed sizes apply on Enter, not per digit
	size->stepped = [this](int dir) {
		if (actions_.step_size)
			actions_.step_size(dir);
	};
	QObject::connect(size, &QSpinBox::editingFinished, this, [this] {
		if (!updating_ && size_->hasFocus()) {
			if (actions_.size_px && size_->value() != state_.size_px)
				actions_.size_px(size_->value());
			if (actions_.back_to_text)
				actions_.back_to_text();
		}
	});
	row->addWidget(size);
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
	auto toggle = [this](Toggle t) {
		return [this, t] {
			if (!updating_ && actions_.toggle)
				actions_.toggle(t);
		};
	};
	italic_ = button(tip("Bar.Italic", "Ctrl+I"), true);
	italic_->setIcon(text_icon(this, "I", false, true, false));
	QObject::connect(italic_, &QToolButton::clicked, this, toggle(Toggle::Italic));
	underline_ = button(tip("Bar.Underline", "Ctrl+U"), true);
	underline_->setIcon(text_icon(this, "U", false, false, true));
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
	QObject::connect(clear_, &QToolButton::clicked, this, [this] {
		if (actions_.clear)
			actions_.clear();
	});

	// Everything before acts on the selection (or what's typed next);
	// alignment, after a line, on the whole text.
	auto *line = new QFrame(this);
	line->setFrameShape(QFrame::VLine);
	line->setFrameShadow(QFrame::Plain);
	line->setFixedWidth(1);
	line->setStyleSheet("color: palette(mid);");
	row->addSpacing(group_gap / 2);
	row->addWidget(line);
	row->addSpacing(group_gap / 2);
	align_ = button(tip("Bar.Align"), false);
	align_->setPopupMode(QToolButton::InstantPopup);
	auto *menu = new QMenu(align_);
	const char *names[] = {"Align.Left", "Align.Centre", "Align.Right"};
	for (int i = 0; i < 3; i++) {
		QAction *a = menu->addAction(align_icon(this, i), tip(names[i]));
		QObject::connect(a, &QAction::triggered, this, [this, i] {
			if (actions_.align)
				actions_.align(i);
		});
	}
	align_->setMenu(menu);

	// A style that's on shows in the theme's highlight, as plainly as a
	// pressed button in OBS's own toolbars.
	// (Only the colour: the shape is the theme's own, as on its buttons.)
	// (The theme gives lists and number fields a margin inside their own
	// height, which would draw them shorter than the buttons beside them.)
	setStyleSheet("QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); }"
		      "QComboBox, QAbstractSpinBox { margin-top: 0px; margin-bottom: 0px; }");

	// Every control the same height (OBS's theme sizes each kind its own
	// way), the buttons square at it, and the line a little shorter.
	int h = 0;
	for (QWidget *w : std::initializer_list<QWidget *>{family_, weight_, size_, italic_, underline_, super_, sub_,
							    colour_, clear_, align_})
		h = std::max(h, w->sizeHint().height());
	for (QWidget *w : std::initializer_list<QWidget *>{family_, weight_, size_})
		w->setFixedHeight(h);
	for (QToolButton *b : {italic_, underline_, super_, sub_, colour_, clear_, align_})
		b->setFixedSize(b == align_ ? h + h / 3 : h, h); // (alignment has its menu's arrow)
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
constexpr int bar_radius = 6;

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
	QPainterPath path;
	path.addRoundedRect(QRectF(rect()), bar_radius, bar_radius);
	setMask(QRegion(path.toFillPolygon().toPolygon()));
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

	if (!size_->hasFocus()) {
		size_->setSpecialValueText(s.size_px > 0 ? QString() : QString(" "));
		size_->setMinimum(s.size_px > 0 ? 1 : 0);
		size_->setValue(s.size_px);
	}
	italic_->setChecked(s.italic);
	underline_->setChecked(s.underline);
	super_->setChecked(s.script == 1);
	sub_->setChecked(s.script == 2);
	colour_->setIcon(colour_icon(this, s.colour));
	align_->setIcon(align_icon(this, s.align));
	updating_ = false;
	adjustSize();
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
				     "colour=%8 align=%9 clear=%10")
				     .arg(c(family_), c(weight_), c(size_), c(italic_), c(underline_), c(super_), c(sub_),
					  c(colour_), c(align_), c(clear_));
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
