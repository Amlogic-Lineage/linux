/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef VIDEO_REG_HEADER_HH
#define VIDEO_REG_HEADER_HH

#include <linux/string.h>
#include <linux/ctype.h>

#define MAX_VD_LAYER 2

struct hw_vd_reg_s {
	u32 vd_if0_gen_reg; /* VD1_IF0_GEN_REG */
	u32 vd_if0_canvas0;/* VD1_IF0_CANVAS0 */
	u32 vd_if0_canvas1;/* VD1_IF0_CANVAS1 */
	u32 vd_if0_luma_x0;/* VD1_IF0_LUMA_X0 */
	u32 vd_if0_luma_y0; /* VD1_IF0_LUMA_Y0 */
	u32 vd_if0_chroma_x0; /* VD1_IF0_CHROMA_X0 */
	u32 vd_if0_chroma_y0; /* VD1_IF0_CHROMA_Y0 */
	u32 vd_if0_luma_x1; /* VD1_IF0_LUMA_X1 */
	u32 vd_if0_luma_y1;/* VD1_IF0_LUMA_Y1 */
	u32 vd_if0_chroma_x1;/* VD1_IF0_CHROMA_X1 */
	u32 vd_if0_chroma_y1;/* VD1_IF0_CHROMA_Y1 */
	u32 vd_if0_rpt_loop; /* VD1_IF0_RPT_LOOP */
	u32 vd_if0_luma0_rpt_pat;/* VD1_IF0_LUMA0_RPT_PAT */
	u32 vd_if0_chroma0_rpt_pat;/* VD1_IF0_CHROMA0_RPT_PAT */
	u32 vd_if0_luma1_rpt_pat;/* VD1_IF0_LUMA1_RPT_PAT */
	u32 vd_if0_chroma1_rpt_pat;/* VD1_IF0_CHROMA1_RPT_PAT */
	u32 vd_if0_luma_psel;/* VD1_IF0_LUMA_PSEL */
	u32 vd_if0_chroma_psel;/* VD1_IF0_CHROMA_PSEL */
	u32 vd_if0_luma_fifo_size;/* VD1_IF0_LUMA_FIFO_SIZE */

	u32 vd_if0_gen_reg2;/* VD1_IF0_GEN_REG2 */
	u32 vd_if0_gen_reg3;/* VD1_IF0_GEN_REG3 */
	u32 viu_vd_fmt_ctrl;/* VIU_VD1_FMT_CTRL */
	u32 viu_vd_fmt_w;/* VIU_VD1_FMT_W */
};

struct hw_fg_reg_s {
	u32 fgrain_ctrl;
	u32 fgrain_win_h;
	u32 fgrain_win_v;
};

extern struct hw_vd_reg_s vd_mif_reg_legacy_array[MAX_VD_LAYER];
extern struct hw_vd_reg_s vd_mif_reg_g12_array[MAX_VD_LAYER];
extern struct hw_vd_reg_s vd_mif_reg_sc2_array[MAX_VD_LAYER];
extern struct hw_vd_reg_s fg_reg_g12_array[MAX_VD_LAYER];
extern struct hw_vd_reg_s fg_reg_sc2_array[MAX_VD_LAYER];

#endif
