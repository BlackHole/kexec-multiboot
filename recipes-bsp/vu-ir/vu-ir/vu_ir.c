// SPDX-License-Identifier: GPL-2.0
/*
 * vu_ir.c - remote control / front panel keys for Vu+ boxes without the
 *           proprietary drivers: 4K (BOLT) and MIPS gen1 / gen2 (CFE):
 *           vuultimo4k, vusolo4k, vuuno4k(se), vuzero4k, vuduo4k(se),
 *           vuzero, vusolose, vusolo2, vuduo2, vusolo, vuuno, vuduo, vuultimo.
 *
 * On Vu+ 4K boxes the IR receiver and front keys are handled by the
 * front-panel microcontroller, which sends packets to the SoC over UART B
 * (serial@f040ab40, ttyS1), 19200 8N1 (81 MHz / (16 * 263)).
 * Found by reversing the original remote module (UART setup: DLL=7 DLM=1)
 * and verified with hexdump on vuultimo4k.
 *
 * Packet (8 bytes):
 *   [0] 0xd2 = remote, 0xd1 = front panel
 *   [1] key code (RC6 command for the remote)
 *   [2] 0x80  [3] 0x52 (system/address)
 *   [4] 0x90 / 0x10 : bit 7 = RC6 toggle, flips on every new key press
 *   [5] 0x02 (gen1 BCM7325: 0xff)
 *   [6] XOR of bytes 0..5 (gen1: sum of bytes 0..5; the gen1 module checks
 *       only d2 / 80 52 / 0d)
 * The MIPS modules (gen1 dvb-bcm7325/7335/7413, gen2 dvb-bcm73625/7241/7356/7424)
 * use the same key codes, except 0x2d/0x30 = KEY_PAUSE (4K: KEY_PLAYPAUSE)
 * and no 0xe6.
 *   [7] 0x0d
 *
 * The UART is driven directly (like the original module). Do not open
 * the matching /dev/ttyS* while this module is loaded.
 *
 * MIPS boxes (CFE, kernel without device tree): same MCU and packets, UART
 * clock 81 MHz (divisor 263) everywhere:
 *   gen2: vuzero (73625) 0x10406880 irq 64, vusolose (7429) 0x10406780 irq 54,
 *         vusolo2 (7346) 0x10406940 irq 66, vuduo2 (7425) 0x10406b40 irq 63
 *   gen1: vusolo (7325), vuduo (7335), vuuno / vuultimo (7413): 0x10400b40 irq 66
 * vuuno and vuultimo share the chip: the build defines VU_UNO / VU_ULTIMO. The chip is recognised by its ID register. The CFE leaves UART 2
 * RX also on another pin (vuzero gpio_92, vusolose gpio_097), so the pinmux
 * is moved to aon_gpio_07/09 (RX/TX), as the original module does.
 *
 * vu_mcu_send() (vu_mcu.h) sends commands to the same MCU, e.g. for the
 * front VFD of vuuno, vuduo, vusolo2 (module vu_vfd); vu_mcu_vfd_chars() tells how many
 * characters the box's VFD has (0 = none).
 *
 * Parameters:  debug=1  log every packet
 *              uart_phys=, uart_divisor=, irq=  override the per-SoC values
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/of_irq.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/serial_reg.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include "vu_mcu.h"

/* kernels without device tree (3.x MIPS, CONFIG_OF off) have no of_find_node_by_path() */
#ifdef CONFIG_OF
#define vu_dt_find(path) of_find_node_by_path(path)
#else
static inline struct device_node *vu_dt_find(const char *path)
{
	return NULL;
}
#endif

#define DRV_NAME "vu_ir"

#define UART_SIZE       0x20
#define UART_REGSHIFT   2

/*
 * The front-panel MCU sits on UART B, whose address differs per SoC
 * (the serial nodes in /proc/iomem): 7445 0x40ab40, 7366 0x40b040.
 */
struct vu_pinmux {
	ulong reg;               /* physical address */
	u32 mask, val;           /* fields to set */
};

struct vu_ir_soc {
	const char *compatible;  /* DT root compatible, NULL = chip_id below */
	ulong uartb;
	int divisor;             /* 0 = 263 (81 MHz UART clock) */
	u32 chip_id;             /* kernels without DT: chip family (e.g. 0x7362, 0x7429) */
	int irq;                 /* kernels without DT: Linux irq of the UART */
	struct vu_pinmux pinmux[2];
	u8 gen1;                 /* keymap of the gen1 module: 0x2d/0x30 = KEY_PAUSE, no 0xe6 */
	u8 vfd;                  /* front VFD on the MCU: number of characters (0 = none, see vu_vfd) */
	u8 front;                /* front keys: 0 = 4K map, FRONT_DUO, FRONT_ULTIMO (gen1) */
};

#define FRONT_DUO    1          /* dvb-bcm7335: 0x04 OK, 0x0e DOWN, 0x0f UP */
#define FRONT_ULTIMO 2          /* dvb-bcm7413 (ultimo): 01 POWER, 02 RIGHT, 04 LEFT, 08 VOL-, 80 VOL+ */

/* chip family ID register of the MIPS chips (registers at 0x10000000) */
#define MIPS_CHIP_ID_REG  0x10404000UL

static const struct vu_ir_soc vu_ir_socs[] = {
	{ "brcm,bcm7445", 0xF040AB40UL },	/* ultimo4k */
	{ "brcm,bcm7366", 0xF040B040UL },	/* solo4k */
	{ "brcm,bcm7439", 0xF040A980UL },	/* uno4kse: UART C (ttyS2), confirmed with cat /dev/ttyS2 */
	{ "brcm,bcm7278", 0x0840E000UL },	/* duo4k: ttyS2, registers at 0x08000000, 81 MHz UART clock */
	{ "brcm,bcm7260", 0xF040D000UL },	/* zero4k: ttyS1 (Vu+ driver: DLL=7 DLM=1 = 263), tested */
	{ NULL, 0x10406880UL, 0, 0x7362, 64, {	/* vuzero: UART C (ttyS2), tested */
		{ 0x1040412cUL, 0xff000000, 0x00000000 },	/* gpio_91/92: off UART 2 */
		{ 0x10408504UL, 0x0000f0f0, 0x00004040 } }, 1 },	/* aon_gpio_07/09 = UART 2 RX/TX; gen1 keymap (dvb-bcm73625) */
	{ NULL, 0x10406780UL, 0, 0x7429, 54, {	/* vusolose: UART C (ttyS2), tested */
		{ 0x10404130UL, 0x000000ff, 0x00000000 },	/* gpio_096/097: off UART 2 */
		{ 0x10408504UL, 0x0000f0f0, 0x00004040 } }, 1 },	/* aon_gpio_07/09 = UART 2 RX/TX; gen1 keymap (dvb-bcm7241) */
	{ NULL, 0x10406b40UL, 0, 0x7425, 63, { { 0 } }, 1 },	/* vuduo2 (BCM7424, 7425 family): from dvb-bcm7424,
						 * divisor 263, gen1 keymap; front LCD on SPI (not the MCU); untested */
	{ NULL, 0x10406940UL, 0, 0x7346, 66, { { 0 } }, 1, 16 },	/* vusolo2 (BCM7346, module dvb-bcm7356): divisor 263,
						 * gen1 keymap, 16-char MCU VFD (0x21); chip id 0x73460012 read on the box, pinmux untested */
	{ NULL, 0x10400b40UL, 0, 0x7325, 66, { { 0 } }, 1 },	/* vusolo gen1: UART B (ttyS1), no pinmux;
						 * divisor 263: BURT_P_SetBaudRate of dvb-bcm7325 = 5062500 (81 MHz / 16) / baud */
#ifdef VU_ULTIMO
	{ NULL, 0x10400b40UL, 0, 0x7413, 66, { { 0 } }, 1, 0, FRONT_ULTIMO },	/* vuultimo gen1: UART B, divisor 263
						 * (written by dvb-bcm7413 itself), gen1 keymap, LCD on MSPI (vu_vfd) */
#elif defined(VU_UNO)
	{ NULL, 0x10400b40UL, 0, 0x7413, 66, {	/* pinmux: as dvb-bcm7413 (NEXUS pinmux init), else */
		{ 0x1040410cUL, 0x00007e00, 0x00002400 } },	/* bits 14:9 = 2,2 (UART B); CFE 0x0da4da49 (5,5):
						 * no TX to the MCU, VFD dark. dvb-bcm7413 writes 0x00002400 */
	  0, 16 },	/* vuuno gen1: UART B (ttyS1), 16-char VFD;
						 * divisor 263: the vuultimo module (same SoC) writes DLL 7 / DLM 1 */
#endif
	{ NULL, 0x10400b40UL, 0, 0x7335, 66, {	/* pinmux: bits 26:24 of 0x1040412c = 2 (UART B TX), */
		{ 0x1040412cUL, 0x07000000, 0x02000000 } },	/* CFE leaves 0: VFD dark (tested); dvb-bcm7335 sets it */
	  1, 16, FRONT_DUO },	/* vuduo gen1: UART B (ttyS1), 16-char VFD,
						 * gen1 keymap (from dvb-bcm7335, untested) */
};

/* kernels without device tree (MIPS): chip family from the ID register */
static u32 __init mips_chip_id(void)
{
	struct device_node *root = vu_dt_find("/");
	void __iomem *r;
	u32 id = 0;

	if (root) {             /* DT kernel: not a MIPS chip of this table */
		of_node_put(root);
		return 0;
	}
	r = ioremap(MIPS_CHIP_ID_REG, 4);
	if (r) {
		id = readl(r);
		id = (id >> 28) ? id >> 16 : id >> 12;	/* 0x07362500 -> 0x7362, 0x74290010 -> 0x7429 */
		iounmap(r);
	}
	return id;
}

static void __init vu_ir_pinmux(const struct vu_pinmux *p, int n)
{
	int i;

	for (i = 0; i < n && p[i].reg; i++) {
		void __iomem *r = ioremap(p[i].reg, 4);
		u32 v;

		if (!r)
			continue;
		v = readl(r);
		writel((v & ~p[i].mask) | p[i].val, r);
		pr_info(DRV_NAME ": pinmux %08lx: %08x -> %08x\n", p[i].reg, v, readl(r));
		iounmap(r);
	}
}

/* true if a root "compatible" string starts with prefix (brcm,bcm7366 matches brcm,bcm7366c0) */
static bool __init soc_is(const char *prefix)
{
	struct device_node *root = vu_dt_find("/");
	const char *c;
	int i;
	bool ret = false;

	for (i = 0; root && !of_property_read_string_index(root, "compatible", i, &c); i++)
		if (!strncmp(c, prefix, strlen(prefix))) {
			ret = true;
			break;
		}
	of_node_put(root);
	return ret;
}

static ulong uart_phys;
module_param(uart_phys, ulong, 0444);
MODULE_PARM_DESC(uart_phys, "physical address of the front-panel UART (0 = from DT compatible)");
static int uart_divisor;		/* 0 = per SoC (263: 81 MHz / 16 / 263 = 19249 baud) */
module_param(uart_divisor, int, 0444);
MODULE_PARM_DESC(uart_divisor, "UART divisor (0 = per SoC default, 263 for 81 MHz clock)");
static int irq;
module_param(irq, int, 0444);
MODULE_PARM_DESC(irq, "UART irq (0 = from DT, or per SoC on kernels without DT)");

#define PKT_LEN         8
#define PKT_REMOTE      0xd2
#define PKT_FRONT       0xd1
#define PKT_END         0x0d

#define KEY_RELEASE_MS  200
/*
 * The MCU always sends every frame at least twice (~107 ms apart), and keeps
 * sending while the key is held. Ignore the first REPEAT_SKIP duplicates so a
 * short press gives exactly one key event; after that, each frame is a repeat.
 */
#define REPEAT_SKIP     2

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "log every packet");

/* remote: RC6 command -> key, = keymap1[] of the original Vu+ module (48 entries) */
static const struct { u8 code; unsigned short key; } remote_map[] = {
	{0x0d, KEY_MUTE},
	{0x0c, KEY_POWER},
	{0x3c, KEY_TEXT},
	{0x4b, KEY_SUBTITLE},
	{0xb9, KEY_HOME},
	{0xba, KEY_END},
	{0x00, KEY_0},
	{0x01, KEY_1},
	{0x02, KEY_2},
	{0x03, KEY_3},
	{0x04, KEY_4},
	{0x05, KEY_5},
	{0x06, KEY_6},
	{0x07, KEY_7},
	{0x08, KEY_8},
	{0x09, KEY_9},
	{0xbb, KEY_PREVIOUS},
	{0xbc, KEY_NEXT},
	{0x6d, KEY_RED},
	{0x6e, KEY_GREEN},
	{0x6f, KEY_YELLOW},
	{0x70, KEY_BLUE},
	{0x58, KEY_UP},
	{0x59, KEY_DOWN},
	{0x5a, KEY_LEFT},
	{0x5b, KEY_RIGHT},
	{0x5c, KEY_OK},
	{0x10, KEY_VOLUMEUP},
	{0x11, KEY_VOLUMEDOWN},
	{0x55, KEY_EXIT},
	{0xcc, KEY_INFO},
	{0x20, KEY_CHANNELUP},
	{0x21, KEY_CHANNELDOWN},
	{0x54, KEY_MENU},
	{0x49, KEY_VIDEO},
	{0xe5, KEY_AUDIO},
	{0x81, KEY_HELP},
	{0x29, KEY_REWIND},
	{0x2c, KEY_PLAY},
	{0x2d, KEY_PLAYPAUSE},
	{0x30, KEY_PLAYPAUSE},
	{0x28, KEY_FASTFORWARD},
	{0xe4, KEY_TV},
	{0x37, KEY_RECORD},
	{0x31, KEY_STOP},
	{0xf2, KEY_RADIO},
	{0xc4, 0x180 /* KEY_TAPE */},
	{0xe6, KEY_EDIT},
};

/* front panel: bitmask -> key (from the old bcm_remote.c, to be verified) */
static const struct { u8 code; unsigned short key; } front_map[] = {
	{0x01, KEY_POWER}, {0x02, KEY_VOLUMEUP}, {0x04, KEY_VOLUMEDOWN},
	{0x08, KEY_CHANNELUP}, {0x80, KEY_CHANNELDOWN},
};

struct vu_ir {
	void __iomem *base;
	int irq;
	struct input_dev *input;
	struct timer_list timer;
	spinlock_t lock;
	u8 pkt[PKT_LEN];
	int pos;
	unsigned short remote_keys[256];
	unsigned short front_keys[256];
	unsigned short cur_key;
	u8 cur_type, cur_code, cur_toggle;
	int dup;
	int vfd_len;             /* characters of the front VFD (vu_vfd), 0 = none */
	struct mutex tx_lock;
};

static struct vu_ir *g_ir;


static inline u32 uart_in(struct vu_ir *ir, int reg)
{
	return readl(ir->base + (reg << UART_REGSHIFT)) & 0xff;
}

static inline void uart_out(struct vu_ir *ir, int reg, u32 v)
{
	writel(v, ir->base + (reg << UART_REGSHIFT));
}

/* called with ir->lock held */
static void vu_ir_release(struct vu_ir *ir)
{
	if (ir->cur_key != KEY_RESERVED) {
		input_report_key(ir->input, ir->cur_key, 0);
		input_sync(ir->input);
		ir->cur_key = KEY_RESERVED;
	}
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
static void vu_ir_timer(struct timer_list *t)
{
	struct vu_ir *ir = from_timer(ir, t, timer);
#else
static void vu_ir_timer(unsigned long data)
{
	struct vu_ir *ir = (struct vu_ir *)data;
#endif
	unsigned long flags;

	spin_lock_irqsave(&ir->lock, flags);
	vu_ir_release(ir);
	spin_unlock_irqrestore(&ir->lock, flags);
}

static void vu_ir_packet(struct vu_ir *ir)
{
	u8 *p = ir->pkt, x = 0, sum = 0, type = p[0], code = p[1], toggle = p[4] & 0x80;
	unsigned short key;
	int i;

	for (i = 0; i < 6; i++) {
		x ^= p[i];
		sum += p[i];
	}
	if (debug)
		pr_info(DRV_NAME ": %*ph%s\n", PKT_LEN, p,
			x == p[6] || sum == p[6] ? "" : " (bad checksum)");
	if (x != p[6] && sum != p[6])   /* 4K/gen2: XOR, gen1: sum */
		return;

	key = (type == PKT_REMOTE) ? ir->remote_keys[code] : ir->front_keys[code];
	input_event(ir->input, EV_MSC, MSC_SCAN, (type << 8) | code);

	if (key == KEY_RESERVED) {
		pr_info(DRV_NAME ": unmapped %s code 0x%02x\n",
			type == PKT_REMOTE ? "remote" : "front", code);
		input_sync(ir->input);
		return;
	}

	if (key == ir->cur_key && type == ir->cur_type && code == ir->cur_code &&
	    toggle == ir->cur_toggle) {
		/* same press: duplicated frame or key held down */
		if (++ir->dup > REPEAT_SKIP)
			input_event(ir->input, EV_KEY, key, 2);
	} else {
		vu_ir_release(ir);
		input_report_key(ir->input, key, 1);
		ir->cur_key = key;
		ir->cur_type = type;
		ir->cur_code = code;
		ir->cur_toggle = toggle;
		ir->dup = 0;
	}
	input_sync(ir->input);
	mod_timer(&ir->timer, jiffies + msecs_to_jiffies(KEY_RELEASE_MS));
}

static void vu_ir_rx_byte(struct vu_ir *ir, u8 c)
{
	if (ir->pos == 0 && c != PKT_REMOTE && c != PKT_FRONT)
		return;                         /* resync on start byte */
	ir->pkt[ir->pos++] = c;
	if (ir->pos < PKT_LEN)
		return;
	ir->pos = 0;
	if (ir->pkt[PKT_LEN - 1] == PKT_END)
		vu_ir_packet(ir);
	else if (debug)
		pr_info(DRV_NAME ": framing error %*ph\n", PKT_LEN, ir->pkt);
}

static irqreturn_t vu_ir_isr(int irq, void *dev_id)
{
	struct vu_ir *ir = dev_id;
	int handled = 0, n = 0;
	u32 iir, lsr;

	spin_lock(&ir->lock);
	while (!((iir = uart_in(ir, UART_IIR)) & UART_IIR_NO_INT) && n++ < 64) {
		handled = 1;
		lsr = uart_in(ir, UART_LSR);
		while (lsr & UART_LSR_DR) {
			vu_ir_rx_byte(ir, uart_in(ir, UART_RX));
			lsr = uart_in(ir, UART_LSR);
		}
		if ((iir & UART_IIR_ID) == UART_IIR_MSI)
			uart_in(ir, UART_MSR);
	}
	spin_unlock(&ir->lock);
	return IRQ_RETVAL(handled);
}

/* send bytes to the MCU (polled; the original module waits ~50 ms for its ack) */
static void vu_ir_send(struct vu_ir *ir, const u8 *buf, int len)
{
	unsigned long flags;
	int i, t;

	for (i = 0; i < len; i++) {
		for (t = 0; t < 1000 && !(uart_in(ir, UART_LSR) & UART_LSR_THRE); t++)
			udelay(10);
		spin_lock_irqsave(&ir->lock, flags);
		uart_out(ir, UART_TX, buf[i]);
		spin_unlock_irqrestore(&ir->lock, flags);
	}
	msleep(50);
}

/* for other modules (vu_vfd): send a command to the front-panel MCU */
int vu_mcu_send(const u8 *buf, int len)
{
	struct vu_ir *ir = g_ir;

	if (!ir)
		return -ENODEV;
	mutex_lock(&ir->tx_lock);
	vu_ir_send(ir, buf, len);
	mutex_unlock(&ir->tx_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(vu_mcu_send);

int vu_mcu_vfd_chars(void)
{
	return g_ir ? g_ir->vfd_len : 0;
}
EXPORT_SYMBOL_GPL(vu_mcu_vfd_chars);

static void vu_ir_uart_init(struct vu_ir *ir)
{
	uart_out(ir, UART_IER, 0);
	uart_out(ir, UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
	uart_out(ir, UART_FCR, UART_FCR_ENABLE_FIFO);
	uart_out(ir, UART_LCR, UART_LCR_DLAB | UART_LCR_WLEN8);
	uart_out(ir, UART_DLL, uart_divisor & 0xff);
	uart_out(ir, UART_DLM, uart_divisor >> 8);
	uart_out(ir, UART_LCR, UART_LCR_WLEN8);         /* 8N1 */
	uart_out(ir, UART_MCR, 0);
	uart_in(ir, UART_LSR);
	uart_in(ir, UART_RX);
	uart_in(ir, UART_IIR);
	uart_in(ir, UART_MSR);
}

static int __init vu_ir_init(void)
{
	struct device_node *np;
	struct vu_ir *ir;
	char node[32];
	const struct vu_ir_soc *soc = NULL;
	u32 chip = mips_chip_id();
	int i, ret;

	ir = kzalloc(sizeof(*ir), GFP_KERNEL);
	if (!ir)
		return -ENOMEM;
	spin_lock_init(&ir->lock);
	mutex_init(&ir->tx_lock);
	ir->cur_key = KEY_RESERVED;

	for (i = 0; !soc && i < ARRAY_SIZE(vu_ir_socs); i++)
		if (vu_ir_socs[i].compatible ? soc_is(vu_ir_socs[i].compatible) :
		    (chip && chip == vu_ir_socs[i].chip_id))
			soc = &vu_ir_socs[i];
	if (soc) {
		if (!uart_phys)
			uart_phys = soc->uartb;
		if (!uart_divisor)
			uart_divisor = soc->divisor;
		if (!irq)
			irq = soc->irq;
		vu_ir_pinmux(soc->pinmux, ARRAY_SIZE(soc->pinmux));
	}
	if (!uart_phys) {
		pr_err(DRV_NAME ": unknown SoC, pass uart_phys= (see the serial nodes in /proc/iomem)\n");
		ret = -ENODEV;
		goto err_free;
	}
	snprintf(node, sizeof(node), "/rdb/serial@%lx", uart_phys);
	np = vu_dt_find(node);
	if (np) {
		ir->irq = irq_of_parse_and_map(np, 0);
		of_node_put(np);
	}
	if (irq > 0)
		ir->irq = irq;
	if (!uart_divisor)
		uart_divisor = 263;
	if (ir->irq <= 0) {
		pr_err(DRV_NAME ": no irq for %s (pass irq=)\n", node);
		ret = -ENODEV;
		goto err_free;
	}

	ir->base = ioremap(uart_phys, UART_SIZE);
	if (!ir->base) {
		ret = -ENOMEM;
		goto err_free;
	}

	ir->input = input_allocate_device();
	if (!ir->input) {
		ret = -ENOMEM;
		goto err_unmap;
	}
	for (i = 0; i < ARRAY_SIZE(remote_map); i++)
		ir->remote_keys[remote_map[i].code] = remote_map[i].key;
	for (i = 0; i < ARRAY_SIZE(front_map); i++)
		ir->front_keys[front_map[i].code] = front_map[i].key;
	if (soc && soc->gen1) {         /* keymap1 of the gen1 module (dvb-bcm7325) */
		ir->remote_keys[0x2d] = KEY_PAUSE;
		ir->remote_keys[0x30] = KEY_PAUSE;
		ir->remote_keys[0xe6] = KEY_RESERVED;
	}
	if (soc && soc->front == FRONT_DUO) {	/* front keys of dvb-bcm7335 */
		memset(ir->front_keys, 0, sizeof(ir->front_keys));
		ir->front_keys[0x04] = KEY_OK;
		ir->front_keys[0x0e] = KEY_DOWN;
		ir->front_keys[0x0f] = KEY_UP;
	} else if (soc && soc->front == FRONT_ULTIMO) {	/* front keys of dvb-bcm7413 (ultimo) */
		memset(ir->front_keys, 0, sizeof(ir->front_keys));
		ir->front_keys[0x01] = KEY_POWER;
		ir->front_keys[0x02] = KEY_RIGHT;
		ir->front_keys[0x04] = KEY_LEFT;
		ir->front_keys[0x08] = KEY_VOLUMEDOWN;
		ir->front_keys[0x80] = KEY_VOLUMEUP;
	}

	/* keep the name the original module used: some images match on it */
	ir->input->name = "dreambox advanced remote control (native)";
	ir->input->phys = DRV_NAME "/input0";
	ir->input->id.bustype = BUS_HOST;
	ir->input->keycode = ir->remote_keys;
	ir->input->keycodesize = sizeof(ir->remote_keys[0]);
	ir->input->keycodemax = ARRAY_SIZE(ir->remote_keys);
	__set_bit(EV_KEY, ir->input->evbit);
	/* no EV_REP: repeats come from the MCU frames, not from the input core */
	input_set_capability(ir->input, EV_MSC, MSC_SCAN);
	for (i = 0; i < 256; i++) {
		__set_bit(ir->remote_keys[i], ir->input->keybit);
		__set_bit(ir->front_keys[i], ir->input->keybit);
	}
	__clear_bit(KEY_RESERVED, ir->input->keybit);

	ret = input_register_device(ir->input);
	if (ret)
		goto err_input_free;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
	timer_setup(&ir->timer, vu_ir_timer, 0);
#else
	setup_timer(&ir->timer, vu_ir_timer, (unsigned long)ir);
#endif

	vu_ir_uart_init(ir);
	ret = request_irq(ir->irq, vu_ir_isr, 0, DRV_NAME, ir);
	if (ret) {
		pr_err(DRV_NAME ": request_irq %d failed: %d (is the /dev/ttyS* of this UART open?)\n", ir->irq, ret);
		goto err_input_unreg;
	}
	uart_out(ir, UART_IER, UART_IER_RDI);

	pr_info(DRV_NAME ": front-panel MCU on UART %08lx irq %d, 19200 8N1\n", uart_phys, ir->irq);
	g_ir = ir;

	if (soc)
		ir->vfd_len = soc->vfd;
	return 0;

err_input_unreg:
	del_timer_sync(&ir->timer);
	input_unregister_device(ir->input);
	goto err_unmap;
err_input_free:
	input_free_device(ir->input);
err_unmap:
	iounmap(ir->base);
err_free:
	kfree(ir);
	return ret;
}

static void __exit vu_ir_exit(void)
{
	struct vu_ir *ir = g_ir;

	if (!ir)
		return;
	uart_out(ir, UART_IER, 0);
	free_irq(ir->irq, ir);
	del_timer_sync(&ir->timer);
	input_unregister_device(ir->input);
	iounmap(ir->base);
	kfree(ir);
	g_ir = NULL;
}

module_init(vu_ir_init);
module_exit(vu_ir_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Vu+ remote control / front keys via front-panel MCU UART");
