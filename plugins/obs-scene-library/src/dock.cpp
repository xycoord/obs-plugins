#include "dock.hpp"
#include "ui-thread.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <algorithm>

#include <QDateTime>
#include <QLocale>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace library {

namespace {

constexpr QSize ICON_SIZE(160, 90);
// Each saved scene: its thumbnail, and its name in up to two lines under it.
constexpr QSize CELL_SIZE(ICON_SIZE.width() + 16, ICON_SIZE.height() + 56);
constexpr int NAME_ROLE = Qt::UserRole;
constexpr int SAVED_ROLE = Qt::UserRole + 1;
constexpr int SCENE_ROLE = Qt::UserRole + 2;

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString Q(const std::string &s)
{
	return QString::fromStdString(s);
}

QString join(const std::vector<std::string> &list)
{
	QStringList l;
	for (auto &s : list)
		l << Q(s);
	return l.join(", ");
}

// The thumbnail fitted into the icon's box, over black.
QIcon thumbnail_icon(const fs::path &file)
{
	QPixmap box(ICON_SIZE);
	box.fill(Qt::black);
	QPixmap thumb(QString::fromStdString(file.string()));
	if (!thumb.isNull()) {
		QPixmap scaled = thumb.scaled(ICON_SIZE, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		QPainter p(&box);
		p.drawPixmap((ICON_SIZE.width() - scaled.width()) / 2, (ICON_SIZE.height() - scaled.height()) / 2,
			     scaled);
	}
	return QIcon(box);
}

} // namespace

Dock::Dock(QWidget *parent) : QWidget(parent)
{
	search = new QLineEdit(this);
	search->setPlaceholderText(T("Search"));
	search->setClearButtonEnabled(true);
	more = new QToolButton(this);
	more->setText(QString::fromUtf8("⋯"));
	more->setToolTip(T("More"));
	auto *top = new QHBoxLayout;
	top->setContentsMargins(0, 0, 0, 0);
	top->addWidget(search, 1);
	top->addWidget(more);

	list = new QListWidget(this);
	list->setViewMode(QListView::IconMode);
	list->setIconSize(ICON_SIZE);
	list->setGridSize(CELL_SIZE);
	list->setResizeMode(QListView::Adjust);
	list->setMovement(QListView::Static);
	list->setWordWrap(true);
	list->setSelectionMode(QAbstractItemView::SingleSelection);
	list->setContextMenuPolicy(Qt::CustomContextMenu);
	list->setTextElideMode(Qt::ElideRight);

	empty = new QLabel(T("Empty"), this);
	empty->setWordWrap(true);
	empty->setAlignment(Qt::AlignCenter);
	empty->setMargin(12);

	status = new QLabel(this);
	status->setWordWrap(true);
	status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	status->hide();
	progress = new QProgressBar(this);
	progress->setRange(0, 100);
	progress->setTextVisible(false);
	progress->setMaximumHeight(8);
	cancel = new QToolButton(this);
	cancel->setText(T("Cancel"));
	auto *progress_row = new QHBoxLayout;
	progress_row->setContentsMargins(0, 0, 0, 0);
	progress_row->addWidget(progress, 1);
	progress_row->addWidget(cancel);
	progress->hide();
	cancel->hide();

	save_button = new QPushButton(T("SaveScene"), this);
	save_button->setToolTip(T("SaveScene.Tooltip"));
	add_button = new QPushButton(T("AddToScenes"), this);
	add_button->setToolTip(T("AddToScenes.Tooltip"));
	auto *buttons = new QHBoxLayout;
	buttons->setContentsMargins(0, 0, 0, 0);
	buttons->addWidget(save_button, 1);
	buttons->addWidget(add_button, 1);

	// Room for a column of thumbnails.
	setMinimumWidth(ICON_SIZE.width() + 48);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->addLayout(top);
	layout->addWidget(list, 1);
	layout->addWidget(empty, 1);
	layout->addWidget(status);
	layout->addLayout(progress_row);
	layout->addLayout(buttons);

	connect(save_button, &QPushButton::clicked, this, [this] { save_scene(); });
	connect(add_button, &QPushButton::clicked, this, [this] {
		if (!selected().empty())
			add_to_scenes(selected());
	});
	connect(list, &QListWidget::itemActivated, this,
		[this](QListWidgetItem *item) { add_to_scenes(item->data(NAME_ROLE).toString().toStdString()); });
	connect(list, &QListWidget::itemSelectionChanged, this, [this] { update_buttons(); });
	connect(list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) { show_entry_menu(pos); });
	connect(search, &QLineEdit::textChanged, this, [this] { apply_search(); });
	connect(more, &QToolButton::clicked, this, [this] { show_more_menu(); });
	connect(cancel, &QToolButton::clicked, this, [this] { stop = true; });

	auto *del = new QShortcut(QKeySequence::Delete, list, nullptr, nullptr, Qt::WidgetShortcut);
	connect(del, &QShortcut::activated, this, [this] {
		if (!selected().empty())
			delete_entry(selected());
	});
	auto *f2 = new QShortcut(QKeySequence(Qt::Key_F2), list, nullptr, nullptr, Qt::WidgetShortcut);
	connect(f2, &QShortcut::activated, this, [this] {
		if (!selected().empty())
			rename_entry(selected());
	});

	// The folder can change from elsewhere (obs-websocket, or by hand): a moment
	// after it does, the list is read again.
	refresh_timer.setSingleShot(true);
	refresh_timer.setInterval(250);
	connect(&refresh_timer, &QTimer::timeout, this, [this] { refresh(); });
	connect(&watcher, &QFileSystemWatcher::directoryChanged, this, [this] { refresh_timer.start(); });

	watch_folder();
	refresh();
}

Dock::~Dock()
{
	shutdown();
}

void Dock::shutdown()
{
	stop = true;
	if (worker.joinable())
		worker.join();
}

void Dock::watch_folder()
{
	if (!watcher.directories().isEmpty())
		watcher.removePaths(watcher.directories());
	std::error_code ec;
	fs::create_directories(folder(), ec);
	watcher.addPath(QString::fromStdString(folder().string()));
}

std::string Dock::selected() const
{
	auto items = list->selectedItems();
	return items.isEmpty() ? std::string() : items.first()->data(NAME_ROLE).toString().toStdString();
}

void Dock::refresh()
{
	std::string keep = select_after_refresh.empty() ? selected() : select_after_refresh;
	select_after_refresh.clear();

	auto all = entries();
	if (sort_order() == "saved") {
		std::sort(all.begin(), all.end(), [](const Entry &a, const Entry &b) { return a.saved > b.saved; });
	} else {
		std::sort(all.begin(), all.end(), [](const Entry &a, const Entry &b) {
			return QString::localeAwareCompare(Q(a.name), Q(b.name)) < 0;
		});
	}

	list->clear();
	for (auto &e : all) {
		auto *item = new QListWidgetItem(thumbnail_icon(e.thumbnail()), Q(e.name));
		item->setSizeHint(CELL_SIZE - QSize(4, 4));
		item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
		item->setData(NAME_ROLE, Q(e.name));
		item->setData(SAVED_ROLE, (qlonglong)e.saved);
		item->setData(SCENE_ROLE, Q(e.scene));
		QString when = QLocale().toString(QDateTime::fromSecsSinceEpoch(e.saved), QLocale::ShortFormat);
		QString tip = "<b>" + Q(e.name).toHtmlEscaped() + "</b><br>" +
			      T("Tooltip.Saved").arg(when, Q(e.scene).toHtmlEscaped()) + "<br>" +
			      T("Tooltip.Counts").arg(e.sources).arg(e.files);
		if (!e.not_copied.empty())
			tip += "<br>" + T("Tooltip.NotCopied").arg(join(e.not_copied).toHtmlEscaped());
		item->setToolTip(tip);
		list->addItem(item);
		if (e.name == keep) {
			item->setSelected(true);
			list->setCurrentItem(item);
		}
	}
	apply_search();
	if (QListWidgetItem *current = list->currentItem())
		list->scrollToItem(current);
	update_buttons();
}

void Dock::apply_search()
{
	QString text = search->text().trimmed();
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *item = list->item(i);
		bool match = text.isEmpty() || item->data(NAME_ROLE).toString().contains(text, Qt::CaseInsensitive) ||
			     item->data(SCENE_ROLE).toString().contains(text, Qt::CaseInsensitive);
		item->setHidden(!match);
	}
	bool none = list->count() == 0;
	list->setVisible(!none);
	empty->setVisible(none);
}

void Dock::update_buttons()
{
	add_button->setEnabled(!selected().empty());
	save_button->setEnabled(!saving);
}

void Dock::message(const QString &text, bool warning)
{
	status->setText(text);
	status->setStyleSheet(warning ? "color: #e5a03c;" : "");
	status->setVisible(!text.isEmpty());
}

// --- Saving ------------------------------------------------------------------

void Dock::save_scene(const std::string &replacing)
{
	if (saving)
		return;
	OBSSourceAutoRelease scene = edited_scene();
	if (!scene) {
		message(T("NoScene"), true);
		return;
	}
	QString scene_name = QString::fromUtf8(obs_source_get_name(scene));
	std::string name = replacing;
	// Replacing a saved scene only when that's been asked for: one of the same
	// name saved meanwhile (through obs-websocket) isn't overwritten.
	bool replace = !replacing.empty();
	if (name.empty()) {
		bool ok = false;
		QString answer = QInputDialog::getText(this, T("SaveDialog.Title"),
						       T("SaveDialog.Text").arg(scene_name), QLineEdit::Normal,
						       scene_name, &ok)
					 .trimmed();
		if (!ok || answer.isEmpty())
			return;
		name = answer.toStdString();
		if (library::find(name)) {
			if (QMessageBox::question(this, T("Replace.Title"), T("Replace.Text").arg(answer)) !=
			    QMessageBox::Yes)
				return;
			replace = true;
		}
	} else if (QMessageBox::question(this, T("Replace.Title"),
					 T("ReplaceWith.Text").arg(Q(replacing), scene_name)) != QMessageBox::Yes) {
		return;
	}
	// The scene may have gone while the dialog was open.
	OBSSourceAutoRelease still = obs_get_source_by_name(scene_name.toUtf8().constData());
	if (still != scene) {
		message(T("NoScene"), true);
		return;
	}
	start_save(snapshot(scene, name), replace);
}

void Dock::start_save(Snapshot snap, bool replace)
{
	if (worker.joinable())
		worker.join();
	saving = true;
	stop = false;
	saving_name = Q(snap.name);
	update_buttons();
	progress->setValue(0);
	progress->show();
	cancel->show();
	message(T("Saving").arg(saving_name).arg(0));

	worker = std::thread([this, replace, snap = std::move(snap)]() mutable {
		QString name = Q(snap.name);
		int last = -1;
		Saved saved = write(std::move(snap), replace, [&](uint64_t done, uint64_t total) {
			int percent = total ? (int)(done * 100 / total) : 100;
			if (percent != last) {
				last = percent;
				QMetaObject::invokeMethod(this, [this, percent] { save_progress(percent); },
							  Qt::QueuedConnection);
			}
			return !stop.load();
		});
		QMetaObject::invokeMethod(this, [this, saved, name] { save_finished(saved, name); },
					  Qt::QueuedConnection);
	});
}

void Dock::save_progress(int percent)
{
	if (!saving)
		return;
	progress->setValue(percent);
	message(T("Saving").arg(saving_name).arg(percent));
}

void Dock::save_finished(const Saved &saved, const QString &name)
{
	saving = false;
	progress->hide();
	cancel->hide();
	update_buttons();
	if (saved.ok) {
		QString text = T("Saved").arg(name);
		if (!saved.entry.not_copied.empty())
			text += "\n" + T("Tooltip.NotCopied").arg(join(saved.entry.not_copied));
		message(text, !saved.entry.not_copied.empty());
		select_after_refresh = saved.entry.name;
	} else if (saved.error == "cancelled") {
		message(T("Cancelled").arg(name));
	} else {
		message(T("SaveFailed").arg(name, Q(saved.error)), true);
	}
	refresh();
}

// --- Adding to the scenes ----------------------------------------------------

void Dock::add_to_scenes(const std::string &name)
{
	auto entry = library::find(name);
	if (!entry) {
		refresh();
		return;
	}
	Loaded loaded = load(*entry);
	if (!loaded.ok) {
		message(T("AddFailed").arg(Q(name), Q(loaded.error)), true);
		return;
	}
	QStringList notes;
	notes << (obs_frontend_preview_program_mode_active() ? T("AddedPreview") : T("Added")).arg(Q(loaded.scene));
	if (!loaded.reused.empty())
		notes << T("Reused").arg(join(loaded.reused));
	if (loaded.other_canvas) {
		obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		notes << T("OtherCanvas").arg(entry->width).arg(entry->height).arg(ovi.base_width).arg(ovi.base_height);
	}
	QStringList problems;
	if (!loaded.missing_types.empty())
		problems << T("MissingTypes").arg(join(loaded.missing_types));
	if (!loaded.missing_files.empty())
		problems << T("MissingFiles").arg(join(loaded.missing_files));
	message((notes + problems).join("\n"), loaded.other_canvas || !problems.isEmpty());
	if (!problems.isEmpty())
		QMessageBox::warning(this, T("SceneLibrary"), (notes + problems).join("\n\n"));
}

// --- Managing ----------------------------------------------------------------

void Dock::rename_entry(const std::string &name)
{
	auto entry = library::find(name);
	if (!entry)
		return refresh();
	bool ok = false;
	QString answer =
		QInputDialog::getText(this, T("Rename.Title"), T("Rename.Text"), QLineEdit::Normal, Q(name), &ok)
			.trimmed();
	if (!ok || answer.isEmpty() || answer.toStdString() == name)
		return;
	std::string error;
	if (!library::rename(*entry, answer.toStdString(), error))
		message(T("RenameFailed").arg(Q(name), Q(error)), true);
	else
		select_after_refresh = answer.toStdString();
	refresh();
}

void Dock::delete_entry(const std::string &name)
{
	auto entry = library::find(name);
	if (!entry)
		return refresh();
	if (QMessageBox::question(this, T("Delete.Title"), T("Delete.Text").arg(Q(name))) != QMessageBox::Yes)
		return;
	std::string error;
	if (!library::remove(*entry, error))
		message(T("DeleteFailed").arg(Q(name), Q(error)), true);
	else
		message(T("Deleted").arg(Q(name)));
	refresh();
}

void Dock::show_entry_menu(const QPoint &pos)
{
	QListWidgetItem *item = list->itemAt(pos);
	if (!item)
		return;
	item->setSelected(true);
	std::string name = item->data(NAME_ROLE).toString().toStdString();
	OBSSourceAutoRelease scene = edited_scene();

	QMenu menu(this);
	menu.addAction(T("AddToScenes"), this, [this, name] { add_to_scenes(name); });
	QAction *replace = menu.addAction(
		T("Menu.Replace").arg(scene ? QString::fromUtf8(obs_source_get_name(scene)) : QString()), this,
		[this, name] { save_scene(name); });
	replace->setEnabled(scene && !saving);
	menu.addAction(T("Menu.Rename"), this, [this, name] { rename_entry(name); });
	menu.addAction(T("Menu.Delete"), this, [this, name] { delete_entry(name); });
	menu.addSeparator();
	menu.addAction(T("Menu.ShowFiles"), this, [name] {
		if (auto e = library::find(name))
			QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(e->dir.string())));
	});
	menu.exec(list->viewport()->mapToGlobal(pos));
}

void Dock::show_more_menu()
{
	QMenu menu(this);
	QAction *by_name = menu.addAction(T("SortName"), this, [this] {
		set_sort_order("name");
		refresh();
	});
	QAction *by_date = menu.addAction(T("SortSaved"), this, [this] {
		set_sort_order("saved");
		refresh();
	});
	by_name->setCheckable(true);
	by_date->setCheckable(true);
	(sort_order() == "saved" ? by_date : by_name)->setChecked(true);
	menu.addSeparator();
	menu.addAction(T("ShowFolder"), this, [] {
		std::error_code ec;
		fs::create_directories(folder(), ec);
		QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder().string())));
	});
	menu.addAction(T("ChangeFolder"), this, [this] { change_folder(); });
	menu.exec(more->mapToGlobal(QPoint(0, more->height())));
}

void Dock::change_folder()
{
	fs::path old = folder();
	QString dir = QFileDialog::getExistingDirectory(this, T("ChangeFolder.Title"), QString::fromStdString(old.string()));
	if (dir.isEmpty() || fs::path(dir.toStdString()) == old)
		return;
	set_folder(dir.toStdString());
	watch_folder();
	refresh();
	// The scenes saved in the old one aren't moved.
	std::error_code ec;
	if (fs::exists(old, ec) && !fs::is_empty(old, ec))
		QMessageBox::information(this, T("SceneLibrary"),
					 T("FolderChanged").arg(dir, QString::fromStdString(old.string())));
}

} // namespace library
