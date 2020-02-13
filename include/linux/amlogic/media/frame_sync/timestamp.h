/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef TIMESTAMP_H
#define TIMESTAMP_H

#ifdef CONFIG_AMAUDIO
//int resample_delta;
#endif

u32 timestamp_vpts_get(void);

void timestamp_vpts_set(u32 pts);

void timestamp_vpts_inc(s32 val);

u32 timestamp_apts_get(void);

void timestamp_apts_set(u32 pts);

void timestamp_apts_inc(s32 val);

u32 timestamp_pcrscr_get(void);

void timestamp_pcrscr_set(u32 pts);

void timestamp_pcrscr_inc(s32 val);

void timestamp_pcrscr_inc_scale(s32 inc, u32 base);

void timestamp_pcrscr_enable(u32 enable);

u32 timestamp_pcrscr_enable_state(void);

void timestamp_pcrscr_set_adj(s32 inc);

void timestamp_pcrscr_set_adj_pcr(s32 inc);

void timestamp_apts_enable(u32 enable);

void timestamp_apts_start(u32 enable);

u32 timestamp_apts_started(void);

void timestamp_firstvpts_set(u32 pts);

u32 timestamp_firstvpts_get(void);

void timestamp_checkin_firstvpts_set(u32 pts);

u32 timestamp_checkin_firstvpts_get(void);

void timestamp_checkin_firstapts_set(u32 pts);

u32 timestamp_checkin_firstapts_get(void);

void timestamp_firstapts_set(u32 pts);

u32 timestamp_firstapts_get(void);

u32 timestamp_tsdemux_pcr_get(void);

#endif /* TIMESTAMP_H */
