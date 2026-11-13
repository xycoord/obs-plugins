#include "preview-editor.hpp"
#include "raster-worker.hpp"
#include "typeset-source.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-typeset", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Typeset Text: text that scales like a vector drawing and is edited right in the preview.";
}

static void frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		typeset::editor_install();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		// In Studio Mode that's the program: editing is in the preview.
		if (!obs_frontend_preview_program_mode_active())
			typeset::editor_scene_changed();
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		typeset::editor_scene_changed();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		typeset::editor_remove();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	typeset::worker_start();
	typeset::register_source();
	obs_frontend_add_event_callback(frontend_event, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	typeset::editor_remove();
	typeset::worker_stop();
}
