#include <drivers/bluetooth.h>
#include <drivers/xbox_ble.h>
#include <hal/timer.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#if defined(__aarch64__)

// Minimal BLE host for one Xbox Wireless Controller, running on top of the BCM43438 HCI
// transport (bcm43438.c). We are the central:
//   connect -> SMP legacy "Just Works" pairing -> encryption -> GATT discovery of the HID
//   Report characteristics -> enable notifications -> route reports to xbox_ble_parse_report().
// ATT MTU stays at the default 23, so every PDU fits in one 27-byte LE ACL fragment and no
// L2CAP reassembly is needed. Keys live in RAM only (re-pair after every boot).

#define L2CAP_CID_ATT       0x0004
#define L2CAP_CID_SIGNALING 0x0005
#define L2CAP_CID_SMP       0x0006

#define ATT_ERROR_RSP       0x01
#define ATT_MTU_REQ         0x02
#define ATT_MTU_RSP         0x03
#define ATT_FIND_INFO_REQ   0x04
#define ATT_FIND_INFO_RSP   0x05
#define ATT_READ_BY_TYPE_REQ 0x08
#define ATT_READ_BY_TYPE_RSP 0x09
#define ATT_WRITE_REQ       0x12
#define ATT_WRITE_RSP       0x13
#define ATT_NOTIFY          0x1B
#define ATT_INDICATE        0x1D
#define ATT_CONFIRM         0x1E

#define SMP_PAIRING_REQ     0x01
#define SMP_PAIRING_RSP     0x02
#define SMP_PAIRING_CONFIRM 0x03
#define SMP_PAIRING_RANDOM  0x04
#define SMP_PAIRING_FAILED  0x05
#define SMP_ENC_INFO        0x06
#define SMP_MASTER_IDENT    0x07
#define SMP_IDENTITY_INFO   0x08
#define SMP_IDENTITY_ADDR   0x09
#define SMP_SECURITY_REQ    0x0B

#define UUID_PRIMARY_SERVICE   0x2800
#define UUID_SECONDARY_SERVICE 0x2801
#define UUID_CHARACTERISTIC    0x2803
#define UUID_CCCD              0x2902
#define UUID_HID_REPORT        0x2A4D

#define MAX_REPORTS 8
#define STEP_TIMEOUT_MS 10000

typedef enum {
    LINK_IDLE,
    LINK_PAIRING,
    LINK_ENCRYPTING,
    LINK_DISCOVERING,
    LINK_CHARS,
    LINK_ENABLING,
    LINK_READY,
} link_state_t;

static const char *const link_state_names[] = {
    "IDLE", "PAIRING", "ENCRYPTING", "DISCOVERING", "CHARS", "ENABLING NOTIFY", "READY",
};

static link_state_t link_state = LINK_IDLE;
static uint64_t link_state_since = 0;
static bool connected = false;
static uint16_t conn_handle = 0;
static uint8_t peer_type = 0;
static uint8_t peer_addr[6];
static uint8_t own_addr[6];
static char link_error[48] = "";

// SMP pairing state
static uint8_t preq[7];
static uint8_t pres[7];
static uint8_t mrand[16];
static uint8_t sconfirm[16];
static uint8_t key_size = 16;
static bool encrypting_with_bond = false;
static bool identity_sent = false;

// Bond from the last successful pairing (RAM only)
static bool bond_valid = false;
static uint8_t bond_addr[6];
static uint8_t bond_ltk[16];
static uint8_t bond_rand[8];
static uint16_t bond_ediv = 0;

// GATT discovery results. Pass 1 (Find Information) records every attribute handle and type;
// pass 2 (Read By Type 0x2803) lists the characteristics with their properties and value handles.
#define MAX_ATTRS 64
static uint16_t attr_handles[MAX_ATTRS];
static uint16_t attr_types[MAX_ATTRS];  // 16-bit type; 0xFFFF = vendor 128-bit UUID
static int attr_count = 0;
static int chars_seen = 0;
static uint16_t report_handles[MAX_REPORTS];
static int report_count = 0;
static uint16_t cccd_handles[MAX_REPORTS];
static int cccd_count = 0;
static int cccd_next = 0;
static uint32_t reports_received = 0;
static uint32_t notifications_received = 0;
static uint16_t last_notify_handle = 0;
static uint16_t last_notify_len = 0;
static uint8_t last_notify_data[20];

// Diagnostics for the [BLE LINK] debug line
static int find_info_responses = 0;
static uint16_t last_attr_handle = 0;
static uint8_t last_att_error_req = 0;
static uint8_t last_att_error_code = 0;
static bool ltk_received = false;

static uint64_t rng_state = 0;

// ---------------------------------------------------------------------------------------------
// AES-128 (encrypt only) and the SMP legacy pairing functions e, c1, s1 (Core spec Vol 3 Part H 2.2)
// ---------------------------------------------------------------------------------------------

static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t aes_xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00));
}

// FIPS-197 AES-128, bytes in the standard (most significant first) order
static void aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    static const uint8_t rcon[10] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36 };
    uint8_t rk[176];
    memcpy(rk, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t0 = rk[i - 4], t1 = rk[i - 3], t2 = rk[i - 2], t3 = rk[i - 1];
        if (i % 16 == 0) {
            uint8_t first = t0;
            t0 = (uint8_t)(aes_sbox[t1] ^ rcon[i / 16 - 1]);
            t1 = aes_sbox[t2];
            t2 = aes_sbox[t3];
            t3 = aes_sbox[first];
        }
        rk[i]     = rk[i - 16] ^ t0;
        rk[i + 1] = rk[i - 15] ^ t1;
        rk[i + 2] = rk[i - 14] ^ t2;
        rk[i + 3] = rk[i - 13] ^ t3;
    }

    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];

    for (int round = 1; round <= 10; round++) {
        uint8_t t[16];
        // SubBytes + ShiftRows (byte i is row i%4, column i/4)
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                t[r + 4 * c] = aes_sbox[s[r + 4 * ((c + r) % 4)]];
            }
        }
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                t[4 * c]     = a0 ^ all ^ aes_xtime(a0 ^ a1);
                t[4 * c + 1] = a1 ^ all ^ aes_xtime(a1 ^ a2);
                t[4 * c + 2] = a2 ^ all ^ aes_xtime(a2 ^ a3);
                t[4 * c + 3] = a3 ^ all ^ aes_xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * round + i];
    }
    memcpy(out, s, 16);
}

// Security function e: AES-128 with key and data least significant byte first, as SMP sends them
static void smp_e(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t k[16], p[16], c[16];
    for (int i = 0; i < 16; i++) {
        k[i] = key[15 - i];
        p[i] = in[15 - i];
    }
    aes128_encrypt(k, p, c);
    for (int i = 0; i < 16; i++) out[i] = c[15 - i];
}

// Confirm value c1 = e(k, e(k, r XOR p1) XOR p2), p1 = pres||preq||rat||iat, p2 = 0||ia||ra
static void smp_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t req[7], const uint8_t rsp[7],
                   uint8_t iat, const uint8_t ia[6], uint8_t rat, const uint8_t ra[6], uint8_t out[16]) {
    uint8_t p1[16], p2[16], t[16];
    p1[0] = iat;
    p1[1] = rat;
    memcpy(&p1[2], req, 7);
    memcpy(&p1[9], rsp, 7);
    memcpy(&p2[0], ra, 6);
    memcpy(&p2[6], ia, 6);
    memset(&p2[12], 0, 4);

    for (int i = 0; i < 16; i++) t[i] = r[i] ^ p1[i];
    smp_e(k, t, t);
    for (int i = 0; i < 16; i++) t[i] ^= p2[i];
    smp_e(k, t, out);
}

// Short term key s1 = e(k, r1[0..7] || r2[0..7]) with r1 in the most significant half
static void smp_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t out[16]) {
    uint8_t r[16];
    memcpy(&r[0], r2, 8);
    memcpy(&r[8], r1, 8);
    smp_e(k, r, out);
}

// Pairing randoms only need to be unpredictable to a passive listener for Just Works; mix the
// free-running counter into a xorshift generator.
static uint8_t random_byte(void) {
    if (rng_state == 0) {
        rng_state = timer_get_ticks() * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL;
    }
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    rng_state += timer_get_ticks();
    return (uint8_t)(rng_state >> 24);
}

// ---------------------------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------------------------

static void set_state(link_state_t state) {
    link_state = state;
    link_state_since = timer_get_ticks();
}

static void set_error(const char *msg, unsigned int code) {
    snprintf(link_error, sizeof(link_error), "%s 0x%02X", msg, code);
    printk("[BLE] %s\n", link_error);
}

static void l2cap_send(uint16_t cid, const uint8_t *data, uint8_t len) {
    uint8_t pdu[64];
    if (len > sizeof(pdu) - 4) return;
    pdu[0] = len;
    pdu[1] = 0;
    pdu[2] = (uint8_t)(cid & 0xFF);
    pdu[3] = (uint8_t)(cid >> 8);
    memcpy(&pdu[4], data, len);
    bcm43438_send_acl(conn_handle, pdu, (uint16_t)(len + 4));
}

static void disconnect(uint8_t reason) {
    uint8_t param[3] = { (uint8_t)(conn_handle & 0xFF), (uint8_t)(conn_handle >> 8), reason };
    bcm43438_send_cmd(0x0406, param, 3); // HCI_Disconnect
}

static void start_encryption(const uint8_t ltk[16], const uint8_t rand[8], uint16_t ediv) {
    uint8_t param[28];
    param[0] = (uint8_t)(conn_handle & 0xFF);
    param[1] = (uint8_t)(conn_handle >> 8);
    memcpy(&param[2], rand, 8);
    param[10] = (uint8_t)(ediv & 0xFF);
    param[11] = (uint8_t)(ediv >> 8);
    memcpy(&param[12], ltk, 16);
    bcm43438_send_cmd(0x2019, param, 28); // HCI_LE_Start_Encryption
    set_state(LINK_ENCRYPTING);
}

static void start_pairing(void) {
    preq[0] = SMP_PAIRING_REQ;
    preq[1] = 0x03; // IO capability: NoInputNoOutput -> Just Works
    preq[2] = 0x00; // OOB data not present
    preq[3] = 0x01; // AuthReq: bonding, no MITM, no Secure Connections (legacy pairing)
    preq[4] = 16;   // Maximum encryption key size
    preq[5] = 0x02; // Initiator key distribution: IdKey (our IRK + identity address)
    preq[6] = 0x01; // Responder key distribution: LTK (EncKey)
    l2cap_send(L2CAP_CID_SMP, preq, 7);
    encrypting_with_bond = false;
    identity_sent = false;
    memset(pres, 0, sizeof(pres));
    set_state(LINK_PAIRING);
}

// Phase 3 key distribution, initiator side: after the responder's keys, send our identity
// (IRK + public identity address) if the controller accepted IdKey in its Pairing Response.
// Windows and Android do this; without it the controller stays in pairing mode.
static void send_identity_keys(void) {
    if (identity_sent || (pres[5] & 0x02) == 0) return;
    uint8_t info[17];
    info[0] = SMP_IDENTITY_INFO;
    for (int i = 0; i < 16; i++) info[1 + i] = random_byte(); // IRK; we never use private addresses
    l2cap_send(L2CAP_CID_SMP, info, 17);

    uint8_t addr[8];
    addr[0] = SMP_IDENTITY_ADDR;
    addr[1] = 0x00; // Public address
    memcpy(&addr[2], own_addr, 6);
    l2cap_send(L2CAP_CID_SMP, addr, 8);
    identity_sent = true;
}

static void send_find_info(uint16_t start) {
    uint8_t req[5] = { ATT_FIND_INFO_REQ, (uint8_t)(start & 0xFF), (uint8_t)(start >> 8), 0xFF, 0xFF };
    l2cap_send(L2CAP_CID_ATT, req, 5);
}

static void start_discovery(void) {
    attr_count = 0;
    chars_seen = 0;
    report_count = 0;
    cccd_count = 0;
    cccd_next = 0;
    find_info_responses = 0;
    last_attr_handle = 0;
    set_state(LINK_DISCOVERING);
    send_find_info(0x0001);
}

// Read By Type for characteristic declarations (0x2803) from start to the end of the table
static void send_read_chars(uint16_t start) {
    uint8_t req[7] = { ATT_READ_BY_TYPE_REQ, (uint8_t)(start & 0xFF), (uint8_t)(start >> 8), 0xFF, 0xFF,
                       (uint8_t)(UUID_CHARACTERISTIC & 0xFF), (uint8_t)(UUID_CHARACTERISTIC >> 8) };
    l2cap_send(L2CAP_CID_ATT, req, 7);
}

static void start_char_discovery(void) {
    set_state(LINK_CHARS);
    send_read_chars(0x0001);
}

// Enable notifications on the next HID report CCCD, or finish
static void enable_next_cccd(void) {
    if (cccd_next >= cccd_count) {
        set_state(LINK_READY);
        printk("[BLE] Xbox controller ready: %d report(s), %d notification(s) enabled\n",
               report_count, cccd_count);
        return;
    }
    uint16_t h = cccd_handles[cccd_next++];
    uint8_t req[5] = { ATT_WRITE_REQ, (uint8_t)(h & 0xFF), (uint8_t)(h >> 8), 0x01, 0x00 };
    l2cap_send(L2CAP_CID_ATT, req, 5);
}

// ---------------------------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------------------------

static void handle_smp(const uint8_t *d, uint16_t len) {
    if (len < 1) return;

    switch (d[0]) {
    case SMP_PAIRING_RSP: {
        if (len < 7 || link_state != LINK_PAIRING) return;
        memcpy(pres, d, 7);
        key_size = pres[4] < 16 ? pres[4] : 16;
        for (int i = 0; i < 16; i++) mrand[i] = random_byte();

        static const uint8_t tk[16] = { 0 }; // Just Works temporary key
        uint8_t confirm[17];
        confirm[0] = SMP_PAIRING_CONFIRM;
        smp_c1(tk, mrand, preq, pres, 0x00, own_addr, peer_type, peer_addr, &confirm[1]);
        l2cap_send(L2CAP_CID_SMP, confirm, 17);
        break;
    }
    case SMP_PAIRING_CONFIRM: {
        if (len < 17 || link_state != LINK_PAIRING) return;
        memcpy(sconfirm, &d[1], 16);
        uint8_t random[17];
        random[0] = SMP_PAIRING_RANDOM;
        memcpy(&random[1], mrand, 16);
        l2cap_send(L2CAP_CID_SMP, random, 17);
        break;
    }
    case SMP_PAIRING_RANDOM: {
        if (len < 17 || link_state != LINK_PAIRING) return;
        static const uint8_t tk[16] = { 0 };
        uint8_t srand[16], check[16], stk[16];
        memcpy(srand, &d[1], 16);
        smp_c1(tk, srand, preq, pres, 0x00, own_addr, peer_type, peer_addr, check);
        if (memcmp(check, sconfirm, 16) != 0) {
            uint8_t failed[2] = { SMP_PAIRING_FAILED, 0x04 }; // Confirm Value Failed
            l2cap_send(L2CAP_CID_SMP, failed, 2);
            set_error("Confirm mismatch", 0x04);
            set_state(LINK_IDLE);
            return;
        }
        smp_s1(tk, srand, mrand, stk);
        memset(&stk[key_size], 0, 16 - key_size); // Shorten to the negotiated key size
        static const uint8_t zero_rand[8] = { 0 };
        start_encryption(stk, zero_rand, 0);
        break;
    }
    case SMP_PAIRING_FAILED:
        set_error("Pairing failed, reason", len > 1 ? d[1] : 0);
        set_state(LINK_IDLE);
        break;
    case SMP_ENC_INFO:
        if (len >= 17) {
            memcpy(bond_ltk, &d[1], 16);
            ltk_received = true;
        }
        break;
    case SMP_MASTER_IDENT:
        if (len >= 11) {
            bond_ediv = (uint16_t)(d[1] | (d[2] << 8));
            memcpy(bond_rand, &d[3], 8);
            memcpy(bond_addr, peer_addr, 6);
            bond_valid = true;
            // EncKey was the last key the responder distributes (we asked for nothing else)
            send_identity_keys();
        }
        break;
    case SMP_SECURITY_REQ:
        if (link_state == LINK_IDLE) {
            if (bond_valid && memcmp(bond_addr, peer_addr, 6) == 0) {
                encrypting_with_bond = true;
                start_encryption(bond_ltk, bond_rand, bond_ediv);
            } else {
                start_pairing();
            }
        }
        break;
    default:
        break; // Identity / signing keys are not requested; ignore anything else
    }
}

// A 128-bit UUID built on the Bluetooth base UUID (0000xxxx-0000-1000-8000-00805F9B34FB) is the
// same attribute type as its 16-bit form; anything else (vendor UUIDs) maps to 0
static uint16_t uuid16_from_uuid128(const uint8_t *u) {
    static const uint8_t base[12] = { 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00 };
    if (memcmp(u, base, 12) != 0 || u[14] != 0 || u[15] != 0) return 0;
    return (uint16_t)(u[12] | (u[13] << 8));
}

static void handle_find_info_rsp(const uint8_t *d, uint16_t len) {
    if (len < 2) return;
    uint8_t format = d[1];
    int entry_size = (format == 0x01) ? 4 : 18; // 16-bit or 128-bit UUIDs
    uint16_t last_handle = 0;
    find_info_responses++;

    for (int p = 2; p + entry_size <= len; p += entry_size) {
        uint16_t handle = (uint16_t)(d[p] | (d[p + 1] << 8));
        uint16_t uuid = (format == 0x01) ? (uint16_t)(d[p + 2] | (d[p + 3] << 8))
                                         : uuid16_from_uuid128(&d[p + 2]);
        last_handle = handle;
        if (attr_count < MAX_ATTRS) {
            attr_handles[attr_count] = handle;
            attr_types[attr_count] = (uuid == 0) ? 0xFFFF : uuid;
            attr_count++;
        }
    }

    if (last_handle != 0) last_attr_handle = last_handle;
    if (last_handle == 0 || last_handle == 0xFFFF) {
        start_char_discovery();
    } else {
        send_find_info((uint16_t)(last_handle + 1));
    }
}

// The CCCD of a characteristic is the 0x2902 descriptor between its value and the next
// declaration; if the attribute table does not show one, assume the usual value + 1
static uint16_t find_cccd(uint16_t value_handle) {
    for (int i = 0; i < attr_count; i++) {
        if (attr_handles[i] <= value_handle) continue;
        uint16_t t = attr_types[i];
        if (t == UUID_CHARACTERISTIC || t == UUID_PRIMARY_SERVICE || t == UUID_SECONDARY_SERVICE) break;
        if (t == UUID_CCCD) return attr_handles[i];
    }
    return (uint16_t)(value_handle + 1);
}

// Subscribe to every CCCD in the attribute table. The Xbox controller has more than one input
// report and the attribute table does not reliably say which carries the gamepad state, so we
// take them all (what ESP32 Xbox libraries do too); a CCCD that refuses 0x0001 just answers with
// an ATT error, which enable_next_cccd() skips. Falls back to the CCCDs found from Read By Type.
static void finish_char_discovery(void) {
    int from_table = 0;
    for (int i = 0; i < attr_count && from_table < MAX_REPORTS; i++) {
        if (attr_types[i] == UUID_CCCD) cccd_handles[from_table++] = attr_handles[i];
    }
    if (from_table > 0) cccd_count = from_table;
    cccd_next = 0;
    set_state(LINK_ENABLING);
    enable_next_cccd();
}

// Read By Type response: entries of [declaration handle, properties, value handle, UUID]
static void handle_read_chars_rsp(const uint8_t *d, uint16_t len) {
    if (len < 2) return;
    uint8_t entry_len = d[1];
    if (entry_len != 7 && entry_len != 21) { // 16-bit or 128-bit characteristic UUID
        set_error("Bad char entry length", entry_len);
        finish_char_discovery();
        return;
    }
    uint16_t last_decl = 0;

    for (int p = 2; p + entry_len <= len; p += entry_len) {
        const uint8_t *e = &d[p];
        uint16_t decl = (uint16_t)(e[0] | (e[1] << 8));
        uint8_t props = e[2];
        uint16_t value = (uint16_t)(e[3] | (e[4] << 8));
        uint16_t uuid = (entry_len == 7) ? (uint16_t)(e[5] | (e[6] << 8)) : uuid16_from_uuid128(&e[5]);
        last_decl = decl;
        chars_seen++;

        if (uuid == UUID_HID_REPORT && report_count < MAX_REPORTS) {
            report_handles[report_count++] = value;
            // Only input reports can notify (properties bit 4 notify, bit 5 indicate)
            if ((props & 0x30) && cccd_count < MAX_REPORTS) {
                cccd_handles[cccd_count++] = find_cccd(value);
            }
        }
    }

    if (last_decl == 0 || last_decl == 0xFFFF) {
        finish_char_discovery();
    } else {
        send_read_chars((uint16_t)(last_decl + 1));
    }
}

static void handle_att(const uint8_t *d, uint16_t len) {
    if (len < 1) return;
    uint8_t op = d[0];

    switch (op) {
    case ATT_ERROR_RSP:
        if (len < 5) return;
        last_att_error_req = d[1];
        last_att_error_code = d[4];
        if (d[1] == ATT_FIND_INFO_REQ && link_state == LINK_DISCOVERING) {
            // 0x0A Attribute Not Found marks the end of the attribute table
            if (d[4] != 0x0A) set_error("Discovery error", d[4]);
            start_char_discovery();
        } else if (d[1] == ATT_READ_BY_TYPE_REQ && link_state == LINK_CHARS) {
            // 0x0A Attribute Not Found: no more characteristic declarations
            if (d[4] != 0x0A) set_error("Char discovery error", d[4]);
            finish_char_discovery();
        } else if (d[1] == ATT_WRITE_REQ && link_state == LINK_ENABLING) {
            set_error("CCCD write error", d[4]);
            enable_next_cccd();
        }
        return;
    case ATT_MTU_REQ: {
        uint8_t rsp[3] = { ATT_MTU_RSP, 23, 0 }; // Keep the default MTU
        l2cap_send(L2CAP_CID_ATT, rsp, 3);
        return;
    }
    case ATT_FIND_INFO_RSP:
        if (link_state == LINK_DISCOVERING) handle_find_info_rsp(d, len);
        return;
    case ATT_READ_BY_TYPE_RSP:
        if (link_state == LINK_CHARS) handle_read_chars_rsp(d, len);
        return;
    case ATT_WRITE_RSP:
        if (link_state == LINK_ENABLING) enable_next_cccd();
        return;
    case ATT_NOTIFY:
    case ATT_INDICATE:
        if (op == ATT_INDICATE) {
            uint8_t confirm = ATT_CONFIRM;
            l2cap_send(L2CAP_CID_ATT, &confirm, 1);
        }
        if (len >= 3) {
            // Any notification big enough to be the gamepad report is parsed; battery level
            // (1 byte) and other small reports are only counted
            notifications_received++;
            last_notify_handle = (uint16_t)(d[1] | (d[2] << 8));
            last_notify_len = (uint16_t)(len - 3);
            memcpy(last_notify_data, &d[3], last_notify_len < sizeof(last_notify_data) ? last_notify_len : sizeof(last_notify_data));
            if (last_notify_len >= sizeof(xbox_ble_report_t)) {
                reports_received++;
                xbox_ble_parse_report(&d[3], last_notify_len);
            }
        }
        return;
    default:
        break;
    }

    // Requests from the controller acting as a GATT client: we expose no attributes.
    // Requests are the even opcodes below 0x40 (0x1E is a confirmation, commands have bit 6 set).
    if ((op & 0x01) == 0 && op < 0x40 && op != ATT_CONFIRM) {
        bool discovery = (op == 0x04 || op == 0x06 || op == 0x08 || op == 0x10);
        uint8_t err[5] = { ATT_ERROR_RSP, op, 0x00, 0x00, discovery ? 0x0A : 0x06 };
        if (len >= 3) {
            err[2] = d[1];
            err[3] = d[2];
        }
        l2cap_send(L2CAP_CID_ATT, err, 5);
    }
}

static void handle_signaling(const uint8_t *d, uint16_t len) {
    if (len < 4) return;
    uint8_t code = d[0];
    uint8_t id = d[1];

    if (code == 0x12 && len >= 12) {
        // Connection Parameter Update Request: accept, then apply it (we are the central)
        uint8_t rsp[6] = { 0x13, id, 2, 0, 0x00, 0x00 };
        l2cap_send(L2CAP_CID_SIGNALING, rsp, 6);

        uint8_t param[14];
        param[0] = (uint8_t)(conn_handle & 0xFF);
        param[1] = (uint8_t)(conn_handle >> 8);
        memcpy(&param[2], &d[4], 8); // interval min, max, latency, supervision timeout
        memset(&param[10], 0, 4);    // CE length min, max
        bcm43438_send_cmd(0x2013, param, 14); // HCI_LE_Connection_Update
    } else if (code == 0x06 || code == 0x14) {
        // Disconnection / LE credit based connection requests: not supported
        uint8_t rej[6] = { 0x01, id, 2, 0, 0x00, 0x00 };
        l2cap_send(L2CAP_CID_SIGNALING, rej, 6);
    }
}

// ---------------------------------------------------------------------------------------------
// Entry points called by the HCI transport (bcm43438.c)
// ---------------------------------------------------------------------------------------------

void ble_host_set_own_address(const uint8_t addr[6]) {
    memcpy(own_addr, addr, 6);
}

void ble_host_on_connect(uint16_t handle, uint8_t addr_type, const uint8_t addr[6]) {
    connected = true;
    conn_handle = handle;
    peer_type = addr_type;
    memcpy(peer_addr, addr, 6);
    link_error[0] = '\0';
    reports_received = 0;
    notifications_received = 0;
    last_notify_len = 0;

    if (bond_valid && memcmp(bond_addr, addr, 6) == 0) {
        encrypting_with_bond = true;
        start_encryption(bond_ltk, bond_rand, bond_ediv);
    } else {
        start_pairing();
    }
}

void ble_host_on_disconnect(void) {
    connected = false;
    set_state(LINK_IDLE);
}

void ble_host_on_encryption_change(uint8_t status, bool enabled) {
    if (!connected) return;
    if (status != 0 || !enabled) {
        set_error("Encryption failed", status);
        if (encrypting_with_bond) {
            bond_valid = false; // Stale key: pair again from scratch
            start_pairing();
        } else {
            set_state(LINK_IDLE);
        }
        return;
    }
    if (link_state == LINK_ENCRYPTING) {
        // Responder distributes no keys: our identity goes out right after encryption
        if (!encrypting_with_bond && (pres[6] & 0x01) == 0) send_identity_keys();
        start_discovery();
    }
}

void ble_host_on_command_status(uint16_t opcode, uint8_t status) {
    if (status == 0) return;
    if (opcode == 0x2019) {
        set_error("Start encryption rejected", status);
        set_state(LINK_IDLE);
    }
}

void ble_host_on_acl(uint16_t handle_flags, const uint8_t *data, uint16_t len) {
    if (!connected || (handle_flags & 0x0FFF) != conn_handle) return;
    if (((handle_flags >> 12) & 0x3) == 0x1) return; // Continuation fragment: not expected at MTU 23
    if (len < 4) return;

    uint16_t l2len = (uint16_t)(data[0] | (data[1] << 8));
    uint16_t cid = (uint16_t)(data[2] | (data[3] << 8));
    if (l2len > len - 4) return;
    const uint8_t *payload = &data[4];

    if (cid == L2CAP_CID_ATT) {
        handle_att(payload, l2len);
    } else if (cid == L2CAP_CID_SMP) {
        handle_smp(payload, l2len);
    } else if (cid == L2CAP_CID_SIGNALING) {
        handle_signaling(payload, l2len);
    }
}

// Called every poll: give up on a step that never completes so the reconnect starts over
void ble_host_poll(void) {
    if (!connected || link_state == LINK_READY || link_state == LINK_IDLE) return;
    if (timer_get_ticks() - link_state_since > STEP_TIMEOUT_MS) {
        set_error("Timeout in state", (unsigned int)link_state);
        set_state(LINK_IDLE);
        disconnect(0x13); // Remote user terminated connection
    }
}

const char *ble_host_status_line(void) {
    static char line[96];
    // at = attributes recorded, e = last ATT error (request / code), ltk = pairing key received,
    // kd = key distribution the controller accepted (initiator / responder), id = our identity sent,
    // cccd = notifications switched on, nt = notifications of any size, rx = gamepad reports parsed
    snprintf(line, sizeof(line), "%s|at:%d e:%02X/%02X ltk:%d kd:%X/%X id:%d cccd:%d nt:%d rx:%d %s",
             connected ? link_state_names[link_state] : "NO LINK",
             attr_count,
             (unsigned int)last_att_error_req, (unsigned int)last_att_error_code, ltk_received ? 1 : 0,
             (unsigned int)pres[5], (unsigned int)pres[6], identity_sent ? 1 : 0,
             cccd_count, (int)notifications_received, (int)reports_received, link_error);
    return line;
}

// Last notification as "handle len: bytes" for the debug screen
const char *ble_host_last_notify_line(void) {
    static char line[96];
    if (notifications_received == 0) {
        snprintf(line, sizeof(line), "no notifications yet (cccd handles:%s%02X %02X %02X)",
                 cccd_count ? " " : " none ", cccd_count > 0 ? cccd_handles[0] : 0,
                 cccd_count > 1 ? cccd_handles[1] : 0, cccd_count > 2 ? cccd_handles[2] : 0);
        return line;
    }
    int n = snprintf(line, sizeof(line), "h:%02X len:%d:", (unsigned int)last_notify_handle, (int)last_notify_len);
    for (int i = 0; i < last_notify_len && i < (int)sizeof(last_notify_data) && n + 3 < (int)sizeof(line); i++) {
        n += snprintf(&line[n], sizeof(line) - (size_t)n, " %02X", last_notify_data[i]);
    }
    return line;
}

// Attribute table from discovery as "handle:type" pairs, 11 per screen line (FFFF = vendor UUID).
// Returns false when line_index is past the end of the table.
bool ble_host_attr_line(int line_index, char *out, size_t size) {
    int first = line_index * 11;
    if (first >= attr_count || size == 0) return false;
    size_t n = 0;
    out[0] = '\0';
    for (int i = first; i < attr_count && i < first + 11 && n + 9 < size; i++) {
        snprintf(&out[n], size - n, "%02X:%04X ", (unsigned int)attr_handles[i], (unsigned int)attr_types[i]);
        n += strlen(&out[n]);
    }
    return true;
}

#endif
