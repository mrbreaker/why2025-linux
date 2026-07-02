// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_partition.h"
#include "hal/cache_hal.h"
#include "hal/gpio_hal.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/hw_stack_guard.h"

static const char *TAG = "boot";

#define KERNEL_LOAD_PA  0x48000000u
#define DTB_LOAD_PA     0x48800000u

/* RISC-V flat Image header magic ("RISCV\0\0\0") at byte offset 48, little-endian u64. */
#define RISCV_IMAGE_MAGIC  0x5643534952ULL

/* ------------------------------------------------------------------ */
/*
 * IDF's esp_cpu_configure_region_protection() leaves PMP entries 5 and
 * 7-10 unset. IDF's own comment admits: "No explicit permission
 * specified in PMP (default all permissions) for External RAM" — but
 * that's only true for M-mode. With no matching PMP entry, U-mode
 * access -> access fault.
 *
 * Program entry 9 (unlocked): M-mode keeps full access, U-mode gets
 * RWX on the whole PSRAM window 0x48000000..0x4C000000 (64 MB NAPOT).
 * The Linux kernel (CONFIG_RISCV_M_MODE=y) can further tighten this
 * later via its own PMP writes — entry 9 is not locked.
 */
static inline void IRAM_ATTR pmp_grant_psram_umode(void)
{
    /* NAPOT for 64 MB @ 0x48000000:
     *   pmpaddr = (0x48000000 >> 2) | ((0x04000000 >> 3) - 1) = 0x127FFFFF
     */
    const unsigned long pmpaddr9 = 0x127FFFFFUL;
    /* cfg byte: NAPOT(0x18) | X(0x04) | W(0x02) | R(0x01) = 0x1F, no L */
    const unsigned long cfg_byte = 0x1FUL;

    unsigned long cfg2;
    __asm__ volatile("csrw pmpaddr9, %0" :: "r"(pmpaddr9));
    __asm__ volatile("csrr %0, pmpcfg2"  : "=r"(cfg2));
    /* pmpcfg2 on RV32 covers entries 8..11; entry 9 = byte 1 = bits [15:8] */
    cfg2 = (cfg2 & ~(0xFFUL << 8)) | (cfg_byte << 8);
    __asm__ volatile("csrw pmpcfg2, %0" :: "r"(cfg2));
}

/* ------------------------------------------------------------------ */
/*
 * Final jump from IRAM — must not execute from PSRAM since we're about
 * to overwrite 0x48000000 with the kernel.
 *
 * RISC-V Linux boot convention (M-mode, no SBI):
 *   a0 = hartid (0)
 *   a1 = FDT physical address
 */
static void IRAM_ATTR do_boot(uint32_t entry, uint32_t dtb_pa)
{
    __asm__ volatile("csrci mstatus, 0x8");  /* disable M-mode interrupts */

    pmp_grant_psram_umode();                 /* U-mode access to PSRAM */

    __asm__ volatile("fence.i");             /* sync instruction fetch */

    __asm__ volatile(
        "li   a0, 0\n"
        "mv   a1, %1\n"
        "jr   %0\n"
        :
        : "r"(entry), "r"(dtb_pa)
        : "a0", "a1"
    );
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/*
 * Copy `size` bytes from the start of `part` (at partition-relative
 * offset 0) into PSRAM at `psram_pa`, using esp_partition_mmap.
 *
 * Reads one 64KB MMU page at a time so we never hold more than one
 * MMU entry.  mmap goes through the cache — no cache-disable, no DMA,
 * no cache-line thrashing.
 */
static esp_err_t load_partition_mmap(const esp_partition_t *part,
                                      uint32_t psram_pa, uint32_t size)
{
    uint8_t *dst = (uint8_t *)(uintptr_t)psram_pa;
    const uint32_t page = CONFIG_MMU_PAGE_SIZE;  /* 64 KB on ESP32-P4 */
    uint32_t done = 0;

    while (done < size) {
        /* Partition offset must be page-aligned for mmap */
        uint32_t part_off = done & ~(page - 1u);
        uint32_t intra    = done - part_off;
        uint32_t n        = MIN(page - intra, size - done);

        const void *mapped;
        esp_partition_mmap_handle_t h;
        esp_err_t err = esp_partition_mmap(part, part_off, page,
                                           ESP_PARTITION_MMAP_DATA,
                                           &mapped, &h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mmap part+0x%"PRIx32 " => %d", part_off, err);
            return err;
        }
        memcpy(dst + done, (const uint8_t *)mapped + intra, n);
        esp_partition_munmap(h);
        done += n;

        if ((done & 0xFFFFF) == 0 || done == size)
            ESP_LOGI(TAG, "  %"PRIu32 " / %"PRIu32 " KB",
                     done / 1024, size / 1024);
    }

    cache_hal_writeback_addr(psram_pa, size);
    return ESP_OK;
}

/*
 * Map the first page of the kernel partition, check the RISC-V Image
 * magic, and return the actual kernel size from the header.
 * Returns 0 if the header can't be mapped or the Image magic is absent
 * (the caller treats 0 as fatal and restarts, rather than jumping into
 * non-kernel flash contents).
 */
static uint32_t probe_kernel_size(const esp_partition_t *part)
{
    const void *mapped;
    esp_partition_mmap_handle_t h;
    esp_err_t err = esp_partition_mmap(part, 0, CONFIG_MMU_PAGE_SIZE,
                                       ESP_PARTITION_MMAP_DATA, &mapped, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "header mmap failed: %d", err);
        return 0;
    }

    uint64_t magic;
    memcpy(&magic, (const uint8_t *)mapped + 48, sizeof(magic));

    uint32_t size = 0;
    if (magic != RISCV_IMAGE_MAGIC) {
        ESP_LOGE(TAG, "No RISC-V Image magic (got 0x%016llx) — kernel partition is not a flat Image",
                 (unsigned long long)magic);
        size = 0;   /* fatal: app_main restarts rather than jump into garbage */
    } else {
        uint64_t image_size;
        memcpy(&image_size, (const uint8_t *)mapped + 16, sizeof(image_size));
        if (image_size == 0 || image_size > part->size) {
            ESP_LOGW(TAG, "image_size=0x%llx invalid — using partition size",
                     (unsigned long long)image_size);
            size = part->size;
        } else {
            ESP_LOGI(TAG, "Image magic OK, size=%"PRIu32" KB",
                     (uint32_t)(image_size / 1024));
            size = (uint32_t)image_size;
        }
    }

    esp_partition_munmap(h);
    return size;
}

/* ------------------------------------------------------------------ */
/*
 * Kick ESP32-C6 out of reset in app-boot mode so its stock
 * network_adapter.elf firmware starts driving the backlight PWM. Same
 * sequence as linux-native-scratch/main/main.c::kick_c6_into_run_mode().
 * Done from the boot shim (before jumping to Linux) because the C6
 * apparently depends on some part of ESP-IDF's pre-app_main state
 * (LDOs / clocks / flash bus) that the kernel boot path doesn't
 * reproduce, so a Linux-side kick at driver probe has no effect.
 */
static void boot_shim_kick_c6(void)
{
    gpio_config_t cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << 12) | (1ULL << 13),
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(13, 1);              /* C6 GPIO0 = app boot */
    gpio_set_level(12, 1);              /* idle */
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(12, 0);              /* assert reset */
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(12, 1);              /* release */
    ESP_LOGI(TAG, "C6 released from reset (shim-side kick)");
    vTaskDelay(pdMS_TO_TICKS(1000));    /* let network_adapter.elf start PWM */
}

/* ------------------------------------------------------------------ */
/*
 * SPI bring-up: nothing to do here.
 *
 * The kernel's `spi-gpio` driver toggles SCK/MOSI/CS via the
 * gpio-esp32p4 controller, and reads MISO the same way. All six lines
 * (32/33/28/31/30/29) come out of POR in GPIO-matrix function, which
 * is what we want — no peripheral signal routing needed. gpio-esp32p4's
 * `request` op programs IO_MUX MCU_SEL when each pin is claimed.
 */

/* ------------------------------------------------------------------ */
/*
 * SD card (slot 0, IOMUX) bring-up.
 *
 * The badge wires a microSD slot to the ESP32-P4 SDMMC controller's
 * dedicated IOMUX pins (GPIO 39 D0, 40 D1, 41 D2, 42 D3, 43 CLK, 44 CMD)
 * and powers the rail from the on-chip LDO channel 4. Linux's mainline
 * dw_mmc driver expects the controller already be clocked and the pins
 * already routed — there's no P4 pinctrl/clock-controller upstream — so
 * the boot shim does that work, mirroring esp-idf's sdmmc_host_init +
 * configure_pin_iomux + sd_pwr_ctrl_new_on_chip_ldo path.
 *
 * Voltage: 3.3V from rail (no UHS), enabled by setting LDO unit 3
 *   (chan_id=4) → PMU.ext_ldo[index 4] = PMU_EXT_LDO_P1_0P2A_REG @ +0x1d8:
 *   force_tieh_sel=1, tieh_sel=0, tieh=1 (rail), xpd=1.
 * Clocking: SDMMC source = PLL_F160M, host divider 4 → 40 MHz LS clock.
 *   Card divider is set later by mainline dw_mmc (it manages CLKDIV.div0
 *   from clk_set_rate). At default speed 25 MHz, dw_mmc picks card_div=1
 *   giving (40 MHz / (2*1)) = 20 MHz SD clock — under the 25 MHz cap.
 *   HS-mode (50 MHz) needs raising the host divider, requires a real
 *   clock controller.
 * IOMUX: function 0 on each pin. Datasheet labels these "SD1_*" but
 *   they're physically slot-0 (sdmmc_pins.h confirms).
 */
#define ESP32P4_PMU_BASE             0x50115000u
#define PMU_EXT_LDO_P1_0P2A          (ESP32P4_PMU_BASE + 0x1d8) /* LDO4 ctrl */
#define  PMU_LDO4_FORCE_TIEH_SEL     (1u << 7)
#define  PMU_LDO4_XPD                (1u << 8)
#define  PMU_LDO4_TIEH_SEL_MASK      (0x7u << 9)
#define  PMU_LDO4_TIEH               (1u << 14)

#define ESP32P4_HP_SYS_CLKRST_BASE   0x500E6000u
#define HP_CLKRST_SOC_CLK_CTRL1      (ESP32P4_HP_SYS_CLKRST_BASE + 0x18)
#define  SOC_CLK_CTRL1_SDMMC_SYS_CLK_EN  (1u << 14)
#define HP_CLKRST_PERI_CLK_CTRL01    (ESP32P4_HP_SYS_CLKRST_BASE + 0x34)
#define  PERI01_SDIO_HS_MODE         (1u << 22)  /* 1 = bypass divider */
#define  PERI01_SDIO_LS_CLK_SRC_SEL  (1u << 23)  /* 0 = PLL_F160M, 1 = SDIO_PLL_200M */
#define  PERI01_SDIO_LS_CLK_EN       (1u << 24)
#define HP_CLKRST_PERI_CLK_CTRL02    (ESP32P4_HP_SYS_CLKRST_BASE + 0x38)
#define  PERI02_LS_EDGE_CFG_UPDATE   (1u << 8)
#define  PERI02_LS_EDGE_L_SHIFT      9       /* 4 bits [12:9]  */
#define  PERI02_LS_EDGE_H_SHIFT      13      /* 4 bits [16:13] */
#define  PERI02_LS_EDGE_N_SHIFT      17      /* 4 bits [20:17] */
#define  PERI02_LS_SLF_EDGE_SEL_SHIFT 21     /* 2 bits */
#define  PERI02_LS_DRV_EDGE_SEL_SHIFT 23     /* 2 bits */
#define  PERI02_LS_SAM_EDGE_SEL_SHIFT 25     /* 2 bits */
#define  PERI02_LS_SLF_CLK_EN        (1u << 27)
#define  PERI02_LS_DRV_CLK_EN        (1u << 28)
#define  PERI02_LS_SAM_CLK_EN        (1u << 29)

#define ESP32P4_LP_CLKRST_BASE       0x50111000u
#define LP_CLKRST_HP_SDMMC_EMAC_RST  (ESP32P4_LP_CLKRST_BASE + 0x4c)
#define  RST_EN_SDMMC                (1u << 28)

/* The 6 dedicated IOMUX pins for SDMMC slot 0 (FUNC 0 on each). */
static const uint8_t sdmmc_iomux_pins[6] = { 39, 40, 41, 42, 43, 44 };

static inline void boot_shim_reg_setbits(uint32_t addr, uint32_t mask)
{
    volatile uint32_t *r = (volatile uint32_t *)addr;
    *r |= mask;
}

static inline void boot_shim_reg_clrsetbits(uint32_t addr, uint32_t clr, uint32_t set)
{
    volatile uint32_t *r = (volatile uint32_t *)addr;
    uint32_t v = *r;
    v = (v & ~clr) | set;
    *r = v;
}

static void boot_shim_init_sdmmc(void)
{
    /* 1. Power on LDO4 at 3.3V (rail bypass). */
    {
        uint32_t set = PMU_LDO4_XPD | PMU_LDO4_FORCE_TIEH_SEL | PMU_LDO4_TIEH;
        boot_shim_reg_clrsetbits(PMU_EXT_LDO_P1_0P2A, PMU_LDO4_TIEH_SEL_MASK, set);
        esp_rom_delay_us(1000);  /* LDO ramp */
    }

    /* 2. Enable SDMMC bus clock and pulse the module reset. */
    boot_shim_reg_setbits(HP_CLKRST_SOC_CLK_CTRL1, SOC_CLK_CTRL1_SDMMC_SYS_CLK_EN);
    boot_shim_reg_setbits(LP_CLKRST_HP_SDMMC_EMAC_RST, RST_EN_SDMMC);
    boot_shim_reg_clrsetbits(LP_CLKRST_HP_SDMMC_EMAC_RST, RST_EN_SDMMC, 0);

    /* 3. Select PLL_F160M as LS clock source, enable LS clock. */
    boot_shim_reg_clrsetbits(HP_CLKRST_PERI_CLK_CTRL01,
                             PERI01_SDIO_LS_CLK_SRC_SEL | PERI01_SDIO_HS_MODE,
                             PERI01_SDIO_LS_CLK_EN);

    /* 4. Set host divider = 4: edge_l=3, edge_n=3, edge_h=1, then update strobe.
     *    sdmmc_ll_set_clock_div: edge_h = div/2 - 1, edge_n = div - 1, edge_l = div - 1.
     *    Then init_phase_delay enables drv/sam/slf clocks at edge_sel = drv:1 sam:0 slf:0.
     *    NOTE: CTRL02 bits [31:30] are MIPI_DSI_DPHY_CLK_SRC_SEL — cannot
     *    do a full-register overwrite without breaking the display.
     */
    {
        uint32_t clr = (0xFu << PERI02_LS_EDGE_L_SHIFT)
                     | (0xFu << PERI02_LS_EDGE_H_SHIFT)
                     | (0xFu << PERI02_LS_EDGE_N_SHIFT)
                     | (0x3u << PERI02_LS_SLF_EDGE_SEL_SHIFT)
                     | (0x3u << PERI02_LS_DRV_EDGE_SEL_SHIFT)
                     | (0x3u << PERI02_LS_SAM_EDGE_SEL_SHIFT);
        uint32_t set = (3u << PERI02_LS_EDGE_L_SHIFT)
                     | (1u << PERI02_LS_EDGE_H_SHIFT)
                     | (3u << PERI02_LS_EDGE_N_SHIFT)
                     | (1u << PERI02_LS_DRV_EDGE_SEL_SHIFT)
                     | PERI02_LS_DRV_CLK_EN
                     | PERI02_LS_SAM_CLK_EN
                     | PERI02_LS_SLF_CLK_EN;
        boot_shim_reg_clrsetbits(HP_CLKRST_PERI_CLK_CTRL02, clr, set);
        boot_shim_reg_setbits(HP_CLKRST_PERI_CLK_CTRL02, PERI02_LS_EDGE_CFG_UPDATE);
        boot_shim_reg_clrsetbits(HP_CLKRST_PERI_CLK_CTRL02, PERI02_LS_EDGE_CFG_UPDATE, 0);
    }

    /* 5. Route the 6 IOMUX pins to SDMMC function 0 with internal pull-ups
     *    (badge has no external pull-ups on these lines per badgevms's
     *    SDMMC_SLOT_FLAG_INTERNAL_PULLUP), input-enable, and max drive.
     *    gpio_iomux_output() programs MCU_SEL = func.
     */
    for (size_t i = 0; i < sizeof(sdmmc_iomux_pins); i++) {
        uint8_t pin = sdmmc_iomux_pins[i];
        gpio_pulldown_dis(pin);
        gpio_pullup_en(pin);
        gpio_input_enable(pin);
        gpio_iomux_output(pin, 0);   /* FUNC_GPIO*_SD1_*_PAD = 0, all 6 pins */
        gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
    }

    esp_rom_delay_us(10);  /* let the new clock propagate before kernel takes over */

    ESP_LOGI(TAG, "SDMMC slot-0 ready: LDO4=3v3, BUS_CLK=on, LS_CLK=PLL160M/4=40MHz");
}

void app_main(void)
{
    boot_shim_kick_c6();
    boot_shim_init_sdmmc();

    ESP_LOGI(TAG, "=== ESP32-P4 native boot shim (flash mode) ===");

    const esp_partition_t *kern_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "kernel");
    if (!kern_part) {
        ESP_LOGE(TAG, "kernel partition not found");
        esp_restart();
    }

    const esp_partition_t *dtb_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "dtb");
    if (!dtb_part) {
        ESP_LOGE(TAG, "dtb partition not found");
        esp_restart();
    }

    uint32_t kern_size = probe_kernel_size(kern_part);
    if (kern_size == 0) {
        ESP_LOGE(TAG, "kernel probe failed");
        esp_restart();
    }

    ESP_LOGI(TAG, "Loading %"PRIu32" KB kernel -> PSRAM 0x%08x",
             kern_size / 1024, KERNEL_LOAD_PA);
    if (load_partition_mmap(kern_part, KERNEL_LOAD_PA, kern_size) != ESP_OK) {
        ESP_LOGE(TAG, "kernel load failed");
        esp_restart();
    }

    ESP_LOGI(TAG, "Loading %"PRIu32" KB DTB -> PSRAM 0x%08x",
             dtb_part->size / 1024, DTB_LOAD_PA);
    if (load_partition_mmap(dtb_part, DTB_LOAD_PA, dtb_part->size) != ESP_OK) {
        ESP_LOGE(TAG, "DTB load failed");
        esp_restart();
    }

    /*
     * Map the rootfs partition into the flash cache window and patch
     * the DTB with the resulting VA + size.
     *
     * The cache window mapping installed by esp_partition_mmap() persists
     * across the jump to Linux because we DO NOT munmap.  Linux's
     * physmap-core driver, bound to the "mtd-rom" compatible, then reads
     * through that VA via memcpy_fromio(), and squashfs decompresses on
     * the fly.  The DTS ships with placeholder values for the flash node:
     *
     *     flash@deadbeef {
     *         compatible = "mtd-rom";
     *         reg = <0xdeadbeef 0xcafefade>;
     *         bank-width = <4>;
     *     };
     *
     * The placeholder is 8 bytes -- the two cells of `reg` -- serialised
     * big-endian (DTB byte order) as DE AD BE EF CA FE FA DE.  Search the
     * DTB blob for that exact byte sequence and replace it with [VA, size]
     * encoded the same way.  Search bound is the DTB header's totalsize
     * field; if not found, log a warning and continue (Linux will fail to
     * mount root and panic, but we want the boot shim to still print why).
     */
    const esp_partition_t *rootfs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "rootfs");
    if (!rootfs_part) {
        ESP_LOGE(TAG, "rootfs partition not found");
        esp_restart();
    }

    const void *rootfs_va;
    esp_partition_mmap_handle_t rootfs_h;
    /* Deliberately never munmap'd: Linux reads the rootfs through this VA
     * after the jump (see the block comment above and the note below). */
    esp_err_t err = esp_partition_mmap(rootfs_part, 0, rootfs_part->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &rootfs_va, &rootfs_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rootfs mmap failed: %d", err);
        esp_restart();
    }

    uint32_t rootfs_va32 = (uint32_t)(uintptr_t)rootfs_va;
    uint32_t rootfs_size = rootfs_part->size;
    {
        const uint8_t *p = (const uint8_t *)rootfs_va;
        ESP_LOGI(TAG, "rootfs mapped @0x%08" PRIx32 " size=%" PRIu32 " KB; magic=%02x %02x %02x %02x",
                 rootfs_va32, rootfs_size / 1024, p[0], p[1], p[2], p[3]);
        if (p[0] != 'h' || p[1] != 's' || p[2] != 'q' || p[3] != 's') {
            ESP_LOGW(TAG, "rootfs partition does not start with squashfs magic");
        }
        /*
         * Two-tier prewalk:
         *  1. Coarse 64 KB stride across the whole partition warms the
         *     IDF flash MMU mappings (one read per page is enough).
         *  2. Fine 64-byte (cache-line) stride across only the LAST 64
         *     KB exercises every cache line that squashfs' near-EOF
         *     metadata blocks live in.  Empirically the squashfs read
         *     errors all clustered in the last few KB before EOF — the
         *     offset shifted by ~4 bytes across rebuilds, so it's a
         *     logical-structure thing not a physical-flash thing.
         *
         * Full-fine prewalk (every cache line of all 8 MB) eliminated
         * the squashfs errors but added ~900 ms of boot time and
         * appeared to slightly increase the residual pwm-c6-window
         * boot-freeze rate.  The two-tier approach gets the squashfs
         * win for ~1 ms of extra prewalk.
         */
        volatile uint8_t prewarm = 0;
        const uint32_t mmu_page = 64u * 1024u;
        const uint32_t cache_line = 64u;
        const uint32_t fine_window = 64u * 1024u;
        for (uint32_t off = 0; off < rootfs_size; off += mmu_page)
            prewarm += p[off];
        uint32_t fine_start = (rootfs_size > fine_window)
                             ? (rootfs_size - fine_window) : 0u;
        for (uint32_t off = fine_start; off < rootfs_size; off += cache_line)
            prewarm += p[off];
        (void)prewarm;
        ESP_LOGI(TAG, "rootfs prewalk: %"PRIu32" KB coarse + last 64 KB fine",
                 rootfs_size / 1024);
    }

    /* Patch the DTB in PSRAM. */
    {
        uint8_t *dtb = (uint8_t *)DTB_LOAD_PA;
        /* DTB header magic = 0xd00dfeed (BE) at offset 0; totalsize at offset 4. */
        if (dtb[0] != 0xd0 || dtb[1] != 0x0d || dtb[2] != 0xfe || dtb[3] != 0xed) {
            ESP_LOGE(TAG, "DTB magic not found at 0x%08x — wrong load addr?", DTB_LOAD_PA);
            esp_restart();
        }
        uint32_t dtb_total = ((uint32_t)dtb[4] << 24) | ((uint32_t)dtb[5] << 16) |
                             ((uint32_t)dtb[6] <<  8) |  (uint32_t)dtb[7];
        if (dtb_total < 16 || dtb_total > dtb_part->size) {
            ESP_LOGE(TAG, "DTB totalsize 0x%" PRIx32 " out of range", dtb_total);
            esp_restart();
        }

        static const uint8_t needle[8] = {
            0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xfa, 0xde
        };
        uint8_t replace[8] = {
            (uint8_t)(rootfs_va32 >> 24), (uint8_t)(rootfs_va32 >> 16),
            (uint8_t)(rootfs_va32 >>  8), (uint8_t)(rootfs_va32      ),
            (uint8_t)(rootfs_size >> 24), (uint8_t)(rootfs_size >> 16),
            (uint8_t)(rootfs_size >>  8), (uint8_t)(rootfs_size      ),
        };

        bool patched = false;
        for (uint32_t i = 0; i + 8 <= dtb_total; i++) {
            if (memcmp(dtb + i, needle, 8) == 0) {
                memcpy(dtb + i, replace, 8);
                ESP_LOGI(TAG, "DTB flash@... reg patched at offset 0x%" PRIx32, i);
                patched = true;
                break;
            }
        }
        if (!patched) {
            ESP_LOGW(TAG, "DTB placeholder 0xdeadbeef/0xcafefade not found — Linux will fail to mount root");
        } else {
            cache_hal_writeback_addr(DTB_LOAD_PA, dtb_total);
        }
    }
    /* INTENTIONALLY no munmap — Linux reads rootfs through this VA. */

    ESP_LOGI(TAG, "Jumping to 0x%08x ...", KERNEL_LOAD_PA);
    vTaskDelay(pdMS_TO_TICKS(20));  /* let the log flush */

    esp_hw_stack_guard_monitor_stop();  /* kernel uses its own SP, not ours */
    do_boot(KERNEL_LOAD_PA, DTB_LOAD_PA);

    ESP_LOGE(TAG, "unreachable");
    esp_restart();
}
