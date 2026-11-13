/* Invert: a video filter that swaps light and dark, keeping each colour's hue
 * and the contrast between colours, so a logo made for white paper reads the
 * same on black. It has no settings.
 *
 * Each colour's tone (CIELAB L*, perceived lightness from luminance alone)
 * becomes 1 - L*, keeping its OKLab hue and chroma; a light, vivid colour with
 * no dark version as colourful loses chroma at the same tone. That takes a
 * Newton solve and, outside the gamut, a search, so it is worked out once, on
 * the CPU, into a 3D lookup table (65 x 65 x 65, indexed by sRGB-encoded
 * colour); the shader just looks colours up in it. tests/invert-check has the
 * same inversion in numpy and checks the filter against it.
 *
 * HDR: the tone flip is across SDR black to SDR white, so on an HDR source
 * white paper still turns black. Anything brighter than SDR white is past
 * paper white, and turns black too. */

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <obs-module.h>
#include <util/threading.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-invert", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Invert filter that keeps hue and contrast";
}

/* ------------------------------------------------------------- the inversion */

#define LUT_SIZE 65

static double srgb_to_linear(double c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double linear_to_srgb(double c)
{
	c = c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
	return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

/* CIELAB L* (0..1) of a luminance, and back. */
static double tone_of(double y)
{
	return y > 0.008856 ? 1.16 * cbrt(y) - 0.16 : y * 9.033;
}

static double luminance_of_tone(double t)
{
	return t > 0.08 ? pow((t + 0.16) / 1.16, 3.0) : t / 9.033;
}

static void linear_to_oklab(const double c[3], double lab[3])
{
	double l = cbrt(0.4122214708 * c[0] + 0.5363325363 * c[1] + 0.0514459929 * c[2]);
	double m = cbrt(0.2119034982 * c[0] + 0.6806995451 * c[1] + 0.1073969566 * c[2]);
	double s = cbrt(0.0883024619 * c[0] + 0.2817188376 * c[1] + 0.6299787005 * c[2]);
	lab[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
	lab[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
	lab[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}

static void oklab_to_linear(double L, double a, double b, double c[3])
{
	double l = L + 0.3963377774 * a + 0.2158037573 * b;
	double m = L - 0.1055613458 * a - 0.0638541728 * b;
	double s = L - 0.0894841775 * a - 1.2914855480 * b;
	l = l * l * l;
	m = m * m * m;
	s = s * s * s;
	c[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
	c[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
	c[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

static bool in_gamut(const double c[3])
{
	for (int i = 0; i < 3; i++)
		if (c[i] < -0.0001 || c[i] > 1.0001)
			return false;
	return true;
}

/* The linear colour with OKLab chroma (a, b) and luminance y: Newton on OKLab L,
 * since luminance is a cubic in it. */
static void with_luminance(double y, double a, double b, double c[3])
{
	const double u[3] = {0.3963377774 * a + 0.2158037573 * b, -0.1055613458 * a - 0.0638541728 * b,
			     -0.0894841775 * a - 1.2914855480 * b};
	const double cw[3] = {-0.04077454, 1.11249218, -0.07171764}; /* luminance from cubed LMS */
	double L = cbrt(y > 0.0 ? y : 0.0);
	for (int i = 0; i < 20; i++) {
		double f = -y, df = 0.0;
		for (int k = 0; k < 3; k++) {
			double q = L + u[k];
			f += cw[k] * q * q * q;
			df += 3.0 * cw[k] * q * q;
		}
		const double next = fmin(fmax(L - f / (fabs(df) > 0.0001 ? df : 0.0001), 0.0), 1.0);
		const bool done = fabs(next - L) < 1e-9;
		L = next;
		if (done)
			break;
	}
	oklab_to_linear(L, a, b, c);
}

/* sRGB-encoded colour in, sRGB-encoded inverse out. */
static void tone_invert(const double rgb[3], double out[3])
{
	double lin[3], lab[3], c[3];
	for (int i = 0; i < 3; i++)
		lin[i] = srgb_to_linear(rgb[i]);
	const double y = luminance_of_tone(1.0 - tone_of(0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]));
	linear_to_oklab(lin, lab);
	with_luminance(y, lab[1], lab[2], c);
	if (!in_gamut(c)) {
		/* Less chroma, same tone and hue: the most that fits. */
		double lo = 0.0, hi = 1.0;
		for (int i = 0; i < 14; i++) {
			double k = 0.5 * (lo + hi);
			with_luminance(y, lab[1] * k, lab[2] * k, c);
			if (in_gamut(c))
				lo = k;
			else
				hi = k;
		}
		with_luminance(y, lab[1] * lo, lab[2] * lo, c);
	}
	for (int i = 0; i < 3; i++)
		out[i] = linear_to_srgb(c[i]);
}

/* IEEE half float, rounded to nearest even (values here are 0..1, so no
 * overflow or NaN handling is needed). */
static uint16_t to_half(float f)
{
	uint32_t x;
	memcpy(&x, &f, sizeof(x));
	const uint32_t sign = (x >> 16) & 0x8000;
	const int32_t exp = (int32_t)((x >> 23) & 0xff) - 127 + 15;
	uint32_t mant = x & 0x7fffff;
	uint32_t shift = 13;
	if (exp <= 0) { /* subnormal or zero */
		if (exp < -10)
			return (uint16_t)sign;
		mant |= 0x800000;
		shift = (uint32_t)(14 - exp);
	}
	uint32_t h = (exp <= 0 ? 0 : (uint32_t)exp << 10) | (mant >> shift);
	const uint32_t rest = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
	if (rest > half || (rest == half && (h & 1)))
		h++; /* a carry into the exponent is still correct */
	return (uint16_t)(sign | h);
}

/* The lookup table, built once and shared: RGBA half floats, red fastest. It
 * takes a few hundred milliseconds, so it is built on a thread started when
 * the module loads; the first filter created waits for it if it must. */
static uint16_t *lut_data;
static pthread_t lut_thread;
static bool lut_thread_started;
static pthread_once_t lut_once = PTHREAD_ONCE_INIT;

static void build_lut(void)
{
	uint16_t *data = bmalloc(sizeof(uint16_t) * 4 * LUT_SIZE * LUT_SIZE * LUT_SIZE);
	size_t i = 0;
	for (int b = 0; b < LUT_SIZE; b++) {
		for (int g = 0; g < LUT_SIZE; g++) {
			for (int r = 0; r < LUT_SIZE; r++) {
				const double in[3] = {r / (LUT_SIZE - 1.0), g / (LUT_SIZE - 1.0), b / (LUT_SIZE - 1.0)};
				double out[3];
				tone_invert(in, out);
				data[i++] = to_half((float)out[0]);
				data[i++] = to_half((float)out[1]);
				data[i++] = to_half((float)out[2]);
				data[i++] = 0x3c00; /* 1.0 */
			}
		}
	}
	lut_data = data;
}

static void *lut_thread_main(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("obs-invert: lookup table");
	build_lut();
	return NULL;
}

static void wait_for_lut(void)
{
	if (lut_thread_started)
		pthread_join(lut_thread, NULL);
	else
		build_lut();
}

/* ----------------------------------------------------------------- the filter */

struct invert_filter {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_texture_t *lut;
	gs_eparam_t *lut_param;
	gs_eparam_t *lut_scale_param;
	gs_eparam_t *lut_offset_param;
	gs_eparam_t *hdr_param;
	gs_eparam_t *multiplier_param;
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
	gs_voltexture_destroy(filter->lut);
	obs_leave_graphics();
	bfree(filter);
}

static void *invert_create(obs_data_t *settings, obs_source_t *context)
{
	UNUSED_PARAMETER(settings);
	struct invert_filter *filter = bzalloc(sizeof(*filter));
	filter->context = context;
	pthread_once(&lut_once, wait_for_lut);

	char *path = obs_module_file("invert.effect");
	char *errors = NULL;
	obs_enter_graphics();
	filter->effect = path ? gs_effect_create_from_file(path, &errors) : NULL;
	const uint8_t *planes[] = {(const uint8_t *)lut_data};
	filter->lut = gs_voltexture_create(LUT_SIZE, LUT_SIZE, LUT_SIZE, GS_RGBA16F, 1, planes, 0);
	if (filter->effect) {
		filter->lut_param = gs_effect_get_param_by_name(filter->effect, "lut");
		filter->lut_scale_param = gs_effect_get_param_by_name(filter->effect, "lut_scale");
		filter->lut_offset_param = gs_effect_get_param_by_name(filter->effect, "lut_offset");
		filter->hdr_param = gs_effect_get_param_by_name(filter->effect, "hdr");
		filter->multiplier_param = gs_effect_get_param_by_name(filter->effect, "multiplier");
	}
	obs_leave_graphics();

	if (!filter->effect || !filter->lut) {
		if (!path)
			blog(LOG_ERROR, "[obs-invert] failed to load: invert.effect not found");
		else if (!filter->effect)
			blog(LOG_ERROR, "[obs-invert] failed to compile %s: %s", path,
			     errors ? errors : "(no details)");
		else
			blog(LOG_ERROR, "[obs-invert] failed to create the lookup table texture");
		bfree(errors);
		bfree(path);
		invert_destroy(filter);
		return NULL;
	}
	bfree(path);
	return filter;
}

/* The space the filter works and outputs in: the source's own, in 16-bit
 * float (so dividing out alpha keeps faint colours precise). */
static enum gs_color_space working_space(struct invert_filter *filter)
{
	const enum gs_color_space spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED, GS_CS_709_SCRGB};
	const enum gs_color_space space =
		obs_source_get_color_space(obs_filter_get_target(filter->context), OBS_COUNTOF(spaces), spaces);
	return space == GS_CS_SRGB ? GS_CS_SRGB_16F : space;
}

static void invert_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct invert_filter *filter = data;

	const enum gs_color_space space = working_space(filter);
	/* Never rendered directly: the effect expects OBS's premultiplied filter
	 * texture, not the source's own. 16-bit float, so dividing out alpha
	 * keeps faint colours precise. */
	if (!obs_source_process_filter_begin_with_color_space(filter->context, GS_RGBA16F, space,
							      OBS_NO_DIRECT_RENDERING))
		return;

	/* scRGB has 1.0 at 80 nits; the inversion works with 1.0 at SDR white. */
	const float sdr_white = obs_get_video_sdr_white_level();
	const float multiplier = space == GS_CS_709_SCRGB ? 80.0f / (sdr_white > 0.0f ? sdr_white : 300.0f) : 1.0f;
	const float scale = (LUT_SIZE - 1.0f) / LUT_SIZE;
	const float offset = 0.5f / LUT_SIZE;
	struct vec3 v;
	gs_effect_set_texture(filter->lut_param, filter->lut);
	vec3_set(&v, scale, scale, scale);
	gs_effect_set_vec3(filter->lut_scale_param, &v);
	vec3_set(&v, offset, offset, offset);
	gs_effect_set_vec3(filter->lut_offset_param, &v);
	const bool hdr = space == GS_CS_709_EXTENDED || space == GS_CS_709_SCRGB;
	gs_effect_set_float(filter->hdr_param, hdr ? 1.0f : 0.0f);
	gs_effect_set_float(filter->multiplier_param, multiplier);

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
	return working_space(data);
}

static struct obs_source_info invert_filter_info = {
	.id = "grove_invert_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	/* SRGB: OBS hands the effect linear light and encodes what it writes. */
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = invert_get_name,
	.create = invert_create,
	.destroy = invert_destroy,
	.video_render = invert_render,
	.video_get_color_space = invert_get_color_space,
};

bool obs_module_load(void)
{
	lut_thread_started = pthread_create(&lut_thread, NULL, lut_thread_main, NULL) == 0;
	obs_register_source(&invert_filter_info);
	return true;
}

void obs_module_unload(void)
{
	pthread_once(&lut_once, wait_for_lut);
	bfree(lut_data);
	lut_data = NULL;
}
