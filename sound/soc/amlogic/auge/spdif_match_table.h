/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#define SPDIF_A	0
#define SPDIF_B	1

struct spdif_chipinfo {
	unsigned int id;

	/* add ch_cnt to ch_num */
	bool chnum_en;
	/*
	 * axg, clear all irq bits
	 * after axg, such as g12a, clear each bits
	 * Reg_clr_interrupt[7:0] for each bit of irq_status[7:0];
	 */
	bool clr_irq_all_bits;
	/* no PaPb irq */
	bool irq_no_papb;
	/* reg_hold_start_en; 1: add delay to match TDM out when share buff; */
	bool hold_start;
	/* eq/drc */
	bool eq_drc_en;
	/* pc, pd interrupt is separated. */
	bool pcpd_separated;
	/* same source, spdif re-enable */
	bool same_src_spdif_reen;
	/* async fifo */
	bool async_fifo;
};

struct spdif_chipinfo axg_spdif_chipinfo = {
	.id               = SPDIF_A,
	.irq_no_papb      = true,
	.clr_irq_all_bits = true,
	.pcpd_separated   = true,
};

struct spdif_chipinfo g12a_spdif_a_chipinfo = {
	.id             = SPDIF_A,
	.chnum_en       = true,
	.hold_start     = true,
	.eq_drc_en      = true,
	.pcpd_separated = true,
};

struct spdif_chipinfo g12a_spdif_b_chipinfo = {
	.id             = SPDIF_B,
	.chnum_en       = true,
	.hold_start     = true,
	.eq_drc_en      = true,
	.pcpd_separated = true,
};

struct spdif_chipinfo tl1_spdif_a_chipinfo = {
	.id           = SPDIF_A,
	.chnum_en     = true,
	.hold_start   = true,
	.eq_drc_en    = true,
	.async_fifo   = true,
};

struct spdif_chipinfo tl1_spdif_b_chipinfo = {
	.id           = SPDIF_B,
	.chnum_en     = true,
	.hold_start   = true,
	.eq_drc_en    = true,
	.async_fifo   = true,
};

struct spdif_chipinfo sm1_spdif_a_chipinfo = {
	.id           = SPDIF_A,
	.chnum_en     = true,
	.hold_start   = true,
	.eq_drc_en    = true,
	.async_fifo   = true,
};

struct spdif_chipinfo sm1_spdif_b_chipinfo = {
	.id           = SPDIF_B,
	.chnum_en     = true,
	.hold_start   = true,
	.eq_drc_en    = true,
	.async_fifo   = true,
};

struct spdif_chipinfo tm2_spdif_a_chipinfo = {
	.id           = SPDIF_A,
	.chnum_en     = true,
	.hold_start   = true,
	.eq_drc_en    = true,
	.async_fifo   = true,
};

struct spdif_chipinfo tm2_spdif_b_chipinfo = {
	.id           = SPDIF_B,
	.chnum_en     = true,
	.hold_start   = true,
	.eq_drc_en    = true,
	.async_fifo   = true,
};

static const struct of_device_id aml_spdif_device_id[] = {
	{
		.compatible = "amlogic, sm1-snd-spdif-a",
		.data		= &sm1_spdif_a_chipinfo,
	},
	{
		.compatible = "amlogic, sm1-snd-spdif-b",
		.data		= &sm1_spdif_b_chipinfo,
	},
	{
		.compatible = "amlogic, tm2-snd-spdif-a",
		.data		= &tm2_spdif_a_chipinfo,
	},
	{
		.compatible = "amlogic, tm2-snd-spdif-b",
		.data		= &tm2_spdif_b_chipinfo,
	},
	{},
};
MODULE_DEVICE_TABLE(of, aml_spdif_device_id);
