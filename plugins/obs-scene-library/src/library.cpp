#include "library.hpp"
#include "snapshot.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QPainter>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <set>

namespace library {

namespace {

constexpr int FORMAT = 1;
constexpr int THUMBNAIL_WIDTH = 480;
// A folder a scene uses is copied if it's no bigger than this.
constexpr uint64_t MAX_FOLDER_BYTES = 4ull << 30;
constexpr size_t MAX_FOLDER_FILES = 5000;

std::mutex config_mutex;
fs::path library_dir;
std::string sort_by = "name";
bool introduced = false;

// One save finishes (swaps its folder in) at a time.
std::mutex finish_mutex;

std::string module_config_path(const char *file)
{
	char *p = obs_module_config_path(file);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void save_config()
{
	OBSDataAutoRelease data = obs_data_create();
	{
		std::lock_guard lock(config_mutex);
		obs_data_set_string(data, "library", library_dir.string().c_str());
		obs_data_set_string(data, "sort", sort_by.c_str());
		obs_data_set_bool(data, "dock_introduced", introduced);
	}
	std::string path = module_config_path("config.json");
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak"))
		blog(LOG_WARNING, "[scene-library] couldn't save its settings to %s", path.c_str());
}

std::string random_hex()
{
	char *uuid = os_generate_uuid();
	std::string s = uuid;
	bfree(uuid);
	return s.substr(0, 8);
}

bool inside(const fs::path &path, const fs::path &dir)
{
	std::error_code ec;
	fs::path p = fs::weakly_canonical(path, ec);
	if (ec)
		return false;
	fs::path d = fs::weakly_canonical(dir, ec);
	if (ec)
		return false;
	auto [di, pi] = std::mismatch(d.begin(), d.end(), p.begin(), p.end());
	return di == d.end();
}

// When a file was last changed, in nanoseconds since 1970.
int64_t mtime_of(const fs::path &path)
{
	std::error_code ec;
	auto t = fs::last_write_time(path, ec);
	if (ec)
		return 0;
	auto sys = std::chrono::file_clock::to_sys(t);
	return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count();
}

// --- Copying files -------------------------------------------------------------

struct Copier {
	const Progress &progress;
	uint64_t done = 0;
	uint64_t total = 0;
	bool cancelled = false;

	bool tick()
	{
		if (progress && !progress(done, total))
			cancelled = true;
		return !cancelled;
	}

	// false: it failed, with `error` set, or was cancelled.
	bool copy(const fs::path &from, const fs::path &to, std::string &error)
	{
		FILE *in = fopen(from.c_str(), "rb");
		if (!in) {
			error = "couldn't read " + from.string();
			return false;
		}
		FILE *out = fopen(to.c_str(), "wb");
		if (!out) {
			fclose(in);
			error = "couldn't write " + to.string();
			return false;
		}
		std::vector<char> buf(1 << 20);
		bool ok = true;
		size_t n;
		while ((n = fread(buf.data(), 1, buf.size(), in)) > 0) {
			if (fwrite(buf.data(), 1, n, out) != n) {
				error = "couldn't write " + to.string() + " (is the disk full?)";
				ok = false;
				break;
			}
			done += n;
			if (!tick()) {
				ok = false;
				break;
			}
		}
		if (ok && ferror(in)) {
			error = "couldn't read " + from.string();
			ok = false;
		}
		fclose(in);
		if (fclose(out) != 0 && ok) {
			error = "couldn't write " + to.string() + " (is the disk full?)";
			ok = false;
		}
		return ok;
	}

	// A file already in the library is linked, not copied: it's never changed
	// once there, and this way takes no more space.
	bool link_or_copy(const fs::path &from, const fs::path &to, bool linkable, uint64_t size, std::string &error)
	{
		if (linkable) {
			std::error_code ec;
			fs::create_hard_link(from, to, ec);
			if (!ec) {
				done += size;
				return tick();
			}
		}
		return copy(from, to, error);
	}
};

struct FileRecord {
	std::string path;   // where it was when the scene was saved
	std::string kind;   // "file" or "folder"
	std::string stored; // its copy, relative to the saved scene's folder ("" if not copied)
	std::string skipped; // why it wasn't copied
	int64_t size = 0;
	int64_t mtime = 0;
};

std::vector<FileRecord> read_files(obs_data_t *data)
{
	std::vector<FileRecord> files;
	OBSDataArrayAutoRelease array = obs_data_get_array(data, "files");
	for (size_t i = 0, n = obs_data_array_count(array); i < n; i++) {
		OBSDataAutoRelease f = obs_data_array_item(array, i);
		FileRecord r;
		r.path = obs_data_get_string(f, "path");
		r.kind = obs_data_get_string(f, "kind");
		r.stored = obs_data_get_string(f, "stored");
		r.skipped = obs_data_get_string(f, "skipped");
		r.size = obs_data_get_int(f, "size");
		r.mtime = obs_data_get_int(f, "mtime");
		files.push_back(r);
	}
	return files;
}

std::optional<Entry> read_entry(const fs::path &dir)
{
	fs::path json = dir / "scene.json";
	std::error_code ec;
	if (!fs::is_regular_file(json, ec))
		return std::nullopt;
	OBSDataAutoRelease data = obs_data_create_from_json_file(json.c_str());
	if (!data || !*obs_data_get_string(data, "name"))
		return std::nullopt;
	Entry e;
	e.dir = dir;
	e.name = obs_data_get_string(data, "name");
	e.scene = obs_data_get_string(data, "scene");
	e.saved = obs_data_get_int(data, "saved");
	OBSDataAutoRelease canvas = obs_data_get_obj(data, "canvas");
	e.width = (uint32_t)obs_data_get_int(canvas, "width");
	e.height = (uint32_t)obs_data_get_int(canvas, "height");
	OBSDataArrayAutoRelease sources = obs_data_get_array(data, "sources");
	e.sources = (int)obs_data_array_count(sources);
	for (auto &f : read_files(data)) {
		if (!f.stored.empty())
			e.files++;
		else
			e.not_copied.push_back(f.path + " (" + f.skipped + ")");
	}
	return e;
}

// The scene and everything in it: its items' sources, and those of any scenes
// and groups in it, each once, the scene first.
void collect(obs_source_t *source, std::vector<OBSSource> &out, std::set<obs_source_t *> &seen)
{
	if (!source || seen.count(source))
		return;
	seen.insert(source);
	out.emplace_back(source);
	obs_scene_t *scene = obs_scene_from_source(source);
	if (!scene)
		scene = obs_group_from_source(source);
	if (!scene)
		return;
	std::vector<obs_source_t *> children;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_sceneitem_get_source(item));
			return true;
		},
		&children);
	for (obs_source_t *child : children)
		collect(child, out, seen);
}

fs::path unique_dir(const fs::path &lib, const std::string &base)
{
	std::error_code ec;
	fs::path dir = lib / base;
	for (int n = 2; fs::exists(dir, ec); n++)
		dir = lib / (base + " (" + std::to_string(n) + ")");
	return dir;
}

} // namespace

// --- The folder --------------------------------------------------------------

void load_config()
{
	std::string path = module_config_path("config.json");
	OBSDataAutoRelease data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	std::lock_guard lock(config_mutex);
	const char *dir = data ? obs_data_get_string(data, "library") : "";
	library_dir = *dir ? fs::path(dir) : fs::path(module_config_path("library"));
	const char *sort = data ? obs_data_get_string(data, "sort") : "";
	sort_by = std::string(sort) == "saved" ? "saved" : "name";
	introduced = data && obs_data_get_bool(data, "dock_introduced");
}

bool dock_introduced()
{
	std::lock_guard lock(config_mutex);
	return introduced;
}

void set_dock_introduced()
{
	{
		std::lock_guard lock(config_mutex);
		introduced = true;
	}
	save_config();
}

fs::path folder()
{
	std::lock_guard lock(config_mutex);
	return library_dir;
}

void set_folder(const fs::path &dir)
{
	{
		std::lock_guard lock(config_mutex);
		library_dir = dir;
	}
	save_config();
}

std::string sort_order()
{
	std::lock_guard lock(config_mutex);
	return sort_by;
}

void set_sort_order(const std::string &order)
{
	{
		std::lock_guard lock(config_mutex);
		sort_by = order;
	}
	save_config();
}

std::string sanitise_name(const std::string &name)
{
	std::string s;
	for (unsigned char c : name)
		s += (c < 0x20 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) ? '-' : (char)c;
	while (!s.empty() && (s.back() == ' ' || s.back() == '.'))
		s.pop_back();
	size_t start = s.find_first_not_of(" .");
	s = start == std::string::npos ? "" : s.substr(start);
	if (s.size() > 120) {
		size_t cut = 120;
		while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
			cut--; // not part-way through a character
		s.resize(cut);
	}
	return s.empty() ? "Scene" : s;
}

// --- Saved scenes ------------------------------------------------------------

std::vector<Entry> entries()
{
	std::vector<Entry> list;
	std::error_code ec;
	for (auto &d : fs::directory_iterator(folder(), ec)) {
		std::string name = d.path().filename().string();
		if (name.empty() || name[0] == '.' || !d.is_directory(ec))
			continue;
		if (auto e = read_entry(d.path()))
			list.push_back(std::move(*e));
	}
	return list;
}

std::optional<Entry> find(const std::string &name)
{
	for (auto &e : entries())
		if (e.name == name)
			return e;
	return std::nullopt;
}

QImage render(obs_source_t *source)
{
	uint32_t cx = obs_source_get_width(source);
	uint32_t cy = obs_source_get_height(source);
	if (!cx || !cy)
		return {};
	QImage frame;
	obs_enter_graphics();
	gs_texrender_t *texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(cx, cy, GS_RGBA);
	if (texrender && stage && gs_texrender_begin(texrender, cx, cy)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(source);
		gs_blend_state_pop();
		gs_texrender_end(texrender);
		gs_stage_texture(stage, gs_texrender_get_texture(texrender));
		uint8_t *data;
		uint32_t linesize;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			frame = QImage(data, (int)cx, (int)cy, (int)linesize, QImage::Format_RGBA8888_Premultiplied).copy();
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(texrender);
	obs_leave_graphics();
	if (frame.isNull())
		return frame;
	// Over black, as on the projector.
	QImage out(frame.size(), QImage::Format_RGB32);
	out.fill(Qt::black);
	QPainter p(&out);
	p.drawImage(0, 0, frame);
	p.end();
	return out;
}

Snapshot snapshot(obs_source_t *scene, const std::string &name)
{
	Snapshot snap;
	snap.name = name;

	std::vector<OBSSource> sources;
	std::set<obs_source_t *> seen;
	collect(scene, sources, seen);

	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (auto &s : sources) {
		OBSDataAutoRelease d = obs_save_source(s);
		obs_data_array_push_back(array, d);
	}

	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	snap.data = obs_data_create();
	obs_data_set_int(snap.data, "format", FORMAT);
	obs_data_set_string(snap.data, "name", name.c_str());
	obs_data_set_string(snap.data, "scene", obs_source_get_name(scene));
	obs_data_set_int(snap.data, "saved", (int64_t)time(nullptr));
	obs_data_set_string(snap.data, "obs_version", obs_get_version_string());
	OBSDataAutoRelease canvas = obs_data_create();
	obs_data_set_int(canvas, "width", ovi.base_width);
	obs_data_set_int(canvas, "height", ovi.base_height);
	obs_data_set_obj(snap.data, "canvas", canvas);
	obs_data_set_array(snap.data, "sources", array);

	snap.paths = referenced_paths(array);
	QImage frame = render(scene);
	if (!frame.isNull())
		snap.thumbnail = frame.width() > THUMBNAIL_WIDTH
					 ? frame.scaledToWidth(THUMBNAIL_WIDTH, Qt::SmoothTransformation)
					 : frame;
	return snap;
}

Saved write(Snapshot snap, const Progress &progress)
{
	Saved result;
	fs::path lib = folder();
	std::error_code ec;
	fs::create_directories(lib, ec);
	if (ec) {
		result.error = "couldn't make the library folder " + lib.string() + ": " + ec.message();
		return result;
	}
	fs::path partial = lib / (".saving-" + random_hex());
	fs::create_directories(partial / "files", ec);
	if (ec) {
		result.error = "couldn't write in the library folder " + lib.string() + ": " + ec.message();
		return result;
	}
	auto fail = [&](const std::string &error) {
		std::error_code ignore;
		fs::remove_all(partial, ignore);
		result.error = error;
		return result;
	};

	// Files other saved scenes have copies of, as they were then: linked to,
	// not copied again.
	std::map<std::tuple<std::string, int64_t, int64_t>, fs::path> stored;
	for (auto &e : entries()) {
		OBSDataAutoRelease data = obs_data_create_from_json_file((e.dir / "scene.json").c_str());
		for (auto &f : read_files(data))
			if (f.kind == "file" && !f.stored.empty())
				stored[{f.path, f.size, f.mtime}] = e.dir / f.stored;
	}

	// What's to be copied, and how much.
	struct Plan {
		FileRecord record;
		std::vector<std::pair<fs::path, fs::path>> copies; // from, to (relative to the scene's folder)
		fs::path link_from;                                 // a copy already in the library
	};
	std::vector<Plan> plans;
	Copier copier{progress};
	int n = 0;
	for (auto &path : snap.paths) {
		Plan plan;
		plan.record.path = path;
		fs::path p(path);
		std::string base = p.filename().string();
		if (base.empty())
			base = "folder";
		fs::path dest = fs::path("files") / std::to_string(++n) / base;
		bool file = fs::is_regular_file(p, ec);
		if (!file && !fs::is_directory(p, ec)) {
			plan.record.kind = "file";
			plan.record.skipped = "it had gone by the time it was to be copied";
		} else if (file && access(p.c_str(), R_OK) != 0) {
			// A file it can't read is noted, not a reason to fail the save.
			plan.record.kind = "file";
			plan.record.skipped = "it couldn't be read";
		} else if (file) {
			plan.record.kind = "file";
			plan.record.size = (int64_t)fs::file_size(p, ec);
			plan.record.mtime = mtime_of(p);
			plan.copies.emplace_back(p, dest);
			if (inside(p, lib))
				plan.link_from = p;
			else if (auto s = stored.find({path, plan.record.size, plan.record.mtime});
				 s != stored.end() && fs::file_size(s->second, ec) == (uintmax_t)plan.record.size && !ec)
				plan.link_from = s->second;
			copier.total += plan.record.size;
		} else {
			plan.record.kind = "folder";
			uint64_t bytes = 0;
			bool too_big = false;
			auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
			for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
				if (!it->is_regular_file(ec))
					continue;
				bytes += it->file_size(ec);
				plan.copies.emplace_back(it->path(), dest / fs::relative(it->path(), p, ec));
				if (bytes > MAX_FOLDER_BYTES || plan.copies.size() > MAX_FOLDER_FILES) {
					too_big = true;
					break;
				}
			}
			if (too_big) {
				plan.copies.clear();
				plan.record.skipped = "it's a folder of over 4 GB or 5,000 files";
			} else {
				copier.total += bytes;
			}
		}
		if (plan.record.skipped.empty()) {
			plan.record.stored = dest.string();
			fs::create_directories(partial / (plan.record.kind == "folder" ? dest : dest.parent_path()), ec);
		}
		plans.push_back(std::move(plan));
	}

	copier.tick();
	OBSDataArrayAutoRelease files = obs_data_array_create();
	for (auto &plan : plans) {
		for (auto &[from, to] : plan.copies) {
			fs::create_directories((partial / to).parent_path(), ec);
			std::string error;
			bool linkable = !plan.link_from.empty();
			fs::path source = linkable ? plan.link_from : from;
			uint64_t size = plan.record.kind == "file" ? (uint64_t)plan.record.size : 0;
			if (!copier.link_or_copy(source, partial / to, linkable, size, error)) {
				if (copier.cancelled)
					return fail("cancelled");
				return fail(error);
			}
		}
		OBSDataAutoRelease f = obs_data_create();
		obs_data_set_string(f, "path", plan.record.path.c_str());
		obs_data_set_string(f, "kind", plan.record.kind.c_str());
		if (!plan.record.stored.empty())
			obs_data_set_string(f, "stored", plan.record.stored.c_str());
		if (!plan.record.skipped.empty())
			obs_data_set_string(f, "skipped", plan.record.skipped.c_str());
		if (plan.record.kind == "file") {
			obs_data_set_int(f, "size", plan.record.size);
			obs_data_set_int(f, "mtime", plan.record.mtime);
		}
		obs_data_array_push_back(files, f);
	}
	obs_data_set_array(snap.data, "files", files);

	if (!snap.thumbnail.isNull() && !snap.thumbnail.save(QString::fromStdString((partial / "thumbnail.png").string()), "PNG"))
		blog(LOG_WARNING, "[scene-library] couldn't save the thumbnail of '%s'", snap.name.c_str());
	if (!obs_data_save_json_pretty_safe(snap.data, (partial / "scene.json").c_str(), "tmp", nullptr))
		return fail("couldn't write " + (partial / "scene.json").string());

	// Swap it in, replacing a saved scene of the same name.
	{
		std::lock_guard lock(finish_mutex);
		fs::path dir;
		if (auto old = find(snap.name)) {
			dir = old->dir;
			fs::path aside = lib / (".replaced-" + random_hex());
			fs::rename(old->dir, aside, ec);
			if (ec)
				return fail("couldn't replace " + old->dir.string() + ": " + ec.message());
			fs::rename(partial, dir, ec);
			if (ec) {
				std::error_code ignore;
				fs::rename(aside, dir, ignore);
				return fail("couldn't write " + dir.string() + ": " + ec.message());
			}
			fs::remove_all(aside, ec);
		} else {
			dir = unique_dir(lib, sanitise_name(snap.name));
			fs::rename(partial, dir, ec);
			if (ec)
				return fail("couldn't write " + dir.string() + ": " + ec.message());
		}
		if (auto e = read_entry(dir)) {
			result.entry = *e;
			result.ok = true;
		} else {
			result.error = "the saved scene couldn't be read back from " + dir.string();
		}
	}
	if (result.ok)
		blog(LOG_INFO, "[scene-library] saved '%s' (%d sources, %d files) to %s", snap.name.c_str(),
		     result.entry.sources, result.entry.files, result.entry.dir.c_str());
	return result;
}

Loaded load(const Entry &entry)
{
	Loaded result;
	OBSDataAutoRelease data = obs_data_create_from_json_file((entry.dir / "scene.json").c_str());
	if (!data) {
		result.error = "couldn't read " + (entry.dir / "scene.json").string();
		return result;
	}
	if (obs_data_get_int(data, "format") > FORMAT) {
		result.error = "it was saved by a newer version of the Scene Library";
		return result;
	}
	OBSDataArrayAutoRelease sources = obs_data_get_array(data, "sources");

	// Each file: where it was, if it's still there as it was, else the copy.
	std::map<std::string, std::string> paths;
	std::error_code ec;
	for (auto &f : read_files(data)) {
		fs::path original(f.path);
		bool there = f.kind == "folder" ? fs::is_directory(original, ec)
						: fs::is_regular_file(original, ec) &&
							  (int64_t)fs::file_size(original, ec) == f.size &&
							  mtime_of(original) == f.mtime;
		fs::path copy = f.stored.empty() ? fs::path() : entry.dir / f.stored;
		if (there)
			paths[f.path] = f.path;
		else if (!copy.empty() && fs::exists(copy, ec))
			paths[f.path] = copy.string();
		else if (fs::exists(original, ec))
			paths[f.path] = f.path; // changed since, but all there is
		else
			result.missing_files.push_back(f.path);
	}

	Existing existing;
	existing.name_taken = [](const std::string &name) {
		OBSSourceAutoRelease s = obs_get_source_by_name(name.c_str());
		return s != nullptr;
	};
	existing.device = [](const std::string &name, const std::string &id) -> std::string {
		OBSSourceAutoRelease s = obs_get_source_by_name(name.c_str());
		if (s && id == obs_source_get_unversioned_id(s))
			return obs_source_get_uuid(s);
		return "";
	};
	existing.captures = [](const std::string &id) {
		switch (obs_source_get_icon_type(id.c_str())) {
		case OBS_ICON_TYPE_AUDIO_INPUT:
		case OBS_ICON_TYPE_AUDIO_OUTPUT:
		case OBS_ICON_TYPE_DESKTOP_CAPTURE:
		case OBS_ICON_TYPE_WINDOW_CAPTURE:
		case OBS_ICON_TYPE_GAME_CAPTURE:
		case OBS_ICON_TYPE_CAMERA:
		case OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT:
			return true;
		default:
			return false;
		}
	};
	existing.new_uuid = [] {
		char *u = os_generate_uuid();
		std::string s = u;
		bfree(u);
		return s;
	};

	for (auto &type : source_types(sources))
		if (!obs_source_get_display_name(type.c_str()))
			result.missing_types.push_back(type);

	Prepared prepared =
		prepare_for_load(sources, obs_data_get_string(data, "scene"), entry.name, existing, paths);
	result.reused = prepared.reused;

	struct Ctx {
		std::string root;
		obs_source_t *source = nullptr;
	} ctx{prepared.root};
	obs_load_sources(
		prepared.sources,
		[](void *param, obs_source_t *source) {
			auto *c = static_cast<Ctx *>(param);
			if (!c->source && c->root == obs_source_get_name(source) && obs_scene_from_source(source))
				c->source = obs_source_get_ref(source);
		},
		&ctx);
	OBSSourceAutoRelease root = ctx.source;
	if (!root) {
		result.error = "OBS didn't create the scene";
		return result;
	}
	result.scene = obs_source_get_name(root);
	if (obs_frontend_preview_program_mode_active())
		obs_frontend_set_current_preview_scene(root);

	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi) && entry.width && entry.height)
		result.other_canvas = ovi.base_width != entry.width || ovi.base_height != entry.height;

	result.ok = true;
	blog(LOG_INFO, "[scene-library] added '%s' to the scenes as '%s'", entry.name.c_str(), result.scene.c_str());
	for (auto &t : result.missing_types)
		blog(LOG_WARNING, "[scene-library] '%s' uses %s, which isn't installed", entry.name.c_str(), t.c_str());
	for (auto &f : result.missing_files)
		blog(LOG_WARNING, "[scene-library] '%s' uses %s, which is missing", entry.name.c_str(), f.c_str());
	return result;
}

bool remove(const Entry &entry, std::string &error)
{
	std::lock_guard lock(finish_mutex);
	std::error_code ec;
	fs::remove_all(entry.dir, ec);
	if (ec) {
		error = "couldn't delete " + entry.dir.string() + ": " + ec.message();
		return false;
	}
	blog(LOG_INFO, "[scene-library] deleted '%s'", entry.name.c_str());
	return true;
}

bool rename(const Entry &entry, const std::string &name, std::string &error)
{
	std::lock_guard lock(finish_mutex);
	if (name.empty()) {
		error = "a saved scene needs a name";
		return false;
	}
	if (auto other = find(name); other && other->dir != entry.dir) {
		error = "there's already a saved scene called '" + name + "'";
		return false;
	}
	fs::path json = entry.dir / "scene.json";
	OBSDataAutoRelease data = obs_data_create_from_json_file(json.c_str());
	if (!data) {
		error = "couldn't read " + json.string();
		return false;
	}
	obs_data_set_string(data, "name", name.c_str());
	if (!obs_data_save_json_pretty_safe(data, json.c_str(), "tmp", nullptr)) {
		error = "couldn't write " + json.string();
		return false;
	}
	// Its folder follows, if it can.
	if (sanitise_name(name) != entry.dir.filename().string()) {
		std::error_code ec;
		fs::rename(entry.dir, unique_dir(entry.dir.parent_path(), sanitise_name(name)), ec);
	}
	blog(LOG_INFO, "[scene-library] renamed '%s' to '%s'", entry.name.c_str(), name.c_str());
	return true;
}

void tidy()
{
	std::error_code ec;
	auto now = fs::file_time_type::clock::now();
	for (auto &d : fs::directory_iterator(folder(), ec)) {
		std::string name = d.path().filename().string();
		if (name.rfind(".saving-", 0) != 0 && name.rfind(".replaced-", 0) != 0)
			continue;
		// Not one another OBS is still writing.
		auto t = fs::last_write_time(d.path(), ec);
		if (!ec && now - t > std::chrono::hours(1))
			fs::remove_all(d.path(), ec);
	}
}

} // namespace library
