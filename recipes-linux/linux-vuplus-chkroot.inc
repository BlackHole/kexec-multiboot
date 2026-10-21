# linux-vuplus-chkroot: the kernel of a Vu+ MIPS box (CFE) with the chkroot
# initramfs + kexec-multiboot menu built in, instead of the prebuilt
# initramfs-mipsel.cpio.xz of meta-vuplus.
#
# Same sources, patches and defconfig as linux-vuplus (required, not copied).
# As linux-vuplus-kexec: KERNEL_PACKAGE_NAME != "kernel", so it builds in its
# own WORKDIR, deploys into ${DEPLOY_DIR_IMAGE}/kernel-chkroot/ and does not
# replace virtual/kernel.
#
# Build:  bitbake linux-vuplus-chkroot   (Makefile-vumultiboot: kernel-chkroot)
# Output: ${DEPLOY_DIR_IMAGE}/kernel-chkroot/kernel_cfe_auto.bin
#         (the stripped, gzipped vmlinux of linux-vuplus' kernel_do_install)

SUMMARY = "Linux kernel for ${MACHINE} with the chkroot + bootmenu initramfs built in"

KERNEL_PACKAGE_NAME = "kernel-chkroot"

LINUX_VUPLUS_DIR := "${@os.path.dirname(bb.utils.which(d.getVar('BBPATH'), 'recipes-linux/linux-vuplus_%s.bb' % d.getVar('LINUX_VUPLUS_VER')))}"
FILESEXTRAPATHS:prepend := "${LINUX_VUPLUS_DIR}/linux-vuplus-${LINUX_VUPLUS_VER}:${LINUX_VUPLUS_DIR}/files:"

PROVIDES:remove = "virtual/kernel"
RPROVIDES:${KERNEL_PACKAGE_NAME}-image = "${KERNEL_PACKAGE_NAME}-image"
# linux-vuplus flashes the kernel in its postinst: not this one
pkg_postinst:${KERNEL_PACKAGE_NAME}-image () {
    true
}

# the musl initramfs (Makefile-vumultiboot sets the file of the musl build dir)
CHKROOT_INITRAMFS_IMAGE ?= "initramfs-chkroot-image"
CHKROOT_INITRAMFS_FILE ?= "${DEPLOY_DIR_IMAGE}/initramfs-chkroot-${MACHINE}.cpio.xz"
# the initramfs is a dependency of do_compile, not do_configure: a new
# initramfs must not rerun do_configure, which rewrites include/config/auto.conf
# and with it include/generated/bounds.h, so that every object and module is
# rebuilt; do_compile only relinks usr/initramfs_data.o and vmlinux
do_compile[depends] += "${CHKROOT_INITRAMFS_IMAGE}:do_image_complete"

do_compile:prepend() {
    if [ ! -f "${CHKROOT_INITRAMFS_FILE}" ]; then
        bbfatal "chkroot initramfs not found: ${CHKROOT_INITRAMFS_FILE}"
    fi
    install -m 0644 "${CHKROOT_INITRAMFS_FILE}" ${B}/initramfs-chkroot.cpio.xz
}

do_configure:prepend() {
    # same settings as the original (xz), only another file; dvbproxy boxes
    # use defconfig_proxy (renamed to defconfig by linux-vuplus): edit both
    for c in ${UNPACKDIR}/defconfig ${UNPACKDIR}/defconfig_proxy; do
        [ -f $c ] || continue
        sed -i -e '/CONFIG_INITRAMFS_SOURCE/d' $c
        echo 'CONFIG_INITRAMFS_SOURCE="initramfs-chkroot.cpio.xz"' >> $c
    done
    rm -f ${B}/.config
}

# kernel_cfe_auto.bin = the stripped vmlinux.gz that linux-vuplus installs in
# /${KERNEL_IMAGEDEST} (the deployed vmlinux keeps its symbols: ~300 KB more
# gzipped, too big for the 4 MiB kernel partition of the gen1 boxes)
do_deploy:append() {
    install -d ${DEPLOYDIR}/${KERNEL_DEPLOYSUBDIR}
    install -m 0644 ${D}/${KERNEL_IMAGEDEST}/${KERNEL_IMAGETYPE}.gz ${DEPLOYDIR}/${KERNEL_DEPLOYSUBDIR}/kernel_cfe_auto.bin
}

# keep the build directory (rm_work): when only the initramfs changes, the
# kernel then relinks instead of rebuilding everything, modules included
RM_WORK_EXCLUDE += "${PN}"
