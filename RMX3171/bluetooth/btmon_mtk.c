#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <stdint.h>

#define BT_DEV "/dev/stpbt"

// PCAP File Header Format (Standard Libpcap)
struct pcap_hdr_s {
    uint32_t magic_number;   // 0xa1b2c3d4
    uint16_t version_major;  // 2
    uint16_t version_minor;  // 4
    int32_t  thiszone;       // 0
    uint32_t sigfigs;        // 0
    uint32_t snaplen;        // max length of captured packets (65535)
    uint32_t network;        // DLT_BLUETOOTH_HCI_H4 = 187 (or DLT_BLUETOOTH_HCI_H4_WITH_PHDR = 201)
} __attribute__((packed));

// PCAP Packet Record Header
struct pcaprec_hdr_s {
    uint32_t ts_sec;         // timestamp seconds
    uint32_t ts_usec;        // timestamp microseconds
    uint32_t incl_len;       // number of octets of packet saved in file
    uint32_t orig_len;       // actual length of packet
} __attribute__((packed));

// DLT 201: Bluetooth H4 with Direction Flag Header
// 4-byte pseudo-header prepended to H4 frame:
// uint32_t direction: 0 = Host to Controller (TX), 1 = Controller to Host (RX)
#define DLT_BLUETOOTH_HCI_H4_WITH_PHDR 201

static volatile int g_running = 1;
static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static FILE *g_pcap_fp = NULL;

static void write_pcap_packet(uint32_t direction, const uint8_t *data, size_t len) {
    if (!g_pcap_fp) return;

    struct timeval tv;
    gettimeofday(&tv, NULL);

    uint32_t dir_be = direction; // 0=TX, 1=RX
    uint32_t total_len = len + 4;

    struct pcaprec_hdr_s rec;
    rec.ts_sec = tv.tv_sec;
    rec.ts_usec = tv.tv_usec;
    rec.incl_len = total_len;
    rec.orig_len = total_len;

    fwrite(&rec, sizeof(rec), 1, g_pcap_fp);
    fwrite(&dir_be, sizeof(dir_be), 1, g_pcap_fp);
    fwrite(data, 1, len, g_pcap_fp);
    fflush(g_pcap_fp);
}

static int send_hci_cmd(int fd, const uint8_t *cmd, size_t len, uint8_t *resp, size_t resp_max) {
    write_pcap_packet(0, cmd, len); // Log TX to PCAP
    ssize_t written = write(fd, cmd, len);
    if (written < 0) return -1;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return -1;

    ssize_t n = read(fd, resp, resp_max);
    if (n > 0) {
        write_pcap_packet(1, resp, n); // Log RX to PCAP
    }
    return (int)n;
}

// MediaTek Vendor Specific Logging Activation
static void enable_mtk_baseband_logging(int fd) {
    // Write "1 1 4" to procfs /proc/driver/bt_dbg if available to enable firmware verbose logging
    int dbg_fd = open("/proc/driver/bt_dbg", O_WRONLY);
    if (dbg_fd >= 0) {
        write(dbg_fd, "1 1 4\n", 6);
        close(dbg_fd);
    }
}

// Parse BLE Advertising Report and extract details
static void parse_ble_report(const uint8_t *pkt, size_t len) {
    // pkt[0]=0x04 (Event), pkt[1]=0x3E (LE Meta), pkt[2]=len, pkt[3]=0x02 (LE Adv Report), pkt[4]=num_reports
    if (len < 12 || pkt[0] != 0x04 || pkt[1] != 0x3e || pkt[3] != 0x02) return;

    uint8_t evt_type = pkt[5];
    uint8_t addr_type = pkt[6];
    const uint8_t *mac = &pkt[7];
    uint8_t data_len = pkt[13];
    const uint8_t *data = &pkt[14];
    int8_t rssi = (len >= 15 + data_len) ? (int8_t)pkt[14 + data_len] : 0;

    // Check device name in AD structure
    char dev_name[64] = {0};
    char mfr_info[128] = {0};
    size_t idx = 0;
    while (idx < data_len) {
        uint8_t ad_len = data[idx];
        if (ad_len == 0 || idx + ad_len >= data_len) break;
        uint8_t ad_type = data[idx + 1];
        if (ad_type == 0x08 || ad_type == 0x09) { // Shortened or Complete Name
            size_t nlen = ad_len - 1;
            if (nlen > sizeof(dev_name) - 1) nlen = sizeof(dev_name) - 1;
            memcpy(dev_name, &data[idx + 2], nlen);
            dev_name[nlen] = '\0';
        } else if (ad_type == 0xFF && ad_len >= 3) { // Manufacturer Data
            uint16_t mfr_id = data[idx + 2] | (data[idx + 3] << 8);
            if (mfr_id == 0x004C) { // Apple
                uint8_t apple_type = (ad_len >= 4) ? data[idx + 4] : 0;
                snprintf(mfr_info, sizeof(mfr_info), "Apple (Type 0x%02X%s)", 
                    apple_type, apple_type == 0x12 ? " AirTag/FindMy" : apple_type == 0x07 ? " AirPods" : "");
            } else if (mfr_id == 0x0006) {
                snprintf(mfr_info, sizeof(mfr_info), "Microsoft");
            } else if (mfr_id == 0x00E0) {
                snprintf(mfr_info, sizeof(mfr_info), "Google FastPair");
            } else {
                snprintf(mfr_info, sizeof(mfr_info), "Mfr: 0x%04X", mfr_id);
            }
        }
        idx += ad_len + 1;
    }

    printf("\033[1;36m[BLE ADV]\033[0m %02X:%02X:%02X:%02X:%02X:%02X [%4d dBm] | %-20s | %s\n",
        mac[5], mac[4], mac[3], mac[2], mac[1], mac[0],
        rssi,
        dev_name[0] ? dev_name : "<anonymous>",
        mfr_info[0] ? mfr_info : "Standard Beacon");
}

int main(int argc, char **argv) {
    const char *pcap_file = "/sdcard/bt_capture.pcap";
    int scan_mode = 3; // 1 = BLE only, 2 = Classic only, 3 = Hybrid BLE + Classic

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) {
            pcap_file = argv[++i];
        } else if (!strcmp(argv[i], "--ble")) {
            scan_mode = 1;
        } else if (!strcmp(argv[i], "--classic")) {
            scan_mode = 2;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: btmon-mtk [-w output.pcap] [--ble] [--classic]\n");
            return 0;
        }
    }

    printf("\033[1;32m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m\n");
    printf("\033[1;37m        BTMON-MTK: Baseband Packet Sniffer\033[0m\n");
    printf("\033[1;30m      Direct HCI Capture (Wireshark PCAP-Ready)\033[0m\n");
    printf("\033[1;32m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m\n");
    printf("[*] PCAP Output:  %s\n", pcap_file);
    printf("[*] Scan Mode:    %s\n", scan_mode == 1 ? "BLE Only" : scan_mode == 2 ? "Classic Only" : "Hybrid BLE + Classic");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // Open PCAP output
    g_pcap_fp = fopen(pcap_file, "wb");
    if (!g_pcap_fp) {
        fprintf(stderr, "[-] Failed to open PCAP file %s for writing: %s\n", pcap_file, strerror(errno));
        return 1;
    }

    struct pcap_hdr_s phdr;
    phdr.magic_number = 0xa1b2c3d4;
    phdr.version_major = 2;
    phdr.version_minor = 4;
    phdr.thiszone = 0;
    phdr.sigfigs = 0;
    phdr.snaplen = 65535;
    phdr.network = DLT_BLUETOOTH_HCI_H4_WITH_PHDR; // 201 (Wireshark compatible)
    fwrite(&phdr, sizeof(phdr), 1, g_pcap_fp);
    fflush(g_pcap_fp);

    printf("[*] Initializing MediaTek Bluetooth baseband radio (/dev/stpbt)...\n");
    int fd = open(BT_DEV, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "[-] Failed to open %s: %s\n", BT_DEV, strerror(errno));
        fclose(g_pcap_fp);
        return 1;
    }
    printf("\033[1;32m[+] Radio online (WMT power enabled).\033[0m\n");

    enable_mtk_baseband_logging(fd);

    uint8_t rx_buf[1024];

    // Reset baseband
    uint8_t cmd_reset[] = { 0x01, 0x03, 0x0c, 0x00 };
    send_hci_cmd(fd, cmd_reset, sizeof(cmd_reset), rx_buf, sizeof(rx_buf));

    // Read Local BD_ADDR
    uint8_t cmd_bdaddr[] = { 0x01, 0x09, 0x10, 0x00 };
    int n = send_hci_cmd(fd, cmd_bdaddr, sizeof(cmd_bdaddr), rx_buf, sizeof(rx_buf));
    if (n >= 13 && rx_buf[0] == 0x04 && rx_buf[6] == 0) {
        printf("[*] Local Adapter MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
            rx_buf[12], rx_buf[11], rx_buf[10], rx_buf[9], rx_buf[8], rx_buf[7]);
    }

    // Set event mask to receive all events (including LE Meta, Inquiry, Connect, etc.)
    uint8_t cmd_evt_mask[] = { 0x01, 0x01, 0x0c, 0x08, 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };
    send_hci_cmd(fd, cmd_evt_mask, sizeof(cmd_evt_mask), rx_buf, sizeof(rx_buf));

    if (scan_mode & 1) {
        // Configure active BLE scan
        uint8_t cmd_scan_param[] = { 0x01, 0x0b, 0x20, 0x07, 0x01, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00 };
        send_hci_cmd(fd, cmd_scan_param, sizeof(cmd_scan_param), rx_buf, sizeof(rx_buf));
        uint8_t cmd_scan_enable[] = { 0x01, 0x0c, 0x20, 0x02, 0x01, 0x00 };
        send_hci_cmd(fd, cmd_scan_enable, sizeof(cmd_scan_enable), rx_buf, sizeof(rx_buf));
        printf("[+] Active BLE Scanner ENGAGED.\n");
    }

    if (scan_mode & 2) {
        // Launch periodic inquiry or single inquiry
        uint8_t cmd_inq[] = { 0x01, 0x01, 0x04, 0x05, 0x33, 0x8b, 0x9e, 0x10, 0x00 };
        send_hci_cmd(fd, cmd_inq, sizeof(cmd_inq), rx_buf, sizeof(rx_buf));
        printf("[+] Classic Bluetooth Radio Inquiry ENGAGED.\n");
    }

    printf("\n\033[1;33m[*] Sniffing Bluetooth airwaves... Press Ctrl+C to stop.\033[0m\n\n");

    uint64_t total_pkts = 0;
    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };

        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(fd, &fds)) {
            ssize_t bytes = read(fd, rx_buf, sizeof(rx_buf));
            if (bytes > 0) {
                total_pkts++;
                write_pcap_packet(1, rx_buf, (size_t)bytes);

                if (rx_buf[0] == 0x04 && rx_buf[1] == 0x3e) {
                    parse_ble_report(rx_buf, (size_t)bytes);
                } else if (rx_buf[0] == 0x04 && (rx_buf[1] == 0x02 || rx_buf[1] == 0x2f)) {
                    printf("\033[1;35m[BT CLASSIC]\033[0m Target Discovered: %02X:%02X:%02X:%02X:%02X:%02X\n",
                        rx_buf[8], rx_buf[7], rx_buf[6], rx_buf[5], rx_buf[4], rx_buf[3]);
                } else if (rx_buf[0] == 0x04 && rx_buf[1] == 0x01) {
                    // Inquiry complete, re-launch inquiry if classic mode active
                    if (scan_mode & 2) {
                        uint8_t r_inq[] = { 0x01, 0x01, 0x04, 0x05, 0x33, 0x8b, 0x9e, 0x10, 0x00 };
                        send_hci_cmd(fd, r_inq, sizeof(r_inq), rx_buf, sizeof(rx_buf));
                    }
                }
            }
        }
    }

    printf("\n[*] Stopping capture and shutting down radio...\n");
    if (scan_mode & 1) {
        uint8_t cmd_scan_off[] = { 0x01, 0x0c, 0x20, 0x02, 0x00, 0x00 };
        send_hci_cmd(fd, cmd_scan_off, sizeof(cmd_scan_off), rx_buf, sizeof(rx_buf));
    }
    if (scan_mode & 2) {
        uint8_t cmd_inq_off[] = { 0x01, 0x02, 0x04, 0x00 };
        send_hci_cmd(fd, cmd_inq_off, sizeof(cmd_inq_off), rx_buf, sizeof(rx_buf));
    }

    close(fd);
    fclose(g_pcap_fp);

    printf("\033[1;32m[✓] Capture saved cleanly! Total frames captured: %llu\033[0m\n", (unsigned long long)total_pkts);
    printf("[*] PCAP path: %s\n", pcap_file);
    return 0;
}
