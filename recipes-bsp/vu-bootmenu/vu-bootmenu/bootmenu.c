// SPDX-License-Identifier: GPL-2.0
/*
 * bootmenu - multiboot menu for the Vu+ kexec initrd (4K and MIPS boxes)
 *            (framebuffer vu_fb + remote vu_ir; front text display vu_vfd:
 *            the selected image is written to /dev/vu_vfd when it exists;
 *            vuduo2: also to its colour TFT /dev/vu_tft, config "Front TFT"
 *            = image name or off; the image that boots stays shown there,
 *            menu opened or not; 4K boxes with a TFT too, via vu_fb keep=1)
 *
 * Usage (from the init script, after mounting the partition with STARTUP*):
 *   bootmenu [--root /newroot] [--wait 0] [--settle 0.15] [--timeout 0]
 *            [--margin 6] [--margin-bottom 18] [--font 2] [--small-font 1.25] [--fb-module vu_fb.ko]
 *            (sizes refer to 576 lines and scale with the screen height;
 *             --margin-bottom defaults to --margin)
 *            [--min-time 2] [--dev-timeout 15] [--dry-run] [--list] [--debug]
 *
 *  1. --wait N (default: STARTUP_CONFIG wait=, else 5): show a hint and wait
 *     N seconds for MENU; --wait 0: open the menu only if MENU is held down
 *     right now (EVIOCGKEY + up to --settle seconds for the next remote frame).
 *  2. list <root>/STARTUP_1..N (+ STARTUP_FLASH of chkroot, STARTUP_RECOVERY;
 *     both optional), resolve the image name (also from a UBI root, ubi0:rootfs)
 *     from etc/image-version of the rootfs given by root= / rootsubdir=
 *  3. on selection copy STARTUP_x -> STARTUP (atomically), remove STARTUP_ONCE
 *
 * USB settle: bootmenu never exits before --min-time seconds (STARTUP_CONFIG
 * min=, default 2), so
 * USB disks have time to appear. A slot with root=UUID=/LABEL=... boots only
 * when its disk is present: every 0.5 s "mdev -s" + blkid look for it. If the
 * disk of the slot to boot is still missing after --dev-timeout seconds
 * (default 15, counted from the start) the menu opens and stays open; if the
 * disk shows up while the menu is untouched, that slot boots by itself.
 *
 * Exit code: 0 = nothing changed, 10 = STARTUP updated, 1 = error
 * Keys: up/down, left/right = page, 1-9 direct, OK boot, EXIT/MENU cancel
 *
 * Build: ${CC} -O2 -static -o bootmenu bootmenu.c
 * Test without hardware: -DOFFSCREEN writes every frame to /tmp/bootmenu.ppm
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <linux/fb.h>
#include <linux/input.h>

#define MAX_ENTRIES   64
#define MAX_STR       256
#define MNT_TMP       "/tmp/bootmenu_mnt"

/* ------------------------------------------------------------------ options */
static const char *opt_root = "/boot";
static double opt_wait = 0, opt_settle = 0.15, opt_font = 2.0, opt_small = 1.25;
static int opt_timeout = 0, opt_margin = 6, opt_margin_bottom = -1;  /* -1: same as --margin (use 18 if the TV crops the bottom) */
static int opt_dry = 0, opt_list = 0, opt_debug = 0, opt_wait_set = 0;
static double opt_min = -1, opt_devto = 15;   /* opt_min -1: STARTUP_CONFIG min= */
static double opt_settle_video = 5;  /* --video-settle: TV lock time when vu_fb started the display itself */
static double t_start;
static const char *opt_fbmod = "vu_fb.ko";   /* --fb-module: file name (in /lib/modules/<kernel>/extra) or path */

/* <root>/STARTUP_CONFIG: "tft=mirror|own|off" and "wait=0|N" (other lines are kept) */
static char cfg_tft[16] = "own";   /* default: image name on the front TFT */
static int cfg_wait = 5;          /* seconds to press MENU (STARTUP_CONFIG wait=) */
static int cfg_min = 2;           /* minimum seconds before booting, USB settle (STARTUP_CONFIG min=) */
static int cfg_bright = 4;        /* front displays 1..4, 4 = brightest (STARTUP_CONFIG bright=) */

/* config "tft" (front display, boxes with a TFT only) -> vu_fb tft= */
static const struct { const char *name, *label; } tfts[] = {
	{ "mirror", "Copy of TV" },
	{ "own",    "Image name" },
	{ "off",    "Off" },
};
#define NTFTS ((int)(sizeof(tfts) / sizeof(tfts[0])))
static const int waits[] = { 0, 1, 3, 5, 10 };
#define NWAITS ((int)(sizeof(waits) / sizeof(waits[0])))
static const int mins[] = { 0, 2, 4, 6, 8 };
#define NMINS ((int)(sizeof(mins) / sizeof(mins[0])))

static void dbg(const char *fmt, ...)
{
	va_list ap;

	if (!opt_debug)
		return;
	va_start(ap, fmt);
	fputs("bootmenu: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ font */
/* font8x8_basic (public domain), ASCII 0x20..0x7e, bit0 = leftmost pixel */
static const uint8_t font8x8[95][8] = {
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},	/* ' ' */
	{0x18, 0x3c, 0x3c, 0x18, 0x18, 0x00, 0x18, 0x00},	/* '!' */
	{0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},	/* '"' */
	{0x36, 0x36, 0x7f, 0x36, 0x7f, 0x36, 0x36, 0x00},	/* '#' */
	{0x0c, 0x3e, 0x03, 0x1e, 0x30, 0x1f, 0x0c, 0x00},	/* '$' */
	{0x00, 0x63, 0x33, 0x18, 0x0c, 0x66, 0x63, 0x00},	/* '%' */
	{0x1c, 0x36, 0x1c, 0x6e, 0x3b, 0x33, 0x6e, 0x00},	/* '&' */
	{0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},	/* ''' */
	{0x18, 0x0c, 0x06, 0x06, 0x06, 0x0c, 0x18, 0x00},	/* '(' */
	{0x06, 0x0c, 0x18, 0x18, 0x18, 0x0c, 0x06, 0x00},	/* ')' */
	{0x00, 0x66, 0x3c, 0xff, 0x3c, 0x66, 0x00, 0x00},	/* 0x2a */
	{0x00, 0x0c, 0x0c, 0x3f, 0x0c, 0x0c, 0x00, 0x00},	/* '+' */
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c, 0x06},	/* ',' */
	{0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x00},	/* '-' */
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c, 0x00},	/* '.' */
	{0x60, 0x30, 0x18, 0x0c, 0x06, 0x03, 0x01, 0x00},	/* 0x2f */
	{0x3e, 0x63, 0x73, 0x7b, 0x6f, 0x67, 0x3e, 0x00},	/* '0' */
	{0x0c, 0x0e, 0x0c, 0x0c, 0x0c, 0x0c, 0x3f, 0x00},	/* '1' */
	{0x1e, 0x33, 0x30, 0x1c, 0x06, 0x33, 0x3f, 0x00},	/* '2' */
	{0x1e, 0x33, 0x30, 0x1c, 0x30, 0x33, 0x1e, 0x00},	/* '3' */
	{0x38, 0x3c, 0x36, 0x33, 0x7f, 0x30, 0x78, 0x00},	/* '4' */
	{0x3f, 0x03, 0x1f, 0x30, 0x30, 0x33, 0x1e, 0x00},	/* '5' */
	{0x1c, 0x06, 0x03, 0x1f, 0x33, 0x33, 0x1e, 0x00},	/* '6' */
	{0x3f, 0x33, 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x00},	/* '7' */
	{0x1e, 0x33, 0x33, 0x1e, 0x33, 0x33, 0x1e, 0x00},	/* '8' */
	{0x1e, 0x33, 0x33, 0x3e, 0x30, 0x18, 0x0e, 0x00},	/* '9' */
	{0x00, 0x0c, 0x0c, 0x00, 0x00, 0x0c, 0x0c, 0x00},	/* ':' */
	{0x00, 0x0c, 0x0c, 0x00, 0x00, 0x0c, 0x0c, 0x06},	/* ';' */
	{0x18, 0x0c, 0x06, 0x03, 0x06, 0x0c, 0x18, 0x00},	/* '<' */
	{0x00, 0x00, 0x3f, 0x00, 0x00, 0x3f, 0x00, 0x00},	/* '=' */
	{0x06, 0x0c, 0x18, 0x30, 0x18, 0x0c, 0x06, 0x00},	/* '>' */
	{0x1e, 0x33, 0x30, 0x18, 0x0c, 0x00, 0x0c, 0x00},	/* '?' */
	{0x3e, 0x63, 0x7b, 0x7b, 0x7b, 0x03, 0x1e, 0x00},	/* '@' */
	{0x0c, 0x1e, 0x33, 0x33, 0x3f, 0x33, 0x33, 0x00},	/* 'A' */
	{0x3f, 0x66, 0x66, 0x3e, 0x66, 0x66, 0x3f, 0x00},	/* 'B' */
	{0x3c, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3c, 0x00},	/* 'C' */
	{0x1f, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1f, 0x00},	/* 'D' */
	{0x7f, 0x46, 0x16, 0x1e, 0x16, 0x46, 0x7f, 0x00},	/* 'E' */
	{0x7f, 0x46, 0x16, 0x1e, 0x16, 0x06, 0x0f, 0x00},	/* 'F' */
	{0x3c, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7c, 0x00},	/* 'G' */
	{0x33, 0x33, 0x33, 0x3f, 0x33, 0x33, 0x33, 0x00},	/* 'H' */
	{0x1e, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00},	/* 'I' */
	{0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1e, 0x00},	/* 'J' */
	{0x67, 0x66, 0x36, 0x1e, 0x36, 0x66, 0x67, 0x00},	/* 'K' */
	{0x0f, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7f, 0x00},	/* 'L' */
	{0x63, 0x77, 0x7f, 0x7f, 0x6b, 0x63, 0x63, 0x00},	/* 'M' */
	{0x63, 0x67, 0x6f, 0x7b, 0x73, 0x63, 0x63, 0x00},	/* 'N' */
	{0x1c, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1c, 0x00},	/* 'O' */
	{0x3f, 0x66, 0x66, 0x3e, 0x06, 0x06, 0x0f, 0x00},	/* 'P' */
	{0x1e, 0x33, 0x33, 0x33, 0x3b, 0x1e, 0x38, 0x00},	/* 'Q' */
	{0x3f, 0x66, 0x66, 0x3e, 0x36, 0x66, 0x67, 0x00},	/* 'R' */
	{0x1e, 0x33, 0x07, 0x0e, 0x38, 0x33, 0x1e, 0x00},	/* 'S' */
	{0x3f, 0x2d, 0x0c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00},	/* 'T' */
	{0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3f, 0x00},	/* 'U' */
	{0x33, 0x33, 0x33, 0x33, 0x33, 0x1e, 0x0c, 0x00},	/* 'V' */
	{0x63, 0x63, 0x63, 0x6b, 0x7f, 0x77, 0x63, 0x00},	/* 'W' */
	{0x63, 0x63, 0x36, 0x1c, 0x1c, 0x36, 0x63, 0x00},	/* 'X' */
	{0x33, 0x33, 0x33, 0x1e, 0x0c, 0x0c, 0x1e, 0x00},	/* 'Y' */
	{0x7f, 0x63, 0x31, 0x18, 0x4c, 0x66, 0x7f, 0x00},	/* 'Z' */
	{0x1e, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1e, 0x00},	/* '[' */
	{0x03, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x40, 0x00},	/* 0x5c */
	{0x1e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1e, 0x00},	/* ']' */
	{0x08, 0x1c, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},	/* '^' */
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff},	/* '_' */
	{0x0c, 0x0c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},	/* '`' */
	{0x00, 0x00, 0x1e, 0x30, 0x3e, 0x33, 0x6e, 0x00},	/* 'a' */
	{0x07, 0x06, 0x06, 0x3e, 0x66, 0x66, 0x3b, 0x00},	/* 'b' */
	{0x00, 0x00, 0x1e, 0x33, 0x03, 0x33, 0x1e, 0x00},	/* 'c' */
	{0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6e, 0x00},	/* 'd' */
	{0x00, 0x00, 0x1e, 0x33, 0x3f, 0x03, 0x1e, 0x00},	/* 'e' */
	{0x1c, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0f, 0x00},	/* 'f' */
	{0x00, 0x00, 0x6e, 0x33, 0x33, 0x3e, 0x30, 0x1f},	/* 'g' */
	{0x07, 0x06, 0x36, 0x6e, 0x66, 0x66, 0x67, 0x00},	/* 'h' */
	{0x0c, 0x00, 0x0e, 0x0c, 0x0c, 0x0c, 0x1e, 0x00},	/* 'i' */
	{0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1e},	/* 'j' */
	{0x07, 0x06, 0x66, 0x36, 0x1e, 0x36, 0x67, 0x00},	/* 'k' */
	{0x0e, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00},	/* 'l' */
	{0x00, 0x00, 0x33, 0x7f, 0x7f, 0x6b, 0x63, 0x00},	/* 'm' */
	{0x00, 0x00, 0x1f, 0x33, 0x33, 0x33, 0x33, 0x00},	/* 'n' */
	{0x00, 0x00, 0x1e, 0x33, 0x33, 0x33, 0x1e, 0x00},	/* 'o' */
	{0x00, 0x00, 0x3b, 0x66, 0x66, 0x3e, 0x06, 0x0f},	/* 'p' */
	{0x00, 0x00, 0x6e, 0x33, 0x33, 0x3e, 0x30, 0x78},	/* 'q' */
	{0x00, 0x00, 0x3b, 0x6e, 0x66, 0x06, 0x0f, 0x00},	/* 'r' */
	{0x00, 0x00, 0x3e, 0x03, 0x1e, 0x30, 0x1f, 0x00},	/* 's' */
	{0x08, 0x0c, 0x3e, 0x0c, 0x0c, 0x2c, 0x18, 0x00},	/* 't' */
	{0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6e, 0x00},	/* 'u' */
	{0x00, 0x00, 0x33, 0x33, 0x33, 0x1e, 0x0c, 0x00},	/* 'v' */
	{0x00, 0x00, 0x63, 0x6b, 0x7f, 0x7f, 0x36, 0x00},	/* 'w' */
	{0x00, 0x00, 0x63, 0x36, 0x1c, 0x36, 0x63, 0x00},	/* 'x' */
	{0x00, 0x00, 0x33, 0x33, 0x33, 0x3e, 0x30, 0x1f},	/* 'y' */
	{0x00, 0x00, 0x3f, 0x19, 0x0c, 0x26, 0x3f, 0x00},	/* 'z' */
	{0x38, 0x0c, 0x0c, 0x07, 0x0c, 0x0c, 0x38, 0x00},	/* '{' */
	{0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},	/* '|' */
	{0x07, 0x0c, 0x0c, 0x38, 0x0c, 0x0c, 0x07, 0x00},	/* '}' */
	{0x6e, 0x3b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},	/* '~' */
};

static const uint8_t *glyph(int c)
{
	if (c < 0x20 || c > 0x7e)
		c = '?';
	return font8x8[c - 0x20];
}

/* ------------------------------------------------------------------ framebuffer */
static struct {
	int fd, w, h, stride, pages, page;
	struct fb_var_screeninfo var;
	uint8_t *mem;
	size_t memlen;
	uint32_t *buf;          /* back buffer, 32 bpp */
} fb;

/* ------------------------------------------------------------------ fb module */
static int load_module(const char *path, const char *params)
{
	int fd = open(path, O_RDONLY), r;
	struct stat st;
	void *img;

	if (fd < 0 || fstat(fd, &st) < 0)
		return -errno;
	img = malloc(st.st_size);
	if (!img || read(fd, img, st.st_size) != st.st_size) {
		close(fd);
		free(img);
		return -EIO;
	}
	close(fd);
	r = syscall(__NR_init_module, img, (unsigned long)st.st_size, params);
	r = r < 0 ? -errno : 0;
	free(img);
	return r == -EEXIST ? 0 : r;
}

/* front TFT: ultimo4k/duo4kse (BCM7445, SD video path), solo4k (BCM7366, SPI panel) */
static int has_tft(void)
{
	static int cached = -1;
	char c[256];
	int fd, n, i;

	if (cached >= 0)
		return cached;
	cached = 0;
	fd = open("/proc/device-tree/compatible", O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, c, sizeof(c) - 1);
	close(fd);
	for (i = 0; i < n; i++)
		if (!c[i])
			c[i] = ' ';
	c[n > 0 ? n : 0] = 0;
	if (!strncmp(c, "UNO4K ", 6))          /* uno4k: 7439 like uno4kse, but no TFT */
		return cached;
	cached = strstr(c, "brcm,bcm7445") || strstr(c, "brcm,bcm7366") || strstr(c, "brcm,bcm7439") || strstr(c, "brcm,bcm7278");
	return cached;
}

/* vuduo2: colour TFT driven by vu_tft (text only: image name or off) */
static int has_vutft(void)
{
	static int cached = -1;

	if (cached < 0)
		cached = access("/dev/vu_tft", W_OK) == 0;
	return cached;
}

/* a TFT row in the configuration: 4K TFT (vu_fb tft=) or vu_tft */
static int cfg_has_tft(void)
{
	return has_tft() || has_vutft();
}

/* front display brightness: vu_vfd / vu_tft parameters writable at run time */
static const char *const bright_params[] = {
	"/sys/module/vu_vfd/parameters/brightness",
	"/sys/module/vu_tft/parameters/brightness",
	"/sys/module/vu_fb/parameters/brightness",     /* 4K front TFT (via the MCU) */
};
#define NBRIGHT ((int)(sizeof(bright_params) / sizeof(bright_params[0])))

/* not cached: on the 4K boxes vu_fb is loaded only when the video is needed */
static int has_bright(void)
{
	int i;

	for (i = 0; i < NBRIGHT; i++)
		if (access(bright_params[i], W_OK) == 0 &&
		    (i < 2 || has_tft()))   /* vu_fb: only boxes with a TFT */
			return 1;
	return 0;
}

static void bright_apply(int level)
{
	char v[4];
	int i, fd;

	snprintf(v, sizeof(v), "%d", level);
	for (i = 0; i < NBRIGHT; i++) {
		fd = open(bright_params[i], O_WRONLY);
		if (fd < 0)
			continue;
		if (write(fd, v, strlen(v)) < 0)
			dbg("%s: write failed\n", bright_params[i]);
		close(fd);
	}
}

/* load the framebuffer module (if /dev/fb0 is not there yet) */
static void fb_module_load(void)
{
	char path[PATH_MAX], params[32] = "";
	int i, r;

	if (access("/dev/fb0", F_OK) == 0)
		return;
	if (strchr(opt_fbmod, '/')) {
		snprintf(path, sizeof(path), "%s", opt_fbmod);
	} else {        /* fixed place of the external modules (installed by vu-fb) */
		struct utsname u;

		uname(&u);
		snprintf(path, sizeof(path), "/lib/modules/%s/extra/%s", u.release, opt_fbmod);
	}
	if (access(path, R_OK)) {
		fprintf(stderr, "bootmenu: %s not found; use --fb-module PATH\n", path);
		return;
	}
	/* one display mode per box (from its BOLT); only the TFT is configurable */
	if (has_tft())
		snprintf(params, sizeof(params), "tft=%s", cfg_tft);
	r = load_module(path, params);
	dbg("insmod %s %s -> %d\n", path, params, r);
	if (r)
		fprintf(stderr, "bootmenu: cannot load %s: %s (see dmesg)\n", path, strerror(-r));
	for (i = 0; i < 50 && access("/dev/fb0", F_OK); i++)    /* devtmpfs creates the node */
		usleep(20000);
}

static int fb_open(void)
{
#ifdef OFFSCREEN
	fb.w = getenv("OFFSCREEN_W") ? atoi(getenv("OFFSCREEN_W")) : 720;
	fb.h = getenv("OFFSCREEN_H") ? atoi(getenv("OFFSCREEN_H")) : 576;
	fb.stride = fb.w * 4;
	fb.pages = 1;
#else
	struct fb_fix_screeninfo fix;

	fb_module_load();
	fb.fd = open("/dev/fb0", O_RDWR);
	if (fb.fd < 0) {
		perror("bootmenu: /dev/fb0");
		return -1;
	}
	ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.var);
	fb.var.bits_per_pixel = 32;
	fb.var.yres_virtual = fb.var.yres * 2;
	fb.var.activate = FB_ACTIVATE_NOW;
	ioctl(fb.fd, FBIOPUT_VSCREENINFO, &fb.var);       /* best effort */
	if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.var) < 0 ||
	    ioctl(fb.fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("bootmenu: fb ioctl");
		return -1;
	}
	if (fb.var.bits_per_pixel != 32 && fb.var.bits_per_pixel != 16) {
		fprintf(stderr, "bootmenu: framebuffer is %u bpp, need 16 or 32\n", fb.var.bits_per_pixel);
		return -1;
	}
	fb.w = fb.var.xres;
	fb.h = fb.var.yres;
	fb.stride = fix.line_length;
	fb.pages = fb.var.yres_virtual >= fb.var.yres * 2 ? 2 : 1;
	fb.memlen = fix.smem_len;
	fb.mem = mmap(NULL, fb.memlen, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
	if (fb.mem == MAP_FAILED) {
		perror("bootmenu: mmap");
		return -1;
	}
#endif
	fb.buf = calloc((size_t)fb.w * fb.h, 4);
	if (!fb.buf)
		return -1;
	if (has_bright())        /* 4K: vu_fb (TFT brightness) is there only now */
		bright_apply(cfg_bright);
	return 0;
}

static void fb_flip(void)
{
#ifdef OFFSCREEN
	FILE *f = fopen("/tmp/bootmenu.ppm", "wb");
	int i;

	if (!f)
		return;
	fprintf(f, "P6 %d %d 255\n", fb.w, fb.h);
	for (i = 0; i < fb.w * fb.h; i++) {
		uint32_t p = fb.buf[i];

		fputc(p >> 16 & 255, f);
		fputc(p >> 8 & 255, f);
		fputc(p & 255, f);
	}
	fclose(f);
#else
	int target = (fb.page + 1) % fb.pages, y;
	uint8_t *dst = fb.mem + (size_t)target * fb.h * fb.stride;

	if (fb.var.bits_per_pixel == 16) {         /* RGB565 (vu_fb with the TFT mirror) */
		for (y = 0; y < fb.h; y++) {
			uint16_t *d = (uint16_t *)(dst + (size_t)y * fb.stride);
			const uint32_t *sp = fb.buf + (size_t)y * fb.w;
			int x;

			for (x = 0; x < fb.w; x++) {
				uint32_t p = sp[x];
				unsigned a = p >> 24;

				if (a != 255) {                 /* no alpha in RGB565: blend on black */
					uint32_t r = (p >> 16 & 255) * a / 255, g = (p >> 8 & 255) * a / 255,
						 b = (p & 255) * a / 255;
					p = r << 16 | g << 8 | b;
				}
				d[x] = (p >> 8 & 0xf800) | (p >> 5 & 0x07e0) | (p >> 3 & 0x001f);
			}
		}
	} else
		for (y = 0; y < fb.h; y++)
			memcpy(dst + (size_t)y * fb.stride, fb.buf + (size_t)y * fb.w, fb.w * 4);
	if (fb.pages > 1) {
		fb.var.xoffset = 0;
		fb.var.yoffset = target * fb.h;
		ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.var);
	}
	fb.page = target;
#endif
}

static void fb_rect(int x, int y, int w, int h, uint32_t argb)
{
	int i, j;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > fb.w) w = fb.w - x;
	if (y + h > fb.h) h = fb.h - y;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			fb.buf[(size_t)(y + j) * fb.w + x + i] = argb;
}

static int char_w(double scale)
{
	return (int)(8 * scale + 0.5);
}

/* text with fractional scale (nearest neighbour); max_chars < 0 = unlimited */
static int fb_text(int x, int y, const char *s, uint32_t fg, double scale, int max_chars)
{
	int cw = char_w(scale), src[64], r, j, n = 0;

	if (cw > 64)
		cw = 64;
	for (j = 0; j < cw; j++) {
		src[j] = (int)(j / scale);
		if (src[j] > 7)
			src[j] = 7;
	}
	for (; *s && (max_chars < 0 || n < max_chars); s++, n++) {
		const uint8_t *g = glyph((unsigned char)*s);
		int cx = x + n * cw;

		if (cx + cw > fb.w)
			break;
		for (r = 0; r < cw; r++) {
			int yy = y + r;
			uint8_t bits = g[src[r]];

			if (yy < 0 || yy >= fb.h)
				continue;
			for (j = 0; j < cw; j++)
				if (bits >> src[j] & 1)
					fb.buf[(size_t)yy * fb.w + cx + j] = fg;
		}
	}
	return n * cw;
}

static void fb_clear(void)
{
	memset(fb.buf, 0, (size_t)fb.w * fb.h * 4);
	fb_flip();
}

/* ------------------------------------------------------------------ remote */
static int rc_fd = -1;

static int rc_open(void)
{
	char path[PATH_MAX], name[256];
	DIR *d = opendir("/sys/class/input");
	struct dirent *de;

	if (!d)
		return -1;
	while ((de = readdir(d))) {
		FILE *f;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/sys/class/input/%s/device/name", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		name[0] = 0;
		if (!fgets(name, sizeof(name), f))
			name[0] = 0;
		fclose(f);
		if (strstr(name, "remote control") || strstr(name, "vu_ir")) {
			snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
			rc_fd = open(path, O_RDONLY | O_NONBLOCK);
			dbg("remote: %s (%s)", path, strtok(name, "\n"));
			break;
		}
	}
	closedir(d);
	return rc_fd < 0 ? -1 : 0;
}

/* next key event: 1 = got one, 0 = timeout. timeout < 0 = wait forever */
static int rc_event(double timeout, int *code, int *value)
{
	double end = now() + (timeout > 0 ? timeout : 0);
	struct input_event ev;

	for (;;) {
		fd_set fds;
		struct timeval tv, *ptv = NULL;

		while (read(rc_fd, &ev, sizeof(ev)) == sizeof(ev))
			if (ev.type == EV_KEY) {
				*code = ev.code;
				*value = ev.value;
				return 1;
			}
		if (timeout >= 0) {
			double left = end - now();

			if (left <= 0)
				return 0;
			tv.tv_sec = (long)left;
			tv.tv_usec = (long)((left - tv.tv_sec) * 1e6);
			ptv = &tv;
		}
		FD_ZERO(&fds);
		FD_SET(rc_fd, &fds);
		if (select(rc_fd + 1, &fds, NULL, NULL, ptv) <= 0)
			return 0;
	}
}

static int rc_is_pressed(int key, double settle)
{
	uint8_t bits[KEY_MAX / 8 + 1];
	double end = now() + settle;
	int code, value;

	memset(bits, 0, sizeof(bits));
	if (ioctl(rc_fd, EVIOCGKEY(sizeof(bits)), bits) >= 0 && (bits[key / 8] >> (key % 8) & 1))
		return 1;
	for (;;) {
		double left = end - now();

		if (!rc_event(left > 0 ? left : 0, &code, &value))
			return 0;
		if (code == key && (value == 1 || value == 2))
			return 1;
	}
}

static void rc_wait_release(int key, double timeout)
{
	double end = now() + timeout;
	int code, value;

	while (now() < end)
		if (rc_event(end - now(), &code, &value) && code == key && value == 0)
			return;
}

/* ------------------------------------------------------------------ helpers */
static void trim(char *s)
{
	char *p = s, *e;

	while (isspace((unsigned char)*p))
		p++;
	memmove(s, p, strlen(p) + 1);
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
}

static void strip_char(char *s, char c)
{
	size_t n = strlen(s);

	if (n && s[n - 1] == c)
		s[--n] = 0;
	if (n && s[0] == c)
		memmove(s, s + 1, n);
}

static void strip_slashes(char *s)
{
	while (*s == '/')
		memmove(s, s + 1, strlen(s));
	while (*s && s[strlen(s) - 1] == '/')
		s[strlen(s) - 1] = 0;
}

static int read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY), n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return n;
}

static int is_dir(const char *p)
{
	struct stat st;

	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int exists(const char *p)
{
	return access(p, F_OK) == 0;
}

/* value of key in a "k=v k2=v2" string (STARTUP line); quotes stripped */
static void startup_get(const char *text, const char *key, char *out, size_t len)
{
	const char *p = text;
	size_t kl = strlen(key);

	out[0] = 0;
	while (*p) {
		const char *e;

		while (*p && isspace((unsigned char)*p))
			p++;
		e = p;
		while (*e && !isspace((unsigned char)*e))
			e++;
		if ((size_t)(e - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
			size_t n = e - p - kl - 1;

			if (n >= len)
				n = len - 1;
			memcpy(out, p + kl + 1, n);
			out[n] = 0;
			{			/* drop all quotes: root=UUID="x" / 'x' -> UUID=x */
				char *r = out, *w = out;

				for (; *r; r++)
					if (*r != '"' && *r != '\'')
						*w++ = *r;
				*w = 0;
			}
			return;
		}
		p = e;
	}
}

/* ------------------------------------------------------------------ blkid */
struct blk {
	char dev[64], uuid[64], partuuid[64], label[64], partlabel[64], type[16];
};
static struct blk blks[32];
static int nblk = -1;

static void blk_attr(const char *line, const char *key, char *out, size_t len)
{
	char pat[24];
	const char *p = line;

	snprintf(pat, sizeof(pat), " %s=\"", key);
	out[0] = 0;
	while ((p = strstr(p, pat))) {
		const char *v = p + strlen(pat), *e = strchr(v, '"');

		if (e) {
			size_t n = e - v < (long)len - 1 ? (size_t)(e - v) : len - 1;

			memcpy(out, v, n);
			out[n] = 0;
		}
		return;
	}
}

static void blkid_load(void)
{
	char line[512];
	FILE *p;

	if (nblk >= 0)
		return;
	nblk = 0;
	p = popen("blkid 2>/dev/null", "r");
	if (!p)
		return;
	while (fgets(line, sizeof(line), p) && nblk < 32) {
		struct blk *b = &blks[nblk];
		char *c = strchr(line, ':');
		char rest[512];

		if (!c)
			continue;
		*c = 0;
		snprintf(b->dev, sizeof(b->dev), "%s", line);
		snprintf(rest, sizeof(rest), " %s", c + 1);   /* leading space for exact key match */
		blk_attr(rest, "UUID", b->uuid, sizeof(b->uuid));
		blk_attr(rest, "PARTUUID", b->partuuid, sizeof(b->partuuid));
		blk_attr(rest, "LABEL", b->label, sizeof(b->label));
		blk_attr(rest, "PARTLABEL", b->partlabel, sizeof(b->partlabel));
		blk_attr(rest, "TYPE", b->type, sizeof(b->type));
		nblk++;
	}
	pclose(p);
}

static struct blk *blk_find(const char *dev)
{
	int i;

	blkid_load();
	for (i = 0; i < nblk; i++)
		if (!strcmp(blks[i].dev, dev))
			return &blks[i];
	return NULL;
}

/* root= -> device path (/dev/xxx, UUID=, PARTUUID=, LABEL=, PARTLABEL=). 0 = found */
static int resolve_dev(const char *root, char *dev, size_t len)
{
	char key[16], val[128], link[PATH_MAX], real[PATH_MAX];
	const char *eq = strchr(root, '=');
	const char *dir = NULL;
	int i;

	dev[0] = 0;
	if (!*root)
		return -1;
	if (!eq) {
		snprintf(dev, len, "%s", root);
		return 0;
	}
	snprintf(key, sizeof(key), "%.*s", (int)(eq - root), root);
	for (i = 0; key[i]; i++)
		key[i] = toupper((unsigned char)key[i]);
	snprintf(val, sizeof(val), "%s", eq + 1);
	strip_char(val, '"');
	strip_char(val, '\'');

	if (!strcmp(key, "UUID"))
		dir = "/dev/disk/by-uuid/";
	else if (!strcmp(key, "PARTUUID"))
		dir = "/dev/disk/by-partuuid/";
	else if (!strcmp(key, "LABEL"))
		dir = "/dev/disk/by-label/";
	else if (!strcmp(key, "PARTLABEL"))
		dir = "/dev/disk/by-partlabel/";
	if (dir) {
		snprintf(link, sizeof(link), "%s%s", dir, val);
		if (realpath(link, real)) {
			snprintf(dev, len, "%s", real);
			return 0;
		}
	}
	blkid_load();
	for (i = 0; i < nblk; i++) {
		const char *v = !strcmp(key, "UUID") ? blks[i].uuid :
				!strcmp(key, "PARTUUID") ? blks[i].partuuid :
				!strcmp(key, "LABEL") ? blks[i].label :
				!strcmp(key, "PARTLABEL") ? blks[i].partlabel : "";
		int uuid = strstr(key, "UUID") != NULL;

		if (*v && (uuid ? !strcasecmp(v, val) : !strcmp(v, val))) {
			snprintf(dev, len, "%s", blks[i].dev);
			return 0;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ disks */
/* disk of root= present? (a root without '=' is a fixed device: always ok) */
static int dev_present(const char *root)
{
	char dev[PATH_MAX];

	if (!*root || !strchr(root, '='))
		return 1;
	return resolve_dev(root, dev, sizeof(dev)) == 0;
}

/* new USB disks: /proc/partitions is cheap to read, so check it every 0.5 s;
 * only when it changes run "mdev -s" (slow: it walks all of /sys) and read
 * blkid again on the next lookup. Returns 1 if the partitions changed */
static int dev_rescan(void)
{
	static char last[2048];
	static double next;
	char cur[2048] = "", line[128];
	FILE *f;

	if (now() < next)
		return 0;
	next = now() + 0.5;
	f = fopen("/proc/partitions", "r");
	if (f) {
		while (fgets(line, sizeof(line), f))
			if (strlen(cur) + strlen(line) < sizeof(cur))
				strcat(cur, line);
		fclose(f);
	}
	if (!strcmp(cur, last))
		return 0;
	snprintf(last, sizeof(last), "%s", cur);
	if (system("mdev -s >/dev/null 2>&1") < 0)
		dbg("mdev -s failed");
	nblk = -1;
	return 1;
}

/* root= of the slot that boots without a choice (STARTUP_ONCE, else STARTUP) */
static void boot_root(const char *rootdir, char *root, size_t len)
{
	char path[PATH_MAX], text[1024];

	root[0] = 0;
	snprintf(path, sizeof(path), "%s/STARTUP_ONCE", rootdir);
	if (read_file(path, text, sizeof(text)) < 0) {
		snprintf(path, sizeof(path), "%s/STARTUP", rootdir);
		if (read_file(path, text, sizeof(text)) < 0)
			return;
	}
	startup_get(text, "root", root, len);
}

/* never exit before --min-time seconds from the start */
static void min_time(void)
{
	double left = t_start + opt_min - now();

	if (left > 0)
		usleep((useconds_t)(left * 1e6));
}

/* mount point of dev (if already mounted) or of path (longest prefix) */
static int mounts_lookup(const char *dev, const char *path, char *out, size_t len)
{
	char line[512], d[256], mp[256], rd[PATH_MAX], rdev[PATH_MAX];
	size_t best = 0;
	FILE *f = fopen("/proc/mounts", "r");
	int found = -1;

	out[0] = 0;
	if (!f)
		return -1;
	if (dev && !realpath(dev, rdev))
		snprintf(rdev, sizeof(rdev), "%s", dev);
	while (fgets(line, sizeof(line), f)) {
		char *o;

		if (sscanf(line, "%255s %255s", d, mp) != 2)
			continue;
		while ((o = strstr(mp, "\\040"))) {             /* escaped space */
			*o = ' ';
			memmove(o + 1, o + 4, strlen(o + 4) + 1);
		}
		if (dev) {
			if (!strcmp(d, dev) || (!strncmp(d, "/dev/", 5) && realpath(d, rd) && !strcmp(rd, rdev))) {
				snprintf(out, len, "%s", mp);
				found = 0;
				break;
			}
		} else {
			size_t l = strlen(mp);

			if (!strncmp(path, mp, l) && (path[l] == '/' || path[l] == 0 || l == 1) && l >= best) {
				best = l;
				snprintf(out, len, "%s", d);
				found = 0;
			}
		}
	}
	fclose(f);
	return found;
}

/* ------------------------------------------------------------------ image name */
static int kv_get(const char *text, const char *key, char *out, size_t len)
{
	const char *p = text;

	out[0] = 0;
	while (p && *p) {
		const char *eol = strchr(p, '\n'), *eq;
		char k[64];
		size_t ll = eol ? (size_t)(eol - p) : strlen(p);

		eq = memchr(p, '=', ll);
		if (eq && *p != '#') {
			snprintf(k, sizeof(k), "%.*s", (int)(eq - p), p);
			trim(k);
			if (!strcasecmp(k, key)) {
				snprintf(out, len, "%.*s", (int)(p + ll - eq - 1), eq + 1);
				trim(out);
				strip_char(out, '"');
				return *out != 0;
			}
		}
		p = eol ? eol + 1 : NULL;
	}
	return 0;
}

/* Creator-Version(Build)-Type, e.g. OpenBh-5.6(002)-nightly. 0 = found */
static int read_image_name(const char *rootdir, char *name, size_t len)
{
	static const char *files[] = { "etc/image-version", "etc/enigma2/image-version",
				       "etc/openvision/image-version" };
	char path[PATH_MAX], txt[4096], c[64], v[64], b[64], t[64];
	unsigned i;

	name[0] = 0;
	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", rootdir, files[i]);
		if (read_file(path, txt, sizeof(txt)) < 0)
			continue;
		if (!kv_get(txt, "creator", c, sizeof(c)) && !kv_get(txt, "distro", c, sizeof(c)))
			kv_get(txt, "imagename", c, sizeof(c));
		if (!kv_get(txt, "version", v, sizeof(v)))
			kv_get(txt, "imageversion", v, sizeof(v));
		if (!kv_get(txt, "build", b, sizeof(b)))
			kv_get(txt, "imagebuild", b, sizeof(b));
		if (!kv_get(txt, "type", t, sizeof(t)))
			kv_get(txt, "imagetype", t, sizeof(t));
		if (*c || *v) {
			snprintf(name, len, "%s%s%s%s%s%s%s%s", c, (*c && *v) ? "-" : "", v,
				 *b ? "(" : "", b, *b ? ")" : "", *t ? "-" : "", t);
			return 0;
		}
	}
	snprintf(path, sizeof(path), "%s/etc/issue", rootdir);
	if (read_file(path, txt, sizeof(txt)) > 0) {
		char *nl = strchr(txt, '\n'), *p;

		if (nl)
			*nl = 0;
		while ((p = strstr(txt, "\\n")) || (p = strstr(txt, "\\l")))
			memmove(p, p + 2, strlen(p + 2) + 1);
		if ((p = strstr(txt, "Welcome to")))
			memmove(p, p + 10, strlen(p + 10) + 1);
		trim(txt);
		if (*txt) {
			snprintf(name, len, "%s", txt);
			return 0;
		}
	}
	return -1;
}

static void try_read(const char *dir, char *name, size_t len, char *status, size_t slen)
{
	int r = read_image_name(dir, name, len);

	dbg("  read %s/etc/image-version -> %s", dir, r ? "(none)" : name);
	if (r)
		snprintf(status, slen, "%s", is_dir(dir) ? "no version info" : "folder missing");
}

static void image_name(const char *root, const char *sub, const char *base_root,
		       const char *base_dev, char *name, size_t len, char *status, size_t slen)
{
	char dev[PATH_MAX], dir[PATH_MAX], parent[PATH_MAX], mp[PATH_MAX];
	const char *bases[3];
	struct blk *b;
	const char *types[6];
	int i, nt = 0, has_dev, err = 0;

	name[0] = status[0] = 0;
	if (!strncmp(root, "ubi", 3) && strchr(root, ':')) {    /* UBI volume (MIPS flash) */
		mkdir("/tmp", 01777);
		mkdir(MNT_TMP, 0755);
		if (mount(root, MNT_TMP, "ubifs", MS_RDONLY, NULL)) {
			snprintf(status, slen, "mount failed: %s", strerror(errno));
			return;
		}
		snprintf(dir, sizeof(dir), "%s/%s", MNT_TMP, sub);
		try_read(dir, name, len, status, slen);
		umount(MNT_TMP);
		return;
	}
	has_dev = resolve_dev(root, dev, sizeof(dev)) == 0;
	dbg("root=%s rootsubdir=%s -> dev=%s (base_dev=%s)", root, sub, has_dev ? dev : "-", base_dev);

	snprintf(parent, sizeof(parent), "%s", base_root);
	if (strrchr(parent, '/') && strrchr(parent, '/') != parent)
		*strrchr(parent, '/') = 0;
	else
		snprintf(parent, sizeof(parent), "/");
	bases[0] = base_root;
	bases[1] = parent;
	bases[2] = "/";

	b = has_dev ? blk_find(dev) : NULL;
	if (!has_dev && strchr(root, '=')) {
		snprintf(status, slen, "disk missing");
		return;
	}
	/* same partition as the STARTUP files, or a partition without a
	 * filesystem (kernel/initrd/splash in flash) -> local subfolder */
	if (!has_dev || !strcmp(dev, base_dev) || !exists(dev) || (b && !b->type[0])) {
		for (i = 0; i < 3 && *sub; i++) {
			snprintf(dir, sizeof(dir), "%s/%s/etc", bases[i], sub);
			if (is_dir(dir)) {
				snprintf(dir, sizeof(dir), "%s/%s", bases[i], sub);
				try_read(dir, name, len, status, slen);
				return;
			}
		}
		dbg("  %s not found locally", sub);
		return;
	}
	/* another disk (USB/HDD): reuse an existing mount or mount read-only */
	if (mounts_lookup(dev, NULL, mp, sizeof(mp)) == 0) {
		dbg("  %s already mounted on %s", dev, mp);
		snprintf(dir, sizeof(dir), "%s/%s", mp, sub);
		try_read(dir, name, len, status, slen);
		return;
	}
	if (b && b->type[0])
		types[nt++] = b->type;
	types[nt++] = "ext4";
	types[nt++] = "ext3";
	types[nt++] = "ext2";
	types[nt++] = "vfat";
	mkdir("/tmp", 01777);           /* the kexec initrd has no /tmp */
	mkdir(MNT_TMP, 0755);
	for (i = 0; i < nt; i++) {
		int r = mount(dev, MNT_TMP, types[i], MS_RDONLY, NULL);

		dbg("  mount -t %s -o ro %s -> %d (%s)", types[i], dev, r, r ? strerror(errno) : "ok");
		if (r == 0)
			break;
		if (i == 0)
			err = errno;            /* errno of the type blkid reported */
	}
	if (i == nt) {
		snprintf(status, slen, "mount failed: %s", strerror(err));
		return;
	}
	snprintf(dir, sizeof(dir), "%s/%s", MNT_TMP, sub);
	try_read(dir, name, len, status, slen);
	umount(MNT_TMP);
}

/* ------------------------------------------------------------------ entries */
struct entry {
	char file[32], label[32], name[96], status[64];
	char kernel[MAX_STR], root[MAX_STR], subdir[MAX_STR];
	int current, missing;
};
static struct entry entries[MAX_ENTRIES];
static int nentries;

/* any slot (or the slot to boot) on a disk found by UUID=/LABEL=? */
static int need_scan(const char *broot)
{
	int i;

	if (strchr(broot, '='))
		return 1;
	for (i = 0; i < nentries; i++)
		if (strchr(entries[i].root, '='))
			return 1;
	return 0;
}

static int same_slot(const char *a, const char *b)
{
	static const char *keys[] = { "kernel", "root", "rootsubdir" };
	char va[MAX_STR], vb[MAX_STR];
	unsigned i;

	if (!a || !b || !*a || !*b)
		return 0;
	for (i = 0; i < 3; i++) {
		startup_get(a, keys[i], va, sizeof(va));
		startup_get(b, keys[i], vb, sizeof(vb));
		strip_slashes(va);
		strip_slashes(vb);
		if (strcmp(va, vb))
			return 0;
	}
	return 1;
}

static int cmp_entry(const void *x, const void *y)
{
	const struct entry *a = x, *b = y;
	int ra = !strcmp(a->file, "STARTUP_RECOVERY"), rb = !strcmp(b->file, "STARTUP_RECOVERY");
	int fa = !strcmp(a->file, "STARTUP_FLASH"), fb_ = !strcmp(b->file, "STARTUP_FLASH");

	if (ra != rb)
		return ra - rb;
	if (fa != fb_)                          /* chkroot: the flash image first */
		return fb_ - fa;
	return atoi(a->file + 8) - atoi(b->file + 8);
}

static void load_entries(const char *root)
{
	char path[PATH_MAX], text[1024], current[1024], base_dev[PATH_MAX];
	DIR *d = opendir(root);
	struct dirent *de;
	int i, have_cur;

	nentries = 0;
	if (!d)
		return;
	while ((de = readdir(d)) && nentries < MAX_ENTRIES) {
		const char *s = de->d_name;
		int ok = !strcmp(s, "STARTUP_RECOVERY") || !strcmp(s, "STARTUP_FLASH");

		if (!ok && !strncmp(s, "STARTUP_", 8) && s[8]) {
			const char *p = s + 8;

			while (isdigit((unsigned char)*p))
				p++;
			ok = (*p == 0);
		}
		if (ok)
			snprintf(entries[nentries++].file, sizeof(entries[0].file), "%s", s);
	}
	closedir(d);
	qsort(entries, nentries, sizeof(entries[0]), cmp_entry);

	snprintf(path, sizeof(path), "%s/STARTUP", root);
	have_cur = read_file(path, current, sizeof(current)) >= 0;
	if (mounts_lookup(NULL, root, base_dev, sizeof(base_dev)) < 0)
		base_dev[0] = 0;

	for (i = 0; i < nentries; i++) {
		struct entry *e = &entries[i];

		snprintf(path, sizeof(path), "%s/%s", root, e->file);
		if (read_file(path, text, sizeof(text)) < 0)
			text[0] = 0;
		trim(text);
		startup_get(text, "kernel", e->kernel, sizeof(e->kernel));
		startup_get(text, "root", e->root, sizeof(e->root));
		startup_get(text, "rootsubdir", e->subdir, sizeof(e->subdir));
		strip_slashes(e->subdir);
		e->current = have_cur && same_slot(text, current);
		e->name[0] = e->status[0] = 0;
		e->missing = !dev_present(e->root);
		if (!strcmp(e->file, "STARTUP_RECOVERY")) {
			snprintf(e->label, sizeof(e->label), "Recovery");
		} else if (!strcmp(e->file, "STARTUP_FLASH")) {
			snprintf(e->label, sizeof(e->label), "Flash");
			image_name(e->root, e->subdir, root, base_dev, e->name, sizeof(e->name),
				   e->status, sizeof(e->status));
		} else {
			snprintf(e->label, sizeof(e->label), "Slot %s", e->file + 8);
			image_name(e->root, e->subdir, root, base_dev, e->name, sizeof(e->name),
				   e->status, sizeof(e->status));
		}
	}
}

/* ------------------------------------------------------------------ UI */
#define C_BG     0xff101828
#define C_PANEL  0xff1c2a44
#define C_TITLE  0xffffffff
#define C_TEXT   0xffd0d8e8
#define C_DIM    0xff7080a0
#define C_SEL_BG 0xffe0a020
#define C_SEL_FG 0xff101010
#define C_FOOT   0xff8898b8
#define C_CUR    0xff40c060

static void safe(int *mx, int *my, int *sw, int *sh)
{
	*mx = fb.w * opt_margin / 100;
	*my = fb.h * opt_margin / 100;
	*sw = fb.w - 2 * *mx;
	*sh = fb.h - *my - fb.h * (opt_margin_bottom >= 0 ? opt_margin_bottom : opt_margin) / 100;
}

/* layout scale against the 720x576 reference (720p -> 1.25) */
static double res_scale(void)
{
	return fb.h / 576.0;
}

#define K(v) ((int)((v) * k))

/* returns rows per page */
static int draw(int sel, int remaining)
{
	int mx, my, sw, sh, cw, lh, cols, th, scw, scols, foot_h, det_h, y0, rows, first, row, i;
	double k = res_scale(), f = opt_font * k, sf = opt_small * k;
	double tf = f * 1.6 < 2 * k ? 2 * k : f * 1.6;
	char buf[2 * MAX_STR];
	int bar;

	safe(&mx, &my, &sw, &sh);
	cw = char_w(f);
	lh = cw + (cw / 2 > 6 ? cw / 2 : 6);
	cols = (sw - 24) / cw;
	th = (int)(8 * tf + 20 * k + 0.5);
	scw = char_w(sf);
	scols = (sw - 24) / scw;
	foot_h = scw + K(10);
	det_h = 2 * (scw + 3) + K(8);

	fb_rect(0, 0, fb.w, fb.h, C_BG);
	fb_rect(0, 0, fb.w, my + th, C_PANEL);
	fb_text(mx + 8, my + K(10), "Vu+ Multiboot", C_TITLE, tf, -1);

	y0 = my + th + K(14);
	rows = (sh - th - K(14) - det_h - foot_h) / lh;
	if (rows < 1)
		rows = 1;
	first = sel - rows / 2;
	if (first > nentries - rows)
		first = nentries - rows;
	if (first < 0)
		first = 0;
	for (row = 0, i = first; i < nentries && row < rows; i++, row++) {
		struct entry *e = &entries[i];
		int y = y0 + row * lh;
		uint32_t fg = C_TEXT;
		const char *shown = e->name;

		if (i == sel) {
			fb_rect(mx, y - (lh - cw) / 2, sw, lh, C_SEL_BG);
			fg = C_SEL_FG;
		}
		fb_text(mx + 12, y, e->label, fg, f, 9);
		if (!*shown) {
			if (*e->status)
				snprintf(buf, sizeof(buf), "(%s)", e->status);
			else
				snprintf(buf, sizeof(buf), "-");
			shown = buf;
		}
		fb_text(mx + 12 + 10 * cw, y, shown, *e->name ? fg : C_DIM, f, cols - 12);
		if (e->current)
			fb_text(mx + sw - 12 - cw, y, "*", i == sel ? C_SEL_FG : C_CUR, f, 1);
	}
	if (nentries > rows) {                  /* scroll position, in the title bar */
		snprintf(buf, sizeof(buf), "%d/%d", sel + 1, nentries);
		fb_text(mx + sw - 8 - (int)strlen(buf) * cw, my + K(10) + (char_w(tf) - cw) / 2,
			buf, C_FOOT, f, -1);
	}

	{
		struct entry *e = &entries[sel];
		int yd = my + sh - foot_h - det_h + 4;

		snprintf(buf, sizeof(buf), "kernel: %s", *e->kernel ? e->kernel : "?");
		fb_text(mx + 12, yd, buf, C_DIM, sf, scols);
		snprintf(buf, sizeof(buf), "root:   %s%s%s", *e->root ? e->root : "?",
			 *e->subdir ? " /" : "", e->subdir);
		fb_text(mx + 12, yd + scw + 3, buf, C_DIM, sf, scols);
	}
	bar = my + sh - foot_h;                 /* bottom bar reaches the screen edge */
	fb_rect(0, bar, fb.w, fb.h - bar, C_PANEL);
	if (remaining == -2)
		snprintf(buf, sizeof(buf), "Disk of current slot missing: insert it or choose a slot");
	else if (remaining >= 0)
		snprintf(buf, sizeof(buf), "Booting current slot in %d s", remaining);
	else
		snprintf(buf, sizeof(buf), "OK boot  EXIT cancel  BLUE config  * current");
	fb_text(mx + 12, bar + (fb.h - bar - scw) / 2, buf, C_FOOT, sf, scols);  /* centered in the bar */
	fb_flip();
	return rows;
}

/* centered text, '\n' separates lines; each line shrinks to fit */
static void message(const char *text, uint32_t color)
{
	char lines[4][128];
	double k = res_scale(), sc[4];
	int n = 0, i, total = 0, y, mx, my, sw, sh, gap = K(12);
	const char *p = text;

	safe(&mx, &my, &sw, &sh);
	while (*p && n < 4) {
		const char *e = strchr(p, '\n');
		size_t l = e ? (size_t)(e - p) : strlen(p);

		snprintf(lines[n], sizeof(lines[n]), "%.*s", (int)l, p);
		n++;
		p = e ? e + 1 : p + l;
	}
	for (i = 0; i < n; i++) {
		sc[i] = 3 * k;
		while (sc[i] > 1 && (int)strlen(lines[i]) * char_w(sc[i]) > sw)
			sc[i] -= 0.5;
		total += char_w(sc[i]);
	}
	total += gap * (n - 1);
	y = (fb.h - total) / 2;
	fb_rect(0, 0, fb.w, fb.h, C_BG);
	for (i = 0; i < n; i++) {
		int maxc = sw / char_w(sc[i]), len = strlen(lines[i]);

		if (len > maxc)
			len = maxc;
		fb_text((fb.w - len * char_w(sc[i])) / 2, y, lines[i], color, sc[i], len);
		y += char_w(sc[i]) + gap;
	}
	fb_flip();
}

static void hint(int seconds, const char *what)
{
	int mx, my, sw, sh;
	double k = res_scale();
	char buf[64];

	safe(&mx, &my, &sw, &sh);
	fb_rect(0, 0, fb.w, fb.h, 0x00000000);              /* transparent */
	fb_rect(0, my + sh - K(56), fb.w, fb.h - (my + sh - K(56)), 0xc0101828);
	snprintf(buf, sizeof(buf), "%s (%d)", what, seconds);
	fb_text(mx + 12, my + sh - K(38), buf, C_TITLE, 2 * k, -1);
	fb_flip();
}

/* ------------------------------------------------------------------ select */
static int select_entry(const char *root, struct entry *e)
{
	char src[PATH_MAX], dst[PATH_MAX], tmp[PATH_MAX], data[4096];
	int fd, n;

	snprintf(src, sizeof(src), "%s/%s", root, e->file);
	snprintf(dst, sizeof(dst), "%s/STARTUP", root);
	snprintf(tmp, sizeof(tmp), "%s/STARTUP.tmp", root);
	printf("bootmenu: %s -> STARTUP\n", e->file);
	if (opt_dry)
		return 0;
	n = read_file(src, data, sizeof(data));
	if (n < 0)
		return -1;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, data, n) != n || fsync(fd) < 0) {
		perror("bootmenu: write STARTUP.tmp");
		if (fd >= 0)
			close(fd);
		return -1;
	}
	close(fd);
	if (rename(tmp, dst) < 0) {                         /* atomic replace */
		perror("bootmenu: rename");
		return -1;
	}
	snprintf(tmp, sizeof(tmp), "%s/STARTUP_ONCE", root);
	unlink(tmp);                                        /* manual choice wins */
	sync();
	return 0;
}

/* ------------------------------------------------------------------ main */
/* ------------------------------------------------------------------ front TFT (fb1) */
/* tft=own: vu_fb exposes the TFT as /dev/fb1 (RGB565); show the selected image there */
static void tft_text(uint16_t *px, int w, int h, int stride, int y, const char *s, int scale, uint16_t fg)
{
	int n = (int)strlen(s), cw = 8 * scale, x0, r, j, i;

	if (n * cw > w)
		n = w / cw;
	x0 = (w - n * cw) / 2;
	for (i = 0; i < n; i++) {
		const uint8_t *g = glyph((unsigned char)s[i]);

		for (r = 0; r < cw; r++) {
			int yy = y + r;

			if (yy < 0 || yy >= h)
				continue;
			for (j = 0; j < cw; j++)
				if (g[r / scale] >> (j / scale) & 1)
					px[(size_t)yy * stride + x0 + i * cw + j] = fg;
		}
	}
}

static void tft_show(const char *line1, const char *line2)
{
	static int fd = -2, w, h, stride;
	static uint16_t *mem;
	static char last[160];
	char key[160];
	int sc;

	if (has_vutft()) {               /* vuduo2: text to /dev/vu_tft, "" when off */
		int t;

		snprintf(key, sizeof(key), "%s%s%s", strcmp(cfg_tft, "off") ? line1 : "",
			 strcmp(cfg_tft, "off") && line2 && *line2 ? "\n" : "",
			 strcmp(cfg_tft, "off") && line2 ? line2 : "");
		if (!strcmp(key, last))
			return;
		snprintf(last, sizeof(last), "%s", key);
		t = open("/dev/vu_tft", O_WRONLY);   /* the driver draws at close */
		if (t < 0)
			return;
		if (write(t, key, strlen(key)) < 0)
			dbg("vu_tft write failed\n");
		close(t);
		return;
	}
	if (!has_tft() || strcmp(cfg_tft, "own"))
		return;
	snprintf(key, sizeof(key), "%s|%s", line1, line2);
	if (fd == -2) {
		struct fb_var_screeninfo v;
		struct fb_fix_screeninfo f;

		fd = open("/dev/fb1", O_RDWR);
		if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 ||
		    ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0 || v.bits_per_pixel != 16) {
			dbg("no TFT framebuffer /dev/fb1\n");
			return;
		}
		w = v.xres;
		h = v.yres;
		stride = f.line_length / 2;
		mem = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (mem == MAP_FAILED)
			mem = NULL;
	}
	if (!mem || !strcmp(key, last))
		return;
	snprintf(last, sizeof(last), "%s", key);
	memset(mem, 0, (size_t)stride * 2 * h);
	sc = h >= 240 ? 4 : 2;                       /* 32 px glyphs on 720x288 */
	tft_text(mem, w, h, stride, h / 2 - 8 * sc - 6, line1, sc, 0x07ff);       /* cyan */
	if (line2 && *line2)
		tft_text(mem, w, h, stride, h / 2 + 6, line2, sc > 2 ? sc - 1 : sc, 0xffff);
}

/* front text display (vu_vfd: MCU VFD, vuultimo LCD, vuduo2 OLED): "line1\nline2".
 * A VFD shows only the first line, the LCD both (large when they are short). */
static void vfd_show(const char *line1, const char *line2)
{
	static char last[160];
	char buf[160];
	int n, fd;

	n = snprintf(buf, sizeof(buf), "%s%s%s", line1, line2 && *line2 ? "\n" : "", line2 ? line2 : "");
	if (n < 0 || !strcmp(buf, last))
		return;
	snprintf(last, sizeof(last), "%s", buf);
	fd = open("/dev/vu_vfd", O_WRONLY);   /* vu_vfd >= 1.5 draws at close */
	if (fd < 0)
		return;
	if (write(fd, buf, strlen(buf)) < 0)
		dbg("vu_vfd write failed\n");
	close(fd);
}

/* ------------------------------------------------------------------ STARTUP_CONFIG */
static void config_load(const char *root)
{
	char path[PATH_MAX], text[2048], v[32];
	int i;

	snprintf(path, sizeof(path), "%s/STARTUP_CONFIG", root);
	if (read_file(path, text, sizeof(text)) < 0)
		return;
	if (kv_get(text, "tft", v, sizeof(v)))
		for (i = 0; i < NTFTS; i++)
			if (!strcasecmp(v, tfts[i].name))
				snprintf(cfg_tft, sizeof(cfg_tft), "%s", tfts[i].name);
	if (kv_get(text, "wait", v, sizeof(v)))
		cfg_wait = atoi(v) < 0 ? 0 : atoi(v);
	if (kv_get(text, "min", v, sizeof(v)))
		cfg_min = atoi(v) < 0 ? 0 : atoi(v);
	if (kv_get(text, "bright", v, sizeof(v)) && atoi(v) >= 1 && atoi(v) <= 4)
		cfg_bright = atoi(v);
	dbg("STARTUP_CONFIG: tft=%s wait=%d min=%d bright=%d\n", cfg_tft, cfg_wait, cfg_min, cfg_bright);
}

/* rewrite tft= (TFT boxes), wait= and min=, keep every other line; atomic */
static int config_save(const char *root)
{
	char path[PATH_MAX], tmp[PATH_MAX], text[2048], out[2600];
	const char *p = text;
	size_t o = 0;
	int fd, n, have_tft = !cfg_has_tft(), have_wait = 0, have_min = 0, have_bright = !has_bright();

	snprintf(path, sizeof(path), "%s/STARTUP_CONFIG", root);
	snprintf(tmp, sizeof(tmp), "%s/STARTUP_CONFIG.tmp", root);
	if (read_file(path, text, sizeof(text)) < 0)
		text[0] = 0;
	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t ll = eol ? (size_t)(eol - p) : strlen(p);
		char line[512];

		snprintf(line, sizeof(line), "%.*s", (int)ll, p);
		trim(line);
		if (cfg_has_tft() && (!strncasecmp(line, "tft=", 4) || !strncasecmp(line, "tft =", 5))) {
			if (!have_tft++)
				o += snprintf(out + o, sizeof(out) - o, "tft=%s\n", cfg_tft);
		} else if (!strncasecmp(line, "wait=", 5) || !strncasecmp(line, "wait =", 6)) {
			if (!have_wait++)
				o += snprintf(out + o, sizeof(out) - o, "wait=%d\n", cfg_wait);
		} else if (!strncasecmp(line, "min=", 4) || !strncasecmp(line, "min =", 5)) {
			if (!have_min++)
				o += snprintf(out + o, sizeof(out) - o, "min=%d\n", cfg_min);
		} else if (has_bright() && (!strncasecmp(line, "bright=", 7) || !strncasecmp(line, "bright =", 8))) {
			if (!have_bright++)
				o += snprintf(out + o, sizeof(out) - o, "bright=%d\n", cfg_bright);
		} else if (*line) {
			o += snprintf(out + o, sizeof(out) - o, "%s\n", line);
		}
		if (o >= sizeof(out))
			return -1;
		p = eol ? eol + 1 : p + ll;
	}
	if (!have_tft)
		o += snprintf(out + o, sizeof(out) - o, "tft=%s\n", cfg_tft);
	if (!have_wait)
		o += snprintf(out + o, sizeof(out) - o, "wait=%d\n", cfg_wait);
	if (!have_min)
		o += snprintf(out + o, sizeof(out) - o, "min=%d\n", cfg_min);
	if (!have_bright)
		o += snprintf(out + o, sizeof(out) - o, "bright=%d\n", cfg_bright);
	printf("bootmenu: STARTUP_CONFIG tft=%s wait=%d min=%d\n", cfg_tft, cfg_wait, cfg_min);
	if (opt_dry)
		return 0;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	n = (int)o;
	if (fd < 0 || write(fd, out, n) != n || fsync(fd) < 0) {
		perror("bootmenu: write STARTUP_CONFIG.tmp");
		if (fd >= 0)
			close(fd);
		return -1;
	}
	close(fd);
	if (rename(tmp, path) < 0)
		return -1;
	sync();
	return 0;
}

static int tft_index(void)
{
	int i;

	for (i = 0; i < NTFTS; i++)
		if (!strcmp(cfg_tft, tfts[i].name))
			return i;
	return 0;
}

static int wait_index(void)
{
	int i;

	for (i = 0; i < NWAITS; i++)
		if (waits[i] == cfg_wait)
			return i;
	return 0;
}

static int min_index(void)
{
	int i;

	for (i = 0; i < NMINS; i++)
		if (mins[i] == cfg_min)
			return i;
	return 1;
}

/* rows: 0 menu wait, 1 minimum time, 2 front TFT (TFT boxes only) */
/* rows: menu wait, minimum time, front TFT (TFT boxes), brightness of the front displays (vu_vfd / vu_tft / vu_fb) */
enum { ROW_WAIT, ROW_MIN, ROW_TFT, ROW_BRIGHT };
#define NCFG (2 + cfg_has_tft() + has_bright())

static int cfg_row(int i)
{
	if (i < 2)
		return i;
	if (i == 2 && cfg_has_tft())
		return ROW_TFT;
	return ROW_BRIGHT;
}

static void draw_config(int sel)
{
	int mx, my, sw, sh, cw, lh, th, scw, foot_h, y0, i, bar;
	double k = res_scale(), f = opt_font * k, sf = opt_small * k;
	double tf = f * 1.6 < 2 * k ? 2 * k : f * 1.6;
	char val[64];

	safe(&mx, &my, &sw, &sh);
	cw = char_w(f);
	lh = cw + (cw / 2 > 6 ? cw / 2 : 6);
	th = (int)(8 * tf + 20 * k + 0.5);
	scw = char_w(sf);
	foot_h = scw + K(10);

	fb_rect(0, 0, fb.w, fb.h, C_BG);
	fb_rect(0, 0, fb.w, my + th, C_PANEL);
	fb_text(mx + 8, my + K(10), "Configuration", C_TITLE, tf, -1);
	y0 = my + th + K(14);
	for (i = 0; i < NCFG; i++) {
		int y = y0 + i * lh * 3 / 2;
		uint32_t fg = C_TEXT;

		if (i == sel) {
			fb_rect(mx, y - (lh - cw) / 2, sw, lh, C_SEL_BG);
			fg = C_SEL_FG;
		}
		if (cfg_row(i) == ROW_BRIGHT)
			snprintf(val, sizeof(val), "< %d >", cfg_bright);
		else if (cfg_row(i) == ROW_TFT)
			snprintf(val, sizeof(val), "< %s >", tfts[tft_index()].label);
		else if (i == 1)
			snprintf(val, sizeof(val), cfg_min ? "< %d s >" : "< off >", cfg_min);
		else if (cfg_wait <= 0)
			snprintf(val, sizeof(val), "< hold MENU >");
		else
			snprintf(val, sizeof(val), "< %d s >", cfg_wait);
		fb_text(mx + 12, y, i == 0 ? "Menu wait" : i == 1 ? "Min. time" :
			cfg_row(i) == ROW_TFT ? "Front TFT" : "Brightness", fg, f, -1);
		fb_text(mx + 12 + 12 * cw, y, val, fg, f, -1);
	}
	fb_text(mx + 12, y0 + NCFG * lh * 3 / 2 + K(10), "Min. time: boot never starts earlier (USB disks settle).", C_DIM, sf, -1);
	fb_text(mx + 12, y0 + NCFG * lh * 3 / 2 + K(10) + scw + 4, "Changes apply at the next boot.", C_DIM, sf, -1);
	bar = my + sh - foot_h;
	fb_rect(0, bar, fb.w, fb.h - bar, C_PANEL);
	fb_text(mx + 12, bar + (fb.h - bar - scw) / 2, "LEFT/RIGHT change  OK save  EXIT back", C_FOOT, sf, -1);
	fb_flip();
}

/* blue key: edit wait (and tft), OK writes STARTUP_CONFIG */
static void config_menu(const char *root)
{
	char old_tft[16];
	int sel = 0, old_wait = cfg_wait, old_min = cfg_min, old_bright = cfg_bright, code, value;

	snprintf(old_tft, sizeof(old_tft), "%s", cfg_tft);
	for (;;) {
		draw_config(sel);
		if (!rc_event(-1, &code, &value) || value != 1)
			continue;
		if (code == KEY_UP || code == KEY_DOWN) {
			sel = (sel + (code == KEY_DOWN ? 1 : NCFG - 1)) % NCFG;
		} else if (code == KEY_LEFT || code == KEY_RIGHT) {
			int d = code == KEY_RIGHT ? 1 : -1;

			if (sel == 1)
				cfg_min = mins[(min_index() + d + NMINS) % NMINS];
			else if (cfg_row(sel) == ROW_BRIGHT) {
				cfg_bright = (cfg_bright - 1 + d + 4) % 4 + 1;
				bright_apply(cfg_bright);   /* visible at once */
			} else if (cfg_row(sel) == ROW_TFT) {
				int t = tft_index();

				do      /* vu_tft cannot mirror the TV */
					t = (t + d + NTFTS) % NTFTS;
				while (has_vutft() && !strcmp(tfts[t].name, "mirror"));
				snprintf(cfg_tft, sizeof(cfg_tft), "%s", tfts[t].name);
			} else
				cfg_wait = waits[(wait_index() + d + NWAITS) % NWAITS];
		} else if (code == KEY_OK || code == KEY_ENTER) {
			message(config_save(root) < 0 ? "Cannot write STARTUP_CONFIG!" : "Saved", C_TITLE);
			sleep(1);
			return;
		} else if (code == KEY_EXIT || code == KEY_BLUE || code == KEY_MENU) {
			snprintf(cfg_tft, sizeof(cfg_tft), "%s", old_tft);
			cfg_wait = old_wait;
			cfg_min = old_min;
			if (cfg_bright != old_bright) {
				cfg_bright = old_bright;
				if (has_bright())
					bright_apply(cfg_bright);
			}
			return;
		}
	}
}

static void usage(void)
{
	fputs("usage: bootmenu [--root DIR] [--wait S] [--settle S] [--timeout S] [--margin P]\n"
	      "                [--margin-bottom P] [--font F] [--small-font F] [--fb-module NAME|PATH]\n"
	      "                [--min-time S] [--dev-timeout S] [--video-settle S]\n"
	      "                [--dry-run] [--list] [--debug]\n"
	      "  <root>/STARTUP_CONFIG: wait=0|N  min=N  tft=mirror|own|off  (--wait/--min-time override)\n",
	      stderr);
	exit(1);
}

/* after the menu (or without it): the image that boots stays on the front displays */
static void announce_boot(void)
{
	char msg[256];
	int i, tft4k = has_tft() && strcmp(cfg_tft, "off");

	if (opt_list || (!tft4k && access("/dev/vu_vfd", W_OK) && access("/dev/vu_tft", W_OK)))
		return;
	load_entries(opt_root);          /* STARTUP may have just changed */
	for (i = 0; i < nentries; i++)
		if (entries[i].current)
			break;
	if (i == nentries)
		return;
	dbg("front display: %s %s\n", entries[i].label, entries[i].name);
	if (tft4k && !fb.mem && fb_open() < 0)  /* menu not opened: vu_fb not loaded yet */
		tft4k = 0;
	tft_show(entries[i].label, entries[i].name);     /* vu_tft, or 4K tft=own (fb1) */
	vfd_show(*entries[i].name ? entries[i].name : entries[i].label,
		 *entries[i].name ? entries[i].label : "");
	if (!tft4k)
		return;
	/* 4K TFT: tft=mirror copies the TV, so the TV shows it too */
	if (!strcmp(cfg_tft, "mirror")) {
		snprintf(msg, sizeof(msg), "Booting\n%s%s%s", entries[i].label,
			 *entries[i].name ? "\n" : "", entries[i].name);
		message(msg, C_TITLE);
	}
	/* vu_fb: keep the picture when init unloads it before kexec */
	i = open("/sys/module/vu_fb/parameters/keep", O_WRONLY);
	if (i >= 0) {
		if (write(i, "1", 1) < 0)
			dbg("vu_fb keep: %s\n", strerror(errno));
		close(i);
	}
}

static int menu_main(int argc, char **argv);

int main(int argc, char **argv)
{
	int r = menu_main(argc, argv);

	if (r == 0 || r == 10)
		announce_boot();
	return r;
}

static int menu_main(int argc, char **argv)
{
	int i, sel = 0, page = 1;
	double deadline = 0;
	char path[PATH_MAX], broot[MAX_STR];
	int auto_cur = 0, last_sel = -1, last_drawn = -3;
	double next_retry = 0;       /* menu opened because the disk is missing: boot when it appears */

	t_start = now();
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;

#define OPT(name) (!strcmp(a, name) && v && (i++, 1))
		if (OPT("--root")) opt_root = v;
		else if (OPT("--wait")) opt_wait = atof(v), opt_wait_set = 1;
		else if (OPT("--fb-module")) opt_fbmod = v;
		else if (OPT("--settle")) opt_settle = atof(v);
		else if (OPT("--timeout")) opt_timeout = atoi(v);
		else if (OPT("--min-time")) opt_min = atof(v);
		else if (OPT("--dev-timeout")) opt_devto = atof(v);
		else if (OPT("--video-settle")) opt_settle_video = atof(v);
		else if (OPT("--margin")) opt_margin = atoi(v);
		else if (OPT("--margin-bottom")) opt_margin_bottom = atoi(v);
		else if (OPT("--font")) opt_font = atof(v);
		else if (OPT("--small-font")) opt_small = atof(v);
		else if (!strcmp(a, "--dry-run")) opt_dry = 1;
		else if (!strcmp(a, "--list")) opt_list = 1;
		else if (!strcmp(a, "--debug")) opt_debug = 1;
		else usage();
#undef OPT
	}

	config_load(opt_root);
	if (has_vutft() && !strcmp(cfg_tft, "mirror"))   /* vu_tft: no copy of the TV */
		snprintf(cfg_tft, sizeof(cfg_tft), "own");
	if (has_bright())
		bright_apply(cfg_bright);
	{	/* no console blanking: after 10 min it blanked HDMI and TFT until a key */
		int bfd = open("/sys/module/kernel/parameters/consoleblank", O_WRONLY);

		if (bfd >= 0) {
			if (write(bfd, "0", 1) < 0)
				dbg("consoleblank: %s\n", strerror(errno));
			close(bfd);
		}
	}
	if (!opt_wait_set)
		opt_wait = cfg_wait;
	if (opt_min < 0)
		opt_min = cfg_min;

	if (opt_list) {                         /* diagnostics only */
		load_entries(opt_root);
		for (i = 0; i < nentries; i++)
			printf("%-9s %-1s %-32s %s\n", entries[i].label, entries[i].current ? "*" : "",
			       *entries[i].name ? entries[i].name : "-", entries[i].status);
		return 0;
	}

	boot_root(opt_root, broot, sizeof(broot));
	dbg("boot slot root=%s", broot);
#ifndef OFFSCREEN
	if (rc_open() < 0) {
		fprintf(stderr, "bootmenu: remote control not found (insmod vu_ir.ko?)\n");
		return 1;
	}
	/* 1. is MENU held? with --wait 0 check instantly, without touching the video.
	 * Then wait for --min-time and for the disk of the slot to boot */
	if (opt_wait <= 0) {
		int menu = rc_is_pressed(KEY_MENU, opt_settle);

		while (!menu) {
			int ok = dev_present(broot);

			if (ok) {
				min_time();
				return 0;
			}
			if (now() >= t_start + opt_devto) {
				auto_cur = 1;
				break;
			}
			if (rc_is_pressed(KEY_MENU, 0.5))
				menu = 1;
			dev_rescan();
		}
		if (fb_open() < 0)
			return 1;
	} else {
		double end, next_scan = 0;
		int shown = -1, pressed = 0, code, value, ok = dev_present(broot);

		if (fb_open() < 0)
			return 1;
		/* the wait counts from when the hint can be seen: loading vu_fb (and,
		 * on boxes without a bootloader display, its own display setup) takes
		 * seconds that would otherwise be eaten from wait= */
		{       /* box without a bootloader display: let the TV lock first */
			char v[8] = "";
			int fd = open("/sys/module/vu_fb/parameters/own_display", O_RDONLY);

			if (fd >= 0) {
				if (read(fd, v, sizeof(v) - 1) < 0)
					v[0] = 0;
				close(fd);
			}
			if (v[0] == '1' && opt_settle_video > 0) {
				dbg("display set up by vu_fb: %.1f s for the TV to lock\n", opt_settle_video);
				usleep((useconds_t)(opt_settle_video * 1e6));
			}
		}
		end = now() + opt_wait;
		if (end < t_start + opt_min)
			end = t_start + opt_min;
		for (;;) {
			double t = now(), stop;
			int n;

			if (t >= next_scan && !ok) {
				dev_rescan();
				ok = dev_present(broot);
				next_scan = t + 0.5;
			}
			if (ok && t >= end)
				break;
			if (!ok && t >= t_start + opt_devto) {
				auto_cur = 1;
				break;
			}
			stop = ok || t < end ? end : t_start + opt_devto;
			n = (int)(stop - t + 0.999);
			if (n != shown * (ok || t < end ? 1 : -1)) {
				char fl[24], fs[16];

				hint(n, ok || t < end ? "Press MENU for multiboot" : "Waiting for disk, MENU for multiboot");
				/* front displays too: one line for the 16-char VFDs */
				snprintf(fl, sizeof(fl), "%s %d", ok || t < end ? "MENU" : "DISK", n);
				snprintf(fs, sizeof(fs), "%d s", n);
				vfd_show(fl, "");
				tft_show(ok || t < end ? "Press MENU" : "Waiting for disk", fs);
				shown = n * (ok || t < end ? 1 : -1);
			}
			if (rc_event(0.25, &code, &value) && code == KEY_MENU && (value == 1 || value == 2)) {
				pressed = 1;
				break;
			}
		}
		if (!pressed && !auto_cur) {
			fb_clear();
			return 0;
		}
	}
#else
	fb_open();
#endif

	/* 2. menu */
	message("Reading slots...", C_TITLE);
#ifndef OFFSCREEN
	rc_wait_release(KEY_MENU, 3.0);         /* MENU held down must not close the menu */
#endif
	dev_rescan();
	load_entries(opt_root);
	if (!nentries) {
		snprintf(path, sizeof(path), "No STARTUP_* in %s", opt_root);
		message(path, 0xffff4040);
		sleep(3);
		fb_clear();
		return 1;
	}
	for (i = 0; i < nentries; i++)
		if (entries[i].current) {
			sel = i;
			break;
		}
	if (opt_timeout > 0 && !auto_cur)
		deadline = now() + opt_timeout;

	for (;;) {
		int remaining = -1, code, value;

		if (now() >= next_retry && now() < t_start + 30) {  /* USB disk still spinning up: retry failed slots for 30 s */
			next_retry = now() + 2.0;
			for (i = 0; i < nentries; i++)
				if (!strncmp(entries[i].status, "mount failed", 12) ||
				    !strcmp(entries[i].status, "disk missing"))
					break;
			if (i < nentries) {
				nblk = -1;
				load_entries(opt_root);
				last_drawn = -3;
			}
		}
		if (need_scan(broot) && dev_rescan()) {             /* a disk came or went: refresh the slots */
			last_drawn = -3;
			load_entries(opt_root);
			if (sel >= nentries)
				sel = nentries ? nentries - 1 : 0;
		}
		if (!nentries) {                /* STARTUP_* gone: boot on, never stop here */
			fb_clear();
			return 0;
		}
		if (auto_cur) {
			if (dev_present(broot)) {
				message("Disk found\nBooting current slot", C_TITLE);
				min_time();
				fb_clear();
				return 0;
			}
			remaining = -2;
		}
		if (deadline > 0) {
			remaining = (int)(deadline - now() + 0.999);
			if (remaining < 0)
				remaining = 0;
		}
		if (sel != last_sel || remaining != last_drawn) {  /* redraw only on changes */
			page = draw(sel, remaining);
			tft_show(entries[sel].label, entries[sel].name);
			vfd_show(*entries[sel].name ? entries[sel].name : entries[sel].label,
				 *entries[sel].name ? entries[sel].label : "");
			last_sel = sel;
			last_drawn = remaining;
		}
#ifdef OFFSCREEN
		return 0;
#endif
		if (remaining == 0) {
			if (!dev_present(broot)) {      /* timeout but no disk: keep the menu */
				deadline = 0;
				auto_cur = 1;
				continue;
			}
			min_time();
			fb_clear();
			return 0;
		}
		if (!rc_event(0.5, &code, &value) || value == 0)
			continue;
		if (value == 2 && code != KEY_UP && code != KEY_DOWN && code != KEY_LEFT && code != KEY_RIGHT)
			continue;                       /* repeats only for scrolling */
		deadline = 0;
		auto_cur = 0;                   /* the user is choosing */
		if (code == KEY_UP) {
			sel = (sel - 1 + nentries) % nentries;
		} else if (code == KEY_DOWN) {
			sel = (sel + 1) % nentries;
		} else if (code == KEY_LEFT) {          /* previous page */
			sel = sel - page < 0 ? 0 : sel - page;
		} else if (code == KEY_RIGHT) {         /* next page */
			sel = sel + page >= nentries ? nentries - 1 : sel + page;
		} else if (code == KEY_OK || code == KEY_ENTER ||
			   (code >= KEY_1 && code <= KEY_9 && code - KEY_1 < nentries)) {
			struct entry *e;
			char msg[256];

			if (code >= KEY_1 && code <= KEY_9)
				sel = code - KEY_1;
			e = &entries[sel];
			if (!dev_present(e->root)) {    /* wait for its disk, EXIT cancels */
				int cancel = 0;

				snprintf(msg, sizeof(msg), "Waiting for disk\n%s\nEXIT cancel", e->label);
				message(msg, C_TITLE);
				while (!dev_present(e->root)) {
					if (rc_event(0.5, &code, &value) && value == 1 &&
					    (code == KEY_EXIT || code == KEY_MENU)) {
						cancel = 1;
						break;
					}
					dev_rescan();
				}
				if (cancel) {
					last_sel = -1;
					continue;
				}
				dev_rescan();
				load_entries(opt_root);
				if (sel >= nentries) {  /* the list changed meanwhile */
					last_sel = -1;
					sel = 0;
					continue;
				}
				e = &entries[sel];
			}
			snprintf(msg, sizeof(msg), "Booting\n%s%s%s", e->label, *e->name ? "\n" : "", e->name);
			message(msg, C_TITLE);
			snprintf(path, sizeof(path), "%s/STARTUP_ONCE", opt_root);
			if (e->current && !exists(path)) {
				sleep(1);
				min_time();
				fb_clear();
				return 0;
			}
			if (select_entry(opt_root, e) < 0) {
				message("Cannot write STARTUP!", 0xffff4040);
				sleep(3);
				fb_clear();
				return 1;
			}
			sleep(1);
			min_time();
			fb_clear();
			return 10;
		} else if (code == KEY_BLUE) {
			config_menu(opt_root);
			last_sel = -1;
		} else if (code == KEY_EXIT || code == KEY_MENU) {
			if (!dev_present(broot)) {      /* nothing to boot yet: stay */
				auto_cur = 1;
				continue;
			}
			min_time();
			fb_clear();
			return 0;
		}
	}
}
