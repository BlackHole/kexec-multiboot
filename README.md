# kexec-multiboot

Linux-as-bootloader multiboot for Vu+ 4K boxes, with a native multiboot menu
(on the TV and on the front display) that needs no proprietary drivers.

Multiboot (kernel_auto.bin): **vuultimo4k, vusolo4k, vuduo4kse, vuduo4k, vuuno4kse, vuuno4k, vuzero4k**.

The native drivers also support the MIPS boxes (CFE bootloader), see
[Native drivers](#native-drivers-recipes-bsp).

## How it works

```
BOLT --> kernel_auto.bin (kernel + 1st stage initramfs) --kexec--> image kernel (+ STARTUP.cpio.gz) --> image
```

* **kernel_auto.bin** is the box kernel with a small initramfs built in (busybox,
  kexec, the init script and the menu). BOLT boots it from the kernel partition.
* The 1st stage init mounts the flash rootfs, shows the multiboot menu, reads
  `STARTUP` (or `STARTUP_ONCE`) and kexec's the kernel of the chosen slot.
* Without `STARTUP` (and `STARTUP_ONCE`) the 1st stage does not kexec: it
  boots the image in `/` of the flash rootfs directly (`switch_root`, with the
  1st stage kernel).
* The image kernel is booted with a 2nd stage initramfs (`STARTUP.cpio.gz`, in
  the root of the flash rootfs) that overrides the static cmdline of the Vu+
  kernels and mounts the flash rootfs into `/boot` (for `STARTUP_ONCE`).

### Multiboot menu

On a cold boot "Press MENU for multiboot" is shown: press **MENU** on the remote
to open the list of slots (`STARTUP_1`..`STARTUP_n`, `STARTUP_RECOVERY`), with
the image name read from each slot's `etc/image-version`.

| key | action |
|---|---|
| UP / DOWN, LEFT / RIGHT | select, page |
| 1..9 | boot that slot |
| OK | boot the slot (copies `STARTUP_x` to `STARTUP`) |
| EXIT | boot the current slot |
| BLUE | configuration |

Configuration (BLUE, saved in `STARTUP_CONFIG` in the flash rootfs):

| key | values | meaning |
|---|---|---|
| `wait=` | 0, 1, 3, 5 (default), 10 | seconds to press MENU; 0 = only if MENU is held at power on |
| `min=` | 0, 2 (default), 4, 6, 8 | the boot never starts earlier (time for USB disks to appear) |
| `tft=` | mirror, own (default), off | front TFT: copy of the TV picture, slot name only, off (vuduo2: own / off) |
| `bright=` | 1..4 (default 4) | "Brightness": all front displays of the box (VFD, LCD, OLED, vuduo2 TFT, 4K TFT); applied at once while editing |

When the menu closes (or when it was never opened) the image that boots stays
on the front display until the image's own drivers take over; on the 4K boxes
with a TFT also on the TFT (vu_fb `keep=1`).

Slots on USB disks (`root=UUID=...` / `LABEL=...`) boot only when their disk is
present: if the disk of the slot to boot does not show up within 15 s the menu
opens and stays open; plugging the disk in boots the slot.

### Native drivers (`recipes-bsp`)

* **vu-fb** - framebuffer. BOLT/CFE leaves its boot splash display running;
  vu_fb only points it at its own buffer (RGB565, 720x576). Front TFT: copy of
  the TV picture (or a separate fb1 with `tft=own`), SPI panel on the solo4k.
  Per SoC data (surface registers found in each BOLT image with
  `tools/bolt_extract.py`, front panel windows) is in `vu_fb_modes.h`.
  The vusolo and vuduo (no CFE display) get their display setup from
  `vu_fb_modes_7325.h` / `vu_fb_modes_7335.h`; on vuuno / vuultimo vu_fb draws
  in the CFE splash. The vusolo2 CFE starts its display only if the splash
  image is in flash (`splash_cfe_auto.bin` from USB); without a running
  display vu_fb exits with `-ENODEV`. `keep=1` leaves the picture on screen at
  unload. On the 4K boxes with a TFT `brightness=1..4` sets the TFT backlight
  through the front MCU (needs vu_ir).
* **vu-ir** - remote control and front keys, read from the front panel MCU on
  its UART. Also gives the MCU to vu_vfd (`vu_mcu_send()`).
* **vu-vfd** - front text display, `/dev/vu_vfd`: the text written between
  open and close is shown at close. MCU VFD of vuuno, vuduo, vusolo2
  (16 characters); on the vuultimo the 256x64 SPI LCD and on the vuduo2 the
  140x32 SPI OLED are used as text displays (internal font). `brightness=1..4`
  (4 = brightest) can be changed at run time in `/sys/module/vu_vfd/parameters`.
* **vu-tft** - the 240x400 colour TFT of the vuduo2 (SPI) as a text display,
  `/dev/vu_tft`, same interface as vu-vfd. Board revision read from the straps.
* **vu-bootmenu** - the menu, a static C binary. Writes the selected image to
  `/dev/vu_vfd` / `/dev/vu_tft` when they exist.

| box | SoC | vu-fb | vu-ir | vu-vfd / vu-tft |
|---|---|---|---|---|
| vuultimo4k, vusolo4k, vuuno4k, vuuno4kse, vuzero4k, vuduo4k, vuduo4kse | 7445, 7366, 7439, 7260, 7278 | yes | yes | - (TFT via vu-fb) |
| vuzero (gen2) | 73625 | yes | yes | - |
| vusolose (gen2) | 7429 | yes | yes | - |
| vusolo2 (gen2) | 7346 (Vu+ module dvb-bcm7356) | yes (splash in flash) | yes | VFD |
| vuduo2 (gen2) | 7424 (family 7425) | yes | yes | OLED (vu-vfd) + colour TFT (vu-tft) |
| vusolo (gen1) | 7325 | yes | yes | - |
| vuuno (gen1) | 7413 | yes | yes | VFD |
| vuultimo (gen1) | 7413 | yes | yes | LCD as text |
| vuduo (gen1) | 7335 | yes | yes | VFD |

All data comes from the boxes themselves (BOLT/CFE images, register dumps)
and from static analysis of the original Vu+ modules. Boxes without a vu-fb
entry load the module with `-ENODEV`, and without a framebuffer the menu
does not open (the boot goes on).

## Building kernel_auto.bin (musl)

The 1st stage initramfs is built with **musl** (small static binaries), the
kernel with the normal toolchain of the box. You need the
[OE-Alliance build-enviroment](https://github.com/oe-alliance/build-enviroment)
for the image of the box; `Makefile-vumultiboot` (in this layer) does the rest.

```
cd build-enviroment
git clone https://github.com/<you>/kexec-multiboot.git
MACHINE=vuultimo4k DISTRO=openbh make -f kexec-multiboot/Makefile-vumultiboot
```

Output: `kernel_auto/vuultimo4k/kernel_auto.bin`.

`Makefile-vumultiboot` includes the build-enviroment `Makefile` and:

1. initialises the box build directory `builds/<distro>/<type>/<box>` (glibc)
   and a second one, `<box>-musl`, with `TCLIBC = "musl"`;
2. adds this layer to the `bblayers.conf` of both;
3. builds `initramfs-kexec-multiboot-image` in `<box>-musl`;
4. builds `linux-vuplus-kexec` in `<box>` with that initramfs built in
   (`bitbake -R conf/kexec-musl.conf`, written by the Makefile);
5. copies the kernel to `kernel_auto/<box>/kernel_auto.bin`.

| target | builds |
|---|---|
| `kernel` (default) | the kernel of the box: `kernel-kexec` on the 4K boxes, `kernel-chkroot` on the MIPS boxes |
| `kernel-kexec` | kernel_auto.bin |
| `kexec-startup` | 2nd stage initramfs, `kernel_auto/<box>/STARTUP.cpio.gz` |
| `kexec-initramfs` | only the musl initramfs |
| `kexec-init` | only sets up the two build directories |

`DISTRO_TYPE` (default `release`) works as with the build-enviroment Makefile.
To rebuild after a change of the drivers or the menu, run the same command again.

`linux-vuplus-kexec` uses the sources, patches and defconfig of the box's
`linux-vuplus` recipe (meta-vuplus) and only builds the initramfs in; it does
not replace the image kernel. busybox `tc` does not build with musl: the layer
disables it for musl builds.

### 2nd stage initramfs

`make -f kexec-multiboot/Makefile-vumultiboot kexec-startup` builds
`initramfs-kexec-multiboot-startup-image`: it goes into the root of the flash
rootfs as `STARTUP.cpio.gz`.

## Installing

Copy kernel_auto.bin to the box and write it to the kernel partition
(**check the partition first: a wrong one can make the box unbootable**):

| box | kernel partition |
|---|---|
| vuultimo4k, vusolo4k, vuuno4k, vuuno4kse | `/dev/mmcblk0p1` |
| vuzero4k | `/dev/mmcblk0p4` (p1 is the NVRAM) |
| vuduo4k, vuduo4kse | `/dev/mmcblk0p6` (p1..p5: nvram, macadr, devtree, bp30, bp31) |

The Makefile prints the size of kernel_auto.bin and stops if it does not fit
the kernel partition of the box (`KEXEC_KERNEL_MAX_<box>`; measured so far:
vuultimo4k, 16 MiB).

```
scp kernel_auto.bin root@<box>:/tmp/
ssh root@<box> dd if=/tmp/kernel_auto.bin of=/dev/mmcblk0p1
```

Image side patches (flashing slots, image manager):

- ofgwrite: https://github.com/oe-alliance/ofgwrite/pull/14
- image manager: https://github.com/BlackHole/obh-core/commits/master
- enigma2: https://github.com/BlackHole/enigma2/commits/Python3.11

More details: https://board.openbh.net/threads/vu-real-multiboot-now-available.3077/

## Tools

`tools/bolt_extract.py <bolt.bin>` - reads the display setup of a BOLT image
(32 or 64 bit) into JSON: useful to find the surface ("scratch") registers when
adding a new box to `vu_fb_modes.h`.

## History

On Vu+ boxes there were different solutions to test images (meoboot,
openmultiboot...). This project started by wrapping openmultiboot with kexec, to
load a kernel instead of flashing the box every time. The Broadcom drivers do not
initialise correctly after kexec if they were already loaded, so the 1st stage
runs "headless" (no proprietary drivers) and the multiboot menu uses its own
minimal drivers. The Vu+ kernels have a static cmdline that the bootloader cannot
override: the 2nd stage initramfs restores the multiboot logic used on other boxes.

## MIPS boxes (chkroot)

vusolo, vuduo, vuuno, vuultimo (kernel 3.9.6) and vuzero, vusolose, vusolo2,
vuduo2 (3.13.5) boot through CFE and use **chkroot**: an initramfs built into
the kernel (`initramfs-mipsel.cpio.xz` of meta-vuplus) whose `/init` reads
`STARTUP` on the `startup` vfat partition and does `switch_root` into the slot
(no kexec: the kernels have no `CONFIG_KEXEC`).

This layer keeps that init and only adds the menu:

* `initramfs-chkroot` - the original chkroot `/init` plus
  `0001-init-chkroot-bootmenu.patch` (mount the startup partition, load
  vu_ir, run bootmenu, unload) and, on the boxes with a front text display,
  `0002-init-chkroot-vu_vfd.patch` (vuduo2 also `0003-init-chkroot-vu_tft.patch`).
  Without a USB startup partition or `STARTUP` the flash image boots
  (`root=` of the CFE cmdline).
* `initramfs-chkroot-image` - busybox + that init + bootmenu, vu-fb, vu-ir
  (vu-vfd, vu-tft), built with musl, `cpio.xz` like the original.
* `linux-vuplus-chkroot` - the box kernel (linux-vuplus 3.9.6 / 3.13.5, same
  sources and defconfig) with that cpio as `CONFIG_INITRAMFS_SOURCE`.

```
MACHINE=vuuno DISTRO=openbh make -f kexec-multiboot/Makefile-vumultiboot
```

The default target sees that the box is a MIPS one and builds `kernel-chkroot`
(the kexec targets refuse the MIPS boxes and vice versa).

Output: `kernel_auto/<box>/kernel_cfe_auto.bin` (the stripped, gzipped vmlinux,
as linux-vuplus packages it). The kernel partition is `/dev/mtd1` on vusolo, vuduo,
vuuno, vuultimo (4 MiB) and `/dev/mtd2` on vuzero, vusolose, vusolo2, vuduo2
(7 MiB on vuzero and vusolo2); the Makefile stops if kernel_cfe_auto.bin does not
fit the partition of the box (sizes in `CHKROOT_KERNEL_MAX_<box>`, not yet
measured on vusolose and vuduo2); `mtd0` is the rootfs. Back it up first (`nanddump`), then
`flash_erase /dev/mtdN 0 0` and `nandwrite -p /dev/mtdN kernel_cfe_auto.bin`. bootmenu also lists `STARTUP_FLASH` (the flash image of
chkroot, read from `ubi0:rootfs`); `STARTUP_RECOVERY` is optional.
