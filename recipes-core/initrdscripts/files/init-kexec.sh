#!/bin/sh

echo "kexec init"

mount -n -t proc proc /proc
mount -n -t sysfs sysfs /sys

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

# read cmdline from STARTUP in flash
CMDLINE=`cat $NEWROOT/STARTUP`
echo "$CMDLINE"

for x in $CMDLINE; do
  case "$x" in
    BOOT_IMAGE=*)
      BOOT_IMAGE="${x#BOOT_IMAGE=}"
      echo "Found BOOT_IMAGE $BOOT_IMAGE"
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
timeout=1
while [ $CNT -lt 40 ]
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

if [ ! -b $SROOT ]; then
  echo "Device $SROOT not found... fallback to $ROOT"
  SROOT=$ROOT
fi

#let us to use an usb device
if [ $ROOT != $SROOT ]; then
  mkdir -p /snewroot/
  mount -n $SROOT /snewroot/
  SNEWROOT="/snewroot"
else
  SNEWROOT=$NEWROOT
fi

kexec -d -l ${SNEWROOT}${BOOT_IMAGE} --initrd="${NEWROOT}/STARTUP.cpio.gz" --command-line="$(cat /proc/cmdline)"
kexec -d -e

