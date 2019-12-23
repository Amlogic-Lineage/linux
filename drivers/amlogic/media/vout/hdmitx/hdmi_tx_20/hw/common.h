/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef __HDMITX_HW_COMMON_H__
#define __HDMITX_HW_COMMON_H__

#include <linux/amlogic/media/vout/hdmi_tx/hdmi_tx_ddc.h>
#include <linux/amlogic/media/vout/hdmi_tx/hdmi_common.h>
#include "hdmi_tx_reg.h"

/***********************************************************************
 *                   hdmi debug printk
 * pr_info(EDID "edid bad\");
 * pr_debug(AUD "set audio format: AC-3\n");
 * pr_err(REG "write reg\n")
 **********************************************************************/
#undef pr_fmt
#define pr_fmt(fmt) "hdmitx: " fmt

#define VID         "video: "
#define AUD         "audio: "
#define CEC         "cec: "
#define EDID        "edid: "
#define HDCP        "hdcp: "
#define SYS         "system: "
#define HPD         "hpd: "
#define HW          "hw: "
#define REG         "reg: "

int hdmitx_hpd_hw_op_gxbb(enum hpd_op cmd);
int read_hpd_gpio_gxbb(void);
int hdmitx_ddc_hw_op_gxbb(enum ddc_op cmd);

int hdmitx_hpd_hw_op_gxtvbb(enum hpd_op cmd);
int read_hpd_gpio_gxtvbb(void);
int hdmitx_ddc_hw_op_gxtvbb(enum ddc_op cmd);

int hdmitx_hpd_hw_op_gxl(enum hpd_op cmd);
int read_hpd_gpio_gxl(void);
int hdmitx_ddc_hw_op_gxl(enum ddc_op cmd);
void set_gxl_hpll_clk_out(unsigned int frac_rate, unsigned int clk);
void set_hpll_sspll_gxl(enum hdmi_vic vic);
void set_hpll_sspll_g12a(enum hdmi_vic vic);

void set_hpll_od1_gxl(unsigned int div);
void set_hpll_od2_gxl(unsigned int div);
void set_hpll_od3_gxl(unsigned int div);

void set_g12a_hpll_clk_out(unsigned int frac_rate, unsigned int clk);
void set_hpll_od1_g12a(unsigned int div);
void set_hpll_od2_g12a(unsigned int div);
void set_hpll_od3_g12a(unsigned int div);
int hdmitx_hpd_hw_op_g12a(enum hpd_op cmd);
int hdmitx_ddc_hw_op_g12a(enum ddc_op cmd);

int read_hpd_gpio_txlx(void);
int hdmitx_hpd_hw_op_txlx(enum hpd_op cmd);
int hdmitx_ddc_hw_op_txlx(enum ddc_op cmd);
unsigned int hdmitx_get_format_txlx(void);
void hdmitx_sys_reset_txlx(void);

#endif
