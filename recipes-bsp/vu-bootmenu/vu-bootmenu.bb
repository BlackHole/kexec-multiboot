SUMMARY = "Multiboot menu for the Vu+ kexec initrd (framebuffer vu_fb, remote vu_ir, front displays vu_vfd / vu_tft)"
DESCRIPTION = "Selects which STARTUP_x slot to boot: hold MENU on the remote during boot \
to open the menu; the chosen STARTUP_x is copied to STARTUP. \
Static C binary, usable inside the kexec initrd."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

PV = "0.30"

SRC_URI = "file://bootmenu.c"

S = "${UNPACKDIR}"

# static: the initrd has no shared libc to rely on.
# -Os + gc-sections keep it small; stripping happens in do_package
do_compile() {
    ${CC} ${CFLAGS} -Os -ffunction-sections -fdata-sections -Wall -Wno-format-truncation \
        -static -o bootmenu ${S}/bootmenu.c ${LDFLAGS} -Wl,--gc-sections
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 bootmenu ${D}${bindir}/bootmenu
}

FILES:${PN} = "${bindir}/bootmenu"

# static binary: skip the QA check meant for dynamically linked ones
INSANE_SKIP:${PN} += "ldflags"

# runtime: the two drivers (busybox blkid is enough for UUID= / LABEL= slots)
RDEPENDS:${PN} = "vu-fb vu-ir"
