/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef __EFFECTS_V2_H__
#define __EFFECTS_V2_H__

enum {
	VERSION1 = 0,
	VERSION2,
	VERSION3
};

int check_aed_version(void);
int card_add_effect_v2_kcontrols(struct snd_soc_card *card);
int get_aed_dst(void);

#endif
