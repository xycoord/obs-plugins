// The Scene Library dock: the saved scenes, with their thumbnails, to add to
// OBS's scenes; and a button to save the scene being edited.
#pragma once

#include "library.hpp"

#include <QFileSystemWatcher>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <thread>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QToolButton;

namespace library {

class Dock : public QWidget {
public:
	explicit Dock(QWidget *parent = nullptr);
	~Dock() override;

	// Stops a save in progress (OBS is closing).
	void shutdown();
	void refresh();

private:
	void save_scene(const std::string &replacing = {});
	void start_save(Snapshot snap, bool replace);
	void save_progress(int percent);
	void save_finished(const Saved &saved, const QString &name);
	void add_to_scenes(const std::string &name);
	void rename_entry(const std::string &name);
	void delete_entry(const std::string &name);
	void show_entry_menu(const QPoint &pos);
	void show_more_menu();
	void change_folder();
	void apply_search();
	void update_buttons();
	void watch_folder();
	void message(const QString &text, bool warning = false);
	std::string selected() const;

	QLineEdit *search;
	QListWidget *list;
	QLabel *empty;
	QLabel *status;
	QProgressBar *progress;
	QToolButton *cancel;
	QPushButton *save_button;
	QPushButton *add_button;
	QToolButton *more;
	QFileSystemWatcher watcher;
	QTimer refresh_timer;

	std::thread worker;
	std::atomic<bool> stop{false};
	bool saving = false;
	QString saving_name;
	std::string select_after_refresh;
};

} // namespace library
