#include "font-browser.hpp"

#include "font-filter.hpp"
#include "font-index.hpp"
#include "format-bar.hpp"
#include "text-engine.hpp"
#include "typeset-source.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <QAbstractListModel>
#include <QApplication>
#include <QCache>
#include <QComboBox>
#include <QCursor>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QListView>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QSizeGrip>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <pango/pangocairo.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace typeset {

namespace {

constexpr int sample_px = 26;      // the samples' size, in the interface's pixels
constexpr int row_height = 64;     // a family's row
constexpr int header_height = 26;  // a section's heading
constexpr int max_recent = 10;
const bool debug = getenv("TYPESET_DEBUG") != nullptr;

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// A weight's name, as the format bar's weight list has it.
QString weight_text(int w)
{
	return text(("Weight." + std::string(weight_name(w))).c_str());
}

// --- Remembered: favourites, recents, the browser's size and filters ---------------------------

struct Prefs {
	std::vector<std::string> favourites, recent;
	int width = 540, height = 600;
	std::set<std::string> categories, feels;
	bool cover = true, weight = false, italic = false, by_name = false;
	bool loaded = false;

	static Prefs &get()
	{
		static Prefs p;
		if (!p.loaded)
			p.load();
		return p;
	}
	static std::string path()
	{
		char *p = obs_module_config_path("font-browser.json");
		std::string out = p ? p : "";
		bfree(p);
		return out;
	}
	void load()
	{
		loaded = true;
		obs_data_t *d = obs_data_create_from_json_file(path().c_str());
		if (!d)
			return;
		auto strings = [&](const char *key, std::vector<std::string> &out) {
			obs_data_array_t *a = obs_data_get_array(d, key);
			for (size_t i = 0, n = a ? obs_data_array_count(a) : 0; i < n; i++) {
				obs_data_t *o = obs_data_array_item(a, i);
				const char *s = obs_data_get_string(o, "name");
				if (s && *s)
					out.emplace_back(s);
				obs_data_release(o);
			}
			obs_data_array_release(a);
		};
		strings("favourites", favourites);
		strings("recent", recent);
		if (obs_data_has_user_value(d, "width")) {
			width = std::clamp(int(obs_data_get_int(d, "width")), 380, 2000);
			height = std::clamp(int(obs_data_get_int(d, "height")), 320, 2000);
		}
		obs_data_release(d);
	}
	void save() const
	{
		char *dir = obs_module_config_path("");
		if (dir)
			os_mkdirs(dir);
		bfree(dir);
		obs_data_t *d = obs_data_create();
		auto strings = [&](const char *key, const std::vector<std::string> &in) {
			obs_data_array_t *a = obs_data_array_create();
			for (const std::string &s : in) {
				obs_data_t *o = obs_data_create();
				obs_data_set_string(o, "name", s.c_str());
				obs_data_array_push_back(a, o);
				obs_data_release(o);
			}
			obs_data_set_array(d, key, a);
			obs_data_array_release(a);
		};
		strings("favourites", favourites);
		strings("recent", recent);
		obs_data_set_int(d, "width", width);
		obs_data_set_int(d, "height", height);
		obs_data_save_json_safe(d, path().c_str(), "tmp", "bak");
		obs_data_release(d);
	}
	bool favourite(const std::string &name) const
	{
		return std::find(favourites.begin(), favourites.end(), name) != favourites.end();
	}
};

// --- The samples, drawn on a thread of their own ------------------------------------------------

struct SampleJob {
	QString key;
	std::string family, text;
	int weight = 400;
	bool italic = false;
	QRgb colour = 0xFFFFFFFF;
	int max_width = 400; // interface pixels
	qreal dpr = 1;
};

QImage draw_sample(PangoContext *context, const SampleJob &j)
{
	PangoLayout *layout = pango_layout_new(context);
	PangoFontDescription *desc = pango_font_description_new();
	pango_font_description_set_family(desc, j.family.c_str());
	pango_font_description_set_weight(desc, PangoWeight(j.weight));
	pango_font_description_set_style(desc, j.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
	pango_font_description_set_absolute_size(desc, sample_px * j.dpr * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_single_paragraph_mode(layout, TRUE);
	pango_layout_set_width(layout, int(j.max_width * j.dpr * PANGO_SCALE));
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_text(layout, j.text.c_str(), -1);
	PangoRectangle ink, logical;
	pango_layout_get_pixel_extents(layout, &ink, &logical);
	const int w = std::max(1, std::max(logical.x + logical.width, ink.x + ink.width));
	const int h = std::max(1, logical.height);
	QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	cairo_surface_t *surface = cairo_image_surface_create_for_data(image.bits(), CAIRO_FORMAT_ARGB32, w, h,
								       int(image.bytesPerLine()));
	cairo_t *cr = cairo_create(surface);
	cairo_set_source_rgba(cr, qRed(j.colour) / 255.0, qGreen(j.colour) / 255.0, qBlue(j.colour) / 255.0, 1.0);
	cairo_move_to(cr, 0, 0);
	show_layout(cr, layout, true);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	g_object_unref(layout);
	image.setDevicePixelRatio(j.dpr);
	return image;
}

class FontBrowser;

// Newest asked for first: what's on screen now comes before what scrolled by.
class SampleWorker {
public:
	explicit SampleWorker(QPointer<FontBrowser> owner) : owner_(owner)
	{
		thread_ = std::thread([this] { run(); });
	}
	~SampleWorker()
	{
		{
			std::lock_guard lock(mutex_);
			stopping_ = true;
		}
		wake_.notify_all();
		thread_.join();
	}
	void ask(SampleJob job)
	{
		{
			std::lock_guard lock(mutex_);
			for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
				if (it->key == job.key) {
					jobs_.erase(it);
					break;
				}
			jobs_.push_front(std::move(job));
			while (jobs_.size() > 48) // scrolled well past: not wanted
				jobs_.pop_back();
		}
		wake_.notify_one();
	}
	void forget()
	{
		std::lock_guard lock(mutex_);
		jobs_.clear();
	}

private:
	void run();
	QPointer<FontBrowser> owner_;
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<SampleJob> jobs_;
	bool stopping_ = false;
};

// --- The list's rows ------------------------------------------------------------------------

struct Row {
	bool header = false;
	QString title;     // a heading's
	size_t family = 0; // into the index
};

class RowModel : public QAbstractListModel {
public:
	std::vector<Row> rows;
	int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(rows.size()); }
	QVariant data(const QModelIndex &index, int role) const override
	{
		if (!index.isValid() || index.row() >= int(rows.size()))
			return {};
		const Row &r = rows[size_t(index.row())];
		if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole)
			return r.header ? r.title : QString::fromStdString(FontIndex::get().families()[r.family].name);
		return {};
	}
	Qt::ItemFlags flags(const QModelIndex &index) const override
	{
		if (!index.isValid() || index.row() >= int(rows.size()) || rows[size_t(index.row())].header)
			return Qt::NoItemFlags;
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	}
	void set(std::vector<Row> r)
	{
		beginResetModel();
		rows = std::move(r);
		endResetModel();
	}
};

// --- The browser ------------------------------------------------------------------------------

class FontBrowser : public RoundedPopup {
public:
	FontBrowser(QWidget *parent, const FontRequest &request, std::function<void(const std::string &)> chosen,
		    std::function<void()> closed);
	~FontBrowser() override;
	void start(const QRect &anchor, const QRect &avoid, const std::string &search);

	// For the rows (the delegate).
	const FontRequest &request() const { return request_; }
	const QImage *sample(const FamilyInfo &f);
	void sample_ready(const QString &key, const QImage &image);
	QRect star_rect(const QRect &row) const { return QRect(row.right() - 30, row.top() + 6, 24, 20); }
	bool covers(const FamilyInfo &f) const { return f.covers(cover_text_); }
	const std::set<std::string> &feels() const { return feels_; }
	QPoint feel_button() const { return feel_->mapToGlobal(feel_->rect().center()); } // (for the tests' log)

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	void rebuild();
	void move_current(int by);
	void choose(const QModelIndex &index);
	void toggle_favourite(const QModelIndex &index);
	void place(const QRect &anchor, const QRect &avoid);
	void make_feel_menu();
	void show_feels();
	QString sample_key(const FamilyInfo &f, int weight) const;

	FontRequest request_;
	std::string sample_text_; // one line of the person's words, shortened
	std::u32string cover_text_;
	std::function<void(const std::string &)> chosen_;
	std::function<void()> closed_;
	bool done_ = false;

	QLineEdit *search_ = nullptr;
	std::vector<std::pair<QToolButton *, std::string>> chips_;
	QToolButton *feel_ = nullptr, *cover_ = nullptr, *weight_ = nullptr, *italic_ = nullptr;
	std::set<std::string> feels_; // chosen in the Feel menu
	QComboBox *order_ = nullptr;
	QListView *list_ = nullptr;
	RowModel *model_ = nullptr;
	QLabel *count_ = nullptr, *hidden_ = nullptr;
	QTimer *waiting_ = nullptr;

	QCache<QString, QImage> samples_{40 * 1024 * 1024}; // by bytes
	std::unique_ptr<SampleWorker> worker_;
};

QPointer<FontBrowser> open_browser;

void SampleWorker::run()
{
	os_set_thread_name("typeset: samples");
	PangoFontMap *map = pango_cairo_font_map_new();
	PangoContext *context = create_context(map);
	for (;;) {
		SampleJob job;
		{
			std::unique_lock lock(mutex_);
			wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
			if (stopping_)
				break;
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}
		QImage image = draw_sample(context, job);
		QPointer<FontBrowser> owner = owner_;
		QMetaObject::invokeMethod(
			qApp,
			[owner, key = job.key, image = std::move(image)] {
				if (owner)
					owner->sample_ready(key, image);
			},
			Qt::QueuedConnection);
	}
	g_object_unref(context);
	g_object_unref(map);
}

class RowDelegate : public QStyledItemDelegate {
public:
	explicit RowDelegate(FontBrowser *browser) : QStyledItemDelegate(browser), browser_(browser) {}

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		const auto *model = static_cast<const RowModel *>(index.model());
		const bool header = model->rows[size_t(index.row())].header;
		return QSize(option.rect.width(), header ? header_height : row_height);
	}

	void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		const auto *model = static_cast<const RowModel *>(index.model());
		const Row &row = model->rows[size_t(index.row())];
		const QPalette &pal = option.palette;
		const QRect r = option.rect;
		p->save();
		if (row.header) {
			QFont f = option.font;
			f.setBold(true);
			f.setPointSizeF(f.pointSizeF() * 0.85);
			p->setFont(f);
			p->setPen(pal.color(QPalette::PlaceholderText));
			p->drawText(r.adjusted(10, 0, -10, -2), Qt::AlignLeft | Qt::AlignBottom, row.title.toUpper());
			p->restore();
			return;
		}
		QStyleOptionViewItem opt = option;
		initStyleOption(&opt, index);
		opt.text.clear();
		const QWidget *w = option.widget;
		(w ? w->style() : QApplication::style())->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, p, w);
		const bool selected = option.state & QStyle::State_Selected;
		const QColor ink = pal.color(selected ? QPalette::HighlightedText : QPalette::Text);
		QColor dim = ink;
		dim.setAlphaF(0.6);

		const FamilyInfo &f = FontIndex::get().families()[row.family];
		const FontRequest &req = browser_->request();
		// Line 1: the name, and what it has, in the interface's own font.
		QFont name_font = option.font;
		name_font.setBold(true);
		p->setFont(name_font);
		p->setPen(ink);
		const QRect line1(r.left() + 10, r.top() + 4, r.width() - 50, 18);
		const QString name = QString::fromStdString(f.name);
		p->drawText(line1, Qt::AlignLeft | Qt::AlignVCenter, name);
		const int name_w = QFontMetrics(name_font).horizontalAdvance(name);
		QStringList facts;
		if (!f.category.empty()) {
			QString cat = QString::fromStdString(f.category);
			if (!f.classes.empty())
				cat += " · " + QString::fromStdString(f.classes.front());
			facts << cat;
		}
		// Its strongest feelings, the ones chosen first.
		QStringList feels;
		const auto strong = strong_feels(f);
		for (const auto &[name, strength] : strong)
			if (browser_->feels().count(name))
				feels << QString::fromStdString(name);
		for (const auto &[name, strength] : strong)
			if (!browser_->feels().count(name) && feels.size() < 2)
				feels << QString::fromStdString(name);
		if (!feels.isEmpty())
			facts << feels.join(", ");
		if (f.variable)
			facts << QString("%1 %2–%3").arg(text("Fonts.Variable")).arg(f.var_lo).arg(f.var_hi);
		else if (f.weights.size() > 1)
			facts << text("Fonts.Weights").arg(f.weights.size());
		if (f.italic)
			facts << text("Fonts.Italic");
		QStringList warnings;
		if (!f.has_weight(req.weight))
			warnings << QString("%1 → %2").arg(weight_text(req.weight), weight_text(f.nearest_weight(req.weight)));
		if (!browser_->covers(f))
			warnings << text("Fonts.Missing");
		if (req.italic && !f.italic)
			warnings << text("Fonts.NoItalic");
		QFont small = option.font;
		small.setPointSizeF(small.pointSizeF() * 0.85);
		p->setFont(small);
		const QRect facts_r(line1.left() + name_w + 10, line1.top(), line1.right() - line1.left() - name_w - 10,
				    line1.height());
		QString fact_text = facts.join("  ·  ");
		if (!warnings.isEmpty())
			fact_text += (fact_text.isEmpty() ? "" : "  ·  ") + warnings.join("  ·  ");
		p->setPen(dim);
		p->drawText(facts_r, Qt::AlignLeft | Qt::AlignVCenter,
			    QFontMetrics(small).elidedText(fact_text, Qt::ElideRight, facts_r.width()));
		// The star: always for a favourite, else on the highlighted row.
		const bool fav = Prefs::get().favourite(f.name);
		if (fav || selected || (option.state & QStyle::State_MouseOver)) {
			QFont star = option.font;
			star.setPointSizeF(star.pointSizeF() * 1.2);
			p->setFont(star);
			p->setPen(fav ? ink : dim);
			p->drawText(browser_->star_rect(r), Qt::AlignCenter, fav ? QStringLiteral("★") : QStringLiteral("☆"));
		}
		// Line 2: the person's words, in it.
		const QRect line2(r.left() + 10, r.top() + 24, r.width() - 20, r.height() - 28);
		if (const QImage *img = browser_->sample(f)) {
			const QSizeF size = QSizeF(img->size()) / img->devicePixelRatio();
			QRectF target(QPointF(line2.left(), line2.top() + (line2.height() - size.height()) / 2), size);
			if (size.height() > line2.height()) { // very tall faces: fitted
				const double k = line2.height() / size.height();
				target = QRectF(line2.left(), line2.top(), size.width() * k, line2.height());
			}
			p->setClipRect(line2);
			p->setRenderHint(QPainter::SmoothPixmapTransform);
			if (selected) {
				// Drawn in the highlight's text colour (the sample is the
				// interface's text colour otherwise).
				QImage tinted = img->copy();
				QPainter tp(&tinted);
				tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
				tp.fillRect(tinted.rect(), ink);
				tp.end();
				p->drawImage(target, tinted);
			} else {
				p->drawImage(target, *img);
			}
		}
		p->restore();
	}

private:
	FontBrowser *browser_;
};

FontBrowser::FontBrowser(QWidget *parent, const FontRequest &request,
			 std::function<void(const std::string &)> chosen, std::function<void()> closed)
	: RoundedPopup(parent), request_(request), chosen_(std::move(chosen)), closed_(std::move(closed))
{
	setAttribute(Qt::WA_DeleteOnClose);
	setObjectName("typesetFontBrowser");

	// One line of the person's words, for the rows (the first line, or the
	// family's name for an empty text); all of them for coverage.
	std::string line = request.sample.substr(0, request.sample.find('\n'));
	const std::u32string u = to_u32(line);
	if (u.size() > 48)
		line = QString::fromStdU32String(u.substr(0, 48)).toStdString();
	sample_text_ = line;
	cover_text_ = drawn_chars(request.sample);

	const Prefs &prefs = Prefs::get();
	auto *col = new QVBoxLayout(this);
	col->setContentsMargins(8, 8, 8, 6);
	col->setSpacing(6);

	search_ = new QLineEdit(this);
	search_->setPlaceholderText(text("Fonts.Search"));
	search_->setClearButtonEnabled(true);
	search_->installEventFilter(this);
	col->addWidget(search_);

	auto chip = [this](const QString &label, bool on) {
		auto *b = new QToolButton(this);
		b->setText(label);
		b->setCheckable(true);
		b->setChecked(on);
		b->setAutoRaise(true);
		b->setFocusPolicy(Qt::NoFocus);
		QObject::connect(b, &QToolButton::toggled, this, [this] { rebuild(); });
		return b;
	};
	auto *cats = new QHBoxLayout();
	cats->setSpacing(4);
	const std::pair<const char *, const char *> categories[] = {{"Fonts.Sans", "Sans Serif"},
								    {"Fonts.Serif", "Serif"},
								    {"Fonts.Display", "Display"},
								    {"Fonts.Handwriting", "Handwriting"},
								    {"Fonts.Mono", "Monospace"}};
	for (const auto &[label, name] : categories) {
		QToolButton *b = chip(text(label), prefs.categories.count(name) > 0);
		chips_.emplace_back(b, name);
		cats->addWidget(b);
	}
	cats->addStretch();
	order_ = new QComboBox(this);
	order_->addItem(text("Fonts.Popular"));
	order_->addItem(text("Fonts.Name"));
	order_->setCurrentIndex(prefs.by_name ? 1 : 0);
	order_->setFocusPolicy(Qt::NoFocus);
	QObject::connect(order_, QOverload<int>::of(&QComboBox::activated), this, [this](int) { rebuild(); });
	cats->addWidget(order_);
	col->addLayout(cats);

	auto *filters = new QHBoxLayout();
	filters->setSpacing(4);
	// Feel: Google's feelings ("Fancy", "Calm"), from a menu (there are 20).
	feel_ = new QToolButton(this);
	feel_->setAutoRaise(true);
	feel_->setFocusPolicy(Qt::NoFocus);
	feel_->setPopupMode(QToolButton::InstantPopup);
	feel_->setToolTip(text("Fonts.Feel.Help"));
	feels_ = prefs.feels;
	filters->addWidget(feel_);
	cover_ = chip(text("Fonts.Covers"), prefs.cover);
	cover_->setToolTip(text("Fonts.Covers.Help"));
	weight_ = chip(text("Fonts.HasWeight").arg(weight_text(request.weight)), prefs.weight);
	italic_ = chip(text("Fonts.HasItalic"), prefs.italic);
	filters->addWidget(cover_);
	filters->addWidget(weight_);
	filters->addWidget(italic_);
	filters->addStretch();
	col->addLayout(filters);

	model_ = new RowModel();
	model_->setParent(this);
	list_ = new QListView(this);
	list_->setModel(model_);
	list_->setItemDelegate(new RowDelegate(this));
	list_->setUniformItemSizes(false);
	list_->setMouseTracking(true);
	list_->setFocusPolicy(Qt::NoFocus); // the keyboard stays in search
	list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	list_->setSelectionMode(QAbstractItemView::SingleSelection);
	list_->viewport()->installEventFilter(this);
	col->addWidget(list_, 1);

	auto *foot = new QHBoxLayout();
	count_ = new QLabel(this);
	hidden_ = new QLabel(this);
	hidden_->setTextFormat(Qt::RichText);
	QObject::connect(hidden_, &QLabel::linkActivated, this, [this](const QString &) { cover_->setChecked(false); });
	foot->addWidget(count_);
	foot->addWidget(hidden_, 1);
	foot->addWidget(new QSizeGrip(this), 0, Qt::AlignBottom | Qt::AlignRight);
	col->addLayout(foot);
	for (QLabel *l : {count_, hidden_}) {
		QFont f = l->font();
		f.setPointSizeF(f.pointSizeF() * 0.9);
		l->setFont(f);
	}

	QObject::connect(search_, &QLineEdit::textChanged, this, [this] { rebuild(); });
	worker_ = std::make_unique<SampleWorker>(QPointer<FontBrowser>(this));
	show_feels();
	setStyleSheet(parent ? parent->styleSheet() : QString());
}

FontBrowser::~FontBrowser()
{
	worker_.reset(); // before what it draws for goes
}

void FontBrowser::start(const QRect &anchor, const QRect &avoid, const std::string &search)
{
	{
		const QSignalBlocker quiet(search_); // the list is made once, below
		search_->setText(QString::fromStdString(search));
	}
	search_->setFocus(Qt::PopupFocusReason);
	search_->end(false);
	const Prefs &prefs = Prefs::get();
	resize(prefs.width, prefs.height);
	place(anchor, avoid);
	if (!FontIndex::get().ready()) {
		waiting_ = new QTimer(this);
		QObject::connect(waiting_, &QTimer::timeout, this, [this] {
			if (FontIndex::get().ready()) {
				waiting_->stop();
				rebuild();
			}
		});
		waiting_->start(100);
	}
	rebuild();
	show();
	search_->setFocus(Qt::PopupFocusReason);
}

// Below what it's opened from if that doesn't cover the text; else beside the
// text, on whichever side has room; else wherever covers it least.
void FontBrowser::place(const QRect &anchor, const QRect &avoid)
{
	QScreen *screen = QGuiApplication::screenAt(anchor.center());
	const QRect area = screen ? screen->availableGeometry() : QRect(0, 0, 1 << 15, 1 << 15);
	const QSize s(std::min(width(), area.width()), std::min(height(), area.height()));
	resize(s);
	const QPoint options[] = {QPoint(anchor.left(), anchor.bottom() + 4), QPoint(avoid.right() + 10, anchor.top()),
				  QPoint(avoid.left() - 10 - s.width(), anchor.top()),
				  QPoint(anchor.left(), anchor.top() - 4 - s.height()),
				  QPoint(anchor.left(), avoid.bottom() + 10), QPoint(anchor.left(), avoid.top() - 10 - s.height())};
	QRect best;
	long best_overlap = -1;
	for (const QPoint &o : options) {
		QRect r(o, s);
		r.moveLeft(std::clamp(r.left(), area.left(), std::max(area.left(), area.right() - s.width() + 1)));
		r.moveTop(std::clamp(r.top(), area.top(), std::max(area.top(), area.bottom() - s.height() + 1)));
		const QRect cover = avoid.isValid() ? r.intersected(avoid) : QRect();
		const long overlap = cover.isValid() ? long(cover.width()) * cover.height() : 0;
		if (best_overlap < 0 || overlap < best_overlap) {
			best = r;
			best_overlap = overlap;
		}
		if (overlap == 0)
			break;
	}
	move(best.topLeft());
}

QString FontBrowser::sample_key(const FamilyInfo &f, int weight) const
{
	return QString("%1|%2|%3|%4|%5")
		.arg(QString::fromStdString(f.name))
		.arg(weight)
		.arg(request_.italic && f.italic ? 1 : 0)
		.arg(devicePixelRatioF())
		.arg(QString::fromStdString(sample_text_.empty() ? f.name : sample_text_));
}

const QImage *FontBrowser::sample(const FamilyInfo &f)
{
	const int weight = f.nearest_weight(request_.weight);
	const QString key = sample_key(f, weight);
	if (const QImage *img = samples_.object(key))
		return img;
	{
		// (Asked again while it's on screen: it moves up the queue.)
		SampleJob job;
		job.key = key;
		job.family = f.name;
		job.text = sample_text_.empty() ? f.name : sample_text_;
		job.weight = weight;
		job.italic = request_.italic && f.italic;
		job.colour = palette().color(QPalette::Text).rgb();
		job.max_width = std::max(100, list_->viewport()->width() - 20);
		job.dpr = devicePixelRatioF();
		worker_->ask(std::move(job));
	}
	return nullptr;
}

void FontBrowser::sample_ready(const QString &key, const QImage &image)
{
	samples_.insert(key, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes()));
	list_->viewport()->update();
}

void FontBrowser::rebuild()
{
	Prefs &prefs = Prefs::get();
	if (FontIndex::get().ready() && !feel_->menu())
		make_feel_menu(); // (once the fonts are read: it lists theirs)
	prefs.categories.clear();
	FontQuery q;
	q.text = search_->text().toStdString();
	for (const auto &[b, name] : chips_)
		if (b->isChecked()) {
			q.categories.insert(name);
			prefs.categories.insert(name);
		}
	q.must_cover = prefs.cover = cover_->isChecked();
	q.cover_text = cover_text_;
	prefs.weight = weight_->isChecked();
	q.need_weight = prefs.weight ? request_.weight : 0;
	q.need_italic = prefs.italic = italic_->isChecked();
	q.feels = prefs.feels = feels_;
	prefs.by_name = order_->currentIndex() == 1;
	q.order = prefs.by_name ? FontQuery::Order::Name : FontQuery::Order::Popular;

	FontIndex &index = FontIndex::get();
	std::vector<Row> rows;
	if (!index.ready()) {
		model_->set({});
		count_->setText(text("Fonts.Reading"));
		hidden_->clear();
		return;
	}
	const std::vector<FamilyInfo> &families = index.families();
	const FontResults found = find_fonts(families, q);
	// With nothing typed: the ones to hand first, in sections; any filter
	// applies to them too.
	const bool sections = q.text.find_first_not_of(' ') == std::string::npos;
	if (sections) {
		std::set<size_t> passing(found.families.begin(), found.families.end());
		auto section = [&](const char *title, const std::vector<std::string> &names) {
			std::vector<Row> part;
			std::set<size_t> seen;
			for (const std::string &n : names) {
				const FamilyInfo *f = index.find(n);
				if (!f)
					continue;
				const size_t i = size_t(f - families.data());
				if (passing.count(i) && seen.insert(i).second)
					part.push_back({false, {}, i});
			}
			if (part.empty())
				return;
			rows.push_back({true, text(title), 0});
			rows.insert(rows.end(), part.begin(), part.end());
		};
		section("Fonts.Recent", prefs.recent);
		section("Fonts.Favourites", prefs.favourites);
		section("Fonts.InUse", request_.in_use);
		if (!found.families.empty())
			rows.push_back({true, text("Fonts.All"), 0});
	}
	for (size_t i : found.families)
		rows.push_back({false, {}, i});
	model_->set(std::move(rows));
	worker_->forget(); // what was asked for may not be on screen now

	count_->setText(text("Fonts.Count").arg(found.families.size()));
	if (found.hidden > 0) {
		QString missing;
		for (char32_t c : found.missing)
			missing += QString::fromStdU32String(std::u32string(1, c)) + " ";
		const QString what = missing.isEmpty() ? text("Fonts.Hidden.Text").arg(found.hidden)
						       : text("Fonts.Hidden").arg(found.hidden).arg(missing.trimmed());
		hidden_->setText(QString("· <a href=\"show\">%1</a>").arg(what.toHtmlEscaped()));
	} else {
		hidden_->clear();
	}
	if (found.unrated > 0) {
		const QString note = text("Fonts.Unrated").arg(found.unrated).toHtmlEscaped();
		hidden_->setText(hidden_->text() + (hidden_->text().isEmpty() ? "" : " ") + "· " + note);
	}
	// Highlighted, for Enter: with nothing typed, the text's own typeface
	// (Enter at once changes nothing); else the best match.
	int first = -1;
	for (int r = 0; r < model_->rowCount(); r++) {
		const Row &row = model_->rows[size_t(r)];
		if (row.header)
			continue;
		if (first < 0)
			first = r;
		if (!sections || families[row.family].name == request_.family) {
			first = r;
			break;
		}
	}
	if (first >= 0) {
		list_->setCurrentIndex(model_->index(first));
		list_->scrollTo(model_->index(std::max(0, first - 1)));
	}
	if (debug) {
		const QModelIndex cur = list_->currentIndex();
		blog(LOG_INFO, "[obs-typeset] fonts shown=%zu hidden=%d unrated=%d first=%s sections=%d feel=%s",
		     found.families.size(), found.hidden, found.unrated,
		     cur.isValid() ? model_->data(cur, Qt::DisplayRole).toString().toUtf8().constData() : "",
		     sections ? 1 : 0, feel_->text().toUtf8().constData());
	}
}

// The Feel menu: every feeling the installed fonts have strongly, with how
// many have it; any number chosen (a family has to have each).
void FontBrowser::make_feel_menu()
{
	auto *menu = new QMenu(feel_);
	menu->installEventFilter(this); // a click on a feeling leaves it open, for another
	QAction *any = menu->addAction(text("Fonts.AnyFeel"));
	QObject::connect(any, &QAction::triggered, this, [this] {
		feels_.clear();
		show_feels();
		rebuild();
	});
	menu->addSeparator();
	const auto counts = feel_counts(FontIndex::get().families());
	for (const auto &[name, count] : counts) {
		QAction *a = menu->addAction(QString("%1  (%2)").arg(QString::fromStdString(name)).arg(count));
		a->setCheckable(true);
		a->setChecked(feels_.count(name) > 0);
		a->setData(QString::fromStdString(name));
		QObject::connect(a, &QAction::toggled, this, [this, name](bool on) {
			if (on)
				feels_.insert(name);
			else
				feels_.erase(name);
			show_feels();
			rebuild();
		});
	}
	if (counts.empty()) {
		menu->addAction(text("Fonts.NoFeels"))->setEnabled(false);
	}
	// Feelings chosen last time but not had by any font now: dropped.
	for (auto it = feels_.begin(); it != feels_.end();)
		it = counts.count(*it) ? std::next(it) : feels_.erase(it);
	feel_->setMenu(menu);
	show_feels();
}

// The button says what's chosen: "Feel", "Fancy", "Fancy + Vintage".
void FontBrowser::show_feels()
{
	QStringList names;
	for (const std::string &n : feels_)
		names << QString::fromStdString(n);
	feel_->setText(names.isEmpty() ? text("Fonts.Feel") : names.join(" + "));
	feel_->setCheckable(true);
	feel_->setChecked(!names.isEmpty());
	if (QMenu *menu = feel_->menu())
		for (QAction *a : menu->actions())
			if (a->isCheckable()) {
				const QSignalBlocker quiet(a);
				a->setChecked(feels_.count(a->data().toString().toStdString()) > 0);
			}
}

void FontBrowser::move_current(int by)
{
	const int n = model_->rowCount();
	if (n == 0)
		return;
	int r = list_->currentIndex().isValid() ? list_->currentIndex().row() : -1;
	const int step = by > 0 ? 1 : -1;
	for (int left = std::abs(by); left > 0;) {
		int next = r + step;
		while (next >= 0 && next < n && model_->rows[size_t(next)].header)
			next += step;
		if (next < 0 || next >= n)
			break;
		r = next;
		left--;
	}
	if (r >= 0) {
		list_->setCurrentIndex(model_->index(r));
		list_->scrollTo(model_->index(r));
	}
}

void FontBrowser::choose(const QModelIndex &index)
{
	if (!index.isValid() || model_->rows[size_t(index.row())].header || done_)
		return;
	const std::string name = FontIndex::get().families()[model_->rows[size_t(index.row())].family].name;
	Prefs &prefs = Prefs::get();
	auto &recent = prefs.recent;
	recent.erase(std::remove(recent.begin(), recent.end(), name), recent.end());
	recent.insert(recent.begin(), name);
	if (recent.size() > size_t(max_recent))
		recent.resize(size_t(max_recent)); // (saved on closing)
	if (debug)
		blog(LOG_INFO, "[obs-typeset] fonts chose %s", name.c_str());
	done_ = true;
	if (chosen_)
		chosen_(name);
	close();
}

void FontBrowser::toggle_favourite(const QModelIndex &index)
{
	if (!index.isValid() || model_->rows[size_t(index.row())].header)
		return;
	const std::string name = FontIndex::get().families()[model_->rows[size_t(index.row())].family].name;
	Prefs &prefs = Prefs::get();
	auto &fav = prefs.favourites;
	if (prefs.favourite(name))
		fav.erase(std::remove(fav.begin(), fav.end(), name), fav.end());
	else
		fav.push_back(name);
	prefs.save();
	list_->viewport()->update();
}

bool FontBrowser::eventFilter(QObject *obj, QEvent *event)
{
	if (obj == search_ && event->type() == QEvent::KeyPress) {
		auto *k = static_cast<QKeyEvent *>(event);
		const int page = std::max(1, list_->viewport()->height() / row_height);
		switch (k->key()) {
		case Qt::Key_Down:
			move_current(1);
			return true;
		case Qt::Key_Up:
			move_current(-1);
			return true;
		case Qt::Key_PageDown:
			move_current(page);
			return true;
		case Qt::Key_PageUp:
			move_current(-page);
			return true;
		case Qt::Key_Return:
		case Qt::Key_Enter:
			choose(list_->currentIndex());
			return true;
		case Qt::Key_Escape:
			close();
			return true;
		case Qt::Key_D:
			if (k->modifiers() & Qt::ControlModifier) {
				toggle_favourite(list_->currentIndex());
				return true;
			}
			break;
		case Qt::Key_F: // Alt+F: the Feel menu, from the keyboard
			if ((k->modifiers() & Qt::AltModifier) && feel_->menu()) {
				feel_->menu()->popup(feel_->mapToGlobal(QPoint(0, feel_->height())));
				return true;
			}
			break;
		default:
			break;
		}
	}
	if (obj == search_ && event->type() == QEvent::ShortcutOverride) {
		event->accept(); // every key to search, none to OBS's shortcuts
		return false;
	}
	if (feel_ && obj == feel_->menu() && event->type() == QEvent::MouseButtonRelease) {
		auto *menu = static_cast<QMenu *>(obj);
		QAction *a = menu->actionAt(static_cast<QMouseEvent *>(event)->position().toPoint());
		if (a && a->isCheckable() && a->isEnabled()) {
			a->trigger();
			return true;
		}
	}
	if (obj == list_->viewport() && event->type() == QEvent::MouseButtonRelease) {
		auto *m = static_cast<QMouseEvent *>(event);
		const QModelIndex index = list_->indexAt(m->position().toPoint());
		if (m->button() == Qt::LeftButton && index.isValid()) {
			if (star_rect(list_->visualRect(index)).contains(m->position().toPoint()))
				toggle_favourite(index);
			else
				choose(index);
			return true;
		}
	}
	return RoundedPopup::eventFilter(obj, event);
}

void FontBrowser::resizeEvent(QResizeEvent *event)
{
	RoundedPopup::resizeEvent(event);
	if (isVisible()) {
		Prefs &prefs = Prefs::get();
		prefs.width = width();
		prefs.height = height();
	}
}

void FontBrowser::hideEvent(QHideEvent *event)
{
	RoundedPopup::hideEvent(event);
	Prefs::get().save();
	if (closed_) {
		auto closed = std::move(closed_);
		closed_ = nullptr;
		closed();
	}
}

} // namespace

void open_font_browser(QWidget *parent, const QRect &anchor, const QRect &avoid, const FontRequest &request,
		       const std::string &search, std::function<void(const std::string &)> chosen,
		       std::function<void()> closed)
{
	if (open_browser)
		open_browser->close();
	auto *b = new FontBrowser(parent, request, std::move(chosen), std::move(closed));
	open_browser = b;
	b->start(anchor, avoid, search);
	if (debug) {
		const QRect g = b->geometry();
		const QPoint feel = b->feel_button();
		blog(LOG_INFO, "[obs-typeset] fonts open x=%d y=%d w=%d h=%d feel=%d,%d", g.x(), g.y(), g.width(),
		     g.height(), feel.x(), feel.y());
	}
}

std::vector<std::string> fonts_in_use()
{
	std::set<std::string> found;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			if (TextSource *t = TextSource::from(source)) {
				auto *set = static_cast<std::set<std::string> *>(param);
				const Document d = t->document();
				if (!d.style.family.empty())
					set->insert(d.style.family);
				for (const Span &s : d.spans)
					if (!s.family.empty())
						set->insert(s.family);
			}
			return true;
		},
		&found);
	return {found.begin(), found.end()};
}

static void browse_fonts_now(obs_source_t *source)
{
	TextSource *text = TextSource::from(source);
	if (!text)
		return;
	const Document d = text->document();
	FontRequest req;
	req.family = d.style.family;
	req.weight = base_weight(d.style);
	req.italic = d.style.face_style.find("Italic") != std::string::npos ||
		     d.style.face_style.find("Oblique") != std::string::npos || (d.style.font_flags & OBS_FONT_ITALIC);
	req.sample = d.text;
	req.in_use = fonts_in_use();
	const QPoint at = QCursor::pos();
	obs_weak_source_t *weak = obs_source_get_weak_source(source);
	open_font_browser(
		QApplication::activeWindow(), QRect(at, QSize(1, 1)), QRect(), req, std::string(),
		[weak, req_weight = req.weight, req_italic = req.italic](const std::string &family) {
			obs_source_t *s = obs_weak_source_get_source(weak);
			if (!s)
				return;
			obs_data_t *settings = obs_source_get_settings(s);
			obs_data_t *font = obs_data_get_obj(settings, "font");
			obs_data_t *changed = obs_data_create();
			obs_data_t *new_font = obs_data_create();
			if (font)
				obs_data_apply(new_font, font);
			obs_data_set_string(new_font, "face", family.c_str());
			// The new family's own style nearest to the text's ("Book" is
			// DejaVu's, not Oswald's): "Regular", "Bold Italic"...
			// And OBS's bold and italic flags to match it: the old ones
			// would otherwise ask for a slant or weight it hasn't got.
			if (const FamilyInfo *f = FontIndex::get().find(family)) {
				const int w = f->nearest_weight(req_weight);
				const std::string weight = weight_label(w);
				const bool italic = req_italic && f->italic;
				std::string style = weight;
				if (italic)
					style = weight == "Regular" ? "Italic" : weight + " Italic";
				obs_data_set_string(new_font, "style", style.c_str());
				const long long others = obs_data_get_int(new_font, "flags") & ~(OBS_FONT_BOLD | OBS_FONT_ITALIC);
				obs_data_set_int(new_font, "flags",
						 others | (w >= 600 ? OBS_FONT_BOLD : 0) | (italic ? OBS_FONT_ITALIC : 0));
			}
			obs_data_set_obj(changed, "font", new_font);
			obs_source_update(s, changed);
			obs_source_update_properties(s); // the dialog shows it
			obs_data_release(new_font);
			obs_data_release(changed);
			obs_data_release(font);
			obs_data_release(settings);
			obs_source_release(s);
		},
		[weak] { obs_weak_source_release(weak); });
}

// A properties button is pressed on whichever thread asks (obs-websocket's,
// a controller's), and widgets are made only on the UI thread: opened there,
// once the press has returned.
void browse_fonts_for(obs_source_t *source)
{
	obs_weak_source_t *weak = obs_source_get_weak_source(source);
	QMetaObject::invokeMethod(
		qApp,
		[weak] {
			obs_source_t *s = obs_weak_source_get_source(weak);
			obs_weak_source_release(weak);
			if (s) {
				browse_fonts_now(s);
				obs_source_release(s);
			}
		},
		Qt::QueuedConnection);
}

} // namespace typeset
