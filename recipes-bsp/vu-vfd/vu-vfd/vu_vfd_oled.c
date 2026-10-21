// SPDX-License-Identifier: GPL-2.0
/*
 * vu_vfd_oled.c - text backend of vu_vfd for the vuduo2 (BCM7424): 140x32
 *                 monochrome OLED on the first MSPI (UPG), chip select on
 *                 gpio 82. Included by vu_vfd.c when VU_VFD_OLED is set.
 *                 (The colour TFT of the vuduo2 is the separate vu_tft.)
 *
 * Everything below was taken from the original module (dvb-bcm7424.ko of
 * the vuduo2: bm750_dbox_lcd_init, the /dev/oled0 write and its frame
 * sender), not from an SDK.
 *
 * MSPI core 0 (base 0x10409200, 27 MHz): SPCR0_LSB 8, SPCR0_MSB 0x83,
 *   CDRAM 0x06 (PCS 0), one byte per transfer, as the original.
 * gpio 82 (bank 0x10406740 bit 24): low during a frame.
 * Frame: 16 bytes 01 00.., then 576 bytes: for each group g of 4 columns
 *   (0..35, x = 4g..4g+3 of 144, visible 2..141) and each row pair
 *   (0..15, rows 2k, 2k+1) one byte, bit = pixel on:
 *     even g: row 2k   x+0 0x04, x+1 0x08, x+2 0x01, x+3 0x02
 *             row 2k+1 x+0 0x40, x+1 0x80, x+2 0x10, x+3 0x20
 *     odd g:  row 2k   x+0..3 0x01, 0x02, 0x04, 0x08
 *             row 2k+1 x+0..3 0x10, 0x20, 0x40, 0x80
 * Brightness: 16 bytes a0 <n> 00..
 * At init the original also sends 22 03 (display on) to the front MCU.
 */
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include "vu_mcu.h"
#include "vu_vfd_font.h"

#define OLED_W        140
#define OLED_H        32
#define OLED_X0       2		/* first visible column of 144 */

#define MSPI_BASE     0x10409200UL
#define GPIO_BANK     0x10406740UL
#define GPIO_DATA     0x4
#define GPIO_ODEN     0x0
#define GPIO_IODIR    0x8
#define GPIO_CS       BIT(82 - 58)

#define MSPI_SPCR0_LSB 0x000
#define MSPI_SPCR0_MSB 0x004
#define MSPI_SPCR1_LSB 0x008
#define MSPI_SPCR1_MSB 0x00c
#define MSPI_NEWQP     0x010
#define MSPI_ENDQP     0x014
#define MSPI_SPCR2     0x018
#define MSPI_STATUS    0x020
#define MSPI_TXRAM(i)  (0x040 + 8 * (i))
#define MSPI_CDRAM(i)  (0x140 + 4 * (i))
#define MSPI_PCS_BITS  0x06

static int spi_div = 8;
module_param(spi_div, int, 0444);
MODULE_PARM_DESC(spi_div, "OLED: MSPI divider, clock = 27 MHz / (2 * n) (original: 8)");

static void __iomem *mspi, *gpio;
static u8 oled_px[OLED_H][144];

static int mspi_byte(u8 b)
{
	int t;

	writel(b, mspi + MSPI_TXRAM(0));
	writel(MSPI_PCS_BITS, mspi + MSPI_CDRAM(0));
	writel(0, mspi + MSPI_NEWQP);
	writel(0, mspi + MSPI_ENDQP);
	writel(readl(mspi + MSPI_SPCR2) | 0x40, mspi + MSPI_SPCR2);
	for (t = 0; t < 10000; t++) {
		if (readl(mspi + MSPI_STATUS) & 1)
			break;
		udelay(1);
	}
	writel(0, mspi + MSPI_STATUS);
	return t < 10000 ? 0 : -ETIMEDOUT;
}

static void oled_cs(int on)
{
	u32 v = readl(gpio + GPIO_DATA);

	writel(on ? (v & ~GPIO_CS) : (v | GPIO_CS), gpio + GPIO_DATA);
}

/* 16-byte command: c, arg, zeros (frame header: 01; brightness: a0 n) */
static int oled_hdr(u8 c, u8 arg)
{
	int i, ret = mspi_byte(c);

	for (i = 1; i < 16 && !ret; i++)
		ret = mspi_byte(i == 1 ? arg : 0);
	return ret;
}

static int oled_flush(void)
{
	static const u8 even[8] = { 0x04, 0x08, 0x01, 0x02, 0x40, 0x80, 0x10, 0x20 };
	static const u8 odd[8]  = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
	int g, k, i, ret;

	oled_cs(1);
	ret = oled_hdr(0x01, 0);
	for (g = 0; g < 36 && !ret; g++) {
		const u8 *m = (g & 1) ? odd : even;

		for (k = 0; k < 16 && !ret; k++) {
			u8 b = 0;

			for (i = 0; i < 8; i++)
				if (oled_px[2 * k + i / 4][4 * g + (i & 3)])
					b |= m[i];
			ret = mspi_byte(b);
		}
		cond_resched();
	}
	oled_cs(0);
	return ret;
}

static void oled_char(int x, int y, char c, int s)
{
	const u8 *g;
	int r, b;

	if (c < 0x20 || c > 0x7e)
		c = ' ';
	g = vu_vfd_font[c - 0x20];
	for (r = 0; r < 16 * s; r++)
		for (b = 0; b < 8 * s; b++) {
			int px = OLED_X0 + x + b, py = y + r;

			if ((g[r / s] & (0x80 >> (b / s))) && px >= OLED_X0 &&
			    px < OLED_X0 + OLED_W && py >= 0 && py < OLED_H)
				oled_px[py][px] = 1;
		}
}

/*
 * Text layout: "\n" splits lines. One line of at most 8 characters is
 * drawn large (16x32), otherwise up to 2 lines of 17 characters (8x16).
 */
static int be_show(const char *s, size_t len)
{
	const char *ln[2];
	int ll[2], n = 0, i, j, scale, maxl = 0;
	size_t p = 0;

	while (p < len && n < 2) {
		size_t q = p;

		while (q < len && s[q] != '\n')
			q++;
		ln[n] = s + p;
		ll[n] = min_t(int, q - p, OLED_W / 8);
		maxl = max(maxl, ll[n]);
		n++;
		p = q + 1;
	}
	while (n > 0 && ll[n - 1] == 0)
		n--;
	scale = (n <= 1 && maxl <= OLED_W / 16) ? 2 : 1;
	memset(oled_px, 0, sizeof(oled_px));
	for (i = 0; i < n; i++) {
		int x = (OLED_W - ll[i] * 8 * scale) / 2;
		int y = (OLED_H - n * 16 * scale) / 2 + i * 16 * scale;

		for (j = 0; j < ll[i]; j++)
			oled_char(x + j * 8 * scale, y, ln[i][j], scale);
	}
	return oled_flush();
}

static void be_bright(int level)		/* 1..4 */
{
	oled_cs(1);
	oled_hdr(0xa0, level * 0x3f);		/* 4 -> 0xfc */
	oled_cs(0);
}

static int be_init(void)
{
	static const u8 on[8] = { 0x22, 0x03 };	/* front MCU: display on */
	u32 v;
	int ret;

	mspi = ioremap(MSPI_BASE, 0x200);
	gpio = ioremap(GPIO_BANK, 0x10);
	if (!mspi || !gpio) {
		ret = -ENOMEM;
		goto fail;
	}

	/* MSPI: master, CPOL = CPHA = 1, 8 bits, polled */
	writel(clamp(spi_div, 1, 255), mspi + MSPI_SPCR0_LSB);
	v = readl(mspi + MSPI_SPCR0_MSB);
	writel((v & ~3) | 0x80 | 3, mspi + MSPI_SPCR0_MSB);
	writel(0, mspi + MSPI_SPCR1_LSB);
	writel(0, mspi + MSPI_SPCR1_MSB);
	writel(readl(mspi + MSPI_SPCR2) & ~0x60, mspi + MSPI_SPCR2);
	writel(0, mspi + MSPI_STATUS);

	/* gpio 82: push-pull output, high (idle) */
	oled_cs(0);
	writel(readl(gpio + GPIO_ODEN) & ~GPIO_CS, gpio + GPIO_ODEN);
	writel(readl(gpio + GPIO_IODIR) & ~GPIO_CS, gpio + GPIO_IODIR);

	vu_mcu_send(on, sizeof(on));

	memset(oled_px, 0, sizeof(oled_px));
	ret = oled_flush();
	if (ret) {
		pr_err("vu_vfd: OLED does not answer on the MSPI (%d)\n", ret);
		goto fail;
	}
	return OLED_W / 8;
fail:
	if (gpio)
		iounmap(gpio);
	if (mspi)
		iounmap(mspi);
	return ret;
}

static void be_exit(void)
{
	iounmap(gpio);
	iounmap(mspi);
}
