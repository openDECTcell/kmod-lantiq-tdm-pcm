/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Lantiq VR9 TDM/PCM block: register map.
 *
 * Offsets and bit meanings are hardware facts, restated in our own words
 * in docs/hardware.md (source: the register description that AVM ships in
 * its GPL drop for the FRITZ!Box 7362 SL, FRITZ!OS 07.12; no code from it is
 * used here). All registers are 32 bit, native (big) endian.
 */
#ifndef _TDM_PCM_REGS_H
#define _TDM_PCM_REGS_H

#include <linux/bits.h>

#define TDM_PHYS_BASE		0x1f100000	/* VR9 */
#define TDM_REG_SIZE		0x100		/* what we map and request */

#define TDM_CHANNELS		16

/* receive "ready" interrupt enable, one bit per channel (15:0) */
#define TDM_RX_IEN		0x00
/* transmit interrupt enable: "ready" 31:16, "empty" 15:0 */
#define TDM_TX_IEN		0x04
/* receive status: channel x holds received data (15:0), read only */
#define TDM_RX_STAT		0x08
/* transmit status: "ready" (room) 31:16, "empty" 15:0, read only */
#define TDM_TX_STAT		0x0c
#define TDM_TX_READY(ch)	BIT(16 + (ch))
#define TDM_TX_EMPTY(ch)	BIT(ch)
/* bit clock: rate code 4:0, 5 = use the code instead of auto detection */
#define TDM_FREQ		0x10
#define TDM_FREQ_FORCE		BIT(5)
#define TDM_FREQ_CODE_M		0x1f
/* common configuration */
#define TDM_CFG			0x14
#define TDM_CFG_ENABLE		BIT(15)	/* interface on */
#define TDM_CFG_DATA_BUF	BIT(14)	/* double buffer for data, not delay */
#define TDM_CFG_AC97_WARM	BIT(12)
#define TDM_CFG_AC97_COLD	BIT(11)
#define TDM_CFG_TX_OFS_S	8	/* 10:8 transmit offset in bit clocks */
#define TDM_CFG_TX_OFS_M	(7 << TDM_CFG_TX_OFS_S)
#define TDM_CFG_DBL_CLK		BIT(7)	/* two bit clocks per bit */
#define TDM_CFG_TX_FALLING	BIT(6)	/* transmit from the falling edge */
#define TDM_CFG_RX_RISING	BIT(5)	/* sample on the rising edge */
#define TDM_CFG_HALF_DRIVE	BIT(4)	/* drive bit 0 for half a clock only */
#define TDM_CFG_SHIFT		BIT(3)	/* double clocking: shift by one clock */
#define TDM_CFG_RX_OFS_M	7	/* 2:0 receive offset in bit clocks */
/* the bits of TDM_CFG a user may set through the module parameter */
#define TDM_CFG_USER_M		(TDM_CFG_DATA_BUF | TDM_CFG_TX_OFS_M | \
				 TDM_CFG_DBL_CLK | TDM_CFG_TX_FALLING | \
				 TDM_CFG_RX_RISING | TDM_CFG_HALF_DRIVE | \
				 TDM_CFG_SHIFT | TDM_CFG_RX_OFS_M)
/* time-slot channel enable, one bit per channel (15:0) */
#define TDM_CH_EN		0x18
/* loop received data of channel x back to its transmitter (15:0) */
#define TDM_LOOP		0x1c
/* channel width: 0 = 8 bit, 1 = 16 bit (two consecutive time slots) */
#define TDM_CH_MODE		0x20
/* status: 15/14 transmit slot collision on highway B/A (write 1 to
 * clear), 3:0 the detected bit-clock rate code, read only
 */
#define TDM_STAT		0x24
#define TDM_STAT_CRASH_B	BIT(15)
#define TDM_STAT_CRASH_A	BIT(14)
#define TDM_STAT_CLK_M		0xf
/* per channel: receive slot 23:16, receive highway 24, transmit slot 7:0,
 * transmit highway 8
 */
#define TDM_CH_SLOT(ch)		(0x28 + 4 * (ch))
#define TDM_CH_SLOT_RX_S	16
#define TDM_CH_SLOT_RX_HW2	BIT(24)
#define TDM_CH_SLOT_TX_HW2	BIT(8)
/* per channel data: a write queues the next transmit word, a read takes
 * the oldest received one; 16-bit mode: 15:8 = slot k, 7:0 = slot k + 1
 */
#define TDM_CH_DATA(ch)		(0x68 + 4 * (ch))

/* Power management unit (owned by the SoC code; we only read status). */
#define LTQ_PMU_PHYS		0x1f102000
#define LTQ_PMU_PWDSR		0x20	/* gate status, 1 = powered down */
#define LTQ_PMU_MAP_SIZE	0x24
#define LTQ_PMU_TDM		BIT(25)	/* VR9 TDM module gate */

/* GPIO / pin mux (owned by pinctrl-xway; we change single bits only). */
#define LTQ_GPIO_PHYS		0x1e100b10
#define LTQ_GPIO_PORT(p)	(0x30 * (p))
#define LTQ_GPIO_DIR		0x08
#define LTQ_GPIO_ALT0		0x0c
#define LTQ_GPIO_ALT1		0x10
#define LTQ_GPIO_OD		0x14	/* 1 = push-pull, 0 = open drain */
#define LTQ_GPIO_MAP_SIZE	(LTQ_GPIO_PORT(2) + LTQ_GPIO_OD + 4)

#endif /* _TDM_PCM_REGS_H */
