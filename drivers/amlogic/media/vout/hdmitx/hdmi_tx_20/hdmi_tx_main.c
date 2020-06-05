// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/major.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/pinctrl/consumer.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/ctype.h>
#include <linux/extcon.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/reboot.h>
#include "../../../../drivers/extcon/extcon.h"
#include <linux/extcon-provider.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
//#include <linux/amlogic/cpu_version.h>
#include <linux/amlogic/media/vout/vinfo.h>
#include <linux/amlogic/media/vout/vout_notify.h>
#ifdef CONFIG_AMLOGIC_SND_SOC
#include <linux/amlogic/media/sound/aout_notify.h>
#endif
#include <linux/amlogic/media/vout/hdmi_tx/hdmi_info_global.h>
#include <linux/amlogic/media/vout/hdmi_tx/hdmi_tx_ddc.h>
#include <linux/amlogic/media/vout/hdmi_tx/hdmi_tx_module.h>
#include <linux/amlogic/media/vout/hdmi_tx/hdmi_config.h>
#include "hw/tvenc_conf.h"
#include "hw/common.h"
#include "hw/hw_clk.h"
#include "hw/reg_ops.h"
#include "hdmi_tx_hdcp.h"

#include <linux/amlogic/gki_module.h>

#define DEVICE_NAME "amhdmitx"
#define HDMI_TX_COUNT 32
#define HDMI_TX_POOL_NUM  6
#define HDMI_TX_RESOURCE_NUM 4
#define HDMI_TX_PWR_CTRL_NUM	6

static struct class *hdmitx_class;
static int set_disp_mode_auto(void);
static void hdmitx_get_edid(struct hdmitx_dev *hdev);
static void hdmitx_set_drm_pkt(struct master_display_info_s *data);
static void hdmitx_set_vsif_pkt(enum eotf_type type, enum mode_type
	tunnel_mode, struct dv_vsif_para *data, bool signal_sdr);
static void hdmitx_set_hdr10plus_pkt(unsigned int flag,
				     struct hdr10plus_para *data);
static void hdmitx_set_emp_pkt(unsigned char *data,
			       unsigned int type,
			       unsigned int size);
static int check_fbc_special(unsigned char *edid_dat);
static void hdmitx_fmt_attr(struct hdmitx_dev *hdev);
static void clear_rx_vinfo(struct hdmitx_dev *hdev);
static void edidinfo_attach_to_vinfo(struct hdmitx_dev *hdev);
static void edidinfo_detach_to_vinfo(struct hdmitx_dev *hdev);


#ifdef CONFIG_AMLOGIC_VOUT_SERVE
static struct vinfo_s *hdmitx_get_current_vinfo(void);
#else
static struct vinfo_s *hdmitx_get_current_vinfo(void)
{
	return NULL;
}
#endif

#ifdef CONFIG_OF
static struct amhdmitx_data_s amhdmitx_data_g12a = {
	.chip_type = MESON_CPU_ID_G12A,
	.chip_name = "g12a",
};

static struct amhdmitx_data_s amhdmitx_data_g12b = {
	.chip_type = MESON_CPU_ID_G12B,
	.chip_name = "g12b",
};

static struct amhdmitx_data_s amhdmitx_data_sm1 = {
	.chip_type = MESON_CPU_ID_SM1,
	.chip_name = "sm1",
};

static const struct of_device_id meson_amhdmitx_of_match[] = {
	{
		.compatible	 = "amlogic, amhdmitx-g12a",
		.data = &amhdmitx_data_g12a,
	},
	{
		.compatible	 = "amlogic, amhdmitx-g12b",
		.data = &amhdmitx_data_g12b,
	},
	{
		.compatible	 = "amlogic, amhdmitx-sm1",
		.data = &amhdmitx_data_sm1,
	},
	{},
};
#else
#define meson_amhdmitx_dt_match NULL
#endif

static DEFINE_MUTEX(setclk_mutex);
static DEFINE_MUTEX(getedid_mutex);

static struct hdmitx_dev hdmitx_device = {
	.frac_rate_policy = 1,
};

static struct hdmitx_report hdmitx_status;

static const struct dv_info dv_dummy;
static int log_level;

struct vout_device_s hdmitx_vdev = {
	.dv_info = &hdmitx_device.rxcap.dv_info,
	.fresh_tx_hdr_pkt = hdmitx_set_drm_pkt,
	.fresh_tx_vsif_pkt = hdmitx_set_vsif_pkt,
	.fresh_tx_hdr10plus_pkt = hdmitx_set_hdr10plus_pkt,
	.fresh_tx_emp_pkt = hdmitx_set_emp_pkt,
};

struct hdmi_config_platform_data *hdmi_pdata;

static const unsigned int hdmi_cable[] = {
	EXTCON_DISP_HDMI,
	EXTCON_NONE,
};

struct extcon_dev *hdmitx_extcon_hdmi;
struct extcon_dev *hdmitx_extcon_audio;
struct extcon_dev *hdmitx_extcon_power;
struct extcon_dev *hdmitx_extcon_hdr;
struct extcon_dev *hdmitx_extcon_rxsense;
struct extcon_dev *hdmitx_extcon_hdcp;
struct extcon_dev *hdmitx_extcon_cedst;

/* There are 3 callback functions for front HDR/DV/HDR10+ modules to notify
 * hdmi drivers to send out related HDMI infoframe
 * hdmitx_set_drm_pkt() is for HDR 2084 SMPTE, HLG, etc.
 * hdmitx_set_vsif_pkt() is for DV
 * hdmitx_set_hdr10plus_pkt is for HDR10+
 * Front modules may call the 2nd, and next call the 1st, and the realted flags
 * are remained the same. So, add hdr_status_pos and place it in the above 3
 * functions to record the position.
 */
int hdr_status_pos;

static inline void hdmitx_notify_hpd(int hpd)
{
	if (hpd)
		hdmitx_event_notify(HDMITX_PLUG, NULL);
	else
		hdmitx_event_notify(HDMITX_UNPLUG, NULL);
}

#ifdef CONFIG_AMLOGIC_LEGACY_EARLY_SUSPEND
#include <linux/amlogic/pm.h>
static void hdmitx_early_suspend(struct early_suspend *h)
{
	struct hdmitx_dev *phdmi = (struct hdmitx_dev *)h->param;
	struct hdmitx_dev *hdev = phdmi;

	phdmi->ready = 0;
	phdmi->hpd_lock = 1;
	hdev->hwop.cntlmisc(hdev, MISC_SUSFLAG, 1);
	usleep_range(10000, 10010);
	phdmi->hwop.cntlmisc(phdmi, MISC_AVMUTE_OP, SET_AVMUTE);
	usleep_range(10000, 10010);
	pr_info(SYS "HDMITX: Early Suspend\n");
	phdmi->hwop.cntl((struct hdmitx_dev *)h->param,
		HDMITX_EARLY_SUSPEND_RESUME_CNTL, HDMITX_EARLY_SUSPEND);
	phdmi->cur_VIC = HDMI_UNKNOWN;
	phdmi->output_blank_flag = 0;
	phdmi->hwop.cntlddc(phdmi, DDC_HDCP_MUX_INIT, 1);
	phdmi->hwop.cntlddc(phdmi, DDC_HDCP_OP, HDCP14_OFF);
	hdmitx_set_vsif_pkt(0, 0, NULL, true);
	hdmitx_set_hdr10plus_pkt(0, NULL);
	clear_rx_vinfo(hdev);
	hdmitx_edid_clear(hdev);
	hdmitx_edid_ram_buffer_clear(hdev);
	edidinfo_detach_to_vinfo(hdev);
	hdmitx_status.pwr = 0;
	extcon_set_state_sync(hdmitx_extcon_power, EXTCON_DISP_HDMI, 0);
	phdmi->hwop.cntlconfig(&hdmitx_device, CONF_CLR_AVI_PACKET, 0);
	phdmi->hwop.cntlconfig(&hdmitx_device, CONF_CLR_VSDB_PACKET, 0);
}

static int hdmitx_is_hdmi_vmode(char *mode_name)
{
	enum hdmi_vic vic = hdmitx_edid_vic_tab_map_vic(mode_name);

	if (vic == HDMI_UNKNOWN)
		return 0;

	return 1;
}

static void hdmitx_late_resume(struct early_suspend *h)
{
	const struct vinfo_s *info = hdmitx_get_current_vinfo();
	struct hdmitx_dev *phdmi = (struct hdmitx_dev *)h->param;

	if (info && (hdmitx_is_hdmi_vmode(info->name) == 1))
		phdmi->hwop.cntlmisc(&hdmitx_device, MISC_HPLL_FAKE, 0);

	phdmi->hpd_lock = 0;

	/* update status for hpd and switch/state */
	hdmitx_device.hpd_state = !!(hdmitx_device.hwop.cntlmisc(&hdmitx_device,
		MISC_HPD_GPI_ST, 0));

	pr_info("hdmitx hpd state: %d\n", hdmitx_device.hpd_state);
	hdmitx_notify_hpd(hdmitx_device.hpd_state);

	/*force to get EDID after resume for Amplifier Power case*/
	if (hdmitx_device.hpd_state)
		hdmitx_get_edid(phdmi);

	hdmitx_device.hwop.cntlconfig(&hdmitx_device,
		CONF_AUDIO_MUTE_OP, AUDIO_MUTE);
	set_disp_mode_auto();

	extcon_set_state_sync(hdmitx_extcon_hdmi, EXTCON_DISP_HDMI,
			      hdmitx_device.hpd_state);
	hdmitx_status.pwr = 1;
	extcon_set_state_sync(hdmitx_extcon_power, EXTCON_DISP_HDMI, 1);
	extcon_set_state_sync(hdmitx_extcon_audio, EXTCON_DISP_HDMI,
			      hdmitx_device.hpd_state);
	hdmitx_status.audio = hdmitx_device.hpd_state;
	pr_info("amhdmitx: late resume module %d\n", __LINE__);
	phdmi->hwop.cntl((struct hdmitx_dev *)h->param,
		HDMITX_EARLY_SUSPEND_RESUME_CNTL, HDMITX_LATE_RESUME);
	phdmi->hwop.cntlmisc(phdmi, MISC_SUSFLAG, 0);
	pr_info(SYS "HDMITX: Late Resume\n");
}

/* Set avmute_set signal to HDMIRX */
static int hdmitx_reboot_notifier(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	struct hdmitx_dev *hdev = container_of(nb, struct hdmitx_dev, nb);

	hdev->ready = 0;
	hdev->hwop.cntlmisc(hdev, MISC_AVMUTE_OP, SET_AVMUTE);
	usleep_range(10000, 10010);
	hdev->hwop.cntlmisc(hdev, MISC_TMDS_PHY_OP, TMDS_PHY_DISABLE);
	hdev->hwop.cntl(hdev, HDMITX_EARLY_SUSPEND_RESUME_CNTL,
		HDMITX_EARLY_SUSPEND);

	return NOTIFY_OK;
}

static struct early_suspend hdmitx_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN - 10,
	.suspend = hdmitx_early_suspend,
	.resume = hdmitx_late_resume,
	.param = &hdmitx_device,
};
#endif

#define INIT_FLAG_VDACOFF		0x1
	/* unplug powerdown */
#define INIT_FLAG_POWERDOWN	  0x2

#define INIT_FLAG_NOT_LOAD 0x80

static unsigned char init_flag;
#undef DISABLE_AUDIO

int get_cur_vout_index(void)
/*
 * return value: 1, vout; 2, vout2;
 */
{
	int vout_index = 1;
	return vout_index;
}

static  int  set_disp_mode(const char *mode)
{
	int ret =  -1;
	enum hdmi_vic vic;

	vic = hdmitx_edid_get_VIC(&hdmitx_device, mode, 1);
	if (strncmp(mode, "2160p30hz", strlen("2160p30hz")) == 0)
		vic = HDMI_4k2k_30;
	else if (strncmp(mode, "2160p25hz", strlen("2160p25hz")) == 0)
		vic = HDMI_4k2k_25;
	else if (strncmp(mode, "2160p24hz", strlen("2160p24hz")) == 0)
		vic = HDMI_4k2k_24;
	else if (strncmp(mode, "smpte24hz", strlen("smpte24hz")) == 0)
		vic = HDMI_4k2k_smpte_24;
	else
		;/* nothing */

	if (strncmp(mode, "1080p60hz", strlen("1080p60hz")) == 0)
		vic = HDMI_1080p60;
	if (strncmp(mode, "1080p50hz", strlen("1080p50hz")) == 0)
		vic = HDMI_1080p50;

	if (vic != HDMI_UNKNOWN) {
		hdmitx_device.mux_hpd_if_pin_high_flag = 1;
		if (hdmitx_device.vic_count == 0) {
			if (hdmitx_device.unplug_powerdown)
				return 0;
		}
	}

	hdmitx_device.cur_VIC = HDMI_UNKNOWN;
	ret = hdmitx_set_display(&hdmitx_device, vic);
	if (ret >= 0) {
		hdmitx_device.hwop.cntl(&hdmitx_device,
			HDMITX_AVMUTE_CNTL, AVMUTE_CLEAR);
		hdmitx_device.cur_VIC = vic;
		hdmitx_device.audio_param_update_flag = 1;
		hdmitx_device.auth_process_timer = AUTH_PROCESS_TIME;
	}

	if (hdmitx_device.cur_VIC == HDMI_UNKNOWN) {
		if (hdmitx_device.hpdmode == 2) {
			/* edid will be read again when hpd is muxed and
			 * it is high
			 */
			hdmitx_edid_clear(&hdmitx_device);
			hdmitx_device.mux_hpd_if_pin_high_flag = 0;
		}
		if (hdmitx_device.hwop.cntl) {
			hdmitx_device.hwop.cntl(&hdmitx_device,
				HDMITX_HWCMD_TURNOFF_HDMIHW,
				(hdmitx_device.hpdmode == 2) ? 1 : 0);
		}
	}
	return ret;
}

static void hdmitx_pre_display_init(void)
{
	hdmitx_device.cur_VIC = HDMI_UNKNOWN;
	hdmitx_device.auth_process_timer = AUTH_PROCESS_TIME;
	hdmitx_device.hwop.cntlconfig(&hdmitx_device,
		CONF_AUDIO_MUTE_OP, AUDIO_MUTE);
	hdmitx_device.hwop.cntlddc(&hdmitx_device, DDC_HDCP_MUX_INIT, 1);
	hdmitx_device.hwop.cntlddc(&hdmitx_device, DDC_HDCP_OP, HDCP14_OFF);
	/* msleep(10); */
	hdmitx_device.hwop.cntlmisc(&hdmitx_device, MISC_TMDS_PHY_OP,
		TMDS_PHY_DISABLE);
	hdmitx_device.hwop.cntlconfig(&hdmitx_device,
		CONF_CLR_AVI_PACKET, 0);
	hdmitx_device.hwop.cntlconfig(&hdmitx_device,
		CONF_CLR_VSDB_PACKET, 0);
}

static void hdmi_physcial_size_update(struct hdmitx_dev *hdev)
{
	unsigned int width, height;
	struct vinfo_s *info = NULL;

	info = hdmitx_get_current_vinfo();
	if (!info || !info->name) {
		pr_info(SYS "cann't get valid mode\n");
		return;
	}

	if (info->mode == VMODE_HDMI) {
		width = hdev->rxcap.physcial_weight;
		height = hdev->rxcap.physcial_height;
		if (width == 0 || height == 0) {
			info->screen_real_width = info->aspect_ratio_num;
			info->screen_real_height = info->aspect_ratio_den;
		} else {
			/* transfer mm */
			info->screen_real_width = width * 10;
			info->screen_real_height = height * 10;
		}
		pr_info(SYS "update physcial size: %d %d\n",
			info->screen_real_width, info->screen_real_height);
	}
}

static void hdrinfo_to_vinfo(struct vinfo_s *info, struct hdmitx_dev *hdev)
{
	unsigned int i, j;
	/*static hdr*/
	info->hdr_info.hdr_support = (hdev->rxcap.hdr_sup_eotf_sdr << 0)
			| (hdev->rxcap.hdr_sup_eotf_hdr << 1)
			| (hdev->rxcap.hdr_sup_eotf_smpte_st_2084 << 2)
			| (hdev->rxcap.hdr_sup_eotf_hlg << 3);
	memcpy(info->hdr_info.rawdata, hdev->rxcap.hdr_rawdata, 7);
	/*dynamic hdr*/
	for (i = 0; i < 4; i++) {
		if (hdev->rxcap.hdr_dynamic_info[i].type == 0) {
			memset(&info->hdr_info.dynamic_info[i],
			       0, sizeof(struct hdr_dynamic));
			continue;
		}
		info->hdr_info.dynamic_info[i].type =
			hdev->rxcap.hdr_dynamic_info[i].type;
		info->hdr_info.dynamic_info[i].of_len =
			hdev->rxcap.hdr_dynamic_info[i].hd_len - 3;
		info->hdr_info.dynamic_info[i].support_flags =
			hdev->rxcap.hdr_dynamic_info[i].support_flags;

		for (j = 0; j < hdev->rxcap.hdr_dynamic_info[i].hd_len - 3; j++)
			info->hdr_info.dynamic_info[i].optional_fields[j] =
			hdev->rxcap.hdr_dynamic_info[i].optional_fields[j];
	}
	/*hdr 10+*/
	memcpy(&info->hdr_info.hdr10plus_info,
	       &hdev->rxcap.hdr10plus_info, sizeof(struct hdr10_plus_info));

	info->hdr_info.colorimetry_support =
		hdev->rxcap.colorimetry_data;
	info->hdr_info.lumi_max = hdev->rxcap.hdr_lum_max;
	info->hdr_info.lumi_avg = hdev->rxcap.hdr_lum_avg;
	info->hdr_info.lumi_min = hdev->rxcap.hdr_lum_min;
	pr_info(SYS "update rx hdr info %x\n",
		info->hdr_info.hdr_support);
}

static void rxlatency_to_vinfo(struct vinfo_s *info, struct rx_cap *rx)
{
	if (!info || !rx)
		return;
	info->rx_latency.vLatency = rx->vLatency;
	info->rx_latency.aLatency = rx->aLatency;
	info->rx_latency.i_vLatency = rx->i_vLatency;
	info->rx_latency.i_aLatency = rx->i_aLatency;
}

static void edidinfo_attach_to_vinfo(struct hdmitx_dev *hdev)
{
	struct vinfo_s *info = NULL;

	/* get current vinfo */
	info = hdmitx_get_current_vinfo();
	if (!info || !info->name)
		return;

	if (strncmp(info->name, "480cvbs", 7) == 0 ||
	    strncmp(info->name, "576cvbs", 7) == 0 ||
	    strncmp(info->name, "null", 4) == 0)
		return;

	hdrinfo_to_vinfo(info, hdev);
	rxlatency_to_vinfo(info, &hdev->rxcap);
	hdmitx_vdev.dv_info = &hdmitx_device.rxcap.dv_info;
}

static void edidinfo_detach_to_vinfo(struct hdmitx_dev *hdev)
{
	struct vinfo_s *info = NULL;

	/* get current vinfo */
	info = hdmitx_get_current_vinfo();
	if (!info || !info->name)
		return;

	edidinfo_attach_to_vinfo(hdev);
	hdmitx_vdev.dv_info = &dv_dummy;
}

static int set_disp_mode_auto(void)
{
	int ret =  -1;

	struct vinfo_s *info = NULL;
	struct hdmitx_dev *hdev = &hdmitx_device;
	struct hdmi_format_para *para = NULL;
	unsigned char mode[32];
	enum hdmi_vic vic = HDMI_UNKNOWN;
	/* vic_ready got from IP */
	enum hdmi_vic vic_ready =
	hdev->hwop.getstate(hdev, STAT_VIDEO_VIC, 0);

	memset(mode, 0, sizeof(mode));
	hdev->ready = 0;

	/* get current vinfo */
	info = hdmitx_get_current_vinfo();
	if (!info || !info->name)
		return -1;

	pr_info(SYS "get current mode: %s\n", info->name);

	/*update hdmi checksum to vout*/
	memcpy(info->hdmichecksum, hdev->rxcap.chksum, 10);

	hdmi_physcial_size_update(hdev);

	/* If info->name equals to cvbs, then set mode to I mode to hdmi
	 */
	if ((strncmp(info->name, "480cvbs", 7) == 0) ||
	    (strncmp(info->name, "576cvbs", 7) == 0) ||
	    (strncmp(info->name, "ntsc_m", 6) == 0) ||
	    (strncmp(info->name, "pal_m", 5) == 0) ||
	    (strncmp(info->name, "pal_n", 5) == 0) ||
	    (strncmp(info->name, "panel", 5) == 0) ||
	    (strncmp(info->name, "null", 4) == 0)) {
		pr_info(SYS "%s not valid hdmi mode\n", info->name);
		hdev->hwop.cntlconfig(hdev, CONF_CLR_AVI_PACKET, 0);
		hdev->hwop.cntlconfig(hdev, CONF_CLR_VSDB_PACKET, 0);
		hdev->hwop.cntlmisc(hdev, MISC_TMDS_PHY_OP, TMDS_PHY_DISABLE);
		hdev->para = hdmi_get_fmt_name("invalid", hdev->fmt_attr);
		if (hdev->cedst_policy)
			cancel_delayed_work(&hdev->work_cedst);
		return -1;
	}
	strncpy(mode, info->name, sizeof(mode));
	mode[31] = '\0';
	if (strstr(mode, "fp")) {
		int i = 0;

		for (; mode[i]; i++) {
			if ((mode[i] == 'f') && (mode[i + 1] == 'p')) {
				/* skip "f", 1080fp60hz -> 1080p60hz */
				do {
					mode[i] = mode[i + 1];
					i++;
				} while (mode[i]);
				break;
			}
		}
	}

	/* In the file hdmi_common/hdmi_parameters.c,
	 * the data array all_fmt_paras[] treat 2160p60hz and 2160p60hz420
	 * as two different modes, such Scrambler
	 * So if node "attr" contains 420, need append 420 to mode.
	 */
	if (strstr(hdev->fmt_attr, "420")) {
		if (!strstr(mode, "420"))
			strcat(mode, "420");
	}

	para = hdmi_get_fmt_name(mode, hdev->fmt_attr);
	hdev->para = para;
	vic = hdmitx_edid_get_VIC(hdev, mode, 1);
	if (strncmp(info->name, "2160p30hz", strlen("2160p30hz")) == 0) {
		vic = HDMI_4k2k_30;
	} else if (strncmp(info->name, "2160p25hz",
		strlen("2160p25hz")) == 0) {
		vic = HDMI_4k2k_25;
	} else if (strncmp(info->name, "2160p24hz",
		strlen("2160p24hz")) == 0) {
		vic = HDMI_4k2k_24;
	} else if (strncmp(info->name, "smpte24hz",
		strlen("smpte24hz")) == 0) {
		vic = HDMI_4k2k_smpte_24;
	} else {
	/* nothing */
	}
	if (vic_ready != HDMI_UNKNOWN && vic_ready == vic) {
		pr_info(SYS "[%s] ALREADY init VIC = %d\n",
			__func__, vic);
		if (hdev->rxcap.ieeeoui == 0) {
			/* DVI case judgement. In uboot, directly output HDMI
			 * mode
			 */
			hdev->hwop.cntlconfig(hdev, CONF_HDMI_DVI_MODE,
				DVI_MODE);
			pr_info(SYS "change to DVI mode\n");
		} else if ((hdev->rxcap.ieeeoui == 0xc03) &&
		(hdev->hwop.cntlconfig(hdev, CONF_GET_HDMI_DVI_MODE, 0)
			== DVI_MODE)) {
			hdev->hwop.cntlconfig(hdev, CONF_HDMI_DVI_MODE,
				HDMI_MODE);
			pr_info(SYS "change to HDMI mode\n");
		}
		hdev->cur_VIC = vic;
		hdev->output_blank_flag = 1;
		hdev->ready = 1;
		edidinfo_attach_to_vinfo(hdev);

		return 1;
	}

	hdmitx_pre_display_init();

	hdev->cur_VIC = HDMI_UNKNOWN;
/* if vic is HDMI_UNKNOWN, hdmitx_set_display will disable HDMI */
	ret = hdmitx_set_display(hdev, vic);

	if (ret >= 0) {
		hdev->hwop.cntl(hdev, HDMITX_AVMUTE_CNTL, AVMUTE_CLEAR);
		hdev->cur_VIC = vic;
		hdev->audio_param_update_flag = 1;
		hdev->auth_process_timer = AUTH_PROCESS_TIME;
	}
	if (hdev->cur_VIC == HDMI_UNKNOWN) {
		if (hdev->hpdmode == 2) {
			/* edid will be read again when hpd is muxed
			 * and it is high
			 */
			hdmitx_edid_clear(hdev);
			hdev->mux_hpd_if_pin_high_flag = 0;
		}
		/* If current display is NOT panel, needn't TURNOFF_HDMIHW */
		if (strncmp(mode, "panel", 5) == 0) {
			hdev->hwop.cntl(hdev, HDMITX_HWCMD_TURNOFF_HDMIHW,
				(hdev->hpdmode == 2) ? 1 : 0);
		}
	}
	hdmitx_set_audio(hdev, &hdev->cur_audio_param);
	if (hdev->cedst_policy) {
		cancel_delayed_work(&hdev->work_cedst);
		queue_delayed_work(hdev->cedst_wq, &hdev->work_cedst, 0);
	}
	hdev->output_blank_flag = 1;
	hdev->ready = 1;
	edidinfo_attach_to_vinfo(hdev);
	return ret;
}

/*disp_mode attr*/
static ssize_t disp_mode_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "VIC:%d\n",
		hdmitx_device.cur_VIC);
	return pos;
}

static ssize_t disp_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	set_disp_mode(buf);
	return count;
}

static ssize_t attr_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	int pos = 0;

	if (!memcmp(hdmitx_device.fmt_attr, "default,", 7)) {
		memset(hdmitx_device.fmt_attr, 0,
		       sizeof(hdmitx_device.fmt_attr));
		hdmitx_fmt_attr(&hdmitx_device);
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "%s\n\r", hdmitx_device.fmt_attr);
	return pos;
}

ssize_t attr_store(struct device *dev,
		   struct device_attribute *attr,
		   const char *buf, size_t count)
{
	strncpy(hdmitx_device.fmt_attr, buf, sizeof(hdmitx_device.fmt_attr));
	hdmitx_device.fmt_attr[15] = '\0';
	if (!memcmp(hdmitx_device.fmt_attr, "rgb", 3))
		hdmitx_device.para->cs = COLORSPACE_RGB444;
	else if (!memcmp(hdmitx_device.fmt_attr, "422", 3))
		hdmitx_device.para->cs = COLORSPACE_YUV422;
	else if (!memcmp(hdmitx_device.fmt_attr, "420", 3))
		hdmitx_device.para->cs = COLORSPACE_YUV420;
	else
		hdmitx_device.para->cs = COLORSPACE_YUV444;
	return count;
}

/*aud_mode attr*/

void setup_attr(const char *buf)
{
	char attr[16] = {0};

	memcpy(attr, buf, sizeof(attr));
	memcpy(hdmitx_device.fmt_attr, attr, sizeof(hdmitx_device.fmt_attr));
}
EXPORT_SYMBOL(setup_attr);

void get_attr(char attr[16])
{
	memcpy(attr, hdmitx_device.fmt_attr, sizeof(hdmitx_device.fmt_attr));
}
EXPORT_SYMBOL(get_attr);

/* for android application data exchange / swap */
static char *tmp_swap;
static DEFINE_MUTEX(mutex_swap);

static ssize_t swap_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	pr_info("%s: %s\n", __func__, buf);
	mutex_lock(&mutex_swap);

	kfree(tmp_swap);
	tmp_swap = kzalloc(count + 1, GFP_KERNEL);
	if (!tmp_swap) {
		mutex_unlock(&mutex_swap);
		return count;
	}
	memcpy(tmp_swap, buf, count);
	tmp_swap[count] = '\0'; /* padding end string */
	mutex_unlock(&mutex_swap);
	return count;
}

static ssize_t swap_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	int i = 0;
	int n = 0;
	struct hdmitx_dev *hdev = &hdmitx_device;
	struct rx_cap *prxcap = &hdev->rxcap;
	struct hdcprp14_topo *topo14 = &hdev->topo_info->topo.topo14;

	mutex_lock(&mutex_swap);

	if (!tmp_swap ||
	    (!hdev->edid_parsing && !strstr(tmp_swap, "hdcp.topo"))) {
		mutex_unlock(&mutex_swap);
		return n;
	}

	/* VSD: Video Short Descriptor */
	if (strstr(tmp_swap, "edid.vsd"))
		for (i = 0; i < prxcap->vsd.len; i++)
			n += snprintf(buf + n, PAGE_SIZE - n, "%02x",
				prxcap->vsd.raw[i]);
	/* ASD: Audio Short Descriptor */
	if (strstr(tmp_swap, "edid.asd"))
		for (i = 0; i < prxcap->asd.len; i++)
			n += snprintf(buf + n, PAGE_SIZE - n, "%02x",
				prxcap->asd.raw[i]);
	/* CEC: Physical Address */
	if (strstr(tmp_swap, "edid.cec"))
		n += snprintf(buf + n, PAGE_SIZE - n, "%x%x%x%x",
			hdev->hdmi_info.vsdb_phy_addr.a,
			hdev->hdmi_info.vsdb_phy_addr.b,
			hdev->hdmi_info.vsdb_phy_addr.c,
			hdev->hdmi_info.vsdb_phy_addr.d);
	/* HDCP TOPO */
	if (strstr(tmp_swap, "hdcp.topo")) {
		char *tmp = (char *)topo14;

		pr_info("max_cascade_exceeded %d\n",
			topo14->max_cascade_exceeded);
		pr_info("depth %d\n", topo14->depth);
		pr_info("max_devs_exceeded %d\n", topo14->max_devs_exceeded);
		pr_info("device_count %d\n", topo14->device_count);
		for (i = 0; i < sizeof(struct hdcprp14_topo); i++)
			n += snprintf(buf + n, PAGE_SIZE - n, "%02x", tmp[i]);
	}

	kfree(tmp_swap);
	tmp_swap = NULL;

	mutex_unlock(&mutex_swap);
	return n;
}

static ssize_t aud_mode_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	return 0;
}

static ssize_t aud_mode_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	/* set_disp_mode(buf); */
	struct hdmitx_audpara *audio_param =
		&hdmitx_device.cur_audio_param;
	if (strncmp(buf, "32k", 3) == 0) {
		audio_param->sample_rate = FS_32K;
	} else if (strncmp(buf, "44.1k", 5) == 0) {
		audio_param->sample_rate = FS_44K1;
	} else if (strncmp(buf, "48k", 3) == 0) {
		audio_param->sample_rate = FS_48K;
	} else {
		hdmitx_device.force_audio_flag = 0;
		return count;
	}
	audio_param->type = CT_PCM;
	audio_param->channel_num = CC_2CH;
	audio_param->sample_size = SS_16BITS;

	hdmitx_device.audio_param_update_flag = 1;
	hdmitx_device.force_audio_flag = 1;

	return count;
}

/*edid attr*/
static ssize_t edid_show(struct device *dev,
			 struct device_attribute *attr,
			 char *buf)
{
	return hdmitx_edid_dump(&hdmitx_device, buf, PAGE_SIZE);
}

static int dump_edid_data(unsigned int type, char *path)
{
	struct file *filp = NULL;
	loff_t pos = 0;
	char line[128] = {0};
	mm_segment_t old_fs = get_fs();
	unsigned int i = 0, j = 0, k = 0, size = 0, block_cnt = 0;
	unsigned int index = 0, tmp = 0;

	set_fs(KERNEL_DS);
	filp = filp_open(path, O_RDWR | O_CREAT, 0666);
	if (IS_ERR(filp)) {
		pr_info("[%s] failed to open/create file: |%s|\n",
			__func__, path);
		goto PROCESS_END;
	}

	block_cnt = hdmitx_device.EDID_buf[0x7e] + 1;
	if (type == 1) {
		/* dump as bin file*/
		size = vfs_write(filp, hdmitx_device.EDID_buf,
				 block_cnt * 128, &pos);
	} else if (type == 2) {
		/* dump as txt file*/

		for (i = 0; i < block_cnt; i++) {
			for (j = 0; j < 8; j++) {
				for (k = 0; k < 16; k++) {
					index = i * 128 + j * 16 + k;
					tmp = hdmitx_device.EDID_buf[index];
					snprintf((char *)&line[k * 6], 7,
						 "0x%02x, ",
						 tmp);
				}
				line[16 * 6 - 1] = '\n';
				line[16 * 6] = 0x0;
				pos = (i * 8 + j) * 16 * 6;
				size += vfs_write(filp, line, 16 * 6, &pos);
			}
		}
	}

	pr_info("[%s] write %d bytes to file %s\n", __func__, size, path);

	vfs_fsync(filp, 0);
	filp_close(filp, NULL);

PROCESS_END:
	set_fs(old_fs);
	return 0;
}

static int load_edid_data(unsigned int type, char *path)
{
	struct file *filp = NULL;
	loff_t pos = 0;
	mm_segment_t old_fs = get_fs();

	struct kstat stat;
	unsigned int length = 0, max_len = EDID_MAX_BLOCK * 128;
	char *buf = NULL;

	set_fs(KERNEL_DS);

	filp = filp_open(path, O_RDONLY, 0444);
	if (IS_ERR(filp)) {
		pr_info("[%s] failed to open file: |%s|\n", __func__, path);
		goto PROCESS_END;
	}

	WARN_ON(vfs_stat(path, &stat));

	length = (stat.size > max_len) ? max_len : stat.size;

	buf = kmalloc(length, GFP_KERNEL);
	if (!buf)
		goto PROCESS_END;

	vfs_read(filp, buf, length, &pos);

	memcpy(hdmitx_device.EDID_buf, buf, length);

	kfree(buf);
	filp_close(filp, NULL);

	pr_info("[%s] %d bytes loaded from file %s\n", __func__, length, path);

	hdmitx_edid_clear(&hdmitx_device);
	hdmitx_edid_parse(&hdmitx_device);
	pr_info("[%s] new edid loaded!\n", __func__);

PROCESS_END:
	set_fs(old_fs);
	return 0;
}

static ssize_t edid_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	unsigned int argn = 0;
	char *p = NULL, *para = NULL, *argv[8] = {NULL};
	unsigned int path_length = 0;
	unsigned int index = 0, tmp = 0;

	p = kstrdup(buf, GFP_KERNEL);
	if (!p)
		return count;

	do {
		para = strsep(&p, " ");
		if (para) {
			argv[argn] = para;
			argn++;
			if (argn > 7)
				break;
		}
	} while (para);

	if (buf[0] == 'h') {
		int i;

		pr_info(EDID "EDID hash value:\n");
		for (i = 0; i < 20; i++)
			pr_info("%02x", hdmitx_device.EDID_hash[i]);
		pr_info("\n");
	}
	if (buf[0] == 'd') {
		int ii, jj;
		unsigned long block_idx;
		int ret;

		ret = kstrtoul(buf + 1, 16, &block_idx);
		if (block_idx < EDID_MAX_BLOCK) {
			for (ii = 0; ii < 8; ii++) {
				for (jj = 0; jj < 16; jj++) {
					index = block_idx * 128 + ii * 16 + jj;
					tmp = hdmitx_device.EDID_buf1[index];
					pr_info(EDID "%02x ", tmp);
				}
				pr_info("\n");
			}
		pr_info("\n");
	}
	}
	if (buf[0] == 'e') {
		int ii, jj;
		unsigned long block_idx;
		int ret;

		ret = kstrtoul(buf + 1, 16, &block_idx);
		if (block_idx < EDID_MAX_BLOCK) {
			for (ii = 0; ii < 8; ii++) {
				for (jj = 0; jj < 16; jj++) {
					index = block_idx * 128 + ii * 16 + jj;
					tmp = hdmitx_device.EDID_buf1[index];
					pr_info(EDID "%02x ", tmp);
				}
				pr_info("\n");
			}
			pr_info("\n");
		}
	}

	if (!strncmp(argv[0], "save", strlen("save"))) {
		unsigned int type = 0;

		if (argn != 3) {
			pr_info("[%s] cmd format: save bin/txt edid_file_path\n",
				__func__);
			goto PROCESS_END;
		}
		if (!strncmp(argv[1], "bin", strlen("bin")))
			type = 1;
		else if (!strncmp(argv[1], "txt", strlen("txt")))
			type = 2;

		if (type == 1 || type == 2) {
			/* clean '\n' from file path*/
			path_length = strlen(argv[2]);
			if (argv[2][path_length - 1] == '\n')
				argv[2][path_length - 1] = 0x0;

			dump_edid_data(type, argv[2]);
		}
	} else if (!strncmp(argv[0], "load", strlen("load"))) {
		if (argn != 2) {
			pr_info("[%s] cmd format: load edid_file_path\n",
				__func__);
			goto PROCESS_END;
		}

		/* clean '\n' from file path*/
		path_length = strlen(argv[1]);
		if (argv[1][path_length - 1] == '\n')
			argv[1][path_length - 1] = 0x0;
		load_edid_data(0, argv[1]);
	}

PROCESS_END:
	kfree(p);
	return 16;
}

/* rawedid attr */
static ssize_t rawedid_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	int pos = 0;
	int i;
	struct hdmitx_dev *hdev = &hdmitx_device;
	int num;

	/* prevent null prt */
	if (!hdev->edid_ptr)
		hdev->edid_ptr = hdev->EDID_buf;

	if (hdev->edid_ptr[0x7e] < 4)
		num = (hdev->edid_ptr[0x7e] + 1) * 0x80;
	else
		num = 0x100;

	for (i = 0; i < num; i++)
		pos += snprintf(buf + pos, PAGE_SIZE, "%02x",
				hdev->edid_ptr[i]);

	pos += snprintf(buf + pos, PAGE_SIZE, "\n");

	return pos;
}

/*
 * edid_parsing attr
 * If RX edid data are all correct, HEAD(00 ff ff ff ff ff ff 00), checksum,
 * version, etc), then return "ok". Otherwise, "ng"
 * Actually, in some old televisions, EDID is stored in EEPROM.
 * some bits in EEPROM may reverse with time.
 * But it does not affect  edid_parsing.
 * Therefore, we consider the RX edid data are all correct, return "OK"
 */
static ssize_t edid_parsing_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "ok\n");
	return pos;
}

/*
 * sink_type attr
 * sink, or repeater
 */
static ssize_t sink_type_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = &hdmitx_device;

	if (!hdev->hpd_state) {
		pos += snprintf(buf + pos, PAGE_SIZE, "none\n");
		return pos;
	}

	if (hdev->hdmi_info.vsdb_phy_addr.b)
		pos += snprintf(buf + pos, PAGE_SIZE, "repeater\n");
	else
		pos += snprintf(buf + pos, PAGE_SIZE, "sink\n");

	return pos;
}

/*
 * hdcp_repeater attr
 * For hdcp 22, hdcp_tx22 will write to hdcp_repeater_store
 * For hdcp 14, directly get bcaps bit
 */
static ssize_t hdcp_repeater_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = &hdmitx_device;

	if (hdev->hdcp_mode == 1)
		hdev->hdcp_bcaps_repeater = hdev->hwop.cntlddc(hdev,
			DDC_HDCP14_GET_BCAPS_RP, 0);

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
			hdev->hdcp_bcaps_repeater);

	return pos;
}

static ssize_t hdcp_repeater_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	if (hdev->hdcp_mode == 2)
		hdev->hdcp_bcaps_repeater = (buf[0] == '1');

	return count;
}

/*
 * hdcp_topo_info attr
 * For hdcp 22, hdcp_tx22 will write to hdcp_topo_info_store
 * For hdcp 14, directly get from HW
 */

static ssize_t hdcp_topo_info_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = &hdmitx_device;
	struct hdcprp_topo *topoinfo = hdev->topo_info;

	if (!hdev->hdcp_mode) {
		pos += snprintf(buf + pos, PAGE_SIZE, "hdcp mode: 0\n");
		return pos;
	}
	if (!topoinfo)
		return pos;

	if (hdev->hdcp_mode == 1) {
		memset(topoinfo, 0, sizeof(struct hdcprp_topo));
		hdev->hwop.cntlddc(hdev, DDC_HDCP14_GET_TOPO_INFO,
			(unsigned long)&topoinfo->topo.topo14);
	}

	pos += snprintf(buf + pos, PAGE_SIZE, "hdcp mode: %s\n",
		hdev->hdcp_mode == 1 ? "14" : "22");
	if (hdev->hdcp_mode == 2) {
		topoinfo->hdcp_ver = HDCPVER_22;
		pos += snprintf(buf + pos, PAGE_SIZE, "max_devs_exceeded: %d\n",
			topoinfo->topo.topo22.max_devs_exceeded);
		pos += snprintf(buf + pos, PAGE_SIZE,
			"max_cascade_exceeded: %d\n",
			topoinfo->topo.topo22.max_cascade_exceeded);
		pos += snprintf(buf + pos, PAGE_SIZE,
				"v2_0_repeater_down: %d\n",
			topoinfo->topo.topo22.v2_0_repeater_down);
		pos += snprintf(buf + pos, PAGE_SIZE, "v1_X_device_down: %d\n",
			topoinfo->topo.topo22.v1_X_device_down);
		pos += snprintf(buf + pos, PAGE_SIZE, "device_count: %d\n",
			topoinfo->topo.topo22.device_count);
		pos += snprintf(buf + pos, PAGE_SIZE, "depth: %d\n",
			topoinfo->topo.topo22.depth);
		return pos;
	}
	if (hdev->hdcp_mode == 1) {
		topoinfo->hdcp_ver = HDCPVER_14;
		pos += snprintf(buf + pos, PAGE_SIZE, "max_devs_exceeded: %d\n",
			topoinfo->topo.topo14.max_devs_exceeded);
		pos += snprintf(buf + pos, PAGE_SIZE,
			"max_cascade_exceeded: %d\n",
			topoinfo->topo.topo14.max_cascade_exceeded);
		pos += snprintf(buf + pos, PAGE_SIZE, "device_count: %d\n",
			topoinfo->topo.topo14.device_count);
		pos += snprintf(buf + pos, PAGE_SIZE, "depth: %d\n",
			topoinfo->topo.topo14.depth);
		return pos;
	}

	return pos;
}

static ssize_t hdcp_topo_info_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = &hdmitx_device;
	struct hdcprp_topo *topoinfo = hdev->topo_info;
	int cnt;

	if (!topoinfo)
		return count;

	if (hdev->hdcp_mode == 2) {
		memset(topoinfo, 0, sizeof(struct hdcprp_topo));
		cnt = sscanf(buf, "%x %x %x %x %x %x",
			     (int *)&topoinfo->topo.topo22.max_devs_exceeded,
			     (int *)&topoinfo->topo.topo22.max_cascade_exceeded,
			     (int *)&topoinfo->topo.topo22.v2_0_repeater_down,
			     (int *)&topoinfo->topo.topo22.v1_X_device_down,
			     (int *)&topoinfo->topo.topo22.device_count,
			     (int *)&topoinfo->topo.topo22.depth);
		if (cnt < 0)
			return count;
	}

	return count;
}

static ssize_t hdcp22_type_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	int pos = 0;

	pos +=
	snprintf(buf + pos, PAGE_SIZE, "%d\n", hdmitx_device.hdcp22_type);

	return pos;
}

static ssize_t hdcp22_type_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int type = 0;
	struct hdmitx_dev *hdev = &hdmitx_device;

	if (buf[0] == '1')
		type = 1;
	else
		type = 0;
	hdev->hdcp22_type = type;

	pr_info("hdmitx: set hdcp22 content type %d\n", type);
	hdev->hwop.cntlddc(hdev, DDC_HDCP_SET_TOPO_INFO, type);

	return count;
}

static ssize_t hdcp22_base_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "0x%x\n", get_hdcp22_base());

	return pos;
}

void hdmitx_audio_mute_op(unsigned int flag)
{
	hdmitx_device.tx_aud_cfg = flag;
	if (flag == 0)
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_AUDIO_MUTE_OP, AUDIO_MUTE);
	else
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_AUDIO_MUTE_OP, AUDIO_UNMUTE);
}
EXPORT_SYMBOL(hdmitx_audio_mute_op);

void hdmitx_video_mute_op(unsigned int flag)
{
	if (flag == 0)
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_VIDEO_MUTE_OP, VIDEO_MUTE);
	else
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_VIDEO_MUTE_OP, VIDEO_UNMUTE);
}
EXPORT_SYMBOL(hdmitx_video_mute_op);

/*
 *  SDR/HDR uevent
 *  1: SDR to HDR
 *  0: HDR to SDR
 */
static void hdmitx_sdr_hdr_uevent(struct hdmitx_dev *hdev)
{
	if (hdev->hdmi_last_hdr_mode == 0 &&
	    hdev->hdmi_current_hdr_mode != 0) {
		/* SDR -> HDR*/
		hdev->hdmi_last_hdr_mode = hdev->hdmi_current_hdr_mode;
		hdmitx_status.hdr = 1;
		extcon_set_state_sync(hdmitx_extcon_hdr, EXTCON_DISP_HDMI, 1);
	} else if ((hdev->hdmi_last_hdr_mode != 0) &&
			(hdev->hdmi_current_hdr_mode == 0)) {
		/* HDR -> SDR*/
		hdev->hdmi_last_hdr_mode = hdev->hdmi_current_hdr_mode;
		hdmitx_status.hdr = 0;
		extcon_set_state_sync(hdmitx_extcon_hdr, EXTCON_DISP_HDMI, 0);
	}
}

static void hdr_work_func(struct work_struct *work)
{
	struct hdmitx_dev *hdev =
		container_of(work, struct hdmitx_dev, work_hdr);

	if (hdev->hdr_transfer_feature == T_BT709 &&
	    hdev->hdr_color_feature == C_BT709) {
		unsigned char DRM_HB[3] = {0x87, 0x1, 26};
		unsigned char DRM_DB[26] = {0x0};

		pr_info("%s: send zero DRM\n", __func__);
		hdev->hwop.setpacket(HDMI_PACKET_DRM, DRM_DB, DRM_HB);
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_AVI_BT2020, hdev->colormetry);

		msleep(1500);/*delay 1.5s*/
		/* disable DRM packets completely ONLY if hdr transfer
		 * feature and color feature still demand SDR.
		 */
		if (hdr_status_pos == 4) {
			/* zero hdr10+ VSIF being sent - disable it */
			pr_info("%s: disable hdr10+ vsif\n", __func__);
			hdev->hwop.setpacket(HDMI_PACKET_VEND, NULL, NULL);
			hdr_status_pos = 0;
		}
		if (hdev->hdr_transfer_feature == T_BT709 &&
		    hdev->hdr_color_feature == C_BT709) {
			pr_info("%s: disable DRM\n", __func__);
			hdev->hwop.setpacket(HDMI_PACKET_DRM, NULL, NULL);
			hdev->hdmi_current_hdr_mode = 0;
			hdmitx_sdr_hdr_uevent(hdev);
		}
	} else {
		hdmitx_sdr_hdr_uevent(hdev);
	}
}

#define hdmi_debug() \
	do { \
		if (log_level == 0xff) \
			pr_info("%s[%d]\n", __func__, __LINE__); \
	} while (0)

/* Init DRM_DB[0] from Uboot status */
static void init_drm_db0(struct hdmitx_dev *hdev, unsigned char *dat)
{
	static int once_flag = 1;

	if (once_flag) {
		once_flag = 0;
		*dat = hdev->hwop.getstate(hdev, STAT_HDR_TYPE, 0);
	}
}

#define GET_LOW8BIT(a)	((a) & 0xff)
#define GET_HIGH8BIT(a)	(((a) >> 8) & 0xff)
struct master_display_info_s hsty_drm_config_data[8];
unsigned int hsty_drm_config_loc, hsty_drm_config_num;
struct master_display_info_s drm_config_data;
static void hdmitx_set_drm_pkt(struct master_display_info_s *data)
{
	struct hdmitx_dev *hdev = &hdmitx_device;
	unsigned char DRM_HB[3] = {0x87, 0x1, 26};
	static unsigned char DRM_DB[26] = {0x0};

	hdmi_debug();
	if (data)
		memcpy(&drm_config_data, data,
		       sizeof(struct master_display_info_s));
	else
		memset(&drm_config_data, 0,
		       sizeof(struct master_display_info_s));
	if (hsty_drm_config_loc > 7)
		hsty_drm_config_loc = 0;
	memcpy(&hsty_drm_config_data[hsty_drm_config_loc++],
	       &drm_config_data, sizeof(struct master_display_info_s));
	if (hsty_drm_config_num < 8)
		hsty_drm_config_num++;
	else
		hsty_drm_config_num = 8;

	init_drm_db0(hdev, &DRM_DB[0]);
	if (hdr_status_pos == 4) {
		/* zero hdr10+ VSIF being sent - disable it */
		pr_info("hdmitx_set_drm_pkt: disable hdr10+ zero vsif\n");
		hdev->hwop.setpacket(HDMI_PACKET_VEND, NULL, NULL);
		hdr_status_pos = 0;
	}

	/*
	 *hdr_color_feature: bit 23-16: color_primaries
	 *	1:bt709  0x9:bt2020
	 *hdr_transfer_feature: bit 15-8: transfer_characteristic
	 *	1:bt709 0xe:bt2020-10 0x10:smpte-st-2084 0x12:hlg(todo)
	 */
	if (data) {
		hdev->hdr_transfer_feature = (data->features >> 8) & 0xff;
		hdev->hdr_color_feature = (data->features >> 16) & 0xff;
		hdev->colormetry = (data->features >> 30) & 0x1;
	}

	if (hdr_status_pos != 1 && hdr_status_pos != 3)
		pr_info("%s: tf=%d, cf=%d, colormetry=%d\n",
			__func__,
			hdev->hdr_transfer_feature,
			hdev->hdr_color_feature,
			hdev->colormetry);
	hdr_status_pos = 1;

	/* hdr10+ content on a hdr10 sink case */
	if (hdev->hdr_transfer_feature == 0x30) {
		if (hdev->rxcap.hdr10plus_info.ieeeoui != 0x90848B ||
		    hdev->rxcap.hdr10plus_info.application_version != 1) {
			hdev->hdr_transfer_feature = T_SMPTE_ST_2084;
			pr_info("%s: HDR10+ not supported, treat as hdr10\n",
				__func__);
		}
	}

	if (!data || (!hdev->rxcap.hdr_sup_eotf_smpte_st_2084 &&
		      !hdev->rxcap.hdr_sup_eotf_hdr &&
		      !hdev->rxcap.hdr_sup_eotf_sdr &&
		      !hdev->rxcap.hdr_sup_eotf_hlg)) {
		DRM_HB[1] = 0;
		DRM_HB[2] = 0;
		DRM_DB[0] = 0;
		hdmitx_device.hwop.setpacket(HDMI_PACKET_DRM, NULL, NULL);
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_AVI_BT2020, hdev->colormetry);
		return;
	}

	/*SDR*/
	if (hdev->hdr_transfer_feature == T_BT709 &&
		hdev->hdr_color_feature == C_BT709) {
		/* send zero drm only for HDR->SDR transition */
		if (DRM_DB[0] == 0x02 || DRM_DB[0] == 0x03) {
			pr_info("%s: HDR->SDR, DRM_DB[0]=%d\n",
				__func__, DRM_DB[0]);
			schedule_work(&hdev->work_hdr);
			DRM_DB[0] = 0;
		}
		return;
	}

	DRM_DB[1] = 0x0;
	DRM_DB[2] = GET_LOW8BIT(data->primaries[0][0]);
	DRM_DB[3] = GET_HIGH8BIT(data->primaries[0][0]);
	DRM_DB[4] = GET_LOW8BIT(data->primaries[0][1]);
	DRM_DB[5] = GET_HIGH8BIT(data->primaries[0][1]);
	DRM_DB[6] = GET_LOW8BIT(data->primaries[1][0]);
	DRM_DB[7] = GET_HIGH8BIT(data->primaries[1][0]);
	DRM_DB[8] = GET_LOW8BIT(data->primaries[1][1]);
	DRM_DB[9] = GET_HIGH8BIT(data->primaries[1][1]);
	DRM_DB[10] = GET_LOW8BIT(data->primaries[2][0]);
	DRM_DB[11] = GET_HIGH8BIT(data->primaries[2][0]);
	DRM_DB[12] = GET_LOW8BIT(data->primaries[2][1]);
	DRM_DB[13] = GET_HIGH8BIT(data->primaries[2][1]);
	DRM_DB[14] = GET_LOW8BIT(data->white_point[0]);
	DRM_DB[15] = GET_HIGH8BIT(data->white_point[0]);
	DRM_DB[16] = GET_LOW8BIT(data->white_point[1]);
	DRM_DB[17] = GET_HIGH8BIT(data->white_point[1]);
	DRM_DB[18] = GET_LOW8BIT(data->luminance[0]);
	DRM_DB[19] = GET_HIGH8BIT(data->luminance[0]);
	DRM_DB[20] = GET_LOW8BIT(data->luminance[1]);
	DRM_DB[21] = GET_HIGH8BIT(data->luminance[1]);
	DRM_DB[22] = GET_LOW8BIT(data->max_content);
	DRM_DB[23] = GET_HIGH8BIT(data->max_content);
	DRM_DB[24] = GET_LOW8BIT(data->max_frame_average);
	DRM_DB[25] = GET_HIGH8BIT(data->max_frame_average);

	/* bt2020 + gamma transfer */
	if (hdev->hdr_transfer_feature == T_BT709 &&
	    hdev->hdr_color_feature == C_BT2020) {
		if (hdev->sdr_hdr_feature == 0) {
			hdmitx_device.hwop.setpacket(HDMI_PACKET_DRM,
				NULL, NULL);
			hdmitx_device.hwop.cntlconfig(&hdmitx_device,
				CONF_AVI_BT2020, SET_AVI_BT2020);
		} else if (hdev->sdr_hdr_feature == 1) {
			memset(DRM_DB, 0, sizeof(DRM_DB));
			hdev->hwop.setpacket(HDMI_PACKET_DRM,
				DRM_DB, DRM_HB);
			hdev->hwop.cntlconfig(&hdmitx_device,
				CONF_AVI_BT2020, SET_AVI_BT2020);
		} else {
			DRM_DB[0] = 0x02; /* SMPTE ST 2084 */
			hdmitx_device.hwop.setpacket(HDMI_PACKET_DRM,
				DRM_DB, DRM_HB);
			hdmitx_device.hwop.cntlconfig(&hdmitx_device,
				CONF_AVI_BT2020, SET_AVI_BT2020);
		}
		return;
	}

	/*must clear hdr mode*/
	hdev->hdmi_current_hdr_mode = 0;

	/* SMPTE ST 2084 and (BT2020 or NON_STANDARD) */
	if (hdev->rxcap.hdr_sup_eotf_smpte_st_2084) {
		if (hdev->hdr_transfer_feature == T_SMPTE_ST_2084 &&
		    hdev->hdr_color_feature == C_BT2020)
			hdev->hdmi_current_hdr_mode = 1;
		else if (hdev->hdr_transfer_feature == T_SMPTE_ST_2084 &&
			 hdev->hdr_color_feature != C_BT2020)
			hdev->hdmi_current_hdr_mode = 2;
	}

	/*HLG and BT2020*/
	if (hdev->rxcap.hdr_sup_eotf_hlg) {
		if (hdev->hdr_color_feature == C_BT2020 &&
		    (hdev->hdr_transfer_feature == T_BT2020_10 ||
		     hdev->hdr_transfer_feature == T_HLG))
			hdev->hdmi_current_hdr_mode = 3;
	}

	switch (hdev->hdmi_current_hdr_mode) {
	case 1:
		/*standard HDR*/
		DRM_DB[0] = 0x02; /* SMPTE ST 2084 */
		hdmitx_device.hwop.setpacket(HDMI_PACKET_DRM,
			DRM_DB, DRM_HB);
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_AVI_BT2020, SET_AVI_BT2020);
		break;
	case 2:
		/*non standard*/
		DRM_DB[0] = 0x02; /* no standard SMPTE ST 2084 */
		hdmitx_device.hwop.setpacket(HDMI_PACKET_DRM,
			DRM_DB, DRM_HB);
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_AVI_BT2020, CLR_AVI_BT2020);
		break;
	case 3:
		/*HLG*/
		DRM_DB[0] = 0x03;/* HLG is 0x03 */
		hdmitx_device.hwop.setpacket(HDMI_PACKET_DRM,
			DRM_DB, DRM_HB);
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_AVI_BT2020, SET_AVI_BT2020);
		break;
	case 0:
	default:
		/*other case*/
		hdmitx_device.hwop.setpacket(HDMI_PACKET_DRM, NULL, NULL);
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_AVI_BT2020, CLR_AVI_BT2020);
		break;
	}

	/* if sdr/hdr mode change ,notify uevent to userspace*/
	if (hdev->hdmi_current_hdr_mode != hdev->hdmi_last_hdr_mode)
		schedule_work(&hdev->work_hdr);
}

static void update_current_para(struct hdmitx_dev *hdev)
{
	struct vinfo_s *info = NULL;
	unsigned char mode[32];

	info = hdmitx_get_current_vinfo();
	if (!info)
		return;

	memset(mode, 0, sizeof(mode));
	strncpy(mode, info->name, sizeof(mode) - 1);
	if (strstr(hdev->fmt_attr, "420")) {
		if (!strstr(mode, "420"))
			strncat(mode, "420", sizeof(mode) - strlen("420") - 1);
	}
	hdev->para = hdmi_get_fmt_name(mode, hdev->fmt_attr);
}

struct vsif_debug_save vsif_debug_info;
struct vsif_debug_save hsty_vsif_config_data[8];
unsigned int hsty_vsif_config_loc, hsty_vsif_config_num;
static void hdmitx_set_vsif_pkt(enum eotf_type type,
				enum mode_type tunnel_mode,
				struct dv_vsif_para *data,
				bool signal_sdr)
{
	struct hdmitx_dev *hdev = &hdmitx_device;
	struct dv_vsif_para para = {0};
	unsigned char VEN_HB[3] = {0x81, 0x01};
	unsigned char VEN_DB1[24] = {0x00};
	unsigned char VEN_DB2[27] = {0x00};
	unsigned char len = 0;
	unsigned int vic = hdev->cur_VIC;
	unsigned int hdmi_vic_4k_flag = 0;
	static enum eotf_type ltype = EOTF_T_NULL;
	static u8 ltmode = -1;

	hdmi_debug();
	if (!data)
		memcpy(&vsif_debug_info.data, &para,
		       sizeof(struct dv_vsif_para));
	else
		memcpy(&vsif_debug_info.data, data,
		       sizeof(struct dv_vsif_para));
	vsif_debug_info.type = type;
	vsif_debug_info.tunnel_mode = tunnel_mode;
	vsif_debug_info.signal_sdr = signal_sdr;

	if (hsty_vsif_config_loc > 7)
		hsty_vsif_config_loc = 0;
	memcpy(&hsty_vsif_config_data[hsty_vsif_config_loc++],
	       &vsif_debug_info, sizeof(struct vsif_debug_save));
	if (hsty_vsif_config_num < 8)
		hsty_vsif_config_num++;
	else
		hsty_vsif_config_num = 8;

	if (hdev->ready == 0 || hdev->rxcap.dv_info.ieeeoui
		!= DV_IEEE_OUI) {
		ltype = EOTF_T_NULL;
		ltmode = -1;
		return;
	}

	if (hdev->data->chip_type < MESON_CPU_ID_GXL) {
		pr_info("hdmitx: not support DolbyVision\n");
		return;
	}

	if (hdr_status_pos != 2)
		pr_info("%s: type = %d\n", __func__, type);
	hdr_status_pos = 2;
	hdev->hdmi_current_eotf_type = type;
	hdev->hdmi_current_tunnel_mode = tunnel_mode;
	/*ver0 and ver1_15 and ver1_12bit with ll= 0 use hdmi 1.4b VSIF*/
	if (hdev->rxcap.dv_info.ver == 0 ||
	    (hdev->rxcap.dv_info.ver == 1 &&
	    hdev->rxcap.dv_info.length == 0xE) ||
	    (hdev->rxcap.dv_info.ver == 1 &&
	    hdev->rxcap.dv_info.length == 0xB &&
	    hdev->rxcap.dv_info.low_latency == 0)) {
		if (vic == HDMI_3840x2160p30_16x9 ||
		    vic == HDMI_3840x2160p25_16x9 ||
		    vic == HDMI_3840x2160p24_16x9 ||
		    vic == HDMI_4096x2160p24_256x135)
			hdmi_vic_4k_flag = 1;

		switch (type) {
		case EOTF_T_DOLBYVISION:
			len = 0x18;
			hdev->dv_src_feature = 1;
			break;
		case EOTF_T_HDR10:
		case EOTF_T_SDR:
		case EOTF_T_NULL:
		default:
			len = 0x05;
			hdev->dv_src_feature = 0;
			break;
		}

		VEN_HB[2] = len;
		VEN_DB1[0] = 0x03;
		VEN_DB1[1] = 0x0c;
		VEN_DB1[2] = 0x00;
		VEN_DB1[3] = 0x00;

		if (hdmi_vic_4k_flag) {
			VEN_DB1[3] = 0x20;
			if (vic == HDMI_3840x2160p30_16x9)
				VEN_DB1[4] = 0x1;
			else if (vic == HDMI_3840x2160p25_16x9)
				VEN_DB1[4] = 0x2;
			else if (vic == HDMI_3840x2160p24_16x9)
				VEN_DB1[4] = 0x3;
			else/*vic == HDMI_4096x2160p24_256x135*/
				VEN_DB1[4] = 0x4;
		}
		if (type == EOTF_T_DOLBYVISION) {
			/*first disable drm package*/
			hdmitx_device.hwop.setpacket(HDMI_PACKET_DRM,
				NULL, NULL);
			hdev->hwop.setpacket(HDMI_PACKET_VEND, VEN_DB1, VEN_HB);
			hdev->hwop.cntlconfig(hdev, CONF_AVI_BT2020,
				CLR_AVI_BT2020);/*BT709*/
			if (tunnel_mode == RGB_8BIT) {
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_RGBYCC_INDIC,
					COLORSPACE_RGB444);
				hdev->hwop.cntlconfig(hdev, CONF_AVI_Q01,
					RGB_RANGE_FUL);
			} else {
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_RGBYCC_INDIC,
					COLORSPACE_YUV422);
				hdev->hwop.cntlconfig(hdev, CONF_AVI_YQ01,
					YCC_RANGE_FUL);
			}
		} else {
			if (hdmi_vic_4k_flag)
				hdev->hwop.setpacket(HDMI_PACKET_VEND,
						     VEN_DB1, VEN_HB);
			else
				hdev->hwop.setpacket(HDMI_PACKET_VEND,
						     NULL, NULL);
			if (signal_sdr) {
				pr_info("hdmitx: H14b VSIF, switching signal to SDR\n");
				update_current_para(hdev);
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_RGBYCC_INDIC, hdev->para->cs);
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_Q01, RGB_RANGE_LIM);
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_YQ01, YCC_RANGE_LIM);
				hdev->hwop.cntlconfig(hdev, CONF_AVI_BT2020,
					CLR_AVI_BT2020);/*BT709*/
			}
		}
	}
	/*ver1_12  with low_latency = 1 and ver2 use Dolby VSIF*/
	if (hdev->rxcap.dv_info.ver == 2 ||
	    (hdev->rxcap.dv_info.ver == 1 &&
	     hdev->rxcap.dv_info.length == 0xB &&
	     hdev->rxcap.dv_info.low_latency == 1) ||
	     type == EOTF_T_LL_MODE) {
		if (!data)
			data = &para;
		len = 0x1b;

		switch (type) {
		case EOTF_T_DOLBYVISION:
		case EOTF_T_LL_MODE:
			hdev->dv_src_feature = 1;
			break;
		case EOTF_T_HDR10:
		case EOTF_T_SDR:
		case EOTF_T_NULL:
		default:
			hdev->dv_src_feature = 0;
			break;
		}
		VEN_HB[2] = len;
		VEN_DB2[0] = 0x46;
		VEN_DB2[1] = 0xd0;
		VEN_DB2[2] = 0x00;
		VEN_DB2[3] = (data->vers.ver2.low_latency) |
			(data->vers.ver2.dobly_vision_signal << 1);
		VEN_DB2[4] = (data->vers.ver2.eff_tmax_PQ_hi)
			| (data->vers.ver2.auxiliary_MD_present << 6)
			| (data->vers.ver2.backlt_ctrl_MD_present << 7);
		VEN_DB2[5] = data->vers.ver2.eff_tmax_PQ_low;
		VEN_DB2[6] = data->vers.ver2.auxiliary_runmode;
		VEN_DB2[7] = data->vers.ver2.auxiliary_runversion;
		VEN_DB2[8] = data->vers.ver2.auxiliary_debug0;

		/*Dolby Vision standard case*/
		if (type == EOTF_T_DOLBYVISION) {
			/*first disable drm package*/
			hdmitx_device.hwop.setpacket(HDMI_PACKET_DRM,
				NULL, NULL);
			hdev->hwop.setpacket(HDMI_PACKET_VEND, VEN_DB2, VEN_HB);
			hdev->hwop.cntlconfig(hdev, CONF_AVI_BT2020,
				CLR_AVI_BT2020);/*BT709*/
			if (tunnel_mode == RGB_8BIT) {/*RGB444*/
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_RGBYCC_INDIC,
					COLORSPACE_RGB444);
				hdev->hwop.cntlconfig(hdev, CONF_AVI_Q01,
					RGB_RANGE_FUL);
			} else {/*YUV422*/
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_RGBYCC_INDIC,
					COLORSPACE_YUV422);
				hdev->hwop.cntlconfig(hdev, CONF_AVI_YQ01,
					YCC_RANGE_FUL);
			}
		}
		/*Dolby Vision low-latency case*/
		else if  (type == EOTF_T_LL_MODE) {
			/*first disable drm package*/
			hdmitx_device.hwop.setpacket(HDMI_PACKET_DRM,
				NULL, NULL);
			hdev->hwop.setpacket(HDMI_PACKET_VEND, VEN_DB2, VEN_HB);
			if (hdev->rxcap.colorimetry_data & 0xe0)
				/*if RX support BT2020, then output BT2020*/
				hdev->hwop.cntlconfig(hdev, CONF_AVI_BT2020,
					SET_AVI_BT2020);/*BT2020*/
			else
				hdev->hwop.cntlconfig(hdev, CONF_AVI_BT2020,
					CLR_AVI_BT2020);/*BT709*/
			if (tunnel_mode == RGB_10_12BIT) {/*10/12bit RGB444*/
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_RGBYCC_INDIC,
					COLORSPACE_RGB444);
				hdev->hwop.cntlconfig(hdev, CONF_AVI_Q01,
					RGB_RANGE_LIM);
			} else if (tunnel_mode == YUV444_10_12BIT) {
				/*10/12bit YUV444*/
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_RGBYCC_INDIC,
					COLORSPACE_YUV444);
				hdev->hwop.cntlconfig(hdev, CONF_AVI_YQ01,
					YCC_RANGE_LIM);
			} else {/*YUV422*/
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_RGBYCC_INDIC,
					COLORSPACE_YUV422);
				hdev->hwop.cntlconfig(hdev, CONF_AVI_YQ01,
					YCC_RANGE_LIM);
			}
		} else { /*SDR case*/
			pr_info("hdmitx: Dolby VSIF, VEN_DB2[3]) = %d\n",
				VEN_DB2[3]);
			hdev->hwop.setpacket(HDMI_PACKET_VEND, VEN_DB2, VEN_HB);
			if (signal_sdr) {
				pr_info("hdmitx: Dolby VSIF, switching signal to SDR\n");
				update_current_para(hdev);
				pr_info("vic:%d, cd:%d, cs:%d, cr:%d\n",
					hdev->para->vic, hdev->para->cd,
					hdev->para->cs, hdev->para->cr);
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_RGBYCC_INDIC, hdev->para->cs);
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_Q01, RGB_RANGE_DEFAULT);
				hdev->hwop.cntlconfig(hdev,
					CONF_AVI_YQ01, YCC_RANGE_LIM);
				hdev->hwop.cntlconfig(hdev, CONF_AVI_BT2020,
					CLR_AVI_BT2020);/*BT709*/
			}
		}
	}
}

struct hdr10plus_para hdr10p_config_data;
struct hdr10plus_para hsty_hdr10p_config_data[8];
unsigned int hsty_hdr10p_config_loc, hsty_hdr10p_config_num;
static void hdmitx_set_hdr10plus_pkt(unsigned int flag,
	struct hdr10plus_para *data)
{
	struct hdmitx_dev *hdev = &hdmitx_device;
	unsigned char VEN_HB[3] = {0x81, 0x01, 0x1b};
	unsigned char VEN_DB[27] = {0x00};

	hdmi_debug();

	if (data)
		memcpy(&hdr10p_config_data, data,
		       sizeof(struct hdr10plus_para));
	else
		memset(&hdr10p_config_data, 0,
		       sizeof(struct hdr10plus_para));
	if (hsty_hdr10p_config_loc > 7)
		hsty_hdr10p_config_loc = 0;
	memcpy(&hsty_hdr10p_config_data[hsty_hdr10p_config_loc++],
	       &hdr10p_config_data, sizeof(struct hdr10plus_para));
	if (hsty_hdr10p_config_num < 8)
		hsty_hdr10p_config_num++;
	else
		hsty_hdr10p_config_num = 8;

	if (flag == HDR10_PLUS_ZERO_VSIF) {
		/* needed during hdr10+ to sdr transition */
		pr_info("hdmitx_set_hdr10plus_pkt: zero vsif\n");
		hdev->hwop.setpacket(HDMI_PACKET_VEND, VEN_DB, VEN_HB);
		hdev->hwop.cntlconfig(hdev, CONF_AVI_BT2020,
			CLR_AVI_BT2020);
		hdev->hdr10plus_feature = 0;
		hdr_status_pos = 4;
		return;
	}

	if (!data || !flag) {
		pr_info("hdmitx_set_hdr10plus_pkt: null vsif\n");
		hdev->hwop.setpacket(HDMI_PACKET_VEND, NULL, NULL);
		hdev->hwop.cntlconfig(hdev, CONF_AVI_BT2020,
			CLR_AVI_BT2020);
		hdev->hdr10plus_feature = 0;
		return;
	}

	if (hdev->hdr10plus_feature != 1)
		pr_info("hdmitx_set_hdr10plus_pkt: flag = %d\n", flag);
	hdev->hdr10plus_feature = 1;
	hdr_status_pos = 3;
	VEN_DB[0] = 0x8b;
	VEN_DB[1] = 0x84;
	VEN_DB[2] = 0x90;

	VEN_DB[3] = ((data->application_version & 0x3) << 6) |
		 ((data->targeted_max_lum & 0x1f) << 1);
	VEN_DB[4] = data->average_maxrgb;
	VEN_DB[5] = data->distribution_values[0];
	VEN_DB[6] = data->distribution_values[1];
	VEN_DB[7] = data->distribution_values[2];
	VEN_DB[8] = data->distribution_values[3];
	VEN_DB[9] = data->distribution_values[4];
	VEN_DB[10] = data->distribution_values[5];
	VEN_DB[11] = data->distribution_values[6];
	VEN_DB[12] = data->distribution_values[7];
	VEN_DB[13] = data->distribution_values[8];
	VEN_DB[14] = ((data->num_bezier_curve_anchors & 0xf) << 4) |
		((data->knee_point_x >> 6) & 0xf);
	VEN_DB[15] = ((data->knee_point_x & 0x3f) << 2) |
		((data->knee_point_y >> 8) & 0x3);
	VEN_DB[16] = data->knee_point_y  & 0xff;
	VEN_DB[17] = data->bezier_curve_anchors[0];
	VEN_DB[18] = data->bezier_curve_anchors[1];
	VEN_DB[19] = data->bezier_curve_anchors[2];
	VEN_DB[20] = data->bezier_curve_anchors[3];
	VEN_DB[21] = data->bezier_curve_anchors[4];
	VEN_DB[22] = data->bezier_curve_anchors[5];
	VEN_DB[23] = data->bezier_curve_anchors[6];
	VEN_DB[24] = data->bezier_curve_anchors[7];
	VEN_DB[25] = data->bezier_curve_anchors[8];
	VEN_DB[26] = ((data->graphics_overlay_flag & 0x1) << 7) |
		((data->no_delay_flag & 0x1) << 6);

	hdev->hwop.setpacket(HDMI_PACKET_VEND, VEN_DB, VEN_HB);
	hdev->hwop.cntlconfig(hdev, CONF_AVI_BT2020,
			SET_AVI_BT2020);
}

#define  EMP_FIRST 0x80
#define  EMP_LAST 0x40
struct emp_debug_save emp_config_data;
static void hdmitx_set_emp_pkt(unsigned char *data, unsigned int type,
			       unsigned int size)
{
	unsigned int number;
	unsigned int remainder;
	unsigned char *virt_ptr;
	unsigned char *virt_ptr_align32bit;
	unsigned long phys_ptr;
	unsigned int i;
	struct hdmitx_dev *hdev = &hdmitx_device;
	unsigned int ds_type = 0;
	unsigned char AFR = 0;
	unsigned char VFR = 0;
	unsigned char sync = 0;
	unsigned char  new = 0;
	unsigned char  end = 0;
	unsigned int organzation_id = 0;
	unsigned int data_set_tag = 0;
	unsigned int data_set_length = 0;

	hdmi_debug();

	if (!data) {
		pr_info("the data is null\n");
		return;
	}

	emp_config_data.type = type;
	emp_config_data.size = size;
	if (size <= 128)
		memcpy(emp_config_data.data, data, size);
	else
		memcpy(emp_config_data.data, data, 128);

	if (hdmitx_device.data->chip_type < MESON_CPU_ID_G12A) {
		pr_info("this chip doesn't support emp function\n");
		return;
	}
	if (size <= 21) {
		number = 1;
		remainder = size;
	} else {
		number = ((size - 21) / 28) + 2;
		remainder = (size - 21) % 28;
	}

	virt_ptr = kzalloc(sizeof(unsigned char) * (number + 0x1f),
			   GFP_KERNEL);
	if (!virt_ptr)
		return;
	pr_info("emp_pkt virt_ptr: %p\n", virt_ptr);
	virt_ptr_align32bit = (unsigned char *)
		((((unsigned long)virt_ptr) + 0x1f) & (~0x1f));
	pr_info("emp_pkt virt_ptr_align32bit: %p\n", virt_ptr_align32bit);

	memset(virt_ptr_align32bit, 0, sizeof(unsigned char) * (number + 0x1f));

	switch (type) {
	case VENDOR_SPECIFIC_EM_DATA:
		break;
	case COMPRESS_VIDEO_TRAMSPORT:
		break;
	case HDR_DYNAMIC_METADATA:
			ds_type = 1;
			sync = 1;
			VFR = 1;
			AFR = 0;
			new = 0x1; /*todo*/
			end = 0x1; /*todo*/
			organzation_id = 2;
		break;
	case VIDEO_TIMING_EXTENDED:
		break;
	default:
		break;
	}

	for (i = 0; i < number; i++) {
		/*HB[0]-[2]*/
		virt_ptr_align32bit[i * 32 + 0] = 0x7F;
		if (i == 0)
			virt_ptr_align32bit[i * 32 + 1] |=  EMP_FIRST;
		if (i == number)
			virt_ptr_align32bit[i * 32 + 1] |= EMP_LAST;
		virt_ptr_align32bit[i * 32 + 2] = number;
		/*PB[0]-[6]*/
		if (i == 0) {
			virt_ptr_align32bit[3] = (new << 7) | (end << 6) |
				(ds_type << 4) | (AFR << 3) |
				(VFR << 2) | (sync << 1);
			virt_ptr_align32bit[4] = 0;/*Rsvd*/
			virt_ptr_align32bit[5] = organzation_id;
			virt_ptr_align32bit[6] = (data_set_tag >> 8) & 0xFF;
			virt_ptr_align32bit[7] = data_set_tag & 0xFF;
			virt_ptr_align32bit[8] = (data_set_length >> 8)
				& 0xFF;
			virt_ptr_align32bit[9] = data_set_length & 0xFF;
		}
		if (number == 1) {
			memcpy(&virt_ptr_align32bit[10], &data[0],
			       sizeof(unsigned char) * remainder);
		} else {
			if (i == 0) {
			/*MD: first package need PB[7]-[27]*/
				memcpy(&virt_ptr_align32bit[10], &data[0],
				       sizeof(unsigned char) * 21);
			} else if (i != number) {
			/*MD: following package need PB[0]-[27]*/
				memcpy(&virt_ptr_align32bit[i * 32 + 10],
				       &data[(i - 1) * 28 + 21],
				       sizeof(unsigned char) * 28);
			} else {
			/*MD: the last package need PB[0] to end */
				memcpy(&virt_ptr_align32bit[0],
				       &data[(i - 1) * 28 + 21],
				       sizeof(unsigned char) * remainder);
			}
		}
			/*PB[28]*/
		virt_ptr_align32bit[i * 32 + 31] = 0;
	}

	phys_ptr = virt_to_phys(virt_ptr_align32bit);
	pr_info("emp_pkt phys_ptr: %lx\n", phys_ptr);

	hdev->hwop.cntlconfig(hdev, CONF_EMP_NUMBER, number);
	hdev->hwop.cntlconfig(hdev, CONF_EMP_PHY_ADDR, phys_ptr);
}

/*config attr*/
static ssize_t config_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	int pos = 0;
	unsigned char *conf;
	struct hdmitx_dev *hdev = &hdmitx_device;

	pos += snprintf(buf + pos, PAGE_SIZE, "cur_VIC: %d\n", hdev->cur_VIC);
	if (hdev->cur_video_param)
		pos += snprintf(buf + pos, PAGE_SIZE,
			"cur_video_param->VIC=%d\n",
			hdev->cur_video_param->VIC);
	if (hdev->para) {
		switch (hdev->para->cd) {
		case COLORDEPTH_24B:
			conf = "8bit";
			break;
		case COLORDEPTH_30B:
			conf = "10bit";
			break;
		case COLORDEPTH_36B:
			conf = "12bit";
			break;
		case COLORDEPTH_48B:
			conf = "16bit";
			break;
		default:
			conf = "reserved";
		}
		pos += snprintf(buf + pos, PAGE_SIZE, "colordepth: %s\n",
				conf);
		switch (hdev->para->cs) {
		case COLORSPACE_RGB444:
			conf = "RGB";
			break;
		case COLORSPACE_YUV422:
			conf = "422";
			break;
		case COLORSPACE_YUV444:
			conf = "444";
			break;
		case COLORSPACE_YUV420:
			conf = "420";
			break;
		default:
			conf = "reserved";
		}
		pos += snprintf(buf + pos, PAGE_SIZE, "colorspace: %s\n",
				conf);
	}

	switch (hdev->tx_aud_cfg) {
	case 0:
		conf = "off";
		break;
	case 1:
		conf = "on";
		break;
	case 2:
		conf = "auto";
		break;
	default:
		conf = "none";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio config: %s\n", conf);

	switch (hdev->hdmi_audio_off_flag) {
	case 0:
		conf = "on";
		break;
	case 1:
		conf = "off";
		break;
	default:
		conf = "none";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio on/off: %s\n", conf);

	switch (hdev->tx_aud_src) {
	case 0:
		conf = "SPDIF";
		break;
	case 1:
		conf = "I2S";
		break;
	default:
		conf = "none";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio source: %s\n", conf);

	switch (hdev->cur_audio_param.type) {
	case CT_REFER_TO_STREAM:
		conf = "refer to stream header";
		break;
	case CT_PCM:
		conf = "L-PCM";
		break;
	case CT_AC_3:
		conf = "AC-3";
		break;
	case CT_MPEG1:
		conf = "MPEG1";
		break;
	case CT_MP3:
		conf = "MP3";
		break;
	case CT_MPEG2:
		conf = "MPEG2";
		break;
	case CT_AAC:
		conf = "AAC";
		break;
	case CT_DTS:
		conf = "DTS";
		break;
	case CT_ATRAC:
		conf = "ATRAC";
		break;
	case CT_ONE_BIT_AUDIO:
		conf = "One Bit Audio";
		break;
	case CT_DOLBY_D:
		conf = "Dobly Digital+";
		break;
	case CT_DTS_HD:
		conf = "DTS_HD";
		break;
	case CT_MAT:
		conf = "MAT";
		break;
	case CT_DST:
		conf = "DST";
		break;
	case CT_WMA:
		conf = "WMA";
		break;
	default:
		conf = "MAX";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio type: %s\n", conf);

	switch (hdev->cur_audio_param.channel_num) {
	case CC_REFER_TO_STREAM:
		conf = "refer to stream header";
		break;
	case CC_2CH:
		conf = "2 channels";
		break;
	case CC_3CH:
		conf = "3 channels";
		break;
	case CC_4CH:
		conf = "4 channels";
		break;
	case CC_5CH:
		conf = "5 channels";
		break;
	case CC_6CH:
		conf = "6 channels";
		break;
	case CC_7CH:
		conf = "7 channels";
		break;
	case CC_8CH:
		conf = "8 channels";
		break;
	default:
		conf = "MAX";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio channel num: %s\n", conf);

	switch (hdev->cur_audio_param.sample_rate) {
	case FS_REFER_TO_STREAM:
		conf = "refer to stream header";
		break;
	case FS_32K:
		conf = "32kHz";
		break;
	case FS_44K1:
		conf = "44.1kHz";
		break;
	case FS_48K:
		conf = "48kHz";
		break;
	case FS_88K2:
		conf = "88.2kHz";
		break;
	case FS_96K:
		conf = "96kHz";
		break;
	case FS_176K4:
		conf = "176.4kHz";
		break;
	case FS_192K:
		conf = "192kHz";
		break;
	case FS_768K:
		conf = "768kHz";
		break;
	default:
		conf = "MAX";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio sample rate: %s\n", conf);

	switch (hdev->cur_audio_param.sample_size) {
	case SS_REFER_TO_STREAM:
		conf = "refer to stream header";
		break;
	case SS_16BITS:
		conf = "16bit";
		break;
	case SS_20BITS:
		conf = "20bit";
		break;
	case SS_24BITS:
		conf = "24bit";
		break;
	default:
		conf = "MAX";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio sample size: %s\n", conf);

	if (hdev->flag_3dfp)
		conf = "FramePacking";
	else if (hdev->flag_3dss)
		conf = "SidebySide";
	else if (hdev->flag_3dtb)
		conf = "TopButtom";
	else
		conf = "off";
	pos += snprintf(buf + pos, PAGE_SIZE, "3D config: %s\n", conf);
	return pos;
}

static ssize_t config_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int ret = 0;
	struct master_display_info_s data = {0};
	struct hdr10plus_para hdr_data = {0x1, 0x2, 0x3};

	pr_info("hdmitx: config: %s\n", buf);

	if (strncmp(buf, "unplug_powerdown", 16) == 0) {
		if (buf[16] == '0')
			hdmitx_device.unplug_powerdown = 0;
		else
			hdmitx_device.unplug_powerdown = 1;
	} else if (strncmp(buf, "3d", 2) == 0) {
		/* Second, set 3D parameters */
		if (strncmp(buf + 2, "tb", 2) == 0) {
			hdmitx_device.flag_3dtb = 1;
			hdmitx_device.flag_3dss = 0;
			hdmitx_device.flag_3dfp = 0;
			hdmi_set_3d(&hdmitx_device, T3D_TAB, 0);
		} else if ((strncmp(buf + 2, "lr", 2) == 0) ||
			(strncmp(buf + 2, "ss", 2) == 0)) {
			unsigned long sub_sample_mode = 0;

			hdmitx_device.flag_3dtb = 0;
			hdmitx_device.flag_3dss = 1;
			hdmitx_device.flag_3dfp = 0;
			if (buf[2])
				ret = kstrtoul(buf + 2, 10,
					       &sub_sample_mode);
			/* side by side */
			hdmi_set_3d(&hdmitx_device, T3D_SBS_HALF,
				    sub_sample_mode);
		} else if (strncmp(buf + 2, "fp", 2) == 0) {
			hdmitx_device.flag_3dtb = 0;
			hdmitx_device.flag_3dss = 0;
			hdmitx_device.flag_3dfp = 1;
			hdmi_set_3d(&hdmitx_device, T3D_FRAME_PACKING, 0);
		} else if (strncmp(buf + 2, "off", 3) == 0) {
			hdmitx_device.flag_3dfp = 0;
			hdmitx_device.flag_3dtb = 0;
			hdmitx_device.flag_3dss = 0;
			hdmi_set_3d(&hdmitx_device, T3D_DISABLE, 0);
		}
	} else if (strncmp(buf, "sdr", 3) == 0) {
		data.features = 0x00010100;
		hdmitx_set_drm_pkt(&data);
	} else if (strncmp(buf, "hdr", 3) == 0) {
		data.features = 0x00091000;
		hdmitx_set_drm_pkt(&data);
	} else if (strncmp(buf, "hlg", 3) == 0) {
		data.features = 0x00091200;
		hdmitx_set_drm_pkt(&data);
	} else if (strncmp(buf, "vsif", 4) == 0) {
		hdmitx_set_vsif_pkt(buf[4] - '0', buf[5] == '1', NULL, true);
	} else if (strncmp(buf, "emp", 3) == 0) {
		if (hdmitx_device.data->chip_type >= MESON_CPU_ID_G12A)
			hdmitx_set_emp_pkt(NULL, 1, 1);
	} else if (strncmp(buf, "hdr10+", 6) == 0) {
		hdmitx_set_hdr10plus_pkt(1, &hdr_data);
	}
	return count;
}

void hdmitx_ext_set_audio_output(int enable)
{
	hdmitx_audio_mute_op(enable);
}
EXPORT_SYMBOL_GPL(hdmitx_ext_set_audio_output);

int hdmitx_ext_get_audio_status(void)
{
	return !!hdmitx_device.tx_aud_cfg;
}
EXPORT_SYMBOL_GPL(hdmitx_ext_get_audio_status);

void hdmitx_ext_set_i2s_mask(char ch_num, char ch_msk)
{
	struct hdmitx_dev *hdev = &hdmitx_device;
	static unsigned int update_flag = -1;

	if (!(ch_num == 2 || ch_num == 4 ||
	      ch_num == 6 || ch_num == 8)) {
		pr_info("err chn setting, must be 2, 4, 6 or 8, Rst as def\n");
		hdev->aud_output_ch = 0;
		if (update_flag != hdev->aud_output_ch) {
			update_flag = hdev->aud_output_ch;
			hdev->hdmi_ch = 0;
			hdmitx_set_audio(hdev, &hdev->cur_audio_param);
		}
	}
	if (ch_msk == 0) {
		pr_info("err chn msk, must larger than 0\n");
		return;
	}
	hdev->aud_output_ch = (ch_num << 4) + ch_msk;
	if (update_flag != hdev->aud_output_ch) {
		update_flag = hdev->aud_output_ch;
		hdev->hdmi_ch = 0;
		hdmitx_set_audio(hdev, &hdev->cur_audio_param);
	}
}

char hdmitx_ext_get_i2s_mask(void)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	return hdev->aud_output_ch & 0xf;
}

static ssize_t vid_mute_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		atomic_read(&hdmitx_device.kref_video_mute));
	return pos;
}

static ssize_t vid_mute_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	atomic_t kref_video_mute = hdmitx_device.kref_video_mute;

	if (buf[0] == '1') {
		atomic_inc(&kref_video_mute);
		if (atomic_read(&kref_video_mute) == 1)
			hdmitx_video_mute_op(0);
	}
	if (buf[0] == '0') {
		if (!(atomic_sub_and_test(0, &kref_video_mute))) {
			atomic_dec(&kref_video_mute);
			if (atomic_sub_and_test(0, &kref_video_mute))
				hdmitx_video_mute_op(1);
		}
	}

	hdmitx_device.kref_video_mute = kref_video_mute;

	return count;
}

static ssize_t debug_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	hdmitx_device.hwop.debugfun(&hdmitx_device, buf);
	return count;
}

/* support format lists */
const char *disp_mode_t[] = {
	"480i60hz",
	"576i50hz",
	"480p60hz",
	"576p50hz",
	"720p60hz",
	"1080i60hz",
	"1080p60hz",
	"720p50hz",
	"1080i50hz",
	"1080p30hz",
	"1080p50hz",
	"1080p25hz",
	"1080p24hz",
	"2560x1080p50hz",
	"2560x1080p60hz",
	"2160p30hz",
	"2160p25hz",
	"2160p24hz",
	"smpte24hz",
	"smpte25hz",
	"smpte30hz",
	"smpte50hz",
	"smpte60hz",
	"2160p50hz",
	"2160p60hz",
	/* VESA modes */
	"640x480p60hz",
	"800x480p60hz",
	"800x600p60hz",
	"852x480p60hz",
	"854x480p60hz",
	"1024x600p60hz",
	"1024x768p60hz",
	"1152x864p75hz",
	"1280x600p60hz",
	"1280x768p60hz",
	"1280x800p60hz",
	"1280x960p60hz",
	"1280x1024p60hz",
	"1360x768p60hz",
	"1366x768p60hz",
	"1400x1050p60hz",
	"1440x900p60hz",
	"1440x2560p60hz",
	"1600x900p60hz",
	"1600x1200p60hz",
	"1680x1050p60hz",
	"1920x1200p60hz",
	"2160x1200p90hz",
	"2560x1080p60hz",
	"2560x1440p60hz",
	"2560x1600p60hz",
	"3440x1440p60hz",
	NULL
};

static int is_4k50_fmt(char *mode)
{
	int i;
	static char const *hdmi4k50[] = {
		"2160p50hz",
		"2160p60hz",
		"smpte50hz",
		"smpte60hz",
		NULL
	};

	for (i = 0; hdmi4k50[i]; i++) {
		if (strcmp(hdmi4k50[i], mode) == 0)
			return 1;
	}
	return 0;
}

static int is_4k_fmt(char *mode)
{
	int i;
	static char const *hdmi4k[] = {
		"2160p",
		"smpte",
		NULL
	};

	for (i = 0; hdmi4k[i]; i++) {
		if (strstr(mode, hdmi4k[i]))
			return 1;
	}
	return 0;
}

/* below items has feature limited, may need extra judgement */
static bool hdmitx_limited_1080p(void)
{
	//if (is_meson_gxl_package_805X())
	//	return 1;
	//else if (is_meson_gxl_package_805Y())
	//	return 1;
	//else
		return 0;
}

/**/
static ssize_t disp_cap_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	int i, pos = 0;
	const char *native_disp_mode =
		hdmitx_edid_get_native_VIC(&hdmitx_device);
	enum hdmi_vic vic;
	char mode_tmp[32];

	if (hdmitx_device.tv_no_edid) {
		pos += snprintf(buf + pos, PAGE_SIZE, "null edid\n");
	} else {
		for (i = 0; disp_mode_t[i]; i++) {
			memset(mode_tmp, 0, sizeof(mode_tmp));
			strncpy(mode_tmp, disp_mode_t[i], 31);
			if (hdmitx_limited_1080p() && is_4k_fmt(mode_tmp))
				continue;
			vic = hdmitx_edid_get_VIC(&hdmitx_device, mode_tmp, 0);
			/* Handling only 4k420 mode */
			if (vic == HDMI_UNKNOWN && is_4k50_fmt(mode_tmp)) {
				strcat(mode_tmp, "420");
				vic = hdmitx_edid_get_VIC(&hdmitx_device,
							  mode_tmp, 0);
			}
		if (vic != HDMI_UNKNOWN) {
			pos += snprintf(buf + pos, PAGE_SIZE, "%s",
				disp_mode_t[i]);
			if (native_disp_mode &&
			    (strcmp(native_disp_mode,
				disp_mode_t[i]) == 0)) {
				pos += snprintf(buf + pos, PAGE_SIZE,
					"*\n");
			} else {
				pos += snprintf(buf + pos, PAGE_SIZE, "\n");
			}
		}
		}
	}
	return pos;
}

static ssize_t preferred_mode_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	int pos = 0;
	struct rx_cap *prxcap = &hdmitx_device.rxcap;

	pos += snprintf(buf + pos, PAGE_SIZE, "%s\n",
		hdmitx_edid_vic_to_string(prxcap->preferred_mode));

	return pos;
}

/* cea_cap, a clone of disp_cap */
static ssize_t cea_cap_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	return disp_cap_show(dev, attr, buf);
}

static ssize_t vesa_cap_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	int i;
	struct hdmi_format_para *para = NULL;
	enum hdmi_vic *vesa_t = &hdmitx_device.rxcap.vesa_timing[0];
	int pos = 0;

	for (i = 0; vesa_t[i] && i < VESA_MAX_TIMING; i++) {
		para = hdmi_get_fmt_paras(vesa_t[i]);
		if (para && para->vic >= HDMITX_VESA_OFFSET)
			pos += snprintf(buf + pos, PAGE_SIZE, "%s\n",
					para->name);
	}
	return pos;
}

/**/
static int local_support_3dfp(enum hdmi_vic vic)
{
	switch (vic) {
	case HDMI_1280x720p50_16x9:
	case HDMI_1280x720p60_16x9:
	case HDMI_1920x1080p24_16x9:
	case HDMI_1920x1080p25_16x9:
	case HDMI_1920x1080p30_16x9:
	case HDMI_1920x1080p50_16x9:
	case HDMI_1920x1080p60_16x9:
		return 1;
	default:
		return 0;
	}
}

static ssize_t disp_cap_3d_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	int i, pos = 0;
	int j = 0;
	enum hdmi_vic vic;
	struct hdmitx_dev *hdev = &hdmitx_device;

	pos += snprintf(buf + pos, PAGE_SIZE, "3D support lists:\n");
	for (i = 0; disp_mode_t[i]; i++) {
		/* 3D is not supported under 4k modes */
		if (strstr(disp_mode_t[i], "2160p") ||
		    strstr(disp_mode_t[i], "smpte"))
			continue;
		vic = hdmitx_edid_get_VIC(hdev, disp_mode_t[i], 0);
		for (j = 0; j < hdev->rxcap.VIC_count; j++) {
			if (vic == hdev->rxcap.VIC[j])
				break;
		}
		pos += snprintf(buf + pos, PAGE_SIZE, "\n%s ",
			disp_mode_t[i]);
		if (local_support_3dfp(vic) &&
		    hdev->rxcap.support_3d_format[hdev->rxcap.VIC[j]].
		    frame_packing == 1) {
			pos += snprintf(buf + pos, PAGE_SIZE, "FramePacking ");
		}
		if (hdev->rxcap.support_3d_format[hdev->rxcap.VIC[j]].
		    top_and_bottom == 1) {
			pos += snprintf(buf + pos, PAGE_SIZE, "TopBottom ");
		}
		if (hdev->rxcap.support_3d_format[hdev->rxcap.VIC[j]].
		    side_by_side == 1) {
			pos += snprintf(buf + pos, PAGE_SIZE, "SidebySide ");
		}
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "\n");

	return pos;
}

static void _show_pcm_ch(struct rx_cap *prxcap, int i,
			 int *ppos, char *buf)
{
	const char * const aud_sample_size[] = {"ReferToStreamHeader",
		"16", "20", "24", NULL};
	int j = 0;

	for (j = 0; j < 3; j++) {
		if (prxcap->RxAudioCap[i].cc3 & (1 << j))
			*ppos += snprintf(buf + *ppos, PAGE_SIZE, "%s/",
				aud_sample_size[j + 1]);
	}
	*ppos += snprintf(buf + *ppos - 1, PAGE_SIZE, " bit\n");
}

/**/
static ssize_t aud_cap_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	int i, pos = 0, j;
	static const char * const aud_ct[] =  {
		"ReferToStreamHeader", "PCM", "AC-3", "MPEG1", "MP3",
		"MPEG2", "AAC", "DTS", "ATRAC",	"OneBitAudio",
		"Dobly_Digital+", "DTS-HD", "MAT", "DST", "WMA_Pro",
		"Reserved", NULL};
	static const char * const aud_sampling_frequency[] = {
		"ReferToStreamHeader", "32", "44.1", "48", "88.2", "96",
		"176.4", "192", NULL};
	struct rx_cap *prxcap = &hdmitx_device.rxcap;

	pos += snprintf(buf + pos, PAGE_SIZE,
		"CodingType MaxChannels SamplingFreq SampleSize\n");
	for (i = 0; i < prxcap->AUD_count; i++) {
		pos += snprintf(buf + pos, PAGE_SIZE, "%s",
			aud_ct[prxcap->RxAudioCap[i].audio_format_code]);
		if (prxcap->RxAudioCap[i].audio_format_code == CT_DOLBY_D &&
		    (prxcap->RxAudioCap[i].cc3 & 1))
			pos += snprintf(buf + pos, PAGE_SIZE, "/ATMOS");
		pos += snprintf(buf + pos, PAGE_SIZE, ", %d ch, ",
			prxcap->RxAudioCap[i].channel_num_max + 1);
		for (j = 0; j < 7; j++) {
			if (prxcap->RxAudioCap[i].freq_cc & (1 << j))
				pos += snprintf(buf + pos, PAGE_SIZE, "%s/",
					aud_sampling_frequency[j + 1]);
		}
		pos += snprintf(buf + pos - 1, PAGE_SIZE, " kHz, ");
		switch (prxcap->RxAudioCap[i].audio_format_code) {
		case CT_PCM:
			_show_pcm_ch(prxcap, i, &pos, buf);
			break;
		case CT_AC_3:
		case CT_MPEG1:
		case CT_MP3:
		case CT_MPEG2:
		case CT_AAC:
		case CT_DTS:
		case CT_ATRAC:
		case CT_ONE_BIT_AUDIO:
			pos += snprintf(buf + pos, PAGE_SIZE,
				"MaxBitRate %dkHz\n",
				prxcap->RxAudioCap[i].cc3 * 8);
			break;
		case CT_DOLBY_D:
		case CT_DTS_HD:
		case CT_MAT:
		case CT_DST:
			pos += snprintf(buf + pos, PAGE_SIZE, "DepVaule 0x%x\n",
				prxcap->RxAudioCap[i].cc3);
			break;
		case CT_WMA:
		default:
			break;
		}
	}
	return pos;
}

/**/
static ssize_t hdmi_hdr_status_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = &hdmitx_device;

	/* pos = 3 */
	if (hdr_status_pos == 3 || hdev->hdr10plus_feature) {
		pos += snprintf(buf + pos, PAGE_SIZE, "HDR10Plus-VSIF");
		return pos;
	}

	/* pos = 2 */
	if (hdr_status_pos == 2) {
		if (hdev->hdmi_current_eotf_type == EOTF_T_DOLBYVISION) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"DolbyVision-Std");
			return pos;
		}
		if (hdev->hdmi_current_eotf_type == EOTF_T_LL_MODE) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"DolbyVision-Lowlatency");
			return pos;
		}
	}

	/* pos = 1 */
	if (hdr_status_pos == 1) {
		if (hdev->hdr_transfer_feature == T_SMPTE_ST_2084) {
			if (hdev->hdr_color_feature == C_BT2020) {
				pos += snprintf(buf + pos, PAGE_SIZE,
					"HDR10-GAMMA_ST2084");
				return pos;
			}
			pos += snprintf(buf + pos, PAGE_SIZE, "HDR10-others");
			return pos;
		}
		if (hdev->hdr_color_feature == C_BT2020 &&
		    (hdev->hdr_transfer_feature == T_BT2020_10 ||
		     hdev->hdr_transfer_feature == T_HLG)) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"HDR10-GAMMA_HLG");
			return pos;
		}
	}

	/* default is SDR */
	pos += snprintf(buf + pos, PAGE_SIZE, "SDR");

	return pos;
}

/**/
static ssize_t dc_cap_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	enum hdmi_vic vic = HDMI_UNKNOWN;
	int pos = 0;
	struct rx_cap *prxcap = &hdmitx_device.rxcap;
	const struct dv_info *dv = &hdmitx_device.rxcap.dv_info;
	const struct dv_info *dv2 = &hdmitx_device.rxcap.dv_info2;

	if (prxcap->dc_36bit_420)
		pos += snprintf(buf + pos, PAGE_SIZE, "420,12bit\n");
	if (prxcap->dc_30bit_420) {
		pos += snprintf(buf + pos, PAGE_SIZE, "420,10bit\n");
		pos += snprintf(buf + pos, PAGE_SIZE, "420,8bit\n");
	} else {
		vic = hdmitx_edid_get_VIC(&hdmitx_device, "2160p60hz420", 0);
		if (vic != HDMI_UNKNOWN) {
			pos += snprintf(buf + pos, PAGE_SIZE, "420,8bit\n");
			goto next444;
		}
		vic = hdmitx_edid_get_VIC(&hdmitx_device, "2160p50hz420", 0);
		if (vic != HDMI_UNKNOWN) {
			pos += snprintf(buf + pos, PAGE_SIZE, "420,8bit\n");
			goto next444;
		}
		vic = hdmitx_edid_get_VIC(&hdmitx_device, "smpte60hz420", 0);
		if (vic != HDMI_UNKNOWN) {
			pos += snprintf(buf + pos, PAGE_SIZE, "420,8bit\n");
			goto next444;
		}
		vic = hdmitx_edid_get_VIC(&hdmitx_device, "smpte50hz420", 0);
		if (vic != HDMI_UNKNOWN) {
			pos += snprintf(buf + pos, PAGE_SIZE, "420,8bit\n");
			goto next444;
		}
	}
next444:
	if (prxcap->dc_y444) {
		if (prxcap->dc_36bit || dv->sup_10b_12b_444 == 0x2 ||
		    dv2->sup_10b_12b_444 == 0x2)
			pos += snprintf(buf + pos, PAGE_SIZE, "444,12bit\n");
		if (prxcap->dc_30bit || dv->sup_10b_12b_444 == 0x1 ||
		    dv2->sup_10b_12b_444 == 0x1) {
			pos += snprintf(buf + pos, PAGE_SIZE, "444,10bit\n");
			pos += snprintf(buf + pos, PAGE_SIZE, "444,8bit\n");
		}
		if (prxcap->dc_36bit || dv->sup_yuv422_12bit ||
		    dv2->sup_yuv422_12bit)
			pos += snprintf(buf + pos, PAGE_SIZE, "422,12bit\n");
		if (prxcap->dc_30bit) {
			pos += snprintf(buf + pos, PAGE_SIZE, "422,10bit\n");
			pos += snprintf(buf + pos, PAGE_SIZE, "422,8bit\n");
			goto nextrgb;
		}
	} else {
		if (prxcap->native_Mode & (1 << 5))
			pos += snprintf(buf + pos, PAGE_SIZE, "444,8bit\n");
		if (prxcap->native_Mode & (1 << 4))
			pos += snprintf(buf + pos, PAGE_SIZE, "422,8bit\n");
	}
nextrgb:
	if (prxcap->dc_36bit || dv->sup_10b_12b_444 == 0x2 ||
	    dv2->sup_10b_12b_444 == 0x2)
		pos += snprintf(buf + pos, PAGE_SIZE, "rgb,12bit\n");
	if (prxcap->dc_30bit || dv->sup_10b_12b_444 == 0x1 ||
	    dv2->sup_10b_12b_444 == 0x1)
		pos += snprintf(buf + pos, PAGE_SIZE, "rgb,10bit\n");
	pos += snprintf(buf + pos, PAGE_SIZE, "rgb,8bit\n");
	return pos;
}

static bool valid_mode;
static char cvalid_mode[32];

static bool pre_process_str(char *name)
{
	int i;
	unsigned int flag = 0;
	char *color_format[4] = {"444", "422", "420", "rgb"};

	for (i = 0 ; i < 4 ; i++) {
		if (strstr(name, color_format[i]))
			flag++;
	}
	if (flag >= 2)
		return 0;
	else
		return 1;
}

static ssize_t valid_mode_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	int pos = 0;
	struct hdmi_format_para *para = NULL;

	if (cvalid_mode[0]) {
		valid_mode = pre_process_str(cvalid_mode);
		if (valid_mode == 0) {
			pos += snprintf(buf + pos, PAGE_SIZE, "%d\n\r",
				valid_mode);
			return pos;
		}
		para = hdmi_tst_fmt_name(cvalid_mode, cvalid_mode);
	}
	if (para) {
		pr_info(SYS "sname = %s\n", para->sname);
		pr_info(SYS "char_clk = %d\n", para->tmds_clk);
		pr_info(SYS "cd = %d\n", para->cd);
		pr_info(SYS "cs = %d\n", para->cs);
	}

	valid_mode = hdmitx_edid_check_valid_mode(&hdmitx_device, para);

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n\r", valid_mode);

	return pos;
}

static ssize_t valid_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	memset(cvalid_mode, 0, sizeof(cvalid_mode));
	strncpy(cvalid_mode, buf, sizeof(cvalid_mode));
	cvalid_mode[31] = '\0';
	return count;
}

static ssize_t allm_cap_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct rx_cap *prxcap = &hdmitx_device.rxcap;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n\r", prxcap->allm);
	return pos;
}

static ssize_t allm_mode_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = &hdmitx_device;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n\r", hdev->allm_mode);

	return pos;
}

static inline int com_str(const char *buf, const char *str)
{
	return strncmp(buf, str, strlen(str)) == 0;
}

static ssize_t allm_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t count)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	pr_info("hdmitx: store allm_mode as %s\n", buf);

	if (com_str(buf, "0")) {
		// disable ALLM
		hdev->allm_mode = 0;
		hdmitx_construct_vsif(hdev, VT_ALLM, 0, NULL);
		if (is_hdmi14_4k(hdev->cur_VIC))
			hdmitx_construct_vsif(hdev, VT_HDMI14_4K, 1, NULL);
	}
	if (com_str(buf, "1")) {
		hdev->allm_mode = 1;
		hdmitx_construct_vsif(hdev, VT_ALLM, 1, NULL);
		hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_OFF);
	}

	return count;
}

static ssize_t contenttype_cap_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct rx_cap *prxcap = &hdmitx_device.rxcap;

	if (prxcap->cnc0)
		pos += snprintf(buf + pos, PAGE_SIZE, "graphics\n\r");
	if (prxcap->cnc1)
		pos += snprintf(buf + pos, PAGE_SIZE, "photo\n\r");
	if (prxcap->cnc2)
		pos += snprintf(buf + pos, PAGE_SIZE, "cinema\n\r");
	if (prxcap->cnc3)
		pos += snprintf(buf + pos, PAGE_SIZE, "game\n\r");

	return pos;
}

static ssize_t contenttype_mode_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = &hdmitx_device;

	if (hdev->ct_mode == 0)
		pos += snprintf(buf + pos, PAGE_SIZE, "off\n\r");
	if (hdev->ct_mode == 1)
		pos += snprintf(buf + pos, PAGE_SIZE, "game\n\r");
	if (hdev->ct_mode == 2)
		pos += snprintf(buf + pos, PAGE_SIZE, "graphics\n\r");
	if (hdev->ct_mode == 3)
		pos += snprintf(buf + pos, PAGE_SIZE, "photo\n\r");
	if (hdev->ct_mode == 4)
		pos += snprintf(buf + pos, PAGE_SIZE, "cinema\n\r");

	return pos;
}

static ssize_t contenttype_mode_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	pr_info("hdmitx: store contenttype_mode as %s\n", buf);

	hdev->allm_mode = 0;
	hdmitx_construct_vsif(hdev, VT_ALLM, 0, NULL);
	if (is_hdmi14_4k(hdev->cur_VIC))
		hdmitx_construct_vsif(hdev, VT_HDMI14_4K, 1, NULL);
	hdev->ct_mode = 0;
	hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_OFF);

	if (com_str(buf, "1") || com_str(buf, "game")) {
		hdev->ct_mode = 1;
		hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_GAME);
	}
	if (com_str(buf, "2") || com_str(buf, "graphics")) {
		hdev->ct_mode = 2;
		hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_GRAPHICS);
	}
	if (com_str(buf, "3") || com_str(buf, "photo")) {
		hdev->ct_mode = 3;
		hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_PHOTO);
	}
	if (com_str(buf, "4") || com_str(buf, "cinema")) {
		hdev->ct_mode = 4;
		hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_CINEMA);
	}

	return count;
}

/**/
static ssize_t hdr_cap_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	int pos = 0;
	unsigned int i, j;
	struct rx_cap *prxcap = &hdmitx_device.rxcap;
	int hdr10plugsupported = 0;

	if (prxcap->hdr10plus_info.ieeeoui == HDR10_PLUS_IEEE_OUI &&
	    prxcap->hdr10plus_info.application_version != 0xFF)
		hdr10plugsupported = 1;
	pos += snprintf(buf + pos, PAGE_SIZE, "HDR10Plus Supported: %d\n",
		hdr10plugsupported);
	pos += snprintf(buf + pos, PAGE_SIZE, "HDR Static Metadata:\n");
	pos += snprintf(buf + pos, PAGE_SIZE, "    Supported EOTF:\n");
	pos += snprintf(buf + pos, PAGE_SIZE, "        Traditional SDR: %d\n",
		prxcap->hdr_sup_eotf_sdr);
	pos += snprintf(buf + pos, PAGE_SIZE, "        Traditional HDR: %d\n",
		prxcap->hdr_sup_eotf_hdr);
	pos += snprintf(buf + pos, PAGE_SIZE, "        SMPTE ST 2084: %d\n",
		prxcap->hdr_sup_eotf_smpte_st_2084);
	pos += snprintf(buf + pos, PAGE_SIZE, "        Hybrif Log-Gamma: %d\n",
		prxcap->hdr_sup_eotf_hlg);
	pos += snprintf(buf + pos, PAGE_SIZE, "    Supported SMD type1: %d\n",
		prxcap->hdr_sup_SMD_type1);
	pos += snprintf(buf + pos, PAGE_SIZE, "    Luminance Data\n");
	pos += snprintf(buf + pos, PAGE_SIZE, "        Max: %d\n",
		prxcap->hdr_lum_max);
	pos += snprintf(buf + pos, PAGE_SIZE, "        Avg: %d\n",
		prxcap->hdr_lum_avg);
	pos += snprintf(buf + pos, PAGE_SIZE, "        Min: %d\n\n",
		prxcap->hdr_lum_min);
	pos += snprintf(buf + pos, PAGE_SIZE, "HDR Dynamic Metadata:");

	for (i = 0; i < 4; i++) {
		if (prxcap->hdr_dynamic_info[i].type == 0)
			continue;
		pos += snprintf(buf + pos, PAGE_SIZE,
			"\n    metadata_version: %x\n",
			prxcap->hdr_dynamic_info[i].type);
		pos += snprintf(buf + pos, PAGE_SIZE,
			"        support_flags: %x\n",
			prxcap->hdr_dynamic_info[i].support_flags);
		pos += snprintf(buf + pos, PAGE_SIZE,
			"        optional_fields:");
		for (j = 0; j <
			(prxcap->hdr_dynamic_info[i].hd_len - 3); j++)
			pos += snprintf(buf + pos, PAGE_SIZE, " %x",
				prxcap->hdr_dynamic_info[i].optional_fields[j]);
	}

	pos += snprintf(buf + pos, PAGE_SIZE, "\n\ncolorimetry_data: %x\n",
		prxcap->colorimetry_data);

	return pos;
}

static ssize_t _show_dv_cap(struct device *dev,
			    struct device_attribute *attr,
			    char *buf,
			    const struct dv_info *dv)
{
	int pos = 0;
	int i;

	if (dv->ieeeoui != DV_IEEE_OUI) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"The Rx don't support DolbyVision\n");
		return pos;
	}
	if (dv->block_flag != CORRECT) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"DolbyVision block is error\n");
		return pos;
	}
	pos += snprintf(buf + pos, PAGE_SIZE,
		"DolbyVision RX support list:\n");

	if (dv->ver == 0) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"VSVDB Version: V%d\n", dv->ver);
		pos += snprintf(buf + pos, PAGE_SIZE,
			"2160p%shz: 1\n",
			dv->sup_2160p60hz ? "60" : "30");
		pos += snprintf(buf + pos, PAGE_SIZE,
			"Support mode:\n");
		pos += snprintf(buf + pos, PAGE_SIZE,
			"  DV_RGB_444_8BIT\n");
		if (dv->sup_yuv422_12bit)
			pos += snprintf(buf + pos, PAGE_SIZE,
				"  DV_YCbCr_422_12BIT\n");
	}
	if (dv->ver == 1) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"VSVDB Version: V%d(%d-byte)\n",
			dv->ver, dv->length + 1);
		if (dv->length == 0xB) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"2160p%shz: 1\n",
				dv->sup_2160p60hz ? "60" : "30");
		pos += snprintf(buf + pos, PAGE_SIZE,
			"Support mode:\n");
		pos += snprintf(buf + pos, PAGE_SIZE,
			"  DV_RGB_444_8BIT\n");
		if (dv->sup_yuv422_12bit)
			pos += snprintf(buf + pos, PAGE_SIZE,
			"  DV_YCbCr_422_12BIT\n");
		if (dv->low_latency == 0x01)
			pos += snprintf(buf + pos, PAGE_SIZE,
				"  LL_YCbCr_422_12BIT\n");
		}

		if (dv->length == 0xE) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"2160p%shz: 1\n",
				dv->sup_2160p60hz ? "60" : "30");
			pos += snprintf(buf + pos, PAGE_SIZE,
				"Support mode:\n");
			pos += snprintf(buf + pos, PAGE_SIZE,
				"  DV_RGB_444_8BIT\n");
			if (dv->sup_yuv422_12bit)
				pos += snprintf(buf + pos, PAGE_SIZE,
				"  DV_YCbCr_422_12BIT\n");
		}
	}
	if (dv->ver == 2) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"VSVDB Version: V%d\n", dv->ver);
		pos += snprintf(buf + pos, PAGE_SIZE,
			"2160p%shz: 1\n",
			dv->sup_2160p60hz ? "60" : "30");
		pos += snprintf(buf + pos, PAGE_SIZE,
			"Support mode:\n");
		if (dv->Interface != 0x00 && dv->Interface != 0x01) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"  DV_RGB_444_8BIT\n");
			if (dv->sup_yuv422_12bit)
				pos += snprintf(buf + pos, PAGE_SIZE,
					"  DV_YCbCr_422_12BIT\n");
		}
		pos += snprintf(buf + pos, PAGE_SIZE,
			"  LL_YCbCr_422_12BIT\n");
		if (dv->Interface == 0x01 || dv->Interface == 0x03) {
			if (dv->sup_10b_12b_444 == 0x1) {
				pos += snprintf(buf + pos, PAGE_SIZE,
					"  LL_RGB_444_10BIT\n");
			}
			if (dv->sup_10b_12b_444 == 0x2) {
				pos += snprintf(buf + pos, PAGE_SIZE,
					"  LL_RGB_444_12BIT\n");
			}
		}
	}
	pos += snprintf(buf + pos, PAGE_SIZE,
		"IEEEOUI: 0x%06x\n", dv->ieeeoui);
	pos += snprintf(buf + pos, PAGE_SIZE, "VSVDB: ");
	for (i = 0; i < (dv->length + 1); i++)
		pos += snprintf(buf + pos, PAGE_SIZE, "%02x",
		dv->rawdata[i]);
	pos += snprintf(buf + pos, PAGE_SIZE, "\n");
	return pos;
}

static ssize_t dv_cap_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int pos = 0;
	const struct dv_info *dv = &hdmitx_device.rxcap.dv_info;

	if (dv->ieeeoui != DV_IEEE_OUI || hdmitx_device.hdr_priority) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"The Rx don't support DolbyVision\n");
		return pos;
	}
	return _show_dv_cap(dev, attr, buf, dv);
}

static ssize_t dv_cap2_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	const struct dv_info *dv2 = &hdmitx_device.rxcap.dv_info2;

	return _show_dv_cap(dev, attr, buf, dv2);
}

static ssize_t aud_ch_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE,
		"hdmi_channel = %d ch\n",
		hdmitx_device.hdmi_ch ? hdmitx_device.hdmi_ch + 1 : 0);
	return pos;
}

static ssize_t aud_ch_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	if (strncmp(buf, "6ch", 3) == 0)
		hdmitx_device.hdmi_ch = 5;
	else if (strncmp(buf, "8ch", 3) == 0)
		hdmitx_device.hdmi_ch = 7;
	else if (strncmp(buf, "2ch", 3) == 0)
		hdmitx_device.hdmi_ch = 1;
	else
		return count;

	hdmitx_device.audio_param_update_flag = 1;
	hdmitx_device.force_audio_flag = 1;

	return count;
}

/*
 *  1: set avmute
 * -1: clear avmute
 *  0: off avmute
 */
static ssize_t avmute_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int cmd = OFF_AVMUTE;
	static int mask0;
	static int mask1;
	static DEFINE_MUTEX(avmute_mutex);

	pr_info("%s %s\n", __func__, buf);
	mutex_lock(&avmute_mutex);
	if (strncmp(buf, "-1", 2) == 0) {
		cmd = CLR_AVMUTE;
		mask0 = -1;
	} else if (strncmp(buf, "0", 1) == 0) {
		cmd = OFF_AVMUTE;
		mask0 = 0;
	} else if (strncmp(buf, "1", 1) == 0) {
		cmd = SET_AVMUTE;
		mask0 = 1;
	}
	if (strncmp(buf, "r-1", 3) == 0) {
		cmd = CLR_AVMUTE;
		mask1 = -1;
	} else if (strncmp(buf, "r0", 2) == 0) {
		cmd = OFF_AVMUTE;
		mask1 = 0;
	} else if (strncmp(buf, "r1", 2) == 0) {
		cmd = SET_AVMUTE;
		mask1 = 1;
	}
	if (mask0 == 1 || mask1 == 1)
		cmd = SET_AVMUTE;
	else if ((mask0 == -1) && (mask1 == -1))
		cmd = CLR_AVMUTE;
	hdmitx_device.hwop.cntlmisc(&hdmitx_device, MISC_AVMUTE_OP, cmd);
	mutex_unlock(&avmute_mutex);

	return count;
}

static ssize_t avmute_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct hdmitx_dev *hdev = &hdmitx_device;
	int ret = 0;
	int pos = 0;

	ret = hdev->hwop.cntlmisc(hdev, MISC_READ_AVMUTE_OP, 0);
	pos += snprintf(buf + pos, PAGE_SIZE, "%d", ret);

	return pos;
}

/*
 * 0: clear vic
 */
static ssize_t vic_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	if (strncmp(buf, "0", 1) == 0) {
		hdev->hwop.cntlconfig(hdev, CONF_CLR_AVI_PACKET, 0);
		hdev->hwop.cntlconfig(hdev, CONF_CLR_VSDB_PACKET, 0);
	}

	return count;
}

static ssize_t vic_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct hdmitx_dev *hdev = &hdmitx_device;
	enum hdmi_vic vic = HDMI_UNKNOWN;
	int pos = 0;

	vic = hdev->hwop.getstate(hdev, STAT_VIDEO_VIC, 0);
	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n", vic);

	return pos;
}

/*
 *  1: enable hdmitx phy
 *  0: disable hdmitx phy
 */
static ssize_t phy_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	int cmd = TMDS_PHY_ENABLE;

	pr_info(SYS "%s %s\n", __func__, buf);

	if (strncmp(buf, "0", 1) == 0)
		cmd = TMDS_PHY_DISABLE;
	else if (strncmp(buf, "1", 1) == 0)
		cmd = TMDS_PHY_ENABLE;
	else
		pr_info(SYS "set phy wrong: %s\n", buf);

	hdmitx_device.hwop.cntlmisc(&hdmitx_device, MISC_TMDS_PHY_OP, cmd);
	return count;
}

static ssize_t phy_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t rxsense_policy_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int val = 0;

	if (isdigit(buf[0])) {
		val = buf[0] - '0';
		pr_info(SYS "hdmitx: set rxsense_policy as %d\n", val);
		if (val == 0 || val == 1)
			hdmitx_device.rxsense_policy = val;
		else
			pr_info(SYS "only accept as 0 or 1\n");
	}
	if (hdmitx_device.rxsense_policy)
		queue_delayed_work(hdmitx_device.rxsense_wq,
				   &hdmitx_device.work_rxsense, 0);
	else
		cancel_delayed_work(&hdmitx_device.work_rxsense);

	return count;
}

static ssize_t rxsense_policy_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		hdmitx_device.rxsense_policy);

	return pos;
}

/* cedst_policy: 0, no CED feature
 *	       1, auto mode, depends on RX scdc_present
 *	       2, forced CED feature
 */
static ssize_t cedst_policy_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t count)
{
	int val = 0;
	struct hdmitx_dev *hdev = &hdmitx_device;

	if (isdigit(buf[0])) {
		val = buf[0] - '0';
		pr_info("hdmitx: set cedst_policy as %d\n", val);
		if (val == 0 || val == 1 || val == 2) {
			hdev->cedst_policy = val;
			if (val == 1) { /* Auto mode, depends on Rx */
				/* check RX scdc_present */
				if (hdev->rxcap.scdc_present)
					hdev->cedst_policy = 1;
				else
					hdev->cedst_policy = 0;
			}
			if (val == 2) /* Force mode */
				hdev->cedst_policy = 1;
			/* assgin cedst_en from dts or here */
			hdev->cedst_en = hdev->cedst_policy;
		} else {
			pr_info("only accept as 0, 1(auto), or 2(force)\n");
		}
	}
	if (hdev->cedst_policy)
		queue_delayed_work(hdev->cedst_wq, &hdev->work_cedst, 0);
	else
		cancel_delayed_work(&hdev->work_cedst);

	return count;
}

static ssize_t cedst_policy_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		hdmitx_device.cedst_policy);

	return pos;
}

static ssize_t cedst_count_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	int pos = 0;
	struct ced_cnt *ced = &hdmitx_device.ced_cnt;
	struct scdc_locked_st *ch_st = &hdmitx_device.chlocked_st;

	if (!ch_st->clock_detected)
		pos += snprintf(buf + pos, PAGE_SIZE, "clock undetected\n");
	if (!ch_st->ch0_locked)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH0 unlocked\n");
	if (!ch_st->ch1_locked)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH1 unlocked\n");
	if (!ch_st->ch2_locked)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH2 unlocked\n");
	if (ced->ch0_valid && ced->ch0_cnt)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH0 ErrCnt 0x%x\n",
			ced->ch0_cnt);
	if (ced->ch1_valid && ced->ch1_cnt)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH1 ErrCnt 0x%x\n",
			ced->ch1_cnt);
	if (ced->ch2_valid && ced->ch2_cnt)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH2 ErrCnt 0x%x\n",
			ced->ch2_cnt);
	memset(ced, 0, sizeof(*ced));

	return pos;
}

static ssize_t sspll_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf,
			   size_t count)
{
	int val = 0;

	if (isdigit(buf[0])) {
		val = buf[0] - '0';
		pr_info(SYS "set sspll : %d\n", val);
		if (val == 0 || val == 1)
			hdmitx_device.sspll = val;
		else
			pr_info(SYS "sspll only accept as 0 or 1\n");
	}

	return count;
}

static ssize_t sspll_show(struct device *dev,
			  struct device_attribute *attr,
			  char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		hdmitx_device.sspll);

	return pos;
}

static ssize_t frac_rate_policy_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t count)
{
	int val = 0;

	if (isdigit(buf[0])) {
		val = buf[0] - '0';
		pr_debug(SYS "set frac_rate_policy as %d\n", val);
		if (val == 0 || val == 1)
			hdmitx_device.frac_rate_policy = val;
		else
			pr_info(SYS "only accept as 0 or 1\n");
	}

	return count;
}

static ssize_t frac_rate_policy_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		hdmitx_device.frac_rate_policy);

	return pos;
}

static ssize_t hdcp_clkdis_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	hdmitx_device.hwop.cntlmisc(&hdmitx_device, MISC_HDCP_CLKDIS,
		buf[0] == '1' ? 1 : 0);
	return count;
}

static ssize_t hdcp_clkdis_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return 0;
}

static ssize_t hdcp_pwr_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	if (buf[0] == '1') {
		hdmitx_device.hdcp_tst_sig = 1;
		pr_info(SYS "set hdcp_pwr 1\n");
	}

	return count;
}

static ssize_t hdcp_pwr_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	int pos = 0;

	if (hdmitx_device.hdcp_tst_sig == 1) {
		pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
			hdmitx_device.hdcp_tst_sig);
		hdmitx_device.hdcp_tst_sig = 0;
		pr_info(SYS "restore hdcp_pwr 0\n");
	}

	return pos;
}

static ssize_t hdcp_byp_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	pr_info(SYS "%s...\n", __func__);

	hdmitx_device.hwop.cntlmisc(&hdmitx_device, MISC_HDCP_CLKDIS,
		buf[0] == '1' ? 1 : 0);

	return count;
}

static ssize_t hdcp_lstore_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	int pos = 0;

	/* if current TX is RP-TX, then return lstore as 00 */
	/* hdcp_lstore is used under only TX */
	if (hdmitx_device.repeater_tx == 1) {
		pos += snprintf(buf + pos, PAGE_SIZE, "00\n");
		return pos;
	}

	if (hdmitx_device.lstore < 0x10) {
		hdmitx_device.lstore = 0;
		if (hdmitx_device.hwop.cntlddc(&hdmitx_device,
					       DDC_HDCP_14_LSTORE,
					       0))
			hdmitx_device.lstore += 1;
		if (hdmitx_device.hwop.cntlddc(&hdmitx_device,
					       DDC_HDCP_22_LSTORE,
					       0))
			hdmitx_device.lstore += 2;
	}
	if ((hdmitx_device.lstore & 0x3) == 0x3) {
		pos += snprintf(buf + pos, PAGE_SIZE, "14+22\n");
	} else {
		if (hdmitx_device.lstore & 0x1)
			pos += snprintf(buf + pos, PAGE_SIZE, "14\n");
		if (hdmitx_device.lstore & 0x2)
			pos += snprintf(buf + pos, PAGE_SIZE, "22\n");
		if ((hdmitx_device.lstore & 0xf) == 0)
			pos += snprintf(buf + pos, PAGE_SIZE, "00\n");
	}
	return pos;
}

static ssize_t hdcp_lstore_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	pr_info("hdcp: set lstore as %s\n", buf);
	if (strncmp(buf, "0", 1) == 0)
		hdmitx_device.lstore = 0x10;
	if (strncmp(buf, "11", 2) == 0)
		hdmitx_device.lstore = 0x11;
	if (strncmp(buf, "12", 2) == 0)
		hdmitx_device.lstore = 0x12;
	if (strncmp(buf, "13", 2) == 0)
		hdmitx_device.lstore = 0x13;

	return count;
}

static int rptxlstore;
static ssize_t hdcp_rptxlstore_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int pos = 0;

	/* if current TX is not RP-TX, then return rptxlstore as 00 */
	/* hdcp_rptxlstore is used under only RP-TX */
	if (hdmitx_device.repeater_tx == 0) {
		pos += snprintf(buf + pos, PAGE_SIZE, "00\n");
		return pos;
	}

	if (rptxlstore < 0x10) {
		rptxlstore = 0;
		if (hdmitx_device.hwop.cntlddc(&hdmitx_device,
					       DDC_HDCP_14_LSTORE,
					       0))
			rptxlstore += 1;
		if (hdmitx_device.hwop.cntlddc(&hdmitx_device,
					       DDC_HDCP_22_LSTORE,
					       0))
			rptxlstore += 2;
	}
	if (rptxlstore & 0x1)
		pos += snprintf(buf + pos, PAGE_SIZE, "14\n");
	if (rptxlstore & 0x2)
		pos += snprintf(buf + pos, PAGE_SIZE, "22\n");
	if ((rptxlstore & 0xf) == 0)
		pos += snprintf(buf + pos, PAGE_SIZE, "00\n");
	return pos;
}

static ssize_t hdcp_rptxlstore_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	pr_info("hdcp: set lstore as %s\n", buf);
	if (strncmp(buf, "0", 1) == 0)
		rptxlstore = 0x10;
	if (strncmp(buf, "11", 2) == 0)
		rptxlstore = 0x11;
	if (strncmp(buf, "12", 2) == 0)
		rptxlstore = 0x12;
	if (strncmp(buf, "13", 2) == 0)
		rptxlstore = 0x13;

	return count;
}

static ssize_t div40_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n", hdmitx_device.div40);

	return pos;
}

static ssize_t div40_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	hdev->hwop.cntlddc(hdev, DDC_SCDC_DIV40_SCRAMB, buf[0] == '1');
	hdmitx_device.div40 = (buf[0] == '1');

	return count;
}

static ssize_t hdcp_mode_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	int pos = 0;

	switch (hdmitx_device.hdcp_mode) {
	case 1:
		pos += snprintf(buf + pos, PAGE_SIZE, "14");
		break;
	case 2:
		pos += snprintf(buf + pos, PAGE_SIZE, "22");
		break;
	default:
		pos += snprintf(buf + pos, PAGE_SIZE, "off");
		break;
	}

	return pos;
}

static ssize_t hdcp_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	pr_info(SYS "hdcp: set mode as %s\n", buf);
	hdmitx_device.hwop.cntlddc(&hdmitx_device, DDC_HDCP_MUX_INIT, 1);
	hdmitx_device.hwop.cntlddc(&hdmitx_device, DDC_HDCP_GET_AUTH, 0);
	if (strncmp(buf, "0", 1) == 0) {
		hdmitx_device.hdcp_mode = 0;
		hdmitx_device.hwop.cntlddc(&hdmitx_device,
			DDC_HDCP_OP, HDCP14_OFF);
		hdmitx_hdcp_do_work(&hdmitx_device);
	}
	if (strncmp(buf, "1", 1) == 0) {
		hdmitx_device.hdcp_mode = 1;
		hdmitx_hdcp_do_work(&hdmitx_device);
		hdmitx_device.hwop.cntlddc(&hdmitx_device,
			DDC_HDCP_OP, HDCP14_ON);
	}
	if (strncmp(buf, "2", 1) == 0) {
		hdmitx_device.hdcp_mode = 2;
		hdmitx_hdcp_do_work(&hdmitx_device);
		hdmitx_device.hwop.cntlddc(&hdmitx_device,
			DDC_HDCP_MUX_INIT, 2);
	}

	return count;
}

static bool hdcp_sticky_mode;
static ssize_t hdcp_stickmode_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n", hdcp_sticky_mode);

	return pos;
}

static ssize_t hdcp_stickmode_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	if (buf[0] == '0')
		hdcp_sticky_mode = 0;
	if (buf[0] == '1')
		hdcp_sticky_mode = 1;

	return count;
}

static unsigned char hdcp_sticky_step;
static ssize_t hdcp_stickstep_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%x\n", hdcp_sticky_step);
	if (hdcp_sticky_step)
		hdcp_sticky_step = 0;

	return pos;
}

static ssize_t hdcp_stickstep_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	if (isdigit(buf[0]))
		hdcp_sticky_step = buf[0] - '0';

	return count;
}

/* Indicate whether a rptx under repeater */
static ssize_t hdmi_repeater_tx_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		!!hdmitx_device.repeater_tx);

	return pos;
}

#include <linux/amlogic/media/vout/hdmi_tx/hdmi_rptx.h>

void direct_hdcptx14_opr(enum rptx_hdcp14_cmd cmd, void *args)
{
	int rst;
	struct hdmitx_dev *hdev = &hdmitx_device;

	pr_info("%s[%d] cmd: %d\n", __func__, __LINE__, cmd);
	switch (cmd) {
	case RPTX_HDCP14_OFF:
		hdev->hdcp_mode = 0;
		hdev->hwop.cntlddc(hdev, DDC_HDCP_OP, HDCP14_OFF);
		break;
	case RPTX_HDCP14_ON:
		hdev->hdcp_mode = 1;
		hdev->hwop.cntlddc(hdev, DDC_HDCP_OP, HDCP14_ON);
		break;
	case RPTX_HDCP14_GET_AUTHST:
		rst = hdev->hwop.cntlddc(hdev, DDC_HDCP_GET_AUTH, 0);
		*(int *)args = rst;
		break;
	}
}
EXPORT_SYMBOL(direct_hdcptx14_opr);

static ssize_t hdcp_ctrl_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	if (hdev->hwop.cntlddc(hdev, DDC_HDCP_14_LSTORE, 0) == 0)
		return count;

	/* for repeater */
	if (hdev->repeater_tx) {
		dev_warn(dev, "hdmitx20: %s\n", buf);
		if (strncmp(buf, "rstop", 5) == 0) {
			if (strncmp(buf + 5, "14", 2) == 0)
				hdev->hwop.cntlddc(hdev, DDC_HDCP_OP,
					HDCP14_OFF);
			if (strncmp(buf + 5, "22", 2) == 0)
				hdev->hwop.cntlddc(hdev, DDC_HDCP_OP,
					HDCP22_OFF);
			hdev->hdcp_mode = 0;
			hdmitx_hdcp_do_work(hdev);
		}
		return count;
	}
	/* for non repeater */
	if (strncmp(buf, "stop", 4) == 0) {
		dev_warn(dev, "hdmitx20: %s\n", buf);
		if (strncmp(buf + 4, "14", 2) == 0)
			hdev->hwop.cntlddc(hdev, DDC_HDCP_OP, HDCP14_OFF);
		if (strncmp(buf + 4, "22", 2) == 0)
			hdev->hwop.cntlddc(hdev, DDC_HDCP_OP, HDCP22_OFF);
		hdev->hdcp_mode = 0;
		hdmitx_hdcp_do_work(hdev);
	}

	return count;
}

static ssize_t hdcp_ctrl_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	return 0;
}

static ssize_t hdcp_ksv_info_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	int pos = 0, i;
	char bksv_buf[5];

	hdmitx_device.hwop.cntlddc(&hdmitx_device, DDC_HDCP_GET_BKSV,
		(unsigned long)bksv_buf);

	pos += snprintf(buf + pos, PAGE_SIZE, "HDCP14 BKSV: ");
	for (i = 0; i < 5; i++) {
		pos += snprintf(buf + pos, PAGE_SIZE, "%02x",
			bksv_buf[i]);
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "  %s\n",
		hdcp_ksv_valid(bksv_buf) ? "Valid" : "Invalid");

	return pos;
}

/* Special FBC check */
static int check_fbc_special(unsigned char *edid_dat)
{
	if (edid_dat[250] == 0xfb && edid_dat[251] == 0x0c)
		return 1;
	else
		return 0;
}

static ssize_t hdcp_ver_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	int pos = 0;
	u32 ver = 0U;

	if (check_fbc_special(&hdmitx_device.EDID_buf[0]) ||
	    check_fbc_special(&hdmitx_device.EDID_buf1[0])) {
		pos += snprintf(buf + pos, PAGE_SIZE, "00\n\r");
		return pos;
	}

	/* if TX don't have HDCP22 key, skip RX hdcp22 ver */
	if (hdmitx_device.hwop.cntlddc(&hdmitx_device,
				       DDC_HDCP_22_LSTORE, 0) == 0)
		goto next;

	/* Detect RX support HDCP22 */
	mutex_lock(&getedid_mutex);
	ver = hdcp_rd_hdcp22_ver();
	mutex_unlock(&getedid_mutex);
	if (ver) {
		pos += snprintf(buf + pos, PAGE_SIZE, "22\n\r");
		pos += snprintf(buf + pos, PAGE_SIZE, "14\n\r");
		return pos;
	}
next:	/* Detect RX support HDCP14 */
	/* Here, must assume RX support HDCP14, otherwise affect 1A-03 */
	pos += snprintf(buf + pos, PAGE_SIZE, "14\n\r");
	return pos;
}

static ssize_t hpd_state_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d",
		hdmitx_device.hpd_state);
	return pos;
}

static ssize_t fake_plug_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d", hdmitx_device.hpd_state);
}

static ssize_t fake_plug_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	pr_info("hdmitx: fake plug %s\n", buf);

	if (strncmp(buf, "1", 1) == 0)
		hdev->hpd_state = 1;

	if (strncmp(buf, "0", 1) == 0)
		hdev->hpd_state = 0;

	extcon_set_state_sync(hdmitx_extcon_hdmi, EXTCON_DISP_HDMI,
			      hdev->hpd_state);
	hdmitx_status.hpd_state = hdev->hpd_state;
	return count;
}

static ssize_t rhpd_state_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct hdmitx_dev *hdev = &hdmitx_device;
	int st;

	st = hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0);

	return snprintf(buf, PAGE_SIZE, "%d", hdev->rhpd_state);
}

static ssize_t max_exceed_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	return snprintf(buf, PAGE_SIZE, "%d", hdev->hdcp_max_exceed_state);
}

static ssize_t hdmi_init_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n\r",
			hdmitx_device.hdmi_init);
	return pos;
}

static ssize_t ready_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\r\n",
		hdmitx_device.ready);
	return pos;
}

static ssize_t ready_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	if (strncmp(buf, "0", 1) == 0)
		hdmitx_device.ready = 0;
	if (strncmp(buf, "1", 1) == 0)
		hdmitx_device.ready = 1;
	return count;
}

static ssize_t support_3d_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
			hdmitx_device.rxcap.threeD_present);
	return pos;
}

void print_drm_config_data(void)
{
	enum hdmi_hdr_transfer hdr_transfer_feature;
	enum hdmi_hdr_color hdr_color_feature;
	unsigned int colormetry;
	unsigned int hcnt, vcnt;

	pr_info("***drm_config_data***\n");
	hdr_transfer_feature = (drm_config_data.features >> 8) & 0xff;
	hdr_color_feature = (drm_config_data.features >> 16) & 0xff;
	colormetry = (drm_config_data.features >> 30) & 0x1;
	pr_info("tf=%u, cf=%u, colormetry=%u\n",
		hdr_transfer_feature, hdr_color_feature,
		colormetry);

	pr_info("primaries:\n");
	for (vcnt = 0; vcnt < 3; vcnt++) {
		for (hcnt = 0; hcnt < 2; hcnt++)
			pr_info("%u, ", drm_config_data.primaries[vcnt][hcnt]);
		pr_info("\n");
	}

	pr_info("white_point: ");
	for (hcnt = 0; hcnt < 2; hcnt++)
		pr_info("%u, ", drm_config_data.white_point[hcnt]);
	pr_info("\n");

	pr_info("luminance: ");
	for (hcnt = 0; hcnt < 2; hcnt++)
		pr_info("%u, ", drm_config_data.luminance[hcnt]);
	pr_info("\n");

	pr_info("max_content: %u, ", drm_config_data.max_content);
	pr_info("max_frame_average: %u\n", drm_config_data.max_frame_average);
}

void print_vsif_config_data(void)
{
	struct dv_vsif_para *data;

	data = &vsif_debug_info.data;
	pr_info("***vsif_config_data***\n");
	pr_info("type: %u, tunnel: %u, sigsdr: %u\n",
		vsif_debug_info.type,
		vsif_debug_info.tunnel_mode,
		vsif_debug_info.signal_sdr);
	pr_info("dv_vsif_para:\n");
	pr_info("ver: %u len: %u\n", data->ver, data->length);
	pr_info("ll: %u dvsig: %u\n", data->vers.ver2.low_latency,
		data->vers.ver2.dobly_vision_signal);
	pr_info("bcMD: %u axMD: %u\n", data->vers.ver2.backlt_ctrl_MD_present,
		data->vers.ver2.auxiliary_MD_present);
	pr_info("PQhi: %u PQlow: %u\n", data->vers.ver2.eff_tmax_PQ_hi,
		data->vers.ver2.eff_tmax_PQ_low);
	pr_info("axrm: %u, axrv: %u, ", data->vers.ver2.auxiliary_runmode,
		data->vers.ver2.auxiliary_runversion);
	pr_info("axdbg: %u\n", data->vers.ver2.auxiliary_debug0);
}

void print_hdr10p_config_data(void)
{
	unsigned int hcnt, vcnt;
	unsigned char *tmp;

	pr_info("***hdr10p_config_data***\n");
	pr_info("appver: %u, tlum: %u, avgrgb: %u\n",
		hdr10p_config_data.application_version,
		hdr10p_config_data.targeted_max_lum,
		hdr10p_config_data.average_maxrgb);
	tmp = hdr10p_config_data.distribution_values;
	pr_info("distribution_values:\n");
	for (vcnt = 0; vcnt < 3; vcnt++) {
		for (hcnt = 0; hcnt < 3; hcnt++)
			pr_info("%u, ", tmp[vcnt * 3 + hcnt]);
		pr_info("\n");
	}

	pr_info("nbca: %u, knpx: %u, knpy: %u\n",
		hdr10p_config_data.num_bezier_curve_anchors,
		hdr10p_config_data.knee_point_x,
		hdr10p_config_data.knee_point_y);
	tmp = hdr10p_config_data.bezier_curve_anchors;
	pr_info("bezier_curve_anchors:\n");
	for (vcnt = 0; vcnt < 3; vcnt++) {
		for (hcnt = 0; hcnt < 3; hcnt++)
			pr_info("%u, ", tmp[vcnt * 3 + hcnt]);
		pr_info("\n");
	}

	pr_info("gof: %u, ndf: %u\n",
		hdr10p_config_data.graphics_overlay_flag,
		hdr10p_config_data.no_delay_flag);
}

void print_emp_config_data(void)
{
	unsigned char *data;
	unsigned int hcnt, vcnt, size;

	data = emp_config_data.data;

	pr_info("***emp_config_data***\n");
	pr_info("type: %u, size: %u\n",
		emp_config_data.type,
		emp_config_data.size);
	pr_info("data:\n");
	size = emp_config_data.size;
	for (vcnt = 0; vcnt < 8; vcnt++) {
		for (hcnt = 0; hcnt < 16; hcnt++) {
			if (vcnt * 16 + hcnt >= size)
				break;
			pr_info("%u, ", data[vcnt * 16 + hcnt]);
		}
		if (vcnt * 16 + hcnt < size)
			pr_info("\n");
		else
			break;
	}
	pr_info("\n");
}

static ssize_t create_hdmitx_out_attr(char *buf)
{
	unsigned int reg_val;
	unsigned int reg_addr;
	unsigned char *conf;
	int pos = 0;

	reg_addr = HDMITX_DWC_FC_AVICONF0;
	reg_val = hdmitx_rd_reg(reg_addr);

	switch (reg_val & 0x3) {
	case 0:
		conf = "RGB";
		break;
	case 1:
		conf = "422";
		break;
	case 2:
		conf = "444";
		break;
	case 3:
		conf = "420";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "%s,", conf);

	reg_addr = HDMITX_DWC_VP_PR_CD;
	reg_val = hdmitx_rd_reg(reg_addr);

	switch ((reg_val & 0xf0) >> 4) {
	case 0:
	case 4:
		conf = "8bit";
		break;
	case 5:
		conf = "10bit";
		break;
	case 6:
		conf = "12bit";
		break;
	case 7:
		conf = "16bit";
		break;
	default:
		conf = "reserved";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "%s", conf);

	return pos;
}

static ssize_t create_hdmitx_out_hdr(char *buf)
{
	unsigned int reg_val, vsd_ieee_id[3];
	unsigned int reg_addr;
	unsigned char *conf;
	int pos = 0;

	if (hdr_status_pos == 2) {
		reg_addr = HDMITX_DWC_FC_VSDIEEEID0;
		reg_val = hdmitx_rd_reg(reg_addr);
		vsd_ieee_id[0] = reg_val;
		reg_addr = HDMITX_DWC_FC_VSDIEEEID1;
		reg_val = hdmitx_rd_reg(reg_addr);
		vsd_ieee_id[1] = reg_val;
		reg_addr = HDMITX_DWC_FC_VSDIEEEID2;
		reg_val = hdmitx_rd_reg(reg_addr);
		vsd_ieee_id[2] = reg_val;

		/*hdmi 1.4b VSIF only Support DolbyVision-Std*/
		if (vsd_ieee_id[0] == 0x03 && vsd_ieee_id[1] == 0x0C &&
		    vsd_ieee_id[2] == 0x00) {
			pos += snprintf(buf + pos, PAGE_SIZE,
					"DolbyVision-Std_hdmi 1.4b VSIF");
		} else if ((vsd_ieee_id[0] == 0x46) &&
			   (vsd_ieee_id[1] == 0xD0) &&
			   (vsd_ieee_id[2] == 0x00)) {
			reg_addr = HDMITX_DWC_FC_AVICONF0;
			reg_val = hdmitx_rd_reg(reg_addr);

			if ((reg_val & 0x3) == 0) {
				/*RGB*/
				reg_addr = HDMITX_DWC_FC_AVICONF2;
				reg_val = hdmitx_rd_reg(reg_addr);
				if (((reg_val & 0xc) >> 2) == 2)/*FULL*/
					pos += snprintf(buf + pos, PAGE_SIZE,
									"DolbyVision-Std");
				else/*LIM*/
					pos += snprintf(buf + pos, PAGE_SIZE,
									"DolbyVision-Lowlatency");
			} else if ((reg_val & 0x3) == 1) {
				/*422*/
				reg_addr = HDMITX_DWC_FC_AVICONF3;
				reg_val = hdmitx_rd_reg(reg_addr);

				if (((reg_val & 0xc) >> 2) == 0)/*LIM*/
					pos += snprintf(buf + pos, PAGE_SIZE,
									"DolbyVision-Lowlatency");
				else/*FULL*/
					pos += snprintf(buf + pos, PAGE_SIZE,
									"DolbyVision-Std");
			} else if ((reg_val & 0x3) == 2) {
		/*444 only one probability: DolbyVision-Lowlatency*/
				pos += snprintf(buf + pos, PAGE_SIZE,
						"DolbyVision-Lowlatency");
			}
		} else {
			pos += snprintf(buf + pos, PAGE_SIZE, "SDR");
		}
	} else {
		reg_addr = HDMITX_DWC_FC_DRM_PB00;
		reg_val = hdmitx_rd_reg(reg_addr);

		switch (reg_val) {
		case 0:
			conf = "SDR";
			break;
		case 1:
			conf = "HDR10-others";
			break;
		case 2:
			conf = "HDR10-GAMMA_ST2084";
			break;
		case 3:
			conf = "HDR10-GAMMA_HLG";
			break;
		default:
			conf = "SDR";
		}
		pos += snprintf(buf + pos, PAGE_SIZE, "%s", conf);
	}
	return pos;
}

#undef pr_fmt
#define pr_fmt(fmt) "" fmt
static ssize_t hdmi_config_info_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	int pos = 0;
	enum hdmi_vic vic;

	pr_info("************");
	pr_info("hdmi_config_info");
	pr_info("************\n");
	pr_info("display_mode\nin:%s\t",
		get_vout_mode_internal());

	vic = hdmitx_device.hwop.getstate(&hdmitx_device, STAT_VIDEO_VIC, 0);
	pr_info("out:%s\n", hdmitx_edid_vic_tab_map_string(vic));

	pos = attr_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("attr\nin:%s\t", buf);

	pos = create_hdmitx_out_attr(buf);
	buf[pos] = '\0';
	pr_info("out:%s\n", buf);

	pos = hdmi_hdr_status_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("hdr_status\nin:%s\t", buf);

	pos = create_hdmitx_out_hdr(buf);
	buf[pos] = '\0';
	pr_info("out:%s\n", buf);

	pos = config_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("\n******config******\n");
	pr_info("%s\n", buf);

	pr_info("******hdcp******\n");
	pos = hdcp_mode_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("hdcp_mode:%s\n", buf);

	pos = hdcp_lstore_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("hdcp_lstore:%s\n", buf);

	pos = hdcp_ver_show(dev, attr, buf);
	buf[pos] = '\0';
	if (buf[0] == '2')
		pr_info("Sink support hdcp22: yes\n");
	else
		pr_info("Sink support hdcp22: no\n");

	pr_info("******scdc******\n");
	pos = div40_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("div40:%s\n", buf);

	pr_info("******hdmi_pll******\n");
	pos = sspll_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("sspll:%s\n", buf);

	pr_info("******dv_vsif_info******\n");
	print_vsif_config_data();
	strcpy(buf, "vsif_info");
	hdmitx_device.hwop.debugfun(&hdmitx_device, buf);

	pr_info("******drm_info******\n");
	print_drm_config_data();
	strcpy(buf, "drm_info");
	hdmitx_device.hwop.debugfun(&hdmitx_device, buf);

	pr_info("******hdr10p_vsif_info******\n");
	print_hdr10p_config_data();
	strcpy(buf, "vsif_info");
	hdmitx_device.hwop.debugfun(&hdmitx_device, buf);

	pr_info("******avi_info******\n");
	strcpy(buf, "avi_info");
	hdmitx_device.hwop.debugfun(&hdmitx_device, buf);

	pr_info("******gcp_info******\n");
	strcpy(buf, "gcp_info");
	hdmitx_device.hwop.debugfun(&hdmitx_device, buf);

	pr_info("******aud_info******\n");
	strcpy(buf, "aud_info");
	hdmitx_device.hwop.debugfun(&hdmitx_device, buf);

	pr_info("******acr_info******\n");
	strcpy(buf, "acr_info");
	hdmitx_device.hwop.debugfun(&hdmitx_device, buf);

	pr_info("******aud_sample******\n");
	strcpy(buf, "aud_sample");
	hdmitx_device.hwop.debugfun(&hdmitx_device, buf);

	pr_info("******aud_chls******\n");
	strcpy(buf, "aud_chls");
	hdmitx_device.hwop.debugfun(&hdmitx_device, buf);

	print_emp_config_data();

	memset(buf, 0, PAGE_SIZE);
	return 0;
}

#undef pr_fmt
#define pr_fmt(fmt) "hdmitx: " fmt
static ssize_t hdmirx_info_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int pos = 0;

	pr_info("************hdmirx_info************\n\n");

	pos = hpd_state_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("******hpd_edid_parsing******\n");
	pr_info("hpd:%s\t", buf);

	pos = edid_parsing_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("edid_parsing:%s\n", buf);

	pos = edid_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("******edid******\n");
	pr_info("%s\n", buf);

	pos = dc_cap_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("******dc_cap******\n%s\n", buf);

	pos = disp_cap_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("******disp_cap******\n%s\n", buf);

	pos = dv_cap_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("******dv_cap******\n%s\n", buf);

	pos = hdr_cap_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("******hdr_cap******\n%s\n", buf);

	pos = sink_type_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("******sink_type******\n%s\n", buf);

	pos = aud_cap_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("******aud_cap******\n%s\n", buf);

	pos = aud_ch_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("******aud_ch******\n%s\n", buf);

	pos = rawedid_show(dev, attr, buf);
	buf[pos] = '\0';
	pr_info("******rawedid******\n%s\n", buf);

	memset(buf, 0, PAGE_SIZE);
	return 0;
}

void print_hsty_drm_config_data(void)
{
	unsigned int hdr_transfer_feature;
	unsigned int hdr_color_feature;
	struct master_display_info_s *drmcfg;
	unsigned int colormetry;
	unsigned int hcnt, vcnt;
	unsigned int arr_cnt, pr_loc;

	pr_loc = hsty_drm_config_loc - 1;
	for (arr_cnt = 0;
		 arr_cnt < hsty_drm_config_num;
		 arr_cnt++)	{
		pr_info("***hsty_drm_config_data[%u]***\n", arr_cnt);
		drmcfg = &hsty_drm_config_data[pr_loc];
		hdr_transfer_feature = (drmcfg->features >> 8) & 0xff;
		hdr_color_feature = (drmcfg->features >> 16) & 0xff;
		colormetry = (drmcfg->features >> 30) & 0x1;
		pr_info("tf=%u, cf=%u, colormetry=%u\n",
			hdr_transfer_feature, hdr_color_feature,
			colormetry);

		pr_info("primaries:\n");
		for (vcnt = 0; vcnt < 3; vcnt++) {
			for (hcnt = 0; hcnt < 2; hcnt++)
				pr_info("%u, ", drmcfg->primaries[vcnt][hcnt]);
			pr_info("\n");
		}

		pr_info("white_point: ");
		for (hcnt = 0; hcnt < 2; hcnt++)
			pr_info("%u, ", drmcfg->white_point[hcnt]);
		pr_info("\n");

		pr_info("luminance: ");
		for (hcnt = 0; hcnt < 2; hcnt++)
			pr_info("%u, ", drmcfg->luminance[hcnt]);
		pr_info("\n");

		pr_info("max_content: %u, ", drmcfg->max_content);
		pr_info("max_frame_average: %u\n", drmcfg->max_frame_average);

		pr_loc = pr_loc > 0 ? pr_loc - 1 : 7;
	}
}

void print_hsty_vsif_config_data(void)
{
	struct dv_vsif_para *data;
	unsigned int arr_cnt, pr_loc;

	pr_loc = hsty_vsif_config_loc - 1;
	for (arr_cnt = 0; arr_cnt < hsty_vsif_config_num; arr_cnt++) {
		pr_info("***hsty_vsif_config_data[%u]***\n", arr_cnt);
		data = &hsty_vsif_config_data[pr_loc].data;
		pr_info("***vsif_config_data***\n");
		pr_info("type: %u, tunnel: %u, sigsdr: %u\n",
			hsty_vsif_config_data[pr_loc].type,
			hsty_vsif_config_data[pr_loc].tunnel_mode,
			hsty_vsif_config_data[pr_loc].signal_sdr);
		pr_info("dv_vsif_para:\n");
		pr_info("ver: %u len: %u\n",
			data->ver, data->length);
		pr_info("ll: %u dvsig: %u\n",
			data->vers.ver2.low_latency,
			data->vers.ver2.dobly_vision_signal);
		pr_info("bcMD: %u axMD: %u\n",
			data->vers.ver2.backlt_ctrl_MD_present,
			data->vers.ver2.auxiliary_MD_present);
		pr_info("PQhi: %u PQlow: %u\n",
			data->vers.ver2.eff_tmax_PQ_hi,
			data->vers.ver2.eff_tmax_PQ_low);
		pr_info("axrm: %u, axrv: %u, ",
			data->vers.ver2.auxiliary_runmode,
			data->vers.ver2.auxiliary_runversion);
		pr_info("axdbg: %u\n",
			data->vers.ver2.auxiliary_debug0);
		pr_loc = pr_loc > 0 ? pr_loc - 1 : 7;
	}
}

void print_hsty_hdr10p_config_data(void)
{
	struct hdr10plus_para *data;
	unsigned int arr_cnt, pr_loc;
	unsigned int hcnt, vcnt;
	unsigned char *tmp;

	pr_loc = hsty_hdr10p_config_loc - 1;
	for (arr_cnt = 0; arr_cnt < hsty_hdr10p_config_num; arr_cnt++) {
		pr_info("***hsty_hdr10p_config_data[%u]***\n", arr_cnt);
		data = &hsty_hdr10p_config_data[pr_loc];
		pr_info("appver: %u, tlum: %u, avgrgb: %u\n",
			data->application_version,
			data->targeted_max_lum,
			data->average_maxrgb);
		tmp = data->distribution_values;
		pr_info("distribution_values:\n");
		for (vcnt = 0; vcnt < 3; vcnt++) {
			for (hcnt = 0; hcnt < 3; hcnt++)
				pr_info("%u, ", tmp[vcnt * 3 + hcnt]);
			pr_info("\n");
		}
		pr_info("nbca: %u, knpx: %u, knpy: %u\n",
			data->num_bezier_curve_anchors,
			data->knee_point_x,
			data->knee_point_y);
		tmp = data->bezier_curve_anchors;
		pr_info("bezier_curve_anchors:\n");
		for (vcnt = 0; vcnt < 3; vcnt++) {
			for (hcnt = 0; hcnt < 3; hcnt++)
				pr_info("%u, ", tmp[vcnt * 3 + hcnt]);
			pr_info("\n");
		}
		pr_info("gof: %u, ndf: %u\n",
			data->graphics_overlay_flag,
			data->no_delay_flag);
		pr_loc = pr_loc > 0 ? pr_loc - 1 : 7;
	}
}

static ssize_t hdmi_hsty_config_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	print_hsty_drm_config_data();
	print_hsty_vsif_config_data();
	print_hsty_hdr10p_config_data();

	memset(buf, 0, PAGE_SIZE);
	return 0;
}

static DEVICE_ATTR_RW(disp_mode);
static DEVICE_ATTR_RW(attr);
static DEVICE_ATTR_RW(aud_mode);
static DEVICE_ATTR_RW(vid_mute);
static DEVICE_ATTR_RW(edid);
static DEVICE_ATTR_RO(rawedid);
static DEVICE_ATTR_RO(sink_type);
static DEVICE_ATTR_RO(edid_parsing);
static DEVICE_ATTR_RW(config);
static DEVICE_ATTR_WO(debug);
static DEVICE_ATTR_RO(disp_cap);
static DEVICE_ATTR_RO(preferred_mode);
static DEVICE_ATTR_RO(cea_cap);
static DEVICE_ATTR_RO(vesa_cap);
static DEVICE_ATTR_RO(aud_cap);
static DEVICE_ATTR_RO(hdmi_hdr_status);
static DEVICE_ATTR_RO(hdr_cap);
static DEVICE_ATTR_RO(dv_cap);
static DEVICE_ATTR_RO(dv_cap2);
static DEVICE_ATTR_RO(dc_cap);
static DEVICE_ATTR_RW(valid_mode);
static DEVICE_ATTR_RO(allm_cap);
static DEVICE_ATTR_RW(allm_mode);
static DEVICE_ATTR_RO(contenttype_cap);
static DEVICE_ATTR_RW(contenttype_mode);
static DEVICE_ATTR_RW(aud_ch);
static DEVICE_ATTR_RW(avmute);
static DEVICE_ATTR_RW(swap);
static DEVICE_ATTR_RW(vic);
static DEVICE_ATTR_RW(phy);
static DEVICE_ATTR_RW(sspll);
static DEVICE_ATTR_RW(frac_rate_policy);
static DEVICE_ATTR_RW(rxsense_policy);
static DEVICE_ATTR_RW(cedst_policy);
static DEVICE_ATTR_RO(cedst_count);
static DEVICE_ATTR_RW(hdcp_clkdis);
static DEVICE_ATTR_RW(hdcp_pwr);
static DEVICE_ATTR_WO(hdcp_byp);
static DEVICE_ATTR_RW(hdcp_mode);
static DEVICE_ATTR_RW(hdcp_lstore);
static DEVICE_ATTR_RW(hdcp_rptxlstore);
static DEVICE_ATTR_RW(hdcp_repeater);
static DEVICE_ATTR_RW(hdcp_topo_info);
static DEVICE_ATTR_RW(hdcp22_type);
static DEVICE_ATTR_RW(hdcp_stickmode);
static DEVICE_ATTR_RW(hdcp_stickstep);
static DEVICE_ATTR_RO(hdmi_repeater_tx);
static DEVICE_ATTR_RO(hdcp22_base);
static DEVICE_ATTR_RW(div40);
static DEVICE_ATTR_RW(hdcp_ctrl);
static DEVICE_ATTR_RO(disp_cap_3d);
static DEVICE_ATTR_RO(hdcp_ksv_info);
static DEVICE_ATTR_RO(hdcp_ver);
static DEVICE_ATTR_RO(hpd_state);
static DEVICE_ATTR_RO(rhpd_state);
static DEVICE_ATTR_RO(max_exceed);
static DEVICE_ATTR_RW(fake_plug);
static DEVICE_ATTR_RO(hdmi_init);
static DEVICE_ATTR_RW(ready);
static DEVICE_ATTR_RO(support_3d);
static DEVICE_ATTR_RO(hdmi_config_info);
static DEVICE_ATTR_RO(hdmirx_info);
static DEVICE_ATTR_RO(hdmi_hsty_config);

#ifdef CONFIG_AMLOGIC_VOUT_SERVE
static struct vinfo_s *hdmitx_vinfo;
static struct vinfo_s *hdmitx_get_current_vinfo(void)
{
	return hdmitx_vinfo;
}

/* fr_tab[]
 * 1080p24hz, 24:1
 * 1080p23.976hz, 2997:125
 * 25/50/100/200hz, no change
 */
static struct frac_rate_table fr_tab[] = {
	{"24hz", 24, 1, 2997, 125},
	{"30hz", 30, 1, 2997, 100},
	{"60hz", 60, 1, 2997, 50},
	{"120hz", 120, 1, 2997, 25},
	{"240hz", 120, 1, 5994, 25},
	{NULL},
};

static void recalc_vinfo_sync_duration(struct vinfo_s *info, unsigned int frac)
{
	struct frac_rate_table *fr = &fr_tab[0];

	pr_info(SYS "recalc before %s %d %d\n", info->name,
		info->sync_duration_num, info->sync_duration_den);

	while (fr->hz) {
		if (strstr(info->name, fr->hz)) {
			if (frac) {
				info->sync_duration_num = fr->sync_num_dec;
				info->sync_duration_den = fr->sync_den_dec;
			} else {
				info->sync_duration_num = fr->sync_num_int;
				info->sync_duration_den = fr->sync_den_int;
			}
			break;
		}
		fr++;
	}

	pr_info(SYS "recalc after %s %d %d\n", info->name,
		info->sync_duration_num, info->sync_duration_den);
}

static int hdmitx_set_current_vmode(enum vmode_e mode)
{
	struct vinfo_s *vinfo;

	pr_info("%s[%d]\n", __func__, __LINE__);
	/* get current vinfo and refesh */
	vinfo = hdmitx_get_current_vinfo();
	if (vinfo && vinfo->name)
		recalc_vinfo_sync_duration(vinfo,
					   hdmitx_device.frac_rate_policy);

	if (!(mode & VMODE_INIT_BIT_MASK))
		set_disp_mode_auto();
	else
		pr_info("alread display in uboot\n");

	return 0;
}

static enum vmode_e hdmitx_validate_vmode(char *mode)
{
	struct vinfo_s *info = hdmi_get_valid_vinfo(mode);

	if (info) {
		hdmitx_vinfo = info;
		hdmitx_vinfo->info_3d = NON_3D;
		if (hdmitx_device.flag_3dfp)
			hdmitx_vinfo->info_3d = FP_3D;

		if (hdmitx_device.flag_3dtb)
			hdmitx_vinfo->info_3d = TB_3D;

		if (hdmitx_device.flag_3dss)
			hdmitx_vinfo->info_3d = SS_3D;

		hdmitx_vinfo->vout_device = &hdmitx_vdev;
		return VMODE_HDMI;
	}
	return VMODE_MAX;
}

static int hdmitx_vmode_is_supported(enum vmode_e mode)
{
	if ((mode & VMODE_MODE_BIT_MASK) == VMODE_HDMI)
		return true;
	else
		return false;
}

static int hdmitx_module_disable(enum vmode_e cur_vmod)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	hdev->hwop.cntlconfig(hdev, CONF_CLR_AVI_PACKET, 0);
	hdev->hwop.cntlconfig(hdev, CONF_CLR_VSDB_PACKET, 0);
	hdev->hwop.cntlmisc(hdev, MISC_TMDS_PHY_OP, TMDS_PHY_DISABLE);
	if (hdev->para->hdmitx_vinfo.viu_mux == VIU_MUX_ENCI)
		hdmitx_disable_vclk2_enci(hdev);
	hdev->para = hdmi_get_fmt_name("invalid", hdev->fmt_attr);
	hdmitx_validate_vmode("null");
	if (hdev->cedst_policy)
		cancel_delayed_work(&hdev->work_cedst);
	if (hdev->rxsense_policy)
		queue_delayed_work(hdev->rxsense_wq, &hdev->work_rxsense, 0);

	return 0;
}

static int hdmitx_vout_state;
static int hdmitx_vout_set_state(int index)
{
	hdmitx_vout_state |= (1 << index);
	return 0;
}

static int hdmitx_vout_clr_state(int index)
{
	hdmitx_vout_state &= ~(1 << index);
	return 0;
}

static int hdmitx_vout_get_state(void)
{
	return hdmitx_vout_state;
}

static struct vout_server_s hdmitx_vout_server = {
	.name = "hdmitx_vout_server",
	.op = {
		.get_vinfo = hdmitx_get_current_vinfo,
		.set_vmode = hdmitx_set_current_vmode,
		.validate_vmode = hdmitx_validate_vmode,
		.vmode_is_supported = hdmitx_vmode_is_supported,
		.disable = hdmitx_module_disable,
		.set_state = hdmitx_vout_set_state,
		.clr_state = hdmitx_vout_clr_state,
		.get_state = hdmitx_vout_get_state,
		.set_bist = NULL,
#ifdef CONFIG_PM
		.vout_suspend = NULL,
		.vout_resume = NULL,
#endif
	},
};
#endif

#ifdef CONFIG_AMLOGIC_VOUT2_SERVE
static struct vout_server_s hdmitx_vout2_server = {
	.name = "hdmitx_vout2_server",
	.op = {
		.get_vinfo = hdmitx_get_current_vinfo,
		.set_vmode = hdmitx_set_current_vmode,
		.validate_vmode = hdmitx_validate_vmode,
		.vmode_is_supported = hdmitx_vmode_is_supported,
		.disable = hdmitx_module_disable,
		.set_state = hdmitx_vout_set_state,
		.clr_state = hdmitx_vout_clr_state,
		.get_state = hdmitx_vout_get_state,
		.set_bist = NULL,
#ifdef CONFIG_PM
		.vout_suspend = NULL,
		.vout_resume = NULL,
#endif
	},
};
#endif

#ifdef CONFIG_AMLOGIC_SND_SOC

#include <linux/soundcard.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>

static struct rate_map_fs map_fs[] = {
	{0,	  FS_REFER_TO_STREAM},
	{32000,  FS_32K},
	{44100,  FS_44K1},
	{48000,  FS_48K},
	{88200,  FS_88K2},
	{96000,  FS_96K},
	{176400, FS_176K4},
	{192000, FS_192K},
};

static enum hdmi_audio_fs aud_samp_rate_map(unsigned int rate)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(map_fs); i++) {
		if (map_fs[i].rate == rate)
			return map_fs[i].fs;
	}
	pr_info(AUD "get FS_MAX\n");
	return FS_MAX;
}

static unsigned char *aud_type_string[] = {
	"CT_REFER_TO_STREAM",
	"CT_PCM",
	"CT_AC_3",
	"CT_MPEG1",
	"CT_MP3",
	"CT_MPEG2",
	"CT_AAC",
	"CT_DTS",
	"CT_ATRAC",
	"CT_ONE_BIT_AUDIO",
	"CT_DOLBY_D",
	"CT_DTS_HD",
	"CT_MAT",
	"CT_DST",
	"CT_WMA",
	"CT_MAX",
};

static struct size_map aud_size_map_ss[] = {
	{0,	 SS_REFER_TO_STREAM},
	{16,	SS_16BITS},
	{20,	SS_20BITS},
	{24,	SS_24BITS},
	{32,	SS_MAX},
};

static enum hdmi_audio_sampsize aud_size_map(unsigned int bits)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(aud_size_map_ss); i++) {
		if (bits == aud_size_map_ss[i].sample_bits)
			return aud_size_map_ss[i].ss;
	}
	pr_info(AUD "get SS_MAX\n");
	return SS_MAX;
}

static int hdmitx_notify_callback_a(struct notifier_block *block,
				    unsigned long cmd, void *para);
static struct notifier_block hdmitx_notifier_nb_a = {
	.notifier_call	= hdmitx_notify_callback_a,
};

static int hdmitx_notify_callback_a(struct notifier_block *block,
				    unsigned long cmd, void *para)
{
	int i, audio_check = 0;
	struct rx_cap *prxcap = &hdmitx_device.rxcap;
	struct snd_pcm_substream *substream =
		(struct snd_pcm_substream *)para;
	struct hdmitx_audpara *audio_param =
		&hdmitx_device.cur_audio_param;
	enum hdmi_audio_fs n_rate = aud_samp_rate_map(substream->runtime->rate);
	enum hdmi_audio_sampsize n_size =
		aud_size_map(substream->runtime->sample_bits);

	hdmitx_device.audio_param_update_flag = 0;
	hdmitx_device.audio_notify_flag = 0;

	if (audio_param->sample_rate != n_rate) {
		audio_param->sample_rate = n_rate;
		hdmitx_device.audio_param_update_flag = 1;
	}

	if (audio_param->type != cmd) {
		audio_param->type = cmd;
	pr_info(AUD "aout notify format %s\n",
		aud_type_string[audio_param->type & 0xff]);
	hdmitx_device.audio_param_update_flag = 1;
	}

	if (audio_param->sample_size != n_size) {
		audio_param->sample_size = n_size;
		hdmitx_device.audio_param_update_flag = 1;
	}

	if (audio_param->channel_num !=
		(substream->runtime->channels - 1)) {
		audio_param->channel_num =
		substream->runtime->channels - 1;
		hdmitx_device.audio_param_update_flag = 1;
	}
	if (hdmitx_device.tx_aud_cfg == 2) {
		pr_info(AUD "auto mode\n");
	/* Detect whether Rx is support current audio format */
	for (i = 0; i < prxcap->AUD_count; i++) {
		if (prxcap->RxAudioCap[i].audio_format_code == cmd)
			audio_check = 1;
	}
	/* sink don't support current audio mode */
	if (!audio_check && cmd != CT_PCM) {
		pr_info("Sink not support this audio format %lu\n",
			cmd);
		hdmitx_device.hwop.cntlconfig(&hdmitx_device,
			CONF_AUDIO_MUTE_OP, AUDIO_MUTE);
		hdmitx_device.audio_param_update_flag = 0;
	}
	}
	if (hdmitx_device.audio_param_update_flag == 0)
		;
	else
		hdmitx_device.audio_notify_flag = 1;

	if ((!(hdmitx_device.hdmi_audio_off_flag)) &&
	    hdmitx_device.audio_param_update_flag) {
		/* plug-in & update audio param */
		if (hdmitx_device.hpd_state == 1) {
			hdmitx_set_audio(&hdmitx_device,
					 &hdmitx_device.cur_audio_param);
		if (hdmitx_device.audio_notify_flag == 1 ||
		    hdmitx_device.audio_step == 1) {
			hdmitx_device.audio_notify_flag = 0;
			hdmitx_device.audio_step = 0;
		}
		hdmitx_device.audio_param_update_flag = 0;
		pr_info(AUD "set audio param\n");
	}
	}

	return 0;
}

#endif
unsigned int hdmitx_check_edid_all_zeros(unsigned char *buf)
{
	unsigned int i = 0, j = 0;
	unsigned int chksum = 0;

	for (j = 0; j < EDID_MAX_BLOCK; j++) {
		chksum = 0;
		for (i = 0; i < 128; i++)
			chksum += buf[i + j * 128];
		if (chksum != 0)
			return 0;
	}
	return 1;
}

static void hdmitx_get_edid(struct hdmitx_dev *hdev)
{
	mutex_lock(&getedid_mutex);
	/* TODO hdmitx_edid_ram_buffer_clear(hdev); */
	hdev->hwop.cntlddc(hdev, DDC_RESET_EDID, 0);
	hdev->hwop.cntlddc(hdev, DDC_PIN_MUX_OP, PIN_MUX);
	/* start reading edid frist time */
	hdev->hwop.cntlddc(hdev, DDC_EDID_READ_DATA, 0);
	hdev->hwop.cntlddc(hdev, DDC_EDID_GET_DATA, 0);
	if (hdmitx_check_edid_all_zeros(hdev->EDID_buf)) {
		hdev->hwop.cntlddc(hdev, DDC_GLITCH_FILTER_RESET, 0);
		hdev->hwop.cntlddc(hdev, DDC_EDID_READ_DATA, 0);
		hdev->hwop.cntlddc(hdev, DDC_EDID_GET_DATA, 0);
	}
	/* If EDID is not correct at first time, then retry */
	if (!check_dvi_hdmi_edid_valid(hdev->EDID_buf)) {
		msleep(100);
		/* start reading edid second time */
		hdev->hwop.cntlddc(hdev, DDC_EDID_READ_DATA, 0);
		hdev->hwop.cntlddc(hdev, DDC_EDID_GET_DATA, 1);
		if (hdmitx_check_edid_all_zeros(hdev->EDID_buf1)) {
			hdev->hwop.cntlddc(hdev, DDC_GLITCH_FILTER_RESET, 0);
			hdev->hwop.cntlddc(hdev, DDC_EDID_READ_DATA, 0);
			hdev->hwop.cntlddc(hdev, DDC_EDID_GET_DATA, 1);
		}
	}
	hdmitx_edid_clear(hdev);
	hdmitx_edid_parse(hdev);
	hdmitx_edid_buf_compare_print(hdev);

	if (hdev->hdr_priority) { /* clear dv_info */
		struct dv_info *dv = &hdev->rxcap.dv_info;

		memset(dv, 0, sizeof(struct dv_info));
		pr_info("clear dv_info\n");
	}
	mutex_unlock(&getedid_mutex);
}

static void hdmitx_rxsense_process(struct work_struct *work)
{
	int sense;
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_rxsense);

	sense = hdev->hwop.cntlmisc(hdev, MISC_TMDS_RXSENSE, 0);
	hdmitx_status.rxsense = sense;
	extcon_set_state_sync(hdmitx_extcon_rxsense, EXTCON_DISP_HDMI, sense);
	queue_delayed_work(hdev->rxsense_wq, &hdev->work_rxsense, HZ);
}

static void hdmitx_cedst_process(struct work_struct *work)
{
	int ced;
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_cedst);

	ced = hdev->hwop.cntlmisc(hdev, MISC_TMDS_CEDST, 0);
	/* firstly send as 0, then real ced, A trigger signal */
	hdmitx_status.cedst = ced;
	extcon_set_state_sync(hdmitx_extcon_cedst, EXTCON_DISP_HDMI, 0);
	extcon_set_state_sync(hdmitx_extcon_cedst, EXTCON_DISP_HDMI, ced);
	queue_delayed_work(hdev->cedst_wq, &hdev->work_cedst, HZ);
}

static void hdmitx_hpd_plugin_handler(struct work_struct *work)
{
	char bksv_buf[5];
	struct vinfo_s *info = NULL;
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_hpd_plugin);

	mutex_lock(&setclk_mutex);
	if (!(hdev->hdmitx_event & (HDMI_TX_HPD_PLUGIN))) {
		mutex_unlock(&setclk_mutex);
		return;
	}
	if (hdev->rxsense_policy) {
		cancel_delayed_work(&hdev->work_rxsense);
		queue_delayed_work(hdev->rxsense_wq, &hdev->work_rxsense, 0);
	}
	pr_info(SYS "plugin\n");
	if (hdev->data->chip_type >= MESON_CPU_ID_G12A)
		hdev->hwop.cntlmisc(hdev, MISC_I2C_RESET, 0);
	hdev->hdmitx_event &= ~HDMI_TX_HPD_PLUGIN;
	/* start reading E-EDID */
	if (hdev->repeater_tx)
		rx_repeat_hpd_state(1);
	hdmitx_get_edid(hdev);
	hdev->cedst_policy = hdev->cedst_en & hdev->rxcap.scdc_present;
	hdmi_physcial_size_update(hdev);
	if (hdev->rxcap.ieeeoui != HDMI_IEEEOUI)
		hdev->hwop.cntlconfig(hdev,
			CONF_HDMI_DVI_MODE, DVI_MODE);
	else
		hdev->hwop.cntlconfig(hdev,
			CONF_HDMI_DVI_MODE, HDMI_MODE);
	mutex_lock(&getedid_mutex);
	if (hdev->data->chip_type < MESON_CPU_ID_G12A)
		hdev->hwop.cntlmisc(hdev, MISC_I2C_REACTIVE, 0);
	mutex_unlock(&getedid_mutex);
	if (hdev->repeater_tx) {
		if (check_fbc_special(&hdev->EDID_buf[0]) ||
		    check_fbc_special(&hdev->EDID_buf1[0]))
			rx_set_repeater_support(0);
		else
			rx_set_repeater_support(1);
		hdev->hwop.cntlddc(hdev, DDC_HDCP_GET_BKSV,
			(unsigned long)bksv_buf);
		rx_set_receive_hdcp(bksv_buf, 1, 1, 0, 0);
	}

	info = hdmitx_get_current_vinfo();
	if (info && info->mode == VMODE_HDMI)
		hdmitx_set_audio(hdev, &hdev->cur_audio_param);
	hdev->hpd_state = 1;
	hdmitx_notify_hpd(hdev->hpd_state);

	hdmitx_status.hpd_state = 1;
	hdmitx_status.audio = 1;
	extcon_set_state_sync(hdmitx_extcon_hdmi, EXTCON_DISP_HDMI, 1);
	extcon_set_state_sync(hdmitx_extcon_audio, EXTCON_DISP_HDMI, 1);
	/* Should be started at end of output */
	cancel_delayed_work(&hdev->work_cedst);
	if (hdev->cedst_policy)
		queue_delayed_work(hdev->cedst_wq, &hdev->work_cedst, 0);
	mutex_unlock(&setclk_mutex);
}

static void clear_rx_vinfo(struct hdmitx_dev *hdev)
{
	struct vinfo_s *info = hdmitx_get_current_vinfo();

	if (info) {
		memset(&info->hdr_info, 0, sizeof(info->hdr_info));
		memset(&info->rx_latency, 0, sizeof(info->rx_latency));
	}
}

static void hdmitx_aud_hpd_plug_handler(struct work_struct *work)
{
	int st;
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_aud_hpd_plug);

	st = hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0);
	pr_info("%s state:%d\n", __func__, st);
	hdmitx_status.audio = st;
	extcon_set_state_sync(hdmitx_extcon_audio, EXTCON_DISP_HDMI, st);
}

static void hdmitx_hpd_plugout_handler(struct work_struct *work)
{
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_hpd_plugout);

	mutex_lock(&setclk_mutex);
	if (!(hdev->hdmitx_event & (HDMI_TX_HPD_PLUGOUT))) {
		mutex_unlock(&setclk_mutex);
		return;
	}
	hdev->hdcp_mode = 0;
	hdev->hdcp_bcaps_repeater = 0;
	hdev->hwop.cntlddc(hdev, DDC_HDCP_MUX_INIT, 1);
	hdev->hwop.cntlddc(hdev, DDC_HDCP_OP, HDCP14_OFF);
	if (hdev->cedst_policy)
		cancel_delayed_work(&hdev->work_cedst);
	edidinfo_detach_to_vinfo(hdev);
	pr_info(SYS "plugout\n");
	if (!!(hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0))) {
		pr_info(SYS "hpd gpio high\n");
		hdev->hdmitx_event &= ~HDMI_TX_HPD_PLUGOUT;
		mutex_unlock(&setclk_mutex);
		return;
	}
	/*after plugout, DV mode can't be supported*/
	hdmitx_set_vsif_pkt(0, 0, NULL, true);
	hdmitx_set_hdr10plus_pkt(0, NULL);
	hdev->ready = 0;
	if (hdev->repeater_tx)
		rx_repeat_hpd_state(0);
	hdev->hwop.cntlconfig(hdev, CONF_CLR_AVI_PACKET, 0);
	hdev->hwop.cntlddc(hdev, DDC_HDCP_MUX_INIT, 1);
	hdev->hwop.cntlddc(hdev, DDC_HDCP_OP, HDCP14_OFF);
	hdev->hwop.cntlddc(hdev, DDC_HDCP_SET_TOPO_INFO, 0);
	hdev->hwop.cntlmisc(hdev, MISC_TMDS_PHY_OP, TMDS_PHY_DISABLE);
	hdev->hdmitx_event &= ~HDMI_TX_HPD_PLUGOUT;
	hdev->hwop.cntlmisc(hdev, MISC_ESM_RESET, 0);
	clear_rx_vinfo(hdev);
	rx_edid_physical_addr(0, 0, 0, 0);
	hdmitx_edid_clear(hdev);
	hdmi_physcial_size_update(hdev);
	hdmitx_edid_ram_buffer_clear(hdev);
	hdev->hpd_state = 0;
	hdmitx_notify_hpd(hdev->hpd_state);
	hdmitx_status.hpd_state = 0;
	hdmitx_status.audio = 0;
	extcon_set_state_sync(hdmitx_extcon_hdmi, EXTCON_DISP_HDMI, 0);
	extcon_set_state_sync(hdmitx_extcon_audio, EXTCON_DISP_HDMI, 0);
	mutex_unlock(&setclk_mutex);
}

static void hdmitx_internal_intr_handler(struct work_struct *work)
{
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_internal_intr);

	hdev->hwop.debugfun(hdev, "dumpintr");
}

int get_hpd_state(void)
{
	int ret;

	mutex_lock(&setclk_mutex);
	ret = hdmitx_device.hpd_state;
	mutex_unlock(&setclk_mutex);

	return ret;
}
EXPORT_SYMBOL(get_hpd_state);

/******************************
 *  hdmitx kernel task
 *******************************/
int tv_audio_support(int type, struct rx_cap *prxcap)
{
	int i, audio_check = 0;

	for (i = 0; i < prxcap->AUD_count; i++) {
		if (prxcap->RxAudioCap[i].audio_format_code == type)
			audio_check = 1;
	}
	return audio_check;
}

/*****************************
 *	hdmitx driver file_operations
 *
 ******************************/
static int amhdmitx_open(struct inode *node, struct file *file)
{
	struct hdmitx_dev *hdmitx_in_devp;

	/* Get the per-device structure that contains this cdev */
	hdmitx_in_devp = container_of(node->i_cdev, struct hdmitx_dev, cdev);
	file->private_data = hdmitx_in_devp;

	return 0;
}

static int amhdmitx_release(struct inode *node, struct file *file)
{
	return 0;
}

static const struct file_operations amhdmitx_fops = {
	.owner	= THIS_MODULE,
	.open	 = amhdmitx_open,
	.release  = amhdmitx_release,
};

struct hdmitx_dev *get_hdmitx_device(void)
{
	return &hdmitx_device;
}
EXPORT_SYMBOL(get_hdmitx_device);

static int get_dt_vend_init_data(struct device_node *np,
				 struct vendor_info_data *vend)
{
	int ret;

	ret = of_property_read_string(np, "vendor_name",
				      (const char **)&vend->vendor_name);
	if (ret)
		pr_info(SYS "not find vendor name\n");

	ret = of_property_read_u32(np, "vendor_id", &vend->vendor_id);
	if (ret)
		pr_info(SYS "not find vendor id\n");

	ret = of_property_read_string(np, "product_desc",
				      (const char **)&vend->product_desc);
	if (ret)
		pr_info(SYS "not find product desc\n");
	return 0;
}

static void hdmitx_fmt_attr(struct hdmitx_dev *hdev)
{
	if (strlen(hdev->fmt_attr) >= 8) {
		pr_info(SYS "fmt_attr %s\n", hdev->fmt_attr);
		return;
	}
	if (hdev->para->cd == COLORDEPTH_RESERVED &&
	    hdev->para->cs == COLORSPACE_RESERVED) {
		strcpy(hdev->fmt_attr, "default");
	} else {
		memset(hdev->fmt_attr, 0, sizeof(hdev->fmt_attr));
		switch (hdev->para->cs) {
		case COLORSPACE_RGB444:
			memcpy(hdev->fmt_attr, "rgb,", 5);
			break;
		case COLORSPACE_YUV422:
			memcpy(hdev->fmt_attr, "422,", 5);
			break;
		case COLORSPACE_YUV444:
			memcpy(hdev->fmt_attr, "444,", 5);
			break;
		case COLORSPACE_YUV420:
			memcpy(hdev->fmt_attr, "420,", 5);
			break;
		default:
			break;
		}
		switch (hdev->para->cd) {
		case COLORDEPTH_24B:
			strcat(hdev->fmt_attr, "8bit");
			break;
		case COLORDEPTH_30B:
			strcat(hdev->fmt_attr, "10bit");
			break;
		case COLORDEPTH_36B:
			strcat(hdev->fmt_attr, "12bit");
			break;
		case COLORDEPTH_48B:
			strcat(hdev->fmt_attr, "16bit");
			break;
		default:
			break;
		}
	}
	pr_info(SYS "fmt_attr %s\n", hdev->fmt_attr);
}

static void hdmitx_init_fmt_attr(struct hdmitx_dev *hdev)
{
	if (strlen(hdev->fmt_attr) >= 8) {
		pr_info(SYS "fmt_attr %s\n", hdev->fmt_attr);
		return;
	}
	if (hdev->para->cd == COLORDEPTH_RESERVED &&
	    hdev->para->cs == COLORSPACE_RESERVED) {
		strcpy(hdev->fmt_attr, "default");
	} else {
		memset(hdev->fmt_attr, 0, sizeof(hdev->fmt_attr));
		switch (hdev->para->cs) {
		case COLORSPACE_RGB444:
			memcpy(hdev->fmt_attr, "rgb,", 5);
			break;
		case COLORSPACE_YUV422:
			memcpy(hdev->fmt_attr, "422,", 5);
			break;
		case COLORSPACE_YUV444:
			memcpy(hdev->fmt_attr, "444,", 5);
			break;
		case COLORSPACE_YUV420:
			memcpy(hdev->fmt_attr, "420,", 5);
			break;
		default:
			break;
		}
		switch (hdev->para->cd) {
		case COLORDEPTH_24B:
			strcat(hdev->fmt_attr, "8bit");
			break;
		case COLORDEPTH_30B:
			strcat(hdev->fmt_attr, "10bit");
			break;
		case COLORDEPTH_36B:
			strcat(hdev->fmt_attr, "12bit");
			break;
		case COLORDEPTH_48B:
			strcat(hdev->fmt_attr, "16bit");
			break;
		default:
			break;
		}
	}
	pr_info(SYS "fmt_attr %s\n", hdev->fmt_attr);
}

/* for notify to cec */
static BLOCKING_NOTIFIER_HEAD(hdmitx_event_notify_list);
int hdmitx_event_notifier_regist(struct notifier_block *nb)
{
	int ret = 0;

	if (!nb)
		return ret;

	ret = blocking_notifier_chain_register(&hdmitx_event_notify_list, nb);
	/* update status when register */
	if (!ret && nb->notifier_call) {
		hdmitx_notify_hpd(hdmitx_device.hpd_state);
		if (hdmitx_device.physical_addr != 0xffff)
			hdmitx_event_notify(HDMITX_PHY_ADDR_VALID,
					    &hdmitx_device.physical_addr);
	}

	return ret;
}
EXPORT_SYMBOL(hdmitx_event_notifier_regist);

int hdmitx_event_notifier_unregist(struct notifier_block *nb)
{
	int ret;

	ret = blocking_notifier_chain_unregister(&hdmitx_event_notify_list, nb);

	return ret;
}
EXPORT_SYMBOL(hdmitx_event_notifier_unregist);

void hdmitx_event_notify(unsigned long state, void *arg)
{
	blocking_notifier_call_chain(&hdmitx_event_notify_list, state, arg);
}

void hdmitx_hdcp_status(int hdmi_authenticated)
{
	hdmitx_status.hdcp = hdmi_authenticated;
	extcon_set_state_sync(hdmitx_extcon_hdcp, EXTCON_DISP_HDMI,
			      hdmi_authenticated);
}

void hdmitx_extcon_register(struct platform_device *pdev, struct device *dev)
{
	struct extcon_dev *edev;
	int ret;

	/*hdmitx extcon hdmi*/
	edev = devm_extcon_dev_allocate(&pdev->dev, hdmi_cable);
	if (IS_ERR(edev)) {
		pr_info(SYS "failed to allocate hdmitx extcon hdmi\n");
		return;
	}
	edev->dev.parent = dev;
	edev->name = "hdmitx_extcon_hdmi";
	dev_set_name(&edev->dev, "hdmi");
	ret = extcon_dev_register(edev);
	if (ret < 0) {
		pr_info(SYS "failed to register hdmitx extcon hdmi\n");
		extcon_dev_free(edev);
		return;
	}
	hdmitx_extcon_hdmi = edev;

	/*hdmitx extcon audio*/
	edev = devm_extcon_dev_allocate(&pdev->dev, hdmi_cable);
	if (IS_ERR(edev)) {
		pr_info(SYS "failed to allocate hdmitx extcon audio\n");
		return;
	}

	edev->dev.parent = dev;
	edev->name = "hdmitx_extcon_audio";
	dev_set_name(&edev->dev, "hdmi_audio");
	ret = extcon_dev_register(edev);
	if (ret < 0) {
		pr_info(SYS "failed to register hdmitx extcon audio\n");
		extcon_dev_free(edev);
		return;
	}
	hdmitx_extcon_audio = edev;

	/*hdmitx extcon power*/
	edev = devm_extcon_dev_allocate(&pdev->dev, hdmi_cable);
	if (IS_ERR(edev)) {
		pr_info(SYS "failed to allocate hdmitx extcon power\n");
		return;
	}

	edev->dev.parent = dev;
	edev->name = "hdmitx_extcon_power";
	dev_set_name(&edev->dev, "hdmi_power");
	ret = extcon_dev_register(edev);
	if (ret < 0) {
		pr_info(SYS "failed to register extcon power\n");
		extcon_dev_free(edev);
		return;
	}
	hdmitx_extcon_power = edev;

	/*hdmitx extcon hdr*/
	edev = devm_extcon_dev_allocate(&pdev->dev, hdmi_cable);
	if (IS_ERR(edev)) {
		pr_info(SYS "failed to allocate hdmitx extcon hdr\n");
		return;
	}

	edev->dev.parent = dev;
	edev->name = "hdmitx_extcon_hdr";
	dev_set_name(&edev->dev, "hdmi_hdr");
	ret = extcon_dev_register(edev);
	if (ret < 0) {
		pr_info(SYS "failed to register hdmitx extcon hdr\n");
		extcon_dev_free(edev);
		return;
	}
	hdmitx_extcon_hdr = edev;

	/*hdmitx extcon CED */
	edev = devm_extcon_dev_allocate(&pdev->dev, hdmi_cable);
	if (IS_ERR(edev)) {
		pr_info(SYS "failed to allocate extcon cedst\n");
		return;
	}
	edev->dev.parent = dev;
	edev->name = "hdmitx_extcon_cedst";
	dev_set_name(&edev->dev, "hdmi_cedst");
	ret = extcon_dev_register(edev);
	if (ret < 0) {
		pr_info(SYS "failed to register extcon cedst\n");
		extcon_dev_free(edev);
		return;
	}
	hdmitx_extcon_cedst = edev;

	/*hdmitx extcon rxsense*/
	edev = devm_extcon_dev_allocate(&pdev->dev, hdmi_cable);
	if (IS_ERR(edev)) {
		pr_info(SYS "failed to allocate extcon rxsense\n");
		return;
	}

	edev->dev.parent = dev;
	edev->name = "hdmitx_extcon_rxsense";
	dev_set_name(&edev->dev, "hdmi_rxsense");
	ret = extcon_dev_register(edev);
	if (ret < 0) {
		pr_info(SYS "failed to register extcon rxsense\n");
		return;
	}
	hdmitx_extcon_rxsense = edev;

	/*hdmitx extcon hdcp*/
	edev = devm_extcon_dev_allocate(&pdev->dev, hdmi_cable);
	if (IS_ERR(edev)) {
		pr_info(SYS "failed to allocate extcon hdcp\n");
		return;
	}

	edev->dev.parent = dev;
	edev->name = "hdmitx_extcon_hdcp";
	dev_set_name(&edev->dev, "hdcp");
	ret = extcon_dev_register(edev);
	if (ret < 0) {
		pr_info(SYS "failed to register extcon hdcp\n");
		extcon_dev_free(edev);
		return;
	}
	hdmitx_extcon_hdcp = edev;
}

static void hdmitx_init_parameters(struct hdmitx_info *info)
{
	memset(info, 0, sizeof(struct hdmitx_info));

	info->video_out_changing_flag = 1;

	info->audio_flag = 1;
	info->audio_info.type = CT_REFER_TO_STREAM;
	info->audio_info.format = AF_I2S;
	info->audio_info.fs = FS_44K1;
	info->audio_info.ss = SS_16BITS;
	info->audio_info.channels = CC_2CH;
	info->audio_out_changing_flag = 1;

	info->auto_hdcp_ri_flag = 1;
	info->hw_sha_calculator_flag = 1;
}

static int amhdmitx_device_init(struct hdmitx_dev *hdmi_dev)
{
	if (!hdmi_dev)
		return 1;

	pr_info(SYS "Ver: %s\n", HDMITX_VER);

	hdmi_dev->hdtx_dev = NULL;

	hdmitx_device.physical_addr = 0xffff;
	/* init para for NULL protection */
	hdmitx_device.para = hdmi_get_fmt_name("invalid",
					       hdmitx_device.fmt_attr);
	hdmitx_device.hdmi_last_hdr_mode = 0;
	hdmitx_device.hdmi_current_hdr_mode = 0;
	hdmitx_device.unplug_powerdown = 0;
	hdmitx_device.vic_count = 0;
	hdmitx_device.auth_process_timer = 0;
	hdmitx_device.force_audio_flag = 0;
	hdmitx_device.hdcp_mode = 0;
	hdmitx_device.ready = 0;
	hdmitx_device.rxsense_policy = 0; /* no RxSense by default */
	/* enable or disable HDMITX SSPLL, enable by default */
	hdmitx_device.sspll = 1;
	/*
	 * 0, do not unmux hpd when off or unplug ;
	 * 1, unmux hpd when unplug;
	 * 2, unmux hpd when unplug  or off;
	 */
	hdmitx_device.hpdmode = 1;

	hdmitx_device.flag_3dfp = 0;
	hdmitx_device.flag_3dss = 0;
	hdmitx_device.flag_3dtb = 0;

	if ((init_flag & INIT_FLAG_POWERDOWN) &&
	    hdmitx_device.hpdmode == 2)
		hdmitx_device.mux_hpd_if_pin_high_flag = 0;
	else
		hdmitx_device.mux_hpd_if_pin_high_flag = 1;

	hdmitx_device.audio_param_update_flag = 0;
	/* 1: 2ch */
	hdmitx_device.hdmi_ch = 1;
	/* default audio configure is on */
	hdmitx_device.tx_aud_cfg = 1;
	hdmitx_device.topo_info =
		kmalloc(sizeof(struct hdcprp_topo), GFP_KERNEL);
	if (!hdmitx_device.topo_info)
		pr_info("failed to alloc hdcp topo info\n");
	hdmitx_init_parameters(&hdmitx_device.hdmi_info);

	return 0;
}

static void amhdmitx_get_drm_info(void)
{
	struct device_node *drm_node;
	unsigned char *drm_status;
	int ret = 0;

	drm_node = of_find_node_by_path("/drm-amhdmitx");
	if (drm_node) {
		ret =
		of_property_read_string(drm_node, "status",
					(const char **)&drm_status);
		if (ret) {
			pr_info(SYS "not find drm_feature\n");
		} else {
			if (memcmp(drm_status, "okay", 4) == 0)
				hdmitx_device.drm_feature = 1;
			else
				hdmitx_device.drm_feature = 0;
			pr_info(SYS "hdmitx_device.drm_feature : %d\n",
				hdmitx_device.drm_feature);
		}
	} else {
		pr_info(SYS "not find drm_amhdmitx\n");
	}
}

static int amhdmitx_get_dt_info(struct platform_device *pdev)
{
	int ret = 0;

#ifdef CONFIG_OF
	int val;
	phandle phandle;
	struct device_node *init_data;
	const struct of_device_id *match;
#endif

	/* HDMITX pinctrl config for hdp and ddc*/
	if (pdev->dev.pins) {
		hdmitx_device.pdev = &pdev->dev;

		hdmitx_device.pinctrl_default =
			pinctrl_lookup_state(pdev->dev.pins->p, "default");
		if (IS_ERR(hdmitx_device.pinctrl_default))
			pr_info(SYS "no default of pinctrl state\n");

		hdmitx_device.pinctrl_i2c =
			pinctrl_lookup_state(pdev->dev.pins->p, "hdmitx_i2c");
		if (IS_ERR(hdmitx_device.pinctrl_i2c))
			pr_info(SYS "no hdmitx_i2c of pinctrl state\n");

		pinctrl_select_state(pdev->dev.pins->p,
				     hdmitx_device.pinctrl_default);
	}

#ifdef CONFIG_OF
	if (pdev->dev.of_node) {
		int dongle_mode = 0;

		memset(&hdmitx_device.config_data, 0,
		       sizeof(struct hdmi_config_platform_data));
		/* Get chip type and name information */
		match = of_match_device(meson_amhdmitx_of_match, &pdev->dev);
		if (!match) {
			pr_info("%s: no match table\n", __func__);
			return -1;
		}
		hdmitx_device.data = (struct amhdmitx_data_s *)match->data;

		pr_info(SYS "chip_type:%d chip_name:%s\n",
			hdmitx_device.data->chip_type,
			hdmitx_device.data->chip_name);

		/* Get dongle_mode information */
		ret = of_property_read_u32(pdev->dev.of_node, "dongle_mode",
					   &dongle_mode);
		hdmitx_device.dongle_mode = !!dongle_mode;
		if (!ret)
			pr_info(SYS "hdmitx_device.dongle_mode: %d\n",
				hdmitx_device.dongle_mode);
		/* Get repeater_tx information */
		ret = of_property_read_u32(pdev->dev.of_node,
					   "repeater_tx", &val);
		if (!ret)
			hdmitx_device.repeater_tx = val;
		if (hdmitx_device.repeater_tx == 1)
			hdmitx_device.topo_info =
			kzalloc(sizeof(*hdmitx_device.topo_info), GFP_KERNEL);

		ret = of_property_read_u32(pdev->dev.of_node,
					   "cedst_en", &val);
		if (!ret)
			hdmitx_device.cedst_en = !!val;

		/* Get vendor information */
		ret = of_property_read_u32(pdev->dev.of_node,
					   "vend-data", &val);
		if (ret)
			pr_info(SYS "not find match init-data\n");
		if (ret == 0) {
			phandle = val;
			init_data = of_find_node_by_phandle(phandle);
			if (!init_data)
				pr_info(SYS "not find device node\n");
			hdmitx_device.config_data.vend_data =
			kzalloc(sizeof(struct vendor_info_data), GFP_KERNEL);
			if (!(hdmitx_device.config_data.vend_data))
				pr_info(SYS "not allocate memory\n");
			ret = get_dt_vend_init_data
			(init_data, hdmitx_device.config_data.vend_data);
			if (ret)
				pr_info(SYS "not find vend_init_data\n");
		}
		/* Get power control */
		ret = of_property_read_u32(pdev->dev.of_node,
					   "pwr-ctrl", &val);
		if (ret)
			pr_info(SYS "not find match pwr-ctl\n");
		if (ret == 0) {
			phandle = val;
			init_data = of_find_node_by_phandle(phandle);
			if (!init_data)
				pr_info(SYS "not find device node\n");
			hdmitx_device.config_data.pwr_ctl =
			kzalloc((sizeof(struct hdmi_pwr_ctl)) *
			HDMI_TX_PWR_CTRL_NUM, GFP_KERNEL);
			if (!hdmitx_device.config_data.pwr_ctl)
				pr_info(SYS "can not get pwr_ctl mem\n");
			memset(hdmitx_device.config_data.pwr_ctl, 0,
			       sizeof(struct hdmi_pwr_ctl));
			if (ret)
				pr_info(SYS "not find pwr_ctl\n");
		}

		/* Get drm feature information */
		amhdmitx_get_drm_info();

		/* Get reg information */
		ret = hdmitx_init_reg_map(pdev);
	}

#else
		hdmi_pdata = pdev->dev.platform_data;
		if (!hdmi_pdata) {
			pr_info(SYS "not get platform data\n");
			r = -ENOENT;
		} else {
			pr_info(SYS "get hdmi platform data\n");
		}
#endif
	hdmitx_device.irq_hpd = platform_get_irq_byname(pdev, "hdmitx_hpd");
	if (hdmitx_device.irq_hpd == -ENXIO) {
		pr_err("%s: ERROR: hdmitx hpd irq No not found\n",
		       __func__);
			return -ENXIO;
	}
	pr_info(SYS "hpd irq = %d\n", hdmitx_device.irq_hpd);

	return ret;
}

/*
 * amhdmitx_clktree_probe
 * get clktree info from dts
 */
static void amhdmitx_clktree_probe(struct device *hdmitx_dev)
{
	struct clk *hdmi_clk_vapb, *hdmi_clk_vpu;
	struct clk *hdcp22_tx_skp, *hdcp22_tx_esm;
	struct clk *venci_top_gate, *venci_0_gate, *venci_1_gate;

	hdmi_clk_vapb = devm_clk_get(hdmitx_dev, "hdmi_vapb_clk");
	if (IS_ERR(hdmi_clk_vapb)) {
		pr_warn(SYS "vapb_clk failed to probe\n");
	} else {
		hdmitx_device.hdmitx_clk_tree.hdmi_clk_vapb = hdmi_clk_vapb;
		clk_prepare_enable(hdmitx_device.hdmitx_clk_tree.hdmi_clk_vapb);
	}

	hdmi_clk_vpu = devm_clk_get(hdmitx_dev, "hdmi_vpu_clk");
	if (IS_ERR(hdmi_clk_vpu)) {
		pr_warn(SYS "vpu_clk failed to probe\n");
	} else {
		hdmitx_device.hdmitx_clk_tree.hdmi_clk_vpu = hdmi_clk_vpu;
		clk_prepare_enable(hdmitx_device.hdmitx_clk_tree.hdmi_clk_vpu);
	}

	hdcp22_tx_skp = devm_clk_get(hdmitx_dev, "hdcp22_tx_skp");
	if (IS_ERR(hdcp22_tx_skp))
		pr_warn(SYS "hdcp22_tx_skp failed to probe\n");
	else
		hdmitx_device.hdmitx_clk_tree.hdcp22_tx_skp = hdcp22_tx_skp;

	hdcp22_tx_esm = devm_clk_get(hdmitx_dev, "hdcp22_tx_esm");
	if (IS_ERR(hdcp22_tx_esm))
		pr_warn(SYS "hdcp22_tx_esm failed to probe\n");
	else
		hdmitx_device.hdmitx_clk_tree.hdcp22_tx_esm = hdcp22_tx_esm;

	venci_top_gate = devm_clk_get(hdmitx_dev, "venci_top_gate");
	if (IS_ERR(venci_top_gate))
		pr_warn(SYS "venci_top_gate failed to probe\n");
	else
		hdmitx_device.hdmitx_clk_tree.venci_top_gate = venci_top_gate;

	venci_0_gate = devm_clk_get(hdmitx_dev, "venci_0_gate");
	if (IS_ERR(venci_0_gate))
		pr_warn(SYS "venci_0_gate failed to probe\n");
	else
		hdmitx_device.hdmitx_clk_tree.venci_0_gate = venci_0_gate;

	venci_1_gate = devm_clk_get(hdmitx_dev, "venci_1_gate");
	if (IS_ERR(venci_1_gate))
		pr_warn(SYS "venci_0_gate failed to probe\n");
	else
		hdmitx_device.hdmitx_clk_tree.venci_1_gate = venci_1_gate;
}

void amhdmitx_vpu_dev_regiter(struct hdmitx_dev *hdev)
{
	hdev->hdmitx_vpu_clk_gate_dev =
	vpu_dev_register(VPU_VENCI, DEVICE_NAME);
}

static long hdmitx_report_ioctl(struct file *f, unsigned int cmd,
				unsigned long arg)
{
	int rtn_val;
	unsigned int out_size;
	unsigned long check_size;

	out_size = _IOC_SIZE(cmd);
	switch (cmd) {
	case HDMI_REPORT_IOC_HPD:
		check_size = copy_to_user((void __user *)arg,
					  (void *)&hdmitx_status.hpd_state,
					  out_size);
		if (check_size == out_size)
			rtn_val = 0;
		else
			rtn_val = -1;
		break;
	case HDMI_REPORT_IOC_AUD:
		check_size = copy_to_user((void __user *)arg,
					  (void *)&hdmitx_status.audio,
					  out_size);
		if (check_size == out_size)
			rtn_val = 0;
		else
			rtn_val = -1;
		break;
	case HDMI_REPORT_IOC_PWR:
		check_size = copy_to_user((void __user *)arg,
					  (void *)&hdmitx_status.pwr,
					  out_size);
		if (check_size == out_size)
			rtn_val = 0;
		else
			rtn_val = -1;
		break;
	case HDMI_REPORT_IOC_HDR:
		check_size = copy_to_user((void __user *)arg,
					  (void *)&hdmitx_status.hdr,
					  out_size);
		if (check_size == out_size)
			rtn_val = 0;
		else
			rtn_val = -1;
		break;
	case HDMI_REPORT_IOC_RXSN:
		check_size = copy_to_user((void __user *)arg,
					  (void *)&hdmitx_status.rxsense,
					  out_size);
		if (check_size == out_size)
			rtn_val = 0;
		else
			rtn_val = -1;
		break;
	case HDMI_REPORT_IOC_HDCP:
		check_size = copy_to_user((void __user *)arg,
					  (void *)&hdmitx_status.hdcp,
					  out_size);
		if (check_size == out_size)
			rtn_val = 0;
		else
			rtn_val = -1;
		break;
	case HDMI_REPORT_IOC_CEDST:
		check_size = copy_to_user((void __user *)arg,
					  (void *)&hdmitx_status.cedst,
					  out_size);
		if (check_size == out_size)
			rtn_val = 0;
		else
			rtn_val = -1;
		break;
	default:
		pr_info("HDMI Report command WRONG\n");
		rtn_val = -EPERM;
	}

	return rtn_val;
}

static const struct file_operations hdmitx_report_file_operations = {
	.unlocked_ioctl = hdmitx_report_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = hdmitx_report_ioctl,
#endif
	.owner = THIS_MODULE,
};

static struct miscdevice hdmitx_report_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "hdmitx_report",
	.fops = &hdmitx_report_file_operations,
};

static int amhdmitx_probe(struct platform_device *pdev)
{
	int r, ret = 0;
	struct device *dev;
	struct hdmitx_dev *hdev = &hdmitx_device;

	pr_debug(SYS "%s start\n", __func__);

	amhdmitx_device_init(hdev);

	ret = amhdmitx_get_dt_info(pdev);
	if (ret)
		return ret;

	amhdmitx_clktree_probe(&pdev->dev);

	amhdmitx_vpu_dev_regiter(hdev);

	misc_register(&hdmitx_report_device);
	r = alloc_chrdev_region(&hdev->hdmitx_id, 0, HDMI_TX_COUNT,
				DEVICE_NAME);
	cdev_init(&hdev->cdev, &amhdmitx_fops);
	hdev->cdev.owner = THIS_MODULE;
	r = cdev_add(&hdev->cdev, hdev->hdmitx_id, HDMI_TX_COUNT);

	hdmitx_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(hdmitx_class)) {
		unregister_chrdev_region(hdev->hdmitx_id, HDMI_TX_COUNT);
		return -1;
	}

	dev = device_create(hdmitx_class, NULL, hdev->hdmitx_id, NULL,
			    "amhdmitx%d", 0); /* kernel>=2.6.27 */

	if (!dev) {
		pr_info(SYS "device_create create error\n");
		class_destroy(hdmitx_class);
		r = -EEXIST;
		return r;
	}
	hdev->hdtx_dev = dev;
	ret = device_create_file(dev, &dev_attr_disp_mode);
	ret = device_create_file(dev, &dev_attr_attr);
	ret = device_create_file(dev, &dev_attr_aud_mode);
	ret = device_create_file(dev, &dev_attr_vid_mute);
	ret = device_create_file(dev, &dev_attr_edid);
	ret = device_create_file(dev, &dev_attr_rawedid);
	ret = device_create_file(dev, &dev_attr_sink_type);
	ret = device_create_file(dev, &dev_attr_edid_parsing);
	ret = device_create_file(dev, &dev_attr_config);
	ret = device_create_file(dev, &dev_attr_debug);
	ret = device_create_file(dev, &dev_attr_disp_cap);
	ret = device_create_file(dev, &dev_attr_preferred_mode);
	ret = device_create_file(dev, &dev_attr_cea_cap);
	ret = device_create_file(dev, &dev_attr_vesa_cap);
	ret = device_create_file(dev, &dev_attr_disp_cap_3d);
	ret = device_create_file(dev, &dev_attr_aud_cap);
	ret = device_create_file(dev, &dev_attr_hdmi_hdr_status);
	ret = device_create_file(dev, &dev_attr_hdr_cap);
	ret = device_create_file(dev, &dev_attr_dv_cap);
	ret = device_create_file(dev, &dev_attr_dv_cap2);
	ret = device_create_file(dev, &dev_attr_aud_ch);
	ret = device_create_file(dev, &dev_attr_avmute);
	ret = device_create_file(dev, &dev_attr_swap);
	ret = device_create_file(dev, &dev_attr_vic);
	ret = device_create_file(dev, &dev_attr_phy);
	ret = device_create_file(dev, &dev_attr_frac_rate_policy);
	ret = device_create_file(dev, &dev_attr_sspll);
	ret = device_create_file(dev, &dev_attr_rxsense_policy);
	ret = device_create_file(dev, &dev_attr_cedst_policy);
	ret = device_create_file(dev, &dev_attr_cedst_count);
	ret = device_create_file(dev, &dev_attr_hdcp_clkdis);
	ret = device_create_file(dev, &dev_attr_hdcp_pwr);
	ret = device_create_file(dev, &dev_attr_hdcp_ksv_info);
	ret = device_create_file(dev, &dev_attr_hdcp_ver);
	ret = device_create_file(dev, &dev_attr_hdcp_byp);
	ret = device_create_file(dev, &dev_attr_hdcp_mode);
	ret = device_create_file(dev, &dev_attr_hdcp_repeater);
	ret = device_create_file(dev, &dev_attr_hdcp_topo_info);
	ret = device_create_file(dev, &dev_attr_hdcp22_type);
	ret = device_create_file(dev, &dev_attr_hdcp_stickmode);
	ret = device_create_file(dev, &dev_attr_hdcp_stickstep);
	ret = device_create_file(dev, &dev_attr_hdmi_repeater_tx);
	ret = device_create_file(dev, &dev_attr_hdcp22_base);
	ret = device_create_file(dev, &dev_attr_hdcp_lstore);
	ret = device_create_file(dev, &dev_attr_hdcp_rptxlstore);
	ret = device_create_file(dev, &dev_attr_div40);
	ret = device_create_file(dev, &dev_attr_hdcp_ctrl);
	ret = device_create_file(dev, &dev_attr_hpd_state);
	ret = device_create_file(dev, &dev_attr_rhpd_state);
	ret = device_create_file(dev, &dev_attr_max_exceed);
	ret = device_create_file(dev, &dev_attr_fake_plug);
	ret = device_create_file(dev, &dev_attr_hdmi_init);
	ret = device_create_file(dev, &dev_attr_ready);
	ret = device_create_file(dev, &dev_attr_support_3d);
	ret = device_create_file(dev, &dev_attr_dc_cap);
	ret = device_create_file(dev, &dev_attr_valid_mode);
	ret = device_create_file(dev, &dev_attr_allm_cap);
	ret = device_create_file(dev, &dev_attr_allm_mode);
	ret = device_create_file(dev, &dev_attr_contenttype_cap);
	ret = device_create_file(dev, &dev_attr_contenttype_mode);
	ret = device_create_file(dev, &dev_attr_hdmi_config_info);
	ret = device_create_file(dev, &dev_attr_hdmirx_info);
	ret = device_create_file(dev, &dev_attr_hdmi_hsty_config);

#ifdef CONFIG_AMLOGIC_LEGACY_EARLY_SUSPEND
	register_early_suspend(&hdmitx_early_suspend_handler);
#endif
	hdev->nb.notifier_call = hdmitx_reboot_notifier;
	register_reboot_notifier(&hdev->nb);

	hdmitx_meson_init(hdev);

	hdev->hpd_state = !!(hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0));

#ifdef CONFIG_AMLOGIC_VOUT_SERVE
	vout_register_server(&hdmitx_vout_server);
#endif
#ifdef CONFIG_AMLOGIC_VOUT2_SERVE
	vout2_register_server(&hdmitx_vout2_server);
#endif
#ifdef CONFIG_AMLOGIC_SND_SOC
	aout_register_client(&hdmitx_notifier_nb_a);
#endif

	hdmitx_extcon_register(pdev, dev);

	/* update fmt_attr */
	hdmitx_init_fmt_attr(hdev);

	hdmitx_extcon_hdmi->state = !!hdev->hwop.cntlmisc(hdev,
							  MISC_HPD_GPI_ST, 0);
	hdmitx_device.hpd_state = hdmitx_extcon_hdmi->state;
	hdmitx_notify_hpd(hdev->hpd_state);
	hdmitx_status.pwr = hdev->hpd_state;
	extcon_set_state_sync(hdmitx_extcon_power, EXTCON_DISP_HDMI,
			      hdev->hpd_state);
	INIT_WORK(&hdev->work_hdr, hdr_work_func);

/* When init hdmi, clear the hdmitx module edid ram and edid buffer. */
	hdmitx_edid_clear(hdev);
	hdmitx_edid_ram_buffer_clear(hdev);
	hdev->hdmi_wq = alloc_workqueue(DEVICE_NAME,
					WQ_HIGHPRI | WQ_CPU_INTENSIVE, 0);
	INIT_DELAYED_WORK(&hdev->work_hpd_plugin, hdmitx_hpd_plugin_handler);
	INIT_DELAYED_WORK(&hdev->work_hpd_plugout, hdmitx_hpd_plugout_handler);
	INIT_DELAYED_WORK(&hdev->work_aud_hpd_plug,
			  hdmitx_aud_hpd_plug_handler);
	INIT_DELAYED_WORK(&hdev->work_internal_intr,
			  hdmitx_internal_intr_handler);

	/* for rx sense feature */
	hdev->rxsense_wq = alloc_workqueue("hdmitx_extcon_rxsense",
					   WQ_SYSFS | WQ_FREEZABLE, 0);
	INIT_DELAYED_WORK(&hdev->work_rxsense, hdmitx_rxsense_process);
	/* for cedst feature */
	hdev->cedst_wq = alloc_workqueue("hdmitx_extcon_cedst",
					 WQ_SYSFS | WQ_FREEZABLE, 0);
	INIT_DELAYED_WORK(&hdev->work_cedst, hdmitx_cedst_process);

	hdev->tx_aud_cfg = 1; /* default audio configure is on */

	/*Direct Rander Management use another irq*/
	if (hdev->drm_feature == 0)
		hdev->hwop.setupirq(hdev);

	/* Trigger HDMITX IRQ*/
	if (hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0))
		hdev->hwop.cntlmisc(hdev, MISC_TRIGGER_HPD, 0);

	hdev->hdmi_init = 1;

	hdmitx_hdcp_init();

	pr_info(SYS "%s end\n", __func__);

	return r;
}

static int amhdmitx_remove(struct platform_device *pdev)
{
	struct device *dev = hdmitx_device.hdtx_dev;

	cancel_work_sync(&hdmitx_device.work_hdr);

	if (hdmitx_device.hwop.uninit)
		hdmitx_device.hwop.uninit(&hdmitx_device);
	hdmitx_device.hpd_event = 0xff;
	kthread_stop(hdmitx_device.task);
#ifdef CONFIG_AMLOGIC_VOUT_SERVE
	vout_unregister_server(&hdmitx_vout_server);
#endif
#ifdef CONFIG_AMLOGIC_VOUT2_SERVE
	vout2_unregister_server(&hdmitx_vout2_server);
#endif
#ifdef CONFIG_AMLOGIC_SND_SOC
	aout_unregister_client(&hdmitx_notifier_nb_a);
#endif

	/* Remove the cdev */
	device_remove_file(dev, &dev_attr_disp_mode);
	device_remove_file(dev, &dev_attr_attr);
	device_remove_file(dev, &dev_attr_aud_mode);
	device_remove_file(dev, &dev_attr_vid_mute);
	device_remove_file(dev, &dev_attr_edid);
	device_remove_file(dev, &dev_attr_rawedid);
	device_remove_file(dev, &dev_attr_sink_type);
	device_remove_file(dev, &dev_attr_edid_parsing);
	device_remove_file(dev, &dev_attr_config);
	device_remove_file(dev, &dev_attr_debug);
	device_remove_file(dev, &dev_attr_disp_cap);
	device_remove_file(dev, &dev_attr_preferred_mode);
	device_remove_file(dev, &dev_attr_cea_cap);
	device_remove_file(dev, &dev_attr_vesa_cap);
	device_remove_file(dev, &dev_attr_disp_cap_3d);
	device_remove_file(dev, &dev_attr_hdr_cap);
	device_remove_file(dev, &dev_attr_dv_cap);
	device_remove_file(dev, &dev_attr_dv_cap2);
	device_remove_file(dev, &dev_attr_dc_cap);
	device_remove_file(dev, &dev_attr_valid_mode);
	device_remove_file(dev, &dev_attr_allm_cap);
	device_remove_file(dev, &dev_attr_allm_mode);
	device_remove_file(dev, &dev_attr_contenttype_cap);
	device_remove_file(dev, &dev_attr_contenttype_mode);
	device_remove_file(dev, &dev_attr_hpd_state);
	device_remove_file(dev, &dev_attr_fake_plug);
	device_remove_file(dev, &dev_attr_rhpd_state);
	device_remove_file(dev, &dev_attr_max_exceed);
	device_remove_file(dev, &dev_attr_hdmi_init);
	device_remove_file(dev, &dev_attr_ready);
	device_remove_file(dev, &dev_attr_support_3d);
	device_remove_file(dev, &dev_attr_avmute);
	device_remove_file(dev, &dev_attr_vic);
	device_remove_file(dev, &dev_attr_frac_rate_policy);
	device_remove_file(dev, &dev_attr_sspll);
	device_remove_file(dev, &dev_attr_rxsense_policy);
	device_remove_file(dev, &dev_attr_cedst_policy);
	device_remove_file(dev, &dev_attr_cedst_count);
	device_remove_file(dev, &dev_attr_hdcp_pwr);
	device_remove_file(dev, &dev_attr_div40);
	device_remove_file(dev, &dev_attr_hdcp_repeater);
	device_remove_file(dev, &dev_attr_hdcp_topo_info);
	device_remove_file(dev, &dev_attr_hdcp22_type);
	device_remove_file(dev, &dev_attr_hdcp_stickmode);
	device_remove_file(dev, &dev_attr_hdcp_stickstep);
	device_remove_file(dev, &dev_attr_hdmi_repeater_tx);
	device_remove_file(dev, &dev_attr_hdcp22_base);
	device_remove_file(dev, &dev_attr_swap);
	device_remove_file(dev, &dev_attr_hdmi_hdr_status);
	device_remove_file(dev, &dev_attr_hdmi_config_info);
	device_remove_file(dev, &dev_attr_hdmirx_info);
	device_remove_file(dev, &dev_attr_hdmi_hsty_config);

	cdev_del(&hdmitx_device.cdev);

	device_destroy(hdmitx_class, hdmitx_device.hdmitx_id);

	class_destroy(hdmitx_class);

	unregister_chrdev_region(hdmitx_device.hdmitx_id, HDMI_TX_COUNT);
	misc_deregister(&hdmitx_report_device);
	return 0;
}

#ifdef CONFIG_PM
static int amhdmitx_suspend(struct platform_device *pdev,
			    pm_message_t state)
{
	hdmitx_device.hwop.cntlddc(&hdmitx_device,
		DDC_RESET_HDCP, 0);
	pr_info("amhdmitx: suspend and reset hdcp\n");
	return 0;
}

static int amhdmitx_resume(struct platform_device *pdev)
{
	struct hdmitx_dev *hdev = &hdmitx_device;

	pr_debug("amhdmitx: I2C_REACTIVE\n");
	hdev->hwop.cntlmisc(hdev, MISC_I2C_REACTIVE, 0);

	return 0;
}
#endif

static struct platform_driver amhdmitx_driver = {
	.probe	  = amhdmitx_probe,
	.remove	 = amhdmitx_remove,
#ifdef CONFIG_PM
	.suspend	= amhdmitx_suspend,
	.resume	 = amhdmitx_resume,
#endif
	.driver	 = {
		.name   = DEVICE_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(meson_amhdmitx_of_match),
#ifdef CONFIG_HIBERNATION
		.pm	= &amhdmitx_pm,
#endif
	}
};

int  __init amhdmitx_init(void)
{
	if (init_flag & INIT_FLAG_NOT_LOAD)
		return 0;

	return platform_driver_register(&amhdmitx_driver);
}

void __exit amhdmitx_exit(void)
{
	pr_info(SYS "%s...\n", __func__);
	cancel_delayed_work_sync(&hdmitx_device.work_do_hdcp);
	kthread_stop(hdmitx_device.task_hdcp);
	platform_driver_unregister(&amhdmitx_driver);
}

#ifndef MODULE
subsys_initcall(amhdmitx_init);
module_exit(amhdmitx_exit);
#endif

//MODULE_DESCRIPTION("AMLOGIC HDMI TX driver");
//MODULE_LICENSE("GPL");
//MODULE_VERSION("1.0.0");

/* besides characters defined in separator, '\"' are used as separator;
 * and any characters in '\"' will not act as separator
 */
static char *next_token_ex(char *separator, char *buf, unsigned int size,
			   unsigned int offset, unsigned int *token_len,
			   unsigned int *token_offset)
{
	char *ptoken = NULL;
	char last_separator = 0;
	char trans_char_flag = 0;

	if (buf) {
		for (; offset < size; offset++) {
			int ii = 0;
		char ch;

		if (buf[offset] == '\\') {
			trans_char_flag = 1;
			continue;
		}
		while (((ch = separator[ii++]) != buf[offset]) && (ch))
			;
		if (ch) {
			if (!ptoken) {
				continue;
		} else {
			if (last_separator != '"') {
				*token_len = (unsigned int)
					(buf + offset - ptoken);
				*token_offset = offset;
				return ptoken;
			}
		}
		} else if (!ptoken) {
			if (trans_char_flag && (buf[offset] == '"'))
				last_separator = buf[offset];
			ptoken = &buf[offset];
		} else if ((trans_char_flag && (buf[offset] == '"')) &&
			   (last_separator == '"')) {
			*token_len = (unsigned int)(buf + offset - ptoken - 2);
			*token_offset = offset + 1;
			return ptoken + 1;
		}
		trans_char_flag = 0;
	}
	if (ptoken) {
		*token_len = (unsigned int)(buf + offset - ptoken);
		*token_offset = offset;
	}
	}
	return ptoken;
}

/* check the colorattribute from uboot */
static void check_hdmiuboot_attr(char *token)
{
	char attr[16] = {0};
	const char * const cs[] = {
		"444", "422", "rgb", "420", NULL};
	const char * const cd[] = {
		"8bit", "10bit", "12bit", "16bit", NULL};
	int i;

	if (hdmitx_device.fmt_attr[0] != 0)
		return;

	if (!token)
		return;

	for (i = 0; cs[i]; i++) {
		if (strstr(token, cs[i])) {
			if (strlen(cs[i]) < sizeof(attr))
				strcpy(attr, cs[i]);
			strcat(attr, ",");
			break;
		}
	}
	for (i = 0; cd[i]; i++) {
		if (strstr(token, cd[i])) {
			if (strlen(cd[i]) < sizeof(attr))
				if (strlen(cd[i]) <
					(sizeof(attr) - strlen(attr)))
					strcat(attr, cd[i]);
			strncpy(hdmitx_device.fmt_attr, attr,
				sizeof(hdmitx_device.fmt_attr));
			hdmitx_device.fmt_attr[15] = '\0';
			break;
		}
	}
}

static int hdmitx_boot_para_setup(char *s)
{
	char separator[] = {' ', ',', ';', 0x0};
	char *token;
	unsigned int token_len = 0;
	unsigned int token_offset = 0;
	unsigned int offset = 0;
	int size = strlen(s);

	memset(hdmitx_device.fmt_attr, 0, sizeof(hdmitx_device.fmt_attr));

	do {
		token = next_token_ex(separator, s, size, offset,
				      &token_len, &token_offset);
		if (token) {
			if (token_len == 3 &&
			    strncmp(token, "off", token_len) == 0) {
				init_flag |= INIT_FLAG_NOT_LOAD;
			}
			check_hdmiuboot_attr(token);
		}
		offset = token_offset;
	} while (token);
	return 0;
}

__setup("hdmitx=", hdmitx_boot_para_setup);

static int hdmitx_boot_frac_rate(char *str)
{
	if (strncmp("0", str, 1) == 0)
		hdmitx_device.frac_rate_policy = 0;
	else
		hdmitx_device.frac_rate_policy = 1;

	pr_info("hdmitx boot frac_rate_policy: %d",
		hdmitx_device.frac_rate_policy);
	return 0;
}

__setup("frac_rate_policy=", hdmitx_boot_frac_rate);

static int hdmitx_boot_hdr_priority(char *str)
{
	if (strncmp("1", str, 1) == 0) {
		hdmitx_device.hdr_priority = 1;
		pr_info("hdmitx boot hdr_priority: 1\n");
	}
	return 0;
}

__setup("hdr_priority=", hdmitx_boot_hdr_priority);

MODULE_PARM_DESC(log_level, "\n log_level\n");
module_param(log_level, int, 0644);
