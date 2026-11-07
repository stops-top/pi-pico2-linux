#!/bin/sh
shift

# The 512KB rootfs limit is hardcoded in three places that are built by
# different systems: genimage.cfg (flash partition), the DTS phram reg (PSRAM
# window) and the bootloader's ROOTFS_MAX_SIZE (boot-time copy guard).  Check
# all three agree and that the image still fits, so a desync or an oversized
# rootfs fails the build instead of silently truncating or overflowing at boot.
board_dir=$(dirname "$0")
external_dir=$(CDPATH= cd -- "$board_dir/../../.." && pwd)
CRAMFS_LIMIT=$((512 * 1024))

cramfs="${BINARIES_DIR}/rootfs.cramfs"
size=$(od -An -tu4 -j4 -N4 "$cramfs" | tr -d ' \n')
if [ "$size" -gt "$CRAMFS_LIMIT" ]; then
	echo "ERROR: rootfs.cramfs declares $size bytes, partition holds $CRAMFS_LIMIT" >&2
	echo "       grow genimage.cfg, the DTS phram reg and ROOTFS_MAX_SIZE together" >&2
	exit 1
fi
echo "cramfs: $size/$CRAMFS_LIMIT bytes ($((CRAMFS_LIMIT - size)) bytes headroom)"

grep -q 'size = 512K' "$board_dir/genimage.cfg" || {
	echo "ERROR: genimage.cfg cramfs size is no longer 512K" >&2; exit 1; }
grep -q '0x11700000 0x80000' "$board_dir/dts/sifive/raspberrypi-pico2.dts" || {
	echo "ERROR: DTS phram reg is no longer 0x80000" >&2; exit 1; }
grep -q 'ROOTFS_MAX_SIZE		0x80000U' \
	"$external_dir/package/pico2-bootloader/bootloader/src/main.c" || {
	echo "ERROR: bootloader ROOTFS_MAX_SIZE is no longer 0x80000" >&2; exit 1; }

# cramfs sits at a fixed 2M in flash.  genimage lays the kernel out after the
# bootloader and DTB (together under 16K) and does not complain if the kernel
# grows into the rootfs, so bound it here.  The slack below 2M is deliberate
# kernel growth headroom, not waste.
grep -q 'offset = 2M' "$board_dir/genimage.cfg" || {
	echo "ERROR: genimage.cfg cramfs offset is no longer 2M" >&2; exit 1; }
grep -q 'FLASH_ROOTFS_ADDR	(0x10000000U + 0x200000U)' \
	"$external_dir/package/pico2-bootloader/bootloader/src/main.c" || {
	echo "ERROR: bootloader FLASH_ROOTFS_ADDR no longer points at flash +2M" >&2; exit 1; }
KERNEL_FLASH_LIMIT=$((2 * 1024 * 1024 - 16 * 1024))
ksize=$(wc -c < "${BINARIES_DIR}/Image")
if [ "$ksize" -gt "$KERNEL_FLASH_LIMIT" ]; then
	echo "ERROR: Image is $ksize bytes, only $KERNEL_FLASH_LIMIT fit below the 2M rootfs" >&2
	exit 1
fi
echo "kernel: $ksize bytes ($((KERNEL_FLASH_LIMIT - ksize)) bytes below the rootfs)"

support/scripts/genimage.sh "$@"

if picotool version > /dev/null; then
	picotool uf2 convert "${BINARIES_DIR}/flash-image.bin" \
		"${BINARIES_DIR}/flash-image.uf2" --family rp2350-riscv
else
	echo "picotool not found, skipping uf2 conversion"
	echo "Please run \`picotool uf2 convert ${BINARIES_DIR}/flash-image.bin \
		${BINARIES_DIR}/flash-image.uf2 --family rp2350-riscv\` instead."
fi
tput smso 2>/dev/null
echo "Run \`picotool load -fu ${BINARIES_DIR}/flash-image.uf2\` to flash to pi pico2."
tput rmso 2>/dev/null