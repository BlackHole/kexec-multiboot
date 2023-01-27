# kexec-multiboot
A multiboot kernel solution for Vuplus boxes

History
---
On Vuplus boxes in the past there were different solutions to test images... meoboot, openmultiboot and maybe others.
I started modifying openmultiboot some time ago since as I wasn't happy with it. I started modifying it by wrapping it with kexec 
to load a kernel instead of flashing the box every time.

Kexec is a system call that enables you to load and boot into another kernel from the currently running kernel. This is useful for kernel developers or anyone that wants to reboot very quickly without waiting for the whole BIOS boot process to finish. Note that kexec may not work correctly for you due to devices not fully re-initialising when using this method, however this is rarely the case. (source: ArchLinux wiki)
It seems that this hybrid solution can't work on all boxes because the broadcom drivers doesn't initialise correctly without a full reboot.


Technical details
---
It seems that the broadcom driver doesn't have issues with loading if the box hasn't been initialisated yet.
I've started testing this new "headless" approach.

Bootloader --> kernel+intramfs --> kexec --> kernel(guest) --> Image 

Another issue I found on Vuplus boxes is that they have started distributing their source code with a kernel configured with a static cmdline.
This cmdline can't be overriden by the bootloader, so it isn't possible to force the kernel to load a different partition.

Solution: 

Bootloader --> kernel+intramfs --> kexec --> kernel(guest) + intramfs --> Image

So.. the two intramfs emulates the multiboot logic used in other boxes.
- 1st intramfs: scans for external usb device, load original image in flash if device is choosen but it is missing, read an emergency file that overrides the default slot.
- 2nd intramfs: overrides the static cmdline, mounts the flash rootfs into /boot (to manage STARTUP_ONCE), fixes some compatibility issues due to a common bug in enigma2
(https://github.com/torvalds/linux/blob/v5.9/Documentation/ABI/testing/sysfs-firmware-ofw) the stable api to use device tree is /proc/device-tree and not the /sys entry. 


Preparation:
---

1st stage initramfs:  
- bitbake initramfs-kexec-multiboot-image
- copy builds/openbh/release/vuultimo4k/tmp/deploy/images/vuultimo4k/openbh-5.1.012.release-vuultimo4k.initramfs-kexec.rootfs.cpio.gz into meta-oe-alliance/meta-brands/meta-vuplus/recipes-linux/linux-vuplus-*/initramfs-kexec.cpio.gz (on each linux vuplus directory)

1st stage kernel with initramfs linked:  
- apply the patches to a working oe-a tree
- bitbake -c cleansstate linux-vuplus
- bitbake linux-vuplus
- the builds/openbh/release/vuultimo4k/tmp/deploy/images/vuultimo4k/zImage--3.14.28-oea4.2-r4-vuultimo4k-20230124223117.bin produced is the 1st stage kernel

2nd stage initramfs:  
- bitbake initramfs-kexec-multiboot-startup-image
- builds/openbh/release/vuultimo4k/tmp/deploy/images/vuultimo4k/openbh-5.1.012.release-vuultimo4k.initramfs-kexec-startup.rootfs.cpio.gz is the STARTUP.cpio.gz to be laced in ther / of the rootfs to used by the 1st stage initramfs.


More details at:
https://board.openbh.net/threads/vu-real-multiboot-now-available.3077/

needed patches:
[kexec-multiboot] tagged patch at the following repository

- ofgwrite (required to flash slots)
https://github.com/oe-alliance/ofgwrite/pull/14

- image manager 
https://github.com/BlackHole/obh-core/commits/master

- enigma2
https://github.com/BlackHole/enigma2/commits/Python3.11
