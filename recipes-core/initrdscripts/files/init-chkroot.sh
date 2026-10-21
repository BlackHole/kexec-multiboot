#!/bin/sh

mkdir -p /dev
[ -c /dev/console ] || mknod /dev/console c 5 1

echo "[init] Starting custom init script" > /dev/console

# Basic mounts
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

ROOTSUBDIR=""
HAS_STARTUP=0
ROOT=""
ROOTFSTYPE="ubifs"
for param in $(cat /proc/cmdline); do
    case "$param" in
        root=*)
            ROOT="${param#root=}"
            ;;
        rootfstype=*)
            ROOTFSTYPE="${param#rootfstype=}"
            ;;
    esac
done

ORIGINAL_ROOT="$ROOT"
ORIGINAL_ROOTFSTYPE="$ROOTFSTYPE"

#wait for USB switch to initialize
sleep 2
mdev -s
BOOT=""
for dev in sda1 sdb1 sdc1 sdd1 mmcblk0p1 mmcblk1p1; do
    if [[ "$dev" == mmcblk*p* ]]; then
        blockdev=${dev%p[0-9]*}
    else
        blockdev=${dev%[0-9]*}
    fi
    uevent="/sys/block/$blockdev/$dev/uevent"
    [ -f "$uevent" ] || continue
    partname=$(grep '^PARTNAME=' "$uevent" 2>/dev/null | cut -d= -f2)
    if [ "$partname" = "startup" ]; then
        BOOT="/dev/$dev"
        break
    fi
done


for dev in sda sdb sdc sdd; do
    blkid1=$(blkid /dev/${dev}1 2>/dev/null)
    blkid2=$(blkid /dev/${dev}2 2>/dev/null)

    echo "$blkid1" | grep -q 'TYPE="vfat"' || continue
    echo "$blkid2" | grep -q 'LABEL="rootfs"' || continue

    BOOT="/dev/${dev}1"
    break
done

if [ -z "$BOOT" ]; then
    for dev in mmcblk0 mmcblk1; do
        blkid1=$(blkid /dev/${dev}p1 2>/dev/null)
        blkid2=$(blkid /dev/${dev}p2 2>/dev/null)

        echo "$blkid1" | grep -q 'TYPE="vfat"' || continue
        echo "$blkid2" | grep -q 'LABEL="rootfs"' || continue

        BOOT="/dev/${dev}p1"
        break
    done
fi

echo "[init] ROOT found:$ROOT" > /dev/console
echo "[init] BOOT found:$BOOT" > /dev/console
echo "[init] ROOTFSTYPE found:$ROOTFSTYPE" > /dev/console

mkdir -p /boot
if mount -t vfat "$BOOT" /boot 2>/dev/null; then
  echo "[init] $BOOT mounted as FAT" > /dev/console

  if [ -f /boot/STARTUP ]; then
    echo "[init] Reading /boot/STARTUP" > /dev/console
    for x in $(cat /boot/STARTUP); do
      case "$x" in
        root=*)
          ROOT="${x#root=}"
          echo "[init] Overriding ROOT: $ROOT" > /dev/console
          ;;
        rootsubdir=*)
          ROOTSUBDIR="${x#rootsubdir=}"
          HAS_STARTUP=1
          echo "[init] Setting ROOTSUBDIR: $ROOTSUBDIR" > /dev/console
          ;;
        rootfstype=*)
          ROOTFSTYPE="${x#rootfstype=}"
          echo "[init] Setting ROOTFSTYPE: $ROOTFSTYPE" > /dev/console
          ;;
      esac
    done
  else
    echo "[init] No STARTUP file found — using default ROOT: $ROOT" > /dev/console
  fi

  umount /boot
else
  echo "[init] Failed to mount $BOOT — skipping STARTUP check" > /dev/console
fi

# Mount the root filesystem
mkdir -p /newroot
if ! mount -n -t "$ROOTFSTYPE" "$ROOT" /newroot; then
  echo "[init] Failed to mount $ROOT" > /dev/console
  exec sh
fi

NEWROOT=""
IMAGE_FOUND=0
IMAGE_NUMBER="0"

# Check if /sbin/ldconfig or /sbin/init (as symlink) exists in ROOT
if [ ! -f "/newroot/sbin/ldconfig" ] && [ ! -L "/newroot/sbin/init" ]; then
  echo "[init] Neither /sbin/ldconfig nor /sbin/init symlink found in root, checking ROOTSUBDIR and alternate roots" > /dev/console

  # If ROOTSUBDIR is specified, check /newroot/$ROOTSUBDIR first
  if [ -n "$ROOTSUBDIR" ] && [ -d "/newroot/$ROOTSUBDIR" ] && { [ -f "/newroot/$ROOTSUBDIR/sbin/ldconfig" ] || [ -L "/newroot/$ROOTSUBDIR/sbin/init" ]; }; then
    echo "[init] Found valid root (ldconfig or init) in /newroot/$ROOTSUBDIR" > /dev/console
    NEWROOT="/newroot/$ROOTSUBDIR"
    IMAGE_FOUND=1
    IMAGE_NUMBER=$(echo "$ROOTSUBDIR" | sed -n 's/.*linuxrootfs\([0-9]\+\).*/\1/p')
  fi


  # If NEWROOT from ROOTSUBDIR needs to be mounted
  if [ ! -z "${ROOTSUBDIR+x}" ]; then
    if [ -d "/newroot/$ROOTSUBDIR" ]; then
      echo "[init] Mount bind $ROOTSUBDIR" > /dev/console
      NEWROOT="/newroot_ext"
      mount --bind "/newroot/$ROOTSUBDIR" "$NEWROOT"
      umount /newroot
    else
      echo "[init] $ROOTSUBDIR is not present or no directory — fallback to root" > /dev/console
    fi
  fi

  # Wenn gültiges Image gefunden, dann STARTUP anpassen
  if [ "$IMAGE_FOUND" = "1" ]; then
    echo "[init] Adjusting STARTUP for found image (Image number: $IMAGE_NUMBER)" > /dev/console
    if mount -t vfat "$BOOT" /boot 2>/dev/null; then
        if [ "$IMAGE_NUMBER" -eq 0  ]; then
            echo "[init] Using MTD Flash, copying STARTUP_FLASH to STARTUP" > /dev/console
            if [ -f "/boot/STARTUP_FLASH" ]; then
                cp "/boot/STARTUP_FLASH" /boot/STARTUP
            fi
        else
            if [ "$IMAGE_NUMBER" -ge 1 ] && [ -f "/boot/STARTUP_$IMAGE_NUMBER" ]; then
                echo "[init] Copying STARTUP_$IMAGE_NUMBER to STARTUP" > /dev/console
                cp "/boot/STARTUP_$IMAGE_NUMBER" /boot/STARTUP
            else
                echo "[init] No matching STARTUP_X file found — keeping existing STARTUP" > /dev/console
            fi
        fi
      sync
      umount /boot
    else
      echo "[init] Failed to mount BOOT to update STARTUP" > /dev/console
    fi
  else
    echo "[init] No valid root found — retrying original root device" > /dev/console
    mount -n -t "$ORIGINAL_ROOTFSTYPE" "$ORIGINAL_ROOT" /newroot
    if [ -f "/newroot/sbin/ldconfig" ] || [ -L "/newroot/sbin/init" ]; then
      echo "[init] Found valid root on original device" > /dev/console
      NEWROOT="/newroot"
      IMAGE_FOUND=1
    else
      echo "[init] No valid root found even on original device — dropping to shell" > /dev/console
      exec sh
    fi
  fi
else
  # /sbin/ldconfig found at first mount
  NEWROOT="/newroot"
fi

# guest Detection
if [ ! -f /etc/.guest ] && [ ! -f "$NEWROOT/etc/.guest" ] && [ "$IMAGE_NUMBER" != "0" ]; then
    mkdir -p /newroot/oldroot
    mount -n -t "$ORIGINAL_ROOTFSTYPE" "$ORIGINAL_ROOT" /newroot/oldroot
    if [ -d "/newroot/oldroot/lib/modules/" ]; then
        echo "[init] prepare Target OS." > /dev/console
        rm -rf $NEWROOT/lib/modules/*
        mkdir -p $NEWROOT/lib/modules
        cp -af /newroot/oldroot/lib/modules/* $NEWROOT/lib/modules/
        cp -af /newroot/oldroot/usr/bin/multiboot-selector.sh $NEWROOT/usr/bin/multiboot-selector.sh
        [ -d /newroot/oldroot/usr/share/multiboot-legacy ] && cp -rT /newroot/oldroot/usr/share/multiboot-legacy $NEWROOT/usr/lib/enigma2/python/Plugins/Extensions/MultiBootSelector
        cp -af /newroot/oldroot/usr/bin/ofgwrite_bin $NEWROOT/usr/bin/ofgwrite_bin
        rm -rf $NEWROOT/etc/modules-load.d/*
        cp -af /newroot/oldroot/etc/modules-load.d/* $NEWROOT/etc/modules-load.d/
        if [ -f /newroot/oldroot/etc/init.d/vuplus-platform-util ]; then
            cp -af /newroot/oldroot/etc/init.d/vuplus-platform-util $NEWROOT/etc/init.d/
        fi
    fi
    umount /newroot/oldroot
    touch $NEWROOT/etc/.guest
fi

# Final checks before switch_root
if [ -z "$NEWROOT" ]; then
  echo "[init] No NEWROOT set — dropping to shell" > /dev/console
  exec sh
fi

if [ ! -f "$NEWROOT/sbin/ldconfig" ] && [ ! -L "$NEWROOT/sbin/init" ]; then
  echo "[init] Neither /sbin/ldconfig nor /sbin/init symlink found in NEWROOT: $NEWROOT — dropping to shell" > /dev/console
  exec sh
fi

# Prepare final root environment
for d in proc sys dev; do
  mkdir -p "$NEWROOT/$d"
  mount --move "/$d" "$NEWROOT/$d"
done

echo "[init] START IMAGE:$IMAGE_NUMBER" > /dev/console

echo "[init] Executing switch_root to $NEWROOT (ROOT: $ROOT)" > /dev/console
exec switch_root "$NEWROOT" /sbin/init

echo "[init] switch_root failed — dropping to shell" > /dev/console
exec sh