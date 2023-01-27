SUMMARY = "Extremely basic live image init script for kexec-startup"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://init-kexec-startup.sh"

S = "${WORKDIR}"

do_install() {
        install -m 0755 ${WORKDIR}/init-kexec-startup.sh ${D}/init

        # Create device nodes expected by some kernels in initramfs
        # before even executing /init.
        install -d ${D}/dev
        install -d ${D}/etc
        install -d ${D}/proc
        install -d ${D}/sys
        install -d ${D}/newroot
        install -d ${D}/newroot_ext
        install -d ${D}/newroot_subdir
        mknod -m 600 ${D}/dev/console c 5 1
        mknod -m 666 ${D}/dev/null c 1 3
        mknod -m 666 ${D}/dev/tty c 5 0
        mknod -m 620 ${D}/dev/tty0 c 4 0
}

inherit allarch

FILES:${PN} += " /init"
FILES:${PN} += " /dev/console"
FILES:${PN} += " /dev/null"
FILES:${PN} += " /dev/tty"
FILES:${PN} += " /dev/tty0"
FILES:${PN} += " /sys"
FILES:${PN} += " /proc"
FILES:${PN} += " /newroot"
FILES:${PN} += " /newroot_ext"
FILES:${PN} += " /newroot_subdir"
