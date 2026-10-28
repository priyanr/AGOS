#include <drivers/sound.h>
#include <hal/aarch64/arm.h>
#include <hal/aarch64/mailbox.h>
#include <hal/timer.h>
#include <kernel/printk.h>
#include <kernel/string.h>

// HDMI audio for the Raspberry Pi 3 without VCHIQ.
// The firmware has already brought up HDMI video (config.txt hdmi_drive=2 = HDMI mode, not DVI).
// We feed the HDMI block's MAI audio FIFO with IEC958 subframes from a DMA channel that loops
// over one buffer forever; sound_play_tone() just rewrites that buffer. Register offsets, bits
// and the start-up order follow the Linux vc4_hdmi driver (BCM2835 HDMI).

#define HDMI_BASE                0x3F902000
#define HDMI_MAI_CHANNEL_MAP     (HDMI_BASE + 0x090)
#define HDMI_MAI_CONFIG          (HDMI_BASE + 0x094)
#define HDMI_AUDIO_PACKET_CONFIG (HDMI_BASE + 0x09C)
#define HDMI_RAM_PACKET_CONFIG   (HDMI_BASE + 0x0A0)
#define HDMI_RAM_PACKET_STATUS   (HDMI_BASE + 0x0A4)
#define HDMI_CRP_CFG             (HDMI_BASE + 0x0A8)
#define HDMI_CTS_0               (HDMI_BASE + 0x0AC)
#define HDMI_CTS_1               (HDMI_BASE + 0x0B0)
#define HDMI_RAM_PACKET(n)       (HDMI_BASE + 0x400 + (n) * 0x24)

#define HD_BASE                  0x3F808000
#define HD_MAI_CTL               (HD_BASE + 0x14)
#define HD_MAI_THR               (HD_BASE + 0x18)
#define HD_MAI_FMT               (HD_BASE + 0x1C)
#define HD_MAI_SMP               (HD_BASE + 0x2C)
#define HD_MAI_DATA_BUS_ADDR     0x7E808020

#define MAI_CTL_RESET            (1u << 0)
#define MAI_CTL_ERRORE           (1u << 1)
#define MAI_CTL_FLUSH            (1u << 2)
#define MAI_CTL_ENABLE           (1u << 3)
#define MAI_CTL_CHNUM(n)         ((uint32_t)(n) << 4)
#define MAI_CTL_DLATE            (1u << 11)
#define MAI_CTL_ERRORF           (1u << 12)
#define MAI_CTL_WHOLSMP          (1u << 14)
#define MAI_CTL_CHALIGN          (1u << 15)

#define RAM_PACKET_ENABLE        (1u << 16)
#define AUDIO_INFOFRAME_PACKET   4 // RAM packet slot = infoframe type 0x84 - 0x80

// DMA channel 5 is in the firmware's list of channels free for the ARM (mask 0x7f35)
#define DMA_CHANNEL              5
#define DMA_CS                   (0x3F007000 + DMA_CHANNEL * 0x100)
#define DMA_CONBLK_AD            (DMA_CS + 0x04)
#define DMA_SOURCE_AD            (DMA_CS + 0x0C)
#define DMA_DEBUG                (DMA_CS + 0x20)
#define DMA_ENABLE               0x3F007FF0
#define DMA_DREQ_HDMI            17

// ARM physical -> VideoCore bus address (uncached alias); the ARM caches are off (MMU off)
#define BUS_ADDRESS(p)           ((uint32_t)(uintptr_t)(p) | 0xC0000000)

// The firmware runs the HDMI state machine clock (HSM) at this rate; vc4_hdmi relies on it too
#define HSM_CLOCK_HZ             163682864u
#define SAMPLE_RATE              48000u
#define CTS_N                    6144u // HDMI spec N for 48 kHz

#define IEC958_BLOCK_FRAMES      192
#define BUFFER_FRAMES            (IEC958_BLOCK_FRAMES * 25) // 100 ms, whole blocks so the loop stays aligned
#define TONE_AMPLITUDE           6000

static uint32_t __attribute__((aligned(32))) GPU_SHARED dma_cb[8];
static uint32_t __attribute__((aligned(32))) GPU_SHARED iec_buffer[BUFFER_FRAMES * 2];

// Consumer channel status (first 5 of 24 bytes): PCM, no copyright, 48 kHz, 16-bit words
static const uint8_t channel_status[5] = { 0x04, 0x00, 0x00, 0x02, 0x02 };

static bool hdmi_audio_running = false;
static uint32_t current_frequency = 0xFFFFFFFF;
static uint32_t pixel_clock_hz = 0;
static uint32_t cts = 0;
static uint32_t smp_n = 0;
static uint32_t smp_m = 0;

static volatile uint32_t __attribute__((aligned(16))) GPU_SHARED audio_mbox[8];

static uint32_t mbox_clock(uint32_t tag, uint32_t clock_id) {
    audio_mbox[0] = 8 * 4;
    audio_mbox[1] = 0;
    audio_mbox[2] = tag;
    audio_mbox[3] = 8;
    audio_mbox[4] = 0;
    audio_mbox[5] = clock_id;
    audio_mbox[6] = 0;
    audio_mbox[7] = 0;
    if (!mbox_call(MBOX_CH_PROP, audio_mbox)) return 0;
    return audio_mbox[6];
}

static uint32_t parity32(uint32_t v) {
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

// One IEC958 subframe as the MAI expects it (same layout as ALSA's iec958 plugin):
// bits 0-3 preamble (8 = block start, 2 = left, 4 = right), 12-27 the 16-bit sample,
// 30 channel status bit, 31 even parity over bits 4-30.
static uint32_t iec958_subframe(uint32_t sample_bits, uint32_t sample_parity, unsigned frame, bool right) {
    uint32_t c = 0;
    if (frame / 8 < sizeof(channel_status)) {
        c = (channel_status[frame / 8] >> (frame % 8)) & 1;
    }
    uint32_t word = sample_bits | (c << 30) | ((sample_parity ^ c) << 31);
    if (right) {
        word |= 0x04;
    } else if (frame == 0) {
        word |= 0x08;
    } else {
        word |= 0x02;
    }
    return word;
}

// Square wave at frequency_hz (0 = silence) over the whole loop buffer
static void fill_buffer(uint32_t frequency_hz) {
    uint32_t high_bits = ((uint32_t)(uint16_t)(int16_t)TONE_AMPLITUDE) << 12;
    uint32_t low_bits = ((uint32_t)(uint16_t)(int16_t)-TONE_AMPLITUDE) << 12;
    uint32_t high_parity = parity32(high_bits);
    uint32_t low_parity = parity32(low_bits);
    uint32_t half_period = frequency_hz ? SAMPLE_RATE / (frequency_hz * 2) : 0;
    if (frequency_hz && half_period == 0) half_period = 1;

    for (unsigned i = 0; i < BUFFER_FRAMES; i++) {
        uint32_t bits = 0;
        uint32_t parity = 0;
        if (half_period) {
            bool high = ((i / half_period) & 1) == 0;
            bits = high ? high_bits : low_bits;
            parity = high ? high_parity : low_parity;
        }
        unsigned frame = i % IEC958_BLOCK_FRAMES;
        iec_buffer[2 * i] = iec958_subframe(bits, parity, frame, false);
        iec_buffer[2 * i + 1] = iec958_subframe(bits, parity, frame, true);
    }
}

// MAI sample clock: n/m ~= HSM clock / sample rate, m <= 256 (rational_best_approximation in vc4)
static void compute_mai_divider(void) {
    uint64_t best_err = (uint64_t)-1;
    for (uint32_t m = 1; m <= 256; m++) {
        uint64_t n = ((uint64_t)HSM_CLOCK_HZ * m + SAMPLE_RATE / 2) / SAMPLE_RATE;
        if (n == 0 || n > 0xFFFFFF) continue;
        uint64_t exact = (uint64_t)HSM_CLOCK_HZ * m;
        uint64_t approx = n * SAMPLE_RATE;
        uint64_t err = (exact > approx ? exact - approx : approx - exact) * 256 / m;
        if (err < best_err) {
            best_err = err;
            smp_n = (uint32_t)n;
            smp_m = m;
        }
    }
}

// The HDMI block picks up packet RAM changes once per frame, so allow up to 100 ms
static void wait_ram_packet_status(uint32_t bit, bool set) {
    uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < 100) {
        bool is_set = (mmio_read32(HDMI_RAM_PACKET_STATUS) & bit) != 0;
        if (is_set == set) return;
    }
}

// Audio InfoFrame (CEA-861): 2 channels, everything else "refer to stream header"
static void write_audio_infoframe(void) {
    uint8_t hb[3] = { 0x84, 0x01, 0x0A };
    uint8_t pb[14] = { 0 };
    pb[1] = 0x01; // CC = channel count - 1
    uint8_t sum = (uint8_t)(hb[0] + hb[1] + hb[2] + pb[1]);
    pb[0] = (uint8_t)(0x100 - sum);

    uint32_t bit = 1u << AUDIO_INFOFRAME_PACKET;
    mmio_write32(HDMI_RAM_PACKET_CONFIG, mmio_read32(HDMI_RAM_PACKET_CONFIG) & ~bit);
    wait_ram_packet_status(bit, false);

    // Packet RAM: header word, then per 7-byte subpacket one word of 4 bytes and one of 3
    uintptr_t reg = HDMI_RAM_PACKET(AUDIO_INFOFRAME_PACKET);
    mmio_write32(reg, hb[0] | (hb[1] << 8) | (hb[2] << 16));
    for (int i = 0; i < 14; i += 7) {
        reg += 4;
        mmio_write32(reg, pb[i] | (pb[i + 1] << 8) | (pb[i + 2] << 16) | ((uint32_t)pb[i + 3] << 24));
        reg += 4;
        mmio_write32(reg, pb[i + 4] | (pb[i + 5] << 8) | (pb[i + 6] << 16));
    }

    mmio_write32(HDMI_RAM_PACKET_CONFIG, mmio_read32(HDMI_RAM_PACKET_CONFIG) | bit | RAM_PACKET_ENABLE);
    wait_ram_packet_status(bit, true);
}

static void start_dma(void) {
    dma_cb[0] = (1u << 3)                    // WAIT_RESP
              | (1u << 6)                    // DEST_DREQ
              | (1u << 8)                    // SRC_INC
              | ((uint32_t)DMA_DREQ_HDMI << 16)
              | (1u << 26);                  // NO_WIDE_BURSTS
    dma_cb[1] = BUS_ADDRESS(iec_buffer);
    dma_cb[2] = HD_MAI_DATA_BUS_ADDR;
    dma_cb[3] = sizeof(iec_buffer);
    dma_cb[4] = 0;
    dma_cb[5] = BUS_ADDRESS(dma_cb);         // Loop back to ourselves forever
    dma_cb[6] = 0;
    dma_cb[7] = 0;

    mmio_write32(DMA_ENABLE, mmio_read32(DMA_ENABLE) | (1u << DMA_CHANNEL));
    mmio_write32(DMA_CS, 1u << 31);          // RESET
    arm_delay(1000);
    mmio_write32(DMA_DEBUG, 0x7);            // Clear error flags
    mmio_write32(DMA_CONBLK_AD, BUS_ADDRESS(dma_cb));
    mmio_write32(DMA_CS, (1u << 28)          // WAIT_FOR_OUTSTANDING_WRITES
                       | (15u << 20)         // PANIC_PRIORITY
                       | (8u << 16)          // PRIORITY
                       | 1u);                // ACTIVE
}

void sound_init(void) {
    if (hdmi_audio_running) {
        sound_stop();
        return;
    }
    printk("[AUDIO] Initializing HDMI audio (MAI + DMA)...\n");

    // Clock regeneration: CTS = TMDS clock * N / (128 * fs); for 48 kHz that is pixel clock / 1000
    pixel_clock_hz = mbox_clock(0x00030002, 9);          // GET_CLOCK_RATE, PIXEL
    if (pixel_clock_hz == 0) {
        pixel_clock_hz = mbox_clock(0x00030047, 9);      // GET_CLOCK_RATE_MEASURED, PIXEL
    }
    if (pixel_clock_hz == 0) {
        pixel_clock_hz = 148500000;                      // 1080p60, the usual TV mode
    }
    cts = (uint32_t)(((uint64_t)pixel_clock_hz * CTS_N) / (128ull * SAMPLE_RATE));
    compute_mai_divider();

    mmio_write32(HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH | MAI_CTL_DLATE | MAI_CTL_ERRORE | MAI_CTL_ERRORF);
    mmio_write32(HD_MAI_SMP, (smp_n << 8) | (smp_m - 1));
    mmio_write32(HD_MAI_FMT, (9u << 8)                   // Sample rate code: 48 kHz
                           | (2u << 16));                // Audio format: PCM
    mmio_write32(HD_MAI_THR, (0x08u << 24) | (0x08u << 16) | (0x06u << 8) | 0x08u);
    mmio_write32(HDMI_MAI_CONFIG, (1u << 27)             // FORMAT_REVERSE
                                | (1u << 26)             // BIT_REVERSE
                                | 0x3u);                 // Channel mask: L, R
    mmio_write32(HDMI_MAI_CHANNEL_MAP, (0u << 0) | (1u << 3));
    mmio_write32(HDMI_AUDIO_PACKET_CONFIG, (1u << 29)    // ZERO_DATA_ON_SAMPLE_FLAT
                                         | (1u << 24)    // ZERO_DATA_ON_INACTIVE_CHANNELS
                                         | (0x8u << 10)  // B frame identifier, matches preamble 8
                                         | 0x3u);        // CEA channel mask
    mmio_write32(HDMI_CRP_CFG, (1u << 24) | CTS_N);      // EXTERNAL_CTS_EN | N
    mmio_write32(HDMI_CTS_0, cts);
    mmio_write32(HDMI_CTS_1, cts);
    write_audio_infoframe();

    fill_buffer(0);
    current_frequency = 0;
    start_dma();
    mmio_write32(HD_MAI_CTL, MAI_CTL_CHNUM(2) | MAI_CTL_WHOLSMP | MAI_CTL_CHALIGN | MAI_CTL_ENABLE);
    hdmi_audio_running = true;

    printk("[AUDIO] pixel %d Hz, CTS %d, MAI SMP %d/%d, MAI_CTL 0x%08X, DMA CS 0x%08X\n",
           (int)pixel_clock_hz, (int)cts, (int)smp_n, (int)smp_m,
           (unsigned int)mmio_read32(HD_MAI_CTL), (unsigned int)mmio_read32(DMA_CS));
}

void sound_play_tone(uint32_t frequency_hz) {
    if (!hdmi_audio_running || frequency_hz == current_frequency) return;
    current_frequency = frequency_hz;
    fill_buffer(frequency_hz);
}

void sound_stop(void) {
    sound_play_tone(0);
}

void sound_play_divisor(int pit_divisor) {
    if (pit_divisor >= 3) {
        sound_play_tone(PIT_BASE_FREQUENCY / (uint32_t)pit_divisor);
    } else {
        sound_stop();
    }
}

const char *sound_status_line(void) {
    static char line[96];
    if (!hdmi_audio_running) return "HDMI audio not started";
    // DMA SOURCE_AD moving and MAI_CTL without error bits (1, 12) means samples are flowing
    snprintf(line, sizeof(line), "pix:%d cts:%d smp:%d/%d mai:%08X dma:%08X src:%08X tone:%dHz",
             (int)pixel_clock_hz, (int)cts, (int)smp_n, (int)smp_m,
             (unsigned int)mmio_read32(HD_MAI_CTL), (unsigned int)mmio_read32(DMA_CS),
             (unsigned int)mmio_read32(DMA_SOURCE_AD), (int)current_frequency);
    return line;
}
