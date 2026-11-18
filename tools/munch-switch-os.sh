#!/bin/sh
# Switch the next boot between Android (slot A) and Windows/UEFI (slot B) on the
# Xiaomi munch with the phone connected over USB.
#
# Usage: tools/munch-switch-os.sh windows|android
#
# Needs adb and fastboot (set PLATFORM_TOOLS=/path/to/platform-tools if they are
# not on PATH). From Windows/UEFI you can instead use the Boot Manager entry
# "Reboot to other slot" (Volume Up at the splash -> Boot Manager).
set -e

TARGET="$1"
case "$TARGET" in
  windows) SLOT=b ;;
  android) SLOT=a ;;
  *) echo "usage: $0 windows|android" >&2; exit 2 ;;
esac

if [ -n "$PLATFORM_TOOLS" ]; then
  ADB="$PLATFORM_TOOLS/adb"; FASTBOOT="$PLATFORM_TOOLS/fastboot"
else
  ADB=adb; FASTBOOT=fastboot
fi

# If Android is running, drop to fastboot first; otherwise assume it is already there.
if "$ADB" get-state >/dev/null 2>&1; then
  "$ADB" reboot bootloader
fi

echo "Waiting for fastboot..."
i=0
until "$FASTBOOT" devices | grep -q fastboot; do
  i=$((i + 1))
  [ "$i" -gt 40 ] && { echo "No device in fastboot. Hold Power + Volume Down." >&2; exit 1; }
  sleep 2
done

"$FASTBOOT" set_active "$SLOT"
"$FASTBOOT" reboot
echo "Rebooting into $TARGET (slot $SLOT)."
