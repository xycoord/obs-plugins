// The library through obs-websocket, for a Stream Deck, Companion or a script,
// as vendor requests of the vendor "scene-library" (CallVendorRequest):
//
//   ListScenes                          -> {scenes: [{name, scene, saved, sources, files, notCopied}]}
//   SaveScene {sceneName?, name?, replace?} -> {ok, name}   (default: the scene being edited, under its own name)
//   LoadScene {name}                    -> {ok, sceneName, missingTypes, missingFiles, reused, otherCanvas}
//   RenameScene {name, newName}         -> {ok}
//   DeleteScene {name}                  -> {ok}
//
// Each answers {ok: false, error} when it can't. (Registering is done through
// obs-websocket's procedures, as its obs-websocket-api.h does.)
#include "websocket.hpp"
#include "library.hpp"
#include "ui-thread.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace library {

namespace {

using RequestFn = void (*)(obs_data_t *request, obs_data_t *response, void *priv);

struct RequestCallback {
	RequestFn callback;
	void *priv_data;
};

proc_handler_t *websocket_ph()
{
	calldata_t cd = {};
	proc_handler_t *ph = nullptr;
	if (proc_handler_call(obs_get_proc_handler(), "obs_websocket_api_get_ph", &cd))
		ph = static_cast<proc_handler_t *>(calldata_ptr(&cd, "ph"));
	calldata_free(&cd);
	return ph;
}

void fail(obs_data_t *response, const std::string &error)
{
	obs_data_set_bool(response, "ok", false);
	obs_data_set_string(response, "error", error.c_str());
}

void list_scenes(obs_data_t *, obs_data_t *response, void *)
{
	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (auto &e : entries()) {
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_string(d, "name", e.name.c_str());
		obs_data_set_string(d, "scene", e.scene.c_str());
		obs_data_set_int(d, "saved", e.saved);
		obs_data_set_int(d, "sources", e.sources);
		obs_data_set_int(d, "files", e.files);
		obs_data_set_string(d, "folder", e.dir.c_str());
		OBSDataArrayAutoRelease not_copied = obs_data_array_create();
		for (auto &f : e.not_copied) {
			OBSDataAutoRelease s = obs_data_create();
			obs_data_set_string(s, "file", f.c_str());
			obs_data_array_push_back(not_copied, s);
		}
		obs_data_set_array(d, "notCopied", not_copied);
		obs_data_array_push_back(array, d);
	}
	obs_data_set_bool(response, "ok", true);
	obs_data_set_array(response, "scenes", array);
}

void save_scene(obs_data_t *request, obs_data_t *response, void *)
{
	std::string scene_name = obs_data_get_string(request, "sceneName");
	std::string name = obs_data_get_string(request, "name");
	bool replace = obs_data_get_bool(request, "replace");
	std::optional<Snapshot> snap;
	std::string error;
	run_on_ui_thread([&] {
		OBSSourceAutoRelease scene = scene_name.empty() ? edited_scene()
								 : obs_get_source_by_name(scene_name.c_str());
		if (!scene || !obs_scene_from_source(scene)) {
			error = scene_name.empty() ? "there's no scene" : "there's no scene called '" + scene_name + "'";
			return;
		}
		if (name.empty())
			name = obs_source_get_name(scene);
		if (!replace && find(name)) {
			error = "there's already a saved scene called '" + name + "' (replace: true replaces it)";
			return;
		}
		snap = snapshot(scene, name);
	});
	if (!snap)
		return fail(response, error);
	// The files are copied here, not on OBS's UI thread.
	Saved saved = write(std::move(*snap));
	if (!saved.ok)
		return fail(response, saved.error);
	notify_changed();
	obs_data_set_bool(response, "ok", true);
	obs_data_set_string(response, "name", saved.entry.name.c_str());
}

void strings(obs_data_t *response, const char *key, const std::vector<std::string> &list)
{
	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (auto &s : list) {
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_string(d, "name", s.c_str());
		obs_data_array_push_back(array, d);
	}
	obs_data_set_array(response, key, array);
}

void load_scene(obs_data_t *request, obs_data_t *response, void *)
{
	std::string name = obs_data_get_string(request, "name");
	auto entry = find(name);
	if (!entry)
		return fail(response, "there's no saved scene called '" + name + "'");
	Loaded loaded;
	run_on_ui_thread([&] { loaded = load(*entry); });
	if (!loaded.ok)
		return fail(response, loaded.error);
	obs_data_set_bool(response, "ok", true);
	obs_data_set_string(response, "sceneName", loaded.scene.c_str());
	strings(response, "missingTypes", loaded.missing_types);
	strings(response, "missingFiles", loaded.missing_files);
	strings(response, "reused", loaded.reused);
	obs_data_set_bool(response, "otherCanvas", loaded.other_canvas);
}

void rename_scene(obs_data_t *request, obs_data_t *response, void *)
{
	std::string name = obs_data_get_string(request, "name");
	auto entry = find(name);
	if (!entry)
		return fail(response, "there's no saved scene called '" + name + "'");
	std::string error;
	if (!rename(*entry, obs_data_get_string(request, "newName"), error))
		return fail(response, error);
	notify_changed();
	obs_data_set_bool(response, "ok", true);
}

void delete_scene(obs_data_t *request, obs_data_t *response, void *)
{
	std::string name = obs_data_get_string(request, "name");
	auto entry = find(name);
	if (!entry)
		return fail(response, "there's no saved scene called '" + name + "'");
	std::string error;
	if (!remove(*entry, error))
		return fail(response, error);
	notify_changed();
	obs_data_set_bool(response, "ok", true);
}

} // namespace

void register_websocket_requests()
{
	proc_handler_t *ph = websocket_ph();
	if (!ph) {
		blog(LOG_INFO, "[scene-library] obs-websocket isn't loaded: no requests through it");
		return;
	}
	calldata_t cd = {};
	calldata_set_string(&cd, "name", "scene-library");
	proc_handler_call(ph, "vendor_register", &cd);
	void *vendor = calldata_ptr(&cd, "vendor");
	calldata_free(&cd);
	if (!vendor) {
		blog(LOG_WARNING, "[scene-library] couldn't register with obs-websocket");
		return;
	}
	const std::pair<const char *, RequestFn> requests[] = {
		{"ListScenes", list_scenes},   {"SaveScene", save_scene},     {"LoadScene", load_scene},
		{"RenameScene", rename_scene}, {"DeleteScene", delete_scene},
	};
	for (auto &[type, fn] : requests) {
		RequestCallback cb = {fn, nullptr};
		calldata_t r = {};
		calldata_set_ptr(&r, "vendor", vendor);
		calldata_set_string(&r, "type", type);
		calldata_set_ptr(&r, "callback", &cb);
		proc_handler_call(ph, "vendor_request_register", &r);
		if (!calldata_bool(&r, "success"))
			blog(LOG_WARNING, "[scene-library] couldn't register the request %s with obs-websocket", type);
		calldata_free(&r);
	}
}

} // namespace library
