SUMMARY = "Front text display of the MIPS Vu+ boxes: MCU VFD (vuuno, vuduo, vusolo2) or MSPI LCD/OLED (vuultimo, vuduo2)"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

COMPATIBLE_MACHINE = "^(vuuno|vuduo|vuultimo|vusolo2|vuduo2)$"

PV = "1.7"

SRC_URI = "file://vu_vfd.c file://vu_vfd_lcd.c file://vu_vfd_oled.c file://vu_vfd_font.h"

S = "${UNPACKDIR}"

# MCU VFD (vuuno, vuduo): vu_mcu_send() comes from vu_ir (header + Module.symvers).
# vuultimo: the LCD sits on the MSPI, no vu_ir needed.
VFD_CFLAGS = "-I${STAGING_INCDIR}/vu-ir"
VFD_CFLAGS:vuultimo = "-DVU_VFD_LCD"
# vuduo2: 140x32 OLED on the MSPI; vu_ir only for the MCU "display on"
VFD_CFLAGS:vuduo2 = "-I${STAGING_INCDIR}/vu-ir -DVU_VFD_OLED"
DEPENDS += "${@'' if d.getVar('MACHINE') == 'vuultimo' else 'vu-ir'}"
RDEPENDS:${PN} += "${@'' if d.getVar('MACHINE') == 'vuultimo' else 'vu-ir'}"
EXTRA_OEMAKE:append = " KBUILD_EXTMOD=${S}"
EXTRA_OEMAKE:append = "${@'' if d.getVar('MACHINE') == 'vuultimo' else ' KBUILD_EXTRA_SYMBOLS=${STAGING_INCDIR}/vu-ir/Module.symvers'}"

do_configure:prepend() {
    echo "obj-m += vu_vfd.o" > ${S}/Makefile
    echo "ccflags-y := -std=gnu89 ${VFD_CFLAGS}" >> ${S}/Makefile
    echo "all:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) modules" >> ${S}/Makefile
    echo "modules_install:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) modules_install" >> ${S}/Makefile
    echo "clean:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) clean" >> ${S}/Makefile
}
