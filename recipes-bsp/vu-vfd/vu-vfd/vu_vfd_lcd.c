// SPDX-License-Identifier: GPL-2.0
/*
 * vu_vfd_lcd.c - text backend of vu_vfd for the vuultimo gen1 (BCM7413):
 *                256x64 monochrome LCD on the UPG MSPI, chip select and
 *                reset on GPIOs. Included by vu_vfd.c when VU_VFD_LCD is set.
 *
 * Everything below was taken from the original module (dvb-bcm7413.ko of
 * the vuultimo, BSPI_* / NEXUS_Gpio_* / dbox_lcd code), not from an SDK.
 *
 * MSPI (base 0x10400400, 27 MHz): SPCR0_LSB = 27 MHz / (2 * baud),
 * SPCR0_MSB = MSTR | CPOL | CPHA, queue NEWQP/ENDQP, SPCR2 bit 6 = start,
 * STATUS bit 0 = done, TXRAM 0x440 + 8*i, CDRAM 0x540 + 4*i, PCS 1.
 * GPIO bank 32..55 at 0x10400720: +0 ODEN, +4 DATA, +8 IODIR (1 = input).
 *   gpio 36 = LCD chip select (low during a command), gpio 55 = LCD reset.
 * Pinmux (SUN_TOP 0x10404100): the CFE drives the LCD itself (splash) and
 *   leaves its pins on other functions; the original module (Nexus pinmux)
 *   sets, among others, 0x10404118 bits 11:6 = 0 (gpio 36/37) and
 *   0x10404134 bits 5:0 = 4,4 (MSPI): without them the LCD stays dark
 *   (tested). gpio 55 (reset, 0x10404120 bits 5:3) is left to the CFE,
 *   which leaves the panel out of reset: the reset pulse below then has no
 *   effect, the init sequence is enough.
 * Controller: init sequence below, 05 <rev(level)> = brightness,
 *   0f 00 00 fc + 2048 bytes = frame: for each column x, 8 bytes (rows
 *   0..63), top row = MSB.
 */
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include "vu_vfd_font.h"

#define LCD_W         256
#define LCD_H         64
#define LCD_REG_BASE  0x10400400UL
#define LCD_REG_SIZE  0x400

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
#define MSPI_PCS       1
#define MSPI_BAUD      1687500		/* default of the original module */

#define GPIO_BANK      0x320		/* gpio 32..55 */
#define GPIO_ODEN      (GPIO_BANK + 0x0)
#define GPIO_DATA      (GPIO_BANK + 0x4)
#define GPIO_IODIR     (GPIO_BANK + 0x8)
#define GPIO_CS        BIT(36 - 32)
#define GPIO_RESET     BIT(55 - 32)

#define PINMUX_BASE    0x10404100UL
static const u32 lcd_pinmux[][3] = {	/* offset, mask, value */
	{ 0x18, 0x00000fc0, 0x00000000 },	/* gpio 36 (CS), gpio 37 */
	{ 0x34, 0x0000003f, 0x00000024 },	/* MSPI pins */
};

static void lcd_set_pinmux(void)
{
	void __iomem *p = ioremap(PINMUX_BASE, 0x40);
	int i;

	if (!p)
		return;
	for (i = 0; i < ARRAY_SIZE(lcd_pinmux); i++)
		writel((readl(p + lcd_pinmux[i][0]) & ~lcd_pinmux[i][1]) | lcd_pinmux[i][2],
		       p + lcd_pinmux[i][0]);
	iounmap(p);
}

static int spi_burst = 1;
module_param(spi_burst, int, 0444);
MODULE_PARM_DESC(spi_burst, "LCD: bytes per MSPI transfer 1..16 (original module: 1)");

static void __iomem *lcd_regs;
static DEFINE_SPINLOCK(lcd_gpio_lock);
static u8 lcd_fb[LCD_W * LCD_H / 8];

static void lcd_gpio(u32 bits, int on)
{
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&lcd_gpio_lock, flags);
	v = readl(lcd_regs + GPIO_DATA);
	writel(on ? (v | bits) : (v & ~bits), lcd_regs + GPIO_DATA);
	spin_unlock_irqrestore(&lcd_gpio_lock, flags);
}

static int mspi_xfer(const u8 *b, int n)
{
	int i, t;

	for (i = 0; i < n; i++) {
		writel(b[i], lcd_regs + MSPI_TXRAM(i));
		writel(((i < n - 1) ? 0x83 : 0x03) & ~BIT(MSPI_PCS), lcd_regs + MSPI_CDRAM(i));
	}
	writel(0, lcd_regs + MSPI_NEWQP);
	writel(n - 1, lcd_regs + MSPI_ENDQP);
	writel(readl(lcd_regs + MSPI_SPCR2) | 0x40, lcd_regs + MSPI_SPCR2);
	for (t = 0; t < 10000; t++) {		/* 10 ms, as the original */
		if (readl(lcd_regs + MSPI_STATUS) & 1)
			break;
		udelay(1);
	}
	writel(0, lcd_regs + MSPI_STATUS);
	return t < 10000 ? 0 : -ETIMEDOUT;
}

static int lcd_send(const u8 *b, int n)
{
	int burst = clamp(spi_burst, 1, 16), ret = 0;

	while (n > 0 && !ret) {
		int k = min(n, burst);

		ret = mspi_xfer(b, k);
		b += k;
		n -= k;
	}
	return ret;
}

/* one controller command: chip select low, bytes, chip select high */
static int lcd_cmd(const u8 *b, int n)
{
	int ret;

	lcd_gpio(GPIO_CS, 0);
	ret = lcd_send(b, n);
	lcd_gpio(GPIO_CS, 1);
	return ret;
}

static u8 lcd_rev(u8 v)
{
	v = (v & 0xf0) >> 4 | (v & 0x0f) << 4;
	v = (v & 0xcc) >> 2 | (v & 0x33) << 2;
	return (v & 0xaa) >> 1 | (v & 0x55) << 1;
}

static int lcd_flush(void)
{
	static const u8 hdr[] = { 0x0f, 0x00, 0x00, 0xfc };
	int ret;

	lcd_gpio(GPIO_CS, 0);
	ret = lcd_send(hdr, sizeof(hdr));
	if (!ret)
		ret = lcd_send(lcd_fb, sizeof(lcd_fb));
	lcd_gpio(GPIO_CS, 1);
	return ret;
}

static void lcd_px(int x, int y)
{
	if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H)
		lcd_fb[x * 8 + y / 8] |= 0x80 >> (y & 7);
}

/* character c at x,y with scale s (1 = 8x16, 2 = 16x32) */
static void lcd_char(int x, int y, char c, int s)
{
	const u8 *g;
	int r, b;

	if (c < 0x20 || c > 0x7e)
		c = ' ';
	g = vu_vfd_font[c - 0x20];
	for (r = 0; r < 16 * s; r++)
		for (b = 0; b < 8 * s; b++)
			if (g[r / s] & (0x80 >> (b / s)))
				lcd_px(x + b, y + r);
}

static void lcd_line(int y, const char *s, int len, int scale)
{
	int x = (LCD_W - len * 8 * scale) / 2, i;

	for (i = 0; i < len; i++)
		lcd_char(x + i * 8 * scale, y, s[i], scale);
}

/*
 * Text layout: "\n" splits lines. Up to 2 lines of at most 16 characters
 * are drawn large (16x32), otherwise up to 4 lines of 32 characters (8x16).
 * Lines are centred.
 */
#define LCD_MAXLINES 4
static int be_show(const char *s, size_t len)
{
	const char *ln[LCD_MAXLINES];
	int ll[LCD_MAXLINES], n = 0, i, scale, maxl = 0, h;
	size_t p = 0;

	while (p < len && n < LCD_MAXLINES) {
		size_t q = p;

		while (q < len && s[q] != '\n')
			q++;
		ln[n] = s + p;
		ll[n] = min_t(int, q - p, LCD_W / 8);
		maxl = max(maxl, ll[n]);
		n++;
		p = q + 1;
	}
	while (n > 0 && ll[n - 1] == 0)		/* trailing empty lines */
		n--;
	scale = (n <= 2 && maxl <= LCD_W / 16) ? 2 : 1;
	h = 16 * scale;
	memset(lcd_fb, 0, sizeof(lcd_fb));
	for (i = 0; i < n; i++)
		lcd_line((LCD_H - n * h) / 2 + i * h, ln[i], ll[i], scale);
	return lcd_flush();
}

static void be_bright(int level)		/* 1..4 */
{
	u8 c[2] = { 0x05, lcd_rev(level * 0x38) };	/* 4 -> 0xe0, the original default */

	lcd_cmd(c, 2);
}

static int be_init(void)
{
	static const u8 seq[] = {	/* length, bytes...: init of the original module */
		1, 0x55,
		2, 0x1e, 0x10,
		3, 0x33, 0x80, 0x00,
		4, 0x07, 0xff, 0xfc, 0x00,
		5, 0x8d, 0xfc, 0xaa, 0x40, 0x60,
		2, 0x05, 0x07,		/* 05 rev(0xe0) */
		3, 0x03, 0x00, 0x00,
		3, 0x0b, 0x00, 0xff,
		2, 0x01, 0x00,
		0 };
	const u8 *p;
	u32 v;
	int ret;

	lcd_regs = ioremap(LCD_REG_BASE, LCD_REG_SIZE);
	if (!lcd_regs)
		return -ENOMEM;

	/* MSPI: master, CPOL = CPHA = 1, 8 bits, no interrupt (polled) */
	writel(27000000 / (2 * MSPI_BAUD), lcd_regs + MSPI_SPCR0_LSB);
	v = readl(lcd_regs + MSPI_SPCR0_MSB);
	writel((v & ~3) | 0x80 | 3, lcd_regs + MSPI_SPCR0_MSB);
	writel(0, lcd_regs + MSPI_SPCR1_LSB);
	writel(0, lcd_regs + MSPI_SPCR1_MSB);
	writel(readl(lcd_regs + MSPI_SPCR2) & ~0x60, lcd_regs + MSPI_SPCR2);
	writel(0, lcd_regs + MSPI_STATUS);

	/* gpio 36 / 55: push-pull outputs, CS high (before the pins become GPIO) */
	lcd_gpio(GPIO_CS | GPIO_RESET, 1);
	writel(readl(lcd_regs + GPIO_ODEN) & ~(GPIO_CS | GPIO_RESET), lcd_regs + GPIO_ODEN);
	writel(readl(lcd_regs + GPIO_IODIR) & ~(GPIO_CS | GPIO_RESET), lcd_regs + GPIO_IODIR);

	lcd_set_pinmux();

	/* reset: low 1 ms, high 10 ms */
	lcd_gpio(GPIO_RESET, 0);
	msleep(1);
	lcd_gpio(GPIO_RESET, 1);
	msleep(10);

	for (p = seq; *p; p += 1 + *p) {
		ret = lcd_cmd(p + 1, *p);
		if (ret) {
			pr_err("vu_vfd: LCD does not answer on the MSPI (%d)\n", ret);
			iounmap(lcd_regs);
			return ret;
		}
	}
	memset(lcd_fb, 0, sizeof(lcd_fb));
	lcd_flush();
	return LCD_W / 8;			/* characters per line (8x16) */
}

static void be_exit(void)
{
	iounmap(lcd_regs);
}
