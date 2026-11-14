#include "dock.hpp"
#include "library.hpp"
#include "ui-thread.hpp"
#include "websocket.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-scene-library", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Scene Library: save a scene with all its sources, filters and files, and add it back months later.";
}

namespace {

constexpr const char *DOCK_ID = "scene-library";
QPointer<library::Dock> dock;

// The first time OBS runs with the plugin, its dock is shown beside the Scenes
// dock (OBS adds it hidden, and floating). From then on it's wherever it was
// left, as OBS remembers.
void introduce_dock()
{
	if (!dock || library::dock_introduced())
		return;
	library::set_dock_introduced();
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *ours = qobject_cast<QDockWidget *>(dock->parentWidget());
	auto *scenes = main ? main->findChild<QDockWidget *>("scenesDock") : nullptr;
	if (!ours)
		return;
	ours->setFloating(false);
	if (scenes && !scenes->isFloating() && scenes->isVisible())
		main->splitDockWidget(scenes, ours, Qt::Horizontal);
	ours->show();
}

void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		introduce_dock();
	else if (event == OBS_FRONTEND_EVENT_EXIT && dock)
		dock->shutdown();
}

} // namespace

namespace library {

void run_on_ui_thread(const std::function<void()> &fn)
{
	if (!qApp || QThread::currentThread() == qApp->thread())
		fn();
	else
		QMetaObject::invokeMethod(qApp, fn, Qt::BlockingQueuedConnection);
}

obs_source_t *edited_scene()
{
	return obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
							  : obs_frontend_get_current_scene();
}

void notify_changed()
{
	QPointer<Dock> d = dock;
	if (d)
		QMetaObject::invokeMethod(d, [d] {
			if (d)
				d->refresh();
		});
}

} // namespace library

bool obs_module_load(void)
{
	library::load_config();
	library::tidy();
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	dock = new library::Dock(main);
	if (!obs_frontend_add_dock_by_id(DOCK_ID, obs_module_text("SceneLibrary"), dock)) {
		blog(LOG_WARNING, "[scene-library] couldn't add its dock");
		delete dock;
	}
	obs_frontend_add_event_callback(frontend_event, nullptr);
	return true;
}

void obs_module_post_load(void)
{
	library::register_websocket_requests();
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	if (dock)
		dock->shutdown();
}
