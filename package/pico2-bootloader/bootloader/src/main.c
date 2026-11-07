#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#include "helpers.h"
#include "qmi.h"

#include "image.h"

#ifdef PICO_SDK
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include "pico/stdlib.h"
#else
#include "rp2350-map.h"

#include "pads.h"
#include "resets.h"
#include "clocks.h"
#include "ticks.h"
#include "uart.h"
#include "gpio.h"

#include "ctype_local.h"
#include "stdio_local.h"
#include "string_local.h"
#endif

#define LED_PIN			25		/* this board wires the GPIO LED to GP25 */
#define RP2350_XIP_CSI_PIN	19

#define PSRAM_LOCATION (0x11000000U)
#define PSRAM_ROOTFS_ADDR	0x11700000U
#define PSRAM_DTB_ADDR		0x11780000U
/* Must match genimage.cfg's cramfs partition size and the DTS phram reg;
 * post-image.sh enforces this.  The DTS also declares the PSRAM above
 * 0x11781000 as system memory, so do not grow this past 0x80000 without
 * moving the DTB slot and shrinking that second memory range. */
#define ROOTFS_MAX_SIZE		0x80000U
/* genimage.cfg places the cramfs partition at flash offset 2M. */
#define FLASH_ROOTFS_ADDR	(0x10000000U + 0x200000U)
#define CRAMFS_MAGIC		0x28cd3d45U

/* Cached XIP windows (0x10/0x11..) plus 4MB select their uncached,
 * non-allocating mirrors: 0x14 (flash) and 0x15 (PSRAM). */
#define XIP_UNCACHED_OFFSET	0x04000000U

#define to_uncached(p)		((void *)(((uintptr_t)(p)) | XIP_UNCACHED_OFFSET))

/* Linker variable for the end of binary image */
#ifdef PICO_SDK
extern size_t __flash_binary_end[];
#else
extern size_t __payload_load_start[];
#endif

#define is_pow2(x) (((x) != 0) && (((x) & ((x) - 1)) == 0))
#define size_ceil(size, align)							\
	({									\
		static_assert(is_pow2(align), "Alignment not a power of 2");	\
		((size) + (align) - 1) & ~((align) - 1);			\
	})

static inline void set_uart0_pinmux(void);
static inline void set_xip_cs1_pinmux(void);

static int wait_for_input(const char *msg);
static inline const void* get_payload_start_address(void);
static int psram_setup_and_test(size_t *psram_size);
static int test_executability(void* addr);
static void jump_to_kernel(void *ram_addr, const void *data_addr);
static int copy_words(void *dst, const void *src, size_t len);

/* Flash reads and PSRAM writes must not interleave inside one burst, so stage
 * the copy through SRAM in chunks. */
#define COPY_CHUNK_WORDS	1024
static uint32_t copy_buf[COPY_CHUNK_WORDS] __attribute__((aligned(16)));
static uint32_t check_buf[COPY_CHUNK_WORDS] __attribute__((aligned(16)));

static int copy_words(void *dst, const void *src, size_t len) {
	volatile uint32_t *d = (volatile uint32_t *)dst;
	const volatile uint32_t *s = (const volatile uint32_t *)src;
	size_t words = len >> 2, done, i;
	uint32_t sum_src = 0, sum_dst = 0;

	for (done = 0; done < words; done += i) {
		size_t n = words - done < COPY_CHUNK_WORDS ? words - done : COPY_CHUNK_WORDS;
		int pass;

		/* The first CS0 transfer after a run of CS1 writes comes back as
		 * zero, so prime the bus and require two matching reads. */
		for (pass = 0; pass < 4; pass++) {
			size_t diff = 0;

			(void)s[done];
			for (i = 0; i < n; i++)
				copy_buf[i] = s[done + i];
			for (i = 0; i < n; i++) {
				check_buf[i] = s[done + i];
				if (check_buf[i] != copy_buf[i])
					diff++;
			}
			if (!diff)
				break;
		}
		if (pass == 4) {
			printf("\rflash reads unstable at word 0x%x\n", (unsigned int)done);
			return -1;
		}

		for (i = 0; i < n; i++) {
			d[done + i] = copy_buf[i];
			sum_src += copy_buf[i];
		}
		for (i = 0; i < n; i++) {
			uint32_t v = d[done + i];

			if (v != copy_buf[i]) {
				printf("\rpsram word 0x%x: wrote %08x read %08x\n",
				       (unsigned int)(done + i) * 4, copy_buf[i], v);
				return -1;
			}
			sum_dst += v;
		}
	}

	return sum_src == sum_dst ? 0 : -1;
}

/* Emits "!c<mcause> e<mepc> v<mtval> i<instruction>" over UART0, then halts.
 * Naked and register-only: the stack may be what faulted. */
static void __attribute__((naked, aligned(4))) dbg_trap(void)
{
	__asm__ volatile (
		"li   t5, 0x40070000\n"
		"li   t4, 33\n"
		"call .Ldbg_putc\n"
		"csrr t1, mcause\n"
		"li   t4, 99\n"
		"call .Ldbg_putc\n"
		"call .Ldbg_puthex\n"
		"csrr t1, mepc\n"
		"li   t4, 101\n"
		"call .Ldbg_putc\n"
		"call .Ldbg_puthex\n"
		"csrr t1, mtval\n"
		"li   t4, 118\n"
		"call .Ldbg_putc\n"
		"call .Ldbg_puthex\n"
		"csrr t3, mepc\n"
		"lw   t1, 0(t3)\n"
		"li   t4, 105\n"
		"call .Ldbg_putc\n"
		"call .Ldbg_puthex\n"
		"7: wfi\n"
		"j 7b\n"
		".Ldbg_putc:\n"
		"1: lw   t6, 24(t5)\n"
		"   andi t6, t6, 32\n"
		"   bnez t6, 1b\n"
		"   sb   t4, 0(t5)\n"
		"   ret\n"
		".Ldbg_puthex:\n"
		"   li   t2, 28\n"
		"2: srl  t3, t1, t2\n"
		"   andi t3, t3, 15\n"
		"   li   t0, 10\n"
		"   blt  t3, t0, 3f\n"
		"   addi t3, t3, 87\n"
		"   j    4f\n"
		"3: addi t3, t3, 48\n"
		"4: mv   t4, t3\n"
		"   lw   t6, 24(t5)\n"
		"   andi t6, t6, 32\n"
		"   bnez t6, 4b\n"
		"   sb   t4, 0(t5)\n"
		"   addi t2, t2, -4\n"
		"   bge  t2, x0, 2b\n"
		"   ret\n"
	);
}

int main(void) {
	int ret = 0;
	uint32_t jump_ret, data, cramfs_magic, rootfs_size;
	size_t device_tree_size, psram_size, kernel_size = 0;
	void *ram_addr = (void *)PSRAM_LOCATION;
	const void *data_addr;
	const void *kernel_src;

#ifdef PICO_SDK
	stdio_init_all();
#else
	xosc_init();

	set_reset(RESETS_PLL_USB, 0);
	set_reset(RESETS_PLL_SYS, 0);
	set_usb_pll();
	set_sys_pll();

	set_ref_clock_xosc();
	set_sys_clock_pll_sys();
	set_usb_clock_pll_usb();
	set_adc_clock_pll_usb();
	set_perif_clock_clk_sys();
	set_hstx_clock_clk_sys();

	riscv_ticks_init(CLK_REF / MHZ);

	set_reset(RESETS_UART0, 0);

	set_uart0_pinmux();

	set_config(LED_PIN, PADS_CLEAR);
	set_pinfunc(LED_PIN, GPIO_FUNC_SIO);
	uart_init();

	SIO_BASE[0x38/4] = BIT(LED_PIN); // OE_SET
	SIO_BASE[0x28/4] = BIT(LED_PIN); // OUT_XOR
#endif

	puts("\n\n\nRP2350 Bootloader starting...\n");
	xip_cache_set(0);
	ret = psram_setup_and_test(&psram_size);
	if (ret)
		goto exit;

	flash_serial_mode();

	data_addr = get_payload_start_address();
	kernel_src = NULL;
	if (!data_addr) {
		printf("Failed to get ROM address is invalid\n");
		goto exit;
	}

	device_tree_size = get_devicetree_size(data_addr);
	kernel_src = (const void *)size_ceil((size_t)data_addr + device_tree_size, 0x10);

	if (device_tree_size) {
		kernel_size = get_kernel_size(kernel_src);
		if(!kernel_size)
			printf("Kernel not found.\n");
	} else {
		printf("Device Tree not found.\n");
	}

	if (kernel_size && (PSRAM_LOCATION + kernel_size > PSRAM_ROOTFS_ADDR)) {
		printf("Kernel end 0x%08x overlaps the PSRAM rootfs at 0x%08x\n",
		       (unsigned int)(PSRAM_LOCATION + kernel_size), PSRAM_ROOTFS_ADDR);
		goto exit;
	}

	if ((device_tree_size + kernel_size + ROOTFS_MAX_SIZE) > psram_size) {
		printf("Data size 0x%04x is larger than PSRAM size 0x%04x\n",
		       (device_tree_size + kernel_size), psram_size);
		goto exit;
	}

	cramfs_magic = *(const volatile uint32_t *)to_uncached((const void *)FLASH_ROOTFS_ADDR);
	if (cramfs_magic != CRAMFS_MAGIC) {
		printf("Bad cramfs magic 0x%08x at 0x%08x\n", cramfs_magic, FLASH_ROOTFS_ADDR);
		goto exit;
	}
	rootfs_size = size_ceil(*(const volatile uint32_t *)
				to_uncached((const void *)(FLASH_ROOTFS_ADDR + 4)), 0x10);
	if (rootfs_size > ROOTFS_MAX_SIZE) {
		printf("cramfs is 0x%x bytes, partition only holds 0x%x\n",
		       rootfs_size, ROOTFS_MAX_SIZE);
		goto exit;
	}

	printf("\rCopying DTB (%u bytes) to 0x%08x...",
	       (unsigned int)device_tree_size, PSRAM_DTB_ADDR);
	if (copy_words(to_uncached((void *)PSRAM_DTB_ADDR), to_uncached(data_addr),
		       size_ceil(device_tree_size, 0x10))) {
		puts(" DTB copy failed");
		goto exit;
	}
	puts(" ok");

	printf("Copying rootfs (%u bytes) to 0x%08x...", rootfs_size, PSRAM_ROOTFS_ADDR);
	if (copy_words(to_uncached((void *)PSRAM_ROOTFS_ADDR),
		       to_uncached((void *)FLASH_ROOTFS_ADDR), rootfs_size)) {
		puts(" rootfs copy failed");
		goto exit;
	}
	puts(" ok");

	printf("Copying kernel (%u bytes) to 0x%08x...",
	       (unsigned int)kernel_size, (unsigned int)PSRAM_LOCATION);
	if (copy_words(to_uncached(ram_addr), to_uncached(kernel_src), kernel_size)) {
		puts(" kernel copy failed");
		goto exit;
	}
	puts(" ok");

	if (kernel_size)
		jump_to_kernel(ram_addr, (const void *)PSRAM_DTB_ADDR);

	ret = wait_for_input("Press y to jump to PSRAM...\r");
	if (ret == 'y' || ret == 'Y') {
		data = (uint32_t)printf;
		jump_ret = ((uint32_t (*)(void *))ram_addr)((void *)data);
		printf("Jump to PSRAM returned 0x%08x\n", jump_ret);
	}

exit:
	wait_for_input("Press any key to reset.\r");
	puts("Resetting...");

	return 0;
}

#ifndef PICO_SDK
static inline void set_uart0_pinmux(void) {
	set_config(0, PADS_CLEAR); /* TX, PICO PIN 1 */
	set_config(1, PADS_IE); /* RX, PICO PIN 2 */
	set_pinfunc(0, GPIO_FUNC_UART); /* TX, PICO PIN 1 */
	set_pinfunc(1, GPIO_FUNC_UART); /* RX, PICO PIN 2 */
}

static inline void set_xip_cs1_pinmux(void) {
	set_config(RP2350_XIP_CSI_PIN, PADS_CLEAR);
	set_pinfunc(RP2350_XIP_CSI_PIN, GPIO_FUNC_XIP_CS1);
}
#endif

static int wait_for_input(const char *msg) {
	int ret;

	while (1) {
		printf(msg);
#ifdef PICO_SDK
		ret = getchar_timeout_us(0);
		if (ret != PICO_ERROR_TIMEOUT) {
#else
		ret = uart_getc();
		if (ret != -1) {
#endif
			puts("");
			return ret;
		}
		delay(500000);
	}
}

static inline const void* get_payload_start_address(void) {
#ifdef PICO_SDK
	size_t payload_load = (size_t)&__flash_binary_end;
#else
	size_t payload_load = (size_t)&__payload_load_start;
#endif

	return (const void*)size_ceil(payload_load, 0x10);
}

static int psram_setup_and_test(size_t *psram_size) {
#ifdef PICO_SDK
	gpio_set_function(RP2350_XIP_CSI_PIN, GPIO_FUNC_XIP_CS1);
#else
	set_xip_cs1_pinmux();
#endif
	*psram_size = setup_psram();

	if (!*psram_size) {
		puts("PSRAM setup failed");
		return -1;
	}

	printf("PSRAM setup complete. PSRAM size 0x%lX (%d)", *psram_size, *psram_size);
	puts("");

	return test_executability((size_t *)(PSRAM_LOCATION + (*psram_size) - 4));
}

static int test_executability(void* addr) {
	const uint16_t ret_inst = 0x8082;
	size_t addr_aligned = ((size_t)addr) & ~1;
	volatile uint16_t* addr_ptr = (volatile uint16_t*)addr_aligned;

	mb();
	*addr_ptr = ret_inst;
	mb();

	printf("PSRAM executes at 0x%08x\n", addr_aligned);

	if (*addr_ptr != ret_inst) {
		printf("ERROR: Expected 0x%04x, got 0x%04x\n", ret_inst, *addr_ptr);
		return -1;
	}

	((void (*)(void))addr_aligned)();

	return 0;
}

static void jump_to_kernel(void *ram_addr, const void *data_addr) {
	typedef void (*image_entry_arg_t)(unsigned long hart, const void *dtb);
	image_entry_arg_t image_entry = (image_entry_arg_t)ram_addr;

	printf("Jumping to kernel at 0x%08x, DT at 0x%08x\n", ram_addr, data_addr);
	/* Nothing but the PSRAM window is touched at runtime (kernel, DTB and rootfs
	 * all live there), so no cross-device cache lines can alias; the cache has
	 * been off for the whole copy, so there are no stale or dirty lines either.
	 */
	xip_cache_set(1);
	SIO_BASE[0x28/4] = BIT(LED_PIN); /* LED on: handing over to the kernel */
	__asm__ volatile ("csrw mtvec, %0" :: "r" (dbg_trap));
	image_entry(0, data_addr);
}
