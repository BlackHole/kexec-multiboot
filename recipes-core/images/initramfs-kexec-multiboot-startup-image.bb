SUMMARY = "Initramfs image for kexec multiboot startup"
DESCRIPTION = "This image provides kexec multiboot (linux as bootloader) and helpers."

PACKAGE_INSTALL = "busybox initramfs-kexec-startup ${ROOTFS_BOOTSTRAP_INSTALL}"

# Do not pollute the initrd image with rootfs features
IMAGE_FEATURES = ""

export IMAGE_BASENAME = "initramfs-kexec-multiboot-image"
IMAGE_NAME:append = ".kexec-multiboot"
IMAGE_NAME_SUFFIX = ".initramfs-kexec-startup"
IMAGE_LINGUAS = ""

# Some BSPs use IMAGE_FSTYPES_<machine override> which would override
# an assignment to IMAGE_FSTYPES so we need anon python
python () {
    d.setVar("IMAGE_FSTYPES", d.getVar("INITRAMFS_FSTYPES"))
}

inherit core-image

IMAGE_ROOTFS_SIZE = "8192"
IMAGE_ROOTFS_EXTRA_SPACE = "0"

BAD_RECOMMENDATIONS += "busybox-syslog"
