#include "snapshot.hpp"

#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

namespace library {

void rewrite_strings(obs_data_t *settings, const StringFn &fn)
{
	if (!settings)
		return;
	// Changed after the walk: setting a string can move the item being walked.
	std::vector<std::pair<std::string, std::string>> changes;
	for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
		if (!obs_data_item_has_user_value(item))
			continue;
		const char *key = obs_data_item_get_name(item);
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_STRING:
			if (const std::string *to = fn(key, obs_data_item_get_string(item)))
				changes.emplace_back(key, *to);
			break;
		case OBS_DATA_OBJECT: {
			OBSDataAutoRelease obj = obs_data_item_get_obj(item);
			rewrite_strings(obj, fn);
			break;
		}
		case OBS_DATA_ARRAY: {
			OBSDataArrayAutoRelease array = obs_data_item_get_array(item);
			for (size_t i = 0, n = obs_data_array_count(array); i < n; i++) {
				OBSDataAutoRelease obj = obs_data_array_item(array, i);
				rewrite_strings(obj, fn);
			}
			break;
		}
		default:
			break;
		}
	}
	for (auto &[key, value] : changes)
		obs_data_set_string(settings, key.c_str(), value.c_str());
}

// (A scene's settings hold its items, and their show and hide transitions'
// settings with them.)
void for_each_settings(obs_data_t *source, const std::function<void(obs_data_t *)> &fn)
{
	OBSDataAutoRelease settings = obs_data_get_obj(source, "settings");
	if (settings)
		fn(settings);
	OBSDataArrayAutoRelease filters = obs_data_get_array(source, "filters");
	for (size_t i = 0, n = obs_data_array_count(filters); i < n; i++) {
		OBSDataAutoRelease filter = obs_data_array_item(filters, i);
		OBSDataAutoRelease filter_settings = obs_data_get_obj(filter, "settings");
		if (filter_settings)
			fn(filter_settings);
	}
}

bool looks_like_path(const char *s)
{
	if (!s)
		return false;
	std::string_view v(s);
	if (v.size() < 2 || v.size() > 4096 || v.find('\n') != v.npos)
		return false;
	if (v[0] == '/')
		return true;
	// C:\ or C:/
	bool letter = (v[0] >= 'A' && v[0] <= 'Z') || (v[0] >= 'a' && v[0] <= 'z');
	return v.size() >= 3 && letter && v[1] == ':' && (v[2] == '\\' || v[2] == '/');
}

std::vector<std::string> referenced_paths(obs_data_array_t *sources)
{
	std::vector<std::string> paths;
	std::set<std::string> seen;
	for (size_t i = 0, n = obs_data_array_count(sources); i < n; i++) {
		OBSDataAutoRelease source = obs_data_array_item(sources, i);
		for_each_settings(source, [&](obs_data_t *settings) {
			rewrite_strings(settings, [&](const char *, const char *value) -> const std::string * {
				if (!looks_like_path(value) || seen.count(value))
					return nullptr;
				std::error_code ec;
				auto status = fs::status(fs::path(value), ec);
				if (!ec && (fs::is_regular_file(status) || fs::is_directory(status))) {
					seen.insert(value);
					paths.emplace_back(value);
				}
				return nullptr;
			});
		});
	}
	return paths;
}

static std::string versioned_id(obs_data_t *data)
{
	const char *v = obs_data_get_string(data, "versioned_id");
	return *v ? v : obs_data_get_string(data, "id");
}

static bool is_scene_type(const std::string &id)
{
	return id == "scene" || id == "group";
}

std::set<std::string> source_types(obs_data_array_t *sources)
{
	std::set<std::string> types;
	for (size_t i = 0, n = obs_data_array_count(sources); i < n; i++) {
		OBSDataAutoRelease source = obs_data_array_item(sources, i);
		types.insert(versioned_id(source));
		OBSDataArrayAutoRelease filters = obs_data_get_array(source, "filters");
		for (size_t j = 0, m = obs_data_array_count(filters); j < m; j++) {
			OBSDataAutoRelease filter = obs_data_array_item(filters, j);
			types.insert(versioned_id(filter));
		}
		if (!is_scene_type(obs_data_get_string(source, "id")))
			continue;
		OBSDataAutoRelease settings = obs_data_get_obj(source, "settings");
		OBSDataArrayAutoRelease items = obs_data_get_array(settings, "items");
		for (size_t j = 0, m = obs_data_array_count(items); j < m; j++) {
			OBSDataAutoRelease item = obs_data_array_item(items, j);
			for (const char *which : {"show_transition", "hide_transition"}) {
				OBSDataAutoRelease transition = obs_data_get_obj(item, which);
				if (transition && *obs_data_get_string(transition, "id"))
					types.insert(versioned_id(transition));
			}
		}
	}
	types.erase("");
	return types;
}

// Keys whose strings are text to show or a font, never a source's name, even
// when they read the same as one.
static bool holds_text(const char *key)
{
	static const std::set<std::string> keys = {"text", "face", "family", "style"};
	return keys.count(key) > 0;
}

Prepared prepare_for_load(obs_data_array_t *saved, const std::string &root_saved_name, const std::string &root_name,
			  const Existing &existing, const std::map<std::string, std::string> &paths)
{
	Prepared out;
	out.sources = obs_data_array_create();
	size_t count = obs_data_array_count(saved);

	std::set<std::string> saved_names;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease source = obs_data_array_item(saved, i);
		saved_names.insert(obs_data_get_string(source, "name"));
	}

	// Names: each source keeps its own if it's free; a name another saved
	// source has is left for that one.
	std::set<std::string> assigned;
	auto unique_name = [&](const std::string &base, const std::string &own) {
		std::string candidate = base;
		for (int n = 2;; n++) {
			bool free = !existing.name_taken(candidate) && !assigned.count(candidate) &&
				    (candidate == own || !saved_names.count(candidate));
			if (free)
				break;
			candidate = base + " " + std::to_string(n);
		}
		assigned.insert(candidate);
		return candidate;
	};

	std::map<std::string, std::string> uuids; // saved uuid -> uuid once loaded
	std::vector<bool> reuse(count, false);
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease source = obs_data_array_item(saved, i);
		std::string name = obs_data_get_string(source, "name");
		std::string uuid = obs_data_get_string(source, "uuid");
		if (name == root_saved_name) {
			out.root = out.names[name] = unique_name(root_name, name);
		} else if (existing.captures(versioned_id(source))) {
			std::string device = existing.device(name, obs_data_get_string(source, "id"));
			if (!device.empty()) {
				reuse[i] = true;
				out.names[name] = name;
				if (!uuid.empty())
					uuids[uuid] = device;
				out.reused.push_back(name);
				continue;
			}
			out.names[name] = unique_name(name, name);
		} else {
			out.names[name] = unique_name(name, name);
		}
		if (!uuid.empty())
			uuids[uuid] = existing.new_uuid();
		OBSDataArrayAutoRelease filters = obs_data_get_array(source, "filters");
		for (size_t j = 0, m = obs_data_array_count(filters); j < m; j++) {
			OBSDataAutoRelease filter = obs_data_array_item(filters, j);
			std::string filter_uuid = obs_data_get_string(filter, "uuid");
			if (!filter_uuid.empty())
				uuids[filter_uuid] = existing.new_uuid();
		}
	}

	std::map<std::string, std::string> renamed;
	for (auto &[from, to] : out.names)
		if (from != to)
			renamed[from] = to;

	auto rewrite = [&](const char *key, const char *value) -> const std::string * {
		if (auto u = uuids.find(value); u != uuids.end())
			return &u->second;
		if (!holds_text(key))
			if (auto r = renamed.find(value); r != renamed.end())
				return &r->second;
		if (auto p = paths.find(value); p != paths.end() && p->first != p->second)
			return &p->second;
		return nullptr;
	};

	for (size_t i = 0; i < count; i++) {
		if (reuse[i])
			continue;
		OBSDataAutoRelease saved_source = obs_data_array_item(saved, i);
		// A copy: what was read from the library is left as it was.
		OBSDataAutoRelease source = obs_data_create_from_json(obs_data_get_json(saved_source));
		std::string name = obs_data_get_string(source, "name");
		obs_data_set_string(source, "name", out.names[name].c_str());
		if (auto u = uuids.find(obs_data_get_string(source, "uuid")); u != uuids.end())
			obs_data_set_string(source, "uuid", u->second.c_str());
		obs_data_erase(source, "hotkeys");
		// (From OBS 31: loaded into the main canvas.)
		obs_data_erase(source, "canvas_uuid");

		OBSDataArrayAutoRelease filters = obs_data_get_array(source, "filters");
		for (size_t j = 0, m = obs_data_array_count(filters); j < m; j++) {
			OBSDataAutoRelease filter = obs_data_array_item(filters, j);
			if (auto u = uuids.find(obs_data_get_string(filter, "uuid")); u != uuids.end())
				obs_data_set_string(filter, "uuid", u->second.c_str());
			obs_data_erase(filter, "hotkeys");
		}
		for_each_settings(source, [&](obs_data_t *settings) { rewrite_strings(settings, rewrite); });
		obs_data_array_push_back(out.sources, source);
	}
	return out;
}

} // namespace library
