SUMMARY = "Initramfs image for kexec multiboot"
DESCRIPTION = "This image provides kexec multiboot (linux as bootloader) and helpers."

PACKAGE_INSTALL = "busybox initramfs-kexec kexec ${ROOTFS_BOOTSTRAP_INSTALL}"

# multiboot menu: framebuffer + remote drivers (no proprietary drivers needed)
# and the static bootmenu binary, started by init-kexec.sh when MENU is held
PACKAGE_INSTALL += "vu-bootmenu vu-fb vu-ir"
# front text display (MCU VFD or, on the vuultimo, the SPI LCD as text)
PACKAGE_INSTALL:append:vuuno = " vu-vfd"
PACKAGE_INSTALL:append:vuduo = " vu-vfd"
PACKAGE_INSTALL:append:vuultimo = " vu-vfd"
PACKAGE_INSTALL:append:vusolo2 = " vu-vfd"

# no recommended extras in the initramfs
NO_RECOMMENDATIONS = "1"

# the kernel-module packages pull kernel-base -> kernel-image (hard RDEPENDS),
# whose zImage lands in /tmp: not needed inside the initramfs, drop it
remove_kernel_image() {
    rm -f ${IMAGE_ROOTFS}/tmp/zImage* ${IMAGE_ROOTFS}/tmp/vmlinux*
    rm -rf ${IMAGE_ROOTFS}/boot
}
ROOTFS_POSTPROCESS_COMMAND += "remove_kernel_image; "

# Do not pollute the initrd image with rootfs features
IMAGE_FEATURES = ""

export IMAGE_BASENAME = "initramfs-kexec-multiboot-image"
IMAGE_NAME:append = ".kexec-multiboot"
IMAGE_NAME_SUFFIX = ".initramfs-kexec"
IMAGE_LINGUAS = ""

# stable name in DEPLOY_DIR_IMAGE, used by linux-vuplus-kexec
IMAGE_LINK_NAME = "initramfs-kexec-multiboot-${MACHINE}"

# Some BSPs use IMAGE_FSTYPES_<machine override> which would override
# an assignment to IMAGE_FSTYPES so we need anon python
python () {
    d.setVar("IMAGE_FSTYPES", d.getVar("INITRAMFS_FSTYPES"))
}

inherit core-image

# some distros (e.g. openbh) build every image in ${TMPDIR}/rootfs: outside
# WORKDIR pseudo does not track it, so /dev nodes and root ownership are lost
# (no /dev/console -> init gets no console). Use the per-recipe default.
IMAGE_ROOTFS = "${WORKDIR}/rootfs"

IMAGE_ROOTFS_SIZE = "8192"
IMAGE_ROOTFS_EXTRA_SPACE = "0"

BAD_RECOMMENDATIONS += "busybox-syslog"
