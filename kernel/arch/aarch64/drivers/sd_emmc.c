#include <drivers/block.h>
#include <hal/aarch64/arm.h>
#include <hal/aarch64/mailbox.h>
#include <hal/timer.h>
#include <kernel/printk.h>
#include <kernel/string.h>

// SD card on the Raspberry Pi 3 through the Arasan SDHCI ("EMMC") controller, polled PIO,
// 1-bit bus. The firmware loads the kernel through the other controller (SDHOST, GPIO 48-53
// ALT0); we route those pins to the Arasan controller (ALT3) instead, as bare-metal Pi code
// usually does. Registered as block device "mmc"; partition_scan_all() adds mmc1, mmc2...

#define EMMC_BASE        0x3F300000
#define EMMC_BLKSIZECNT  (EMMC_BASE + 0x04)
#define EMMC_ARG1        (EMMC_BASE + 0x08)
#define EMMC_CMDTM       (EMMC_BASE + 0x0C)
#define EMMC_RESP0       (EMMC_BASE + 0x10)
#define EMMC_RESP1       (EMMC_BASE + 0x14)
#define EMMC_RESP2       (EMMC_BASE + 0x18)
#define EMMC_RESP3       (EMMC_BASE + 0x1C)
#define EMMC_DATA        (EMMC_BASE + 0x20)
#define EMMC_STATUS      (EMMC_BASE + 0x24)
#define EMMC_CONTROL0    (EMMC_BASE + 0x28)
#define EMMC_CONTROL1    (EMMC_BASE + 0x2C)
#define EMMC_INTERRUPT   (EMMC_BASE + 0x30)
#define EMMC_INT_MASK    (EMMC_BASE + 0x34)
#define EMMC_INT_EN      (EMMC_BASE + 0x38)
#define EMMC_SLOTISR_VER (EMMC_BASE + 0xFC)

#define GPIO_BASE        0x3F200000
#define GPFSEL3          (GPIO_BASE + 0x0C)
#define GPFSEL4          (GPIO_BASE + 0x10)
#define GPFSEL5          (GPIO_BASE + 0x14)
#define GPPUD            (GPIO_BASE + 0x94)
#define GPPUDCLK1        (GPIO_BASE + 0x9C)

#define SR_CMD_INHIBIT   (1 << 0)
#define SR_DAT_INHIBIT   (1 << 1)

#define INT_CMD_DONE     (1 << 0)
#define INT_DATA_DONE    (1 << 1)
#define INT_WRITE_RDY    (1 << 4)
#define INT_READ_RDY     (1 << 5)
#define INT_ERROR_MASK   0x017E8000

#define C1_CLK_INTLEN    (1 << 0)
#define C1_CLK_STABLE    (1 << 1)
#define C1_CLK_EN        (1 << 2)
#define C1_TOUNIT_MAX    (0xE << 16)
#define C1_SRST_HC       (1 << 24)
#define C1_SRST_CMD      (1 << 25)
#define C1_SRST_DATA     (1 << 26)

// CMDTM: index << 24 | response type | data present (1 << 21) | read (1 << 4)
#define RESP_NONE        0x00000000
#define RESP_136         0x00010000
#define RESP_48          0x00020000
#define RESP_48_BUSY     0x00030000
#define CMD_DATA         0x00200000
#define CMD_READ         0x00000010

#define CMD_GO_IDLE      ((0u << 24) | RESP_NONE)
#define CMD_ALL_SEND_CID ((2u << 24) | RESP_136)
#define CMD_SEND_RCA     ((3u << 24) | RESP_48)
#define CMD_SELECT       ((7u << 24) | RESP_48_BUSY)
#define CMD_SEND_IF_COND ((8u << 24) | RESP_48)
#define CMD_SEND_CSD     ((9u << 24) | RESP_136)
#define CMD_SET_BLOCKLEN ((16u << 24) | RESP_48)
#define CMD_READ_SINGLE  ((17u << 24) | RESP_48 | CMD_DATA | CMD_READ)
#define CMD_WRITE_SINGLE ((24u << 24) | RESP_48 | CMD_DATA)
#define CMD_APP_CMD      ((55u << 24) | RESP_48)
#define ACMD_SEND_OP_COND ((41u << 24) | RESP_48)

static uint32_t base_clock_hz = 0;
static uint32_t rca = 0;
static bool high_capacity = false; // SDHC/SDXC: block addresses; SDSC: byte addresses
static block_device_t sd_device;
static volatile uint32_t __attribute__((aligned(16))) GPU_SHARED sd_mbox[8];

static bool wait_ms(uintptr_t reg, uint32_t mask, uint32_t value, uint32_t timeout_ms) {
    uint64_t start = timer_get_ticks();
    while ((mmio_read32(reg) & mask) != value) {
        if (timer_get_ticks() - start > timeout_ms) return false;
    }
    return true;
}

// After an error the command and data state machines must be reset
static void reset_lines(void) {
    mmio_write32(EMMC_CONTROL1, mmio_read32(EMMC_CONTROL1) | C1_SRST_CMD | C1_SRST_DATA);
    wait_ms(EMMC_CONTROL1, C1_SRST_CMD | C1_SRST_DATA, 0, 100);
    mmio_write32(EMMC_INTERRUPT, 0xFFFFFFFF);
}

static bool sd_cmd(uint32_t cmd, uint32_t arg) {
    if (!wait_ms(EMMC_STATUS, SR_CMD_INHIBIT, 0, 100)) {
        reset_lines();
        return false;
    }
    mmio_write32(EMMC_INTERRUPT, mmio_read32(EMMC_INTERRUPT));
    mmio_write32(EMMC_ARG1, arg);
    mmio_write32(EMMC_CMDTM, cmd);

    uint64_t start = timer_get_ticks();
    uint32_t irq;
    while (!((irq = mmio_read32(EMMC_INTERRUPT)) & (INT_CMD_DONE | INT_ERROR_MASK))) {
        if (timer_get_ticks() - start > 500) {
            reset_lines();
            return false;
        }
    }
    if (irq & INT_ERROR_MASK) {
        reset_lines();
        return false;
    }
    mmio_write32(EMMC_INTERRUPT, INT_CMD_DONE);
    return true;
}

static bool sd_app_cmd(uint32_t cmd, uint32_t arg) {
    return sd_cmd(CMD_APP_CMD, rca) && sd_cmd(cmd, arg);
}

static bool wait_data_irq(uint32_t mask) {
    uint64_t start = timer_get_ticks();
    uint32_t irq;
    while (!((irq = mmio_read32(EMMC_INTERRUPT)) & (mask | INT_ERROR_MASK))) {
        if (timer_get_ticks() - start > 1000) {
            reset_lines();
            return false;
        }
    }
    if (irq & INT_ERROR_MASK) {
        reset_lines();
        return false;
    }
    mmio_write32(EMMC_INTERRUPT, mask);
    return true;
}

// SDHCI v3 10-bit divided clock: SD clock = base / (2 * divisor), rounded down to <= hz
static bool set_clock(uint32_t hz) {
    if (!wait_ms(EMMC_STATUS, SR_CMD_INHIBIT | SR_DAT_INHIBIT, 0, 100)) return false;
    mmio_write32(EMMC_CONTROL1, mmio_read32(EMMC_CONTROL1) & ~C1_CLK_EN);
    timer_sleep_ms(2);

    uint32_t div = (base_clock_hz + 2 * hz - 1) / (2 * hz);
    if (div > 0x3FF) div = 0x3FF;
    uint32_t c1 = mmio_read32(EMMC_CONTROL1) & 0xFFFF003F;
    c1 |= ((div & 0xFF) << 8) | (((div >> 8) & 0x3) << 6);
    mmio_write32(EMMC_CONTROL1, c1);
    timer_sleep_ms(2);
    mmio_write32(EMMC_CONTROL1, mmio_read32(EMMC_CONTROL1) | C1_CLK_EN);
    return wait_ms(EMMC_CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE, 100);
}

static uint32_t emmc_base_clock(void) {
    sd_mbox[0] = 8 * 4;
    sd_mbox[1] = 0;
    sd_mbox[2] = 0x00030002; // GET_CLOCK_RATE
    sd_mbox[3] = 8;
    sd_mbox[4] = 0;
    sd_mbox[5] = 1;          // EMMC clock
    sd_mbox[6] = 0;
    sd_mbox[7] = 0;
    if (mbox_call(MBOX_CH_PROP, sd_mbox) && sd_mbox[6] != 0) return sd_mbox[6];
    return 50000000;
}

static void gpio_setup(void) {
    // GPIO 34-39 (WiFi SDIO, also wired to this controller) to inputs; keep 30-33 (Bluetooth)
    mmio_write32(GPFSEL3, mmio_read32(GPFSEL3) & ~0x3FFFF000u);
    // GPIO 48 (CLK), 49 (CMD) and 50-53 (DAT0-3) to ALT3
    mmio_write32(GPFSEL4, mmio_read32(GPFSEL4) | (7u << 24) | (7u << 27));
    mmio_write32(GPFSEL5, mmio_read32(GPFSEL5) | 0xFFF);
    // Pull-ups on CMD and DAT0-3
    mmio_write32(GPPUD, 2);
    arm_delay(150);
    mmio_write32(GPPUDCLK1, (1 << 17) | (1 << 18) | (1 << 19) | (1 << 20) | (1 << 21));
    arm_delay(150);
    mmio_write32(GPPUD, 0);
    mmio_write32(GPPUDCLK1, 0);
}

// Card size from the CSD register (R2 response: CSD bits 127..8 in RESP3..RESP0)
static uint64_t csd_sector_count(void) {
    uint32_t r1 = mmio_read32(EMMC_RESP1);
    uint32_t r2 = mmio_read32(EMMC_RESP2);
    uint32_t r3 = mmio_read32(EMMC_RESP3);
    uint32_t structure = (r3 >> 22) & 3;
    if (structure == 1) {                       // CSD 2.0 (SDHC/SDXC)
        uint32_t c_size = (r1 >> 8) & 0x3FFFFF; // CSD[69:48]
        return (uint64_t)(c_size + 1) * 1024;   // (C_SIZE + 1) * 512 KB
    }
    uint32_t c_size = ((r1 >> 22) & 0x3FF) | ((r2 & 0x3) << 10); // CSD[73:62]
    uint32_t mult = (r1 >> 7) & 0x7;                              // CSD[49:47]
    uint32_t read_bl_len = (r2 >> 8) & 0xF;                       // CSD[83:80]
    uint64_t bytes = (uint64_t)(c_size + 1) << (mult + 2 + read_bl_len);
    return bytes / 512;
}

static int sd_read_sectors(block_device_t *dev, uint64_t lba, uint32_t count, void *buffer) {
    (void)dev;
    uint8_t *out = (uint8_t *)buffer;
    for (uint32_t s = 0; s < count; s++) {
        if (!wait_ms(EMMC_STATUS, SR_DAT_INHIBIT, 0, 500)) {
            reset_lines();
            return -1;
        }
        mmio_write32(EMMC_BLKSIZECNT, (1u << 16) | 512);
        uint64_t sector = lba + s;
        if (!sd_cmd(CMD_READ_SINGLE, (uint32_t)(high_capacity ? sector : sector * 512))) return -1;
        if (!wait_data_irq(INT_READ_RDY)) return -1;
        for (int i = 0; i < 128; i++) {
            uint32_t w = mmio_read32(EMMC_DATA);
            out[0] = (uint8_t)w; // Byte stores: the caller's buffer may be unaligned
            out[1] = (uint8_t)(w >> 8);
            out[2] = (uint8_t)(w >> 16);
            out[3] = (uint8_t)(w >> 24);
            out += 4;
        }
        if (!wait_data_irq(INT_DATA_DONE)) return -1;
    }
    return 0;
}

static int sd_write_sectors(block_device_t *dev, uint64_t lba, uint32_t count, const void *buffer) {
    (void)dev;
    const uint8_t *in = (const uint8_t *)buffer;
    for (uint32_t s = 0; s < count; s++) {
        if (!wait_ms(EMMC_STATUS, SR_DAT_INHIBIT, 0, 500)) {
            reset_lines();
            return -1;
        }
        mmio_write32(EMMC_BLKSIZECNT, (1u << 16) | 512);
        uint64_t sector = lba + s;
        if (!sd_cmd(CMD_WRITE_SINGLE, (uint32_t)(high_capacity ? sector : sector * 512))) return -1;
        if (!wait_data_irq(INT_WRITE_RDY)) return -1;
        for (int i = 0; i < 128; i++) {
            uint32_t w = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
                         ((uint32_t)in[3] << 24);
            mmio_write32(EMMC_DATA, w);
            in += 4;
        }
        if (!wait_data_irq(INT_DATA_DONE)) return -1;
    }
    return 0;
}

void sd_init(void) {
    gpio_setup();
    base_clock_hz = emmc_base_clock();

    // Reset the host controller, then internal clock on with the longest data timeout
    mmio_write32(EMMC_CONTROL0, 0);
    mmio_write32(EMMC_CONTROL1, mmio_read32(EMMC_CONTROL1) | C1_SRST_HC);
    if (!wait_ms(EMMC_CONTROL1, C1_SRST_HC, 0, 100)) {
        printk("SD: controller reset timed out\n");
        return;
    }
    mmio_write32(EMMC_CONTROL1, mmio_read32(EMMC_CONTROL1) | C1_CLK_INTLEN | C1_TOUNIT_MAX);
    timer_sleep_ms(10);
    if (!set_clock(400000)) {
        printk("SD: clock did not stabilise\n");
        return;
    }
    mmio_write32(EMMC_INT_EN, 0xFFFFFFFF);
    mmio_write32(EMMC_INT_MASK, 0xFFFFFFFF);

    // Identification: reset, voltage check (SD 2.0+), wait for power-up, get the address
    rca = 0;
    sd_cmd(CMD_GO_IDLE, 0);
    timer_sleep_ms(2);
    bool v2 = sd_cmd(CMD_SEND_IF_COND, 0x1AA) && (mmio_read32(EMMC_RESP0) & 0xFFF) == 0x1AA;

    uint32_t ocr = 0;
    uint64_t start = timer_get_ticks();
    while (1) {
        if (sd_app_cmd(ACMD_SEND_OP_COND, v2 ? 0x51FF8000 : 0x00FF8000)) {
            ocr = mmio_read32(EMMC_RESP0);
            if (ocr & 0x80000000) break; // Power-up done
        }
        if (timer_get_ticks() - start > 1500) {
            printk("SD: no card (ACMD41 timed out)\n");
            return;
        }
        timer_sleep_ms(10);
    }
    high_capacity = (ocr & 0x40000000) != 0;

    if (!sd_cmd(CMD_ALL_SEND_CID, 0) || !sd_cmd(CMD_SEND_RCA, 0)) {
        printk("SD: card did not give an address\n");
        return;
    }
    rca = mmio_read32(EMMC_RESP0) & 0xFFFF0000;

    uint64_t sectors = 0;
    if (sd_cmd(CMD_SEND_CSD, rca)) sectors = csd_sector_count();

    if (!set_clock(25000000) || !sd_cmd(CMD_SELECT, rca)) {
        printk("SD: card select failed\n");
        return;
    }
    if (!high_capacity) sd_cmd(CMD_SET_BLOCKLEN, 512);

    strcpy(sd_device.name, "mmc");
    sd_device.sector_count = sectors;
    sd_device.sector_size = 512;
    sd_device.read_sectors = sd_read_sectors;
    sd_device.write_sectors = sd_write_sectors;
    sd_device.priv = NULL;
    block_register_device(&sd_device);
    printk("SD: %s card online as mmc: %u MB\n", high_capacity ? "SDHC/SDXC" : "SDSC",
           (unsigned int)(sectors / 2048));
}
