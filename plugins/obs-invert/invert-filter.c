/* Invert: a video filter that swaps light and dark, by default keeping each
 * colour's hue and saturation (see data/invert.effect for how). Made for
 * logos supplied on a white background that need to go on a black one: it can
 * also clean up what's left of a JPEG's background and make the background
 * transparent, so the logo can go over anything. */

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-invert", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Invert filter that can keep hue and saturation";
}

#define SETTING_KEEP_HUE "keep_hue_saturation"
#define SETTING_CLEAN_UP "clean_up"
#define SETTING_TRANSPARENT "transparent"
#define SETTING_SOLID "solid_above"

struct invert_filter {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_eparam_t *keep_hue_param;
	gs_eparam_t *black_point_param;
	gs_eparam_t *transparent_param;
	gs_eparam_t *solid_param;
	gs_eparam_t *texel_param;

	bool keep_hue;
	float black_point; /* 0..1 */
	bool transparent;
	float solid; /* 0..1 */
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
	filter->black_point = (float)obs_data_get_double(settings, SETTING_CLEAN_UP) / 100.0f;
	filter->transparent = obs_data_get_bool(settings, SETTING_TRANSPARENT);
	filter->solid = (float)obs_data_get_double(settings, SETTING_SOLID) / 100.0f;
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
	if (filter->effect) {
		filter->keep_hue_param = gs_effect_get_param_by_name(filter->effect, "keep_hue");
		filter->black_point_param = gs_effect_get_param_by_name(filter->effect, "black_point");
		filter->transparent_param = gs_effect_get_param_by_name(filter->effect, "transparent");
		filter->solid_param = gs_effect_get_param_by_name(filter->effect, "solid");
		filter->texel_param = gs_effect_get_param_by_name(filter->effect, "texel");
	}
	obs_leave_graphics();

	if (!filter->effect) {
		if (path)
			blog(LOG_ERROR, "[obs-invert] failed to compile %s: %s", path,
			     errors ? errors : "(no details)");
		else
			blog(LOG_ERROR, "[obs-invert] failed to load: invert.effect not found");
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
	/* JPEG specks around a logo's text are almost all below 8% brightness
	 * once inverted. */
	obs_data_set_default_double(settings, SETTING_CLEAN_UP, 8.0);
	obs_data_set_default_bool(settings, SETTING_TRANSPARENT, false);
	/* Grey shadows and glows fade out over visuals; the glow on a test logo
	 * came closest to its true transparency at 50%. */
	obs_data_set_default_double(settings, SETTING_SOLID, 50.0);
}

static bool transparent_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	obs_property_set_visible(obs_properties_get(props, SETTING_SOLID),
				 obs_data_get_bool(settings, SETTING_TRANSPARENT));
	return true;
}

static obs_properties_t *invert_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_bool(props, SETTING_KEEP_HUE, obs_module_text("KeepHueSaturation"));
	obs_property_set_long_description(p, obs_module_text("KeepHueSaturation.Help"));

	p = obs_properties_add_float_slider(props, SETTING_CLEAN_UP, obs_module_text("CleanUp"), 0.0, 25.0, 0.5);
	obs_property_float_set_suffix(p, "%");
	obs_property_set_long_description(p, obs_module_text("CleanUp.Help"));

	p = obs_properties_add_bool(props, SETTING_TRANSPARENT, obs_module_text("Transparent"));
	obs_property_set_long_description(p, obs_module_text("Transparent.Help"));
	obs_property_set_modified_callback(p, transparent_changed);

	p = obs_properties_add_float_slider(props, SETTING_SOLID, obs_module_text("SolidAbove"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, "%");
	obs_property_set_long_description(p, obs_module_text("SolidAbove.Help"));

	return props;
}

/* The inversion works on gamma-encoded SDR values, as image editors do. HDR
 * sources (extended range or scRGB) are passed through untouched: "1 - value"
 * has no meaning above SDR white. */
static bool is_hdr(enum gs_color_space space)
{
	return space == GS_CS_709_EXTENDED || space == GS_CS_709_SCRGB;
}

static enum gs_color_space source_space(struct invert_filter *filter)
{
	const enum gs_color_space spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED, GS_CS_709_SCRGB};
	return obs_source_get_color_space(obs_filter_get_target(filter->context), OBS_COUNTOF(spaces), spaces);
}

static void invert_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct invert_filter *filter = data;

	if (is_hdr(source_space(filter))) {
		obs_source_skip_video_filter(filter->context);
		return;
	}
	if (!obs_source_process_filter_begin_with_color_space(filter->context, GS_RGBA, GS_CS_SRGB,
							      OBS_NO_DIRECT_RENDERING))
		return;

	gs_effect_set_float(filter->keep_hue_param, filter->keep_hue ? 1.0f : 0.0f);
	gs_effect_set_float(filter->black_point_param, filter->black_point);
	gs_effect_set_float(filter->transparent_param, filter->transparent ? 1.0f : 0.0f);
	gs_effect_set_float(filter->solid_param, filter->solid);
	obs_source_t *target = obs_filter_get_target(filter->context);
	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);
	struct vec2 texel;
	vec2_set(&texel, 1.0f / (float)(width ? width : 1), 1.0f / (float)(height ? height : 1));
	gs_effect_set_vec2(filter->texel_param, &texel);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_end(filter->context, filter->effect, 0, 0);
	gs_blend_state_pop();
}

static enum gs_color_space invert_get_color_space(void *data, size_t count, const enum gs_color_space *preferred)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred);
	struct invert_filter *filter = data;
	const enum gs_color_space space = source_space(filter);
	return is_hdr(space) ? space : GS_CS_SRGB;
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
