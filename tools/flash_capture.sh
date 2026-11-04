#!/bin/sh
# Wait for a board in BOOTSEL, start a serial capture, flash the UF2, reboot, dump the log.
# Paths are overridable so the script is usable from other machines:
#   PICOTOOL=... CAPTURE_PS1=... UF2=... CAPTURE_SECS=120 WAIT_ITERS=150 sh tools/flash_capture.sh
PICOTOOL=${PICOTOOL:-/mnt/c/Users/qitas/Downloads/picotool/picotool.exe}
CAPTURE_PS1=${CAPTURE_PS1:-C:\\Users\\qitas\\Downloads\\cap_live.ps1}
POWERSHELL=${POWERSHELL:-/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe}
UF2=${UF2:-C:\\Users\\qitas\\Downloads\\flash-image.uf2}
CAP=${CAP:-/tmp/final_cap.txt}
SECS=${CAPTURE_SECS:-240}
found=0; i=0
while [ $i -lt ${WAIT_ITERS:-150} ]; do
  if ${PICOTOOL} info 2>&1 | grep -q RP2350; then found=1; break; fi
  sleep 2; i=$((i+1))
done
echo "DEVICE_FOUND=$found (waited $((i*2))s)"
[ "$found" = "1" ] || { echo "NO_DEVICE"; exit 0; }
sleep 2
rm -f $CAP
timeout $SECS "$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$CAPTURE_PS1" > $CAP 2>&1 &
sleep 3
${PICOTOOL} load -fu "$UF2" 2>&1 | tr '\r' '\n' | grep -avE "^\s*$" | tail -1
sleep 2
${PICOTOOL} reboot 2>&1 | head -1
echo "=== flashing done, capturing ${SECS}s ==="
sleep $((SECS - 10))
echo "=== CAPTURE ==="
tr -d '\r' < $CAP
