// SPDX-License-Identifier: GPL-2.0
/*
 * vu_fb.c - framebuffer for Vu+ boxes without Nexus:
 *   4K (BOLT):        vuultimo4k (BCM7445), vusolo4k (7366), vuuno4k/vuuno4kse
 *                     (7439), vuzero4k (7260), vuduo4k/vuduo4kse (7278)
 *   MIPS gen2 (CFE):  vuzero (73625), vusolose (7429), vuduo2 (7424, family 7425),
 *                     vusolo2 (7346; needs the CFE splash image in flash)
 *   MIPS gen1 (CFE):  vusolo (7325), vuuno and vuultimo (7413)
 *   MIPS gen1 (CFE):  vuduo (7335): lists of the 97335 splash (vu_fb_modes_7335.h)
 *
 * BOLT leaves its boot splash display (576p50, RGB565) running: the
 * register-DMA (RDC) lists it programmed re-load the graphics feeder (GFD0)
 * every vsync, taking the surface address from a few RDC "scratch"
 * registers. vu_fb keeps that display as it is and only points the scratch
 * registers at its own buffer in bmem (reuse of BOLT's display). GFD0's
 * vertical scaler, left by BOLT stretching its 720x480 splash, is set to
 * 576 lines 1:1. Double buffering via FBIOPAN_DISPLAY. The scratch
 * registers of each SoC were found in the box's BOLT image
 * (bolt_analysis/bolt_extract.py).
 *
 * Gen1 MIPS: on the vusolo (BCM7325) the CFE starts no display, so vu_fb
 * loads a 576p50 + PAL display setup itself (register list and RDC lists in
 * vu_fb_modes_7325.h, ARGB8888) and points it at its buffer by patching
 * GFD0's SRC_START in the lists. On vuuno / vuultimo (BCM7413) the CFE
 * splash is on GFD1 at a fixed address: vu_fb draws in it (cfe_splash=1,
 * RGB565, one page); cfe_splash=0 loads the lists of vu_fb_modes_7413.h.
 *
 * vuzero (BCM73625, MIPS, kernel without device tree): the CFE splash works
 * the same way (registers at 0x10000000, same GFD0 layout as the 7445). The
 * chip is recognised by its ID register; its scratch registers were found
 * on the box (the two RDC scratch registers holding GFD0's SRC_START).
 *
 * Front TFT (tft=mirror|own|off):
 *   SD video path, whole picture   solo4k (HDMI and SD read the same
 *                                  surface): mirror = the SD path reads fb0
 *   SD video path, panel window    ultimo4k, uno4kse, duo4kse, duo4k: the panel shows
 *                                  only a window of the SD frame; mirror = a
 *                                  thread scales fb0 into it (software copy);
 *                                  own = fb1 on the SD frame
 *   SPI panel                      solo4k, see vu_fb_tft_spi.c
 *
 * Module parameters:
 *   tft=mirror|own|off  see above (ignored without a TFT)
 *   tft_win=x,y,w,h     panel window of the SD frame (to measure a panel)
 *   mem_phys=0x...      framebuffer address in bmem (default per SoC)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include "vu_fb_tft_spi.h"

/* kernels without device tree (3.x MIPS, CONFIG_OF off) have no of_find_node_by_path() */
#ifdef CONFIG_OF
#define vu_dt_find(path) of_find_node_by_path(path)
#else
static inline struct device_node *vu_dt_find(const char *path)
{
	return NULL;
}
#endif

#define DRV_NAME "vu_fb"

#ifndef pgprot_writecombine             /* older MIPS kernels */
#define pgprot_writecombine pgprot_noncached
#endif

/* own copy of vm_iomap_memory(): it appeared in 3.10, the Vu+ 3.9 kernel has it
 * backported (declared in mm.h), other old kernels do not */
static int vu_iomap_memory(struct vm_area_struct *vma, phys_addr_t start, unsigned long len)
{
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > PAGE_ALIGN(len))
		return -EINVAL;
	return io_remap_pfn_range(vma, vma->vm_start, start >> PAGE_SHIFT, size, vma->vm_page_prot);
}

/* register window: 0xF0000000 on 7445/7366/7439/7260, 0x08000000 on 7278,
 * 0x10000000 on the MIPS chips (7362) */
static ulong bchp_phys = 0xF0000000UL;
#define BCHP_PHYS                 bchp_phys

/*
 * GFD0 base differs per SoC (found in the BOLT lists): 7445/7439/7260 0x641000,
 * 7366 0x681000, 7278 0x024000. Selected at init from the DT compatible.
 */
static ulong gfd_start;
#define GFD_0_REG_START           gfd_start
#define GFD_MAP_SIZE              0x400      /* up to the vertical scaler (+0x2xx) */
/* graphics feeder register layout (offsets from the GFD base, seen in the BOLT lists) */
struct gfd_layout {
	u16 enable, fmt1, fmt2, hsize, start, pitch, disp, vsize, vstep;
};
static const struct gfd_layout gfd_7445 = {	/* 7445/7366/7439/7260 */
	0x08, 0x14, 0x18, 0x44, 0x48, 0x50, 0x5c, 0x204, 0x20c };
static const struct gfd_layout gfd_7278 = {	/* 7278: no enable register checked */
	0, 0xb0, 0xb4, 0xa0, 0x20, 0xa8, 0xac, 0x60, 0x68 };
static const struct gfd_layout gfd_7325 = {	/* 7325: no vertical scaler */
	0x04, 0x10, 0x14, 0x3c, 0x40, 0x44, 0x5c, 0, 0 };
static const struct gfd_layout gfd_7335 = {	/* 7335 (97335 splash lists): as 7325 */
	0x04, 0x10, 0x14, 0x3c, 0x40, 0x44, 0x5c, 0, 0 };
static const struct gfd_layout gfd_7413 = {	/* 7413 (CFE splash, 97413 lists): as 7325 */
	0x04, 0x10, 0x14, 0x3c, 0x40, 0x44, 0x5c, 0, 0 };
static const struct gfd_layout *gl = &gfd_7445;
#define GFD_0_ENABLE              (GFD_0_REG_START + gl->enable)
#define GFD_0_FORMAT_DEF_1        (GFD_0_REG_START + gl->fmt1)
#define GFD_0_FORMAT_DEF_2        (GFD_0_REG_START + gl->fmt2)
#define GFD_0_SRC_HSIZE           (GFD_0_REG_START + gl->hsize)
#define GFD_0_SRC_START           (GFD_0_REG_START + gl->start)
#define GFD_0_SRC_PITCH           (GFD_0_REG_START + gl->pitch)
#define GFD_0_DISP_PIC_SIZE       (GFD_0_REG_START + gl->disp)
#define GFD_0_SRC_VSIZE           (GFD_0_REG_START + gl->vsize)
#define GFD_0_VSTEP               (GFD_0_REG_START + gl->vstep)

struct vu_fb_soc {
	const char *compatible;
	ulong gfd;
	ulong mem;               /* default mem_phys (0 = 0xa4100000) */
	bool tft;                /* front TFT fed by the SD path */
	bool tft_spi;            /* front TFT on bit-banged SPI (solo4k) */
	ulong bchp;              /* register window (0 = 0xF0000000) */
	const struct gfd_layout *gfd_l;   /* NULL = 7445 layout */
	ulong rdc;               /* RDC descriptor 0 (modes with lists to load) */
};

static ulong mem_default, rdc_desc;
static bool soc_tft, soc_tft_spi;

static const struct vu_fb_soc vu_fb_socs[] = {
	{ "brcm,bcm7445", 0x00641000UL, 0, true },	/* ultimo4k (front TFT) */
	{ "brcm,bcm7366", 0x00681000UL, 0x3e000000UL, false, true },	/* solo4k: GFD/RDC read MEMC0 (bmem.0 0x18300000..0x3fbfffff) */
	{ "brcm,bcm7439", 0x00641000UL, 0xbe000000UL, true },	/* uno4kse: display reads MEMC1 (BOLT splash @0xbff57400), bmem.1 0x97e00000..0xbfefffff */
	{ "brcm,bcm7278", 0x00024000UL, 0xfe000000UL, true, false,
	  0x08000000UL, &gfd_7278 },	/* duo4k: BOLT splash @0xffe57300 after bmem.1 0xdc000000..0xffefffff */
	{ "brcm,bcm7260", 0x00641000UL, 0x60000000UL },	/* zero4k: 2GB RAM, bmem 0x52500000.. */
	{ "brcm,bcm7362", 0x00641000UL, 0x0e000000UL, false, false,
	  0x10000000UL },	/* vuzero (MIPS, no DT): bmem.0 0x04000000..0x0fffffff, CFE splash @0x0c800000 */
	{ "brcm,bcm7429", 0x00641000UL, 0x0e000000UL, false, false,
	  0x10000000UL },	/* vusolose (MIPS, no DT): bmem.0 0x04000000..0x0fffffff, CFE splash @0x0c800000 */
	{ "brcm,bcm7425", 0x00641000UL, 0x0e000000UL, false, false,
	  0x10000000UL },	/* vuduo2 (MIPS, no DT): bmem.0 0x04000000..0x0fffffff, CFE splash @0x0c800000 */
	{ "brcm,bcm7346", 0x00641000UL, 0x0e000000UL, false, false,
	  0x10000000UL },	/* vusolo2 (MIPS, no DT): bmem.0 0x04000000..0x0fffffff, CFE splash @0x0c800000 */
	{ "brcm,bcm7325", 0x00141000UL, 0x0e000000UL, false, false,
	  0x10000000UL, &gfd_7325, 0x00102200UL },	/* vusolo gen1 (MIPS, no DT): bmem 0x06400000..0x0fffffff, no CFE display */
	{ "brcm,bcm7335", 0x00141000UL, 0x0e000000UL, false, false,
	  0x10000000UL, &gfd_7335, 0x00102200UL },	/* vuduo gen1 (7325 sibling): bmem 0x08a00000..0x0fffffff, no CFE display */
	{ "brcm,bcm7413", 0x00141000UL, 0x0e000000UL, false, false,
	  0x10000000UL, &gfd_7413, 0x00102200UL },	/* vuuno, vuultimo gen1 (7405 family): bmem 0x01800000..0x0fffffff (ultimo: CFE splash to confirm) */
};

/*
 * Kernels without device tree (MIPS): the chip family from its ID register,
 * as a "brcm,bcmXXXX" name so the tables above match it the same way.
 */
#define MIPS_CHIP_ID_REG  0x10404000UL
static char mips_soc[16];
/* test a box with the data of another SoC, e.g. vuduo (7335, no entry yet):
 * soc_as=brcm,bcm7325 (the vusolo display lists). Only for tests: wrong
 * register lists can hang the box (power cycle) */
static char *soc_as = "";
module_param(soc_as, charp, 0444);
MODULE_PARM_DESC(soc_as, "MIPS test only: use the data of this SoC (e.g. brcm,bcm7325)");

static void __init mips_soc_read(void)
{
	struct device_node *root = vu_dt_find("/");
	void __iomem *r;

	if (root) {             /* DT kernel */
		of_node_put(root);
		return;
	}
	r = ioremap(MIPS_CHIP_ID_REG, 4);
	if (r) {
		u32 v = readl(r);

		/* 0x07362500 -> 7362, 0x74290010 -> 7429 */
		snprintf(mips_soc, sizeof(mips_soc), "brcm,bcm%x", (v >> 28) ? v >> 16 : v >> 12);
		iounmap(r);
		pr_info(DRV_NAME ": no device tree, chip %s\n", mips_soc);
	}
	if (*soc_as) {
		pr_warn(DRV_NAME ": soc_as=%s: using the data of that SoC (test)\n", soc_as);
		snprintf(mips_soc, sizeof(mips_soc), "%s", soc_as);
	}
}

/* boards sharing a SoC with a TFT box but without the panel (first root "compatible") */
static const char * const no_tft_boards[] = { "UNO4K" };

/* true if the board name (first root "compatible", e.g. "DUO4KSE") is name */
static bool board_is(const char *name)
{
	struct device_node *root = vu_dt_find("/");
	const char *c;
	bool ret = false;

	if (root && !of_property_read_string_index(root, "compatible", 0, &c))
		ret = !strcmp(c, name);
	of_node_put(root);
	return ret;
}

static bool __init board_has_no_tft(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(no_tft_boards); i++)
		if (board_is(no_tft_boards[i]))
			return true;
	return false;
}

/* true if a root "compatible" string starts with prefix (brcm,bcm7366 matches brcm,bcm7366c0) */
static bool __init soc_is(const char *prefix)
{
	struct device_node *root;
	const char *c;
	int i;
	bool ret = false;

	if (mips_soc[0])
		return !strcmp(mips_soc, prefix);
	root = vu_dt_find("/");

	for (i = 0; root && !of_property_read_string_index(root, "compatible", i, &c); i++)
		if (!strncmp(c, prefix, strlen(prefix))) {
			ret = true;
			break;
		}
	of_node_put(root);
	return ret;
}

#include "vu_fb_modes.h"

#define MODE_SD_OFFSET   (15 << 20)     /* from mem_phys: SD (front TFT) surface */
#define MODE_RUL_OFFSET  (16 << 20)     /* from mem_phys: RDC lists (modes loaded by vu_fb) */
#define MODE_RUL_SIZE    (1 << 20)
#define MAX_RULS         40
#define RDC_DESC_ADDR(s)       ((s) * 16)
#define RDC_DESC_CONFIG(s)     ((s) * 16 + 4)
#define RDC_DESC_IMMEDIATE(s)  ((s) * 16 + 0xc)
#define RDC_CFG_DONE     0x08000000
#define RDC_CFG_ENABLE   0x04000000
#define RDC_CFG_REPEAT   0x02000000
#define OP(w)            ((w) >> 24)
#define OP_IMM_TO_REG    0x01
#define OP_IMMS_TO_REGS  0x06

static ulong mem_phys;	/* 0: per SoC default */
module_param(mem_phys, ulong, 0444);
MODULE_PARM_DESC(mem_phys, "physical address of the framebuffer, must be in bmem");
static char *tft = "mirror";
module_param(tft, charp, 0444);
MODULE_PARM_DESC(tft, "front TFT: mirror = copy of fb0, own = separate fb1 (ultimo4k 720x288, solo4k 480x320, RGB565), off");


#define MAX_ADDR_REGS 4

struct vu_fb {
	struct fb_info *info;
	void __iomem *gfd;
	void *vaddr;
	u32 paddr;
	size_t size;
	int w, h;

	u32 addr_reg[MAX_ADDR_REGS];       /* scratch regs feeding SRC_START */
	int naddr;

	u32 orig_fmt1, orig_fmt2;
	const struct vu_mode *mode;
	void *sd_mem;
	void __iomem *rdc;                 /* loaded modes: RDC descriptors */
	bool fixed;                        /* drawing in the bootloader's splash surface */
	void *rul_mem;
	struct { u32 *v; u32 cnt; } rul[MAX_RULS];
	int nrul;
	u32 sd_phys;
	u32 pseudo_palette[16];
	struct fb_info *tft;               /* fb1: SD path / front TFT, when not mirrored */
	u32 tft_palette[16];
};

static struct vu_fb *g_fb;

#define GFD(off)  (fb->gfd + ((off) - GFD_0_REG_START))

/* ------------------------------------------------------------------ */
/* physical memory mapping (bmem pages have struct page: use vmap)     */
/* ------------------------------------------------------------------ */
/*
 * MIPS FLATMEM: pfn_valid() reads min_low_pfn, which the 3.9.6 kernels of
 * some boxes do not export (vusolo: "Unknown symbol min_low_pfn"). The same
 * test with max_mapnr (exported; ARCH_PFN_OFFSET is 0 on these boxes).
 */
#if defined(CONFIG_MIPS) && defined(CONFIG_FLATMEM)
#define vu_pfn_valid(pfn) ((pfn) < max_mapnr)
#else
#define vu_pfn_valid(pfn) pfn_valid(pfn)
#endif

static void *map_phys(u32 phys, size_t len)
{
	unsigned long pfn = phys >> PAGE_SHIFT;
	unsigned int off = phys & ~PAGE_MASK;
	unsigned int i, n = PAGE_ALIGN(off + len) >> PAGE_SHIFT;
	struct page **pages;
	void *v;

	for (i = 0; i < n; i++)
		if (!vu_pfn_valid(pfn + i))
			return NULL;
	pages = kmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;
	for (i = 0; i < n; i++)
		pages[i] = pfn_to_page(pfn + i);
	v = vmap(pages, n, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	kfree(pages);
	return v ? v + off : NULL;
}

static void unmap_phys(void *v)
{
	if (v)
		vunmap((void *)((unsigned long)v & PAGE_MASK));
}

static void reg_write_phys(u32 reg, u32 val)
{
	void __iomem *r = ioremap(reg & ~3, 4);

	if (r) {
		writel(val, r);
		iounmap(r);
	}
}


/* SD path reads the framebuffer too (tft=mirror, or SoCs whose SD display
 * reads the HD surface): pan moves it as well */
static bool sd_fb;

static void vu_fb_unmap(struct vu_fb *fb)
{
	unmap_phys(fb->sd_mem);
	fb->sd_mem = NULL;
	unmap_phys(fb->rul_mem);
	fb->rul_mem = NULL;
	fb->nrul = 0;
	if (fb->rdc)
		iounmap(fb->rdc);
	fb->rdc = NULL;
}

/*
 * Modes loaded by vu_fb: change the value the lists write into a register
 * (IMM_TO_REG, or inside an IMMS_TO_REGS block). reg is a register window
 * offset; the lists use the bus form (window | offset). Returns the places.
 */
/* keep=1 (writable, set by bootmenu): at unload leave the picture on screen
 * (HDMI and front TFT keep showing it until the next system's driver) */
/* 1 = vu_fb started the display itself (no bootloader display): the TV still
 * needs a few seconds to lock on the new signal (read by bootmenu) */
static int own_display;
module_param(own_display, int, 0444);
MODULE_PARM_DESC(own_display, "read only: 1 if vu_fb set up the display itself (TV needs time to lock)");

static bool keep;
module_param(keep, bool, 0644);
MODULE_PARM_DESC(keep, "at unload leave the picture on screen (default: black)");

/*
 * Front TFT brightness (4K boxes with a TFT): the front MCU sets the
 * backlight. Original modules (dvb-bcm7444/7278/7376/7252sse,
 * vu_solo4k_lcd_brightness): 8-byte packet 08 <n> 00.., n = v * 42 / 255
 * for oled_brightness v = 0..255. The MCU UART belongs to vu_ir: its
 * vu_mcu_send() is taken with symbol_get, so vu_fb loads without vu_ir.
 * brightness = 1..4 (4 = brightest, n 10/21/32/42), 0 = leave as is.
 */
extern int vu_mcu_send(const u8 *buf, int len);
static int brightness;

static void tft_brightness_apply(void)
{
	static const u8 lv[5] = { 0, 10, 21, 32, 42 };
	int (*send)(const u8 *, int);
	u8 pkt[8] = { 0x08 };

	if (!(soc_tft || soc_tft_spi) || brightness < 1 || brightness > 4)
		return;
	send = symbol_get(vu_mcu_send);
	if (!send) {
		pr_info(DRV_NAME ": brightness: vu_ir not loaded\n");
		return;
	}
	pkt[1] = lv[brightness];
	send(pkt, sizeof(pkt));
	symbol_put(vu_mcu_send);
}

static int vu_fb_set_bright(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (ret)
		return ret;
	if (brightness < 0 || brightness > 4)
		brightness = 4;
	if (g_fb)
		tft_brightness_apply();
	return 0;
}

static const struct kernel_param_ops vu_fb_bright_ops = {
	.set = vu_fb_set_bright,
	.get = param_get_int,
};
module_param_cb(brightness, &vu_fb_bright_ops, &brightness, 0644);
MODULE_PARM_DESC(brightness, "front TFT brightness 1..4, 4 = brightest (0 = leave as is); 4K boxes with a TFT, needs vu_ir");

static int cfe_splash = 1;
module_param(cfe_splash, int, 0444);
MODULE_PARM_DESC(cfe_splash, "boxes whose bootloader shows a splash with fixed address (vuuno, vuultimo gen1): 1 = draw in it, 0 = load own display lists");

static int vu_fb_rul_set(struct vu_fb *fb, u32 reg, u32 val)
{
	int k, i, n = 0;

	for (k = 0; k < fb->nrul; k++) {
		u32 *w = fb->rul[k].v;
		int cnt = fb->rul[k].cnt;

		for (i = 0; i + 2 < cnt; i++) {
			u32 base = w[i + 1] & 0x0fffffff;

			if (OP(w[i]) == OP_IMM_TO_REG && base == reg) {
				w[i + 2] = val;
				n++;
			} else if (OP(w[i]) == OP_IMMS_TO_REGS) {
				u32 num = (w[i] & 0xfff) + 1, j;

				for (j = 0; j < num && i + 2 + j < cnt; j++)
					if (base + 4 * j == reg) {
						w[i + 2 + j] = val;
						n++;
					}
			}
		}
	}
	wmb();
	return n;
}

/*
 * Chips whose bootloader starts no display (BCM7325): write the register
 * list, then copy the RDC lists to bmem and chain them per slot (each list
 * ends by loading the next one into its descriptor), as the bootloader
 * splash code does. Format and pitch as in the lists, surface address patched.
 */
static int vu_fb_load_mode(struct vu_fb *fb, const struct vu_mode *m)
{
	u32 rul_phys = mem_phys + MODE_RUL_OFFSET, off = 0;
	u32 rul_bus = BCHP_PHYS;        /* the lists use bus addresses window | offset */
	u32 *mem;
	int k, i;

	fb->rdc = ioremap(BCHP_PHYS + rdc_desc, 0x400);
	fb->rul_mem = mem = map_phys(rul_phys, MODE_RUL_SIZE);
	if (!fb->rdc || !mem)
		return -ENOMEM;

	for (k = 0; k < m->nrul; k++)           /* stop the slots */
		writel(RDC_CFG_DONE, fb->rdc + RDC_DESC_CONFIG(m->rul[k].slot));
	wmb();
	msleep(60);
	for (i = 0; i < m->nreg; i++)
		reg_write_phys(BCHP_PHYS + m->reg[2 * i], m->reg[2 * i + 1]);

	fb->nrul = 0;
	for (k = 0; k < m->nrul; k++) {
		const struct vu_mode_rul *r = &m->rul[k];
		u32 first = 0, first_cnt = 0, *prev = NULL;

		for (i = 0; i < r->nlists; i++) {
			u32 n = r->cnt[i + 1] - r->cnt[i];
			u32 phys = rul_phys + off;
			u32 *v = mem + off / 4;
			bool last = (i == r->nlists - 1);

			if (off + (n + 6) * 4 > MODE_RUL_SIZE)
				return -ENOSPC;
			memcpy(v, r->list + r->cnt[i], n * 4);
			if (prev) {             /* previous list loads this one */
				u32 *p = prev;

				*p++ = OP_IMM_TO_REG << 24;
				*p++ = rul_bus + rdc_desc + RDC_DESC_ADDR(r->slot);
				*p++ = phys;
				*p++ = OP_IMM_TO_REG << 24;
				*p++ = rul_bus + rdc_desc + RDC_DESC_CONFIG(r->slot);
				*p++ = ((last ? n : n + 6) - 1) | (r->trig << 16) |
				       RDC_CFG_REPEAT | RDC_CFG_ENABLE | RDC_CFG_DONE;
			} else {
				first = phys;
				first_cnt = r->nlists > 1 ? n + 6 : n;
			}
			prev = v + n;
			if (fb->nrul < MAX_RULS) {
				fb->rul[fb->nrul].v = v;
				fb->rul[fb->nrul].cnt = n;
				fb->nrul++;
			}
			off = ALIGN(off + (n + 6) * 4, 32);
		}
		wmb();
		writel(first, fb->rdc + RDC_DESC_ADDR(r->slot));
		writel((first_cnt - 1) | (r->trig << 16) |
		       RDC_CFG_REPEAT | RDC_CFG_ENABLE | RDC_CFG_DONE,
		       fb->rdc + RDC_DESC_CONFIG(r->slot));
	}
	vu_fb_rul_set(fb, GFD_0_SRC_START, fb->paddr);
	wmb();
	for (k = 0; k < m->nrul; k++)           /* kick the top-field slots */
		if (!(m->rul[k].slot & 1))
			writel(1, fb->rdc + RDC_DESC_IMMEDIATE(m->rul[k].slot));
	wmb();
	msleep(200);
	fb->mode = m;
	pr_info(DRV_NAME ": display mode loaded: %u regs, %d lists (%u bytes) @%08x\n",
		m->nreg, fb->nrul, off, rul_phys);
	return 0;
}

/* ------------------------------------------------------------------ */
/* display mode of the SoC / board                                     */
/* ------------------------------------------------------------------ */
static const struct vu_mode *vu_fb_find_mode(void)
{
	int i, pass;

	/* pass 0: entry for this board; pass 1: any board of the SoC */
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < ARRAY_SIZE(vu_modes); i++) {
			const struct vu_mode *m = &vu_modes[i];

			if (!soc_is(m->soc))
				continue;
			if (pass == 0 ? (m->board && board_is(m->board)) : !m->board) {
				if (m->board)
					pr_info(DRV_NAME ": %s board\n", m->board);
				return m;
			}
		}
	return NULL;
}

/*
 * Software mirror (duo4k, duo4kse: the front panel shows only a window of the
 * SD frame): the SD path reads a frame of its own and a thread scales fb0
 * into the visible window a few times a second, writing only the lines that
 * changed. HDMI keeps reading fb0 1:1.
 */
static bool swm;
static struct task_struct *swm_task;
static u16 win_x, win_y, win_w, win_h;   /* swm window: mode values or tft_win= */

/* tft_win=x,y,w,h overrides the panel window of the mode (to measure a panel) */
static int tft_win[4];
static int tft_win_n;
module_param_array(tft_win, int, &tft_win_n, 0444);
MODULE_PARM_DESC(tft_win, "x,y,w,h of the SD frame the front panel shows (tft=mirror software copy); default per board");

static void swm_window(const struct vu_mode *m)
{
	win_x = m->swm_x, win_y = m->swm_y, win_w = m->swm_w, win_h = m->swm_h;
	if (tft_win_n == 4 && tft_win[2] > 0 && tft_win[3] > 0 &&
	    tft_win[0] >= 0 && tft_win[1] >= 0 &&
	    tft_win[0] + tft_win[2] <= m->sd_w && tft_win[1] + tft_win[3] <= m->sd_h)
		win_x = tft_win[0], win_y = tft_win[1], win_w = tft_win[2], win_h = tft_win[3];
}

static int swm_thread(void *data)
{
	struct vu_fb *fb = data;
	const struct vu_mode *m = fb->mode;
	struct fb_info *info = fb->info;
	u32 sp = m->sd_pitch;                       /* SD frame line, RGB565 */
	u16 *src = kmalloc(fb->w * 4, GFP_KERNEL);  /* one fb0 line, cached */
	u16 *dst = kmalloc(win_w * 2, GFP_KERNEL);
	int x, y;

	if (!src || !dst)
		goto out;
	while (!kthread_should_stop()) {
		int bpp = info->var.bits_per_pixel;
		u32 pitch = info->fix.line_length;
		const u8 *base = (const u8 *)fb->vaddr + info->var.yoffset * pitch;

		for (y = 0; y < win_h; y++) {
			int sy = y * fb->h / win_h;
			u16 *d = (u16 *)((u8 *)fb->sd_mem + (win_y + y) * sp) + win_x;

			memcpy(src, base + sy * pitch, fb->w * bpp / 8);
			for (x = 0; x < win_w; x++) {
				int sx = x * fb->w / win_w;

				if (bpp == 16) {
					dst[x] = src[sx];
				} else {
					u32 p = ((u32 *)src)[sx];

					dst[x] = ((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x1f);
				}
			}
			if (memcmp(d, dst, win_w * 2))
				memcpy(d, dst, win_w * 2);
			if (!(y & 31))
				cond_resched();
		}
		msleep_interruptible(100);
	}
out:
	kfree(src);
	kfree(dst);
	while (!kthread_should_stop())
		msleep_interruptible(100);
	return 0;
}

/* SD path reads fb0 (tft=mirror); false with a software copy (swm) or no mirror */
static bool sd_mirror(const struct vu_mode *m)
{
	bool mirror = m->sd_scratch && (m->sd_same || (soc_tft && !strcmp(tft, "mirror")));

	swm_window(m);
	if (mirror && win_w && !m->sd_same) {
		swm = true;             /* the SD path reads its own frame, filled by swm_thread */
		return false;
	}
	return mirror;
}

/*
 * Keep the display BOLT left running (splash): its lists stay in place and
 * read the surface from the scratch registers, so only those are moved to
 * the framebuffer. Format stays BOLT's RGB565.
 */
static int vu_fb_reuse_bolt(struct vu_fb *fb, const struct vu_mode *m)
{
	int i;

	fb->sd_phys = mem_phys + MODE_SD_OFFSET;
	sd_fb = sd_mirror(m);
	if (m->sd_scratch && !sd_fb) {
		fb->sd_mem = map_phys(fb->sd_phys, m->sd_pitch * m->sd_h);
		if (fb->sd_mem)
			memset(fb->sd_mem, 0, m->sd_pitch * m->sd_h);
	}
	fb->naddr = 0;
	for (i = 0; i < 2; i++)
		if (m->scratch[i]) {
			fb->addr_reg[fb->naddr++] = BCHP_PHYS + m->scratch[i];
		}
	if (sd_fb) {
		fb->addr_reg[fb->naddr++] = BCHP_PHYS + m->sd_scratch;
		if (m->sd_scratch2)
			fb->addr_reg[fb->naddr++] = BCHP_PHYS + m->sd_scratch2;
	} else if (m->sd_scratch) {
		reg_write_phys(BCHP_PHYS + m->sd_scratch, fb->sd_mem ? fb->sd_phys : fb->paddr);
		if (m->sd_scratch2)
			reg_write_phys(BCHP_PHYS + m->sd_scratch2, fb->sd_mem ? fb->sd_phys : fb->paddr);
	}
	fb->mode = m;
	pr_info(DRV_NAME ": reusing the bootloader's display (scratch %08x)%s\n",
		m->scratch[0],
		sd_fb ? ", SD path mirrors the framebuffer" :
		swm ? ", TFT: software copy of fb0" : "");
	return 0;
}

/* ------------------------------------------------------------------ */
/* hardware programming                                                 */
/* ------------------------------------------------------------------ */
static void vu_fb_set_start(struct vu_fb *fb, u32 phys)
{
	int a;

	if (fb->fixed)                  /* the bootloader's lists read a fixed address */
		return;
	for (a = 0; a < fb->naddr; a++)
		reg_write_phys(fb->addr_reg[a], phys);
	if (fb->nrul)                   /* loaded mode: the lists write SRC_START */
		vu_fb_rul_set(fb, GFD_0_SRC_START, phys);
	writel(phys, GFD(GFD_0_SRC_START));
	wmb();
}

static void vu_fb_set_format(struct vu_fb *fb, int depth, u32 pitch)
{
	u32 type = fb->orig_fmt1 & 0x00070000;
	u32 f1 = type | 0x0565;              /* R5 G6 B5, as BOLT */
	u32 f2 = ((fb->orig_fmt1 & 0xffff) == 0x0565) ? fb->orig_fmt2 : 0x000b0500;

	if (fb->fixed) {
		pr_info(DRV_NAME ": %d bpp, pitch %u in the bootloader's splash surface\n", depth, pitch);
		return;
	}
	if (fb->nrul) {                 /* loaded mode: format and pitch set by the lists */
		pr_info(DRV_NAME ": format %d bpp, pitch %u (from the loaded lists)\n", depth, pitch);
		return;
	}

	pr_info(DRV_NAME ": format %d bpp: FORMAT_DEF_1=%08x FORMAT_DEF_2=%08x pitch=%u\n",
		depth, f1, f2, pitch);
	writel(f1, GFD(GFD_0_FORMAT_DEF_1));
	writel(f2, GFD(GFD_0_FORMAT_DEF_2));
	writel(pitch, GFD(GFD_0_SRC_PITCH));
	wmb();
	/*
	 * The BOLT splash image is 720x480: GFD0's vertical scaler reads 480
	 * source lines (+0x204) and stretches them to the 576-line display
	 * (+0x20c = 480/576 in 1.20 fixed point), so the bottom 96 lines of a
	 * 576-line framebuffer were never shown. Read all lines 1:1 instead.
	 */
	if (!gl->vsize)
		return;
	/* BOLT's lists write these only once at start-up */
	writel(fb->h, GFD(GFD_0_SRC_VSIZE));
	writel(0x100000, GFD(GFD_0_VSTEP));
	wmb();
	pr_info(DRV_NAME ": vertical scaler %u lines 1:1\n", fb->h);
}

/* ------------------------------------------------------------------ */
/* fbdev ops                                                            */
/* ------------------------------------------------------------------ */
static void vu_fb_fill_var(struct vu_fb *fb, struct fb_var_screeninfo *var, int depth)
{
	var->xres = var->xres_virtual = fb->w;
	var->yres = fb->h;
	if (var->yres_virtual != fb->h * 2)
		var->yres_virtual = fb->h;
	var->xoffset = 0;
	if (var->yoffset + var->yres > var->yres_virtual)
		var->yoffset = 0;
	var->bits_per_pixel = depth;
	var->grayscale = 0;
	var->nonstd = 0;
	var->height = var->width = -1;
	if (depth == 32) {
		var->transp.offset = 24; var->transp.length = 8;
		var->red.offset = 16;    var->red.length = 8;
		var->green.offset = 8;   var->green.length = 8;
		var->blue.offset = 0;    var->blue.length = 8;
	} else {
		var->transp.offset = 0;  var->transp.length = 0;
		var->red.offset = 11;    var->red.length = 5;
		var->green.offset = 5;   var->green.length = 6;
		var->blue.offset = 0;    var->blue.length = 5;
	}
	var->red.msb_right = var->green.msb_right = var->blue.msb_right = var->transp.msb_right = 0;
}

static int vu_fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct vu_fb *fb = info->par;
	int depth = fb->nrul && fb->mode->bpp ? fb->mode->bpp : 16;	/* BOLT's display: RGB565 */

	if (var->xres != fb->w || var->yres != fb->h)
		pr_info(DRV_NAME ": %ux%u requested, only %dx%d supported\n",
			var->xres, var->yres, fb->w, fb->h);
	vu_fb_fill_var(fb, var, depth);
	if ((size_t)fb->w * var->yres_virtual * (depth / 8) > fb->size)
		var->yres_virtual = fb->h;
	return 0;
}

static int vu_fb_set_par(struct fb_info *info)
{
	struct vu_fb *fb = info->par;
	struct fb_var_screeninfo *var = &info->var;
	u32 pitch = fb->w * (var->bits_per_pixel / 8);

	info->fix.line_length = pitch;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.ypanstep = 1;
	vu_fb_set_format(fb, var->bits_per_pixel, pitch);
	vu_fb_set_start(fb, fb->paddr + var->yoffset * pitch);
	return 0;
}

static int vu_fb_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct vu_fb *fb = info->par;

	if (var->yoffset + info->var.yres > info->var.yres_virtual)
		return -EINVAL;
	vu_fb_set_start(fb, fb->paddr + var->yoffset * info->fix.line_length);
	return 0;
}

static int vu_fb_setcolreg(unsigned regno, unsigned red, unsigned green,
			   unsigned blue, unsigned transp, struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;

	if (regno >= 16)
		return -EINVAL;
	if (info->var.bits_per_pixel == 32)
		pal[regno] = 0xff000000 | ((red >> 8) << 16) | ((green >> 8) << 8) | (blue >> 8);
	else
		pal[regno] = ((red >> 11) << 11) | ((green >> 10) << 5) | (blue >> 11);
	return 0;
}

static int vu_fb_blank(int blank, struct fb_info *info)
{
	return 0;   /* accept, nothing to do */
}

static int vu_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct vu_fb *fb = info->par;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return vu_iomap_memory(vma, fb->paddr, fb->size);
}

static int vu_fb_tft_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return vu_iomap_memory(vma, info->fix.smem_start, info->fix.smem_len);
}

static struct fb_ops vu_fb_tft_ops = {
	.owner          = THIS_MODULE,
	.fb_setcolreg   = vu_fb_setcolreg,
	.fb_mmap        = vu_fb_tft_mmap,
};

/*
 * Second framebuffer for the front TFT: the SD display reads its own
 * surface (a full frame, RGB565; BOLT's SD lists read every other line
 * per field) - tft=own.
 */
static void vu_fb_tft_register(struct vu_fb *fb)
{
	const struct vu_mode *m = fb->mode;
	struct fb_info *t;
	u32 w = m->sd_w, h = m->sd_h, pitch = m->sd_pitch;

	t = framebuffer_alloc(0, NULL);
	if (!t)
		return;
	strlcpy(t->fix.id, "vu_fb_tft", sizeof(t->fix.id));
	t->fix.smem_start = fb->sd_phys;
	t->fix.smem_len   = PAGE_ALIGN(pitch * h);
	t->fix.type       = FB_TYPE_PACKED_PIXELS;
	t->fix.visual     = FB_VISUAL_TRUECOLOR;
	t->fix.accel      = FB_ACCEL_NONE;
	t->fix.line_length = pitch;
	t->fbops          = &vu_fb_tft_ops;
	t->flags          = FBINFO_DEFAULT;
	t->screen_base    = (char __iomem *)fb->sd_mem;
	t->screen_size    = pitch * h;
	t->pseudo_palette = fb->tft_palette;
	t->var.xres = t->var.xres_virtual = w;
	t->var.yres = t->var.yres_virtual = h;
	t->var.bits_per_pixel = 16;
	t->var.red.offset = 11;   t->var.red.length = 5;
	t->var.green.offset = 5;  t->var.green.length = 6;
	t->var.blue.offset = 0;   t->var.blue.length = 5;
	t->var.activate = FB_ACTIVATE_NOW;
	if (register_framebuffer(t)) {
		framebuffer_release(t);
		return;
	}
	fb->tft = t;
	pr_info(DRV_NAME ": fb%d front TFT %ux%u 16 bpp @%08x\n", t->node, w, h, fb->sd_phys);
}

static struct fb_ops vu_fb_ops = {
	.owner          = THIS_MODULE,
	.fb_check_var   = vu_fb_check_var,
	.fb_set_par     = vu_fb_set_par,
	.fb_pan_display = vu_fb_pan_display,
	.fb_setcolreg   = vu_fb_setcolreg,
	.fb_blank       = vu_fb_blank,
	.fb_mmap        = vu_fb_mmap,
};

static void vu_fb_test_pattern(struct vu_fb *fb, int depth)
{
	static const u32 bars[] = { 0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
				    0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000 };
	int x, y, c, w = fb->w / ARRAY_SIZE(bars);

	for (y = 0; y < fb->h; y++)
		for (x = 0; x < fb->w; x++) {
			u32 p = bars[min_t(int, x / w, ARRAY_SIZE(bars) - 1)];

			if (depth == 32) {
				((u32 *)fb->vaddr)[y * fb->w + x] = p;
			} else {
				c = ((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x1f);
				((u16 *)fb->vaddr)[y * fb->w + x] = c;
			}
		}
}

/* ------------------------------------------------------------------ */
static int __init vu_fb_init(void)
{
	struct vu_fb *fb;
	struct fb_info *info;
	int ret, k, depth = 16;

	info = framebuffer_alloc(sizeof(*fb), NULL);
	if (!info)
		return -ENOMEM;
	fb = info->par;
	fb->info = info;
	g_fb = fb;

	mips_soc_read();
	for (k = 0; !gfd_start && k < ARRAY_SIZE(vu_fb_socs); k++)
		if (soc_is(vu_fb_socs[k].compatible))
			gfd_start = vu_fb_socs[k].gfd,
			mem_default = vu_fb_socs[k].mem,
			soc_tft = vu_fb_socs[k].tft, soc_tft_spi = vu_fb_socs[k].tft_spi,
			bchp_phys = vu_fb_socs[k].bchp ? vu_fb_socs[k].bchp : bchp_phys,
			gl = vu_fb_socs[k].gfd_l ? vu_fb_socs[k].gfd_l : gl,
			rdc_desc = vu_fb_socs[k].rdc;
	if (soc_tft && board_has_no_tft()) {
		pr_info(DRV_NAME ": board without front TFT\n");
		soc_tft = false;
	}
	if (!gfd_start) {
		pr_err(DRV_NAME ": SoC not supported (no display mode)\n");
		ret = -ENODEV;
		goto err;
	}
	pr_info(DRV_NAME ": GFD0 at %08lx\n", BCHP_PHYS + gfd_start);

	fb->gfd = ioremap(BCHP_PHYS + GFD_0_REG_START, GFD_MAP_SIZE);
	if (!fb->gfd) {
		ret = -ENOMEM;
		goto err;
	}

	if (!mem_phys)
		mem_phys = mem_default ? mem_default : 0xa4100000UL;
	fb->paddr = mem_phys;
	{
		const struct vu_mode *m = vu_fb_find_mode();

		if (!m) {
			pr_err(DRV_NAME ": no display mode for this SoC/board\n");
			ret = -EINVAL;
			goto err;
		}
		if (m->fixed_phys && cfe_splash) {
			fb->fixed = true;
			fb->mode = m;
			fb->paddr = m->fixed_phys;
			fb->w = m->fixed_w;
			fb->h = m->fixed_h;
			pr_info(DRV_NAME ": drawing in the bootloader's splash @%08x\n", fb->paddr);
			ret = 0;
		} else if (m->rul && rdc_desc) {
			ret = vu_fb_load_mode(fb, m);
			own_display = !ret;
		} else if (gl->enable && !(readl(GFD(GFD_0_ENABLE)) & 1)) {
			/* check before touching the scratch registers */
			pr_err(DRV_NAME ": GFD0 not enabled - the bootloader's display is not running\n");
			ret = -ENODEV;
		} else {
			ret = vu_fb_reuse_bolt(fb, m);
		}
		if (ret)
			goto err;
	}
	if (fb->fixed)
		goto have_size;
	if (gl->enable && !(readl(GFD(GFD_0_ENABLE)) & 1)) {
		pr_err(DRV_NAME ": GFD0 not enabled - the bootloader's display is not running\n");
		ret = -ENODEV;
		goto err;
	}
	fb->orig_fmt1  = readl(GFD(GFD_0_FORMAT_DEF_1));
	fb->orig_fmt2  = readl(GFD(GFD_0_FORMAT_DEF_2));
	fb->w = readl(GFD(GFD_0_SRC_HSIZE)) & 0x1fff;
	fb->h = readl(GFD(GFD_0_DISP_PIC_SIZE)) & 0xfff;
	pr_info(DRV_NAME ": bootloader's display %dx%d fmt=%08x/%08x\n",
		fb->w, fb->h, fb->orig_fmt1, fb->orig_fmt2);
	if (!fb->w || !fb->h) {
		ret = -ENODEV;
		goto err;
	}
have_size:

	/* two pages for double buffering, at the depth of the mode */
	if (fb->nrul && fb->mode->bpp)
		depth = fb->mode->bpp;
	fb->size = PAGE_ALIGN(fb->w * fb->h * (depth / 8) * (fb->fixed ? 1 : 2));
	fb->vaddr = map_phys(fb->paddr, fb->size);
	if (!fb->vaddr) {
		pr_err(DRV_NAME ": cannot map %zu bytes at %08x\n", fb->size, fb->paddr);
		ret = -ENOMEM;
		goto err;
	}
	memset(fb->vaddr, 0, fb->size);

	strlcpy(info->fix.id, "vu_fb", sizeof(info->fix.id));
	info->fix.smem_start = fb->paddr;
	info->fix.smem_len   = fb->size;
	info->fix.type       = FB_TYPE_PACKED_PIXELS;
	info->fix.visual     = FB_VISUAL_TRUECOLOR;
	info->fix.accel      = FB_ACCEL_NONE;
	info->fix.ypanstep   = 1;
	info->fbops          = &vu_fb_ops;
	info->flags          = FBINFO_DEFAULT;
	info->screen_base    = (char __iomem *)fb->vaddr;
	info->screen_size    = fb->size;
	info->pseudo_palette = fb->pseudo_palette;

	info->var.yres_virtual = fb->fixed ? fb->h : fb->h * 2;
	vu_fb_fill_var(fb, &info->var, depth);
	info->var.activate = FB_ACTIVATE_NOW;

	vu_fb_test_pattern(fb, depth);
	vu_fb_set_par(info);

	ret = register_framebuffer(info);
	if (ret)
		goto err;

	if (soc_tft && !strcmp(tft, "own") && fb->mode->sd_scratch && !sd_fb && fb->sd_mem)
		vu_fb_tft_register(fb);
	if (swm && fb->sd_mem) {
		swm_task = kthread_run(swm_thread, fb, "vu_fb_tft");
		if (IS_ERR(swm_task))
			swm_task = NULL;
		else
			pr_info(DRV_NAME ": TFT copy %dx%d -> %ux%u at %u,%u\n", fb->w, fb->h,
				win_w, win_h, win_x, win_y);
	}
	if (soc_tft_spi && vu_tft_spi_start(tft, info))
		pr_warn(DRV_NAME ": front TFT (SPI) not started\n");

	msleep(100);
	pr_info(DRV_NAME ": fb%d %dx%d %d bpp @%08x; GFD start=%08x fmt=%08x/%08x pitch=%u\n",
		info->node, fb->w, fb->h, depth, fb->paddr,
		readl(GFD(GFD_0_SRC_START)), readl(GFD(GFD_0_FORMAT_DEF_1)),
		readl(GFD(GFD_0_FORMAT_DEF_2)), readl(GFD(GFD_0_SRC_PITCH)));
	tft_brightness_apply();
	return 0;

err:
	unmap_phys(fb->vaddr);
	vu_fb_unmap(fb);
	if (fb->gfd)
		iounmap(fb->gfd);
	framebuffer_release(info);
	g_fb = NULL;
	return ret;
}

static void __exit vu_fb_exit(void)
{
	struct vu_fb *fb = g_fb;

	if (swm_task) {
		kthread_stop(swm_task);
		swm_task = NULL;
	}
	if (!fb)
		return;
	if (soc_tft_spi)
		vu_tft_spi_stop();
	if (fb->tft) {
		unregister_framebuffer(fb->tft);
		framebuffer_release(fb->tft);
	}
	unregister_framebuffer(fb->info);

	/* BOLT's display keeps reading our buffer: leave it black (keep=0) */
	if (!keep)
		memset(fb->vaddr, 0, fb->size);

	unmap_phys(fb->vaddr);
	vu_fb_unmap(fb);
	iounmap(fb->gfd);
	framebuffer_release(fb->info);
	g_fb = NULL;
}

module_init(vu_fb_init);
module_exit(vu_fb_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Native framebuffer for Vu+ 4K reusing the bootloader's display pipeline");
