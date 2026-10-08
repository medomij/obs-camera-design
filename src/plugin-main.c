/*
Camera Design for OBS
Copyright (C) 2026 medomij

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <math.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#define T_(s) obs_module_text(s)

#define S_DEVICE "video_device_id"
#define S_RESOLUTION "resolution"
#define S_FPS "fps"
#define S_MIRROR "mirror"
#define S_ASPECT "aspect"
#define S_CROP_L "crop_left"
#define S_CROP_R "crop_right"
#define S_CROP_T "crop_top"
#define S_CROP_B "crop_bottom"
#define S_OFFSET_X "offset_x"
#define S_OFFSET_Y "offset_y"
#define S_ZOOM "zoom"
#define S_CORNER "corner"

static const char *effect_source =
	"uniform float4x4 ViewProj;\n"
	"uniform texture2d image;\n"
	"uniform float2 out_size;\n"
	"uniform float2 uv_origin;\n"
	"uniform float2 uv_size;\n"
	"uniform float radius;\n"
	"uniform float mirror;\n"
	"\n"
	"sampler_state texSampler {\n"
	"    Filter = Linear;\n"
	"    AddressU = Clamp;\n"
	"    AddressV = Clamp;\n"
	"};\n"
	"\n"
	"struct VertData {\n"
	"    float4 pos : POSITION;\n"
	"    float2 uv : TEXCOORD0;\n"
	"};\n"
	"\n"
	"VertData VSMain(VertData v_in)\n"
	"{\n"
	"    VertData v_out;\n"
	"    v_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);\n"
	"    v_out.uv = v_in.uv;\n"
	"    return v_out;\n"
	"}\n"
	"\n"
	"float4 PSMain(VertData v_in) : TARGET\n"
	"{\n"
	"    float2 half_size = out_size * 0.5;\n"
	"    float2 p = v_in.uv * out_size - half_size;\n"
	"    float2 q = abs(p) - (half_size - float2(radius, radius));\n"
	"    float dist = length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - radius;\n"
	"    float mask = saturate(0.5 - dist);\n"
	"    float2 uv = v_in.uv;\n"
	"    if (mirror > 0.5)\n"
	"        uv.x = 1.0 - uv.x;\n"
	"    uv = uv_origin + uv * uv_size;\n"
	"    float4 c = image.Sample(texSampler, uv);\n"
	"    return float4(c.rgb, c.a * mask);\n"
	"}\n"
	"\n"
	"technique Draw\n"
	"{\n"
	"    pass\n"
	"    {\n"
	"        vertex_shader = VSMain(v_in);\n"
	"        pixel_shader = PSMain(v_in);\n"
	"    }\n"
	"}\n";

static const float aspect_values[] = {0.0f, 16.0f / 9.0f, 9.0f / 16.0f, 1.0f, 4.0f / 3.0f, 3.0f / 4.0f};

struct cd_data {
	obs_source_t *source;
	obs_source_t *child;
	gs_effect_t *effect;
	gs_texrender_t *texrender;
	gs_eparam_t *p_image;
	gs_eparam_t *p_out_size;
	gs_eparam_t *p_uv_origin;
	gs_eparam_t *p_uv_size;
	gs_eparam_t *p_radius;
	gs_eparam_t *p_mirror;

	bool mirror;
	int aspect;
	float crop_l;
	float crop_r;
	float crop_t;
	float crop_b;
	float offset_x;
	float offset_y;
	float zoom;
	float corner;

	uint32_t last_w;
	uint32_t last_h;
};

struct cd_layout {
	uint32_t out_w;
	uint32_t out_h;
	struct vec2 uv_origin;
	struct vec2 uv_size;
	float radius;
};

static float cd_clamp(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static void compute_layout(const struct cd_data *d, uint32_t cw, uint32_t ch, struct cd_layout *l)
{
	float fw = (float)cw;
	float fh = (float)ch;

	float cl = cd_clamp(d->crop_l, 0.0f, 90.0f);
	float cr = cd_clamp(d->crop_r, 0.0f, 90.0f);
	float ct = cd_clamp(d->crop_t, 0.0f, 90.0f);
	float cb = cd_clamp(d->crop_b, 0.0f, 90.0f);
	if (cl + cr > 95.0f) {
		float k = 95.0f / (cl + cr);
		cl *= k;
		cr *= k;
	}
	if (ct + cb > 95.0f) {
		float k = 95.0f / (ct + cb);
		ct *= k;
		cb *= k;
	}

	float rx = cl / 100.0f * fw;
	float ry = ct / 100.0f * fh;
	float rw = fw - (cl + cr) / 100.0f * fw;
	float rh = fh - (ct + cb) / 100.0f * fh;

	float ww = rw;
	float wh = rh;
	int idx = d->aspect;
	if (idx < 0 || idx >= (int)(sizeof(aspect_values) / sizeof(aspect_values[0])))
		idx = 0;
	float target = aspect_values[idx];
	if (target > 0.0f) {
		if (rw / rh > target)
			ww = rh * target;
		else
			wh = rw / target;
	}

	float zoom = cd_clamp(d->zoom / 100.0f, 1.0f, 8.0f);
	float vw = ww / zoom;
	float vh = wh / zoom;
	float ox = cd_clamp(d->offset_x, -100.0f, 100.0f) / 200.0f;
	float oy = cd_clamp(d->offset_y, -100.0f, 100.0f) / 200.0f;
	float vx = rx + (rw - vw) * (0.5f + ox);
	float vy = ry + (rh - vh) * (0.5f + oy);

	l->uv_origin.x = vx / fw;
	l->uv_origin.y = vy / fh;
	l->uv_size.x = vw / fw;
	l->uv_size.y = vh / fh;

	l->out_w = (uint32_t)(ww + 0.5f);
	l->out_h = (uint32_t)(wh + 0.5f);
	if (l->out_w < 2)
		l->out_w = 2;
	if (l->out_h < 2)
		l->out_h = 2;

	float min_side = (float)(l->out_w < l->out_h ? l->out_w : l->out_h);
	l->radius = min_side * 0.5f * cd_clamp(d->corner, 0.0f, 100.0f) / 100.0f;
}

static void get_child_size(struct cd_data *d, uint32_t *w, uint32_t *h)
{
	uint32_t cw = d->child ? obs_source_get_width(d->child) : 0;
	uint32_t ch = d->child ? obs_source_get_height(d->child) : 0;
	if (cw && ch) {
		d->last_w = cw;
		d->last_h = ch;
	}
	*w = d->last_w;
	*h = d->last_h;
}

static const char *cd_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("SourceName");
}

static void cd_update(void *data, obs_data_t *settings)
{
	struct cd_data *d = data;

	d->mirror = obs_data_get_bool(settings, S_MIRROR);
	d->aspect = (int)obs_data_get_int(settings, S_ASPECT);
	d->crop_l = (float)obs_data_get_double(settings, S_CROP_L);
	d->crop_r = (float)obs_data_get_double(settings, S_CROP_R);
	d->crop_t = (float)obs_data_get_double(settings, S_CROP_T);
	d->crop_b = (float)obs_data_get_double(settings, S_CROP_B);
	d->offset_x = (float)obs_data_get_double(settings, S_OFFSET_X);
	d->offset_y = (float)obs_data_get_double(settings, S_OFFSET_Y);
	d->zoom = (float)obs_data_get_double(settings, S_ZOOM);
	d->corner = (float)obs_data_get_double(settings, S_CORNER);

	if (!d->child)
		return;

	obs_data_t *cs = obs_data_create();
	const char *dev = obs_data_get_string(settings, S_DEVICE);
	if (dev && *dev)
		obs_data_set_string(cs, "video_device_id", dev);

	const char *res = obs_data_get_string(settings, S_RESOLUTION);
	if (res && *res && strcmp(res, "auto") != 0) {
		obs_data_set_int(cs, "res_type", 1);
		obs_data_set_string(cs, "resolution", res);
	} else {
		obs_data_set_int(cs, "res_type", 0);
	}

	long long fps = obs_data_get_int(settings, S_FPS);
	if (fps > 0)
		obs_data_set_int(cs, "frame_interval", 10000000LL / fps);
	else
		obs_data_set_int(cs, "frame_interval", 0);

	obs_source_update(d->child, cs);
	obs_data_release(cs);
}

static void *cd_create(obs_data_t *settings, obs_source_t *source)
{
	struct cd_data *d = bzalloc(sizeof(struct cd_data));
	d->source = source;
	d->last_w = 1280;
	d->last_h = 720;

	obs_enter_graphics();
	char *err = NULL;
	d->effect = gs_effect_create(effect_source, "camera_design.effect", &err);
	if (!d->effect)
		obs_log(LOG_ERROR, "failed to create effect: %s", err ? err : "unknown error");
	bfree(err);
	d->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (d->effect) {
		d->p_image = gs_effect_get_param_by_name(d->effect, "image");
		d->p_out_size = gs_effect_get_param_by_name(d->effect, "out_size");
		d->p_uv_origin = gs_effect_get_param_by_name(d->effect, "uv_origin");
		d->p_uv_size = gs_effect_get_param_by_name(d->effect, "uv_size");
		d->p_radius = gs_effect_get_param_by_name(d->effect, "radius");
		d->p_mirror = gs_effect_get_param_by_name(d->effect, "mirror");
	}
	obs_leave_graphics();

	d->child = obs_source_create_private("dshow_input", "camera_design_camera", NULL);
	if (!d->child)
		obs_log(LOG_WARNING, "camera source (dshow_input) is not available on this system");

	cd_update(d, settings);
	return d;
}

static void cd_destroy(void *data)
{
	struct cd_data *d = data;

	if (d->child)
		obs_source_release(d->child);

	obs_enter_graphics();
	gs_texrender_destroy(d->texrender);
	gs_effect_destroy(d->effect);
	obs_leave_graphics();

	bfree(d);
}

static void cd_show(void *data)
{
	struct cd_data *d = data;
	if (d->child)
		obs_source_inc_showing(d->child);
}

static void cd_hide(void *data)
{
	struct cd_data *d = data;
	if (d->child)
		obs_source_dec_showing(d->child);
}

static void cd_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_RESOLUTION, "auto");
	obs_data_set_default_int(settings, S_FPS, 0);
	obs_data_set_default_bool(settings, S_MIRROR, false);
	obs_data_set_default_int(settings, S_ASPECT, 0);
	obs_data_set_default_double(settings, S_CROP_L, 0.0);
	obs_data_set_default_double(settings, S_CROP_R, 0.0);
	obs_data_set_default_double(settings, S_CROP_T, 0.0);
	obs_data_set_default_double(settings, S_CROP_B, 0.0);
	obs_data_set_default_double(settings, S_OFFSET_X, 0.0);
	obs_data_set_default_double(settings, S_OFFSET_Y, 0.0);
	obs_data_set_default_double(settings, S_ZOOM, 100.0);
	obs_data_set_default_double(settings, S_CORNER, 12.0);
}

static void add_device_items(struct cd_data *d, obs_property_t *list)
{
	if (!d->child)
		return;

	obs_properties_t *cp = obs_source_properties(d->child);
	obs_property_t *dev = cp ? obs_properties_get(cp, "video_device_id") : NULL;
	if (dev) {
		size_t n = obs_property_list_item_count(dev);
		for (size_t i = 0; i < n; i++) {
			const char *name = obs_property_list_item_name(dev, i);
			const char *val = obs_property_list_item_string(dev, i);
			if (name && val && *val)
				obs_property_list_add_string(list, name, val);
		}
	}
	obs_properties_destroy(cp);
}

static void add_slider(obs_properties_t *g, const char *key, const char *label, double min, double max)
{
	obs_property_t *p = obs_properties_add_float_slider(g, key, T_(label), min, max, 0.1);
	obs_property_float_set_suffix(p, "%");
}

static obs_properties_t *cd_get_properties(void *data)
{
	struct cd_data *d = data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *g;
	obs_property_t *p;

	g = obs_properties_create();
	p = obs_properties_add_list(g, S_DEVICE, T_("Device"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_device_items(d, p);
	p = obs_properties_add_list(g, S_RESOLUTION, T_("Resolution"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Auto"), "auto");
	obs_property_list_add_string(p, "3840x2160", "3840x2160");
	obs_property_list_add_string(p, "2560x1440", "2560x1440");
	obs_property_list_add_string(p, "1920x1080", "1920x1080");
	obs_property_list_add_string(p, "1280x720", "1280x720");
	obs_property_list_add_string(p, "640x480", "640x480");
	p = obs_properties_add_list(g, S_FPS, T_("Fps"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Auto"), 0);
	obs_property_list_add_int(p, "30", 30);
	obs_property_list_add_int(p, "60", 60);
	obs_properties_add_group(props, "group_camera", T_("GroupCamera"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_bool(g, S_MIRROR, T_("Mirror"));
	p = obs_properties_add_list(g, S_ASPECT, T_("Aspect"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("AspectOriginal"), 0);
	obs_property_list_add_int(p, "16:9", 1);
	obs_property_list_add_int(p, T_("AspectVertical"), 2);
	obs_property_list_add_int(p, "1:1", 3);
	obs_property_list_add_int(p, "4:3", 4);
	obs_property_list_add_int(p, "3:4", 5);
	add_slider(g, S_CROP_L, "CropLeft", 0.0, 90.0);
	add_slider(g, S_CROP_R, "CropRight", 0.0, 90.0);
	add_slider(g, S_CROP_T, "CropTop", 0.0, 90.0);
	add_slider(g, S_CROP_B, "CropBottom", 0.0, 90.0);
	add_slider(g, S_ZOOM, "Zoom", 100.0, 400.0);
	add_slider(g, S_OFFSET_X, "OffsetX", -100.0, 100.0);
	add_slider(g, S_OFFSET_Y, "OffsetY", -100.0, 100.0);
	obs_properties_add_group(props, "group_frame", T_("GroupFrame"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	add_slider(g, S_CORNER, "Corner", 0.0, 100.0);
	obs_properties_add_group(props, "group_corners", T_("GroupCorners"), OBS_GROUP_NORMAL, g);

	return props;
}

static uint32_t cd_get_width(void *data)
{
	struct cd_data *d = data;
	uint32_t cw, ch;
	struct cd_layout l;

	get_child_size(d, &cw, &ch);
	compute_layout(d, cw, ch, &l);
	return l.out_w;
}

static uint32_t cd_get_height(void *data)
{
	struct cd_data *d = data;
	uint32_t cw, ch;
	struct cd_layout l;

	get_child_size(d, &cw, &ch);
	compute_layout(d, cw, ch, &l);
	return l.out_h;
}

static void cd_video_render(void *data, gs_effect_t *unused)
{
	struct cd_data *d = data;
	UNUSED_PARAMETER(unused);

	if (!d->child || !d->effect || !d->texrender)
		return;

	uint32_t cw = obs_source_get_width(d->child);
	uint32_t ch = obs_source_get_height(d->child);
	if (!cw || !ch)
		return;

	struct cd_layout l;
	compute_layout(d, cw, ch, &l);

	gs_texrender_reset(d->texrender);
	if (gs_texrender_begin(d->texrender, cw, ch)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
		obs_source_video_render(d->child);
		gs_texrender_end(d->texrender);
	}

	gs_texture_t *tex = gs_texrender_get_texture(d->texrender);
	if (!tex)
		return;

	struct vec2 out_size;
	vec2_set(&out_size, (float)l.out_w, (float)l.out_h);

	gs_effect_set_texture(d->p_image, tex);
	gs_effect_set_vec2(d->p_out_size, &out_size);
	gs_effect_set_vec2(d->p_uv_origin, &l.uv_origin);
	gs_effect_set_vec2(d->p_uv_size, &l.uv_size);
	gs_effect_set_float(d->p_radius, l.radius);
	gs_effect_set_float(d->p_mirror, d->mirror ? 1.0f : 0.0f);

	while (gs_effect_loop(d->effect, "Draw"))
		gs_draw_sprite(NULL, 0, l.out_w, l.out_h);
}

static struct obs_source_info camera_design_info = {
	.id = "camera_design_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.icon_type = OBS_ICON_TYPE_CAMERA,
	.get_name = cd_get_name,
	.create = cd_create,
	.destroy = cd_destroy,
	.update = cd_update,
	.show = cd_show,
	.hide = cd_hide,
	.get_defaults = cd_get_defaults,
	.get_properties = cd_get_properties,
	.get_width = cd_get_width,
	.get_height = cd_get_height,
	.video_render = cd_video_render,
};

bool obs_module_load(void)
{
	obs_register_source(&camera_design_info);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
