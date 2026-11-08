// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bring-up fallback for the Lantiq VR9 TDM/PCM transport.
 *
 * OpenWrt's device tree for the FRITZ!Box 7362 SL has no node for the TDM
 * block, so nothing would bind the driver on a stock image. Only on that
 * machine, and only if no "lantiq,xrx200-tdm-pcm" node exists, this file
 * registers the platform device itself with the board facts below. The
 * proper path is a device tree node (docs/hardware.md, "Device tree").
 *
 * Board facts (docs/hardware.md): TDM block at 0x1F100000; receive
 * interrupt ICU 1 line 20, transmit ICU 1 line 19 (hardware numbers 60 and
 * 59 in the OpenWrt xway irq domain, which counts from 8); module gate PMU
 * bit 25. PCM pads: FSC GPIO 0 (function 3), DO GPIO 25 (function 1,
 * push-pull output as on the stock board), DCL GPIO 40 and DI GPIO 41
 * (function 2).
 */

#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#include "tdm_pcm.h"
#include "tdm_pcm_regs.h"

#define TDM_FB_MACHINE		"avm,fritz7362sl"
#define TDM_FB_ICU_COMPAT	"lantiq,icu"
#define TDM_FB_HWIRQ_RX		60
#define TDM_FB_HWIRQ_TX		59

static const struct tdm_pcm_pin tdm_fb_pins[] = {
	{ .gpio = 0,  .alt0 = 1, .alt1 = 1 },			/* FSC in */
	{ .gpio = 25, .alt0 = 1, .alt1 = 0, .out = 1, .push_pull = 1 },	/* DO */
	{ .gpio = 40, .alt0 = 0, .alt1 = 1 },			/* DCL in */
	{ .gpio = 41, .alt0 = 0, .alt1 = 1 },			/* DI in */
};

static const struct tdm_pcm_pdata tdm_fb_pdata = {
	.pmu_gate	= LTQ_PMU_TDM,
	.pins		= tdm_fb_pins,
	.n_pins		= ARRAY_SIZE(tdm_fb_pins),
};

static struct platform_device *tdm_fb_pdev;
static unsigned int tdm_fb_virq[2];

static unsigned int tdm_fb_map_irq(struct device_node *icu, u32 hwirq)
{
	struct of_phandle_args args = {
		.np		= icu,
		.args_count	= 1,
		.args		= { hwirq },
	};

	return irq_create_of_mapping(&args);
}

int tdm_pcm_fallback_init(void)
{
	struct resource res[3] = {
		DEFINE_RES_MEM(TDM_PHYS_BASE, TDM_REG_SIZE),
	};
	struct platform_device_info info = {
		.name		= TDM_PCM_DRV_NAME,
		.id		= PLATFORM_DEVID_NONE,
		.res		= res,
		.num_res	= 1,
		.data		= &tdm_fb_pdata,
		.size_data	= sizeof(tdm_fb_pdata),
	};
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "lantiq,xrx200-tdm-pcm");
	if (np) {
		of_node_put(np);
		return 0;	/* described by the device tree */
	}
	if (!of_machine_is_compatible(TDM_FB_MACHINE))
		return 0;

	np = of_find_compatible_node(NULL, NULL, TDM_FB_ICU_COMPAT);
	if (np) {
		tdm_fb_virq[0] = tdm_fb_map_irq(np, TDM_FB_HWIRQ_RX);
		tdm_fb_virq[1] = tdm_fb_map_irq(np, TDM_FB_HWIRQ_TX);
		of_node_put(np);
	}
	if (tdm_fb_virq[0] && tdm_fb_virq[1]) {
		res[1] = (struct resource)DEFINE_RES_IRQ(tdm_fb_virq[0]);
		res[2] = (struct resource)DEFINE_RES_IRQ(tdm_fb_virq[1]);
		info.num_res = 3;
	}

	tdm_fb_pdev = platform_device_register_full(&info);
	if (IS_ERR(tdm_fb_pdev)) {
		int ret = PTR_ERR(tdm_fb_pdev);

		tdm_fb_pdev = NULL;
		tdm_pcm_fallback_exit();
		return ret;
	}
	pr_info(TDM_PCM_DRV_NAME ": fallback device for %s registered\n",
		TDM_FB_MACHINE);
	return 0;
}

void tdm_pcm_fallback_exit(void)
{
	unsigned int i;

	if (tdm_fb_pdev) {
		platform_device_unregister(tdm_fb_pdev);
		tdm_fb_pdev = NULL;
	}
	for (i = 0; i < ARRAY_SIZE(tdm_fb_virq); i++) {
		if (tdm_fb_virq[i])
			irq_dispose_mapping(tdm_fb_virq[i]);
		tdm_fb_virq[i] = 0;
	}
}
