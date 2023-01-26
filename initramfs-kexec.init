#!/bin/sh

echo "##############################################################################"
echo "kexecboot init"
echo "##############################################################################"
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
    echo "this box isn't supported yet"
;;
esac

# read cmdline
CMDLINE=`cat /proc/cmdline`
echo "$CMDLINE"

for x in $CMDLINE; do
  case "$x" in
    root=*)
      ROOT="${x#root=}"
      echo "Found root $ROOT"
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
  echo "WAITING"
  usleep 200000
  mdev -s
done

mount -n $ROOT /newroot/
NEWROOT="/newroot"

#wait for USB switch to initialize
sleep 2
mdev -s
for device in sda sda1 sdb sdb1 sdc sdc1 scdd sdd1
do
  if [ ! -b /dev/$device ]; then
      echo "INFO: /dev/$device is not a block device... skip"
      continue
  fi
  mkdir -p /tmp/$device
  mount -n /dev/$device /tmp/$device 2>/dev/null
  [ -f /tmp/$device/STARTUP_RECOVERY ];
  RC=$?
  umount /tmp/$device 2>/dev/null
  if [ $RC = 0 ]; then
    echo "INFO: STARTUP_RECOVERY found on /dev/$device"
    echo "INFO: copying STARTUP_RECOVERY into STARTUP"
    cp $NEWROOT/STARTUP_RECOVERY $NEWROOT/STARTUP
    break
  else
    echo "INFO: STARTUP_RECOVERY not present in /dev/$device"
  fi
done

# read cmdline from STARTUP in flash
if [ -f $NEWROOT/STARTUP_ONCE ]; then
    echo "INFO: loading STARTUP_ONCE from $ROOT..."
    STARTUP=`cat $NEWROOT/STARTUP_ONCE`
else
    echo "INFO: loading STARTUP from $ROOT..."
    STARTUP=`cat $NEWROOT/STARTUP`
fi
echo "STARTUP: $STARTUP"

SINITRD="STARTUP.cpio.gz"

for x in $STARTUP; do
  case "$x" in
    kernel=*)
      SKERNEL="${x#kernel=}"
      echo "Found kernel $SKERNEL"
      ;;
    initrd=*)
      SINITRD="${x#initrd=}"
      echo "Found initrd $SINITRD"
      ;;
    root=*)
      SROOT=$(echo "${x#root=}" | tr -d '"')
      echo "Found root $SROOT"
      ;;
    rootsubdir=*)
      ROOTSUBDIR="/${x#rootsubdir=}"
      echo "Found rootsubdir $ROOTSUBDIR"
      ;;
  esac
done

# wait until startup root is available
mdev -s
CNT=0
while [ $CNT -lt 10 ]
do
  echo "WAITING"
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
  echo "Device $SROOT not found... fallback to $ROOT"
  SKERNELDIR=/dev
  SKERNEL=$KERNEL
  SNEWROOT=$NEWROOT
else
  SNEWROOT=$NEWROOT
  SKERNELDIR=${SNEWROOT}
fi

echo
echo "##############################################################################"
echo "booting kernel: ${SKERNELDIR}/${SKERNEL}"
echo "booting initrd: ${NEWROOT}/${SINITRD}"
echo "##############################################################################"

kexec -d -l ${SKERNELDIR}/${SKERNEL} --initrd="${NEWROOT}/$SINITRD" --command-line="$(cat /proc/cmdline)"
kexec -d -e

