# kexec-multiboot
A multiboot kernel solution for Vuplus boxes

History
---
On Vuplus boxes in the past there was different solution to test images... meoboot, openmultiboot and maybe others.
I've started modifying openmultiboot some time ago since i wasn't happy with it. I've started modifying it by wrapping it with kexec 
to load a kernel instead of flashing the box every time.

Kexec is a system call that enables you to load and boot into another kernel from the currently running kernel. This is useful for kernel developers or other people who need to reboot very quickly without waiting for the whole BIOS boot process to finish. Note that kexec may not work correctly for you due to devices not fully re-initializing when using this method, however this is rarely the case. (source: ArchLinux wiki)
It seems that this hibrid solution couldn't work on all boxes because the broadcom drivers doesn't initialize correctly without a full reboot.


Technical deatais
---
It seems that the broadcom driver doesn't have issue by loading if the box haven't been initializated yet.
I've started testing this new "headless" approach.

Bootloader --> kernel+intramfs --> kexec --> kernel(guest) --> Image 

Another issue I found on vuplus boxes is that they have started distributing their source code with a kernel configured with a static cmdline.
This cmdline can't be overrided by the bootloader, so isn't possible to force the kernel to load a different partition.

Solution: 

Bootloader --> kernel+intramfs --> kexec --> kernel(guest) + intramfs --> Image

So.. the two intramfs emulates the multiboot logic used in other boxes.
- 1st intramfs: scan for external usb device, load original image in flash if device is choosen but it  is missing, read an emergency file that overrides the default slot.
- 2nd intramfs: overrides the static cmdline, mount the flash rootfs into /boot (to manage STARTUP_ONCE), fixes some compatibility issue due to a common bug in enigma2
(https://github.com/torvalds/linux/blob/v5.9/Documentation/ABI/testing/sysfs-firmware-ofw) the stable api to use device tree is /proc/device-tree and not the /sys entry. 

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
