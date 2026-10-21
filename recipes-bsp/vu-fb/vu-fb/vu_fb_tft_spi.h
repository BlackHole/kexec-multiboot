/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VU_FB_TFT_SPI_H
#define VU_FB_TFT_SPI_H
#include <linux/fb.h>

/* front TFT on a bit-banged SPI bus (solo4k) */
int  vu_tft_spi_start(const char *mode, struct fb_info *src);
void vu_tft_spi_stop(void);

#endif
