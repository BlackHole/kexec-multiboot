SUMMARY = "Extremely basic live image init script for kexec"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://init-kexec.sh"

# boxes with a front text display (vu-vfd): init loads vu_vfd.ko
VFD_PATCH = "file://0001-init-kexec-load-vu_vfd.patch"
SRC_URI:append:vuuno = " ${VFD_PATCH}"
SRC_URI:append:vuduo = " ${VFD_PATCH}"
SRC_URI:append:vuultimo = " ${VFD_PATCH}"
SRC_URI:append:vusolo2 = " ${VFD_PATCH}"


S = "${UNPACKDIR}"

do_install() {
        install -m 0755 ${S}/init-kexec.sh ${D}/init

        # Create device nodes expected by some kernels in initramfs
        # before even executing /init.
        install -d ${D}/dev
        install -d ${D}/etc
        install -d ${D}/proc
        install -d ${D}/sys
        install -d ${D}/newroot
        install -d ${D}/newroot_ext
        install -d ${D}/tmp
        mknod -m 600 ${D}/dev/console c 5 1
        mknod -m 666 ${D}/dev/null c 1 3
        mknod -m 666 ${D}/dev/tty c 5 0
        mknod -m 620 ${D}/dev/tty0 c 4 0
}

inherit allarch
# the patched init is machine specific
PACKAGE_ARCH:vuuno = "${MACHINE_ARCH}"
PACKAGE_ARCH:vuduo = "${MACHINE_ARCH}"
PACKAGE_ARCH:vuultimo = "${MACHINE_ARCH}"
PACKAGE_ARCH:vusolo2 = "${MACHINE_ARCH}"

FILES:${PN} += " /init"
FILES:${PN} += " /dev/console"
FILES:${PN} += " /dev/null"
FILES:${PN} += " /dev/tty"
FILES:${PN} += " /dev/tty0"
FILES:${PN} += " /sys"
FILES:${PN} += " /proc"
FILES:${PN} += " /newroot"
FILES:${PN} += " /newroot_ext"
FILES:${PN} += " /tmp"
