# musl build of the kexec initramfs: busybox "tc" does not build against musl
FILESEXTRAPATHS:prepend:libc-musl := "${THISDIR}/files:"
SRC_URI:append:libc-musl = " file://musl-notc.cfg"
