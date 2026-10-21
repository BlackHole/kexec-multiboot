SUMMARY = "chkroot initramfs with the kexec-multiboot menu (Vu+ MIPS boxes)"
DESCRIPTION = "Replaces initramfs-mipsel.cpio.xz of meta-vuplus: the original \
chkroot init (switch_root multiboot) plus bootmenu and the native drivers. \
Built with musl (TCLIBC = musl, see Makefile-vumultiboot), xz compressed like the original."

COMPATIBLE_MACHINE = "^(vusolo|vuduo|vuuno|vuultimo|vuzero|vusolose|vusolo2|vuduo2)$"

PACKAGE_INSTALL = "busybox initramfs-chkroot ${ROOTFS_BOOTSTRAP_INSTALL}"
PACKAGE_INSTALL += "vu-bootmenu vu-fb vu-ir"
PACKAGE_INSTALL:append:vuuno = " vu-vfd"
PACKAGE_INSTALL:append:vuduo = " vu-vfd"
PACKAGE_INSTALL:append:vuultimo = " vu-vfd"
PACKAGE_INSTALL:append:vusolo2 = " vu-vfd"
PACKAGE_INSTALL:append:vuduo2 = " vu-vfd vu-tft"

NO_RECOMMENDATIONS = "1"
BAD_RECOMMENDATIONS += "busybox-syslog"

# kernel-module packages pull kernel-image (vmlinux.gz in /tmp): drop it
remove_kernel_image() {
    rm -f ${IMAGE_ROOTFS}/tmp/vmlinux* ${IMAGE_ROOTFS}/tmp/zImage*
    rm -rf ${IMAGE_ROOTFS}/boot/*
}
ROOTFS_POSTPROCESS_COMMAND += "remove_kernel_image; "

IMAGE_FEATURES = ""
IMAGE_LINGUAS = ""
export IMAGE_BASENAME = "initramfs-chkroot-image"
IMAGE_NAME_SUFFIX = ".initramfs-chkroot"
# stable name in DEPLOY_DIR_IMAGE, used by linux-vuplus-chkroot
IMAGE_LINK_NAME = "initramfs-chkroot-${MACHINE}"

# only cpio.xz, as the original initramfs-mipsel.cpio.xz. forcevariable: the
# Vu+ MIPS machines set IMAGE_FSTYPES ?= "ubi" and image.bbclass creates the
# image tasks in its own anonymous python, before one of ours would run
IMAGE_FSTYPES:forcevariable = "cpio.xz"

inherit core-image

# see initramfs-kexec-multiboot-image: per-recipe rootfs, keeps /dev nodes
IMAGE_ROOTFS = "${WORKDIR}/rootfs"
IMAGE_ROOTFS_SIZE = "8192"
IMAGE_ROOTFS_EXTRA_SPACE = "0"

