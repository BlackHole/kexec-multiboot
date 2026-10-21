// SPDX-License-Identifier: GPL-2.0
/*
 * vu_fb_tft_spi.c - front TFT of the solo4k (BCM7366).
 *
 * The panel is not on a video output: it is a 320x480 ILI9488-type LCD with
 * its own frame memory, on a 3-wire+D/C SPI bus bit-banged on SoC GPIOs:
 *   GPIO 93 MOSI, 94 RESET, 95 SCK, 96 D/C (0 = command), 97 CS (active low)
 * It is run in landscape (480x320) with 3 bits per pixel (8 colours, two
 * pixels per byte: bits 5..3 and 2..0, R G B from the high bit).
 *
 * A kernel thread polls the source picture and sends the rows that changed:
 *   tft=mirror  fb0 stretched to 480x320, nearest of the 8 colours
 *   tft=own     fb1 480x320 RGB565 of its own
 *   tft=off     panel off
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/string.h>
#include "vu_fb_tft_spi.h"

#define DRV_NAME "vu_fb"

#define GPIO_PHYS   0xF040A400UL     /* bank n at +n*0x20: +0 ODEN, +4 DATA, +8 IODIR ... */
#define PMUX_PHYS   0xF0404000UL     /* SUN_TOP_CTRL pin mux */
#define P_MOSI 93
#define P_RST  94
#define P_SCK  95
#define P_DC   96
#define P_CS   97

#define TW 480
#define TH 320
#define TROW (TW / 2)                /* bytes per packed row */

#define TFT_MADCTL  0xe8             /* landscape: MY|MX|MV|BGR, checked on the solo4k */
#define TFT_POLL_MS 150              /* refresh check interval */

static void __iomem *gpio, *pmux;
static struct task_struct *thr;
static struct fb_info *src_fb, *own_fb;
static void *own_mem;
static u8 *cur, *sent;               /* packed frames: to send / on the panel */
static bool mirror;

/* ------------------------------------------------------------------ GPIO / SPI */
static inline void gset(int pin, int v)
{
	void __iomem *r = gpio + (pin / 32) * 0x20 + 4;
	u32 m = 1u << (pin % 32);

	writel(v ? readl(r) | m : readl(r) & ~m, r);
}

static void ginit(int pin, int v)
{
	void __iomem *b = gpio + (pin / 32) * 0x20;
	u32 m = 1u << (pin % 32);
	static const int clr[] = { 0x08, 0x00, 0x14, 0x0c, 0x10, 0x18 };
	int i;

	writel(v ? readl(b + 4) | m : readl(b + 4) & ~m, b + 4);
	for (i = 0; i < ARRAY_SIZE(clr); i++)
		writel(readl(b + clr[i]) & ~m, b + clr[i]);
}

static void spi_byte(u8 c)
{
	int i;

	for (i = 0; i < 8; i++, c <<= 1) {
		gset(P_SCK, 0);
		gset(P_MOSI, c & 0x80);
		gset(P_SCK, 1);
	}
}

static void lcd_cmd(const u8 *p, int n)
{
	int i;

	gset(P_CS, 0);
	gset(P_DC, 0);
	spi_byte(p[0]);
	gset(P_CS, 1);
	if (n > 1) {
		gset(P_CS, 0);
		gset(P_DC, 1);
		for (i = 1; i < n; i++)
			spi_byte(p[i]);
		gset(P_CS, 1);
	}
}
#define C(...) do { static const u8 _c[] = { __VA_ARGS__ }; lcd_cmd(_c, sizeof(_c)); } while (0)

static void lcd_window(int y0, int y1)
{
	u8 ca[] = { 0x2a, 0, 0, (TW - 1) >> 8, (TW - 1) & 0xff };
	u8 pa[] = { 0x2b, y0 >> 8, y0 & 0xff, y1 >> 8, y1 & 0xff };

	lcd_cmd(ca, sizeof(ca));
	lcd_cmd(pa, sizeof(pa));
}

static void lcd_init(void)
{
	u8 mad[] = { 0x36, TFT_MADCTL };

	writel(readl(pmux + 0x13c) & ~0xfff00000u, pmux + 0x13c);
	writel(readl(pmux + 0x140) & ~0xffu, pmux + 0x140);
	ginit(P_RST, 1); msleep(20);
	gset(P_RST, 0);  msleep(20);
	gset(P_RST, 1);  msleep(120);
	ginit(P_CS, 1); ginit(P_SCK, 1); ginit(P_MOSI, 0); ginit(P_DC, 0);

	C(0xf7, 0xa9, 0x51, 0x2c);
	C(0xc0, 0x1e, 0x19);
	C(0xc1, 0x41);
	C(0xc5, 0x00, 0x26, 0x80);
	C(0xb1, 0xb0, 0x11);
	C(0xb4, 0x02);
	C(0xb5, 0x02, 0x02, 0x0a, 0x03);
	C(0xb6, 0x02, 0x02, 0x3b);
	C(0xb7, 0x06);
	C(0xbe, 0x00, 0x04);
	C(0xe9, 0x00);
	lcd_cmd(mad, sizeof(mad));
	C(0x3a, 0x61);
	C(0xe0, 0x00, 0x07, 0x12, 0x0b, 0x18, 0x0b, 0x3f, 0x9b, 0x4b, 0x0b, 0x0f, 0x0b, 0x15, 0x17, 0x0f);
	C(0xe1, 0x00, 0x16, 0x1b, 0x02, 0x0f, 0x06, 0x34, 0x46, 0x48, 0x04, 0x0d, 0x0d, 0x35, 0x36, 0x0f);
	C(0x11);
	msleep(120);
	C(0x29);
	C(0x51, 0xff);
	C(0x53, 0x24);
	C(0x55, 0x01);
}

/* send rows y0..y1 of cur */
static void lcd_push(int y0, int y1)
{
	int i, n = (y1 - y0 + 1) * TROW;
	const u8 *p = cur + y0 * TROW;

	lcd_window(y0, y1);
	gset(P_CS, 0); gset(P_DC, 0); spi_byte(0x2c); gset(P_CS, 1);
	gset(P_CS, 0); gset(P_DC, 1);
	for (i = 0; i < n; i++) {
		spi_byte(p[i]);
		if ((i & 0x3ff) == 0x3ff)
			cond_resched();
	}
	gset(P_CS, 1);
}

/* ------------------------------------------------------------------ rendering */
/* 3-bit colour: each channel on or off */
static inline int px3(int r, int g, int b)
{
	return (r > 127) << 2 | (g > 127) << 1 | (b > 127);
}

static inline void rgb_at(const struct fb_info *f, const u8 *base, int x, int y, int *r, int *g, int *b)
{
	if (f->var.bits_per_pixel == 32) {
		u32 p = *(const u32 *)(base + y * f->fix.line_length + x * 4);

		*r = (p >> 16) & 0xff; *g = (p >> 8) & 0xff; *b = p & 0xff;
	} else {
		u16 p = *(const u16 *)(base + y * f->fix.line_length + x * 2);

		*r = (p >> 8) & 0xf8; *g = (p >> 3) & 0xfc; *b = (p << 3) & 0xf8;
	}
}

/* stretch the source to 480x320, averaging the 2x2 source block at each sample */
static void render(void)
{
	struct fb_info *f = mirror ? src_fb : own_fb;
	int sw = f->var.xres, sh = f->var.yres, x, y;
	const u8 *base = (const u8 *)f->screen_base +
		(mirror ? f->var.yoffset * f->fix.line_length : 0);

	for (y = 0; y < TH; y++) {
		int sy = y * sh / TH, sy1 = min(sy + 1, sh - 1);
		u8 *o = cur + y * TROW;

		for (x = 0; x < TW; x++) {
			int sx = x * sw / TW, sx1 = min(sx + 1, sw - 1);
			int r, g, b, r1, g1, b1, c;

			rgb_at(f, base, sx, sy, &r, &g, &b);
			if (sw != TW || sh != TH) {
				rgb_at(f, base, sx1, sy, &r1, &g1, &b1); r += r1; g += g1; b += b1;
				rgb_at(f, base, sx, sy1, &r1, &g1, &b1); r += r1; g += g1; b += b1;
				rgb_at(f, base, sx1, sy1, &r1, &g1, &b1); r += r1; g += g1; b += b1;
				r >>= 2; g >>= 2; b >>= 2;
			}
			c = px3(r, g, b);
			if (x & 1)
				o[x / 2] |= c;
			else
				o[x / 2] = c << 3;
		}
	}
}

static int tft_thread(void *unused)
{
	while (!kthread_should_stop()) {
		int y, y0 = -1, y1 = -1;

		render();
		for (y = 0; y < TH; y++)
			if (memcmp(cur + y * TROW, sent + y * TROW, TROW)) {
				if (y0 < 0)
					y0 = y;
				y1 = y;
			}
		if (y0 >= 0) {
			lcd_push(y0, y1);
			memcpy(sent + y0 * TROW, cur + y0 * TROW, (y1 - y0 + 1) * TROW);
		}
		msleep_interruptible(TFT_POLL_MS);
	}
	return 0;
}

/* ------------------------------------------------------------------ fb1 (tft=own) */
static u32 own_palette[16];

static int own_setcolreg(unsigned regno, unsigned red, unsigned green,
			 unsigned blue, unsigned transp, struct fb_info *info)
{
	if (regno >= 16)
		return -EINVAL;
	own_palette[regno] = (red & 0xf800) | ((green >> 5) & 0x07e0) | (blue >> 11);
	return 0;
}

static int own_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	return remap_vmalloc_range(vma, own_mem, vma->vm_pgoff);
}

static struct fb_ops own_ops = {
	.owner        = THIS_MODULE,
	.fb_setcolreg = own_setcolreg,
	.fb_mmap      = own_mmap,
};

static int own_register(void)
{
	struct fb_info *t;
	u32 len = PAGE_ALIGN(TW * TH * 2);

	own_mem = vmalloc_user(len);
	t = own_mem ? framebuffer_alloc(0, NULL) : NULL;
	if (!t) {
		vfree(own_mem);
		own_mem = NULL;
		return -ENOMEM;
	}
	strlcpy(t->fix.id, "vu_fb_tft", sizeof(t->fix.id));
	t->fix.smem_start  = 0;
	t->fix.smem_len    = len;
	t->fix.type        = FB_TYPE_PACKED_PIXELS;
	t->fix.visual      = FB_VISUAL_TRUECOLOR;
	t->fix.accel       = FB_ACCEL_NONE;
	t->fix.line_length = TW * 2;
	t->fbops           = &own_ops;
	t->flags           = FBINFO_DEFAULT | FBINFO_VIRTFB;
	t->screen_base     = (char __iomem *)own_mem;
	t->screen_size     = TW * TH * 2;
	t->pseudo_palette  = own_palette;
	t->var.xres = t->var.xres_virtual = TW;
	t->var.yres = t->var.yres_virtual = TH;
	t->var.bits_per_pixel = 16;
	t->var.red.offset = 11;   t->var.red.length = 5;
	t->var.green.offset = 5;  t->var.green.length = 6;
	t->var.blue.offset = 0;   t->var.blue.length = 5;
	t->var.activate = FB_ACTIVATE_NOW;
	if (register_framebuffer(t)) {
		framebuffer_release(t);
		vfree(own_mem);
		own_mem = NULL;
		return -EINVAL;
	}
	own_fb = t;
	pr_info(DRV_NAME ": fb%d front TFT %dx%d 16 bpp (SPI panel)\n", t->node, TW, TH);
	return 0;
}

/* ------------------------------------------------------------------ start / stop */
int vu_tft_spi_start(const char *mode, struct fb_info *src)
{
	gpio = ioremap(GPIO_PHYS, 0x100);
	pmux = ioremap(PMUX_PHYS, 0x200);
	if (!gpio || !pmux) {
		vu_tft_spi_stop();
		return -ENOMEM;
	}
	lcd_init();
	if (!strcmp(mode, "off")) {
		C(0x28);
		C(0x10);
		pr_info(DRV_NAME ": front TFT off\n");
		return 0;
	}
	mirror = strcmp(mode, "own") != 0;
	src_fb = src;
	cur = kzalloc(TROW * TH, GFP_KERNEL);
	sent = kmalloc(TROW * TH, GFP_KERNEL);
	if (!cur || !sent)
		goto nomem;
	memset(sent, 0xff, TROW * TH);          /* force a full first push */
	if (!mirror && own_register())
		goto nomem;
	thr = kthread_run(tft_thread, NULL, "vu_fb_tft");
	if (IS_ERR(thr)) {
		thr = NULL;
		goto nomem;
	}
	pr_info(DRV_NAME ": front TFT (SPI) %s\n", mirror ? "mirror of fb0" : "own fb");
	return 0;
nomem:
	vu_tft_spi_stop();
	return -ENOMEM;
}

void vu_tft_spi_stop(void)
{
	if (thr) {
		kthread_stop(thr);
		thr = NULL;
	}
	if (own_fb) {
		unregister_framebuffer(own_fb);
		framebuffer_release(own_fb);
		own_fb = NULL;
	}
	vfree(own_mem);
	own_mem = NULL;
	kfree(cur);
	kfree(sent);
	cur = sent = NULL;
	if (gpio)
		iounmap(gpio);
	if (pmux)
		iounmap(pmux);
	gpio = pmux = NULL;
}
