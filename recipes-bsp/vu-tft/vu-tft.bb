SUMMARY = "Front colour LCD (TFT, 240x400) of the vuduo2 as a text display"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

COMPATIBLE_MACHINE = "^(vuduo2)$"

PV = "1.2"

SRC_URI = "file://vu_tft.c file://vu_vfd_font.h"

S = "${UNPACKDIR}"

EXTRA_OEMAKE:append = " KBUILD_EXTMOD=${S}"

do_configure:prepend() {
    echo "obj-m += vu_tft.o" > ${S}/Makefile
    echo "ccflags-y := -std=gnu89" >> ${S}/Makefile
    echo "all:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) modules" >> ${S}/Makefile
    echo "modules_install:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) modules_install" >> ${S}/Makefile
    echo "clean:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) clean" >> ${S}/Makefile
}
