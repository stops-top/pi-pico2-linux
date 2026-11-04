# Raspberry Pi Pico 2 / RP2350A Buildroot — Linux on 8 MB of PSRAM

Linux (RISC-V, rv32 nommu, M-mode) built with Buildroot for the RP2350 RISC-V
cores. The kernel and a cramfs root filesystem are copied into external QSPI
PSRAM by a small bootloader and executed there.

This fork is brought up on a custom **RP2350A minimal board** (USB-C, 4 MB QSPI
flash on CS0, 8 MB PSRAM on CS1) and boots to a interactive shell:

```
RP2350 Bootloader starting...
PSRAM ID: 5d 46
PSRAM setup complete. PSRAM size 0x800000 (8388608)
PSRAM executes at 0x117ffffc
Copying DTB (1837 bytes) to 0x11780000... ok
Copying rootfs (479232 bytes) to 0x11700000... ok
Copying kernel (1793288 bytes) to 0x11000000... ok
Jumping to kernel at 0x11000000, DT at 0x11780000
[    0.000000] Linux version 6.15.0 ... riscv32
[    1.213891] VFS: Mounted root (cramfs filesystem) readonly on device 31:0.
[    1.240067] Run /sbin/init as init process
~ #
```

`MemTotal` is ~5.4 MB, boot-to-shell is ~1.2 s. See
[PORTING-NOTES.md](PORTING-NOTES.md) for the layout, per-file rationale and
measured performance history.

Upstream project: <https://github.com/Mr-Bossman/pi-pico2-linux>
(designed for the [SparkFun Pro Micro RP2350](https://www.sparkfun.com/products/24870),
which has an `APS6404L` PSRAM; see *Other boards* below).

## 1. Build

```bash
git clone --recursive https://github.com/stops-top/pi-pico2-linux
cd pi-pico2-linux
git submodule update --init           # buildroot is a submodule (pico-sdk is not needed)

make -C buildroot BR2_EXTERNAL=$PWD/ raspberrypi-pico2_defconfig
make -C buildroot BR2_EXTERNAL=$PWD/
```

The last step prints the exact flashing command, and puts the artifacts in
`buildroot/output/images/`:

| file | contents |
|---|---|
| `flash-image.uf2` | what you flash to the board (5 MB UF2, 2.5 MB image) |
| `flash-image.bin` | raw flash image, same content, for `picotool load -fu` |
| `bootloader.bin`, `Image`, `raspberrypi-pico2.dtb`, `rootfs.cramfs` | the individual pieces |

Build gotchas worth knowing:

* **`BR2_EXTERNAL` on every `make`** — the upstream README shows a bare
  `make -C buildroot`; pass `BR2_EXTERNAL=$PWD/` again if the output directory
  is not the one that was configured.
* **Building as root** — Buildroot's host-tar refuses to configure as root; use
  `FORCE_UNSAFE_CONFIGURE=1 make -C buildroot ...`.
* **Downloads** — the 6.15 kernel tarball is fetched from kernel.org, which can
  stall. Either pre-place it in `buildroot/dl/linux/` (a mirror such as
  `https://mirrors.tuna.tsinghua.edu.cn/kernel/` works), or copy a populated
  `buildroot/dl/` from an earlier build. The sha256 that Buildroot expects lives
  in `board/raspberrypi/raspberrypi-pico2/patches/linux/linux.hash` (external
  trees must provide their own hash file for custom package versions).
* **Rebuilding one piece** — Buildroot does not notice edits inside
  `package/pico2-bootloader/` or the `.dts`; use
  `make -C buildroot BR2_EXTERNAL=$PWD/ pico2-bootloader-rebuild all` and
  `... linux-rebuild all` respectively (`linux-reconfigure` also re-applies the
  kernel config fragment).

## 2. Flash

Put the board in BOOTSEL (hold `BOOTSEL` while plugging in, or `picotool reboot`
if the running image supports it), then:

```bash
picotool load -fu buildroot/output/images/flash-image.uf2   # -f force, -u reboot into the app
picotool info                    # only shows devices sitting in BOOTSEL
```

Windows drag-and-drop of the `.uf2` works too (the file is a valid UF2 for the
`rp2350-riscv` family). To go back to MicroPython or any other image, erase
first — the bootloader leaves no USB stack behind, and a foreign image cannot be
overwritten reliably otherwise:

```bash
sudo picotool erase
```

## 3. Serial console

The kernel console is **PL011 UART0** on the RP2350, 115200 8N1:

| USB-UART adapter | RP2350A |
|---|---|
| RX (adapter receives) | GPIO0 (board TX) |
| TX (adapter sends) | GPIO1 (board RX) |
| GND | GND |

Open a terminal at 115200 8N1 (`~ #` after boot). Wiring is the same as
described in the original guide: adapter `RX` → Pico 2 `GPIO0`, adapter `TX` →
`GPIO1`. The baud rate is programmed by the bootloader; the SBSA PL011 driver
deliberately does not touch the divisor (hence `base_baud = 0` in the log, which
is expected, not an error).

`tools/flash_capture.sh` automates flash + log capture over WSL interop:

```sh
WAIT_ITERS=400 CAPTURE_SECS=200 sh tools/flash_capture.sh   # waits for BOOTSEL, flashes, captures the port
PICOTOOL=... UF2='C:\path\flash-image.uf2' CAPTURE_PS1=... sh tools/flash_capture.sh
```

## 4. Hardware of this board (from `SCH_Schemati_RP2350A Minimal`)

* RP2350A running on the RISC-V cores, 12 MHz crystal, USB-C (5.1 kΩ CC
  resistors), `BOOT`/`RUN` buttons.
* QSPI CS0: serial flash (image is laid out for 4 MB — adjust
  `board/raspberrypi/raspberrypi-pico2/genimage.cfg` and the `.dts` for a
  different size).
* QSPI CS1 (GPIO19): 8 MB PSRAM, marking `LSP-PSRAM64H`; the QSPI device ID read
  is `0x5d` (known-good-die byte) with size byte `0x46`, which the bootloader
  maps to 8 MB.
* Status LED on **GPIO25** (`LED_PIN` in
  `package/pico2-bootloader/bootloader/src/main.c`), power LED on the 3V3 rail.

Runtime memory map (also in `PORTING-NOTES.md`):

| address | contents |
|---|---|
| `0x10000000` | flash XIP: picobin + bootloader + DTB + kernel, cramfs at +2 MB |
| `0x11000000` | kernel Image (link/run address = `CONFIG_PHYS_RAM_BASE`), RAM claimed up to `0x11700000` |
| `0x11700000` | PSRAM copy of the cramfs, exposed to Linux as `mtd0` (`root=mtd:cramfs`) |
| `0x11780000` | PSRAM copy of the DTB (passed in `a1`) |

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
export PICO_SDK_PATH=~/pico/pico-sdk          # put it in ~/.bashrc to keep it
mkdir -p picotool/build && cd picotool/build
cmake .. && make -j$(nproc) && sudo make install
picotool help
```

**Windows**: download `picotool-x.x.x-x64-win.zip` from the
[pico-sdk-tools releases](https://github.com/raspberrypi/pico-sdk-tools/releases)
page, unzip it, and add the folder to `Path` (Settings → "Environment Variables"
→ edit `Path` → New → OK). Then `picotool help` works in any new terminal;
without the `Path` entry, open a terminal in the extracted `picotool` folder and
call `.\picotool.exe`.

## 6. Why it is laid out like this

### Atomics

From the RP2350 datasheet (MCAUSE, code 7 — Store/AMO access fault):

> A store/AMO failed a PMP check, or encountered a downstream bus error. Also
> set if an AMO is attempted on a region that does not support atomics (on
> RP2350, anything but SRAM).

The kernel runs from PSRAM, so every `lr/sc` and every `amo*` against it faults.
Upstream handled that with two patches: `0002` emulates the faulting atomics in
the trap handler and `0003` rewrites the `lr/sc` sequences as plain `lw/sw`.
This fork adds two more: `0007` turns the AMO forms into an
`arch_local_irq_save()`-protected read-modify-write (816 AMOs in `vmlinux` down
to 5, all in `futex_wake_op()`), and `0008` puts the same interrupt mask around
the bare `lw/sw` sequences left by `0003`. Both keep the RMW uninterruptible,
which is what makes this safe on a single-core part.

### Flash/PSRAM sharing one QSPI bus

`M0_TIMING.MAX_SELECT` resets to 0, which lets the QMI hold CS0 asserted
indefinitely while the flash is in continuous-read mode. Because CS0 and CS1
share SCK and the data pins, every PSRAM transfer clocks a still-selected flash
and its read stream desynchronises — afterwards flash reads return zeros, which
is exactly what an unexplained `Copy attempt N mismatch` looked like. The
bootloader now resets the flash (`0xFF`, `0x66`/`0x99` via QMI direct mode) and
reconfigures window 0 for plain single-SPI `03h` reads that deassert CS0 after
each transfer, copies through the uncached XIP mirrors with a priming read, and
verifies every chunk. It also keeps the root filesystem in PSRAM so the running
kernel never reads flash at all.

### Debugging leftovers

`package/pico2-bootloader/bootloader/src/main.c` still installs a small M-mode
trap handler before jumping (`!c<mcause> e<mepc> v<mtval> i<instruction>` over the
PL011). It is replaced as soon as the kernel installs its own vector, and has
already caught a store fault at `set_cpu_online()` and a bad `gp`. `tools/loc.sh
<addr>` maps such an `mepc` back to symbol, file:line and disassembly.

## 7. Known limitations

* **No writable storage.** The root is read-only cramfs copied into PSRAM, so a
  power cycle reverts everything; Linux cannot reach the flash at all
  (QSPI direct mode blocks all memory-mapped access, and the kernel executes
  from PSRAM). See `PORTING-NOTES.md` §6 for the design that would fix it.
* **`reboot`/`poweroff` hang.** `machine_restart()` is
  `do_kernel_restart(); while (1);` and there is no restart handler, so the
  board must be power-cycled (`picotool` also cannot reach it in application
  mode). Tracked as a follow-up.
* Long lines pasted into the console can still produce
  `ttyAMA0: input overrun` (interrupt-driven RX, no flow control).
* No network, no RTC; `date` starts at 1970. A few busybox applets
  (`free`, `uptime`) are not enabled.
* PSRAM timing (`CLKDIV=2`, i.e. 75 MHz SCK, `RXDELAY=2`) is validated only by
  the copy's read-back checks, not by long soak testing.

## 8. Other boards

This only works on the RP2350 RISC-V cores. For a different board, change
`RP2350_XIP_CSI_PIN` (PSRAM chip select, currently GPIO19) and `LED_PIN` in
`package/pico2-bootloader/bootloader/src/main.c`, the QSPI sizes in
`board/raspberrypi/raspberrypi-pico2/genimage.cfg` plus the `.dts`/`.config` in
the same directory. The PSRAM chips exercised so far are `APS6404L` (upstream)
and `LSP-PSRAM64H` (this board, detected as ID `0x5d 0x46`).

![Image of boot](images/booting.png)

## 9. Sources

* `docs/rp2350a-minimal-schematic.pdf` (originally
  `SCH_Schemati_RP2350A Minimal_2026-08-26.pdf`) — the board schematic used for
  §4: PSRAM on QSPI CS1/GPIO19, LED on GPIO25, USB-C with 5.1 kΩ CC resistors,
  `BOOT`/`RUN` buttons, 12 MHz crystal.
* The original compile/flash guide and the picotool installation guide (Chinese
  PDFs plus their English `.docx` sources) are fully folded into §1–§3 and §5
  above; where they disagreed with this tree the corrections are noted inline
  (there is no `make -C psram-bootloader flash-kernel` target here, and
  `BR2_EXTERNAL` has to be passed again on a fresh output directory). They are
  kept beside the working copy of this port if you need the originals.

Everything that is *not* in those guides — why CS0/CS1 interfere, the atomics
handling, the PSRAM memory map and the measured numbers — is in
[PORTING-NOTES.md](PORTING-NOTES.md).
