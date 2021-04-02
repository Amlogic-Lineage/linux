// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/amlogic/media/vout/hdmi_tx21/hdmi_tx_ext.h>

int register_earcrx_callback(pf_callback callback)
{
	pf_callback *hdmitx;
#ifdef CONFIG_AMLOGIC_HDMITX
	hdmitx = hdmitx_earc_hpdst();
	*hdmitx = callback;
#endif

#ifdef CONFIG_AMLOGIC_HDMITX21
	hdmitx = hdmitx21_earc_hpdst();
	*hdmitx = callback;
#endif

	return 0;
}
EXPORT_SYMBOL(register_earcrx_callback);

void unregister_earcrx_callback(void)
{
	pf_callback *hdmitx;
#ifdef CONFIG_AMLOGIC_HDMITX
	hdmitx = hdmitx_earc_hpdst();
	*hdmitx = NULL;
#endif

#ifdef CONFIG_AMLOGIC_HDMITX21
	hdmitx = hdmitx21_earc_hpdst();
	*hdmitx = NULL;
#endif
}
EXPORT_SYMBOL(unregister_earcrx_callback);
