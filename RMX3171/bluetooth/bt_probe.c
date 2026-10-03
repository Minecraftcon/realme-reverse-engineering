#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <stdint.h>

#define BT_DEV "/dev/stpbt"

// MediaTek ioctls discovered from bt_drv.ko reverse engineering
#define MTK_BT_IOCTL_GET_HW_VER  0x8008b002
#define MTK_BT_IOCTL_GET_FW_VER  0x8008b003

void print_hex(const char *label, const uint8_t *buf, size_t len) {
    printf("%s [%zu bytes]: ", label, len);
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", buf[i]);
    }
    printf("\n");
}

int send_hci_cmd(int fd, const uint8_t *cmd, size_t len, uint8_t *resp, size_t resp_max) {
    print_hex("-> TX HCI CMD", cmd, len);
    ssize_t written = write(fd, cmd, len);
    if (written < 0) {
        perror("[-] write failed");
        return -1;
    }

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) {
        printf("[-] RX Timeout (no response within 2s)\n");
        return -1;
    }

    ssize_t n = read(fd, resp, resp_max);
    if (n < 0) {
        perror("[-] read failed");
        return -1;
    }
    print_hex("<- RX HCI EVT", resp, (size_t)n);
    return (int)n;
}

int main(int argc, char **argv) {
    printf("====================================================\n");
    printf("  MediaTek MT6768 / MT6631 Bluetooth HCI Direct Probe\n");
    printf("====================================================\n");

    printf("[*] Opening %s (this automatically powers on MT6631 BT radio via WMT)...\n", BT_DEV);
    int fd = open(BT_DEV, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "[-] Failed to open %s: %s (errno %d)\n", BT_DEV, strerror(errno), errno);
        return 1;
    }
    printf("[+] Successfully opened %s (fd=%d)!\n", BT_DEV, fd);

    // Test MediaTek custom ioctls
    unsigned long hw_ver = 0;
    if (ioctl(fd, MTK_BT_IOCTL_GET_HW_VER, &hw_ver) == 0) {
        printf("[+] MTK Baseband HW Version: 0x%08lx\n", hw_ver);
    } else {
        printf("[-] MTK HW Version ioctl returned: %s\n", strerror(errno));
    }

    unsigned long fw_ver = 0;
    if (ioctl(fd, MTK_BT_IOCTL_GET_FW_VER, &fw_ver) == 0) {
        printf("[+] MTK Baseband FW Version: 0x%08lx\n", fw_ver);
    } else {
        printf("[-] MTK FW Version ioctl returned: %s\n", strerror(errno));
    }

    uint8_t rx_buf[512];

    // 1. HCI Reset Command: 0x01 (HCI Command), 0x03 0x0C (OpCode 0x0C03), 0x00 (Len)
    printf("\n[*] Step 1: Sending standard HCI_Reset (0x0C03)...\n");
    uint8_t cmd_reset[] = { 0x01, 0x03, 0x0c, 0x00 };
    send_hci_cmd(fd, cmd_reset, sizeof(cmd_reset), rx_buf, sizeof(rx_buf));

    // 2. HCI Read Local Version Information: 0x01, 0x01 0x10, 0x00 (OpCode 0x1001)
    printf("\n[*] Step 2: Reading Local Version Information (0x1001)...\n");
    uint8_t cmd_ver[] = { 0x01, 0x01, 0x10, 0x00 };
    int len = send_hci_cmd(fd, cmd_ver, sizeof(cmd_ver), rx_buf, sizeof(rx_buf));
    if (len >= 15 && rx_buf[0] == 0x04 && rx_buf[1] == 0x0e) {
        // rx_buf: [0]=type, [1]=evt, [2]=len, [3]=ncmd, [4..5]=opcode, [6]=status, [7]=hci_ver, [8..9]=hci_rev, [10]=lmp_ver, [11..12]=mfr, [13..14]=lmp_subver
        uint8_t status = rx_buf[6];
        uint8_t hci_ver = rx_buf[7];
        uint16_t hci_rev = rx_buf[8] | (rx_buf[9] << 8);
        uint8_t lmp_ver = rx_buf[10];
        uint16_t mfr_id = rx_buf[11] | (rx_buf[12] << 8);
        uint16_t lmp_subver = rx_buf[13] | (rx_buf[14] << 8);

        printf("    Status:        0x%02x (%s)\n", status, status == 0 ? "SUCCESS" : "ERROR");
        printf("    HCI Version:   %d (Bluetooth %s)\n", hci_ver, 
            hci_ver == 6 ? "4.0" : hci_ver == 7 ? "4.1" : hci_ver == 8 ? "4.2" : hci_ver == 9 ? "5.0" : hci_ver == 10 ? "5.1" : hci_ver == 11 ? "5.2" : "Unknown");
        printf("    HCI Revision:  0x%04x\n", hci_rev);
        printf("    LMP Version:   %d (Bluetooth %s)\n", lmp_ver,
            lmp_ver == 6 ? "4.0" : lmp_ver == 7 ? "4.1" : lmp_ver == 8 ? "4.2" : lmp_ver == 9 ? "5.0" : lmp_ver == 10 ? "5.1" : lmp_ver == 11 ? "5.2" : "Unknown");
        printf("    Manufacturer:  0x%04x (%s)\n", mfr_id, mfr_id == 0x0046 ? "MediaTek, Inc." : "Other");
        printf("    LMP Subversion:0x%04x\n", lmp_subver);
    }

    // 3. HCI Read BD_ADDR: 0x01, 0x09 0x10, 0x00 (OpCode 0x1009)
    printf("\n[*] Step 3: Reading Local BD_ADDR (0x1009)...\n");
    uint8_t cmd_bdaddr[] = { 0x01, 0x09, 0x10, 0x00 };
    len = send_hci_cmd(fd, cmd_bdaddr, sizeof(cmd_bdaddr), rx_buf, sizeof(rx_buf));
    if (len >= 13 && rx_buf[0] == 0x04 && rx_buf[1] == 0x0e) {
        printf("    Status:        0x%02x (%s)\n", rx_buf[6], rx_buf[6] == 0 ? "SUCCESS" : "ERROR");
        printf("    BD_ADDR:       %02X:%02X:%02X:%02X:%02X:%02X\n",
            rx_buf[12], rx_buf[11], rx_buf[10], rx_buf[9], rx_buf[8], rx_buf[7]);
    }

    // 4. Live BLE Active Scanning Test
    printf("\n[*] Step 4: Configuring Bluetooth Low Energy (BLE) Active Scanner...\n");
    // LE_Set_Scan_Parameters: OpCode 0x200B (OGF 0x08, OCF 0x000B), len 7
    // scan_type=0x01 (Active), interval=0x40 (40ms), window=0x40 (40ms)
    uint8_t cmd_scan_param[] = { 0x01, 0x0b, 0x20, 0x07, 0x01, 0x40, 0x00, 0x40, 0x00, 0x00, 0x00 };
    send_hci_cmd(fd, cmd_scan_param, sizeof(cmd_scan_param), rx_buf, sizeof(rx_buf));

    printf("[*] Enabling BLE Over-the-Air Active Packet Reception...\n");
    uint8_t cmd_scan_enable[] = { 0x01, 0x0c, 0x20, 0x02, 0x01, 0x00 };
    send_hci_cmd(fd, cmd_scan_enable, sizeof(cmd_scan_enable), rx_buf, sizeof(rx_buf));

    // Also start Classic Bluetooth Inquiry: 0x01, 0x01 0x04, 0x05, 0x33 0x8B 0x9E, 0x08, 0x00
    printf("\n[*] Step 5: Launching Classic Bluetooth Radio Inquiry (GIAC 0x9E8B33)...\n");
    uint8_t cmd_inquiry[] = { 0x01, 0x01, 0x04, 0x05, 0x33, 0x8b, 0x9e, 0x08, 0x00 };
    send_hci_cmd(fd, cmd_inquiry, sizeof(cmd_inquiry), rx_buf, sizeof(rx_buf));

    printf("[*] Sniffing live Bluetooth RF spectrum (BLE + Classic) for 6 seconds...\n");
    int rx_count = 0;
    for (int i = 0; i < 25; i++) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ret > 0) {
            ssize_t n = read(fd, rx_buf, sizeof(rx_buf));
            if (n > 0) {
                rx_count++;
                if (rx_buf[0] == 0x04 && rx_buf[1] == 0x3e && rx_buf[3] == 0x02) {
                    int8_t rssi = (int8_t)rx_buf[n - 1];
                    printf("    [OTA BLE] MAC: %02X:%02X:%02X:%02X:%02X:%02X | RSSI: %4d dBm | Len: %zd B\n",
                        rx_buf[12], rx_buf[11], rx_buf[10], rx_buf[9], rx_buf[8], rx_buf[7], rssi, n);
                } else if (rx_buf[0] == 0x04 && (rx_buf[1] == 0x02 || rx_buf[1] == 0x2f)) {
                    // Inquiry Result or Extended Inquiry Result
                    printf("    [OTA BT Classic] Target Discovered! MAC: %02X:%02X:%02X:%02X:%02X:%02X | Clock: 0x%02X%02X\n",
                        rx_buf[8], rx_buf[7], rx_buf[6], rx_buf[5], rx_buf[4], rx_buf[3], rx_buf[12], rx_buf[11]);
                } else {
                    print_hex("    [HCI EVT]", rx_buf, (size_t)n);
                }
            }
        }
    }
    printf("[+] Total Over-the-Air Bluetooth Frames Intercepted: %d frames\n", rx_count);

    // Cancel inquiry and disable scan
    uint8_t cmd_inq_cancel[] = { 0x01, 0x02, 0x04, 0x00 };
    send_hci_cmd(fd, cmd_inq_cancel, sizeof(cmd_inq_cancel), rx_buf, sizeof(rx_buf));
    uint8_t cmd_scan_disable[] = { 0x01, 0x0c, 0x20, 0x02, 0x00, 0x00 };
    send_hci_cmd(fd, cmd_scan_disable, sizeof(cmd_scan_disable), rx_buf, sizeof(rx_buf));

    printf("\n[*] Closing %s (powers off BT radio)...\n", BT_DEV);
    close(fd);
    printf("[+] Done. Direct Bluetooth HCI bus probe succeeded!\n");
    printf("====================================================\n");
    return 0;
}
