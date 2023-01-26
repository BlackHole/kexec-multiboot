#!/bin/sh

echo "##############################################################################"
echo "Subrootdir init"
echo "##############################################################################"
echo

mount -n -t proc proc /proc
mount -n -t sysfs sysfs /sys

# read cmdline
CMDLINE=`cat /proc/cmdline`
echo "$CMDLINE"

NEWCMDLINE="kexec=1"

for x in $CMDLINE; do
  case "$x" in
    root=*)
      ROOT="${x#root=}"
      echo "Found root $ROOT"
      ;;
    rootsubdir=*)
      ROOTSUBDIR="${x#rootsubdir=}"
      echo "Found rootsubdir $ROOTSUBDIR"
      ;;
    *)
      NEWCMDLINE="${NEWCMDLINE} ${x}"
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
if [ -f $NEWROOT/STARTUP_ONCE ]; then
    echo "INFO: loading STARTUP_ONCE from $ROOT..."
    STARTUP=`cat $NEWROOT/STARTUP_ONCE`
    echo "INFO: removing STARTUP_ONCE"
    rm $NEWROOT/STARTUP_ONCE
else
    echo "INFO: loading STARTUP from $ROOT..."
    STARTUP=`cat $NEWROOT/STARTUP`
fi
echo "STARTUP: $STARTUP"

for x in $STARTUP; do
  case "$x" in
    kernel=*)
      kernel="${x#kernel=}"
      echo "Found kernel $kernel"
      NEWCMDLINE="${NEWCMDLINE} ${x}"
      ;;
    root=*)
      SROOT=$(echo "${x#root=}" | tr -d '"')
      echo "Found root $SROOT"
      NEWCMDLINE="${NEWCMDLINE} ${x}"
      ;;
    rootsubdir=*)
      ROOTSUBDIR="${x#rootsubdir=}"
      echo "Found rootsubdir $ROOTSUBDIR"
      NEWCMDLINE="${NEWCMDLINE} ${x}"
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

if [ ! -b $SROOT ]; then
  echo "WARNING: Device $SROOT not found... fallback to $ROOT"
  SROOT=$ROOT
fi

#let us to use an usb device
if [ x$ROOT != x$SROOT ]; then
  #umount /newroot
  mkdir -p /newroot_ext/
  mount -n $SROOT /newroot_ext/
  SNEWROOT="/newroot_ext"
else
  SNEWROOT=$NEWROOT
fi


if [ $ROOTSUBDIR = linuxrootfs0 ]; then
    unset ROOTSUBDIR
fi

if [ ! -z ${ROOTSUBDIR+x} ];
then
  if [ -d $SNEWROOT/$ROOTSUBDIR ];
  then
    echo "Mount bind $ROOTSUBDIR"
    mount --bind $SNEWROOT/$ROOTSUBDIR /newroot_subdir
    SNEWROOT="/newroot_subdir"
    grep -q '/newroot_ext' /proc/mounts && umount /newroot_ext
  else
    echo "$ROOTSUBDIR is not present or no directory. Fallback to root."
  fi
fi

umount proc sys
#umount proc

kver=$(uname -r)
echo "##############################################################################"
echo "Hack to override vuplus static cmdline"
echo "##############################################################################"
mkdir -p $SNEWROOT/var/volatile
mount -n -t tmpfs tmpfs $SNEWROOT/var/volatile
mkdir -p $SNEWROOT/var/volatile/tmp/firmware/devicetree/base/chosen/
mkdir -p $SNEWROOT/var/volatile/tmp/proc/
mount -n -t sysfs sysfs $SNEWROOT/sys
#mount -o bind /sys $SNEWROOT/sys
mount -n -t proc proc $SNEWROOT/proc
echo "$NEWCMDLINE" > $SNEWROOT/var/volatile/tmp/proc/cmdline
echo "$NEWCMDLINE" > $SNEWROOT/var/volatile/tmp/firmware/devicetree/base/chosen/bootargs
case $kver in
3*)
echo "DEBUG: kernel v3"
mount -o bind $SNEWROOT/var/volatile/tmp/firmware $SNEWROOT/sys/firmware #k3
;;
4*)
echo "DEBUG: kernel v4"
mount -o bind $SNEWROOT/var/volatile/tmp/firmware/devicetree/base/chosen/bootargs $SNEWROOT/sys/firmware/devicetree/base/chosen/bootargs #k4
;;
esac
mount -o bind $SNEWROOT/var/volatile/tmp/proc/cmdline $SNEWROOT/proc/cmdline #k3/k4
echo

echo "##############################################################################"
echo "Mounting $NEWROOT to $SNEWROOT/boot to expose STARTUP_RECOVERY"
echo "##############################################################################"
if [ $NEWROOT = $SNEWROOT ]; then
    echo "mount -o bind $NEWROOT $SNEWROOT/boot/"
    mount -o bind $NEWROOT $SNEWROOT/boot/
else
    echo "mount -o move $NEWROOT $SNEWROOT/boot/"
    mount -o move $NEWROOT $SNEWROOT/boot/
fi
echo

echo "INFO: mount devtmpfs to $SNEWROOT/dev"
mount -n -t devtmpfs devtmpfs $SNEWROOT/dev
echo

echo "##############################################################################"
echo "Executing switch_root $SNEWROOT with root $SROOT and rootsubdir $ROOTSUBDIR"
echo "##############################################################################"

exec switch_root $SNEWROOT /sbin/init

echo "switch_root failed"

exec sh 