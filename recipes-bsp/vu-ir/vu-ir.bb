SUMMARY = "Native remote control / front keys for Vu+ 4K and MIPS boxes (front-panel MCU on a UART)"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

COMPATIBLE_MACHINE = "^(vuultimo4k|vuduo4kse|vuduo4k|vusolo4k|vuzero4k|vuuno4kse|vuuno4k|vuzero|vusolose|vusolo2|vuduo2|vusolo|vuuno|vuduo|vuultimo)$"

PV = "1.17"

SRC_URI = "file://vu_ir.c file://vu_mcu.h"

S = "${UNPACKDIR}"

EXTRA_OEMAKE:append = " KBUILD_EXTMOD=${S}"

# vuultimo and vuuno share the SoC (chip id 0x7413): the machine decides
# (without one of the two defines the 0x7413 entry is left out)
IR_CFLAGS = ""
IR_CFLAGS:vuultimo = "-DVU_ULTIMO"
IR_CFLAGS:vuuno = "-DVU_UNO"

do_configure:prepend() {
    echo "obj-m += vu_ir.o" > ${S}/Makefile
    echo "ccflags-y := -std=gnu89 ${IR_CFLAGS}" >> ${S}/Makefile
    echo "all:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) modules" >> ${S}/Makefile
    echo "modules_install:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) modules_install" >> ${S}/Makefile
    echo "clean:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) clean" >> ${S}/Makefile
}

# the .ko goes into kernel-module-<name> (module.bbclass); ${PN} just depends on it

# vu_mcu.h + Module.symvers (installed by module.bbclass) for vu-vfd
do_install:append() {
    install -d ${D}${includedir}/${BPN}
    install -m 0644 ${S}/vu_mcu.h ${D}${includedir}/${BPN}/
}
