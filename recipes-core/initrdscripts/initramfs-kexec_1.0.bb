SUMMARY = "Extremely basic live image init script"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://init-kexec.sh"

S = "${WORKDIR}"

do_install() {
        install -m 0755 ${WORKDIR}/init-kexec.sh ${D}/init
        install -d ${D}/newroot
        install -d ${D}/newroot_ext
        mknod -m 600 dev/console c 5 1
        mknod -m 666 dev/null c 1 3
        mknod -m 666 dev/tty c 5 0
        chgrp tty dev/tty
        mknod -m 620 dev/tty0 c 4 0
}

inherit allarch

FILES:${PN} += " /init /newroot /newroot_ext /dev"
