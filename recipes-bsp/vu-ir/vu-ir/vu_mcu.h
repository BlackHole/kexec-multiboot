/* SPDX-License-Identifier: GPL-2.0 */
/* vu_mcu.h - front-panel MCU access exported by vu_ir */
#ifndef VU_MCU_H
#define VU_MCU_H
#include <linux/types.h>

int vu_mcu_send(const u8 *buf, int len);   /* raw command to the MCU (UART) */
int vu_mcu_vfd_chars(void);                /* characters of the front VFD, 0 = none */

#endif
