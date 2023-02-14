#!/bin/sh

LOGGER="KEXEC-1ST STAGE"
echo "$LOGGER: ##############################################################################"
echo "$LOGGER: kexecboot init"
echo "$LOGGER: ##############################################################################"
echo

mount -n -t proc proc /proc
mount -n -t sysfs sysfs /sys

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

#wait for USB switch to initialize
sleep 2
mdev -s
for device in sda sda1 sdb sdb1 sdc sdc1 scdd sdd1
do
  if [ ! -b /dev/$device ]; then
      echo "$LOGGER: /dev/$device is not a block device... skip" | tee -a $NEWROOT/kexec-multiboot.log
      continue
  fi
  mkdir -p /tmp/$device
  mount -n /dev/$device /tmp/$device 2>/dev/null
  [ -f /tmp/$device/STARTUP_RECOVERY ];
  RC=$?
  umount /tmp/$device 2>/dev/null
  if [ $RC = 0 ]; then
    echo "$LOGGER: STARTUP_RECOVERY found on /dev/$device" | tee -a $NEWROOT/kexec-multiboot.log
    echo "$LOGGER: copying STARTUP_RECOVERY into STARTUP" | tee -a $NEWROOT/kexec-multiboot.log
    cp $NEWROOT/STARTUP_RECOVERY $NEWROOT/STARTUP
    break
  else
    echo "$LOGGER: STARTUP_RECOVERY not present in /dev/$device" | tee -a $NEWROOT/kexec-multiboot.log
  fi
done

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

