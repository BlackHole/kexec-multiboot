#!/bin/sh

LOGGER="KEXEC-1ST STAGE"
echo "$LOGGER: ##############################################################################"
echo "$LOGGER: kexecboot init"
echo "$LOGGER: ##############################################################################"
echo

mount -n -t proc proc /proc
mount -n -t sysfs sysfs /sys

# make sure /dev is populated (console, null, mmcblk*) even if the device
# nodes are missing from the initramfs, and get a console for our output
mount -n -t devtmpfs devtmpfs /dev 2>/dev/null
[ -c /dev/console ] && exec </dev/console >/dev/console 2>&1

MODEL=`cat /proc/device-tree/bolt/board | tr "[A-Z]" "[a-z]"`

case $MODEL in
solo4k|uno4k|ultimo4k)
    KERNEL=mmcblk0p1
    ROOTFS=mmcblk0p4
;;
uno4kse)
    KERNEL=mmcblk0p1
    ROOTFS=mmcblk0p4
;;
zero4k)
    KERNEL=mmcblk0p4
    ROOTFS=mmcblk0p7
;;
duo4k|duo4kse)
    KERNEL=mmcblk0p6
    ROOTFS=mmcblk0p9
;;
*)
    echo "$LOGGER: this box isn't supported yet"
;;
esac

# read cmdline
CMDLINE=`cat /proc/cmdline`
echo "$CMDLINE"

for x in $CMDLINE; do
  case "$x" in
    root=*)
      ROOT="${x#root=}"
      echo "$LOGGER: Found root $ROOT"
      ;;
#    rootsubdir=*)
#      ROOTSUBDIR="${x#rootsubdir=}"
#      echo "Found rootsubdir $ROOTSUBDIR"
#      ;;
  esac
done

# wait until root is available
mdev -s
while [ ! -b $ROOT ]
do
  echo "$LOGGER: WAITING"
  usleep 200000
  mdev -s
done

mount -n $ROOT /newroot/
NEWROOT="/newroot"
SROOTWAIT=10

> $NEWROOT/kexec-multiboot.log

# multiboot menu: press MENU on the remote during the wait (bootmenu wait=,
# default 5 s, set in $NEWROOT/STARTUP_CONFIG with the BLUE key) to choose the
# slot. vu_fb/vu_ir drive the display and the front panel MCU without the
# proprietary drivers; bootmenu rewrites $NEWROOT/STARTUP if a slot is chosen.
# bootmenu stays at least min= seconds (STARTUP_CONFIG, default 2) so USB devices can appear,
# and waits for the disk of a root=UUID=/LABEL= slot (opens the menu if it is missing).
if [ -x /usr/bin/bootmenu ]; then
  # vu_ir only: bootmenu loads vu_fb itself (mode from $NEWROOT/STARTUP_CONFIG)
  # when it shows the wait hint or the menu (with wait=0 a normal boot does not touch the video)
  insmod /lib/modules/`uname -r`/extra/vu_ir.ko
  /usr/bin/bootmenu --root $NEWROOT
  RC=$?
  echo "$LOGGER: bootmenu exit code $RC (10 = STARTUP changed)" | tee -a $NEWROOT/kexec-multiboot.log
  # unload: give the display back to BOLT's splash before kexec
  rmmod vu_ir 2>/dev/null
  rmmod vu_fb 2>/dev/null
fi

# no STARTUP (nor STARTUP_ONCE), or an empty one: no multiboot set up yet.
# Boot the image in / of $ROOT directly with this kernel (no kexec)
if [ ! -s $NEWROOT/STARTUP_ONCE ] && [ ! -s $NEWROOT/STARTUP ]; then
  if [ -x $NEWROOT/sbin/init ] || [ -L $NEWROOT/sbin/init ]; then
    echo "$LOGGER: no STARTUP in $ROOT: booting the image in / of $ROOT without kexec" | tee -a $NEWROOT/kexec-multiboot.log
    for d in dev proc sys; do
      mount --move /$d $NEWROOT/$d 2>/dev/null || umount /$d 2>/dev/null
    done
    exec switch_root $NEWROOT /sbin/init
    echo "$LOGGER: switch_root failed"
  else
    echo "$LOGGER: no STARTUP and no /sbin/init in $ROOT" | tee -a $NEWROOT/kexec-multiboot.log
  fi
fi

# read cmdline from STARTUP in flash
if [ -f $NEWROOT/STARTUP_ONCE ]; then
    echo "$LOGGER: loading STARTUP_ONCE from $ROOT..." | tee -a $NEWROOT/kexec-multiboot.log
    STARTUP=`cat $NEWROOT/STARTUP_ONCE`
else
    echo "$LOGGER: loading STARTUP from $ROOT..." | tee -a $NEWROOT/kexec-multiboot.log
    STARTUP=`cat $NEWROOT/STARTUP`
fi
echo "$LOGGER: STARTUP: $STARTUP" | tee -a $NEWROOT/kexec-multiboot.log

SINITRD="STARTUP.cpio.gz"

for x in $STARTUP; do
  case "$x" in
    kernel=*)
      SKERNEL="${x#kernel=}"
      echo "$LOGGER: Found kernel $SKERNEL" | tee -a $NEWROOT/kexec-multiboot.log
      ;;
    initrd=*)
      SINITRD="${x#initrd=}"
      echo "$LOGGER: Found initrd $SINITRD" | tee -a $NEWROOT/kexec-multiboot.log
      ;;
    root=*)
      SROOT=$(echo "${x#root=}" | tr -d '"')
      echo "$LOGGER: Found root $SROOT" | tee -a $NEWROOT/kexec-multiboot.log
      ;;
    rootwait=*)
      SROOTWAIT=$(echo "${x#rootwait=}" | tr -d '"')
      echo "$LOGGER: Found rootwait $SROOTWAIT" | tee -a $NEWROOT/kexec-multiboot.log
      ;;
    rootsubdir=*)
      ROOTSUBDIR="/${x#rootsubdir=}"
      echo "$LOGGER: Found rootsubdir $ROOTSUBDIR" | tee -a $NEWROOT/kexec-multiboot.log
      ;;
  esac
done

# wait until startup root is available
mdev -s
CNT=0
while [ $CNT -lt ${SROOTWAIT} ]
do
  echo "$LOGGER: WAITING" | tee -a $NEWROOT/kexec-multiboot.log
  usleep 200000
  let CNT++
  mdev -s
  if echo $SROOT | grep -qi "UUID="; then
    DEVICE=$(blkid | sed -n "/${SROOT#*=}/s/\([^:]\+\):.*/\\1/p")
    if [ x${DEVICE} != x ]; then
        SROOT=$DEVICE
    fi
  fi
  if [ -b $SROOT ]; then
     break
  fi
done

#let us to use an usb device
if [ -b $SROOT ] && [ $ROOT != $SROOT  ]; then
  mount -n $SROOT /newroot_ext/
  SNEWROOT="/newroot_ext"
  SKERNELDIR=${SNEWROOT}
elif [ ! -b $SROOT ]; then
  echo "$LOGGER: Device $SROOT not found... fallback to $ROOT" | tee -a $NEWROOT/kexec-multiboot.log
  SKERNELDIR=/dev
  SKERNEL=$KERNEL
  SNEWROOT=$NEWROOT
else
  SNEWROOT=$NEWROOT
  SKERNELDIR=${SNEWROOT}
fi

if [ ! -f ${SKERNELDIR}/${SKERNEL} ]; then
  echo "$LOGGER: Kernel not found in ${SKERNELDIR}/${SKERNEL}" | tee -a $NEWROOT/kexec-multiboot.log
  echo "$LOGGER: Device $SROOT not found... fallback to $ROOT" | tee -a $NEWROOT/kexec-multiboot.log
  SKERNELDIR=/dev
  SKERNEL=$KERNEL
  SNEWROOT=$NEWROOT
fi

echo | tee -a $NEWROOT/kexec-multiboot.log
echo "$LOGGER: ##############################################################################" | tee -a $NEWROOT/kexec-multiboot.log
echo "$LOGGER: booting kernel: ${SKERNELDIR}/${SKERNEL}" | tee -a $NEWROOT/kexec-multiboot.log
echo "$LOGGER: booting initrd: ${NEWROOT}/${SINITRD}" | tee -a $NEWROOT/kexec-multiboot.log
echo "$LOGGER: ##############################################################################" | tee -a $NEWROOT/kexec-multiboot.log

kexec -d -l ${SKERNELDIR}/${SKERNEL} --initrd="${NEWROOT}/$SINITRD" --command-line="$(cat /proc/cmdline)" | tee -a $NEWROOT/kexec-multiboot.log
kexec -d -e

