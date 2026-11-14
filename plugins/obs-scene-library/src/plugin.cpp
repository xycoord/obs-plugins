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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-scene-library", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Scene Library: save a scene with all its sources, filters and files, and add it back months later.";
}

namespace {

constexpr const char *DOCK_ID = "scene-library";
QPointer<library::Dock> dock; // used on the UI thread only
std::atomic<bool> closing{false};

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
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		introduce_dock();
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		closing = true;
		if (dock)
			dock->shutdown();
		break;
	default:
		break;
	}
}

} // namespace

namespace library {

bool exiting()
{
	return closing.load();
}

bool run_on_ui_thread(const std::function<void()> &fn)
{
	if (!qApp || QThread::currentThread() == qApp->thread()) {
		fn();
		return true;
	}
	// Not a blocking connection: once OBS is closing its event loop may have
	// stopped, and this thread would wait for ever (and obs-websocket, for
	// this thread, as it unloads). So it waits while it can, and gives up if
	// OBS closes before fn has started; fn, which refers to the caller's
	// stack, is then never run.
	struct State {
		std::mutex m;
		std::condition_variable cv;
		bool started = false, done = false, abandoned = false;
	};
	auto state = std::make_shared<State>();
	QMetaObject::invokeMethod(
		qApp,
		[state, &fn] {
			{
				std::lock_guard lock(state->m);
				if (state->abandoned)
					return;
				state->started = true;
			}
			fn();
			std::lock_guard lock(state->m);
			state->done = true;
			state->cv.notify_all();
		},
		Qt::QueuedConnection);
	std::unique_lock lock(state->m);
	while (!state->done) {
		if (closing && !state->started) {
			state->abandoned = true;
			return false;
		}
		state->cv.wait_for(lock, std::chrono::milliseconds(100));
	}
	return true;
}

obs_source_t *edited_scene()
{
	return obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
							  : obs_frontend_get_current_scene();
}

void notify_changed()
{
	if (qApp)
		QMetaObject::invokeMethod(qApp, [] {
			if (dock)
				dock->refresh();
		}, Qt::QueuedConnection);
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
