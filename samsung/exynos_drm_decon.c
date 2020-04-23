// SPDX-License-Identifier: GPL-2.0-only
/* exynos_drm_decon.c
 *
 * Copyright (C) 2018 Samsung Electronics Co.Ltd
 * Authors:
 *	Hyung-jun Kim <hyungjun07.kim@samsung.com>
 *	Seong-gyu Park <seongyu.park@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */
#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/exynos_drm.h>
#include <drm/exynos_display_common.h>

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/pm_runtime.h>
#include <linux/console.h>
#include <linux/iommu.h>
#include <linux/exynos_iovmm.h>

#include <video/videomode.h>

#include <exynos_drm_crtc.h>
#include <exynos_drm_plane.h>
#include <exynos_drm_dpp.h>
#include <exynos_drm_dsim.h>
#include <exynos_drm_drv.h>
#include <exynos_drm_fb.h>
#include <exynos_drm_decon.h>

#include <decon_cal.h>
#include <regs-decon.h>

struct decon_device *decon_drvdata[MAX_DECON_CNT];

static int decon_log_level = 6;

#define decon_info(decon, fmt, ...)					   \
	do {								   \
		if (decon_log_level >= 6) {				   \
			DRM_INFO("%s[%d]: "fmt, decon->dev->driver->name,  \
					decon->id, ##__VA_ARGS__);	   \
		}							   \
	} while (0)

#define decon_warn(decon, fmt, ...)					   \
	do {								   \
		if (decon_log_level >= 4) {				   \
			DRM_WARN("%s[%d]: "fmt, decon->dev->driver->name,  \
					decon->id, ##__VA_ARGS__);	   \
		}							   \
	} while (0)

#define decon_err(decon, fmt, ...)					   \
	do {								   \
		if (decon_log_level >= 3) {				   \
			DRM_ERROR("%s[%d]: "fmt, decon->dev->driver->name, \
					decon->id, ##__VA_ARGS__);	   \
		}							   \
	} while (0)

#define decon_dbg(decon, fmt, ...)					   \
	do {								   \
		if (decon_log_level >= 7) {				   \
			DRM_INFO("%s[%d]: "fmt, decon->dev->driver->name,  \
					decon->id, ##__VA_ARGS__);	   \
		}							   \
	} while (0)

#define SHADOW_UPDATE_TIMEOUT_US	(300 * USEC_PER_MSEC) /* 300ms */

static const struct of_device_id decon_driver_dt_match[] = {
	{.compatible = "samsung,exynos-decon"},
	{},
};
MODULE_DEVICE_TABLE(of, decon_driver_dt_match);

void decon_dump(struct decon_device *decon)
{
	int i;
	int acquired = console_trylock();
	struct decon_device *d;

	for (i = 0; i < REGS_DECON_ID_MAX; ++i) {
		d = get_decon_drvdata(i);
		if (!d)
			continue;

		if (d->state != DECON_STATE_ON) {
			decon_info(decon, "DECON disabled(%d)\n", decon->state);
			continue;
		}

		__decon_dump(d->id, &d->regs, d->config.dsc.enabled);
	}

	for (i = 0; i < decon->dpp_cnt; ++i)
		dpp_dump(decon->dpp[i]);

	if (acquired)
		console_unlock();
}

static inline u32 win_start_pos(int x, int y)
{
	return (WIN_STRPTR_Y_F(y) | WIN_STRPTR_X_F(x));
}

static inline u32 win_end_pos(int x, int y,  u32 xres, u32 yres)
{
	return (WIN_ENDPTR_Y_F(y + yres - 1) | WIN_ENDPTR_X_F(x + xres - 1));
}

/* ARGB value */
#define COLOR_MAP_VALUE			0x00340080

/*
 * This function is used to disable all windows and make black frame via
 * decon on the first frame after enabling.
 */
static void decon_set_color_map(struct decon_device *decon, u32 win_id,
						u32 hactive, u32 vactive)
{
	struct decon_window_regs win_info;
	int i;

	decon_dbg(decon, "%s +\n", __func__);

	for (i = 0; i < MAX_WIN_PER_DECON; ++i)
		decon_reg_set_win_enable(decon->id, i, 0);

	memset(&win_info, 0, sizeof(struct decon_window_regs));
	win_info.start_pos = win_start_pos(0, 0);
	win_info.end_pos = win_end_pos(0, 0, hactive, vactive);
	win_info.start_time = 0;
	win_info.colormap = 0x000000; /* black */
	win_info.blend = DECON_BLENDING_NONE;
	decon_reg_set_window_control(decon->id, win_id, &win_info, true);
	decon_reg_update_req_window(decon->id, win_id);

	decon_dbg(decon, "%s -\n", __func__);
}

static int decon_enable_vblank(struct exynos_drm_crtc *crtc)
{
	struct decon_device *decon = crtc->ctx;

	/* TODO : need to write code completely */
	decon_dbg(decon, "%s\n", __func__);

	return 0;
}

static void decon_disable_vblank(struct exynos_drm_crtc *crtc)
{
	struct decon_device *decon = crtc->ctx;

	/* TODO : need to write code completely */
	decon_dbg(decon, "%s\n", __func__);

}

static bool has_writeback_job(struct drm_crtc_state *new_crtc_state)
{
	int i;
	struct drm_atomic_state *state = new_crtc_state->state;
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;

	for_each_new_connector_in_state(state, conn, conn_state, i) {
		if (!(new_crtc_state->connector_mask &
					drm_connector_mask(conn)))
			continue;

		if (conn_state->writeback_job && conn_state->writeback_job->fb)
			return true;
	}
	return false;
}

static int decon_atomic_check(struct exynos_drm_crtc *exynos_crtc,
		struct drm_crtc_state *state)
{
	const struct decon_device *decon = exynos_crtc->ctx;
	bool is_wb = has_writeback_job(state);
	bool is_swb = decon->config.out_type == DECON_OUT_WB;
	struct exynos_drm_crtc_state *exynos_crtc_state =
					to_exynos_crtc_state(state);

	if (is_wb)
		exynos_crtc_state->wb_type =
			is_swb ? EXYNOS_WB_SWB : EXYNOS_WB_CWB;
	else
		exynos_crtc_state->wb_type = EXYNOS_WB_NONE;

	if (is_swb)
		state->no_vblank = true;

	return 0;
}

static void decon_atomic_begin(struct exynos_drm_crtc *crtc)
{
	struct decon_device *decon = crtc->ctx;

	decon_dbg(decon, "%s +\n", __func__);
	DPU_EVENT_LOG(DPU_EVT_ATOMIC_BEGIN, decon->id, NULL);
	decon_reg_wait_update_done_and_mask(decon->id, &decon->config.mode,
			SHADOW_UPDATE_TIMEOUT_US);
	decon_dbg(decon, "%s -\n", __func__);
}

static void decon_update_plane(struct exynos_drm_crtc *crtc,
			       struct exynos_drm_plane *plane)
{
	struct exynos_drm_plane_state *state =
				to_exynos_plane_state(plane->base.state);
	struct dpp_device *dpp = plane_to_dpp(plane);
	struct decon_device *decon = crtc->ctx;
	struct decon_window_regs win_info;
	unsigned int zpos;
	bool is_colormap = false;
	u16 hw_alpha;

	decon_dbg(decon, "%s +\n", __func__);

	memset(&win_info, 0, sizeof(struct decon_window_regs));

	is_colormap = state->base.fb &&
			exynos_drm_fb_is_colormap(state->base.fb);
	if (is_colormap)
		win_info.colormap = exynos_drm_fb_dma_addr(state->base.fb, 0);

	win_info.start_pos = win_start_pos(state->crtc.x, state->crtc.y);
	win_info.end_pos = win_end_pos(state->crtc.x, state->crtc.y,
			state->crtc.w, state->crtc.h);
	win_info.start_time = 0;

	win_info.ch = dpp->id; /* DPP's id is DPP channel number */

	hw_alpha = DIV_ROUND_CLOSEST(state->base.alpha * EXYNOS_PLANE_ALPHA_MAX,
			DRM_BLEND_ALPHA_OPAQUE);
	win_info.plane_alpha = hw_alpha;
	win_info.blend = state->base.pixel_blend_mode;

	zpos = state->base.normalized_zpos;
	if (zpos == 0 && hw_alpha == EXYNOS_PLANE_ALPHA_MAX)
		win_info.blend = DRM_MODE_BLEND_PIXEL_NONE;

	decon_reg_set_window_control(decon->id, zpos, &win_info, is_colormap);

	if (!is_colormap) {
		dpp->decon_id = decon->id;
		dpp->update(dpp, state);
		dpp->is_win_connected = true;
	} else {
		dpp->disable(dpp);
		dpp->is_win_connected = false;
	}

	dpp->win_id = zpos;

	DPU_EVENT_LOG(DPU_EVT_PLANE_UPDATE, decon->id, dpp);
	decon_dbg(decon, "plane idx[%d]: alpha(0x%x) hw alpha(0x%x)\n",
			drm_plane_index(&plane->base), state->base.alpha,
			hw_alpha);
	decon_dbg(decon, "blend_mode(%d) color(%s:0x%x)\n", win_info.blend,
			is_colormap ? "enable" : "disable", win_info.colormap);
	decon_dbg(decon, "%s -\n", __func__);
}

static void decon_disable_plane(struct exynos_drm_crtc *exynos_crtc,
				struct exynos_drm_plane *exynos_plane)
{
	struct decon_device *decon = exynos_crtc->ctx;
	struct dpp_device *dpp = plane_to_dpp(exynos_plane);
	const struct drm_plane *plane = &exynos_plane->base;
	const struct drm_crtc *crtc = &exynos_crtc->base;
	const unsigned int num_planes = hweight32(crtc->state->plane_mask);

	decon_dbg(decon, "%s +\n", __func__);

	pr_debug("%s win_id(%d/%d) zpos(%d) is_win_connected(%d) visible(%d)\n",
		 plane->name, dpp->win_id, num_planes,
		 plane->state->normalized_zpos, dpp->is_win_connected,
		 plane->state->visible);

	/*
	 * When disabling the plane, previously connected window(zpos) should be
	 * disabled not newly requested zpos(window). Only disable window if it
	 * was previously connected and it's not going to be used by any other
	 * plane, by using normalized zpos as win_id we know that any win_id
	 * beyond the number of planes will not be used.
	 */
	if (dpp->win_id < MAX_PLANE && dpp->win_id >= num_planes)
		decon_reg_set_win_enable(decon->id, dpp->win_id, 0);

	/*
	 * This can be called when changing zpos. Only disable dpp if plane is
	 * not going to be visible anymore (with different win_id)
	 */
	if (dpp->is_win_connected &&
	    (!plane->state->visible || !plane->state->crtc)) {
		dpp->decon_id = decon->id;
		dpp->disable(dpp);
		dpp->is_win_connected = false;
	}
	DPU_EVENT_LOG(DPU_EVT_PLANE_DISABLE, decon->id, dpp);
	decon_dbg(decon, "%s -\n", __func__);
}

static void decon_atomic_flush(struct exynos_drm_crtc *exynos_crtc,
		struct drm_crtc_state *old_crtc_state)
{
	struct decon_device *decon = exynos_crtc->ctx;
	struct drm_crtc_state *new_crtc_state = exynos_crtc->base.state;
	struct exynos_drm_crtc_state *new_exynos_crtc_state =
					to_exynos_crtc_state(new_crtc_state);
	struct exynos_drm_crtc_state *old_exynos_crtc_state =
					to_exynos_crtc_state(old_crtc_state);

	decon_dbg(decon, "%s +\n", __func__);

	if (new_exynos_crtc_state->wb_type == EXYNOS_WB_NONE &&
			decon->config.out_type == DECON_OUT_WB)
		return;

	if (new_exynos_crtc_state->wb_type == EXYNOS_WB_CWB)
		decon_reg_set_cwb_enable(decon->id, true);
	else if (old_exynos_crtc_state->wb_type == EXYNOS_WB_CWB)
		decon_reg_set_cwb_enable(decon->id, false);

	/* if there are no planes attached, enable colormap as fallback */
	if (new_crtc_state->plane_mask == 0) {
		decon_dbg(decon, "no planes, enable color map\n");

		decon_set_color_map(decon, 0, decon->config.image_width,
				decon->config.image_height);
	}

	decon_reg_all_win_shadow_update_req(decon->id);
	decon_reg_start(decon->id, &decon->config);

	if (!new_crtc_state->no_vblank)
		exynos_crtc_handle_event(exynos_crtc);

	reinit_completion(&decon->framestart_done);
	DPU_EVENT_LOG(DPU_EVT_ATOMIC_FLUSH, decon->id, NULL);
	decon_dbg(decon, "%s -\n", __func__);
}

static void decon_print_config_info(struct decon_device *decon)
{
	char *str_output = NULL;
	char *str_trigger = NULL;

	if (decon->config.mode.trig_mode == DECON_HW_TRIG)
		str_trigger = "hw trigger.";
	else if (decon->config.mode.trig_mode == DECON_SW_TRIG)
		str_trigger = "sw trigger.";
	if (decon->config.mode.op_mode == DECON_VIDEO_MODE)
		str_trigger = "";

	if (decon->config.out_type == DECON_OUT_DSI)
		str_output = "Dual DSI";
	else if (decon->config.out_type & DECON_OUT_DSI0)
		str_output = "DSI0";
	else if  (decon->config.out_type & DECON_OUT_DSI1)
		str_output = "DSI1";
	else if  (decon->config.out_type & DECON_OUT_DP0)
		str_output = "DP0";
	else if  (decon->config.out_type & DECON_OUT_DP1)
		str_output = "DP1";
	else if  (decon->config.out_type & DECON_OUT_WB)
		str_output = "WB";

	decon_info(decon, "%s mode. %s %s output.(%dx%d@%dhz)\n",
			decon->config.mode.op_mode ? "command" : "video",
			str_trigger, str_output,
			decon->config.image_width, decon->config.image_height,
			decon->bts.fps);
}

static void decon_set_te_pinctrl(struct decon_device *decon, bool en)
{
	int ret;

	if ((decon->config.mode.op_mode != DECON_MIPI_COMMAND_MODE) ||
			(decon->config.mode.trig_mode != DECON_HW_TRIG))
		return;

	if (!decon->res.pinctrl || !decon->res.te_on)
		return;

	ret = pinctrl_select_state(decon->res.pinctrl,
			en ? decon->res.te_on : decon->res.te_off);
	if (ret)
		decon_err(decon, "failed to control decon TE(%d)\n", en);
}

static void decon_enable_irqs(struct decon_device *decon)
{
	decon_reg_set_interrupts(decon->id, 1);

	enable_irq(decon->irq_fs);
	enable_irq(decon->irq_fd);
	enable_irq(decon->irq_ext);
	if ((decon->config.mode.op_mode == DECON_MIPI_COMMAND_MODE) &&
			(decon->config.mode.trig_mode == DECON_HW_TRIG))
		enable_irq(decon->irq_te);
}

static void _decon_enable(struct decon_device *decon)
{
	decon_reg_init(decon->id, &decon->config);
	decon_enable_irqs(decon);
}

static bool decon_mode_fixup(struct exynos_drm_crtc *crtc,
			const struct drm_display_mode *mode,
			struct drm_display_mode *adjusted_mode)
{
	const struct exynos_display_mode *mode_priv;
	struct decon_device *decon = crtc->ctx;

	mode_priv = drm_mode_to_exynos(adjusted_mode);
	if (!mode_priv)
		return true;

	if (!(mode_priv->mode_flags & MIPI_DSI_MODE_VIDEO)) {
		if (!decon->irq_te || !decon->res.pinctrl) {
			decon_err(decon, "TE error: irq_te %p, te_pinctrl %p\n",
				  decon->irq_te, decon->res.pinctrl);

			return false;
		}
	}

	return true;
}

static void decon_update_config_for_display_mode(struct exynos_drm_crtc *crtc)
{
	struct decon_device *decon = crtc->ctx;
	const struct drm_display_mode *mode = &crtc->base.state->adjusted_mode;
	const struct exynos_display_mode *mode_priv =
						      drm_mode_to_exynos(mode);

	if (!mode_priv) {
		decon_info(decon, "%s: no private mode config\n", __func__);
		return;
	}

	decon->config.dsc.enabled = mode_priv->dsc.enabled;
	if (mode_priv->dsc.enabled) {
		decon->config.dsc.dsc_count = mode_priv->dsc.dsc_count;
		decon->config.dsc.slice_count = mode_priv->dsc.slice_count;
		decon->config.dsc.slice_height = mode_priv->dsc.slice_height;
	}

	decon->config.mode.op_mode =
		(mode_priv->mode_flags & MIPI_DSI_MODE_VIDEO) ?
			DECON_VIDEO_MODE : DECON_MIPI_COMMAND_MODE;

	decon->config.out_bpc = mode_priv->bpc;
}

static void decon_enable(struct exynos_drm_crtc *crtc)
{
	struct decon_device *decon = crtc->ctx;
	int i;

	if (decon->state == DECON_STATE_ON) {
		decon_info(decon, "decon%d already enabled(%d)\n",
				decon->id, decon->state);
		return;
	}

	decon_info(decon, "%s +\n", __func__);

	if (crtc->base.state->mode_changed)
		decon_update_config_for_display_mode(crtc);
	pm_runtime_get_sync(decon->dev);

	decon_set_te_pinctrl(decon, true);

	_decon_enable(decon);

	if ((decon->config.mode.op_mode == DECON_MIPI_COMMAND_MODE) &&
			(decon->config.out_type & DECON_OUT_DSI)) {
		decon_set_color_map(decon, 0, decon->config.image_width,
				decon->config.image_height);
		decon_reg_start(decon->id, &decon->config);
	}

	decon_print_config_info(decon);

	for (i = 0; i < MAX_PLANE; ++i)
		decon->dpp[i]->win_id = 0xFF;

	decon->state = DECON_STATE_ON;

	DPU_EVENT_LOG(DPU_EVT_DECON_ENABLED, decon->id, decon);

	decon_info(decon, "%s -\n", __func__);
}

void decon_exit_hibernation(struct decon_device *decon)
{
	if (decon->state != DECON_STATE_HIBERNATION)
		return;

	pr_debug("%s +\n", __func__);

	_decon_enable(decon);

	decon->state = DECON_STATE_ON;

	pr_debug("%s -\n", __func__);
}

static void decon_disable_irqs(struct decon_device *decon)
{
	disable_irq(decon->irq_fs);
	disable_irq(decon->irq_fd);
	disable_irq(decon->irq_ext);
	decon_reg_set_interrupts(decon->id, 0);
	if ((decon->config.mode.op_mode == DECON_MIPI_COMMAND_MODE) &&
			(decon->config.mode.trig_mode == DECON_HW_TRIG))
		disable_irq(decon->irq_te);
}

static void _decon_disable(struct decon_device *decon)
{
	int i;

	decon_disable_irqs(decon);
	decon_reg_stop(decon->id, &decon->config, true, decon->bts.fps);

	for (i = 0; i < decon->dpp_cnt; ++i) {
		struct dpp_device *dpp = decon->dpp[i];

		if (!dpp)
			continue;

		dpp->disable(dpp);
	}
}

void decon_enter_hibernation(struct decon_device *decon)
{
	pr_debug("%s +\n", __func__);

	if (decon->state != DECON_STATE_ON)
		return;

	_decon_disable(decon);

	decon->state = DECON_STATE_HIBERNATION;
	pr_debug("%s -\n", __func__);
}

static void decon_disable(struct exynos_drm_crtc *crtc)
{
	struct decon_device *decon = crtc->ctx;

	pr_info("%s +\n", __func__);

	_decon_disable(decon);

	decon_set_te_pinctrl(decon, false);

	pm_runtime_put_sync(decon->dev);

	decon->state = DECON_STATE_OFF;

	DPU_EVENT_LOG(DPU_EVT_DECON_DISABLED, decon->id, decon);

	pr_info("%s -\n", __func__);
}

static const struct exynos_drm_crtc_ops decon_crtc_ops = {
	.enable = decon_enable,
	.disable = decon_disable,
	.enable_vblank = decon_enable_vblank,
	.disable_vblank = decon_disable_vblank,
	.mode_fixup = decon_mode_fixup,
	.atomic_check = decon_atomic_check,
	.atomic_begin = decon_atomic_begin,
	.update_plane = decon_update_plane,
	.disable_plane = decon_disable_plane,
	.atomic_flush = decon_atomic_flush,
};

static int dpu_sysmmu_fault_handler(struct iommu_domain *domain,
	struct device *dev, unsigned long iova, int flags, void *token)
{
	pr_info("%s +\n", __func__);
	return 0;
}

static int decon_bind(struct device *dev, struct device *master, void *data)
{
	struct decon_device *decon = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	struct exynos_drm_private *priv = drm_dev->dev_private;
	struct drm_plane *default_plane;
	struct exynos_drm_plane_config plane_config;
	int i, ret = 0;

	decon->drm_dev = drm_dev;

	/* plane initialization in DPP channel order */
	if (decon->config.out_type & DECON_OUT_DSI) {
		for (i = 0; i < decon->dpp_cnt; ++i) {
			struct dpp_device *dpp = decon->dpp[i];

			if (!dpp)
				continue;

			memset(&plane_config, 0, sizeof(plane_config));

			plane_config.pixel_formats = dpp->pixel_formats;
			plane_config.num_pixel_formats = dpp->num_pixel_formats;
			plane_config.zpos = i;
			plane_config.type = (i == 0) ? DRM_PLANE_TYPE_PRIMARY :
				DRM_PLANE_TYPE_OVERLAY;
			if (dpp->is_support & DPP_SUPPORT_AFBC)
				plane_config.capabilities |=
					EXYNOS_DRM_PLANE_CAP_AFBC;

			ret = exynos_plane_init(drm_dev, &dpp->plane, i,
					&plane_config);
			if (ret)
				return ret;
		}
	}

	default_plane = &decon->dpp[decon->id]->plane.base;

	decon->crtc = exynos_drm_crtc_create(drm_dev, default_plane,
			decon->con_type, &decon_crtc_ops, decon);
	if (IS_ERR(decon->crtc))
		return PTR_ERR(decon->crtc);

	for (i = 0; i < decon->dpp_cnt; ++i) {
		struct dpp_device *dpp = decon->dpp[i];
		struct drm_plane *plane = &dpp->plane.base;

		plane->possible_crtcs |=
			drm_crtc_mask(&decon->crtc->base);
		pr_debug("plane possible_crtcs = 0x%x\n",
				plane->possible_crtcs);
	}

	ret = iovmm_activate(dev);
	if (ret) {
		pr_err("failed to activate iovmm\n");
		return ret;
	}
	priv->iommu_client = dev;

	iovmm_set_fault_handler(dev, dpu_sysmmu_fault_handler, NULL);

	decon_dbg(decon, "%s -\n", __func__);
	return 0;
}

static void decon_unbind(struct device *dev, struct device *master,
			void *data)
{
	struct decon_device *decon = dev_get_drvdata(dev);

	decon_dbg(decon, "%s +\n", __func__);
	decon_disable(decon->crtc);
	decon_dbg(decon, "%s -\n", __func__);
}

static const struct component_ops decon_component_ops = {
	.bind	= decon_bind,
	.unbind = decon_unbind,
};

static irqreturn_t decon_irq_handler(int irq, void *dev_data)
{
	struct decon_device *decon = dev_data;
	u32 irq_sts_reg;
	u32 ext_irq = 0;

	spin_lock(&decon->slock);
	if (decon->state != DECON_STATE_ON)
		goto irq_end;

	irq_sts_reg = decon_reg_get_interrupt_and_clear(decon->id, &ext_irq);
	decon_dbg(decon, "%s: irq_sts_reg = %x, ext_irq = %x\n", __func__,
			irq_sts_reg, ext_irq);

	if (irq_sts_reg & DPU_FRAME_START_INT_PEND) {
		complete(&decon->framestart_done);
		DPU_EVENT_LOG(DPU_EVT_DECON_FRAMESTART, decon->id, decon);
		decon_dbg(decon, "%s: frame start\n", __func__);
	}

	if (irq_sts_reg & DPU_FRAME_DONE_INT_PEND) {
		DPU_EVENT_LOG(DPU_EVT_DECON_FRAMEDONE, decon->id, decon);
		decon_dbg(decon, "%s: frame done\n", __func__);
	}

	if (ext_irq & DPU_RESOURCE_CONFLICT_INT_PEND)
		decon_dbg(decon, "%s: resource conflict\n", __func__);

	if (ext_irq & DPU_TIME_OUT_INT_PEND) {
		decon_err(decon, "%s: timeout irq occurs\n", __func__);
		decon_dump(decon);
		WARN_ON(1);
	}

irq_end:
	spin_unlock(&decon->slock);
	return IRQ_HANDLED;
}

static int decon_parse_dt(struct decon_device *decon, struct device_node *np)
{
	struct device_node *dpp_np = NULL;
	struct property *prop;
	const __be32 *cur;
	u32 val;
	int ret = 0, i;
	int dpp_id;

	of_property_read_u32(np, "decon,id", &decon->id);

	ret = of_property_read_u32(np, "max_win", &decon->win_cnt);
	if (ret) {
		decon_err(decon, "failed to parse max windows count\n");
		return ret;
	}

	ret = of_property_read_u32(np, "op_mode", &decon->config.mode.op_mode);
	if (ret) {
		decon_err(decon, "failed to parse operation mode(%d)\n", ret);
		return ret;
	}

	ret = of_property_read_u32(np, "trig_mode",
			&decon->config.mode.trig_mode);
	if (ret) {
		decon_err(decon, "failed to parse trigger mode(%d)\n", ret);
		return ret;
	}

	ret = of_property_read_u32(np, "out_type", &decon->config.out_type);
	if (ret) {
		decon_err(decon, "failed to parse output type(%d)\n", ret);
		return ret;
	}

	if (decon->config.mode.trig_mode == DECON_HW_TRIG) {
		ret = of_property_read_u32(np, "te_from",
				&decon->config.te_from);
		if (ret) {
			pr_err("failed to get value of TE from DDI\n");
			return ret;
		}
		if (decon->config.te_from >= MAX_DECON_TE_FROM_DDI) {
			pr_err("TE from DDI is wrong(%d)\n",
					decon->config.te_from);
			return ret;
		}
		pr_info("DECON TE from DDI%d\n", decon->config.te_from);
	} else {
		decon->config.te_from = MAX_DECON_TE_FROM_DDI;
		pr_info("DECON TE from NONE\n");
	}

	if (of_property_read_u32(np, "ppc", (u32 *)&decon->bts.ppc))
		decon->bts.ppc = 2UL;
	decon_info(decon, "PPC(%llu)\n", decon->bts.ppc);

	if (of_property_read_u32(np, "line_mem_cnt",
				(u32 *)&decon->bts.line_mem_cnt)) {
		decon->bts.line_mem_cnt = 4UL;
		decon_warn(decon, "WARN: line memory cnt is not defined in DT.\n");
	}
	decon_info(decon, "line memory cnt(%d)\n", decon->bts.line_mem_cnt);

	if (of_property_read_u32(np, "cycle_per_line",
				(u32 *)&decon->bts.cycle_per_line)) {
		decon->bts.cycle_per_line = 8UL;
		decon_warn(decon, "WARN: cycle per line is not defined in DT.\n");
	}
	decon_info(decon, "cycle per line(%d)\n", decon->bts.cycle_per_line);

	if (decon->config.out_type == DECON_OUT_DSI)
		decon->config.mode.dsi_mode = DSI_MODE_DUAL_DSI;
	else if (decon->config.out_type & (DECON_OUT_DSI0 | DECON_OUT_DSI1))
		decon->config.mode.dsi_mode = DSI_MODE_SINGLE;
	else
		decon->config.mode.dsi_mode = DSI_MODE_NONE;

	decon->dpp_cnt = of_count_phandle_with_args(np, "dpps", NULL);
	for (i = 0; i < decon->dpp_cnt; ++i) {
		dpp_np = of_parse_phandle(np, "dpps", i);
		if (!dpp_np) {
			decon_err(decon, "can't find dpp%d node\n", i);
			return -EINVAL;
		}

		decon->dpp[i] = of_find_dpp_by_node(dpp_np);
		if (!decon->dpp[i]) {
			decon_err(decon, "can't find dpp%d structure\n", i);
			return -EINVAL;
		}

		dpp_id = decon->dpp[i]->id;
		decon_info(decon, "found dpp%d\n", dpp_id);

		if (dpp_np)
			of_node_put(dpp_np);
	}

	of_property_for_each_u32(np, "connector", prop, cur, val)
		decon->con_type |= val;

	return 0;
}

static int decon_remap_regs(struct decon_device *decon)
{
	struct resource *res;
	struct device *dev = decon->dev;
	struct device_node *np;
	struct platform_device *pdev;

	pdev = container_of(dev, struct platform_device, dev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	decon->regs.regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(decon->regs.regs)) {
		DRM_DEV_ERROR(decon->dev, "failed decon ioremap\n");
		return PTR_ERR(decon->regs.regs);
	}
	decon_regs_desc_init(decon->regs.regs, "decon", REGS_DECON,
			decon->id);

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos9-disp_ss");
	if (IS_ERR_OR_NULL(np)) {
		DRM_DEV_ERROR(decon->dev, "failed to find disp_ss node");
		return PTR_ERR(np);
	}
	decon->regs.ss_regs = of_iomap(np, 0);
	if (!decon->regs.ss_regs) {
		DRM_DEV_ERROR(decon->dev, "failed to map sysreg-disp address.");
		return -ENOMEM;
	}
	decon_regs_desc_init(decon->regs.ss_regs, "decon-ss", REGS_DECON_SYS,
			decon->id);

	return 0;
}

static irqreturn_t decon_te_irq_handler(int irq, void *dev_id)
{
	struct decon_device *decon = dev_id;
	struct exynos_hibernation *hibernation;

	if (!decon || decon->state != DECON_STATE_ON)
		goto end;

	DPU_EVENT_LOG(DPU_EVT_TE_INTERRUPT, decon->id, NULL);

	if (decon->config.mode.op_mode == DECON_MIPI_COMMAND_MODE)
		drm_crtc_handle_vblank(&decon->crtc->base);

	hibernation = decon->hibernation;

	if (hibernation && !is_hibernaton_blocked(hibernation))
		kthread_queue_work(&hibernation->worker, &hibernation->work);

end:
	return IRQ_HANDLED;
}

static int decon_register_irqs(struct decon_device *decon)
{
	struct device *dev = decon->dev;
	struct platform_device *pdev;
	struct resource *res;
	int ret = 0;
	int gpio;

	pdev = container_of(dev, struct platform_device, dev);

	/* 1: FRAME START */
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	decon->irq_fs = res->start;
	ret = devm_request_irq(dev, res->start, decon_irq_handler,
			0, pdev->name, decon);
	if (ret) {
		decon_err(decon, "failed to install FRAME START irq\n");
		return ret;
	}
	disable_irq(decon->irq_fs);

	/* 2: FRAME DONE */
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 1);
	decon->irq_fd = res->start;
	ret = devm_request_irq(dev, res->start, decon_irq_handler,
			0, pdev->name, decon);
	if (ret) {
		decon_err(decon, "failed to install FRAME DONE irq\n");
		return ret;
	}
	disable_irq(decon->irq_fd);

	/* 3: EXTRA: resource conflict, timeout and error irq */
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 2);
	decon->irq_ext = res->start;
	ret = devm_request_irq(dev, res->start, decon_irq_handler,
			0, pdev->name, decon);
	if (ret) {
		decon_err(decon, "failed to install EXTRA irq\n");
		return ret;
	}
	disable_irq(decon->irq_ext);

	/* Get IRQ resource and register IRQ handler. Only enabled in command
	 * mode.
	 */
	if (of_get_property(dev->of_node, "gpios", NULL) != NULL) {
		gpio = of_get_gpio(dev->of_node, 0);
		if (gpio < 0) {
			decon_err(decon, "failed to get TE gpio\n");
			return -ENODEV;
		}
	} else {
		decon_dbg(decon, "failed to find TE gpio node\n");
		return 0;
	}

	decon->irq_te = gpio_to_irq(gpio);

	decon_info(decon, "TE irq number(%d)\n", decon->irq_te);
	irq_set_status_flags(decon->irq_te, IRQ_DISABLE_UNLAZY);
	ret = devm_request_irq(dev, decon->irq_te, decon_te_irq_handler,
			IRQF_TRIGGER_RISING, pdev->name, decon);
	disable_irq(decon->irq_te);

	return ret;
}

static int decon_get_pinctrl(struct decon_device *decon)
{
	int ret = 0;

	decon->res.pinctrl = devm_pinctrl_get(decon->dev);
	if (IS_ERR(decon->res.pinctrl)) {
		decon_dbg(decon, "failed to get pinctrl\n");
		ret = PTR_ERR(decon->res.pinctrl);
		decon->res.pinctrl = NULL;
		/* optional in video mode */
		return 0;
	}

	decon->res.te_on = pinctrl_lookup_state(decon->res.pinctrl, "hw_te_on");
	if (IS_ERR(decon->res.te_on)) {
		decon_err(decon, "failed to get hw_te_on pin state\n");
		ret = PTR_ERR(decon->res.te_on);
		decon->res.te_on = NULL;
		goto err;
	}
	decon->res.te_off = pinctrl_lookup_state(decon->res.pinctrl,
			"hw_te_off");
	if (IS_ERR(decon->res.te_off)) {
		decon_err(decon, "failed to get hw_te_off pin state\n");
		ret = PTR_ERR(decon->res.te_off);
		decon->res.te_off = NULL;
		goto err;
	}

err:
	return ret;
}

#ifndef CONFIG_BOARD_EMULATOR
static int decon_get_clock(struct decon_device *decon)
{
	decon->res.aclk = devm_clk_get(decon->dev, "aclk");
	if (IS_ERR_OR_NULL(decon->res.aclk)) {
		decon_info(decon, "failed to get aclk(optional)\n");
		decon->res.aclk = NULL;
	}

	decon->res.aclk_disp = devm_clk_get(decon->dev, "aclk-disp");
	if (IS_ERR_OR_NULL(decon->res.aclk_disp)) {
		decon_info(decon, "failed to get aclk_disp(optional)\n");
		decon->res.aclk_disp = NULL;
	}

	return 0;
}
#else
static inline int decon_get_clock(struct decon_device *decon) { return 0; }
#endif

static int decon_init_resources(struct decon_device *decon)
{
	int ret = 0;

	ret = decon_remap_regs(decon);
	if (ret)
		goto err;

	ret = decon_register_irqs(decon);
	if (ret)
		goto err;

	ret = decon_get_pinctrl(decon);
	if (ret)
		goto err;

	ret = decon_get_clock(decon);
	if (ret)
		goto err;

	ret = __decon_init_resources(decon);
	if (ret)
		goto err;

err:
	return ret;
}

static int decon_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct decon_device *decon;
	struct device *dev = &pdev->dev;

	decon = devm_kzalloc(dev, sizeof(struct decon_device), GFP_KERNEL);
	if (!decon)
		return -ENOMEM;

	decon->dev = dev;

	ret = decon_parse_dt(decon, dev->of_node);
	if (ret)
		goto err;

	decon_drvdata[decon->id] = decon;

	spin_lock_init(&decon->slock);
	init_completion(&decon->framestart_done);

	if (IS_ENABLED(CONFIG_EXYNOS_BTS)) {
		decon->bts.ops = &dpu_bts_control;
		decon->bts.ops->bts_init(decon);
	}

	decon->state = DECON_STATE_OFF;
	pm_runtime_enable(decon->dev);

	ret = decon_init_resources(decon);
	if (ret)
		goto err;

	/* set drvdata */
	platform_set_drvdata(pdev, decon);

	decon->hibernation = exynos_hibernation_register(decon);

	decon->dqe = exynos_dqe_register(decon);

	ret = component_add(dev, &decon_component_ops);
	if (ret)
		goto err;

	dev_info(dev, "successfully probed");

err:
	return ret;
}

static int decon_remove(struct platform_device *pdev)
{
	struct decon_device *decon;

	decon = platform_get_drvdata(pdev);
	if (IS_ENABLED(CONFIG_EXYNOS_BTS))
		decon->bts.ops->bts_deinit(decon);

	exynos_hibernation_destroy(decon->hibernation);

	component_del(&pdev->dev, &decon_component_ops);

	return 0;
}

#ifdef CONFIG_PM
static int decon_suspend(struct device *dev)
{
	struct decon_device *decon = dev_get_drvdata(dev);

	if (decon->res.aclk)
		clk_disable_unprepare(decon->res.aclk);

	if (decon->res.aclk_disp)
		clk_disable_unprepare(decon->res.aclk_disp);

	if (decon->dqe)
		exynos_dqe_reset(decon->dqe);

	pr_debug("suspended\n");

	return 0;
}

static int decon_resume(struct device *dev)
{
	struct decon_device *decon = dev_get_drvdata(dev);

	if (decon->res.aclk)
		clk_prepare_enable(decon->res.aclk);

	if (decon->res.aclk_disp)
		clk_prepare_enable(decon->res.aclk_disp);

	pr_debug("resumed\n");

	return 0;
}
#endif

static const struct dev_pm_ops decon_pm_ops = {
	SET_RUNTIME_PM_OPS(decon_suspend, decon_resume, NULL)
};

struct platform_driver decon_driver = {
	.probe		= decon_probe,
	.remove		= decon_remove,
	.driver		= {
		.name	= "exynos-decon",
		.pm	= &decon_pm_ops,
		.of_match_table = decon_driver_dt_match,
	},
};

MODULE_AUTHOR("Hyung-jun Kim <hyungjun07.kim@samsung.com>");
MODULE_AUTHOR("Seong-gyu Park <seongyu.park@samsung.com>");
MODULE_DESCRIPTION("Samsung SoC Display and Enhancement Controller");
MODULE_LICENSE("GPL v2");
