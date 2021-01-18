// SPDX-License-Identifier: GPL-2.0-only
/*
 * Amlogic Meson8b, Meson8m2 and GXBB DWMAC glue layer
 *
 * Copyright (C) 2016 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_net.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/stmmac.h>
#include <linux/arm-smccc.h>

#include "stmmac_platform.h"

#define PRG_ETH0			0x0

#define PRG_ETH0_RGMII_MODE		BIT(0)

#define PRG_ETH0_EXT_PHY_MODE_MASK	GENMASK(2, 0)
#define PRG_ETH0_EXT_RGMII_MODE		1
#define PRG_ETH0_EXT_RMII_MODE		4

/* mux to choose between fclk_div2 (bit unset) and mpll2 (bit set) */
#define PRG_ETH0_CLK_M250_SEL_SHIFT	4
#define PRG_ETH0_CLK_M250_SEL_MASK	GENMASK(4, 4)

#define PRG_ETH0_TXDLY_SHIFT		5
#define PRG_ETH0_TXDLY_MASK		GENMASK(6, 5)

/* divider for the result of m250_sel */
#define PRG_ETH0_CLK_M250_DIV_SHIFT	7
#define PRG_ETH0_CLK_M250_DIV_WIDTH	3

#define PRG_ETH0_RGMII_TX_CLK_EN	10

#define PRG_ETH0_INVERTED_RMII_CLK	BIT(11)
#define PRG_ETH0_TX_AND_PHY_REF_CLK	BIT(12)

#define MUX_CLK_NUM_PARENTS		2

#ifdef CONFIG_AMLOGIC_ETH_PRIVE
#define ETH_PLL_CTL3_CTS            0x50

unsigned int cts_setting[16] = {0xA7E00000, 0x87E00000, 0x8BE00000, 0x93E00000,
				0x8FE00000, 0x97E00000,	0x9BE00000, 0xA7E00000,
				0xABE00000, 0xB3E00000, 0xAFE00000, 0xB7E00000,
				0xE7E00000, 0xEFE00000, 0xFBE00000, 0xFFE00000};

enum {
	/* chip num */
	ETH_PHY		= 0x0,
	ETH_PHY_C1	= 0x1,
	ETH_PHY_C2	= 0x2,
	ETH_PHY_SC2	= 0x3, //kerel android-q
	ETH_PHY_T5      = 0x4,
	ETH_PHY_T7      = 0x5,
};

unsigned int tx_amp_bl2;
EXPORT_SYMBOL_GPL(tx_amp_bl2);
unsigned int enet_type;
EXPORT_SYMBOL_GPL(enet_type);
#endif

struct meson8b_dwmac;

struct meson8b_dwmac_data {
	int (*set_phy_mode)(struct meson8b_dwmac *dwmac);
};

struct meson8b_dwmac {
	struct device			*dev;
	void __iomem			*regs;

	const struct meson8b_dwmac_data	*data;
	phy_interface_t			phy_mode;
	struct clk			*rgmii_tx_clk;
	u32				tx_delay_ns;
};

struct meson8b_dwmac_clk_configs {
	struct clk_mux		m250_mux;
	struct clk_divider	m250_div;
	struct clk_fixed_factor	fixed_div2;
	struct clk_gate		rgmii_tx_en;
};

static void meson8b_dwmac_mask_bits(struct meson8b_dwmac *dwmac, u32 reg,
				    u32 mask, u32 value)
{
	u32 data;

	data = readl(dwmac->regs + reg);
	data &= ~mask;
	data |= (value & mask);

	writel(data, dwmac->regs + reg);
}

static struct clk *meson8b_dwmac_register_clk(struct meson8b_dwmac *dwmac,
					      const char *name_suffix,
					      const char **parent_names,
					      int num_parents,
					      const struct clk_ops *ops,
					      struct clk_hw *hw)
{
	struct clk_init_data init;
	char clk_name[32];

	snprintf(clk_name, sizeof(clk_name), "%s#%s", dev_name(dwmac->dev),
		 name_suffix);

	init.name = clk_name;
	init.ops = ops;
	init.flags = CLK_SET_RATE_PARENT;
	init.parent_names = parent_names;
	init.num_parents = num_parents;

	hw->init = &init;

	return devm_clk_register(dwmac->dev, hw);
}

static int meson8b_init_rgmii_tx_clk(struct meson8b_dwmac *dwmac)
{
	int i, ret;
	struct clk *clk;
	struct device *dev = dwmac->dev;
	const char *parent_name, *mux_parent_names[MUX_CLK_NUM_PARENTS];
	struct meson8b_dwmac_clk_configs *clk_configs;
	static const struct clk_div_table div_table[] = {
		{ .div = 2, .val = 2, },
		{ .div = 3, .val = 3, },
		{ .div = 4, .val = 4, },
		{ .div = 5, .val = 5, },
		{ .div = 6, .val = 6, },
		{ .div = 7, .val = 7, },
		{ /* end of array */ }
	};

	clk_configs = devm_kzalloc(dev, sizeof(*clk_configs), GFP_KERNEL);
	if (!clk_configs)
		return -ENOMEM;

	/* get the mux parents from DT */
	for (i = 0; i < MUX_CLK_NUM_PARENTS; i++) {
		char name[16];

		snprintf(name, sizeof(name), "clkin%d", i);
		clk = devm_clk_get(dev, name);
		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "Missing clock %s\n", name);
			return ret;
		}

		mux_parent_names[i] = __clk_get_name(clk);
	}

	clk_configs->m250_mux.reg = dwmac->regs + PRG_ETH0;
	clk_configs->m250_mux.shift = PRG_ETH0_CLK_M250_SEL_SHIFT;
	clk_configs->m250_mux.mask = PRG_ETH0_CLK_M250_SEL_MASK;
#ifdef CONFIG_AMLOGIC_ETH_PRIVE
	clk = meson8b_dwmac_register_clk(dwmac, "m250_sel",
					 &mux_parent_names[0],
					 1, &clk_mux_ops,
					 &clk_configs->m250_mux.hw);
#else
	clk = meson8b_dwmac_register_clk(dwmac, "m250_sel", mux_parent_names,
					 MUX_CLK_NUM_PARENTS, &clk_mux_ops,
					 &clk_configs->m250_mux.hw);
#endif
	if (WARN_ON(IS_ERR(clk)))
		return PTR_ERR(clk);

	parent_name = __clk_get_name(clk);
	clk_configs->m250_div.reg = dwmac->regs + PRG_ETH0;
	clk_configs->m250_div.shift = PRG_ETH0_CLK_M250_DIV_SHIFT;
	clk_configs->m250_div.width = PRG_ETH0_CLK_M250_DIV_WIDTH;
	clk_configs->m250_div.table = div_table;
	clk_configs->m250_div.flags = CLK_DIVIDER_ALLOW_ZERO |
				      CLK_DIVIDER_ROUND_CLOSEST;
	clk = meson8b_dwmac_register_clk(dwmac, "m250_div", &parent_name, 1,
					 &clk_divider_ops,
					 &clk_configs->m250_div.hw);
	if (WARN_ON(IS_ERR(clk)))
		return PTR_ERR(clk);

	parent_name = __clk_get_name(clk);
	clk_configs->fixed_div2.mult = 1;
	clk_configs->fixed_div2.div = 2;
	clk = meson8b_dwmac_register_clk(dwmac, "fixed_div2", &parent_name, 1,
					 &clk_fixed_factor_ops,
					 &clk_configs->fixed_div2.hw);
	if (WARN_ON(IS_ERR(clk)))
		return PTR_ERR(clk);

	parent_name = __clk_get_name(clk);
	clk_configs->rgmii_tx_en.reg = dwmac->regs + PRG_ETH0;
	clk_configs->rgmii_tx_en.bit_idx = PRG_ETH0_RGMII_TX_CLK_EN;
	clk = meson8b_dwmac_register_clk(dwmac, "rgmii_tx_en", &parent_name, 1,
					 &clk_gate_ops,
					 &clk_configs->rgmii_tx_en.hw);
	if (WARN_ON(IS_ERR(clk)))
		return PTR_ERR(clk);

	dwmac->rgmii_tx_clk = clk;

	return 0;
}

static int meson8b_set_phy_mode(struct meson8b_dwmac *dwmac)
{
	switch (dwmac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		/* enable RGMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_RGMII_MODE,
					PRG_ETH0_RGMII_MODE);
		break;
	case PHY_INTERFACE_MODE_RMII:
		/* disable RGMII mode -> enables RMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_RGMII_MODE, 0);
		break;
	default:
		dev_err(dwmac->dev, "fail to set phy-mode %s\n",
			phy_modes(dwmac->phy_mode));
		return -EINVAL;
	}

	return 0;
}

static int meson_axg_set_phy_mode(struct meson8b_dwmac *dwmac)
{
	switch (dwmac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		/* enable RGMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_EXT_PHY_MODE_MASK,
					PRG_ETH0_EXT_RGMII_MODE);
		break;
	case PHY_INTERFACE_MODE_RMII:
		/* disable RGMII mode -> enables RMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_EXT_PHY_MODE_MASK,
					PRG_ETH0_EXT_RMII_MODE);
		break;
	default:
		dev_err(dwmac->dev, "fail to set phy-mode %s\n",
			phy_modes(dwmac->phy_mode));
		return -EINVAL;
	}

	return 0;
}

static int meson8b_init_prg_eth(struct meson8b_dwmac *dwmac)
{
	int ret;
	u8 tx_dly_val = 0;

	switch (dwmac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_RXID:
		/* TX clock delay in ns = "8ns / 4 * tx_dly_val" (where
		 * 8ns are exactly one cycle of the 125MHz RGMII TX clock):
		 * 0ns = 0x0, 2ns = 0x1, 4ns = 0x2, 6ns = 0x3
		 */
		tx_dly_val = dwmac->tx_delay_ns >> 1;
		/* fall through */

	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		/* only relevant for RMII mode -> disable in RGMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_INVERTED_RMII_CLK, 0);

		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0, PRG_ETH0_TXDLY_MASK,
					tx_dly_val << PRG_ETH0_TXDLY_SHIFT);

		/* Configure the 125MHz RGMII TX clock, the IP block changes
		 * the output automatically (= without us having to configure
		 * a register) based on the line-speed (125MHz for Gbit speeds,
		 * 25MHz for 100Mbit/s and 2.5MHz for 10Mbit/s).
		 */
		ret = clk_set_rate(dwmac->rgmii_tx_clk, 125 * 1000 * 1000);
		if (ret) {
			dev_err(dwmac->dev,
				"failed to set RGMII TX clock\n");
			return ret;
		}

		ret = clk_prepare_enable(dwmac->rgmii_tx_clk);
		if (ret) {
			dev_err(dwmac->dev,
				"failed to enable the RGMII TX clock\n");
			return ret;
		}

		devm_add_action_or_reset(dwmac->dev,
					(void(*)(void *))clk_disable_unprepare,
					dwmac->rgmii_tx_clk);
		break;

	case PHY_INTERFACE_MODE_RMII:
		/* invert internal clk_rmii_i to generate 25/2.5 tx_rx_clk */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_INVERTED_RMII_CLK,
					PRG_ETH0_INVERTED_RMII_CLK);

		/* TX clock delay cannot be configured in RMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0, PRG_ETH0_TXDLY_MASK,
					0);

		break;

	default:
		dev_err(dwmac->dev, "unsupported phy-mode %s\n",
			phy_modes(dwmac->phy_mode));
		return -EINVAL;
	}

	/* enable TX_CLK and PHY_REF_CLK generator */
	meson8b_dwmac_mask_bits(dwmac, PRG_ETH0, PRG_ETH0_TX_AND_PHY_REF_CLK,
				PRG_ETH0_TX_AND_PHY_REF_CLK);

	return 0;
}

#ifdef CONFIG_AMLOGIC_ETH_PRIVE
void __iomem *ee_reset_base;
static int aml_custom_setting(struct platform_device *pdev, struct meson8b_dwmac *dwmac)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
//	struct device_node *np = dev->of_node;
//	struct g12a_mdio_mux *priv = dev_get_drvdata(dev);
	void __iomem *tx_amp_src = NULL;
	void __iomem *addr = NULL;
	struct resource *res = NULL;
	unsigned int internal_phy = 0;
	unsigned int cts_valid = 0;
	unsigned int cts_amp = 0;

	tx_amp_bl2 = 0;
	enet_type = 0;

	/*get tx amp setting from tx_amp_src*/
	pr_info("aml_cust_setting\n");

	/*map ETH_RESET address*/
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "eth_reset");
	if (!res) {
		dev_err(&pdev->dev, "Unable to get resource(%d)\n", __LINE__);
		ee_reset_base = NULL;
	} else {
		addr = devm_ioremap(dev, res->start, resource_size(res));
		if (IS_ERR(addr))
			dev_err(&pdev->dev, "Unable to map reset base\n");
		ee_reset_base = addr;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "tx_amp_src");
	if (!res) {
		pr_info("tx_amp_src not setup\n");
	} else {
		tx_amp_src = devm_ioremap_resource(dev, res);
		if (IS_ERR(addr)) {
			dev_err(&pdev->dev,
				"cat't map tx_amp (%d)\n", __LINE__);
		}
		tx_amp_bl2 = readl(tx_amp_src);
	}
	/*enet_type*/
	if (of_property_read_u32(np, "enet_type", &enet_type))
		pr_info("default enet type as 0\n");

	if (of_property_read_u32(np, "internal_phy", &internal_phy) != 0)
		pr_info("use default internal_phy as 0\n");

	if (internal_phy) {
		tx_amp_bl2 = (readl(tx_amp_src) & 0x3f);
		/*T5 use new method for tuning cts*/
		if (enet_type == ETH_PHY_T5) {
			cts_valid =  (tx_amp_bl2 >> 4) & 0x3;

			if (cts_valid)
				cts_amp  = tx_amp_bl2 & 0xf;
			/*invalid will set cts_setting[0] 0xA7E00000*/
			writel(cts_setting[cts_amp], dwmac->regs + ETH_PLL_CTL3_CTS);
			tx_amp_bl2 = 0x15;
		}
	/*test*/
//	tx_amp_bl2 = 0x15;
	}
	return 0;
}

extern int stmmac_pltfr_suspend(struct device *dev);
static int aml_dwmac_suspend(struct device *dev)
{
	int ret = 0;

	pr_info("wzh aml_suspend\n");
	ret = stmmac_pltfr_suspend(dev);
	return ret;
}

extern int stmmac_pltfr_resume(struct device *dev);
static int aml_dwmac_resume(struct device *dev)
{
	int ret = 0;

	pr_info("wzh aml_resume\n");
	ret = stmmac_pltfr_resume(dev);
	return 0;
}

void set_wol_notify_bl31(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(0x8200009D, 0,
					0, 0, 0, 0, 0, 0, &res);
}

#endif
static int meson8b_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct meson8b_dwmac *dwmac;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = stmmac_probe_config_dt(pdev, &stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	dwmac = devm_kzalloc(&pdev->dev, sizeof(*dwmac), GFP_KERNEL);
	if (!dwmac) {
		ret = -ENOMEM;
		goto err_remove_config_dt;
	}

	dwmac->data = (const struct meson8b_dwmac_data *)
		of_device_get_match_data(&pdev->dev);
	if (!dwmac->data) {
		ret = -EINVAL;
		goto err_remove_config_dt;
	}
	dwmac->regs = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(dwmac->regs)) {
		ret = PTR_ERR(dwmac->regs);
		goto err_remove_config_dt;
	}

	dwmac->dev = &pdev->dev;
	dwmac->phy_mode = of_get_phy_mode(pdev->dev.of_node);
	if ((int)dwmac->phy_mode < 0) {
		dev_err(&pdev->dev, "missing phy-mode property\n");
		ret = -EINVAL;
		goto err_remove_config_dt;
	}
#ifdef CONFIG_AMLOGIC_ETH_PRIVE
	/*clear top reg bit13 to disable adj function*/
	writel((readl(dwmac->regs) & (~0x2000)), dwmac->regs);
#endif
	/* use 2ns as fallback since this value was previously hardcoded */
	if (of_property_read_u32(pdev->dev.of_node, "amlogic,tx-delay-ns",
				 &dwmac->tx_delay_ns))
		dwmac->tx_delay_ns = 2;

	ret = meson8b_init_rgmii_tx_clk(dwmac);
	if (ret)
		goto err_remove_config_dt;

	ret = dwmac->data->set_phy_mode(dwmac);
	if (ret)
		goto err_remove_config_dt;

	ret = meson8b_init_prg_eth(dwmac);
	if (ret)
		goto err_remove_config_dt;

	plat_dat->bsp_priv = dwmac;

	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret)
		goto err_remove_config_dt;
#ifdef CONFIG_AMLOGIC_ETH_PRIVE
	aml_custom_setting(pdev, dwmac);
	set_wol_notify_bl31();
	device_init_wakeup(&pdev->dev, 1);
#endif
	return 0;

err_remove_config_dt:
	stmmac_remove_config_dt(pdev, plat_dat);

	return ret;
}

static const struct meson8b_dwmac_data meson8b_dwmac_data = {
	.set_phy_mode = meson8b_set_phy_mode,
};

static const struct meson8b_dwmac_data meson_axg_dwmac_data = {
	.set_phy_mode = meson_axg_set_phy_mode,
};

static const struct of_device_id meson8b_dwmac_match[] = {
	{
		.compatible = "amlogic,meson8b-dwmac",
		.data = &meson8b_dwmac_data,
	},
	{
		.compatible = "amlogic,meson8m2-dwmac",
		.data = &meson8b_dwmac_data,
	},
	{
		.compatible = "amlogic,meson-gxbb-dwmac",
		.data = &meson8b_dwmac_data,
	},
	{
		.compatible = "amlogic,meson-axg-dwmac",
		.data = &meson_axg_dwmac_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, meson8b_dwmac_match);
#ifdef CONFIG_AMLOGIC_ETH_PRIVE
SIMPLE_DEV_PM_OPS(stmmac_meson8b_pm_ops, aml_dwmac_suspend,
		  aml_dwmac_resume);
#endif
static struct platform_driver meson8b_dwmac_driver = {
	.probe  = meson8b_dwmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "meson8b-dwmac",
#ifdef CONFIG_AMLOGIC_ETH_PRIVE
		.pm		= &stmmac_meson8b_pm_ops,
#else
		.pm		= &stmmac_pltfr_pm_ops,
#endif
		.of_match_table = meson8b_dwmac_match,
	},
};
module_platform_driver(meson8b_dwmac_driver);

MODULE_AUTHOR("Martin Blumenstingl <martin.blumenstingl@googlemail.com>");
MODULE_DESCRIPTION("Amlogic Meson8b, Meson8m2 and GXBB DWMAC glue layer");
MODULE_LICENSE("GPL v2");
