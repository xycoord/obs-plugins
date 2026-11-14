// Unit tests for the Scene Library's changes to saved scenes (snapshot.cpp):
// libobs's data functions only, no OBS running.
#include "snapshot.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace library;

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                     \
		if (!(cond)) {                                                   \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                              \
		}                                                                \
	} while (0)

static std::string str(obs_data_t *d, const char *key)
{
	return obs_data_get_string(d, key);
}

static OBSDataAutoRelease source_named(obs_data_array_t *sources, const std::string &name)
{
	for (size_t i = 0, n = obs_data_array_count(sources); i < n; i++) {
		obs_data_t *s = obs_data_array_item(sources, i);
		if (str(s, "name") == name)
			return s;
		obs_data_release(s);
	}
	return nullptr;
}

static OBSDataArrayAutoRelease parse(const char *json)
{
	OBSDataAutoRelease wrapper = obs_data_create_from_json(json);
	return obs_data_get_array(wrapper, "sources");
}

// What's in OBS already, for the tests: some names taken, one camera.
static Existing existing_with(std::set<std::string> taken)
{
	static int counter = 0;
	Existing e;
	e.name_taken = [taken](const std::string &n) { return taken.count(n) > 0; };
	e.captures = [](const std::string &id) { return id == "v4l2_input"; };
	e.device = [taken](const std::string &name, const std::string &id) -> std::string {
		return id == "v4l2_input" && taken.count(name) ? "camera-uuid-in-obs" : "";
	};
	e.new_uuid = [] {
		char buf[64];
		std::snprintf(buf, sizeof buf, "00000000-0000-0000-0000-%012d", ++counter);
		return std::string(buf);
	};
	return e;
}

static const char *SAVED = R"({"sources": [
 {"name": "Look", "uuid": "u-look", "id": "scene", "versioned_id": "scene", "canvas_uuid": "c-1",
  "hotkeys": {"OBSBasic.SelectScene": [{"key": "OBS_KEY_1"}]},
  "settings": {"items": [
    {"name": "Logo", "source_uuid": "u-logo", "id": 1},
    {"name": "Inner", "source_uuid": "u-inner", "id": 2},
    {"name": "Camera", "source_uuid": "u-cam", "id": 3},
    {"name": "Caption", "source_uuid": "u-caption", "id": 4}]}},
 {"name": "Inner", "uuid": "u-inner", "id": "scene", "versioned_id": "scene",
  "settings": {"items": [{"name": "Logo 2", "source_uuid": "u-logo2", "id": 1}]}},
 {"name": "Logo", "uuid": "u-logo", "id": "image_source", "versioned_id": "image_source",
  "settings": {"file": "/assets/logo.png"},
  "filters": [{"name": "Mask", "uuid": "u-mask", "id": "mask_filter", "versioned_id": "mask_filter_v2",
               "hotkeys": {"x": []},
               "settings": {"image_path": "/assets/mask.png", "follow": "u-logo2"}}]},
 {"name": "Logo 2", "uuid": "u-logo2", "id": "image_source", "versioned_id": "image_source",
  "settings": {"file": "/assets/logo.png", "slides": [{"value": "/assets/logo.png"}]}},
 {"name": "Camera", "uuid": "u-cam", "id": "v4l2_input", "versioned_id": "v4l2_input",
  "settings": {"device_id": "/dev/video0"}},
 {"name": "Caption", "uuid": "u-caption", "id": "text_ft2_source", "versioned_id": "text_ft2_source_v2",
  "settings": {"text": "Logo", "clone_of": "Logo", "font": {"face": "Logo"}}}
]})";

static void test_unique_names()
{
	// "Logo" is taken in OBS; "Logo 2" is free in OBS but another saved source
	// is called that, so "Logo" becomes "Logo 3", and "Logo 2" keeps its name.
	OBSDataArrayAutoRelease saved = parse(R"({"sources": [
	 {"name": "S", "uuid": "a", "id": "scene", "settings": {"items": [
	   {"name": "Logo", "source_uuid": "b"}, {"name": "Logo 2", "source_uuid": "c"}]}},
	 {"name": "Logo", "uuid": "b", "id": "image_source"},
	 {"name": "Logo 2", "uuid": "c", "id": "image_source"}]})");
	Prepared p = prepare_for_load(saved, "S", "S", existing_with({"Logo"}), {});
	CHECK(p.root == "S");
	CHECK(p.names["Logo"] == "Logo 3");
	CHECK(p.names["Logo 2"] == "Logo 2");
	OBSDataAutoRelease scene = source_named(p.sources, "S");
	OBSDataAutoRelease settings = obs_data_get_obj(scene, "settings");
	OBSDataArrayAutoRelease items = obs_data_get_array(settings, "items");
	OBSDataAutoRelease first = obs_data_array_item(items, 0);
	OBSDataAutoRelease second = obs_data_array_item(items, 1);
	CHECK(str(first, "name") == "Logo 3");
	CHECK(str(second, "name") == "Logo 2");
	OBSDataAutoRelease logo3 = source_named(p.sources, "Logo 3");
	CHECK(logo3 && str(first, "source_uuid") == str(logo3, "uuid"));

	// The scene is called what it's saved as, even when that's another saved
	// source's name... which it then leaves to that one.
	OBSDataArrayAutoRelease saved2 = parse(R"({"sources": [
	 {"name": "Look", "uuid": "a", "id": "scene", "settings": {"items": [{"name": "Interval", "source_uuid": "b"}]}},
	 {"name": "Interval", "uuid": "b", "id": "scene"}]})");
	Prepared p2 = prepare_for_load(saved2, "Look", "Interval", existing_with({}), {});
	CHECK(p2.root == "Interval 2");
	CHECK(p2.names["Interval"] == "Interval");
}

static void test_full()
{
	OBSDataArrayAutoRelease saved = parse(SAVED);
	auto existing = existing_with({"Look", "Inner", "Logo", "Logo 2", "Camera", "Caption", "Interval"});
	std::map<std::string, std::string> paths = {{"/assets/logo.png", "/library/Interval/files/1/logo.png"},
						    {"/assets/mask.png", "/assets/mask.png"}};
	Prepared p = prepare_for_load(saved, "Look", "Interval", existing, paths);

	CHECK(p.root == "Interval 2");
	CHECK(p.names["Inner"] == "Inner 2");
	CHECK(p.names["Logo"] == "Logo 3"); // "Logo 2" is taken in OBS too
	CHECK(p.names["Logo 2"] == "Logo 2 2");
	CHECK(p.names["Caption"] == "Caption 2");
	// The camera is in OBS: used as it is, not copied.
	CHECK(p.names["Camera"] == "Camera");
	CHECK(p.reused == std::vector<std::string>{"Camera"});
	CHECK(obs_data_array_count(p.sources) == 5);
	CHECK(!source_named(p.sources, "Camera"));

	OBSDataAutoRelease look = source_named(p.sources, "Interval 2");
	CHECK(look);
	CHECK(!obs_data_has_user_value(look, "hotkeys"));
	CHECK(!obs_data_has_user_value(look, "canvas_uuid"));
	CHECK(str(look, "uuid") != "u-look" && str(look, "uuid").size() == 36);
	OBSDataAutoRelease settings = obs_data_get_obj(look, "settings");
	OBSDataArrayAutoRelease items = obs_data_get_array(settings, "items");
	OBSDataAutoRelease logo = source_named(p.sources, "Logo 3");
	OBSDataAutoRelease inner = source_named(p.sources, "Inner 2");
	OBSDataAutoRelease i0 = obs_data_array_item(items, 0), i1 = obs_data_array_item(items, 1),
			   i2 = obs_data_array_item(items, 2), i3 = obs_data_array_item(items, 3);
	CHECK(str(i0, "name") == "Logo 3" && str(i0, "source_uuid") == str(logo, "uuid"));
	CHECK(str(i1, "name") == "Inner 2" && str(i1, "source_uuid") == str(inner, "uuid"));
	CHECK(str(i2, "name") == "Camera" && str(i2, "source_uuid") == "camera-uuid-in-obs");
	CHECK(str(i3, "name") == "Caption 2");

	// Files: pointed where they are now; a file that's where it was, left.
	OBSDataAutoRelease logo_settings = obs_data_get_obj(logo, "settings");
	CHECK(str(logo_settings, "file") == "/library/Interval/files/1/logo.png");
	OBSDataAutoRelease logo2 = source_named(p.sources, "Logo 2 2");
	OBSDataAutoRelease logo2_settings = obs_data_get_obj(logo2, "settings");
	OBSDataArrayAutoRelease slides = obs_data_get_array(logo2_settings, "slides");
	OBSDataAutoRelease slide = obs_data_array_item(slides, 0);
	CHECK(str(slide, "value") == "/library/Interval/files/1/logo.png");

	// Filters: a uuid of their own, no hotkeys, their settings rewritten too.
	OBSDataArrayAutoRelease filters = obs_data_get_array(logo, "filters");
	OBSDataAutoRelease mask = obs_data_array_item(filters, 0);
	CHECK(str(mask, "uuid") != "u-mask" && str(mask, "uuid").size() == 36);
	CHECK(!obs_data_has_user_value(mask, "hotkeys"));
	OBSDataAutoRelease mask_settings = obs_data_get_obj(mask, "settings");
	CHECK(str(mask_settings, "image_path") == "/assets/mask.png");
	CHECK(str(mask_settings, "follow") == str(logo2, "uuid"));

	// A setting naming a source follows its rename; text and fonts don't.
	OBSDataAutoRelease caption = source_named(p.sources, "Caption 2");
	OBSDataAutoRelease cs = obs_data_get_obj(caption, "settings");
	CHECK(str(cs, "clone_of") == "Logo 3");
	CHECK(str(cs, "text") == "Logo");
	OBSDataAutoRelease font = obs_data_get_obj(cs, "font");
	CHECK(str(font, "face") == "Logo");

	// What was read is left as it was.
	OBSDataAutoRelease orig = source_named(saved, "Look");
	CHECK(str(orig, "uuid") == "u-look" && obs_data_has_user_value(orig, "hotkeys"));
}

static void test_capture_not_in_obs()
{
	// A camera that isn't in OBS is made, like anything else.
	OBSDataArrayAutoRelease saved = parse(R"({"sources": [
	 {"name": "S", "uuid": "a", "id": "scene", "settings": {"items": [{"name": "Camera", "source_uuid": "b"}]}},
	 {"name": "Camera", "uuid": "b", "id": "v4l2_input"}]})");
	Prepared p = prepare_for_load(saved, "S", "S", existing_with({}), {});
	CHECK(p.reused.empty());
	CHECK(source_named(p.sources, "Camera"));
}

static void test_paths_and_types()
{
	CHECK(looks_like_path("/home/a/b.png"));
	CHECK(looks_like_path("C:\\Videos\\a.mp4"));
	CHECK(looks_like_path("d:/a.mp4"));
	CHECK(!looks_like_path("/"));
	CHECK(!looks_like_path("logo.png"));
	CHECK(!looks_like_path("https://example.com/a.png"));
	CHECK(!looks_like_path("/a\nb"));
	CHECK(!looks_like_path(nullptr));

	namespace fs = std::filesystem;
	fs::path dir = fs::temp_directory_path() / "scene-library-unit";
	fs::remove_all(dir);
	fs::create_directories(dir / "slides");
	std::ofstream(dir / "a.png") << "x";
	std::string json = R"({"sources": [
	 {"name": "A", "id": "image_source", "settings": {"file": ")" + (dir / "a.png").string() + R"("}},
	 {"name": "B", "id": "slideshow", "settings": {"files": [{"value": ")" + (dir / "slides").string() +
			   R"("}, {"value": ")" + (dir / "a.png").string() + R"("}, {"value": "/no/such/file.png"}]},
	  "filters": [{"name": "F", "id": "mask_filter", "versioned_id": "mask_filter_v2",
	               "settings": {"image_path": ")" + (dir / "a.png").string() + R"("}}]},
	 {"name": "S", "id": "scene", "settings": {"items": [{"name": "A",
	   "show_transition": {"id": "fade_transition", "versioned_id": "fade_transition"},
	   "hide_transition": {"duration": 0}}]}}]})";
	OBSDataArrayAutoRelease sources = parse(json.c_str());
	auto paths = referenced_paths(sources);
	CHECK(paths.size() == 2);
	CHECK(paths.size() == 2 && paths[0] == (dir / "a.png").string() && paths[1] == (dir / "slides").string());
	auto types = source_types(sources);
	CHECK(types == (std::set<std::string>{"image_source", "slideshow", "mask_filter_v2", "scene", "fade_transition"}));
	fs::remove_all(dir);
}

int main()
{
	test_unique_names();
	test_full();
	test_capture_not_in_obs();
	test_paths_and_types();
	if (failures) {
		std::fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	std::printf("scene library unit tests: all passed\n");
	return 0;
}
