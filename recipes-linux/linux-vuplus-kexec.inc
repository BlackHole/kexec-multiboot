# linux-vuplus-kexec: the kexec-multiboot "1st stage" kernel.
#
# Same sources, patches and defconfig as linux-vuplus (the recipe of the same
# version is required, not copied), plus the kexec initramfs built in.
# linux-vuplus itself is not touched: this is an *alternate* kernel recipe.
#
# Because KERNEL_PACKAGE_NAME != "kernel", kernel.bbclass:
#   - builds in ${WORKDIR}/kernel-source and kernel-build-artifacts instead of
#     work-shared, so it never clobbers the sources the out-of-tree modules
#     (vuplus drivers, vu-fb, vu-ir...) are built against;
#   - deploys into ${DEPLOY_DIR_IMAGE}/kernel-kexec/ so the zImage symlink
#     used by the machine image commands keeps pointing to linux-vuplus.
#
# Build:  bitbake linux-vuplus-kexec
# Output: ${DEPLOY_DIR_IMAGE}/kernel-kexec/zImage   (-> kernel_auto.bin)

SUMMARY = "Linux kernel for ${MACHINE} with the kexec-multiboot initramfs built in"

KERNEL_PACKAGE_NAME = "kernel-kexec"

# linux-vuplus finds its defconfig/patches through the default FILESPATH
# (<its dir>/linux-vuplus-<version>, <its dir>/files): add the same dirs of
# meta-vuplus, located through BBPATH like the require above.
LINUX_VUPLUS_DIR := "${@os.path.dirname(bb.utils.which(d.getVar('BBPATH'), 'recipes-linux/linux-vuplus_%s.bb' % d.getVar('LINUX_VUPLUS_VER')))}"
FILESEXTRAPATHS:prepend := "${LINUX_VUPLUS_DIR}/linux-vuplus-${LINUX_VUPLUS_VER}:${LINUX_VUPLUS_DIR}/files:"

# never selected as the system kernel, never replaces it in a feed
PROVIDES:remove = "virtual/kernel"
RPROVIDES:${KERNEL_PACKAGE_NAME}-image = "${KERNEL_PACKAGE_NAME}-image"

# 1st stage initramfs (busybox + kexec + init script)
KEXEC_INITRAMFS_IMAGE ?= "initramfs-kexec-multiboot-image"
KEXEC_INITRAMFS_FILE ?= "${DEPLOY_DIR_IMAGE}/initramfs-kexec-multiboot-${MACHINE}.cpio.gz"
# the initramfs is a dependency of do_compile, not do_configure: a new
# initramfs must not rerun do_configure, which rewrites include/config/auto.conf
# and with it include/generated/bounds.h, so that every object and module is
# rebuilt; do_compile only relinks usr/initramfs_data.o and vmlinux
do_compile[depends] += "${KEXEC_INITRAMFS_IMAGE}:do_image_complete"

do_compile:prepend() {
    if [ ! -f "${KEXEC_INITRAMFS_FILE}" ]; then
        bbfatal "kexec initramfs not found: ${KEXEC_INITRAMFS_FILE}"
    fi
    install -m 0644 "${KEXEC_INITRAMFS_FILE}" ${B}/initramfs-kexec.cpio.gz
}

do_configure:prepend() {
    # point the kernel at it; edits the unpacked copy of the defconfig only
    sed -i -e '/^CONFIG_INITRAMFS_SOURCE=/d' -e '/^# CONFIG_INITRAMFS_SOURCE is not set/d' ${UNPACKDIR}/defconfig
    echo 'CONFIG_INITRAMFS_SOURCE="initramfs-kexec.cpio.gz"' >> ${UNPACKDIR}/defconfig
    rm -f ${B}/.config
}

# keep the build directory (rm_work): when only the initramfs changes, the
# kernel then relinks instead of rebuilding everything, modules included
RM_WORK_EXCLUDE += "${PN}"
