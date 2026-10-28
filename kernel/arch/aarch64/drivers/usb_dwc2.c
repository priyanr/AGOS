#include <drivers/usb.h>
#include <hal/aarch64/arm.h>
#include <hal/aarch64/mailbox.h>
#include <hal/timer.h>
#include <kernel/printk.h>
#include <kernel/string.h>

// USB host for the Raspberry Pi 3: Synopsys DesignWare OTG core (DWC2) in host mode with its
// internal DMA, polled (no interrupts). The Pi 3B's only root device is the LAN9514 hub
// (Ethernet on port 1, the four USB sockets on ports 2-5), so a keyboard is a low/full-speed
// device behind a high-speed hub and every transfer to it is a split transaction.
//
// One channel (0) does every transfer, one packet at a time; callers are serialised by
// hal_poll_input() and boot-time init. Register sequence follows Circle/USPi (rsta2).

#define USB_BASE      0x3F980000
#define GOTGCTL       (USB_BASE + 0x000)
#define GAHBCFG       (USB_BASE + 0x008)
#define GUSBCFG       (USB_BASE + 0x00C)
#define GRSTCTL       (USB_BASE + 0x010)
#define GINTSTS       (USB_BASE + 0x014)
#define GRXFSIZ       (USB_BASE + 0x024)
#define GNPTXFSIZ     (USB_BASE + 0x028)
#define GSNPSID       (USB_BASE + 0x040)
#define GHWCFG2       (USB_BASE + 0x048)
#define HPTXFSIZ      (USB_BASE + 0x100)
#define HCFG          (USB_BASE + 0x400)
#define HFNUM         (USB_BASE + 0x408)
#define HPRT          (USB_BASE + 0x440)
#define PCGCCTL       (USB_BASE + 0xE00)
#define HCCHAR0       (USB_BASE + 0x500)
#define HCSPLT0       (USB_BASE + 0x504)
#define HCINT0        (USB_BASE + 0x508)
#define HCINTMSK0     (USB_BASE + 0x50C)
#define HCTSIZ0       (USB_BASE + 0x510)
#define HCDMA0        (USB_BASE + 0x514)

#define GAHBCFG_DMA_ENABLE      (1 << 5)
#define GAHBCFG_WAIT_AXI_WRITES (1 << 4) // BCM2835-specific
#define GUSBCFG_PHYIF           (1 << 3)
#define GUSBCFG_ULPI_UTMI_SEL   (1 << 4)
#define GUSBCFG_SRP_CAPABLE     (1 << 8)
#define GUSBCFG_HNP_CAPABLE     (1 << 9)
#define GUSBCFG_ULPI_FSLS       (1 << 17)
#define GUSBCFG_ULPI_CLK_SUS_M  (1 << 19)
#define GUSBCFG_ULPI_EXT_VBUS   (1 << 20)
#define GUSBCFG_TERM_SEL_DL     (1 << 22)
#define GUSBCFG_FORCE_HOST      (1 << 29)
#define GUSBCFG_FORCE_DEVICE    (1 << 30)
#define GRSTCTL_CSRST           (1 << 0)
#define GRSTCTL_RXFFLSH         (1 << 4)
#define GRSTCTL_TXFFLSH         (1 << 5)
#define GRSTCTL_AHBIDLE         (1u << 31)

#define HPRT_CONN_STS   (1 << 0)
#define HPRT_ENA        (1 << 2)
#define HPRT_RST        (1 << 8)
#define HPRT_PWR        (1 << 12)
#define HPRT_W1C_MASK   0x2E // CONN_DET, ENA, ENA_CHNG, OVRCURR_CHNG: writing 1 clears/disables

#define HCCHAR_EP_IN    (1 << 15)
#define HCCHAR_LOW_SPD  (1 << 17)
#define HCCHAR_ODD_FRM  (1 << 29)
#define HCCHAR_CH_DIS   (1 << 30)
#define HCCHAR_CH_ENA   (1u << 31)
#define HCSPLT_COMPLETE (1 << 16)
#define HCSPLT_ENABLE   (1u << 31)

#define HCINT_XFER_COMPL (1 << 0)
#define HCINT_CH_HALTED  (1 << 1)
#define HCINT_STALL      (1 << 3)
#define HCINT_NAK        (1 << 4)
#define HCINT_ACK        (1 << 5)
#define HCINT_NYET       (1 << 6)
#define HCINT_ERRORS     ((1 << 2) | (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10))

#define EP_CONTROL   0
#define EP_INTERRUPT 3

#define PID_DATA0 0
#define PID_DATA1 2
#define PID_SETUP 3

#define USB_NYET     (-4) // internal: split not complete yet
#define USB_ACK      (-5) // internal: start split accepted by the hub

#define MAX_DEVICES 32 // Slots are not reused; enough for a few re-plugs
#define MAX_HUBS    4
#define MAX_PORTS   8

typedef struct {
    usb_device_t *dev;
    uint8_t ports;
    uint8_t attached; // Bit per port (bit 0 = port 1) with an enumerated device
} usb_hub_t;

static usb_device_t devices[MAX_DEVICES];
static int device_count = 0;
static uint8_t next_address = 1;
static usb_hub_t hubs[MAX_HUBS];
static int hub_count = 0;
static bool host_ready = false;
static usb_hub_t *keyboard_hub = NULL; // Where the keyboard is plugged in, for hot-plug
static uint8_t keyboard_port = 0;
static uint64_t last_scan_ms = 0;

// DMA buffers: GPU_SHARED is uncached, so the DMA engine and the CPU always agree
static uint8_t __attribute__((aligned(16))) GPU_SHARED setup_buf[8];
static uint8_t __attribute__((aligned(16))) GPU_SHARED data_buf[512];
static volatile uint32_t __attribute__((aligned(16))) GPU_SHARED usb_mbox[8];

static bool wait_reg(uintptr_t reg, uint32_t mask, uint32_t value, uint32_t timeout_ms) {
    uint64_t start = timer_get_ticks();
    while ((mmio_read32(reg) & mask) != value) {
        if (timer_get_ticks() - start > timeout_ms) return false;
    }
    return true;
}

static uint32_t microframe(void) {
    return mmio_read32(HFNUM) & 7;
}

// Run one packet on channel 0 and wait for the channel to halt
static int channel_run(uint32_t hcchar, uint32_t hcsplt, uint8_t pid, uint8_t *dma, uint16_t len,
                       uint16_t *transferred) {
    mmio_write32(HCINT0, 0xFFFFFFFF);
    mmio_write32(HCINTMSK0, 0x7FF);
    mmio_write32(HCSPLT0, hcsplt);
    mmio_write32(HCTSIZ0, (uint32_t)len | (1u << 19) | ((uint32_t)pid << 29)); // 1 packet
    mmio_write32(HCDMA0, BUS_ADDRESS(dma));
    mmio_write32(HCCHAR0, (hcchar & ~HCCHAR_CH_DIS) | HCCHAR_CH_ENA);

    uint64_t start = timer_get_ticks();
    uint32_t hcint;
    while (!((hcint = mmio_read32(HCINT0)) & HCINT_CH_HALTED)) {
        if (timer_get_ticks() - start > 50) {
            mmio_write32(HCCHAR0, mmio_read32(HCCHAR0) | HCCHAR_CH_DIS | HCCHAR_CH_ENA);
            wait_reg(HCINT0, HCINT_CH_HALTED, HCINT_CH_HALTED, 10);
            return USB_ERROR;
        }
    }

    if (hcint & HCINT_STALL) return USB_STALL;
    if (hcint & HCINT_ERRORS) return USB_ERROR;
    if (hcint & HCINT_NAK) return USB_NAK;
    if (hcint & HCINT_NYET) return USB_NYET;
    if (hcint & HCINT_XFER_COMPL) {
        if (transferred) *transferred = (uint16_t)(len - (mmio_read32(HCTSIZ0) & 0x7FFFF));
        return 0;
    }
    if (hcint & HCINT_ACK) return USB_ACK;
    return USB_ERROR;
}

// One packet to/from an endpoint, with split transactions when the device needs them.
// Returns the byte count, or USB_NAK / USB_STALL / USB_ERROR.
static int packet(usb_device_t *dev, uint8_t ep, uint8_t type, bool in, uint8_t pid, uint8_t *dma,
                  uint16_t len, uint16_t mps) {
    uint32_t hcchar = (uint32_t)(mps & 0x7FF) | ((uint32_t)(ep & 0xF) << 11) |
                      (in ? HCCHAR_EP_IN : 0) | (dev->speed == USB_SPEED_LOW ? HCCHAR_LOW_SPD : 0) |
                      ((uint32_t)type << 18) | (1u << 20) | ((uint32_t)dev->address << 22);
    uint16_t done = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        int res;
        if (!dev->split) {
            if (type == EP_INTERRUPT) {
                hcchar = (hcchar & ~HCCHAR_ODD_FRM) | ((mmio_read32(HFNUM) & 1) ? 0 : HCCHAR_ODD_FRM);
            }
            res = channel_run(hcchar, 0, pid, dma, len, &done);
            if (res == USB_NYET && !in) res = 0; // High-speed OUT: accepted, next one may NYET
            if (res == 0) return in ? done : len;
            if (res != USB_ERROR) return res;
            continue;
        }

        uint32_t hcsplt = HCSPLT_ENABLE | (3u << 14) | ((uint32_t)dev->tt_hub << 7) | dev->tt_port;
        bool periodic = (type == EP_INTERRUPT);

        // Start split. Periodic splits must start early in a frame so the complete splits
        // (two microframes later) still fall inside it.
        uint32_t next = 0;
        if (periodic) {
            next = (microframe() + 1) & 7;
            if (next >= 5) next = 0;
            uint64_t t0 = timer_get_ticks();
            while (microframe() != next && timer_get_ticks() - t0 < 5) { }
            hcchar = (hcchar & ~HCCHAR_ODD_FRM) | ((next & 1) ? HCCHAR_ODD_FRM : 0);
        }
        res = channel_run(hcchar, hcsplt, pid, dma, len, NULL);
        if (res == USB_NAK || res == USB_STALL) return res;
        if (res == USB_ERROR) continue;
        if (res != USB_ACK && res != 0) continue;

        // Complete split: the hub answers NYET until the low/full-speed transaction is done
        int tries = periodic ? 3 : 2000;
        next = (next + 2) & 7;
        while (tries-- > 0) {
            if (periodic) {
                uint64_t t0 = timer_get_ticks();
                while (microframe() != next && timer_get_ticks() - t0 < 5) { }
                hcchar = (hcchar & ~HCCHAR_ODD_FRM) | ((next & 1) ? HCCHAR_ODD_FRM : 0);
                next = (next + 1) & 7;
            }
            res = channel_run(hcchar, hcsplt | HCSPLT_COMPLETE, pid, dma, len, &done);
            if (res != USB_NYET) break;
        }
        if (res == 0) return in ? done : len;
        if (res == USB_NAK || res == USB_STALL) return res;
        // NYET to the end or an error: start the split again
    }
    return USB_ERROR;
}

// Retry a packet while the device NAKs (control transfers are not allowed to give up on NAK)
static int packet_wait(usb_device_t *dev, uint8_t ep, uint8_t type, bool in, uint8_t pid,
                       uint8_t *dma, uint16_t len, uint16_t mps) {
    uint64_t start = timer_get_ticks();
    while (1) {
        int res = packet(dev, ep, type, in, pid, dma, len, mps);
        if (res != USB_NAK) return res;
        if (timer_get_ticks() - start > 500) return USB_ERROR;
    }
}

int usb_control(usb_device_t *dev, uint8_t request_type, uint8_t request, uint16_t value,
                uint16_t index, uint16_t length, void *data) {
    if (!host_ready || length > sizeof(data_buf)) return USB_ERROR;
    uint16_t mps = dev->max_packet0 ? dev->max_packet0 : 8;
    bool in = (request_type & 0x80) != 0;

    setup_buf[0] = request_type;
    setup_buf[1] = request;
    setup_buf[2] = (uint8_t)value;
    setup_buf[3] = (uint8_t)(value >> 8);
    setup_buf[4] = (uint8_t)index;
    setup_buf[5] = (uint8_t)(index >> 8);
    setup_buf[6] = (uint8_t)length;
    setup_buf[7] = (uint8_t)(length >> 8);
    int res = packet_wait(dev, 0, EP_CONTROL, false, PID_SETUP, setup_buf, 8, mps);
    if (res < 0) return res;

    // Data stage, packet by packet, starting with DATA1
    uint16_t done = 0;
    uint8_t pid = PID_DATA1;
    if (!in && length > 0) memcpy(data_buf, data, length);
    while (done < length) {
        uint16_t chunk = (uint16_t)((length - done) < mps ? (length - done) : mps);
        res = packet_wait(dev, 0, EP_CONTROL, in, pid, data_buf + done, chunk, mps);
        if (res < 0) return res;
        done += (uint16_t)res;
        pid = (pid == PID_DATA1) ? PID_DATA0 : PID_DATA1;
        if (in && res < chunk) break; // Short packet ends the data stage
    }
    if (in && done > 0) memcpy(data, data_buf, done);

    // Status stage: zero-length DATA1 in the opposite direction
    res = packet_wait(dev, 0, EP_CONTROL, !in || length == 0, PID_DATA1, data_buf, 0, mps);
    if (res < 0) return res;
    return done;
}

int usb_interrupt_in(usb_device_t *dev, uint8_t endpoint, uint16_t max_packet, uint8_t *toggle,
                     void *data, uint16_t length) {
    if (!host_ready || length > max_packet || length > sizeof(data_buf)) return USB_ERROR;
    int res = packet(dev, endpoint, EP_INTERRUPT, true, *toggle ? PID_DATA1 : PID_DATA0,
                     data_buf, length, max_packet);
    if (res >= 0) {
        *toggle ^= 1;
        memcpy(data, data_buf, (size_t)res);
    }
    return res;
}

static void hub_init(usb_device_t *dev, int depth);

// Give a newly reset device an address, read its descriptors, configure it and hand it to the
// hub or keyboard driver. parent is NULL for the root port.
static bool enumerate(usb_device_t *parent, uint8_t port, uint8_t speed, int depth) {
    if (device_count >= MAX_DEVICES || next_address > 127) return false;
    usb_device_t *dev = &devices[device_count];
    memset(dev, 0, sizeof(*dev));
    dev->speed = speed;
    dev->max_packet0 = 8;
    if (parent && speed != USB_SPEED_HIGH) {
        if (parent->speed == USB_SPEED_HIGH) {
            dev->split = true;
            dev->tt_hub = parent->address;
            dev->tt_port = port;
        } else if (parent->split) {
            dev->split = true; // Behind a full-speed hub: same transaction translator
            dev->tt_hub = parent->tt_hub;
            dev->tt_port = parent->tt_port;
        }
    }

    uint8_t desc[18];
    if (usb_control(dev, 0x80, 6, 0x0100, 0, 8, desc) < 8) {
        printk("USB: no device descriptor (port %d)\n", port);
        return false;
    }
    dev->max_packet0 = desc[7] ? desc[7] : 8;

    uint8_t address = next_address++;
    if (usb_control(dev, 0x00, 5, address, 0, 0, NULL) < 0) {
        printk("USB: SET_ADDRESS failed (port %d)\n", port);
        return false;
    }
    timer_sleep_ms(10);
    dev->address = address;
    device_count++;

    if (usb_control(dev, 0x80, 6, 0x0100, 0, 18, desc) < 18) return false;
    uint8_t dev_class = desc[4];
    uint16_t vid = (uint16_t)(desc[8] | (desc[9] << 8));
    uint16_t pid = (uint16_t)(desc[10] | (desc[11] << 8));

    uint8_t config[256];
    if (usb_control(dev, 0x80, 6, 0x0200, 0, 9, config) < 9) return false;
    uint16_t total = (uint16_t)(config[2] | (config[3] << 8));
    if (total > sizeof(config)) total = sizeof(config);
    int got = usb_control(dev, 0x80, 6, 0x0200, 0, total, config);
    if (got < 9) return false;
    if (usb_control(dev, 0x00, 9, config[5], 0, 0, NULL) < 0) return false; // SET_CONFIGURATION

    bool is_hub = (dev_class == 9) || (got >= 18 && config[9 + 5] == 9);
    printk("USB: device %d %04x:%04x %s-speed%s\n", address, vid, pid,
           speed == USB_SPEED_HIGH ? "high" : speed == USB_SPEED_FULL ? "full" : "low",
           is_hub ? " (hub)" : "");

    if (is_hub) {
        hub_init(dev, depth + 1);
        return true;
    }
    if (usb_keyboard_probe(dev, config, (uint16_t)got)) return true;
    return true;
}

// Reset a hub port that has a device and enumerate it
static void hub_port_attach(usb_hub_t *hub, uint8_t port, int depth) {
    usb_device_t *dev = hub->dev;
    uint8_t status[4];
    if (usb_control(dev, 0xA3, 0, 0, port, 4, status) < 4) return; // GET_STATUS
    if (!(status[0] & 0x01)) return;                                 // Nothing connected

    usb_control(dev, 0x23, 3, 4, port, 0, NULL); // SET_FEATURE PORT_RESET
    uint64_t start = timer_get_ticks();
    do {
        timer_sleep_ms(10);
        if (usb_control(dev, 0xA3, 0, 0, port, 4, status) < 4) return;
    } while ((status[0] & 0x10) && timer_get_ticks() - start < 500); // PORT_RESET still set
    usb_control(dev, 0x23, 1, 20, port, 0, NULL); // CLEAR_FEATURE C_PORT_RESET
    usb_control(dev, 0x23, 1, 16, port, 0, NULL); // CLEAR_FEATURE C_PORT_CONNECTION
    if (!(status[0] & 0x02)) return;              // Not enabled
    timer_sleep_ms(20);                           // Reset recovery

    uint16_t port_status = (uint16_t)(status[0] | (status[1] << 8));
    uint8_t speed = (port_status & (1 << 9)) ? USB_SPEED_LOW :
                    (port_status & (1 << 10)) ? USB_SPEED_HIGH : USB_SPEED_FULL;
    bool had_keyboard = usb_keyboard_attached();
    // Marked attached even if enumeration fails, so a broken device is not reset every
    // second; unplugging it (connection change) makes the port eligible again
    hub->attached |= (uint8_t)(1 << (port - 1));
    enumerate(dev, port, speed, depth);
    if (!had_keyboard && usb_keyboard_attached()) {
        keyboard_hub = hub;
        keyboard_port = port;
    }
}

// Hot-plug: attach devices plugged into free ports, and free ports whose device was unplugged
static void hub_scan(usb_hub_t *hub) {
    for (uint8_t port = 1; port <= hub->ports; port++) {
        uint8_t status[4];
        if (usb_control(hub->dev, 0xA3, 0, 0, port, 4, status) < 4) continue;
        uint8_t bit = (uint8_t)(1 << (port - 1));
        if (status[2] & 0x01) { // C_PORT_CONNECTION
            usb_control(hub->dev, 0x23, 1, 16, port, 0, NULL);
            hub->attached &= (uint8_t)~bit;
        }
        if ((status[0] & 0x01) && !(hub->attached & bit)) {
            hub_port_attach(hub, port, 1);
        }
    }
}

static void hub_init(usb_device_t *dev, int depth) {
    if (depth > 4 || hub_count >= MAX_HUBS) return;
    uint8_t desc[8];
    if (usb_control(dev, 0xA0, 6, 0x2900, 0, sizeof(desc), desc) < 7) return; // Hub descriptor
    usb_hub_t *hub = &hubs[hub_count++];
    hub->dev = dev;
    hub->ports = desc[2] > MAX_PORTS ? MAX_PORTS : desc[2];
    hub->attached = 0;

    for (uint8_t port = 1; port <= hub->ports; port++) {
        usb_control(dev, 0x23, 3, 8, port, 0, NULL); // SET_FEATURE PORT_POWER
    }
    timer_sleep_ms((uint32_t)desc[5] * 2 + 200); // bPwrOn2PwrGood plus time for devices to connect

    for (uint8_t port = 1; port <= hub->ports; port++) {
        hub_port_attach(hub, port, depth);
    }
}

static bool power_on(void) {
    usb_mbox[0] = 8 * 4;
    usb_mbox[1] = 0;
    usb_mbox[2] = 0x00028001; // SET_POWER_STATE
    usb_mbox[3] = 8;
    usb_mbox[4] = 0;
    usb_mbox[5] = 3;          // Device: USB HCD
    usb_mbox[6] = 3;          // On, wait until stable
    usb_mbox[7] = 0;
    return mbox_call(MBOX_CH_PROP, usb_mbox) && (usb_mbox[6] & 1);
}

static bool core_init(void) {
    if ((mmio_read32(GSNPSID) & 0xFFFFF000) != 0x4F542000) {
        printk("USB: no DWC2 controller (id 0x%08x)\n", mmio_read32(GSNPSID));
        return false;
    }
    if (!power_on()) {
        printk("USB: power-on failed\n");
        return false;
    }

    mmio_write32(GAHBCFG, mmio_read32(GAHBCFG) & ~1u); // Global interrupts off: we poll
    mmio_write32(GUSBCFG, mmio_read32(GUSBCFG) & ~(GUSBCFG_ULPI_EXT_VBUS | GUSBCFG_TERM_SEL_DL));

    // Core soft reset
    if (!wait_reg(GRSTCTL, GRSTCTL_AHBIDLE, GRSTCTL_AHBIDLE, 100)) return false;
    mmio_write32(GRSTCTL, GRSTCTL_CSRST);
    if (!wait_reg(GRSTCTL, GRSTCTL_CSRST, 0, 100)) return false;
    timer_sleep_ms(100);

    // UTMI+ 8-bit PHY; the dedicated FS PHY settings only apply to a ULPI high-speed PHY
    uint32_t usbcfg = mmio_read32(GUSBCFG) & ~(GUSBCFG_ULPI_UTMI_SEL | GUSBCFG_PHYIF);
    uint32_t hwcfg2 = mmio_read32(GHWCFG2);
    if (((hwcfg2 >> 6) & 3) == 2 && ((hwcfg2 >> 8) & 3) == 1) {
        usbcfg |= GUSBCFG_ULPI_FSLS | GUSBCFG_ULPI_CLK_SUS_M;
    } else {
        usbcfg &= ~(GUSBCFG_ULPI_FSLS | GUSBCFG_ULPI_CLK_SUS_M);
    }
    usbcfg &= ~(GUSBCFG_HNP_CAPABLE | GUSBCFG_SRP_CAPABLE | GUSBCFG_FORCE_DEVICE);
    usbcfg |= GUSBCFG_FORCE_HOST;
    mmio_write32(GUSBCFG, usbcfg);
    timer_sleep_ms(50); // Mode change takes up to 25 ms

    uint32_t ahbcfg = mmio_read32(GAHBCFG);
    ahbcfg |= GAHBCFG_DMA_ENABLE | GAHBCFG_WAIT_AXI_WRITES;
    ahbcfg &= ~(3u << 1); // Max AXI burst
    mmio_write32(GAHBCFG, ahbcfg);
    mmio_write32(GINTSTS, 0xFFFFFFFF);

    // Host mode: PHY clock 30/60 MHz, FIFO split (in 32-bit words), flush FIFOs
    mmio_write32(PCGCCTL, 0);
    mmio_write32(HCFG, mmio_read32(HCFG) & ~3u);
    mmio_write32(GRXFSIZ, 1024);
    mmio_write32(GNPTXFSIZ, (1024u << 16) | 1024);
    mmio_write32(HPTXFSIZ, (1024u << 16) | 2048);
    mmio_write32(GRSTCTL, (0x10u << 6) | GRSTCTL_TXFFLSH);
    wait_reg(GRSTCTL, GRSTCTL_TXFFLSH, 0, 100);
    mmio_write32(GRSTCTL, GRSTCTL_RXFFLSH);
    wait_reg(GRSTCTL, GRSTCTL_RXFFLSH, 0, 100);

    uint32_t hprt = mmio_read32(HPRT);
    if (!(hprt & HPRT_PWR)) {
        mmio_write32(HPRT, (hprt & ~HPRT_W1C_MASK) | HPRT_PWR);
    }
    return true;
}

void usb_init(void) {
    if (!core_init()) return;

    // Root port: wait for the device (LAN9514 hub on a Pi 3B), reset it
    if (!wait_reg(HPRT, HPRT_CONN_STS, HPRT_CONN_STS, 1000)) {
        printk("USB: nothing on the root port\n");
        return;
    }
    timer_sleep_ms(100);
    mmio_write32(HPRT, (mmio_read32(HPRT) & ~HPRT_W1C_MASK) | HPRT_RST);
    timer_sleep_ms(50);
    mmio_write32(HPRT, mmio_read32(HPRT) & ~(HPRT_W1C_MASK | HPRT_RST));
    timer_sleep_ms(20);
    if (!wait_reg(HPRT, HPRT_ENA, HPRT_ENA, 200)) {
        printk("USB: root port did not enable (HPRT 0x%08x)\n", mmio_read32(HPRT));
        return;
    }

    host_ready = true;
    uint8_t speed = (uint8_t)((mmio_read32(HPRT) >> 17) & 3);
    printk("USB: DWC2 host ready, root port %s-speed\n",
           speed == USB_SPEED_HIGH ? "high" : speed == USB_SPEED_FULL ? "full" : "low");
    enumerate(NULL, 0, speed, 0);
    if (!usb_keyboard_attached()) {
        printk("USB: no keyboard found (plug one in any time)\n");
    }
    last_scan_ms = timer_get_ticks();
}

void usb_poll(void) {
    if (!host_ready) return;

    if (usb_keyboard_attached()) {
        if (usb_keyboard_poll()) return;
        // Keyboard stopped answering: forget it so its port is scanned again
        printk("USB: keyboard removed\n");
        usb_keyboard_detach();
        if (keyboard_hub) keyboard_hub->attached &= (uint8_t)~(1 << (keyboard_port - 1));
        keyboard_hub = NULL;
    }

    // Hot-plug: look for a newly connected device once a second while there is no keyboard
    uint64_t now = timer_get_ticks();
    if (now - last_scan_ms < 1000) return;
    last_scan_ms = now;
    for (int h = 0; h < hub_count; h++) {
        hub_scan(&hubs[h]);
    }
}
