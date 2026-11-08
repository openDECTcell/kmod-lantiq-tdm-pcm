/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Lantiq VR9 TDM/PCM transport: what the board code (the bring-up fallback)
 * hands to the driver.
 */
#ifndef _TDM_PCM_H
#define _TDM_PCM_H

#include <linux/types.h>

#define TDM_PCM_DRV_NAME	"lantiq-tdm-pcm"

/*
 * One pad of the PCM bus. pinctrl-xway has no TDM group, so the driver
 * sets the mux itself: ALT0/ALT1 select the function, out the pad
 * direction, push_pull the driver type of an output (else open drain).
 */
struct tdm_pcm_pin {
	u8 gpio;
	u8 alt0:1;
	u8 alt1:1;
	u8 out:1;
	u8 push_pull:1;
};

struct tdm_pcm_pdata {
	u32 pmu_gate;		/* PMU gate bit when no clk is described */
	const struct tdm_pcm_pin *pins;
	unsigned int n_pins;
};

int tdm_pcm_fallback_init(void);
void tdm_pcm_fallback_exit(void);

#endif /* _TDM_PCM_H */
