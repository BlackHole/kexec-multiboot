SUMMARY = "chkroot init script of the Vu+ MIPS boxes, with the kexec-multiboot menu"
DESCRIPTION = "The /init of the chkroot initramfs (initramfs-mipsel.cpio.xz of \
meta-vuplus, switch_root multiboot, no kexec), unchanged except for the call \
of bootmenu before STARTUP is read."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

COMPATIBLE_MACHINE = "^(vusolo|vuduo|vuuno|vuultimo|vuzero|vusolose|vusolo2|vuduo2)$"
PACKAGE_ARCH = "${MACHINE_ARCH}"

# init-chkroot.sh: original script (from the 3.13.5 cpio; the 3.9.6 one only
# lacks the optional copy of vuplus-platform-util)
SRC_URI = "file://init-chkroot.sh \
           file://0001-init-chkroot-bootmenu.patch \
          "
# boxes with a front text display (vu-vfd)
VFD_PATCH = "file://0002-init-chkroot-vu_vfd.patch"
SRC_URI:append:vuuno = " ${VFD_PATCH}"
SRC_URI:append:vuduo = " ${VFD_PATCH}"
SRC_URI:append:vuultimo = " ${VFD_PATCH}"
SRC_URI:append:vusolo2 = " ${VFD_PATCH}"
# vuduo2: OLED (vu-vfd) and colour TFT (vu-tft)
SRC_URI:append:vuduo2 = " ${VFD_PATCH} file://0003-init-chkroot-vu_tft.patch"

S = "${UNPACKDIR}"

do_install() {
        install -m 0755 ${S}/init-chkroot.sh ${D}/init
        # directories and nodes of the original cpio
        install -d ${D}/dev ${D}/proc ${D}/sys ${D}/run ${D}/newroot ${D}/newroot_ext ${D}/boot ${D}/tmp
        mknod -m 600 ${D}/dev/console c 5 1
        mknod -m 666 ${D}/dev/null c 1 3
}

FILES:${PN} = "/init /dev /proc /sys /run /newroot /newroot_ext /boot /tmp"
