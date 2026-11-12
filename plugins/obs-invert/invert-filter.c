/* Invert: a video filter that swaps light and dark, by default keeping each
 * colour's hue and saturation (see data/invert.effect for how). Made for
 * logos supplied on a white background that need to go on a black one. */

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-invert", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Invert filter that can keep hue and saturation";
}

#define SETTING_KEEP_HUE "keep_hue_saturation"

struct invert_filter {
	obs_source_t *context;
	gs_effect_t *effect;
	bool keep_hue;
};

static const char *invert_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Invert");
}

static void invert_update(void *data, obs_data_t *settings)
{
	struct invert_filter *filter = data;
	filter->keep_hue = obs_data_get_bool(settings, SETTING_KEEP_HUE);
}

static void invert_destroy(void *data)
{
	struct invert_filter *filter = data;
	if (filter->effect) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		obs_leave_graphics();
	}
	bfree(filter);
}

static void *invert_create(obs_data_t *settings, obs_source_t *context)
{
	struct invert_filter *filter = bzalloc(sizeof(*filter));
	filter->context = context;

	char *path = obs_module_file("invert.effect");
	char *errors = NULL;
	obs_enter_graphics();
	filter->effect = path ? gs_effect_create_from_file(path, &errors) : NULL;
	obs_leave_graphics();

	if (!filter->effect) {
		blog(LOG_ERROR, "[obs-invert] failed to load %s: %s", path ? path : "invert.effect",
		     errors ? errors : "file not found");
		bfree(errors);
		bfree(path);
		invert_destroy(filter);
		return NULL;
	}
	bfree(path);

	invert_update(filter, settings);
	return filter;
}

static void invert_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, SETTING_KEEP_HUE, true);
}

static obs_properties_t *invert_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_bool(props, SETTING_KEEP_HUE, obs_module_text("KeepHueSaturation"));
	obs_property_set_long_description(p, obs_module_text("KeepHueSaturation.Help"));
	return props;
}

/* The inversion works on gamma-encoded SDR values, as image editors do. HDR
 * sources (extended range) are passed through untouched: "1 - value" has no
 * meaning above SDR white. */
static enum gs_color_space source_space(struct invert_filter *filter)
{
	const enum gs_color_space spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
	return obs_source_get_color_space(obs_filter_get_target(filter->context), OBS_COUNTOF(spaces), spaces);
}

static void invert_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct invert_filter *filter = data;

	if (source_space(filter) == GS_CS_709_EXTENDED) {
		obs_source_skip_video_filter(filter->context);
		return;
	}
	if (!obs_source_process_filter_begin_with_color_space(filter->context, GS_RGBA, GS_CS_SRGB,
							      OBS_ALLOW_DIRECT_RENDERING))
		return;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(filter->context, filter->effect, 0, 0,
					   filter->keep_hue ? "InvertKeepHue" : "Invert");
	gs_blend_state_pop();
}

static enum gs_color_space invert_get_color_space(void *data, size_t count, const enum gs_color_space *preferred)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred);
	struct invert_filter *filter = data;
	return source_space(filter) == GS_CS_709_EXTENDED ? GS_CS_709_EXTENDED : GS_CS_SRGB;
}

static struct obs_source_info invert_filter_info = {
	.id = "grove_invert_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = invert_get_name,
	.create = invert_create,
	.destroy = invert_destroy,
	.update = invert_update,
	.get_defaults = invert_defaults,
	.get_properties = invert_properties,
	.video_render = invert_render,
	.video_get_color_space = invert_get_color_space,
};

bool obs_module_load(void)
{
	obs_register_source(&invert_filter_info);
	return true;
}
