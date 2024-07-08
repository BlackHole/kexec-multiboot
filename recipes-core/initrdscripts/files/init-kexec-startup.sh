#!/bin/sh

LOGGER="KEXEC-2ND STAGE"
echo "$LOGGER: ##############################################################################"
echo "$LOGGER: Subrootdir init"
echo "$LOGGER: ##############################################################################"
echo

mount -n -t proc proc /proc
mount -n -t sysfs sysfs /sys

# read cmdline
CMDLINE=`cat /proc/cmdline`
echo "$LOGGER: /proc/cmdline: $CMDLINE"

NEWCMDLINE="kexec=1"
ERROR=":"

for x in $CMDLINE; do
  case "$x" in
    root=*)
      ROOT="${x#root=}"
      echo "$LOGGER: Found root $ROOT"
      ;;
    rootsubdir=*)
      ROOTSUBDIR="${x#rootsubdir=}"
      echo "$LOGGER: Found rootsubdir $ROOTSUBDIR"
      ;;
    *)
      NEWCMDLINE="${NEWCMDLINE} ${x}"
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

>> $NEWROOT/kexec-multiboot.log

# read cmdline from STARTUP in flash
if [ -f $NEWROOT/STARTUP_ONCE ]; then
    echo "$LOGGER: loading STARTUP_ONCE from $ROOT..." | tee -a $NEWROOT/kexec-multiboot.log
    STARTUP=`cat $NEWROOT/STARTUP_ONCE`
    echo "$LOGGER: removing STARTUP_ONCE" | tee -a $NEWROOT/kexec-multiboot.log
    rm $NEWROOT/STARTUP_ONCE
else
    echo "$LOGGER: loading STARTUP from $ROOT..." | tee -a $NEWROOT/kexec-multiboot.log
    STARTUP=`cat $NEWROOT/STARTUP`
fi
echo "$LOGGER: STARTUP: $STARTUP" | tee -a $NEWROOT/kexec-multiboot.log

for x in $STARTUP; do
  case "$x" in
    kernel=*)
      kernel="${x#kernel=}"
      echo "$LOGGER: Found kernel $kernel" | tee -a $NEWROOT/kexec-multiboot.log
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
      ROOTSUBDIR="${x#rootsubdir=}"
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

if [ ! -b $SROOT ]; then
  echo "$LOGGER: WARNING: Device $SROOT not found... fallback to $ROOT" | tee -a $NEWROOT/kexec-multiboot.log
  SROOT=$ROOT
  ERROR="${ERROR}:scan_device_fails"
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

if [ ! -z ${ROOTSUBDIR+x} ];
then
  if [ $ROOTSUBDIR != linuxrootfs0 ]; then
    if [ -d $SNEWROOT/$ROOTSUBDIR ]
    then
      echo "$LOGGER: Mount bind $ROOTSUBDIR" | tee -a $NEWROOT/kexec-multiboot.log
      mount --bind $SNEWROOT/$ROOTSUBDIR /newroot_subdir
      SNEWROOT="/newroot_subdir"
      grep -q '/newroot_ext' /proc/mounts && umount /newroot_ext
    else
      echo "$LOGGER: $ROOTSUBDIR is not present or no directory. Fallback to root." | tee -a $NEWROOT/kexec-multiboot.log
      kernel="/zImage"
      ROOT=${ROOT}
      ROOTSUBDIR="linuxrootfs0"
      ERROR="${ERROR}:subdir_missing"
    fi
  fi
fi
umount proc sys
#umount proc

NEWCMDLINE="${NEWCMDLINE} kernel=$kernel root=${SROOT} rootsubdir=$ROOTSUBDIR"
if [ x${ERROR} != x":" ]; then
    NEWCMDLINE="${NEWCMDLINE} error=${ERROR}"
fi

kver=$(uname -r)
echo "$LOGGER: ##############################################################################" | tee -a $NEWROOT/kexec-multiboot.log
echo "$LOGGER: Hack to override vuplus static cmdline" | tee -a $NEWROOT/kexec-multiboot.log
echo "$LOGGER: ##############################################################################" | tee -a $NEWROOT/kexec-multiboot.log
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
echo "$LOGGER: kernel v3" | tee -a $NEWROOT/kexec-multiboot.log
mount -o bind $SNEWROOT/var/volatile/tmp/firmware $SNEWROOT/sys/firmware #k3
;;
4*)
echo "$LOGGER: kernel v4" | tee -a $NEWROOT/kexec-multiboot.log
mount -o bind $SNEWROOT/var/volatile/tmp/firmware/devicetree/base/chosen/bootargs $SNEWROOT/sys/firmware/devicetree/base/chosen/bootargs #k4
;;
esac
mount -o bind $SNEWROOT/var/volatile/tmp/proc/cmdline $SNEWROOT/proc/cmdline #k3/k4
echo | tee -a $NEWROOT/kexec-multiboot.log

echo "$LOGGER: ##############################################################################" | tee -a $NEWROOT/kexec-multiboot.log
echo "$LOGGER: Mounting $NEWROOT to $SNEWROOT/boot to expose STARTUP_RECOVERY" | tee -a $NEWROOT/kexec-multiboot.log
echo "$LOGGER: ##############################################################################" | tee -a $NEWROOT/kexec-multiboot.log
if [ $NEWROOT = $SNEWROOT ]; then
    echo "$LOGGER: mount -o bind $NEWROOT $SNEWROOT/boot/" | tee -a $NEWROOT/kexec-multiboot.log
    mount -o bind $NEWROOT $SNEWROOT/boot/
else
    echo "$LOGGER: mount -o move $NEWROOT $SNEWROOT/boot/" | tee -a $NEWROOT/kexec-multiboot.log
    mount -o move $NEWROOT $SNEWROOT/boot/
    NEWROOT=$SNEWROOT/boot
fi
echo | tee -a $NEWROOT/kexec-multiboot.log

echo "$LOGGER: mount devtmpfs to $SNEWROOT/dev" | tee -a $NEWROOT/kexec-multiboot.log
mount -n -t devtmpfs devtmpfs $SNEWROOT/dev
echo | tee -a $NEWROOT/kexec-multiboot.log

echo "$LOGGER: ##############################################################################" | tee -a $NEWROOT/kexec-multiboot.log
echo "$LOGGER: Executing switch_root $SNEWROOT with root $SROOT and rootsubdir $ROOTSUBDIR" | tee -a $NEWROOT/kexec-multiboot.log
echo "$LOGGER: ##############################################################################" | tee -a $NEWROOT/kexec-multiboot.log

exec switch_root $SNEWROOT /sbin/init

echo "$LOGGER: switch_root failed" | tee -a $NEWROOT/kexec-multiboot.log

exec sh 
