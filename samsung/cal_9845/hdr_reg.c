// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Register access functions for Samsung HDR
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <cal_config.h>
#include <hdr_cal.h>
#include <dpp_cal.h>

#include "regs-hdr.h"

static struct cal_regs_desc regs_hdr[REGS_DPP_ID_MAX - 1];

#define hdr_regs_desc(id)			(&regs_hdr[id])
#define hdr_read(id, offset)			\
	cal_read(hdr_regs_desc(id), offset)
#define hdr_write(id, offset, val)		\
	cal_write(hdr_regs_desc(id), offset, val)
#define hdr_read_mask(id, offset, mask)		\
	cal_read_mask(hdr_regs_desc(id), offset, mask)
#define hdr_write_mask(id, offset, val, mask)	\
	cal_write_mask(hdr_regs_desc(id), offset, val, mask)
#define hdr_write_relaxed(id, offset, val)	\
	cal_write_relaxed(hdr_regs_desc(id), offset, val)

void hdr_regs_desc_init(void __iomem *regs, const char *name, u32 id)
{
	regs_hdr[id].regs = regs;
	regs_hdr[id].name = name;
}

void hdr_reg_set_hdr(u32 id, bool en)
{
	cal_log_debug(id, "%s +\n", __func__);
	hdr_write_mask(id, HDR_LSI_L_COM_CTRL, en ? ~0 : 0,
			COM_CTRL_ENABLE_MASK);
	cal_log_debug(id, "%s -\n", __func__);
}

void hdr_reg_set_eotf_lut(u32 id, struct hdr_eotf_lut *lut)
{
	int i;
	u32 val;

	cal_log_debug(id, "%s +\n", __func__);

	if (!lut) {
		hdr_write_mask(id, HDR_LSI_L_MOD_CTRL, MOD_CTRL_EEN(0),
				MOD_CTRL_EEN_MASK);
		return;
	}

	for (i = 0; i < HDR_EOTF_POSX_LUT_REG_CNT; i++) {
		val = EOTF_POSX_H(lut->posx[i * 2 + 1]) |
			EOTF_POSX_L(lut->posx[i * 2]);
		hdr_write_relaxed(id, HDR_LSI_L_EOTF_POSX(i), val);
		cal_log_debug(id, "POSX[%d]: 0x%x\n", i, val);
	}

	for (i = 0; i < HDR_EOTF_POSY_LUT_REG_CNT; i++) {
		hdr_write_relaxed(id, HDR_LSI_L_EOTF_POSY(i), lut->posy[i]);
		cal_log_debug(id, "POSY[%d]: 0x%x\n", i, lut->posy[i]);
	}

	hdr_write_mask(id, HDR_LSI_L_MOD_CTRL, MOD_CTRL_EEN(1),
			MOD_CTRL_EEN_MASK);

	cal_log_debug(id, "%s -\n", __func__);
}

void hdr_reg_set_oetf_lut(u32 id, struct hdr_oetf_lut *lut)
{
	int i;
	u32 val;

	cal_log_debug(id, "%s +\n", __func__);

	if (!lut) {
		hdr_write_mask(id, HDR_LSI_L_MOD_CTRL, MOD_CTRL_OEN(0),
				MOD_CTRL_OEN_MASK);
		return;
	}

	for (i = 0; i < HDR_OETF_POSX_LUT_REG_CNT; i++) {
		val = OETF_POSX_H(lut->posx[i * 2 + 1]) |
			OETF_POSX_L(lut->posx[i * 2]);
		hdr_write_relaxed(id, HDR_LSI_L_OETF_POSX(i), val);
		cal_log_debug(id, "POSX[%d]: 0x%x\n", i, val);
	}

	for (i = 0; i < HDR_OETF_POSY_LUT_REG_CNT; i++) {
		val = OETF_POSY_H(lut->posy[i * 2 + 1]) |
			OETF_POSY_L(lut->posy[i * 2]);
		hdr_write_relaxed(id, HDR_LSI_L_OETF_POSY(i), val);
		cal_log_debug(id, "POSY[%d]: 0x%x\n", i, val);
	}

	hdr_write_mask(id, HDR_LSI_L_MOD_CTRL, MOD_CTRL_OEN(1),
			MOD_CTRL_OEN_MASK);

	cal_log_debug(id, "%s -\n", __func__);
}

/*
 * |Rout| = |C00 C01 C02| |Rin| + |offset0|
 * |Gout| = |C10 C11 C12| |Gin| + |offset1|
 * |Bout| = |C20 C21 C22| |Bin| + |offset2|
 */
void hdr_reg_set_gm(u32 id, struct hdr_gm_data *data)
{
	int i;

	cal_log_debug(id, "%s +\n", __func__);

	if (!data) {
		hdr_write_mask(id, HDR_LSI_L_MOD_CTRL, MOD_CTRL_GEN(0),
				MOD_CTRL_GEN_MASK);
		return;
	}

	for (i = 0; i < HDR_GM_COEF_REG_CNT; ++i) {
		hdr_write_relaxed(id, HDR_LSI_L_GM_COEF(i), data->coeffs[i]);
		cal_log_debug(id, "COEFFS[%d]: 0x%x\n", i, data->coeffs[i]);
	}

	for (i = 0; i < HDR_GM_OFFS_REG_CNT; ++i) {
		hdr_write_relaxed(id, HDR_LSI_L_GM_OFFS(i), data->offsets[i]);
		cal_log_debug(id, "OFFSETS[%d]: 0x%x\n", i, data->offsets[i]);
	}

	hdr_write_mask(id, HDR_LSI_L_MOD_CTRL, MOD_CTRL_GEN(1),
			MOD_CTRL_GEN_MASK);

	cal_log_debug(id, "%s -\n", __func__);
}

void hdr_reg_set_tm(u32 id, struct hdr_tm_data *tm)
{
	int i;
	u32 val;

	cal_log_debug(id, "%s +\n", __func__);

	if (!tm) {
		hdr_write_mask(id, HDR_LSI_L_MOD_CTRL, 0, MOD_CTRL_TEN_MASK);
		return;
	}

	val = TM_COEFB(tm->coeff_b) | TM_COEFG(tm->coeff_g) |
		TM_COEFR(tm->coeff_r);
	hdr_write_relaxed(id, HDR_LSI_L_TM_COEF, val);
	cal_log_debug(id, "COEFF: 0x%x\n", val);

	val = TM_RNGX_MAXX(tm->rng_x_max) | TM_RNGX_MINX(tm->rng_x_min);
	hdr_write_relaxed(id, HDR_LSI_L_TM_RNGX, val);
	cal_log_debug(id, "RNGX: 0x%x\n", val);

	val = TM_RNGY_MAXY(tm->rng_y_max) | TM_RNGY_MINY(tm->rng_y_min);
	hdr_write_relaxed(id, HDR_LSI_L_TM_RNGY, val);
	cal_log_debug(id, "RNGY: 0x%x\n", val);

	for (i = 0; i < HDR_TM_POSX_LUT_REG_CNT; i++) {
		val = TM_POSX_H(tm->posx[i * 2 + 1]) |
			TM_POSX_L(tm->posx[i * 2]);
		hdr_write_relaxed(id, HDR_LSI_L_TM_POSX(i), val);
		cal_log_debug(id, "POSX[%d]: 0x%x\n", i, val);
	}

	for (i = 0; i < HDR_TM_POSY_LUT_REG_CNT; i++) {
		hdr_write_relaxed(id, HDR_LSI_L_TM_POSY(i), tm->posy[i]);
		cal_log_debug(id, "POSY[%d]: 0x%x\n", i, tm->posy[i]);
	}

	hdr_write_mask(id, HDR_LSI_L_MOD_CTRL, ~0, MOD_CTRL_TEN_MASK);

	cal_log_debug(id, "%s -\n", __func__);
}

static void hdr_reg_print(u32 id, u32 start, u32 count, enum elem_size size)
{
	u32 reg, val;
	int idx, i;
	char buf[256];
	char *p = buf;
	const char *end = buf + sizeof(buf);
	bool is_32 = (size == ELEM_SIZE_32);
	u32 pcount;

	pcount = is_32 ? count : DIV_ROUND_UP(count, 2);

	for (i = 0; i < pcount; ++i) {
		reg = hdr_read(id, start + i * 4);

		idx = is_32 ? i : i * 2;
		val = is_32 ? reg : GET_LUT_L(reg);

		p += scnprintf(p, end - p, "[%4d]%8x ", idx, val);

		if (((i * 2 + 1) != count) && !is_32)
			p += scnprintf(p, end - p, "[%4d]%8x ", i * 2 + 1,
					GET_LUT_H(reg));

		if ((i % 5) == 4) {
			cal_log_info(id, "%s\n", buf);
			p = buf;
		}
	}

	if (p != buf)
		cal_log_info(id, "%s\n", buf);
}

void hdr_reg_print_eotf_lut(u32 id)
{
	u32 val;

	val = hdr_read_mask(id, HDR_LSI_L_MOD_CTRL, MOD_CTRL_EEN_MASK);
	cal_log_info(id, "HDR: eotf %s\n", val ? "on" : "off");

	if (!val)
		return;

	cal_log_info(id, "POSX:\n");
	hdr_reg_print(id, HDR_LSI_L_EOTF_POSX(0), DRM_SAMSUNG_HDR_EOTF_LUT_LEN,
								ELEM_SIZE_16);
	cal_log_info(id, "POSY:\n");
	hdr_reg_print(id, HDR_LSI_L_EOTF_POSY(0), DRM_SAMSUNG_HDR_EOTF_LUT_LEN,
								ELEM_SIZE_32);
}

void hdr_reg_print_oetf_lut(u32 id)
{
	u32 val;

	val = hdr_read_mask(id, HDR_LSI_L_MOD_CTRL, MOD_CTRL_OEN_MASK);
	cal_log_info(id, "HDR: oetf %s\n", val ? "on" : "off");

	if (!val)
		return;

	cal_log_info(id, "POSX:\n");
	hdr_reg_print(id, HDR_LSI_L_OETF_POSX(0), DRM_SAMSUNG_HDR_OETF_LUT_LEN,
								ELEM_SIZE_16);
	cal_log_info(id, "POSY:\n");
	hdr_reg_print(id, HDR_LSI_L_OETF_POSY(0), DRM_SAMSUNG_HDR_OETF_LUT_LEN,
								ELEM_SIZE_16);
}

void hdr_reg_print_gm(u32 id)
{
	u32 val;

	val = hdr_read_mask(id, HDR_LSI_L_MOD_CTRL, MOD_CTRL_GEN_MASK);
	cal_log_info(id, "HDR: gammut %s\n", val ? "on" : "off");

	if (!val)
		return;

	cal_log_info(id, "COEFFS:\n");
	hdr_reg_print(id, HDR_LSI_L_GM_COEF(0), HDR_GM_COEF_REG_CNT,
							ELEM_SIZE_32);
	cal_log_info(id, "OFFSETS:\n");
	hdr_reg_print(id, HDR_LSI_L_GM_OFFS(0), HDR_GM_OFFS_REG_CNT,
							ELEM_SIZE_32);
}

void hdr_reg_print_tm(u32 id)
{
	u32 val;

	val = hdr_read_mask(id, HDR_LSI_L_MOD_CTRL, MOD_CTRL_TEN_MASK);
	cal_log_info(id, "HDR: tone mapping %s\n", val ? "on" : "off");

	cal_log_info(id, "COEFF: 0x%x\n", hdr_read(id, HDR_LSI_L_TM_COEF));
	cal_log_info(id, "RNGX: 0x%x\n", hdr_read(id, HDR_LSI_L_TM_RNGX));
	cal_log_info(id, "RNGY: 0x%x\n", hdr_read(id, HDR_LSI_L_TM_RNGY));

	cal_log_info(id, "POSX:\n");
	hdr_reg_print(id, HDR_LSI_L_TM_POSX(0), DRM_SAMSUNG_HDR_TM_LUT_LEN,
								ELEM_SIZE_16);
	cal_log_info(id, "POSY:\n");
	hdr_reg_print(id, HDR_LSI_L_TM_POSY(0), DRM_SAMSUNG_HDR_TM_LUT_LEN,
								ELEM_SIZE_32);
}
