// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

//needed for gdc/gdc configuration
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/clk.h>

//data types and prototypes
#include "gdc_api.h"
#include "system_log.h"
#include "gdc_config.h"

/**
 *   Configure the output gdc configuration address/size
 *
 *   and buffer address/size; and resolution.
 *
 *   More than one gdc settings can be accessed by index to a gdc_config_t.
 *
 *   @param  gdc_cmd - overall gdc settings and state
 *   @param  gdc_config_num - selects the current gdc config to be applied
 *
 *   @return 0 - success
 *	 -1 - fail.
 */
int gdc_init(struct gdc_cmd_s *gdc_cmd)
{
	gdc_cmd->is_waiting_gdc = 0;
	gdc_cmd->current_addr = gdc_cmd->buffer_addr;

	if (gdc_cmd->gdc_config.output_width == 0 ||
	    gdc_cmd->gdc_config.output_height == 0) {
		gdc_log(LOG_ERR, "Wrong GDC output resolution.\n");
		return -1;
	}
	//stop gdc
	gdc_start_flag_write(0);
	//set the configuration address and size to the gdc block
	gdc_config_addr_write(gdc_cmd->gdc_config.config_addr);
	gdc_config_size_write(gdc_cmd->gdc_config.config_size);

	//set the gdc output resolution
	gdc_dataout_width_write(gdc_cmd->gdc_config.output_width);
	gdc_dataout_height_write(gdc_cmd->gdc_config.output_height);

	return 0;
}

/**
 *   This function stops the gdc block
 *
 *   @param  gdc_cmd - overall gdc settings and state
 *
 */
void gdc_stop(struct gdc_cmd_s *gdc_cmd)
{
	gdc_cmd->is_waiting_gdc = 0;
	gdc_start_flag_write(0);
}

/**
 *   This function starts the gdc block
 *
 *   Writing 0->1 transition is necessary for trigger
 *
 *   @param  gdc_cmd - overall gdc settings and state
 *
 */
void gdc_start(struct gdc_cmd_s *gdc_cmd)
{
	gdc_start_flag_write(0); //do a stop for sync
	gdc_start_flag_write(1);
	gdc_cmd->is_waiting_gdc = 1;
}

/**
 *   This function points gdc to its input resolution
 *
 *   and yuv address and offsets
 *
 *   Shown inputs to GDC are Y and UV plane address and offsets
 *
 *   @param  gdc_cmd - overall gdc settings and state
 *   @param  active_width -  input width resolution
 *   @param  active_height - input height resolution
 *   @param  y_base_addr -  input Y base address
 *   @param  uv_base_addr - input UV base address
 *   @param  y_line_offset - input Y line buffer offset
 *   @param  uv_line_offset-  input UV line buffer offer
 *
 *   @return 0 - success
 *	 -1 - no interrupt from GDC.
 */
int gdc_process(struct gdc_cmd_s *gdc_cmd,
		u32 y_base_addr, u32 uv_base_addr)
{
	u32 gdc_out_base_addr = gdc_cmd->current_addr;
	u32 input_width = gdc_cmd->gdc_config.input_width;
	u32 input_height = gdc_cmd->gdc_config.input_height;
	u32 output_height = gdc_cmd->gdc_config.output_height;
	u32 i_y_line_offset = gdc_cmd->gdc_config.input_y_stride;
	u32 i_uv_line_offset = gdc_cmd->gdc_config.input_c_stride;
	u32 o_y_line_offset = gdc_cmd->gdc_config.output_y_stride;
	u32 o_uv_line_offset = gdc_cmd->gdc_config.output_c_stride;

	if (gdc_cmd->is_waiting_gdc) {
		gdc_start_flag_write(0);
		gdc_log(LOG_CRIT, "No interrupt Still waiting...\n");
		gdc_start_flag_write(1);

		return -1;
	}

	gdc_log(LOG_DEBUG, "starting GDC process.\n");

	gdc_datain_width_write(input_width);
	gdc_datain_height_write(input_height);
	//input y plane
	gdc_data1in_addr_write(y_base_addr);
	gdc_data1in_line_offset_write(i_y_line_offset);

	//input uv plane
	gdc_data2in_addr_write(uv_base_addr);
	gdc_data2in_line_offset_write(i_uv_line_offset);

	//gdc y output
	gdc_data1out_addr_write(gdc_out_base_addr);
	gdc_data1out_line_offset_write(o_y_line_offset);

	//gdc uv output
	if (gdc_cmd->outplane == 1)
		gdc_out_base_addr += output_height * o_y_line_offset;
	else
		gdc_out_base_addr = gdc_cmd->uv_out_base_addr;
	gdc_data2out_addr_write(gdc_out_base_addr);
	gdc_data2out_line_offset_write(o_uv_line_offset);

	gdc_start(gdc_cmd);

	return 0;
}

/**
 *   This function points gdc to its input resolution
 *
 *   and yuv address and offsets
 *
 *   Shown inputs to GDC are Y and UV plane address and offsets
 *
 *   @param  gdc_cmd - overall gdc settings and state
 *   @param  active_width -  input width resolution
 *   @param  active_height - input height resolution
 *   @param  y_base_addr -  input Y base address
 *   @param  uv_base_addr - input UV base address
 *   @param  y_line_offset - input Y line buffer offset
 *   @param  uv_line_offset-  input UV line buffer offer
 *
 *   @return 0 - success
 *	 -1 - no interrupt from GDC.
 */
int gdc_process_yuv420p(struct gdc_cmd_s *gdc_cmd,
			u32 y_base_addr, u32 u_base_addr, u32 v_base_addr)
{
	struct gdc_config_s  *gc = &gdc_cmd->gdc_config;
	u32 gdc_out_base_addr = gdc_cmd->current_addr;
	u32 input_width = gc->input_width;
	u32 input_height = gc->input_height;
	u32 input_stride = gc->input_y_stride;
	u32 input_u_stride = gc->input_c_stride;
	u32 input_v_stride = gc->input_c_stride;

	gdc_log(LOG_DEBUG, "is_waiting_gdc=%d\n", gdc_cmd->is_waiting_gdc);
	if (gdc_cmd->is_waiting_gdc) {
		gdc_start_flag_write(0);
		gdc_log(LOG_CRIT, "No interrupt Still waiting...\n");
		gdc_start_flag_write(1);
		return -1;
	}

	gdc_log(LOG_DEBUG, "starting GDC process.\n");

	/* already set in gdc_init */
	/* u32 output_width = gc->output_width; */
	u32 output_height = gc->output_height;
	u32 output_stride = gc->output_y_stride;
	u32 output_u_stride = gc->output_c_stride;
	u32 output_v_stride = gc->output_c_stride;

	gdc_datain_width_write(input_width);
	gdc_datain_height_write(input_height);
	//input y plane
	gdc_data1in_addr_write(y_base_addr);
	gdc_data1in_line_offset_write(input_stride);

	//input u plane
	gdc_data2in_addr_write(u_base_addr);
	gdc_data2in_line_offset_write(input_u_stride);

	//input v plane
	gdc_data3in_addr_write(v_base_addr);
	gdc_data3in_line_offset_write(input_v_stride);

	//gdc y output
	gdc_data1out_addr_write(gdc_out_base_addr);
	gdc_data1out_line_offset_write(output_stride);

	//gdc u output
	if (gdc_cmd->outplane == 1)
		gdc_out_base_addr += output_height * output_stride;
	else
		gdc_out_base_addr = gdc_cmd->u_out_base_addr;
	gdc_data2out_addr_write(gdc_out_base_addr);
	gdc_data2out_line_offset_write(output_u_stride);

	//gdc v output
	if (gdc_cmd->outplane == 1)
		gdc_out_base_addr += output_height * output_u_stride / 2;
	else
		gdc_out_base_addr = gdc_cmd->v_out_base_addr;
	gdc_data3out_addr_write(gdc_out_base_addr);
	gdc_data3out_line_offset_write(output_v_stride);
	gdc_start(gdc_cmd);

	return 0;
}

/**
 *   This function points gdc to its input resolution
 *
 *   and yuv address and offsets
 *
 *   Shown inputs to GDC are Y plane address and offsets
 *
 *   @param  gdc_cmd - overall gdc settings and state
 *   @param  active_width -  input width resolution
 *   @param  active_height - input height resolution
 *   @param  y_base_addr -  input Y base address
 *   @param  y_line_offset - input Y line buffer offset
 *
 *   @return 0 - success
 *	 -1 - no interrupt from GDC.
 */
int gdc_process_y_grey(struct gdc_cmd_s *gdc_cmd,
		       u32 y_base_addr)
{
	struct gdc_config_s  *gc = &gdc_cmd->gdc_config;
	u32 gdc_out_base_addr = gdc_cmd->current_addr;
	u32 input_width = gc->input_width;
	u32 input_height = gc->input_height;
	u32 input_stride = gc->input_y_stride;
	u32 output_stride = gc->output_y_stride;

	gdc_log(LOG_DEBUG, "is_waiting_gdc=%d\n", gdc_cmd->is_waiting_gdc);
	if (gdc_cmd->is_waiting_gdc) {
		gdc_start_flag_write(0);
		gdc_log(LOG_CRIT, "No interrupt Still waiting...\n");
		gdc_start_flag_write(1);
		return -1;
	}

	gdc_log(LOG_DEBUG, "starting GDC process.\n");

	gdc_datain_width_write(input_width);
	gdc_datain_height_write(input_height);
	//input y plane
	gdc_data1in_addr_write(y_base_addr);
	gdc_data1in_line_offset_write(input_stride);

	//gdc y output
	gdc_data1out_addr_write(gdc_out_base_addr);
	gdc_data1out_line_offset_write(output_stride);

	gdc_start(gdc_cmd);

	return 0;
}

/**
 *   This function points gdc to its input resolution
 *
 *   and yuv address and offsets
 *
 *   Shown inputs to GDC are Y and UV plane address and offsets
 *
 *   @param  gdc_cmd - overall gdc settings and state
 *   @param  active_width -  input width resolution
 *   @param  active_height - input height resolution
 *   @param  y_base_addr -  input Y base address
 *   @param  uv_base_addr - input UV base address
 *   @param  y_line_offset - input Y line buffer offset
 *   @param  uv_line_offset-  input UV line buffer offer
 *
 *   @return 0 - success
 *	 -1 - no interrupt from GDC.
 */
int gdc_process_yuv444p(struct gdc_cmd_s *gdc_cmd,
			u32 y_base_addr, u32 u_base_addr, u32 v_base_addr)
{
	struct gdc_config_s  *gc = &gdc_cmd->gdc_config;
	u32 gdc_out_base_addr = gdc_cmd->current_addr;
	u32 input_width = gc->input_width;
	u32 input_height = gc->input_height;
	u32 input_stride = gc->input_y_stride;
	u32 input_u_stride = gc->input_c_stride;
	u32 input_v_stride = gc->input_c_stride;
	u32 output_height = gc->output_height;
	u32 output_stride = gc->output_y_stride;
	u32 output_u_stride = gc->output_c_stride;
	u32 output_v_stride = gc->output_c_stride;

	gdc_log(LOG_DEBUG, "is_waiting_gdc=%d\n", gdc_cmd->is_waiting_gdc);
	if (gdc_cmd->is_waiting_gdc) {
		gdc_start_flag_write(0);
		gdc_log(LOG_CRIT, "No interrupt Still waiting...\n");
		gdc_start_flag_write(1);
		return -1;
	}

	gdc_log(LOG_DEBUG, "starting GDC process.\n");

	gdc_datain_width_write(input_width);
	gdc_datain_height_write(input_height);
	//input y plane
	gdc_data1in_addr_write(y_base_addr);
	gdc_data1in_line_offset_write(input_stride);

	//input u plane
	gdc_data2in_addr_write(u_base_addr);
	gdc_data2in_line_offset_write(input_u_stride);

	//input v plane
	gdc_data3in_addr_write(v_base_addr);
	gdc_data3in_line_offset_write(input_v_stride);

	//gdc y output
	gdc_data1out_addr_write(gdc_out_base_addr);
	gdc_data1out_line_offset_write(output_stride);

	//gdc u output
	if (gdc_cmd->outplane == 1)
		gdc_out_base_addr += output_height * output_stride;
	else
		gdc_out_base_addr = gdc_cmd->u_out_base_addr;
	gdc_data2out_addr_write(gdc_out_base_addr);
	gdc_data2out_line_offset_write(output_u_stride);

	//gdc v output
	if (gdc_cmd->outplane == 1)
		gdc_out_base_addr += output_height * output_u_stride;
	else
		gdc_out_base_addr = gdc_cmd->v_out_base_addr;
	gdc_data3out_addr_write(gdc_out_base_addr);
	gdc_data3out_line_offset_write(output_v_stride);
	gdc_start(gdc_cmd);

	return 0;
}

/**
 *   This function points gdc to its input resolution
 *
 *   and rgb address and offsets
 *
 *   Shown inputs to GDC are R\G\B plane address and offsets
 *
 *   @param  gdc_cmd - overall gdc settings and state
 *   @param  active_width -  input width resolution
 *   @param  active_height - input height resolution
 *   @param  y_base_addr -  input R base address
 *   @param  u_base_addr - input G base address
 *   @param  v_base_addr - input B base address
 *   @param  y_line_offset - input R line buffer offset
 *   @param  u_line_offset-  input G line buffer offer
 *   @param  v_line_offset-  input B line buffer offer
 *
 *   @return 0 - success
 *	 -1 - no interrupt from GDC.
 */
int gdc_process_rgb444p(struct gdc_cmd_s *gdc_cmd,
			u32 y_base_addr, u32 u_base_addr, u32 v_base_addr)
{
	struct gdc_config_s  *gc = &gdc_cmd->gdc_config;
	u32 gdc_out_base_addr = gdc_cmd->current_addr;
	u32 input_width = gc->input_width;
	u32 input_height = gc->input_height;
	u32 input_stride = gc->input_y_stride;
	u32 input_u_stride = gc->input_c_stride;
	u32 input_v_stride = gc->input_c_stride;
	u32 output_height = gc->output_height;
	u32 output_stride = gc->output_y_stride;
	u32 output_u_stride = gc->output_c_stride;
	u32 output_v_stride = gc->output_c_stride;

	gdc_log(LOG_DEBUG, "is_waiting_gdc=%d\n", gdc_cmd->is_waiting_gdc);
	if (gdc_cmd->is_waiting_gdc) {
		gdc_start_flag_write(0);
		gdc_log(LOG_CRIT, "No interrupt Still waiting...\n");
		gdc_start_flag_write(1);
		return -1;
	}

	gdc_log(LOG_DEBUG, "starting GDC process.\n");

	gdc_datain_width_write(input_width);
	gdc_datain_height_write(input_height);
	//input y plane
	gdc_data1in_addr_write(y_base_addr);
	gdc_data1in_line_offset_write(input_stride);

	//input u plane
	gdc_data2in_addr_write(u_base_addr);
	gdc_data2in_line_offset_write(input_u_stride);

	//input v plane
	gdc_data3in_addr_write(v_base_addr);
	gdc_data3in_line_offset_write(input_v_stride);

	//gdc y output
	gdc_data1out_addr_write(gdc_out_base_addr);
	gdc_data1out_line_offset_write(output_stride);

	//gdc u output
	if (gdc_cmd->outplane == 1)
		gdc_out_base_addr += output_height * output_stride;
	else
		gdc_out_base_addr = gdc_cmd->u_out_base_addr;
	gdc_data2out_addr_write(gdc_out_base_addr);
	gdc_data2out_line_offset_write(output_u_stride);

	//gdc v output
	if (gdc_cmd->outplane == 1)
		gdc_out_base_addr += output_height * output_u_stride;
	else
		gdc_out_base_addr = gdc_cmd->v_out_base_addr;
	gdc_data3out_addr_write(gdc_out_base_addr);
	gdc_data3out_line_offset_write(output_v_stride);
	gdc_start(gdc_cmd);

	return 0;
}

/**
 *   This function set the GDC power on/off
 *
 *   @param enable - power off/on
 *   @return  0 - success
 *           -1 - fail.
 */
int gdc_pwr_config(bool enable)
{
	struct meson_gdc_dev_t *gdc_dev = gdc_manager.gdc_dev;

	if (!gdc_dev ||
	    !gdc_dev->clk_core ||
	    !gdc_dev->clk_axi) {
		gdc_log(LOG_ERR, "core/axi set err.\n");
		return -1;
	}

	/* clk */
	if (enable) {
		clk_prepare_enable(gdc_dev->clk_core);
		clk_prepare_enable(gdc_dev->clk_axi);
	} else {
		clk_disable_unprepare(gdc_dev->clk_core);
		clk_disable_unprepare(gdc_dev->clk_axi);
	}

	return 0;
}
