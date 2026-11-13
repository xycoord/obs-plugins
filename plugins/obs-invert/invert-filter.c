/* Invert: a video filter that swaps light and dark, by default keeping each
 * colour's hue and saturation. Made for logos supplied on a white (or cream)
 * background that need to go on a black one: it also cleans up what JPEG does
 * to them and can make the background transparent, so the logo can go over
 * anything. The method is described in data/invert.effect; this file runs its
 * passes. */

#include <obs-module.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-invert", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Invert filter that can keep hue and saturation";
}

#define SETTING_KEEP_HUE "keep_hue_saturation"
#define SETTING_CLEAN_UP "clean_up"
#define SETTING_TRANSPARENT "transparent"
#define SETTING_FADE "fade_soft"
#define SETTING_BACKGROUND "background"         /* "auto" or "pick" */
#define SETTING_BACKGROUND_COLOUR "background_colour"
#define SETTING_EDGE_SOFTNESS "edge_softness"
#define SETTING_THIN "thin_details"             /* "auto", "solid" or "faint" */
#define SETTING_GREY_SHADOWS "grey_shadows"

struct invert_filter {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_eparam_t *image_param;
	gs_eparam_t *paper_param;
	gs_eparam_t *source_param;
	gs_eparam_t *keep_hue_param;
	gs_eparam_t *black_point_param;
	gs_eparam_t *fade_param;
	gs_eparam_t *texel_param;
	gs_eparam_t *paper_pick_param;
	gs_eparam_t *reach_param;
	gs_eparam_t *detail_param;
	gs_eparam_t *grey_shadows_param;
	gs_texrender_t *copy;   /* the source, read by every pass */
	gs_texrender_t *paper;  /* 1 x 1: the paper colour */
	gs_texrender_t *processed;

	bool keep_hue;
	float black_point; /* 0..1 */
	bool transparent;
	float fade; /* 0..1 */
	struct vec4 paper_pick; /* rgb, a: 1 to use it */
	float reach;            /* 1..4 */
	float detail;           /* 0 faint, 0.5 automatic, 1 solid */
	bool grey_shadows;
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
	filter->fade = (float)obs_data_get_double(settings, SETTING_FADE) / 100.0f;
	uint32_t colour = (uint32_t)obs_data_get_int(settings, SETTING_BACKGROUND_COLOUR); /* 0xAABBGGRR */
	bool pick = strcmp(obs_data_get_string(settings, SETTING_BACKGROUND), "pick") == 0;
	vec4_set(&filter->paper_pick, (float)(colour & 0xff) / 255.0f, (float)((colour >> 8) & 0xff) / 255.0f,
		 (float)((colour >> 16) & 0xff) / 255.0f, pick ? 1.0f : 0.0f);
	filter->reach = (float)obs_data_get_double(settings, SETTING_EDGE_SOFTNESS);
	const char *thin = obs_data_get_string(settings, SETTING_THIN);
	filter->detail = strcmp(thin, "solid") == 0 ? 1.0f : strcmp(thin, "faint") == 0 ? 0.0f : 0.5f;
	filter->grey_shadows = obs_data_get_bool(settings, SETTING_GREY_SHADOWS);
}

static void invert_destroy(void *data)
{
	struct invert_filter *filter = data;
	obs_enter_graphics();
	gs_effect_destroy(filter->effect);
	gs_texrender_destroy(filter->copy);
	gs_texrender_destroy(filter->paper);
	gs_texrender_destroy(filter->processed);
	obs_leave_graphics();
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
		filter->image_param = gs_effect_get_param_by_name(filter->effect, "image");
		filter->paper_param = gs_effect_get_param_by_name(filter->effect, "paper_tex");
		filter->source_param = gs_effect_get_param_by_name(filter->effect, "source_tex");
		filter->keep_hue_param = gs_effect_get_param_by_name(filter->effect, "keep_hue");
		filter->black_point_param = gs_effect_get_param_by_name(filter->effect, "black_point");
		filter->fade_param = gs_effect_get_param_by_name(filter->effect, "fade");
		filter->texel_param = gs_effect_get_param_by_name(filter->effect, "texel");
		filter->paper_pick_param = gs_effect_get_param_by_name(filter->effect, "paper_pick");
		filter->reach_param = gs_effect_get_param_by_name(filter->effect, "reach");
		filter->detail_param = gs_effect_get_param_by_name(filter->effect, "detail");
		filter->grey_shadows_param = gs_effect_get_param_by_name(filter->effect, "grey_shadows");
		filter->copy = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		filter->paper = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		filter->processed = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
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
	/* Shadows and glows fade fully, as on the page. */
	obs_data_set_default_double(settings, SETTING_FADE, 100.0);
	obs_data_set_default_string(settings, SETTING_BACKGROUND, "auto");
	obs_data_set_default_int(settings, SETTING_BACKGROUND_COLOUR, 0xffffffff);
	obs_data_set_default_double(settings, SETTING_EDGE_SOFTNESS, 1.0);
	obs_data_set_default_string(settings, SETTING_THIN, "auto");
	obs_data_set_default_bool(settings, SETTING_GREY_SHADOWS, true);
}

static bool transparent_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	bool on = obs_data_get_bool(settings, SETTING_TRANSPARENT);
	const char *only_transparent[] = {SETTING_EDGE_SOFTNESS, SETTING_THIN, SETTING_GREY_SHADOWS, SETTING_FADE};
	for (size_t i = 0; i < OBS_COUNTOF(only_transparent); i++)
		obs_property_set_visible(obs_properties_get(props, only_transparent[i]), on);
	return true;
}

static bool background_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	obs_property_set_visible(obs_properties_get(props, SETTING_BACKGROUND_COLOUR),
				 strcmp(obs_data_get_string(settings, SETTING_BACKGROUND), "pick") == 0);
	return true;
}

static obs_properties_t *invert_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_bool(props, SETTING_KEEP_HUE, obs_module_text("KeepHueSaturation"));
	obs_property_set_long_description(p, obs_module_text("KeepHueSaturation.Help"));

	p = obs_properties_add_list(props, SETTING_BACKGROUND, obs_module_text("Background"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Background.Auto"), "auto");
	obs_property_list_add_string(p, obs_module_text("Background.Pick"), "pick");
	obs_property_set_long_description(p, obs_module_text("Background.Help"));
	obs_property_set_modified_callback(p, background_changed);
	p = obs_properties_add_color(props, SETTING_BACKGROUND_COLOUR, obs_module_text("BackgroundColour"));
	obs_property_set_long_description(p, obs_module_text("BackgroundColour.Help"));

	p = obs_properties_add_float_slider(props, SETTING_CLEAN_UP, obs_module_text("CleanUp"), 0.0, 25.0, 0.5);
	obs_property_float_set_suffix(p, "%");
	obs_property_set_long_description(p, obs_module_text("CleanUp.Help"));

	p = obs_properties_add_bool(props, SETTING_TRANSPARENT, obs_module_text("Transparent"));
	obs_property_set_long_description(p, obs_module_text("Transparent.Help"));
	obs_property_set_modified_callback(p, transparent_changed);

	p = obs_properties_add_float_slider(props, SETTING_EDGE_SOFTNESS, obs_module_text("EdgeSoftness"), 1.0, 4.0, 0.25);
	obs_property_float_set_suffix(p, " px");
	obs_property_set_long_description(p, obs_module_text("EdgeSoftness.Help"));

	p = obs_properties_add_list(props, SETTING_THIN, obs_module_text("Thin"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Thin.Auto"), "auto");
	obs_property_list_add_string(p, obs_module_text("Thin.Solid"), "solid");
	obs_property_list_add_string(p, obs_module_text("Thin.Faint"), "faint");
	obs_property_set_long_description(p, obs_module_text("Thin.Help"));

	p = obs_properties_add_bool(props, SETTING_GREY_SHADOWS, obs_module_text("GreyShadows"));
	obs_property_set_long_description(p, obs_module_text("GreyShadows.Help"));

	p = obs_properties_add_float_slider(props, SETTING_FADE, obs_module_text("FadeSoft"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, "%");
	obs_property_set_long_description(p, obs_module_text("FadeSoft.Help"));

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

/* Draw tex through the effect's technique into the current target, w x h.
 * The settings are set for every pass: OBS's filter drawing (the copy pass)
 * leaves the effect's other parameters at zero. */
static void draw_pass(struct invert_filter *filter, gs_texture_t *tex, const char *technique, uint32_t w, uint32_t h,
		      const struct vec2 *texel, gs_texture_t *paper)
{
	gs_effect_set_float(filter->keep_hue_param, filter->keep_hue ? 1.0f : 0.0f);
	gs_effect_set_float(filter->black_point_param, filter->black_point);
	gs_effect_set_float(filter->fade_param, filter->fade);
	gs_effect_set_vec2(filter->texel_param, texel);
	gs_effect_set_vec4(filter->paper_pick_param, &filter->paper_pick);
	gs_effect_set_float(filter->reach_param, filter->reach);
	gs_effect_set_float(filter->detail_param, filter->detail);
	gs_effect_set_float(filter->grey_shadows_param, filter->grey_shadows ? 1.0f : 0.0f);
	if (paper)
		gs_effect_set_texture(filter->paper_param, paper);
	gs_effect_set_texture(filter->image_param, tex);
	while (gs_effect_loop(filter->effect, technique))
		gs_draw_sprite(tex, 0, w, h);
}

/* Render into tr at w x h, replacing what's there. */
static bool begin_target(gs_texrender_t *tr, uint32_t w, uint32_t h)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	return true;
}

static void invert_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct invert_filter *filter = data;

	if (is_hdr(source_space(filter))) {
		obs_source_skip_video_filter(filter->context);
		return;
	}
	obs_source_t *target = obs_filter_get_target(filter->context);
	uint32_t w = obs_source_get_base_width(target);
	uint32_t h = obs_source_get_base_height(target);
	if (!w || !h) {
		obs_source_skip_video_filter(filter->context);
		return;
	}
	if (!obs_source_process_filter_begin_with_color_space(filter->context, GS_RGBA, GS_CS_SRGB,
							      OBS_NO_DIRECT_RENDERING))
		return;

	struct vec2 texel;
	vec2_set(&texel, 1.0f / (float)w, 1.0f / (float)h);

	/* Passes into textures replace what's there. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	/* 1. The source, to read several times. */
	if (!begin_target(filter->copy, w, h)) {
		gs_blend_state_pop();
		return;
	}
	obs_source_process_filter_tech_end(filter->context, filter->effect, w, h, "Copy");
	gs_texrender_end(filter->copy);
	gs_texture_t *source = gs_texrender_get_texture(filter->copy);

	/* 2. The paper colour, one pixel. */
	if (begin_target(filter->paper, 1, 1)) {
		draw_pass(filter, source, "Paper", 1, 1, &texel, NULL);
		gs_texrender_end(filter->paper);
	}
	gs_texture_t *paper = gs_texrender_get_texture(filter->paper);

	/* 3. Processed and marked image. Needed for transparency, and when keeping
	 * hue (the colour of each part is inverted at its full strength, which
	 * needs the alpha pass's estimate of it). */
	gs_texture_t *processed = NULL;
	bool alpha_pass = filter->transparent || filter->keep_hue;
	if (alpha_pass && begin_target(filter->processed, w, h)) {
		draw_pass(filter, source, "Process", w, h, &texel, paper);
		gs_texrender_end(filter->processed);
		processed = gs_texrender_get_texture(filter->processed);
	}
	gs_blend_state_pop();

	/* 4. Out: the alpha pass (opaque: with the source's alpha), or for plain
	 * inversion without transparency the processing itself. Premultiplied. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture(filter->source_param, source);
	if (processed)
		draw_pass(filter, processed, filter->transparent ? "Alpha" : "AlphaOpaque", w, h, &texel, paper);
	else
		draw_pass(filter, source, "ProcessOpaque", w, h, &texel, paper);
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
