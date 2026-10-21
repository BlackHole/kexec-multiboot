// SPDX-License-Identifier: GPL-2.0
/*
 * vu_tft.c - front colour LCD (TFT) of the vuduo2 (BCM7424): 240x400
 *                RGB565 colour LCD on the second MSPI, D/C and reset on
 *                GPIOs. Shown as a text display
 *                on /dev/vu_tft (the VFD is a separate display).
 *
 * Everything below was taken from the original module (dvb-bcm7424.ko of
 * the vuduo2: brcm_duo2_lcd_init, bm750_bd_revision; dvb_base.ko: BSPI_*,
 * NEXUS_Gpio_*), not from an SDK, and checked against the registers the
 * original module leaves behind.
 *
 * Board revision: AON GPIO 0 and 1 (bank 0x104094c0, +4 DATA), rev = b0 | b1 << 1.
 *   rev 0, 1, 3: NEXUS SPI channel 0x10000 = MSPI core 1 (0x1041c200); rev 1
 *   has its own panel setup. rev 2: other SPI and an extra chip select (gpio
 *   35): not supported.
 * MSPI core 1 (base 0x1041c200, 27 MHz): SPCR0_LSB 8 (1.6875 MHz),
 *   SPCR0_MSB 0x83, CDRAM low nibble 0x6 (PCS).
 * GPIO bank 0..31 at 0x10406700: +0 ODEN, +4 DATA, +8 IODIR (1 = input).
 *   gpio 31 = D/C (low = command), gpio 30 = reset.
 * EBI chip select 4 must be routed to the MSPI (0x10419920 bit 4), or the
 *   panel never sees its chip select and keeps the front MCU's picture.
 * Panel: 240 columns x 400 rows, 2a 00 00 00 ef / 2b 00 00 01 8f / 2c +
 *   pixels, 2 bytes each, MSB first; 51 <n> = brightness.
 *
 * /dev/vu_tft   the text written between open and close is shown at close, "\n" splits lines (at most 8),
 *               the font is scaled as large as the lines allow.
 * Parameters:   brightness=1..4, text= shown at load ("|" = line break),
 *               rotate=, color=, rev=, spi_div=, bright_max= (see below).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include "vu_vfd_font.h"

#define PANEL_W       240		/* columns (2a) */
#define PANEL_H       400		/* rows (2b) */
#define TFT_W         400		/* text area, landscape */
#define TFT_H         240

#define MSPI_BASE     0x1041c200UL
#define GPIO_BASE     0x10406700UL
#define AON_GPIO_BASE 0x104094c0UL
/*
 * BSPI_OpenChannel of the original: HIF SPI channel 0 has ebiCs 4, so it
 * sets bit 4 here (EBI chip select 4 to the MSPI). Reading the register
 * from user space gives a bus error, so it is only written (as the test
 * that made the panel answer: 0x10).
 */
#define EBI_CS_SPI_SEL 0x10419920UL
#define EBI_CS_LCD     BIT(4)

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
#define MSPI_PCS_BITS  0x06		/* as left by the original module */

#define GPIO_ODEN      0x0
#define GPIO_DATA      0x4
#define GPIO_IODIR     0x8
#define GPIO_DC        BIT(31)
#define GPIO_RESET     BIT(30)
#define GPIO_CS_BANK   0x20		/* gpio 32..63 */
#define GPIO_CS        BIT(35 - 32)	/* chip select, driven high by the original */


static int spi_div = 8;
module_param(spi_div, int, 0444);
MODULE_PARM_DESC(spi_div, "LCD: MSPI divider, clock = 27 MHz / (2 * n) (original: 8)");
static int rotate = 1;
module_param(rotate, int, 0444);
MODULE_PARM_DESC(rotate, "LCD: text orientation 0..3 (quarter turns)");
static int color = 0xffff;
module_param(color, int, 0444);
MODULE_PARM_DESC(color, "LCD: text colour, RGB565");
static int rev = -1;
module_param(rev, int, 0444);
MODULE_PARM_DESC(rev, "board revision 0..3 (-1 = read the straps)");
static int bright_max = 0x28;
module_param(bright_max, int, 0444);
MODULE_PARM_DESC(bright_max, "LCD: 51 value at brightness 4 (original: 0x28)");

static void __iomem *mspi, *gpio;
static u8 *tft_fb;			/* panel order, 2 bytes per pixel */

static void tft_gpio(u32 bits, int on)
{
	u32 v = readl(gpio + GPIO_DATA);

	writel(on ? (v | bits) : (v & ~bits), gpio + GPIO_DATA);
}

static int mspi_xfer(const u8 *b, int n)
{
	int i, t;

	for (i = 0; i < n; i++) {
		writel(b[i], mspi + MSPI_TXRAM(i));
		writel((i < n - 1 ? 0x80 : 0) | MSPI_PCS_BITS, mspi + MSPI_CDRAM(i));
	}
	writel(0, mspi + MSPI_NEWQP);
	writel(n - 1, mspi + MSPI_ENDQP);
	writel(readl(mspi + MSPI_SPCR2) | 0x40, mspi + MSPI_SPCR2);
	for (t = 0; t < 10000; t++) {
		if (readl(mspi + MSPI_STATUS) & 1)
			break;
		udelay(1);
	}
	writel(0, mspi + MSPI_STATUS);
	return t < 10000 ? 0 : -ETIMEDOUT;
}

static int tft_send(const u8 *b, int n)
{
	int ret = 0, k = 0;

	while (n > 0 && !ret) {
		int c = min(n, 16);

		ret = mspi_xfer(b, c);
		b += c;
		n -= c;
		if (++k % 256 == 0)
			cond_resched();
	}
	return ret;
}

/* command byte (D/C low) followed by its parameters (D/C high) */
static int tft_cmd(u8 cmd, const u8 *p, int n)
{
	int ret;

	tft_gpio(GPIO_DC, 0);
	ret = mspi_xfer(&cmd, 1);
	tft_gpio(GPIO_DC, 1);
	if (!ret && n)
		ret = tft_send(p, n);
	return ret;
}

/* sequences: cmd, nparams, params...; 0xff n = sleep n*1 ms; 0 ends */
#define B 0xfe			/* placeholder: brightness */
static const u8 seq_start[] = {
	0x53, 1, 0x2c,  0xbe, 1, 0x06,  0x51, 1, B,  0xe9, 1, 0x20,
	0x11, 0,  0xff, 100,
	0x53, 1, 0x2c,  0xbe, 1, 0x06,  0x51, 1, B,
	0 };
static const u8 seq_rev1[] = {
	0xd1, 3, 0x00, 0x58, 0x12,
	0xd0, 3, 0x07, 0x01, 0x88,
	0x36, 1, 0x08,
	0x3a, 1, 0x55,
	0xc1, 4, 0x10, 0x10, 0x02, 0x02,
	0xc0, 6, 0x01, 0x35, 0x00, 0x00, 0x01, 0x02,
	0xc5, 1, 0x03,
	0xd2, 2, 0x01, 0x22,
	0xc8, 15, 0x00, 0x44, 0x14, 0x23, 0x08, 0x08, 0x36, 0x33, 0x77, 0x32, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x53, 1, 0x2c,  0xbe, 1, 0x06,
	0 };
static const u8 seq_rev03[] = {
	0xd1, 3, 0x00, 0x79, 0x1e,
	0xd0, 3, 0x07, 0x01, 0x04,
	0x36, 1, 0x48,
	0x3a, 1, 0x55,
	0xc1, 4, 0x10, 0x10, 0x02, 0x02,
	0xc0, 6, 0x00, 0x35, 0x00, 0x00, 0x01, 0x02,
	0xc5, 1, 0x02,
	0xd2, 2, 0x01, 0x22,
	0xc8, 15, 0x02, 0x77, 0x47, 0x09, 0x09, 0x00, 0x03, 0x00, 0x57, 0x50, 0x00, 0x10, 0x08, 0x80, 0x00,
	0x53, 1, 0x2c,
	0 };
static const u8 seq_end[] = {
	0x51, 1, B,  0x29, 0,  0xff, 150,
	0 };

static int tft_seq(const u8 *s, u8 bright)
{
	u8 p[16];
	int i, ret;

	while (*s) {
		if (*s == 0xff) {
			msleep(s[1]);
			s += 2;
			continue;
		}
		for (i = 0; i < s[1]; i++)
			p[i] = s[2 + i] == B ? bright : s[2 + i];
		ret = tft_cmd(s[0], p, s[1]);
		if (ret)
			return ret;
		s += 2 + s[1];
	}
	return 0;
}

static int tft_flush(void)
{
	static const u8 ca[] = { 0x00, 0x00, 0x00, PANEL_W - 1 };
	static const u8 ra[] = { 0x00, 0x00, (PANEL_H - 1) >> 8, (PANEL_H - 1) & 0xff };
	int ret;

	ret = tft_cmd(0x2a, ca, 4);
	if (!ret)
		ret = tft_cmd(0x2b, ra, 4);
	if (!ret)
		ret = tft_cmd(0x2c, tft_fb, PANEL_W * PANEL_H * 2);
	return ret;
}

/* logical landscape pixel (x 0..399, y 0..239) to panel column/row */
static void tft_px(int x, int y)
{
	int c, r, w = TFT_W, h = TFT_H;
	u8 *p;

	if ((rotate & 1) == 0) {	/* portrait text area */
		w = TFT_H;
		h = TFT_W;
	}
	if (x < 0 || x >= w || y < 0 || y >= h)
		return;
	switch (rotate & 3) {
	case 0:  c = x;               r = y;               break;
	case 1:  c = PANEL_W - 1 - y; r = x;               break;
	case 2:  c = PANEL_W - 1 - x; r = PANEL_H - 1 - y; break;
	default: c = y;               r = PANEL_H - 1 - x; break;
	}
	p = tft_fb + (r * PANEL_W + c) * 2;
	p[0] = color >> 8;
	p[1] = color;
}

static void tft_char(int x, int y, char c, int s)
{
	const u8 *g;
	int r, b;

	if (c < 0x20 || c > 0x7e)
		c = ' ';
	g = vu_vfd_font[c - 0x20];
	for (r = 0; r < 16 * s; r++)
		for (b = 0; b < 8 * s; b++)
			if (g[r / s] & (0x80 >> (b / s)))
				tft_px(x + b, y + r);
}

/*
 * Text layout: "\n" splits lines (at most 8). The font is scaled as large
 * as the lines allow (up to 4x = 32x64); lines are centred.
 */
#define TFT_MAXLINES 8
static int be_show(const char *s, size_t len)
{
	const char *ln[TFT_MAXLINES];
	int ll[TFT_MAXLINES], n = 0, i, j, scale, maxl = 1, w, h;
	size_t p = 0;

	w = (rotate & 1) ? TFT_W : TFT_H;
	h = (rotate & 1) ? TFT_H : TFT_W;
	while (p < len && n < TFT_MAXLINES) {
		size_t q = p;

		while (q < len && s[q] != '\n')
			q++;
		ln[n] = s + p;
		ll[n] = min_t(int, q - p, w / 8);
		maxl = max(maxl, ll[n]);
		n++;
		p = q + 1;
	}
	while (n > 0 && ll[n - 1] == 0)
		n--;
	for (scale = 4; scale > 1; scale--)
		if (maxl * 8 * scale <= w && n * 16 * scale <= h)
			break;
	memset(tft_fb, 0, PANEL_W * PANEL_H * 2);
	for (i = 0; i < n; i++) {
		int x = (w - ll[i] * 8 * scale) / 2;
		int y = (h - n * 16 * scale) / 2 + i * 16 * scale;

		for (j = 0; j < ll[i]; j++)
			tft_char(x + j * 8 * scale, y, ln[i][j], scale);
	}
	return tft_flush();
}

static void be_bright(int level)		/* 1..4 */
{
	u8 v = bright_max * level / 4;

	tft_cmd(0x51, &v, 1);
}

static int be_init(void)
{
	void __iomem *aon;
	u32 v;
	int ret;

	if (rev < 0) {
		aon = ioremap(AON_GPIO_BASE, 0x10);
		if (!aon)
			return -ENOMEM;
		rev = readl(aon + GPIO_DATA) & 3;
		iounmap(aon);
	}
	pr_info("vu_tft: board revision %d\n", rev);
	if (rev == 2) {
		pr_err("vu_tft: board revision 2 is not supported\n");
		return -ENODEV;
	}

	tft_fb = vzalloc(PANEL_W * PANEL_H * 2);
	mspi = ioremap(MSPI_BASE, 0x200);
	gpio = ioremap(GPIO_BASE, 0x30);
	if (!tft_fb || !mspi || !gpio) {
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

	{
		void __iomem *ebi = ioremap(EBI_CS_SPI_SEL, 4);

		if (ebi) {
			writel(EBI_CS_LCD, ebi);
			iounmap(ebi);
		}
	}

	/* gpio 35: output high, as the original */
	writel(readl(gpio + GPIO_CS_BANK + GPIO_DATA) | GPIO_CS, gpio + GPIO_CS_BANK + GPIO_DATA);
	writel(readl(gpio + GPIO_CS_BANK + GPIO_ODEN) & ~GPIO_CS, gpio + GPIO_CS_BANK + GPIO_ODEN);
	writel(readl(gpio + GPIO_CS_BANK + GPIO_IODIR) & ~GPIO_CS, gpio + GPIO_CS_BANK + GPIO_IODIR);

	/* gpio 30 / 31: push-pull outputs */
	tft_gpio(GPIO_DC | GPIO_RESET, 1);
	writel(readl(gpio + GPIO_ODEN) & ~(GPIO_DC | GPIO_RESET), gpio + GPIO_ODEN);
	writel(readl(gpio + GPIO_IODIR) & ~(GPIO_DC | GPIO_RESET), gpio + GPIO_IODIR);

	/* reset: low 1 ms, high 10 ms */
	tft_gpio(GPIO_RESET, 0);
	msleep(1);
	tft_gpio(GPIO_RESET, 1);
	msleep(10);

	v = clamp(bright_max, 0, 255);
	ret = tft_seq(seq_start, v);
	if (!ret)
		ret = tft_seq(rev == 1 ? seq_rev1 : seq_rev03, v);
	if (!ret)
		ret = tft_seq(seq_end, v);
	if (!ret)
		ret = tft_flush();
	if (ret) {
		pr_err("vu_tft: LCD does not answer on the MSPI (%d)\n", ret);
		goto fail;
	}
	return ((rotate & 1) ? TFT_W : TFT_H) / 8;
fail:
	if (gpio)
		iounmap(gpio);
	if (mspi)
		iounmap(mspi);
	vfree(tft_fb);
	return ret;
}

static void be_exit(void)
{
	iounmap(gpio);
	iounmap(mspi);
	vfree(tft_fb);
}

#define DRV_NAME  "vu_tft"
#define TFT_BUF   400

static int brightness = 4;
static bool tft_ready;
static char *text = "";
module_param(text, charp, 0444);
MODULE_PARM_DESC(text, "text shown when the module is loaded (| = line break)");

static DEFINE_MUTEX(tft_lock);

/* brightness: also writable at run time (/sys/module/vu_tft/parameters/brightness) */
static int vu_tft_set_bright(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (ret)
		return ret;
	if (brightness < 0 || brightness > 4)
		brightness = 4;
	if (tft_ready && brightness) {
		mutex_lock(&tft_lock);
		be_bright(brightness);
		mutex_unlock(&tft_lock);
	}
	return 0;
}

static const struct kernel_param_ops vu_tft_bright_ops = {
	.set = vu_tft_set_bright,
	.get = param_get_int,
};
module_param_cb(brightness, &vu_tft_bright_ops, &brightness, 0644);
MODULE_PARM_DESC(brightness, "brightness 1..4, 4 = bright_max (0 = leave as is); writable at run time");

static int vu_tft_show(char *s, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (s[i] == '|' && s == text)
			s[i] = '\n';
	while (len > 0 && s[len - 1] == '\n')
		len--;
	return be_show(s, len);
}

/*
 * The text is collected from open to close and drawn at close: a shell
 * echo may send one write() per line, which would redraw line by line.
 */
static char tft_buf[TFT_BUF];
static size_t tft_len;

static int vu_tft_open(struct inode *inode, struct file *f)
{
	mutex_lock(&tft_lock);
	tft_len = 0;
	mutex_unlock(&tft_lock);
	return 0;
}

static ssize_t vu_tft_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	size_t n;

	mutex_lock(&tft_lock);
	n = min(len, sizeof(tft_buf) - tft_len);
	if (copy_from_user(tft_buf + tft_len, ubuf, n)) {
		mutex_unlock(&tft_lock);
		return -EFAULT;
	}
	tft_len += n;
	mutex_unlock(&tft_lock);
	return len;
}

static int vu_tft_release(struct inode *inode, struct file *f)
{
	if (!(f->f_mode & FMODE_WRITE))
		return 0;
	mutex_lock(&tft_lock);
	vu_tft_show(tft_buf, tft_len);
	tft_len = 0;
	mutex_unlock(&tft_lock);
	return 0;
}

static const struct file_operations vu_tft_fops = {
	.owner   = THIS_MODULE,
	.open    = vu_tft_open,
	.write   = vu_tft_write,
	.release = vu_tft_release,
};

static struct miscdevice vu_tft_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = DRV_NAME,
	.fops  = &vu_tft_fops,
};

static int __init vu_tft_init(void)
{
	int chars, ret;

	chars = be_init();
	if (chars <= 0)
		return chars ? chars : -ENODEV;
	ret = misc_register(&vu_tft_dev);
	if (ret) {
		be_exit();
		return ret;
	}
	if (brightness >= 1 && brightness <= 4)
		be_bright(brightness);
	tft_ready = true;
	if (*text)
		vu_tft_show(text, strlen(text));
	pr_info(DRV_NAME ": %d characters per line on /dev/" DRV_NAME "\n", chars);
	return 0;
}

static void __exit vu_tft_exit(void)
{
	misc_deregister(&vu_tft_dev);
	be_exit();
}

module_init(vu_tft_init);
module_exit(vu_tft_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Vu+ vuduo2 front colour LCD (TFT) as a text display");
