/*
Hymnal Display source

Wraps OBS's built-in text source (text_gdiplus on Windows, text_ft2_source
elsewhere) so it inherits full-fidelity text rendering (fonts, outline,
background, alignment, word wrap) for free, while giving the Hymnal Browser
dock a simple "text" setting to drive with the current verse.

On top of that it draws a lower-third style overlay around the text: a
rounded, drop-shadowed panel sized to the verse, with a darker reference box on
the left holding a short label ("HYMN 39" / "VERSE 1") pushed by the dock. A
handful of colour themes are offered as one-click presets, and every colour
and dimension can still be tuned by hand.
*/

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <graphics/vec4.h>
#include <string.h>

#include "hymnal-source.h"
#include "plugin-support.h"

#define S_TEXT "text"
#define S_ALIGN "align"

#define S_OVERLAY "overlay"
#define S_OVERLAY_THEME "overlay_theme"
#define S_OVERLAY_LABEL "overlay_label"
#define S_OVERLAY_TEXT_COLOR "overlay_text_color"
#define S_OVERLAY_PANEL_COLOR "overlay_panel_color"
#define S_OVERLAY_PANEL_OPACITY "overlay_panel_opacity"
#define S_OVERLAY_BOX_COLOR "overlay_box_color"
#define S_OVERLAY_BOX_OPACITY "overlay_box_opacity"
#define S_OVERLAY_LABEL_COLOR "overlay_label_color"
#define S_OVERLAY_LABEL_SIZE "overlay_label_size"
#define S_OVERLAY_BOX_WIDTH "overlay_box_width"
#define S_OVERLAY_ACCENT "overlay_accent"
#define S_OVERLAY_ACCENT_COLOR "overlay_accent_color"
#define S_OVERLAY_RADIUS "overlay_radius"
#define S_OVERLAY_PADDING "overlay_padding"
#define S_OVERLAY_MIN_WIDTH "overlay_min_width"
#define S_OVERLAY_MIN_HEIGHT "overlay_min_height"
#define S_OVERLAY_SHADOW "overlay_shadow"
#define S_OVERLAY_SHADOW_OPACITY "overlay_shadow_opacity"
#define S_OVERLAY_SHADOW_BLUR "overlay_shadow_blur"
#define S_OVERLAY_SHADOW_OFFSET "overlay_shadow_offset"

#define THEME_CUSTOM "custom"

/* OBS colour pickers hand back 0xAABBGGRR, red in the low byte. */
#define HYMNAL_RGB(r, g, b) (((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

struct hymnal_theme {
	const char *id;
	const char *locale_key;
	uint32_t text;
	uint32_t panel;
	uint32_t panel_opacity;
	uint32_t box;
	uint32_t box_opacity;
	uint32_t label;
	uint32_t accent;
};

static const struct hymnal_theme themes[] = {
	{"rose", "HymnalSource.Theme.Rose", HYMNAL_RGB(0x18, 0x18, 0x20), HYMNAL_RGB(0xD9, 0xB3, 0xAE), 100,
	 HYMNAL_RGB(0x3A, 0x15, 0x12), 100, HYMNAL_RGB(0xF5, 0xEF, 0xEC), HYMNAL_RGB(0xE8, 0xC9, 0xC2)},
	{"ivory", "HymnalSource.Theme.Ivory", HYMNAL_RGB(0x1F, 0x1B, 0x16), HYMNAL_RGB(0xF3, 0xEC, 0xDF), 100,
	 HYMNAL_RGB(0x8A, 0x5A, 0x2B), 100, HYMNAL_RGB(0xFF, 0xF8, 0xEC), HYMNAL_RGB(0xE0, 0xC9, 0xA6)},
	{"midnight", "HymnalSource.Theme.Midnight", HYMNAL_RGB(0xFF, 0xFF, 0xFF), HYMNAL_RGB(0x10, 0x14, 0x1C), 82,
	 HYMNAL_RGB(0xC9, 0xA2, 0x4A), 100, HYMNAL_RGB(0x14, 0x11, 0x0A), HYMNAL_RGB(0xFF, 0xF4, 0xD6)},
	{"slate", "HymnalSource.Theme.Slate", HYMNAL_RGB(0xF4, 0xF6, 0xF8), HYMNAL_RGB(0x2B, 0x32, 0x3B), 90,
	 HYMNAL_RGB(0x4F, 0x8B, 0xD6), 100, HYMNAL_RGB(0xFF, 0xFF, 0xFF), HYMNAL_RGB(0xA9, 0xC7, 0xEE)},
	{"forest", "HymnalSource.Theme.Forest", HYMNAL_RGB(0xF6, 0xF3, 0xEA), HYMNAL_RGB(0x1F, 0x3A, 0x2E), 92,
	 HYMNAL_RGB(0xD8, 0xA2, 0x3A), 100, HYMNAL_RGB(0x1B, 0x15, 0x08), HYMNAL_RGB(0xF1, 0xD8, 0x9A)},
};

static const struct hymnal_theme *default_theme = &themes[0];

enum hymnal_align { HYMNAL_ALIGN_LEFT, HYMNAL_ALIGN_CENTER, HYMNAL_ALIGN_RIGHT };

struct hymnal_color {
	struct vec4 linear;
	struct vec4 srgb;
};

struct hymnal_source {
	obs_source_t *source;
	obs_source_t *child;
	obs_source_t *label;

	bool overlay;
	bool accent;
	bool shadow;
	enum hymnal_align align;
	uint32_t padding;
	uint32_t radius;
	uint32_t box_min_width;
	uint32_t min_width;
	uint32_t min_height;
	uint32_t shadow_blur;
	int shadow_offset;
	bool has_label;

	struct hymnal_color panel_color;
	struct hymnal_color box_color;
	struct hymnal_color accent_color;
	struct hymnal_color shadow_color;

	/* Serialised settings last pushed to the label child, so a verse change
	 * (which arrives as a full settings update) does not force the label to
	 * re-rasterise as well. */
	char *label_json;
};

/* Geometry of one frame, derived from the two child text sources' sizes. */
struct hymnal_layout {
	uint32_t width;
	uint32_t height;
	uint32_t box_width;
	float text_x;
	float text_y;
	float label_x;
	float label_y;
};

/* Shared rounded-rectangle effect; created on first render, freed at unload. */
static gs_effect_t *overlay_effect;
static bool overlay_effect_tried;

static const char *hymnal_text_child_id(void)
{
	const char *id;
	size_t idx = 0;

	while (obs_enum_input_types(idx++, &id)) {
		if (strcmp(id, "text_gdiplus") == 0)
			return "text_gdiplus";
	}

	idx = 0;
	while (obs_enum_input_types(idx++, &id)) {
		if (strcmp(id, "text_ft2_source") == 0)
			return "text_ft2_source";
	}

	return NULL;
}

static const char *hymnal_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("HymnalSource.Name");
}

static obs_data_t *hymnal_copy_settings(obs_data_t *settings)
{
	obs_data_t *copy = obs_data_create();
	obs_data_apply(copy, settings);
	return copy;
}

static void hymnal_set_color(struct hymnal_color *dst, uint32_t color, uint32_t opacity)
{
	uint32_t alpha = (opacity > 100 ? 100 : opacity) * 255 / 100;
	uint32_t rgba = (color & 0xFFFFFF) | (alpha << 24);

	vec4_from_rgba(&dst->linear, rgba);
	vec4_from_rgba_srgb(&dst->srgb, rgba);
}

static gs_effect_t *hymnal_get_effect(void)
{
	if (overlay_effect_tried)
		return overlay_effect;
	overlay_effect_tried = true;

	char *path = obs_module_file("overlay.effect");
	if (!path) {
		obs_log(LOG_WARNING, "Hymnal: overlay.effect not found in module data; using square panels");
		return NULL;
	}

	char *error = NULL;
	overlay_effect = gs_effect_create_from_file(path, &error);
	if (!overlay_effect)
		obs_log(LOG_WARNING, "Hymnal: could not compile overlay.effect (%s); using square panels",
			error ? error : "unknown error");

	bfree(error);
	bfree(path);
	return overlay_effect;
}

void hymnal_source_free_effect(void)
{
	if (!overlay_effect)
		return;

	obs_enter_graphics();
	gs_effect_destroy(overlay_effect);
	obs_leave_graphics();
	overlay_effect = NULL;
}

/*
 * Draws a rectangle of `cx` x `cy` at (x, y). Corner radii are given
 * clockwise from top-left; `feather` > 0 spreads the edge outward by that
 * many pixels (used for the shadow), otherwise the edge is crisp.
 */
static void hymnal_draw_rect(const struct hymnal_color *color, float x, float y, uint32_t cx, uint32_t cy,
			     const struct vec4 *radii, float feather)
{
	if (!cx || !cy)
		return;

	/* The linear path is what keeps translucent panels blending correctly. */
	const bool linear_srgb = gs_get_linear_srgb() || (color->linear.w < 1.0f);
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear_srgb);

	const struct vec4 *value = linear_srgb ? &color->srgb : &color->linear;
	gs_effect_t *effect = hymnal_get_effect();

	gs_matrix_push();

	if (effect) {
		float margin = feather > 0.0f ? feather : 0.0f;
		float softness = feather > 0.0f ? feather : 0.7f;
		struct vec2 size;
		vec2_set(&size, (float)cx, (float)cy);

		gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), value);
		gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "size"), &size);
		gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "radii"), radii);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "margin"), margin);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "softness"), softness);

		gs_matrix_translate3f(x - margin, y - margin, 0.0f);

		uint32_t full_cx = cx + (uint32_t)(margin * 2.0f);
		uint32_t full_cy = cy + (uint32_t)(margin * 2.0f);

		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(0, 0, full_cx, full_cy);
	} else {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), value);

		gs_matrix_translate3f(x, y, 0.0f);

		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(0, 0, cx, cy);
	}

	gs_matrix_pop();

	gs_enable_framebuffer_srgb(previous);
}

static void hymnal_calc_layout(struct hymnal_source *ctx, struct hymnal_layout *out)
{
	memset(out, 0, sizeof(*out));

	uint32_t text_cx = ctx->child ? obs_source_get_width(ctx->child) : 0;
	uint32_t text_cy = ctx->child ? obs_source_get_height(ctx->child) : 0;

	if (!ctx->overlay) {
		out->width = text_cx;
		out->height = text_cy;
		return;
	}

	/*
	 * No verse on screen means the whole overlay collapses, so "Clear" in
	 * the dock really does blank the display instead of leaving an empty
	 * panel behind.
	 */
	if (!text_cx)
		return;

	uint32_t pad = ctx->padding;
	uint32_t label_cx = (ctx->label && ctx->has_label) ? obs_source_get_width(ctx->label) : 0;
	uint32_t label_cy = (ctx->label && ctx->has_label) ? obs_source_get_height(ctx->label) : 0;

	uint32_t box = 0;
	if (label_cx) {
		box = label_cx + pad;
		if (box < ctx->box_min_width)
			box = ctx->box_min_width;
	}

	uint32_t height = text_cy + pad * 2;
	if (label_cy && label_cy + pad * 2 > height)
		height = label_cy + pad * 2;
	if (height < ctx->min_height)
		height = ctx->min_height;

	uint32_t width = box + text_cx + pad * 2;
	if (width < ctx->min_width)
		width = ctx->min_width;

	out->width = width;
	out->height = height;
	out->box_width = box;

	float area_x = (float)(box + pad);
	float area_w = (float)(width - box - pad * 2);
	float slack = area_w - (float)text_cx;

	if (ctx->align == HYMNAL_ALIGN_CENTER)
		out->text_x = area_x + slack / 2.0f;
	else if (ctx->align == HYMNAL_ALIGN_RIGHT)
		out->text_x = area_x + slack;
	else
		out->text_x = area_x;

	out->text_y = (float)(height - text_cy) / 2.0f;
	out->label_x = (float)(box - label_cx) / 2.0f;
	out->label_y = (float)(height - label_cy) / 2.0f;
}

static void hymnal_draw_accent(struct hymnal_source *ctx, const struct hymnal_layout *layout)
{
	uint32_t pad = ctx->padding;
	uint32_t tick_w = pad / 9 + 2;
	uint32_t tick_h = pad / 2 + 4;
	uint32_t gap = tick_w;
	const int count = 5;

	float x = (float)pad / 2.0f;
	float y = (float)pad / 2.0f;

	if (x + (float)(count * (tick_w + gap)) > (float)layout->box_width)
		return;
	if (y + (float)tick_h > (float)layout->height)
		return;

	struct vec4 radii;
	vec4_set(&radii, 1.0f, 1.0f, 1.0f, 1.0f);

	for (int i = 0; i < count; i++)
		hymnal_draw_rect(&ctx->accent_color, x + (float)(i * (int)(tick_w + gap)), y, tick_w, tick_h, &radii,
				 0.0f);
}

/*
 * Builds the settings for the small reference box text. It deliberately does
 * not inherit the verse text's colour/outline/background so the box always
 * looks like part of the overlay artwork.
 */
static obs_data_t *hymnal_label_settings(obs_data_t *settings)
{
	obs_data_t *label_settings = obs_data_create();

	obs_data_t *source_font = obs_data_get_obj(settings, "font");
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", source_font ? obs_data_get_string(source_font, "face") : "Arial");
	obs_data_set_int(font, "size", obs_data_get_int(settings, S_OVERLAY_LABEL_SIZE));
	obs_data_set_int(font, "flags", OBS_FONT_BOLD);
	obs_data_set_string(font, "style", "Bold");
	obs_data_set_obj(label_settings, "font", font);
	obs_data_release(font);
	obs_data_release(source_font);

	uint32_t color = (uint32_t)obs_data_get_int(settings, S_OVERLAY_LABEL_COLOR);

	obs_data_set_string(label_settings, S_TEXT, obs_data_get_string(settings, S_OVERLAY_LABEL));
	obs_data_set_string(label_settings, S_ALIGN, "center");
	obs_data_set_string(label_settings, "valign", "top");
	obs_data_set_int(label_settings, "color", color);
	obs_data_set_int(label_settings, "opacity", 100);
	obs_data_set_int(label_settings, "color1", color | 0xFF000000);
	obs_data_set_int(label_settings, "color2", color | 0xFF000000);
	obs_data_set_bool(label_settings, "gradient", false);
	obs_data_set_bool(label_settings, "outline", false);
	obs_data_set_int(label_settings, "outline_size", 0);
	obs_data_set_int(label_settings, "bk_opacity", 0);
	obs_data_set_bool(label_settings, "extents", false);
	obs_data_set_bool(label_settings, "word_wrap", false);
	obs_data_set_bool(label_settings, "antialiasing", true);
	obs_data_set_int(label_settings, "transform", 1); /* uppercase */

	return label_settings;
}

/*
 * The verse child renders on top of the panel, so while the overlay is on it
 * gets the overlay's text colour and no outline/background of its own.
 */
static obs_data_t *hymnal_child_settings(obs_data_t *settings, bool overlay)
{
	obs_data_t *copy = hymnal_copy_settings(settings);

	if (!overlay)
		return copy;

	uint32_t color = (uint32_t)obs_data_get_int(settings, S_OVERLAY_TEXT_COLOR);

	obs_data_set_int(copy, "color", color);
	obs_data_set_int(copy, "opacity", 100);
	obs_data_set_int(copy, "color1", color | 0xFF000000);
	obs_data_set_int(copy, "color2", color | 0xFF000000);
	obs_data_set_bool(copy, "gradient", false);
	obs_data_set_bool(copy, "outline", false);
	obs_data_set_int(copy, "outline_size", 0);
	obs_data_set_int(copy, "bk_opacity", 0);
	obs_data_set_bool(copy, "extents", false);

	return copy;
}

static void hymnal_update_label(struct hymnal_source *ctx, obs_data_t *settings)
{
	if (!ctx->label)
		return;

	obs_data_t *label_settings = hymnal_label_settings(settings);
	const char *json = obs_data_get_json(label_settings);

	if (!ctx->label_json || strcmp(ctx->label_json, json) != 0) {
		bfree(ctx->label_json);
		ctx->label_json = bstrdup(json);
		obs_source_update(ctx->label, label_settings);
	}

	obs_data_release(label_settings);
}

static void hymnal_source_update(void *data, obs_data_t *settings)
{
	struct hymnal_source *ctx = data;

	ctx->overlay = obs_data_get_bool(settings, S_OVERLAY);
	ctx->accent = obs_data_get_bool(settings, S_OVERLAY_ACCENT);
	ctx->shadow = obs_data_get_bool(settings, S_OVERLAY_SHADOW);
	ctx->padding = (uint32_t)obs_data_get_int(settings, S_OVERLAY_PADDING);
	ctx->radius = (uint32_t)obs_data_get_int(settings, S_OVERLAY_RADIUS);
	ctx->box_min_width = (uint32_t)obs_data_get_int(settings, S_OVERLAY_BOX_WIDTH);
	ctx->min_width = (uint32_t)obs_data_get_int(settings, S_OVERLAY_MIN_WIDTH);
	ctx->min_height = (uint32_t)obs_data_get_int(settings, S_OVERLAY_MIN_HEIGHT);
	ctx->shadow_blur = (uint32_t)obs_data_get_int(settings, S_OVERLAY_SHADOW_BLUR);
	ctx->shadow_offset = (int)obs_data_get_int(settings, S_OVERLAY_SHADOW_OFFSET);

	const char *label_text = obs_data_get_string(settings, S_OVERLAY_LABEL);
	ctx->has_label = label_text && *label_text;

	const char *align = obs_data_get_string(settings, S_ALIGN);
	if (align && strcmp(align, "center") == 0)
		ctx->align = HYMNAL_ALIGN_CENTER;
	else if (align && strcmp(align, "right") == 0)
		ctx->align = HYMNAL_ALIGN_RIGHT;
	else
		ctx->align = HYMNAL_ALIGN_LEFT;

	hymnal_set_color(&ctx->panel_color, (uint32_t)obs_data_get_int(settings, S_OVERLAY_PANEL_COLOR),
			 (uint32_t)obs_data_get_int(settings, S_OVERLAY_PANEL_OPACITY));
	hymnal_set_color(&ctx->box_color, (uint32_t)obs_data_get_int(settings, S_OVERLAY_BOX_COLOR),
			 (uint32_t)obs_data_get_int(settings, S_OVERLAY_BOX_OPACITY));
	hymnal_set_color(&ctx->accent_color, (uint32_t)obs_data_get_int(settings, S_OVERLAY_ACCENT_COLOR), 100);
	hymnal_set_color(&ctx->shadow_color, 0x000000, (uint32_t)obs_data_get_int(settings, S_OVERLAY_SHADOW_OPACITY));

	if (ctx->child) {
		obs_data_t *copy = hymnal_child_settings(settings, ctx->overlay);
		obs_source_update(ctx->child, copy);
		obs_data_release(copy);
	}

	hymnal_update_label(ctx, settings);
}

static void *hymnal_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct hymnal_source *ctx = bzalloc(sizeof(struct hymnal_source));
	ctx->source = source;

	const char *child_id = hymnal_text_child_id();
	if (!child_id) {
		obs_log(LOG_ERROR, "No text source (text_gdiplus / text_ft2_source) is registered; "
				   "Hymnal Display source will not render anything.");
		return ctx;
	}

	obs_data_t *copy = hymnal_child_settings(settings, obs_data_get_bool(settings, S_OVERLAY));
	ctx->child = obs_source_create_private(child_id, "hymnal_text_child", copy);
	obs_data_release(copy);

	obs_data_t *label_settings = hymnal_label_settings(settings);
	ctx->label = obs_source_create_private(child_id, "hymnal_label_child", label_settings);
	ctx->label_json = bstrdup(obs_data_get_json(label_settings));
	obs_data_release(label_settings);

	hymnal_source_update(ctx, settings);

	return ctx;
}

static void hymnal_source_destroy(void *data)
{
	struct hymnal_source *ctx = data;

	if (ctx->label)
		obs_source_release(ctx->label);

	if (ctx->child)
		obs_source_release(ctx->child);

	bfree(ctx->label_json);
	bfree(ctx);
}

static void hymnal_source_activate(void *data)
{
	struct hymnal_source *ctx = data;

	if (ctx->child)
		obs_source_add_active_child(ctx->source, ctx->child);
	if (ctx->label)
		obs_source_add_active_child(ctx->source, ctx->label);
}

static void hymnal_source_deactivate(void *data)
{
	struct hymnal_source *ctx = data;

	if (ctx->child)
		obs_source_remove_active_child(ctx->source, ctx->child);
	if (ctx->label)
		obs_source_remove_active_child(ctx->source, ctx->label);
}

static void hymnal_source_show(void *data)
{
	struct hymnal_source *ctx = data;

	if (ctx->child)
		obs_source_inc_showing(ctx->child);
	if (ctx->label)
		obs_source_inc_showing(ctx->label);
}

static void hymnal_source_hide(void *data)
{
	struct hymnal_source *ctx = data;

	if (ctx->child)
		obs_source_dec_showing(ctx->child);
	if (ctx->label)
		obs_source_dec_showing(ctx->label);
}

static void hymnal_source_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct hymnal_source *ctx = data;

	if (!ctx->child)
		return;

	if (!ctx->overlay) {
		obs_source_video_render(ctx->child);
		return;
	}

	struct hymnal_layout layout;
	hymnal_calc_layout(ctx, &layout);

	if (!layout.width || !layout.height)
		return;

	float r = (float)ctx->radius;
	float max_r = (float)(layout.height < layout.width ? layout.height : layout.width) / 2.0f;
	if (r > max_r)
		r = max_r;

	struct vec4 panel_radii;
	vec4_set(&panel_radii, r, r, r, r);

	if (ctx->shadow && ctx->shadow_blur) {
		hymnal_draw_rect(&ctx->shadow_color, 0.0f, (float)ctx->shadow_offset, layout.width, layout.height,
				 &panel_radii, (float)ctx->shadow_blur);
	}

	hymnal_draw_rect(&ctx->panel_color, 0.0f, 0.0f, layout.width, layout.height, &panel_radii, 0.0f);

	if (layout.box_width) {
		/* The box shares the panel's left corners and butts squarely
		 * against the verse area on the right. */
		struct vec4 box_radii;
		vec4_set(&box_radii, r, 0.0f, 0.0f, r);
		hymnal_draw_rect(&ctx->box_color, 0.0f, 0.0f, layout.box_width, layout.height, &box_radii, 0.0f);

		if (ctx->accent)
			hymnal_draw_accent(ctx, &layout);

		if (ctx->label && ctx->has_label) {
			gs_matrix_push();
			gs_matrix_translate3f(layout.label_x, layout.label_y, 0.0f);
			obs_source_video_render(ctx->label);
			gs_matrix_pop();
		}
	}

	gs_matrix_push();
	gs_matrix_translate3f(layout.text_x, layout.text_y, 0.0f);
	obs_source_video_render(ctx->child);
	gs_matrix_pop();
}

static uint32_t hymnal_source_get_width(void *data)
{
	struct hymnal_source *ctx = data;
	struct hymnal_layout layout;

	hymnal_calc_layout(ctx, &layout);
	return layout.width;
}

static uint32_t hymnal_source_get_height(void *data)
{
	struct hymnal_source *ctx = data;
	struct hymnal_layout layout;

	hymnal_calc_layout(ctx, &layout);
	return layout.height;
}

static const struct hymnal_theme *hymnal_find_theme(const char *id)
{
	if (!id)
		return NULL;

	for (size_t i = 0; i < sizeof(themes) / sizeof(themes[0]); i++) {
		if (strcmp(themes[i].id, id) == 0)
			return &themes[i];
	}

	return NULL;
}

static void hymnal_apply_theme(obs_data_t *settings, const struct hymnal_theme *theme)
{
	obs_data_set_int(settings, S_OVERLAY_TEXT_COLOR, theme->text);
	obs_data_set_int(settings, S_OVERLAY_PANEL_COLOR, theme->panel);
	obs_data_set_int(settings, S_OVERLAY_PANEL_OPACITY, theme->panel_opacity);
	obs_data_set_int(settings, S_OVERLAY_BOX_COLOR, theme->box);
	obs_data_set_int(settings, S_OVERLAY_BOX_OPACITY, theme->box_opacity);
	obs_data_set_int(settings, S_OVERLAY_LABEL_COLOR, theme->label);
	obs_data_set_int(settings, S_OVERLAY_ACCENT_COLOR, theme->accent);
}

static bool hymnal_theme_matches(obs_data_t *settings, const struct hymnal_theme *theme);

/* Picking a theme writes its colours into the individual colour fields. */
static bool hymnal_theme_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	const struct hymnal_theme *theme = hymnal_find_theme(obs_data_get_string(settings, S_OVERLAY_THEME));
	if (!theme || hymnal_theme_matches(settings, theme))
		return false;

	hymnal_apply_theme(settings, theme);
	return true;
}

static bool hymnal_theme_matches(obs_data_t *settings, const struct hymnal_theme *theme)
{
	return (uint32_t)obs_data_get_int(settings, S_OVERLAY_TEXT_COLOR) == theme->text &&
	       (uint32_t)obs_data_get_int(settings, S_OVERLAY_PANEL_COLOR) == theme->panel &&
	       (uint32_t)obs_data_get_int(settings, S_OVERLAY_PANEL_OPACITY) == theme->panel_opacity &&
	       (uint32_t)obs_data_get_int(settings, S_OVERLAY_BOX_COLOR) == theme->box &&
	       (uint32_t)obs_data_get_int(settings, S_OVERLAY_BOX_OPACITY) == theme->box_opacity &&
	       (uint32_t)obs_data_get_int(settings, S_OVERLAY_LABEL_COLOR) == theme->label &&
	       (uint32_t)obs_data_get_int(settings, S_OVERLAY_ACCENT_COLOR) == theme->accent;
}

/*
 * Hand-editing a colour away from the selected preset turns the theme selector
 * back to "Custom". OBS also runs every modified callback when the Properties
 * dialog opens, so this must be a no-op while the colours still match.
 */
static bool hymnal_color_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	const struct hymnal_theme *theme = hymnal_find_theme(obs_data_get_string(settings, S_OVERLAY_THEME));
	if (!theme || hymnal_theme_matches(settings, theme))
		return false;

	obs_data_set_string(settings, S_OVERLAY_THEME, THEME_CUSTOM);
	return true;
}

static obs_property_t *hymnal_add_color(obs_properties_t *props, const char *name, const char *locale_key)
{
	obs_property_t *p = obs_properties_add_color(props, name, obs_module_text(locale_key));
	obs_property_set_modified_callback(p, hymnal_color_modified);
	return p;
}

static obs_property_t *hymnal_add_opacity(obs_properties_t *props, const char *name, const char *locale_key)
{
	obs_property_t *p = obs_properties_add_int_slider(props, name, obs_module_text(locale_key), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_property_set_modified_callback(p, hymnal_color_modified);
	return p;
}

static obs_property_t *hymnal_add_px(obs_properties_t *props, const char *name, const char *locale_key, int min,
				     int max)
{
	obs_property_t *p = obs_properties_add_int(props, name, obs_module_text(locale_key), min, max, 1);
	obs_property_int_set_suffix(p, " px");
	return p;
}

static obs_properties_t *hymnal_source_get_properties(void *data)
{
	struct hymnal_source *ctx = data;

	obs_properties_t *props = (ctx && ctx->child) ? obs_source_properties(ctx->child) : obs_properties_create();

	obs_properties_t *overlay = obs_properties_create();

	obs_property_t *theme = obs_properties_add_list(overlay, S_OVERLAY_THEME,
							obs_module_text("HymnalSource.Overlay.Theme"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < sizeof(themes) / sizeof(themes[0]); i++)
		obs_property_list_add_string(theme, obs_module_text(themes[i].locale_key), themes[i].id);
	obs_property_list_add_string(theme, obs_module_text("HymnalSource.Theme.Custom"), THEME_CUSTOM);
	obs_property_set_modified_callback(theme, hymnal_theme_modified);

	obs_properties_add_text(overlay, S_OVERLAY_LABEL, obs_module_text("HymnalSource.Overlay.Label"),
				OBS_TEXT_MULTILINE);

	hymnal_add_color(overlay, S_OVERLAY_TEXT_COLOR, "HymnalSource.Overlay.TextColor");
	hymnal_add_color(overlay, S_OVERLAY_PANEL_COLOR, "HymnalSource.Overlay.PanelColor");
	hymnal_add_opacity(overlay, S_OVERLAY_PANEL_OPACITY, "HymnalSource.Overlay.PanelOpacity");
	hymnal_add_color(overlay, S_OVERLAY_BOX_COLOR, "HymnalSource.Overlay.BoxColor");
	hymnal_add_opacity(overlay, S_OVERLAY_BOX_OPACITY, "HymnalSource.Overlay.BoxOpacity");
	hymnal_add_color(overlay, S_OVERLAY_LABEL_COLOR, "HymnalSource.Overlay.LabelColor");
	hymnal_add_px(overlay, S_OVERLAY_LABEL_SIZE, "HymnalSource.Overlay.LabelSize", 8, 400);
	hymnal_add_px(overlay, S_OVERLAY_BOX_WIDTH, "HymnalSource.Overlay.BoxWidth", 0, 2000);

	obs_properties_add_bool(overlay, S_OVERLAY_ACCENT, obs_module_text("HymnalSource.Overlay.Accent"));
	hymnal_add_color(overlay, S_OVERLAY_ACCENT_COLOR, "HymnalSource.Overlay.AccentColor");

	hymnal_add_px(overlay, S_OVERLAY_RADIUS, "HymnalSource.Overlay.Radius", 0, 200);
	hymnal_add_px(overlay, S_OVERLAY_PADDING, "HymnalSource.Overlay.Padding", 0, 400);
	hymnal_add_px(overlay, S_OVERLAY_MIN_WIDTH, "HymnalSource.Overlay.MinWidth", 0, 8000);
	hymnal_add_px(overlay, S_OVERLAY_MIN_HEIGHT, "HymnalSource.Overlay.MinHeight", 0, 4000);

	obs_properties_add_bool(overlay, S_OVERLAY_SHADOW, obs_module_text("HymnalSource.Overlay.Shadow"));
	obs_property_t *shadow_opacity = obs_properties_add_int_slider(
		overlay, S_OVERLAY_SHADOW_OPACITY, obs_module_text("HymnalSource.Overlay.ShadowOpacity"), 0, 100, 1);
	obs_property_int_set_suffix(shadow_opacity, "%");
	hymnal_add_px(overlay, S_OVERLAY_SHADOW_BLUR, "HymnalSource.Overlay.ShadowBlur", 0, 200);
	hymnal_add_px(overlay, S_OVERLAY_SHADOW_OFFSET, "HymnalSource.Overlay.ShadowOffset", -200, 200);

	obs_properties_add_group(props, S_OVERLAY, obs_module_text("HymnalSource.Overlay"), OBS_GROUP_CHECKABLE,
				 overlay);

	return props;
}

static void hymnal_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_OVERLAY, true);
	obs_data_set_default_string(settings, S_OVERLAY_THEME, default_theme->id);
	obs_data_set_default_string(settings, S_OVERLAY_LABEL, "HYMN");
	obs_data_set_default_int(settings, S_OVERLAY_TEXT_COLOR, default_theme->text);
	obs_data_set_default_int(settings, S_OVERLAY_PANEL_COLOR, default_theme->panel);
	obs_data_set_default_int(settings, S_OVERLAY_PANEL_OPACITY, default_theme->panel_opacity);
	obs_data_set_default_int(settings, S_OVERLAY_BOX_COLOR, default_theme->box);
	obs_data_set_default_int(settings, S_OVERLAY_BOX_OPACITY, default_theme->box_opacity);
	obs_data_set_default_int(settings, S_OVERLAY_LABEL_COLOR, default_theme->label);
	obs_data_set_default_int(settings, S_OVERLAY_LABEL_SIZE, 30);
	obs_data_set_default_int(settings, S_OVERLAY_BOX_WIDTH, 170);
	obs_data_set_default_bool(settings, S_OVERLAY_ACCENT, true);
	obs_data_set_default_int(settings, S_OVERLAY_ACCENT_COLOR, default_theme->accent);
	obs_data_set_default_int(settings, S_OVERLAY_RADIUS, 18);
	obs_data_set_default_int(settings, S_OVERLAY_PADDING, 30);
	obs_data_set_default_int(settings, S_OVERLAY_MIN_WIDTH, 0);
	obs_data_set_default_int(settings, S_OVERLAY_MIN_HEIGHT, 0);
	obs_data_set_default_bool(settings, S_OVERLAY_SHADOW, true);
	obs_data_set_default_int(settings, S_OVERLAY_SHADOW_OPACITY, 45);
	obs_data_set_default_int(settings, S_OVERLAY_SHADOW_BLUR, 28);
	obs_data_set_default_int(settings, S_OVERLAY_SHADOW_OFFSET, 10);

	const char *child_id = hymnal_text_child_id();
	if (!child_id || strcmp(child_id, "text_gdiplus") != 0)
		return;

	/*
	 * text_gdiplus's own defaults live in a separate "default value" layer
	 * that obs_source_get_settings()/obs_data_apply() do not surface, so
	 * they have to be replicated here explicitly (matching
	 * plugins/obs-text/gdiplus/obs-text.cpp's own defaults()) rather than
	 * queried from a throwaway child instance.
	 */
	obs_data_t *font_obj = obs_data_create();
	obs_data_set_default_string(font_obj, "face", "Arial");
	obs_data_set_default_int(font_obj, "size", 36);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);

	obs_data_set_default_string(settings, S_ALIGN, "left");
	obs_data_set_default_string(settings, "valign", "top");
	obs_data_set_default_int(settings, "color", 0xFFFFFF);
	obs_data_set_default_int(settings, "opacity", 100);
	obs_data_set_default_int(settings, "gradient_color", 0xFFFFFF);
	obs_data_set_default_int(settings, "gradient_opacity", 100);
	obs_data_set_default_double(settings, "gradient_dir", 90.0);
	obs_data_set_default_int(settings, "bk_color", 0x000000);
	obs_data_set_default_int(settings, "bk_opacity", 0);
	obs_data_set_default_int(settings, "outline_size", 2);
	obs_data_set_default_int(settings, "outline_color", 0xFFFFFF);
	obs_data_set_default_int(settings, "outline_opacity", 100);
	obs_data_set_default_int(settings, "chatlog_lines", 6);
	obs_data_set_default_bool(settings, "extents_wrap", true);
	obs_data_set_default_int(settings, "extents_cx", 100);
	obs_data_set_default_int(settings, "extents_cy", 100);
	obs_data_set_default_int(settings, "transform", 0);
	obs_data_set_default_bool(settings, "antialiasing", true);
}

struct obs_source_info hymnal_text_source_info = {
	.id = "hymnal_text_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = hymnal_source_get_name,
	.create = hymnal_source_create,
	.destroy = hymnal_source_destroy,
	.update = hymnal_source_update,
	.get_defaults = hymnal_source_get_defaults,
	.get_properties = hymnal_source_get_properties,
	.activate = hymnal_source_activate,
	.deactivate = hymnal_source_deactivate,
	.show = hymnal_source_show,
	.hide = hymnal_source_hide,
	.video_render = hymnal_source_video_render,
	.get_width = hymnal_source_get_width,
	.get_height = hymnal_source_get_height,
	.icon_type = OBS_ICON_TYPE_TEXT,
};
