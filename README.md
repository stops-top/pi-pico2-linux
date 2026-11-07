# Raspberry Pi Pico 2 / RP2350A Buildroot — Linux on 8 MB of PSRAM

Linux (RISC-V, rv32 nommu, M-mode) built with Buildroot for the RP2350 RISC-V
cores. A small bootloader copies the kernel, the device tree and a cramfs root
filesystem into external QSPI PSRAM and executes everything from there.

This fork is brought up on a custom **RP2350A minimal board** (USB-C, QSPI flash
on CS0, 8 MB PSRAM on CS1). The kernel reaches `/sbin/init` 0.98 s after the
bootloader jumps to it:

```
RP2350 Bootloader starting...
PSRAM ID: 5d 46
PSRAM setup complete. PSRAM size 0x800000 (8388608)
PSRAM executes at 0x117ffffc
Copying DTB (1989 bytes) to 0x11780000... ok
Copying rootfs (442368 bytes) to 0x11700000... ok
Copying kernel (1334536 bytes) to 0x11000000... ok
Jumping to kernel at 0x11000000, DT at 0x11780000
[    0.000000] Linux version 6.15.0 ... riscv32
[    0.102954] Memory: 6064K/7676K available (885K kernel code, ...)
[    0.958231] VFS: Mounted root (cramfs filesystem) readonly on device 31:0.
[    0.977389] Run /sbin/init as init process
~ #
```

Upstream project: <https://github.com/Mr-Bossman/pi-pico2-linux> (written for the
[SparkFun Pro Micro RP2350](https://www.sparkfun.com/products/24870), whose
PSRAM is an `APS6404L`; see [Other boards](#10-other-boards)).

---

## 1. Build

```bash
git clone --recursive https://github.com/stops-top/pi-pico2-linux
cd pi-pico2-linux
git submodule update --init           # buildroot is the submodule; pico-sdk is not needed

make -C buildroot BR2_EXTERNAL=$PWD/ raspberrypi-pico2_defconfig
make -C buildroot BR2_EXTERNAL=$PWD/
```

The last step prints the flashing command and puts everything in
`buildroot/output/images/`:

| file | contents |
|---|---|
| `flash-image.uf2` | what you flash (5 MB UF2 carrying a 2.5 MB image) |
| `flash-image.bin` | the same content as a raw flash image |
| `bootloader.bin`, `Image`, `raspberrypi-pico2.dtb`, `rootfs.cramfs` | the individual pieces |

Gotchas:

* **`BR2_EXTERNAL` on every `make`** — upstream shows a bare `make -C buildroot`;
  it only works when the output directory is the one that was configured.
* **Building as root** — host-tar refuses:
  `FORCE_UNSAFE_CONFIGURE=1 make -C buildroot ...`.
* **Downloads** — the 6.15 kernel tarball comes from kernel.org and can stall; a
  mirror such as `https://mirrors.tuna.tsinghua.edu.cn/kernel/` works, or copy a
  populated `buildroot/dl/` from an earlier build. The expected sha256 lives in
  `board/raspberrypi/raspberrypi-pico2/patches/linux/linux.hash` — an external
  tree must ship its own hash file for a custom package version, otherwise
  `ERROR: No hash found for linux-6.15.tar.xz`.
* **Rebuilding one piece** — Buildroot does not notice edits inside
  `package/pico2-bootloader/` or in the `.dts`; use
  `... pico2-bootloader-rebuild all` and `... linux-rebuild all`
  (`linux-reconfigure` also re-applies the kernel config fragment).
* **Toolchain from scratch** takes roughly 20 min over 282 package steps; only
  `makeinfo` warnings are expected, no real errors.
* **Buildroot version** — the submodule is pinned to `2026.11-git` (commit
  `c17458af`), upgraded from 2025.08. That bumps uClibc-ng 1.0.52 → 1.0.59, whose
  new `libm/riscv32/` fenv sources embed `frrm`/`fsrm`/`frflags`/`fsflags` and the
  `fcsr`/`frm`/`fflags` CSRs. Hazard3 has neither F nor Zfinx, so binutils rejects
  them (`unrecognized opcode 'frrm a0'`). The fix lives in
  [`external.mk`](external.mk): it redefines `UCLIBC_FLOAT_CONFIG` to keep
  buildroot's settings minus `UCLIBC_HAS_FENV`. This works because
  `include $(BR2_EXTERNAL_MKS)` runs *after* `include package/*/*.mk`, and because
  `UCLIBC_KCONFIG_FIXUP_CMDS` is expanded at build time. Two things that look like
  fixes but are not: a `BR2_UCLIBC_CONFIG_FRAGMENT_FILES` fragment (the fixup runs
  after fragments are merged and re-enables the symbol), and `BR2_SOFT_FLOAT=y`
  in the defconfig (a hidden symbol with no prompt and no riscv `select`, so
  kconfig silently drops it — verified with both `--defconfig` and `olddefconfig`).

## 2. Flash and capture the log

Put the board in BOOTSEL (hold `BOOTSEL` while plugging in) and:

```bash
picotool load -fu buildroot/output/images/flash-image.uf2   # -f force, -u run after load
picotool info                      # lists only devices that are in BOOTSEL
picotool reboot                    # needed if the board stayed in BOOTSEL
```

Drag-and-drop of the `.uf2` in Windows works as well (family `rp2350-riscv`).
Before flashing MicroPython or any other image, erase first — our image leaves no
USB stack behind, so `picotool` cannot reach the board in application mode:

```bash
sudo picotool erase
```

Two capture pitfalls worth knowing (both cost a debugging cycle if you trip on
them):

* `picotool load -fu` resets the board as soon as the last block lands, so start
  the serial capture **before** the transfer, not after.
* If the capture opens the port exactly while the board is resetting, the
  received stream can show the *same* log record (same timestamp) repeated dozens
  of times with mojibake in between. It looks like a kernel crash loop and is
  not: `grep -c "Linux version"` returning 1 proves the kernel booted once.
  Re-plug the board and capture again before chasing it.

## 3. Serial console

Console is **PL011 UART0**, 115200 8N1:

| USB-UART adapter | RP2350A |
|---|---|
| RX (adapter receives) | GPIO0 (board TX) |
| TX (adapter sends) | GPIO1 (board RX) |
| GND | GND |

The bootloader programs the baud rate; the SBSA PL011 driver deliberately never
touches the divisor, which is why the log says `base_baud = 0` — expected, not a
fault. Using `arm,pl011`+`arm,primecell` instead of `arm,sbsa-uart` was tried and
does not work here (ttyAMA0 is never created, or the driver probe fails with
`-2`), and this kernel has no RX-watermark option, so long pasted lines can still
log `ttyAMA0: input overrun`. Type short commands, or wire RTS/CTS on GPIO2/GPIO3
if you need flow control.

## 4. Hardware and memory layout

From this board's schematic (`SCH_Schemati_RP2350A Minimal_2026-08-26.pdf`):

* RP2350A on the RISC-V cores, 12 MHz crystal, USB-C with 5.1 kΩ CC resistors,
  `BOOT` and `RUN` buttons, separate 3V3 rail with power LED.
* QSPI **CS0**: serial flash. The image is laid out for 4 MB (change
  `genimage.cfg` + the `.dts` for another size).
* QSPI **CS1 = GPIO19**: 8 MB PSRAM, marking `LSP-PSRAM64H`; the ID the
  bootloader reads is `0x5d` (known-good-die byte) / `0x46` (size → 8 MB).
* Status LED on **GPIO25** (`LED_PIN` in
  `package/pico2-bootloader/bootloader/src/main.c`; it is turned on right before
  the jump into the kernel).

Flash layout (`board/raspberrypi/raspberrypi-pico2/genimage.cfg`):

| offset | contents |
|---|---|
| `0x000000` | picobin header + bootloader + DTB + kernel (the bootloader locates the payload through `__payload_load_start`, hence the 16-byte padding and `align = 16`) |
| `0x200000` | cramfs, 512 KB partition (current image is 432 KB, so an oversized rootfs fails at *build* time) |

The kernel lands at `0x2560` and ends at `0x13CE8C`, leaving 780 KB of slack below
the rootfs. That gap is deliberate growth headroom, not waste: `post-image.sh`
asserts the kernel still fits under 2 MB − 16 KB and that the cramfs offset, the
bootloader's `FLASH_ROOTFS_ADDR` and the three copies of the 512 KB limit
(`genimage.cfg`, the DTS `phram` reg, `ROOTFS_MAX_SIZE`) all still agree, so a
desync fails the build instead of silently truncating at boot.

PSRAM layout (fixed addresses shared between the bootloader and the `.dts`):

| address | contents |
|---|---|
| `0x11000000` | kernel Image; link/run address = `CONFIG_PHYS_RAM_BASE` |
| `0x11700000` | PSRAM copy of the cramfs, 512 KB, exposed as `mtd0` (`root=mtd:cramfs`) |
| `0x11780000` | PSRAM copy of the DTB (passed in `a1`) |

The `memory` node claims two ranges, not one:

```dts
reg = <0x11000000 0x700000>, <0x11781000 0x7f000>;
```

7 MB below the rootfs, plus the 508 KB above the DTB slot that used to be left
unused. The second range starts 4 KB past `0x11780000` so it can never overlap the
bootloader's DTB copy however large the DTB grows. This is why the kernel reports
`Total pages: 1919` and `MemTotal 6348 kB` rather than the 5416 kB of the original
single-range layout.

The bootloader refuses to boot if the kernel would overlap the rootfs, if the
cramfs magic/length is wrong, or if anything exceeds the 8 MB device — so a
growing kernel cannot silently corrupt memory.

Why the root filesystem is copied at all: see
[Flash/PSRAM sharing one QSPI bus](#flashpsram-sharing-one-qspi-bus).

## 5. Installing picotool

**Linux** (build from source):

```bash
sudo apt update
sudo apt install build-essential cmake gcc-arm-none-eabi libnewlib-arm-none-eabi \
                 libstdc++-arm-none-eabi-newlib pkg-config libusb-1.0-0-dev
mkdir -p ~/pico && cd ~/pico
git clone https://github.com/raspberrypi/pico-sdk.git
git clone https://github.com/raspberrypi/picotool.git
git -C pico-sdk submodule update --init
export PICO_SDK_PATH=~/pico/pico-sdk          # add to ~/.bashrc to keep it
mkdir -p picotool/build && cd picotool/build
cmake .. && make -j$(nproc) && sudo make install
picotool help
```

**Windows**: grab `picotool-x.x.x-x64-win.zip` from the
[pico-sdk-tools releases](https://github.com/raspberrypi/pico-sdk-tools/releases)
page, unzip, and add the folder to `Path` (Settings → "Environment Variables" →
edit `Path` → New → OK). `picotool help` then works in any new terminal; without
the `Path` entry, open a terminal inside the extracted `picotool` folder and call
`.\picotool.exe`.

## 6. Changes on top of upstream

| file | change |
|---|---|
| `board/.../genimage.cfg` | cramfs partition moved to `offset 2M / size 512K` (upstream's 6M/2M does not fit a 4 MB part); `align = 16` on the `Image` partition so the payload lands where the bootloader expects it |
| `board/.../dts/sifive/raspberrypi-pico2.dts` | flash window 4 MB, then the `phram` root filesystem moved from flash into PSRAM (`0x11700000`, 512 KB, partition at offset 0); `memory` split into two ranges to reclaim the 508 KB above the DTB slot (§4); `plic0` moved out of the `soc` simple-bus and its bogus `reg = <0x0 0x0>` dropped — `irq-simple-intc.c` never maps registers, and a reg-less child of a simple-bus makes dtc warn |
| `board/.../raspberrypi-pico2.config` | `RISCV_ISA_ZBA/ZBB/ZBKB=n` — Hazard3 is RV32IMAC + Zicsr + Zifencei; enabling any Zb\* puts `clz/ctz/cpop` into the global `-march` and the kernel dies with an illegal instruction before the console exists. `RISCV_ISA_C=y` (C *is* real silicon support and userspace is already built `rv32imc`; every other consumer — KGDB, KPROBES, UPROBES, BPF JIT — is off and `RELOCATABLE=n`, so it only buys code density: **−440 KB of Image, −31.8% kernel text**). `IRQ_STACKS=n`, `GOLDFISH=n`, `SMP=n`, `BLOCK=n` |
| `board/.../busybox.config` | 24 applets that cannot work on this kernel disabled (no `CONFIG_NET`, no `CONFIG_BLOCK`, no `CONFIG_I2C`); busybox shrank by 94384 bytes to **429188**, 143 applets remain on the board, and cramfs headroom went from 13602 to **81920** bytes |
| `board/.../post-image.sh` | build-time guards: cramfs ≤ 512 KB, kernel ≤ 2 MB − 16 KB, and the cramfs offset / `FLASH_ROOTFS_ADDR` / all three 512 KB constants still in sync |
| `board/.../rootfs_overlay/etc/fstab` | `/tmp` and `/run` as **ramfs**, not tmpfs — `CONFIG_TMPFS` depends on SHMEM which depends on MMU. `/dev/shm` omitted entirely (`shm_open` needs shmem, not just a mount) |
| `board/.../rootfs_overlay/etc/inittab` | `mkdir -p /run/lock/subsys` now succeeds against the ramfs; neither `rcS` nor `rcK` is run, because there are no services (§9) |
| `configs/raspberrypi-pico2_defconfig` | `BR2_PACKAGE_IFUPDOWN_SCRIPTS` / `BR2_PACKAGE_URANDOM_SCRIPTS` off — they install `S40network` (needs `ifup`/`ifdown`) and `S01seedrng` (needs a persistent writable `/var/lib/seedrng`), neither of which exists here |
| `external.mk` | redefines `UCLIBC_FLOAT_CONFIG` to keep `UCLIBC_HAS_FENV` off; see the uClibc 1.0.59 gotcha in §1 |
| `board/.../patches/linux/{linux,linux-headers}.hash` | required sha256 files for the custom kernel version in an external tree |
| `board/.../patches/linux/0007-rp2350-amo-as-irq-masked-rmw.patch` | `bitops.h`, `atomic.h`, `cmpxchg.h` AMO forms → interrupt-masked read-modify-write |
| `board/.../patches/linux/0008-mask-lr-sw-rmw-sequences.patch` | same interrupt mask around the bare `lw/sw` sequences that upstream `0003` introduced |
| `board/.../patches/linux/0009-rp2350-futex-amo-as-irq-masked-rmw.patch` | the last 5 AMOs, in `futex_wake_op()` — `vmlinux` now contains **zero** AMO/lr/sc |
| `board/.../patches/linux/0010-rp2350-harden-amo-trap-emulation.patch` | the trap emulator read the instruction word and the AMO target with plain dereferences, so an unmapped `epc` or `rs1` faulted a second time inside the handler and panicked. Both now go through `copy_{from,to}_kernel_nofault()` and return 0 on failure, letting `fixup_exception()` report `-EFAULT`. Also drops the bogus `badaddr += 4` |
| `board/.../patches/linux/0012-rp2350-reset-via-watchdog.patch` | real `machine_restart()` via the watchdog/PSM — **hardware-verified**, `reboot` produces a fresh bootloader banner (§8) |
| `board/.../patches/linux/0013-rp2350-fix-malformed-Kconfig-entry.patch` | fixes a malformed Kconfig entry in the RP2350 SoC support |
| `package/.../bootloader/Makefile` | `truncate -s %16` after objcopy, so the appended payload is aligned |
| `package/.../bootloader/src/rp2350-crt.S` | fixed the BSS clear / `.data` copy loops (the source pointer was passed as zero and the loop bound was wrong) |
| `package/.../bootloader/src/qmi.c` | PSRAM write-enable bit fixed (a bit mask was used as a register index); `RXDELAY` 1 → 2 to match pico-sdk at 150 MHz; new `flash_serial_mode()` (see below) |
| `package/.../bootloader/src/main.c` | cache off during the copy, `flash_serial_mode()`, verified chunked copy through the uncached XIP mirrors, DTB/rootfs/kernel copies with size and overlap checks, `LED_PIN` for this board |

## 7. Why it is laid out like this

### Atomics

From the RP2350 datasheet (MCAUSE code 7, store/AMO access fault):

> A store/AMO failed a PMP check, or encountered a downstream bus error. Also set
> if an AMO is attempted on a region that does not support atomics (on RP2350,
> anything but SRAM).

The kernel executes from PSRAM, so every `lr/sc` and every `amo*` against its own
data faults. Upstream answers with `0002` (emulate atomics in the trap handler)
and `0003` (rewrite `lr/sc` as plain `lw/sw`). This fork adds `0007`, `0008` and
`0009`: all turn the read-modify-write into an `arch_local_irq_save()`-protected
sequence, which keeps it uninterruptible (a trap is uninterruptible by
construction; a bare `lw/sw` pair is not) while removing the trap cost —
`vmlinux` goes from 816 AMO instructions to 5 (all in `futex_wake_op()`, fixed by
`0009`) to **zero**. Everything here still assumes **one in-order core**; that is
the real precondition of the whole port.

`0010` hardens the emulator that remains for anything not rewritten: it used to
dereference `regs->epc` and the AMO target directly, so a bad address faulted a
second time *inside* the trap handler and panicked. Both reads/writes now go
through `copy_{from,to}_kernel_nofault()` and return 0 on failure, so
`do_trap_error()` falls through to `fixup_exception()` and the instruction's own
exception-table entry reports `-EFAULT`.

**Userspace must not use the A extension.** Two deliberate choices combine here:
the toolchain's default arch is `rv32imc_zicsr_zifencei_zmmul_zca` — no `a` — so
userspace atomics go through libatomic's plain read-modify-write; and
`do_trap_error()` only calls the emulator in its *kernel-mode* branch, so a user
AMO is never emulated and is delivered as `SIGSEGV`. This was confirmed on the
board with a hand-built bFLT containing `amoadd.w`/`amoswap.w` aimed at PSRAM
(`0x116f6050`): cause 7, code `0x2`, `unhandled signal 11`, the process died and
**the kernel survived** — the shell kept running and dmesg showed no oops. Do not
add `-march=rv32imac` to userspace builds. Note also that `SC.W` in the emulator
always reports success: there is no reservation model, so even in-kernel
compare-and-swap semantics must not be relied upon.

### Flash/PSRAM sharing one QSPI bus

`M0_TIMING.MAX_SELECT` resets to 0, which lets the QMI hold CS0 asserted
indefinitely while the flash is in continuous-read mode — and CS0/CS1 share SCK
and the data pins. Every PSRAM transfer therefore clocks a still-selected flash
until its read stream desynchronises, after which flash reads return zeros; that
is what an unexplained `Copy attempt N mismatch` looked like. The bootloader now
resets the flash (`0xFF`, then `0x66`/`0x99` through QMI direct mode) and
reconfigures window 0 for plain single-SPI `03h` reads that deassert CS0 after
each transfer. Copies then run through the *uncached* XIP mirrors (`0x14…`/`0x15…`)
with a priming read, two agreeing source reads per chunk, a read-back of every
destination chunk and a whole-region checksum; the XIP cache is re-enabled only
for the kernel, which then never touches flash at all because its root filesystem
lives in PSRAM.

### Debug leftovers

`main.c` still installs a tiny M-mode trap handler before the jump that prints
`!c<mcause> e<mepc> v<mtval> i<instruction>` over the PL011; the kernel replaces
it as soon as it sets its own vector. It has already localised a store fault to
`set_cpu_online()` and a garbage `gp`. To turn such an `mepc` back into source,
use the snippet in [Appendix A](#appendix-a-helper-snippets).

## 8. Measured numbers

Same benchmark on the board (`dd if=/dev/zero bs=1k count=400 | wc -c`, timed by
the kernel's own `/proc/uptime`):

| stage of this port | boot to `/sbin/init` | 400 KB pipe | throughput |
|---|---|---|---|
| XIP cache left off the whole time | 9.7 s | 8.42 s | 47 KB/s |
| re-enable the cache before the jump + copy the real cramfs length | 1.40 s | 1.03 s | 390 KB/s |
| + `0007` (AMOs without traps) | 1.22 s | 0.97 s | 404 KB/s |
| + `0008` (mask the bare `lw/sw` sequences) | 1.24 s | 0.97 s | 404 KB/s |

Those are historical pipe benchmarks. The current kernel (`0009`/`0010` +
`RISCV_ISA_C=y`) reaches `/sbin/init` at **0.977 s**; the raw PSRAM/ramfs
throughput measured on COM32 is in the acceptance table below.

### Build sizes (Buildroot 2026.11-git, kernel 6.15.0 `#1`)

| artifact | bytes | note |
|---|---|---|
| `Image` | 1288492 | was 1739052 before `RISCV_ISA_C=y` |
| `vmlinux` text / data / bss | 978454 / 298944 / 41837 | text was 1434880 (−31.8%) |
| `rootfs.cramfs` | 442368 | of a 524288-byte partition → 81920 bytes headroom |
| `bootloader.bin` | 7568 | |
| `raspberrypi-pico2.dtb` | 1989 | zero dtc warnings |
| `flash-image.bin` / `.uf2` | 2539520 / 5079040 | of 4194304 flash bytes; 1654784 free at the tail |
| `/bin/busybox` | 429188 | md5 `8d5884415a26dab1283281b06cf39164`, 143 applets |

The kernel is copied as 1334536 bytes, not 1288492: the RISC-V image header's
`image_size` is `_end - _start`, which includes BSS. The extra 46044 bytes are
harmless — the kernel zeroes its own BSS — and the copy still ends at `0x11145D08`,
far below the `0x11700000` rootfs guard.

### Acceptance run on the board (COM32, 115200 8N1)

| check | result |
|---|---|
| flash + boot | UF2 copied to the BOOTSEL volume; bootloader copies DTB 1989 / rootfs 442368 / kernel 1334536, all `ok`; reaches `~ #` |
| compressed kernel | `RISCV_ISA_C=y` image boots: `Machine model: Raspberry Pi RP2350 (Hazard3 RISC-V, nommu)`, `riscv: base ISA extensions aim` |
| memory reclaim | both ranges recognised, `Total pages: 1919` (+127 pages = +508 KB), `MemTotal 6348 kB` (was 5416) |
| kernel footprint | `Memory: 6064K/7676K available (885K kernel code, 129K rwdata, 144K rodata, 94K init, 43K bss, 1420K reserved)` |
| rootfs integrity | on-board `md5sum /bin/busybox` = `8d5884415a26dab1283281b06cf39164`, `ls -l` = 429188 — byte-identical to the build; all 24 disabled applets gone (`ifconfig`, `wget`, … → `applet not found`) |
| MTD | `mtd0: 00080000 00001000 "cramfs"` (512 KB) |
| writable mounts | `ramfs on /tmp (mode=1777)`, `ramfs on /run`, `/run/lock/subsys` present; `Shmem 0 kB` (no SHMEM, as expected without an MMU) |
| **`reboot`** | **real watchdog reset**: `reboot: Restarting system` → `RP2350 Bootloader starting...` → PSRAM re-init → full second boot to `~ #` (init at 0.974 s). Patch `0012` hardware-proven |
| clean shutdown | after disabling `IFUPDOWN_SCRIPTS`/`URANDOM_SCRIPTS` and dropping the `rcK` line, `/etc/init.d` holds only `S11modules rcK rcS` and the shutdown log has **zero** hits for `fail/error/warn/panic/oops/allocation failure/errno/applet not found` (§9) |
| PSRAM throughput | 1 MiB write to ramfs 0.96 s ≈ **1.04 MiB/s**; 4 × 1 MiB read 2.02 s ≈ **1.98 MiB/s** (`/proc/uptime`, 0.01 s resolution) |
| atomics | `vmlinux` contains 0 AMO/lr/sc; a forced user-space `amoadd.w` to PSRAM gives `signal 11 code 0x2 at 0x116f6050` and kills only that process (§7) |
| stability | 455 s uptime, `MemFree 3784 kB`, interrupts `clint-timer 113816` / `uart-pl011 250`; full dmesg scan matches **only** the one expected AMO signal line — no panic, oops, BUG, WARNING or Call Trace; no leak after `rm` |
| idle fragmentation | `/proc/buddyinfo` orders 0–10 = `0 0 1 4 6 5 4 1 1 0 0` — one 512 KB and one 1 MB block free, enough for a bFLT load (§9) |

`poweroff` was deliberately not exercised: `default_power_off()` halts with IRQs
masked and recovery needs a physical power cycle.

## 9. Known limitations

* **Nothing is writable.** Root is read-only cramfs copied into PSRAM, so every
  power cycle reverts; Linux cannot reach the flash at all, because QMI direct
  mode blocks *all* memory-mapped transfers and the kernel fetches its own
  instructions from that same bus. A real update path needs a driver whose
  erase/program routine runs from on-chip SRAM with PSRAM access quiesced, plus a
  spare partition and jffs2/overlay. Until then, re-flash from the host with
  `picotool`.
* **`reboot` works; `poweroff` is a one-way halt.** Patch `0012` gives
  `machine_restart()` a real watchdog/PSM reset, verified on the board (§8).
  `default_power_off()` now masks IRQs before halting so the timer and UART do not
  keep running and look like a crash — but there is no power controller to wake
  from, so recovery is a physical power cycle.
* **nommu + bFLT means every process needs one contiguous ~444 KB block.**
  `binfmt_flat` loads busybox's whole text+data into a single physically
  contiguous allocation (429188 bytes → order-7, 512 KB). There is no MMU and
  `CONFIG_COMPACTION` depends on one, so fragmentation never heals. Observed on
  the board: with `MemFree 3056 kB` but no order-7 block left, `ls` failed with
  `page allocation failure: order:7` and
  `binfmt_flat: Unable to allocate RAM for process text/data, errno -12` — i.e.
  *new processes can fail with ENOMEM while 3 MB is still free*. At idle the
  allocator does have a 512 KB and a 1 MB block (§8), so this only bites after
  heavy churn. Keep allocations small and short-lived; ramfs has no size limit
  and there is no swap, so filling `/tmp` can OOM the machine.
* **Userspace must not use the A extension** — the toolchain is built without `a`
  and the trap emulator is kernel-mode-only, so a user AMO to PSRAM is a
  `SIGSEGV`. See §7 for the on-board evidence.
* **No services, so neither `rcS` nor `rcK` is run.** `S40network` and
  `S01seedrng` are not even installed (`BR2_PACKAGE_IFUPDOWN_SCRIPTS` /
  `BR2_PACKAGE_URANDOM_SCRIPTS` off): they need `ifup`/`ifdown` (no `CONFIG_NET`)
  and a persistent writable `/var/lib/seedrng` (read-only cramfs). `rcS`, `rcK`
  and `S11modules` still ship because `BR2_INIT_BUSYBOX` mandatorily selects
  `BR2_PACKAGE_INITSCRIPTS`; leaving them uninvoked is deliberate. Running `rcK`
  at shutdown used to produce `Stopping network: ifdown: applet not found / FAIL`,
  `seedrng: ... Read-only file system`, and the `S11modules` ENOMEM above; with
  both changes the verified shutdown is now just
  `umount: devtmpfs busy - remounted read-only` (a normal fallback), the
  SIGTERM/SIGKILL notices and `reboot: Restarting system` — a scan of the whole
  log for `fail|error|warn|panic|oops|allocation failure|errno|applet not found`
  returns zero hits.
* Long pasted input can overrun the UART (§3).
* No network, no RTC (`date` starts at 1970), no `/proc/cpuisa`, and a few busybox
  applets (`free`, `uptime`, `ifconfig`, `wget`) are not present. `dd` cannot
  report throughput either — `CONFIG_FEATURE_DD_THIRD_STATUS_LINE` is off in
  Buildroot's minimal busybox config; bracket it with `cat /proc/uptime` instead.
* `base_baud = 0` in the ttyAMA0/ttyAMA1 probe lines is expected (§3), not a fault.
* PSRAM timing (`CLKDIV=2` → 75 MHz SCK, `RXDELAY=2`, `MAX_SELECT=16`) is
  covered by the copy's read-back verification, not by long soak testing.
* The copy is deliberately paranoid (two source reads plus a read-back), which
  costs about a second of boot.

## 10. Other boards

RP2350 RISC-V cores only. For another board, change `RP2350_XIP_CSI_PIN` (PSRAM
chip select, GPIO19 here) and `LED_PIN` in
`package/pico2-bootloader/bootloader/src/main.c`, plus the QSPI sizes in
`board/raspberrypi/raspberrypi-pico2/genimage.cfg`, the `.dts` and the kernel
`PHYS_RAM_BASE`/`memory` node in that directory. PSRAM parts exercised so far:
`APS6404L` (upstream) and `LSP-PSRAM64H` (this board, ID `0x5d 0x46`).

## Appendix A: helper snippets

Flash-and-capture over WSL interop (defaults point at one Windows machine, all
overridable). Save as `tools/flash_capture.sh` or paste into a shell:

```sh
#!/bin/sh
# Wait for a board in BOOTSEL, start a serial capture, flash the UF2, reboot, dump the log.
PICOTOOL=${PICOTOOL:-/mnt/c/Users/qitas/Downloads/picotool/picotool.exe}
CAPTURE_PS1=${CAPTURE_PS1:-C:\\Users\\qitas\\Downloads\\cap_live.ps1}
POWERSHELL=${POWERSHELL:-/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe}
UF2=${UF2:-C:\\Users\\qitas\\Downloads\\flash-image.uf2}
CAP=${CAP:-/tmp/final_cap.txt}
SECS=${CAPTURE_SECS:-240}

found=0; i=0
while [ $i -lt ${WAIT_ITERS:-150} ]; do
  if $PICOTOOL info 2>&1 | grep -q RP2350; then found=1; break; fi
  sleep 2; i=$((i+1))
done
echo "DEVICE_FOUND=$found (waited $((i*2))s)"
[ "$found" = "1" ] || { echo "NO_DEVICE"; exit 0; }
sleep 2
rm -f $CAP
timeout $SECS "$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$CAPTURE_PS1" > $CAP 2>&1 &
sleep 3
$PICOTOOL load -fu "$UF2" 2>&1 | tr '\r' '\n' | grep -avE "^\s*$" | tail -1
sleep 2
$PICOTOOL reboot 2>&1 | head -1
echo "=== flashing done, capturing ${SECS}s ==="
sleep $((SECS - 10))
echo "=== CAPTURE ==="
tr -d '\r' < $CAP
```

The companion `cap_live.ps1` (Windows side) is just a read loop on the COM port:

```powershell
$sp = New-Object System.IO.Ports.SerialPort("COM32",115200,'None',8,'One')
$sp.ReadTimeout = 300; $sp.Open()
while ($true) { try { [Console]::Out.Write([char]$sp.ReadByte()); [Console]::Out.Flush() } catch [System.TimeoutException] {} }
```

Map a trap's `mepc` back to file:line (needs `CONFIG_DEBUG_INFO=y`, which this
kernel has):

```sh
#!/bin/sh
# loc.sh <hex-addr>
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KT=${KERNEL_TREE:-$ROOT/buildroot/output/build/linux-6.15}
CROSS=${CROSS_COMPILE:-$ROOT/buildroot/output/host/bin/riscv32-buildroot-linux-uclibc-}
A=$(echo "$1" | sed 's/^0x//')
awk -v a="$A" 'BEGIN{n=strtonum("0x" a)} {if ($1 <= sprintf("%08x", n)) {sym=$3; addr=$1}}
     END{printf "closest symbol: %s at %s\n", sym, addr}' "$KT/System.map"
"${CROSS}addr2line" -f -e "$KT/vmlinux" 0x$A
"${CROSS}objdump" -d --start-address=$((0x$A - 16)) --stop-address=$((0x$A + 8)) "$KT/vmlinux" | tail -8
```

## Sources of the documentation above

The original Chinese guides (project compile/flash, picotool install) and their
English `.docx` versions are folded into §1–§3 and §5. Where they disagreed with
this tree the corrections are inline: there is no `make -C psram-bootloader
flash-kernel` target here, `BR2_EXTERNAL` must be passed again on a fresh output
directory, and the submodule to initialise is `buildroot`, not `pico-sdk`. The
hardware facts in §4 come from the board schematic named there.

![Image of boot](images/booting.png)
