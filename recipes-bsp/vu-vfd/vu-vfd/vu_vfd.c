// SPDX-License-Identifier: GPL-2.0
/*
 * vu_vfd.c - front text display of the gen1 Vu+ boxes, one interface for
 *            two kinds of hardware (chosen at build time):
 *
 *  - vuuno, vuduo: 16-character VFD driven by the front-panel MCU,
 *    reached through vu_ir, which owns its UART (vu_mcu_send()).
 *    Commands (from the original modules dvb-bcm7413 / dvb-bcm7335):
 *      0x21 + N chars   text (ASCII, padded with spaces), 1 + N bytes
 *      0x22 0x03        display on (sent at init by the original module)
 *      0x25 <1..4>      brightness (1 = brightest)
 *      0x11 / 0x12 <n>  icon n on / off
 *    Every command except the text is an 8-byte packet (zero padded): the
 *    MCU reads 8 bytes, a shorter command swallows the next one.
 *  - vuultimo (VU_VFD_LCD): 256x64 graphic LCD on the MSPI, used as a text
 *    display with an internal font (vu_vfd_lcd.c).
 *  - vuduo2 (VU_VFD_OLED): 140x32 OLED on the MSPI, same text interface
 *    (vu_vfd_oled.c); its colour TFT is the separate vu_tft.
 *
 * /dev/vu_vfd   the text written between open and close, shown at close. VFD: one line, cut at the first
 *               newline. LCD: "\n" splits lines; up to 2 short lines are
 *               drawn large, otherwise up to 4 lines of 32 characters.
 * Parameters:   brightness=1..4 (4 = brightest, 0 = leave as is; writable in
 *               /sys/module/vu_vfd/parameters), text= shown at load
 *               (LCD: "|" in text= is a line break; with spaces quote it
 *               twice: text='"two words"', busybox insmod splits on spaces)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/string.h>

#define DRV_NAME   "vu_vfd"
#define VFD_BUF    160

static int brightness = 4;
static bool vfd_ready;
static char *text = "";
module_param(text, charp, 0444);
MODULE_PARM_DESC(text, "text shown when the module is loaded");

static DEFINE_MUTEX(vfd_lock);

#if defined(VU_VFD_OLED)
#include "vu_vfd_oled.c"
#define VFD_KEEP_NEWLINES 1
#elif defined(VU_VFD_LCD)
#include "vu_vfd_lcd.c"
#define VFD_KEEP_NEWLINES 1
#else
#include "vu_mcu.h"
#define VFD_KEEP_NEWLINES 0
#define VFD_TEXT   0x21
#define VFD_BRIGHT 0x25
#define VFD_MAX    32
#define VFD_DISPLAY 0x22
#define VFD_CMD_LEN 8

/* 8-byte command packet, as the original module sends it */
static int vfd_cmd(u8 cmd, u8 arg)
{
	u8 pkt[VFD_CMD_LEN] = { cmd, arg };

	return vu_mcu_send(pkt, VFD_CMD_LEN);
}

static int vfd_chars;

static int be_show(const char *s, size_t len)
{
	u8 pkt[1 + VFD_MAX];
	int i;

	pkt[0] = VFD_TEXT;
	for (i = 0; i < vfd_chars; i++) {
		char c = i < len ? s[i] : ' ';

		pkt[1 + i] = (c >= 0x20 && c < 0x7f) ? c : ' ';
	}
	return vu_mcu_send(pkt, 1 + vfd_chars);
}

/*
 * 25 <n>: n = 1 is the brightest, 4 the dimmest (the original maps
 * oled_brightness 191..255 -> 1, 127..190 -> 2, 63..126 -> 3, 0..62 -> 4).
 * vu_vfd level 4 = brightest.
 */
static void be_bright(int level)
{
	vfd_cmd(VFD_BRIGHT, 5 - level);
}

static int be_init(void)
{
	vfd_chars = vu_mcu_vfd_chars();
	if (vfd_chars <= 0 || vfd_chars > VFD_MAX)
		return -ENODEV;
	vfd_cmd(VFD_DISPLAY, 0x03);
	return vfd_chars;
}

static void be_exit(void)
{
}
#endif

/* brightness: also writable at run time (/sys/module/vu_vfd/parameters/brightness) */
static int vu_vfd_set_bright(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (ret)
		return ret;
	if (brightness < 0 || brightness > 4)
		brightness = 4;
	if (vfd_ready && brightness) {
		mutex_lock(&vfd_lock);
		be_bright(brightness);
		mutex_unlock(&vfd_lock);
	}
	return 0;
}

static const struct kernel_param_ops vu_vfd_bright_ops = {
	.set = vu_vfd_set_bright,
	.get = param_get_int,
};
module_param_cb(brightness, &vu_vfd_bright_ops, &brightness, 0644);
MODULE_PARM_DESC(brightness, "brightness 1..4, 4 = brightest (0 = leave as is); writable at run time");

static int vu_vfd_show(char *s, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (s[i] == '\n' && !VFD_KEEP_NEWLINES)
			break;
		if (s[i] == '|' && VFD_KEEP_NEWLINES && s == text)
			s[i] = '\n';
	}
	while (i > 0 && s[i - 1] == '\n')
		i--;
	return be_show(s, i);
}

/*
 * The text is collected from open to close and shown at close: a shell
 * echo may send one write() per line, which would redraw line by line.
 */
static char vfd_buf[VFD_BUF];
static size_t vfd_len;

static int vu_vfd_open(struct inode *inode, struct file *f)
{
	mutex_lock(&vfd_lock);
	vfd_len = 0;
	mutex_unlock(&vfd_lock);
	return 0;
}

static ssize_t vu_vfd_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	size_t n;

	mutex_lock(&vfd_lock);
	n = min(len, sizeof(vfd_buf) - vfd_len);
	if (copy_from_user(vfd_buf + vfd_len, ubuf, n)) {
		mutex_unlock(&vfd_lock);
		return -EFAULT;
	}
	vfd_len += n;
	mutex_unlock(&vfd_lock);
	return len;
}

static int vu_vfd_release(struct inode *inode, struct file *f)
{
	if (!(f->f_mode & FMODE_WRITE))
		return 0;
	mutex_lock(&vfd_lock);
	vu_vfd_show(vfd_buf, vfd_len);
	vfd_len = 0;
	mutex_unlock(&vfd_lock);
	return 0;
}

static const struct file_operations vu_vfd_fops = {
	.owner   = THIS_MODULE,
	.open    = vu_vfd_open,
	.write   = vu_vfd_write,
	.release = vu_vfd_release,
};

static struct miscdevice vu_vfd_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = DRV_NAME,
	.fops  = &vu_vfd_fops,
};

static int __init vu_vfd_init(void)
{
	int chars, ret;

	chars = be_init();
	if (chars <= 0) {
		pr_info(DRV_NAME ": no front display on this box\n");
		return chars ? chars : -ENODEV;
	}
	ret = misc_register(&vu_vfd_dev);
	if (ret) {
		be_exit();
		return ret;
	}
	if (brightness >= 1 && brightness <= 4)
		be_bright(brightness);
	vfd_ready = true;
	if (*text)
		vu_vfd_show(text, strlen(text));
	pr_info(DRV_NAME ": %d characters per line on /dev/" DRV_NAME "\n", chars);
	return 0;
}

static void __exit vu_vfd_exit(void)
{
	misc_deregister(&vu_vfd_dev);
	be_exit();
}

module_init(vu_vfd_init);
module_exit(vu_vfd_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Vu+ gen1 front text display (MCU VFD or MSPI LCD)");
