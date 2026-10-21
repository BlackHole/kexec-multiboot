SUMMARY = "Native minimal framebuffer for Vu+ 4K and MIPS boxes (reuses the display BOLT/CFE leaves running)"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

COMPATIBLE_MACHINE = "^(vuultimo4k|vuduo4kse|vuduo4k|vusolo4k|vuzero4k|vuuno4kse|vuuno4k|vuzero|vusolose|vusolo2|vuduo2|vusolo|vuuno|vuduo|vuultimo)$"
# vusolo2: needs the CFE splash image in flash (else the CFE starts no display)

PV = "1.66"

SRC_URI = "file://vu_fb_core.c file://vu_fb_tft_spi.c file://vu_fb_tft_spi.h file://vu_fb_modes.h \
           file://vu_fb_modes_7325.h file://vu_fb_modes_7413.h file://vu_fb_modes_7335.h"

S = "${UNPACKDIR}"

EXTRA_OEMAKE:append = " KBUILD_EXTMOD=${S}"

do_configure:prepend() {
    echo "obj-m += vu_fb.o" > ${S}/Makefile
    echo "vu_fb-objs := vu_fb_core.o vu_fb_tft_spi.o" >> ${S}/Makefile
    echo "ccflags-y := -std=gnu89" >> ${S}/Makefile
    echo "all:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) modules" >> ${S}/Makefile
    echo "modules_install:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) modules_install" >> ${S}/Makefile
    echo "clean:" >> ${S}/Makefile
    echo "	make -C \$(KERNEL_SRC) M=\$(PWD) clean" >> ${S}/Makefile
}

# the .ko goes into kernel-module-<name> (module.bbclass); ${PN} just depends on it
