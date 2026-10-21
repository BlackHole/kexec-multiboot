/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vu_fb_modes.h - per SoC / board data for vu_fb
 *
 * BOLT's splash display reads its surface address from RDC scratch
 * registers (offsets from the register window); found in each box's BOLT
 * image with bolt_analysis/bolt_extract.py (vuzero: read on the box, the
 * scratch registers holding GFD0's SRC_START under the CFE splash). The SD
 * path (composite / front TFT) has its own scratch register. Panel windows
 * were measured on the box.
 */
/* RDC lists of a mode loaded by vu_fb: slot, trigger, lists (cnt: nlists + 1 offsets) */
struct vu_mode_rul {
	u8 slot, trig, nlists;
	const u16 *cnt;
	const u32 *list;
};

struct vu_mode {
	const char *soc;
	const char *board;       /* root compatible[0], NULL = any board of the SoC */
	u32 scratch[2];          /* RDC scratch regs feeding GFD0 SRC_START */
	u32 sd_scratch;          /* same for the SD (composite) path, 0 = none */
	u32 sd_scratch2;         /* second SD scratch reg (vusolo2), 0 = none */
	u16 sd_w, sd_h, sd_pitch;
	u8 sd_same;              /* SD display reads the HD surface */
	u16 swm_x, swm_y, swm_w, swm_h; /* tft=mirror on a panel showing only this window of the
	                                    SD frame: software copy of fb0 (w 0 = SD reads fb0) */
	const u32 *reg;          /* modes loaded by vu_fb (bootloader without display): */
	u32 nreg;                /*   register list (offset, value) and RDC lists */
	const struct vu_mode_rul *rul;
	u8 nrul;
	u8 bpp;                  /*   depth of the loaded lists (16 RGB565, 32 ARGB8888) */
	u32 fixed_phys;          /* bootloader splash whose lists write the surface address
	                            themselves (vuuno gen1): draw in it, RGB565, one page */
	u16 fixed_w, fixed_h;
};

#include "vu_fb_modes_7325.h"
#include "vu_fb_modes_7413.h"
#include "vu_fb_modes_7335.h"


static const struct vu_mode vu_modes[] = {
	{ .soc = "brcm,bcm7325",	/* vusolo gen1: CFE starts no display, mode loaded */
	  .reg = bcm7325_splash_reg, .nreg = ARRAY_SIZE(bcm7325_splash_reg) / 2,
	  .rul = bcm7325_splash_rul, .nrul = ARRAY_SIZE(bcm7325_splash_rul), .bpp = BCM7325_SPLASH_BPP },
	{ .soc = "brcm,bcm7335",	/* vuduo gen1: CFE starts no display, mode loaded (lists of the 97335 splash) */
	  .reg = bcm7335_splash_reg, .nreg = ARRAY_SIZE(bcm7335_splash_reg) / 2,
	  .rul = bcm7335_splash_rul, .nrul = ARRAY_SIZE(bcm7335_splash_rul), .bpp = BCM7335_SPLASH_BPP },
	{ .soc = "brcm,bcm7413",	/* vuuno gen1: CFE splash on GFD1 @0x0c800000 (cfe_splash=0: lists) */
	  .fixed_phys = 0x0c800000, .fixed_w = 720, .fixed_h = 576,
	  .reg = bcm7413_splash_reg, .nreg = ARRAY_SIZE(bcm7413_splash_reg) / 2,
	  .rul = bcm7413_splash_rul, .nrul = ARRAY_SIZE(bcm7413_splash_rul), .bpp = BCM7413_SPLASH_BPP },
	{ .soc = "brcm,bcm7445", .board = "DUO4KSE",
	  .scratch = { 0x00603988, 0x0060398c },
	  .sd_scratch = 0x006039a4, .sd_w = 720, .sd_h = 576, .sd_pitch = 1440,
	  .swm_x = 3, .swm_y = 104, .swm_w = 688, .swm_h = 337 },	/* panel: cols 3..690, lines 104..440 */
	{ .soc = "brcm,bcm7445",	/* ultimo4k */
	  .scratch = { 0x00603988, 0x0060398c },
	  .sd_scratch = 0x006039a4, .sd_w = 720, .sd_h = 576, .sd_pitch = 1440,
	  .swm_x = 0, .swm_y = 18, .swm_w = 720, .swm_h = 538 },	/* panel: lines 18..555 */
	{ .soc = "brcm,bcm7366",	/* solo4k (TFT on SPI) */
	  .scratch = { 0x00603988, 0x0060398c },
	  .sd_scratch = 0x006039a4, .sd_w = 720, .sd_h = 576, .sd_pitch = 1440, .sd_same = 1 },
	{ .soc = "brcm,bcm7439",	/* uno4kse, uno4k (no TFT: no software copy) */
	  .scratch = { 0x00603988, 0x0060398c },
	  .sd_scratch = 0x006039a4, .sd_w = 720, .sd_h = 576, .sd_pitch = 1440,
	  .swm_x = 10, .swm_y = 50, .swm_w = 698, .swm_h = 470 },	/* uno4kse panel: cols 10..707, lines 50..519 (tft_win) */
	{ .soc = "brcm,bcm7260",	/* zero4k */
	  .scratch = { 0x00603988, 0x0060398c } },
	{ .soc = "brcm,bcm7362",	/* vuzero (MIPS): CFE splash, scratch 39 and 40 */
	  .scratch = { 0x0060289c, 0x006028a0 } },
	{ .soc = "brcm,bcm7429",	/* vusolose (MIPS): CFE splash, scratch 45/46 (HDMI), 51 (SD) */
	  .scratch = { 0x006028b4, 0x006028b8 },
	  .sd_scratch = 0x006028cc, .sd_w = 720, .sd_h = 576, .sd_pitch = 1440, .sd_same = 1 },
	{ .soc = "brcm,bcm7425",	/* vuduo2 (MIPS, BCM7424): CFE splash, scratch 51/52 (HDMI), 57 (SD) */
	  .scratch = { 0x006028cc, 0x006028d0 },
	  .sd_scratch = 0x006028e4, .sd_w = 720, .sd_h = 576, .sd_pitch = 1440, .sd_same = 1 },
	{ .soc = "brcm,bcm7346",	/* vusolo2 (MIPS, BCM7346, CFE 7356B2): CFE splash, scratch 46/47 (HDMI), 52/53 (SD) */
	  .scratch = { 0x006028b8, 0x006028bc },
	  .sd_scratch = 0x006028d0, .sd_scratch2 = 0x006028d4,
	  .sd_w = 720, .sd_h = 576, .sd_pitch = 1440, .sd_same = 1 },
	{ .soc = "brcm,bcm7278",	/* duo4k */
	  .scratch = { 0x00004918, 0 },
	  .sd_scratch = 0x00004938, .sd_w = 720, .sd_h = 576, .sd_pitch = 1440,
	  .swm_x = 3, .swm_y = 104, .swm_w = 688, .swm_h = 337 },	/* same panel as duo4kse */
};
