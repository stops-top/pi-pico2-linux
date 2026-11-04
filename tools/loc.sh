#!/bin/sh
# loc.sh <hex-addr> -> nearest symbol, file:line (needs DEBUG_INFO) and surrounding disassembly.
# Override the kernel build tree with KERNEL_TREE=... if it lives elsewhere.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KT=${KERNEL_TREE:-$ROOT/buildroot/output/build/linux-6.15}
CROSS=${CROSS_COMPILE:-$ROOT/buildroot/output/host/bin/riscv32-buildroot-linux-uclibc-}
A=$(echo "$1" | sed 's/^0x//')
[ -f "$KT/System.map" ] || { echo "no $KT/System.map (build the kernel first)"; exit 1; }
awk -v a="$A" 'BEGIN{n=strtonum("0x" a)} {if ($1 <= sprintf("%08x", n)) {sym=$3; addr=$1}}
	END{printf "closest symbol below: %s at %s\n", sym, addr}' "$KT/System.map"
"${CROSS}addr2line" -f -e "$KT/vmlinux" 0x$A 2>/dev/null
"${CROSS}objdump" -d --start-address=$((0x$A - 16)) --stop-address=$((0x$A + 8)) "$KT/vmlinux" | tail -8
