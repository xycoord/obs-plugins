/* Invert: a video filter that swaps light and dark, keeping each colour's hue
 * and the contrast between colours, so a logo made for white paper reads the
 * same on black. The method is described in data/invert.effect. It has no
 * settings. */

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-invert", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Invert filter that keeps hue and contrast";
}

struct invert_filter {
	obs_source_t *context;
	gs_effect_t *effect;
};

static const char *invert_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Invert");
}

static void invert_destroy(void *data)
{
	struct invert_filter *filter = data;
	obs_enter_graphics();
	gs_effect_destroy(filter->effect);
	obs_leave_graphics();
	bfree(filter);
}

static void *invert_create(obs_data_t *settings, obs_source_t *context)
{
	UNUSED_PARAMETER(settings);
	struct invert_filter *filter = bzalloc(sizeof(*filter));
	filter->context = context;

	char *path = obs_module_file("invert.effect");
	char *errors = NULL;
	obs_enter_graphics();
	filter->effect = path ? gs_effect_create_from_file(path, &errors) : NULL;
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
	return filter;
}

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

	/* Inverting HDR has no natural white to flip around: pass it through. */
	if (is_hdr(source_space(filter))) {
		obs_source_skip_video_filter(filter->context);
		return;
	}
	/* Never rendered directly: the effect expects OBS's premultiplied
	 * filter texture, not the source's own. */
	if (!obs_source_process_filter_begin_with_color_space(filter->context, GS_RGBA, GS_CS_SRGB,
							      OBS_NO_DIRECT_RENDERING))
		return;

	/* The output is premultiplied. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(filter->context, filter->effect, 0, 0, "Draw");
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
	.video_render = invert_render,
	.video_get_color_space = invert_get_color_space,
};

bool obs_module_load(void)
{
	obs_register_source(&invert_filter_info);
	return true;
}
