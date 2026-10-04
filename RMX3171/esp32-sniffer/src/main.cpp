/*
 * ESP32 Interactive 802.11 Sniffer — RMX3171 Ground Truth Engine
 * Realme Narzo 30A (MT6768) Project | Shado & Antigravity (2026)
 *
 * Commands (send via Serial Monitor @ 115200 baud):
 *   .help            — Show full help menu
 *   .start           — Start sniffing
 *   .stop            — Pause sniffing (keeps stats)
 *   .filter <type>   — Filter frame type: all | mgmt | data | ctrl | deauth | probe | beacon | eapol | disassoc
 *   .lock-target <SSID|BSSID>  — Lock output to a specific target
 *   .unlock          — Remove target lock
 *   .rec             — Start recording frames to serial (PCAP-style hex)
 *   .stoprec         — Stop recording
 *   .listen <type>   — Alias for .filter
 *   .hop             — Start channel hopping (1-13)
 *   .hop stop        — Stop channel hopping
 *   .channel <1-14>  — Lock to specific channel
 *   .stats           — Print current statistics
 *   .reset           — Reset all counters
 *   .clear           — Clear screen
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

// ─── FreeRTOS Thread-Safe Serial Wrappers ────────────────────────────────────
// Prevents UART ringbuffer corruption & circular repeating output between
// Core 0 (Wi-Fi promiscuous RX callback) and Core 1 (Arduino loop/command task)
static SemaphoreHandle_t serial_mutex = NULL;

static void safe_serial_print(const char *msg) {
    if (!serial_mutex) {
        Serial.print(msg);
        return;
    }
    if (xSemaphoreTake(serial_mutex, pdMS_TO_TICKS(15)) == pdTRUE) {
        Serial.print(msg);
        xSemaphoreGive(serial_mutex);
    }
}

static void safe_serial_println(const char *msg = "") {
    if (!serial_mutex) {
        Serial.println(msg);
        return;
    }
    if (xSemaphoreTake(serial_mutex, pdMS_TO_TICKS(15)) == pdTRUE) {
        Serial.println(msg);
        xSemaphoreGive(serial_mutex);
    }
}

static void safe_serial_printf(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    safe_serial_print(buf);
}

// ─── State ───────────────────────────────────────────────────────────────────
static uint8_t  current_channel  = 6;
static uint32_t pkt_total        = 0;
static uint32_t pkt_mgmt         = 0;
static uint32_t pkt_data         = 0;
static uint32_t pkt_ctrl         = 0;
static uint32_t pkt_deauth       = 0;
static uint32_t pkt_disassoc     = 0;
static uint32_t pkt_probe_req    = 0;
static uint32_t pkt_probe_resp   = 0;
static uint32_t pkt_beacon       = 0;
static uint32_t pkt_eapol        = 0;
static uint32_t pkt_recorded     = 0;
static bool     sniffing         = true;
static bool     recording        = false;
static bool     hopping          = false;
static unsigned long last_hop    = 0;
static unsigned long last_hb     = 0;
static uint8_t  hop_idx          = 0;

// Filter modes
typedef enum {
    FILTER_ALL = 0, FILTER_MGMT, FILTER_DATA, FILTER_CTRL,
    FILTER_DEAUTH, FILTER_PROBE, FILTER_BEACON, FILTER_EAPOL, FILTER_DISASSOC
} filter_mode_t;
static filter_mode_t filter_mode = FILTER_ALL;

// Target lock (BSSID bytes, all zeros = no lock)
static uint8_t target_bssid[6]  = {0};
static bool    target_locked     = false;
static char    target_label[64] = {0};

// Hop channel sequence (non-overlapping weighted)
static const uint8_t HOP_CHANNELS[] = {1,6,11,1,2,6,7,11,12,1,3,6,8,11,13};
static const uint8_t HOP_LEN = sizeof(HOP_CHANNELS);

// ─── Helpers ─────────────────────────────────────────────────────────────────
static void mac_str(const uint8_t *m, char *out) {
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
}

static bool mac_matches_target(const uint8_t *da, const uint8_t *sa, const uint8_t *bssid) {
    if (!target_locked) return true;
    return (memcmp(da, target_bssid, 6) == 0 ||
            memcmp(sa, target_bssid, 6) == 0 ||
            memcmp(bssid, target_bssid, 6) == 0);
}

static bool parse_mac(const char *str, uint8_t *out) {
    return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &out[0],&out[1],&out[2],&out[3],&out[4],&out[5]) == 6;
}

static void set_channel(uint8_t ch) {
    if (ch < 1 || ch > 14) return;
    current_channel = ch;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static const char *filter_name() {
    switch(filter_mode) {
        case FILTER_MGMT:    return "MGMT";
        case FILTER_DATA:    return "DATA";
        case FILTER_CTRL:    return "CTRL";
        case FILTER_DEAUTH:  return "DEAUTH";
        case FILTER_PROBE:   return "PROBE";
        case FILTER_BEACON:  return "BEACON";
        case FILTER_EAPOL:   return "EAPOL";
        case FILTER_DISASSOC:return "DISASSOC";
        default:             return "ALL";
    }
}

// Beacon SSID -> BSSID lookup table (simple ring, last 16 beacons seen)
struct ssid_entry { char ssid[33]; uint8_t bssid[6]; };
static ssid_entry ssid_table[16];
static uint8_t ssid_table_idx = 0;

// Populate SSID->BSSID lookup table from beacons (called from sniffer_callback)
void populate_ssid_table(const uint8_t *p, int len, const uint8_t *bssid) {
    if (len >= 38 && p[36] == 0x00) {
        uint8_t ssid_len = p[37];
        if (ssid_len > 32) ssid_len = 32;
        if (ssid_len > 0 && len >= (int)(38 + ssid_len)) {
            uint8_t idx = ssid_table_idx % 16;
            memset(ssid_table[idx].ssid, 0, 33);
            memcpy(ssid_table[idx].ssid, &p[38], ssid_len);
            ssid_table[idx].ssid[ssid_len] = '\0';
            memcpy(ssid_table[idx].bssid, bssid, 6);
            ssid_table_idx++;
        }
    }
}

// ─── Sniffer Callback ─────────────────────────────────────────────────────────
void sniffer_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!sniffing) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p  = pkt->payload;
    int len            = pkt->rx_ctrl.sig_len;
    int rssi           = pkt->rx_ctrl.rssi;
    int ch             = pkt->rx_ctrl.channel;

    if (len < 10) return;
    pkt_total++;

    uint8_t fc0        = p[0];
    uint8_t frame_type = (fc0 >> 2) & 0x03;  // 0=mgmt 1=ctrl 2=data
    uint8_t subtype    = (fc0 >> 4) & 0x0F;

    // Count by type
    if (frame_type == 0) pkt_mgmt++;
    else if (frame_type == 1) pkt_ctrl++;
    else if (frame_type == 2) pkt_data++;

    if (len < 24) return;

    const uint8_t *da    = &p[4];
    const uint8_t *sa    = &p[10];
    const uint8_t *bssid = &p[16];

    char da_s[18], sa_s[18], bs_s[18];
    mac_str(da, da_s); mac_str(sa, sa_s); mac_str(bssid, bs_s);

    bool is_deauth   = (frame_type == 0 && subtype == 12);
    bool is_disassoc = (frame_type == 0 && subtype == 10);
    bool is_beacon   = (frame_type == 0 && subtype == 8);
    bool is_probe_req  = (frame_type == 0 && subtype == 4);
    bool is_probe_resp = (frame_type == 0 && subtype == 5);
    bool is_auth     = (frame_type == 0 && subtype == 11);
    bool is_assoc    = (frame_type == 0 && (subtype == 0 || subtype == 2));
    // EAPOL: data frame, LLC header starts at byte 24, ethertype 0x888E
    bool is_eapol    = (frame_type == 2 && len >= 32 &&
                        p[30] == 0x88 && p[31] == 0x8E);

    if (is_deauth) { pkt_deauth++; }
    if (is_disassoc) { pkt_disassoc++; }
    if (is_beacon) { pkt_beacon++; populate_ssid_table(p, len, bssid); }
    if (is_probe_req || is_probe_resp) { pkt_probe_req++; }
    if (is_eapol) { pkt_eapol++; }

    // Target filter
    if (!mac_matches_target(da, sa, bssid)) return;

    // Frame type filter
    bool show = false;
    switch (filter_mode) {
        case FILTER_ALL:     show = true; break;
        case FILTER_MGMT:    show = (frame_type == 0); break;
        case FILTER_DATA:    show = (frame_type == 2); break;
        case FILTER_CTRL:    show = (frame_type == 1); break;
        case FILTER_DEAUTH:  show = is_deauth; break;
        case FILTER_PROBE:   show = (is_probe_req || is_probe_resp); break;
        case FILTER_BEACON:  show = is_beacon; break;
        case FILTER_EAPOL:   show = is_eapol; break;
        case FILTER_DISASSOC:show = is_disassoc; break;
    }
    if (!show) return;

    // Critical frames (deauth, disassoc, eapol) are never dropped.
    // Routine frames are throttled if UART TX FIFO is congested to avoid blocking wifi_task
    // and prevent UART buffer circular wrap-around / dropped input.
    bool is_critical = (is_deauth || is_disassoc || is_eapol);
    if (!is_critical && Serial.availableForWrite() < 48) {
        return;
    }

    // ─── Print frame ────────────────────────────────────────────────────────
    if (is_deauth) {
        uint16_t reason = (len >= 26) ? (p[24] | (p[25] << 8)) : 0;
        safe_serial_printf("!!! [DEAUTH #%u] CH:%d RSSI:%ddBm | SA=%s -> DA=%s | BSSID=%s | Reason=%u\n",
            pkt_deauth, ch, rssi, sa_s, da_s, bs_s, reason);
    } else if (is_disassoc) {
        uint16_t reason = (len >= 26) ? (p[24] | (p[25] << 8)) : 0;
        safe_serial_printf("!!! [DISASSOC #%u] CH:%d RSSI:%ddBm | SA=%s -> DA=%s | BSSID=%s | Reason=%u\n",
            pkt_disassoc, ch, rssi, sa_s, da_s, bs_s, reason);
    } else if (is_beacon) {
        // Parse SSID from beacon (tag 0 at fixed offset 36)
        char ssid[33] = {0};
        if (len >= 38 && p[36] == 0x00) {
            uint8_t ssid_len = p[37];
            if (ssid_len > 32) ssid_len = 32;
            if (ssid_len > 0 && len >= (int)(38 + ssid_len)) {
                memcpy(ssid, &p[38], ssid_len);
                ssid[ssid_len] = '\0';
            }
        }
        safe_serial_printf("    [BEACON] CH:%d RSSI:%ddBm | BSSID=%s | SSID=\"%s\"\n",
            ch, rssi, bs_s, ssid);
    } else if (is_probe_req) {
        char ssid[33] = {0};
        if (len >= 26 && p[24] == 0x00) {
            uint8_t ssid_len = p[25];
            if (ssid_len > 32) ssid_len = 32;
            if (ssid_len > 0 && len >= (int)(26 + ssid_len)) {
                memcpy(ssid, &p[26], ssid_len);
                ssid[ssid_len] = '\0';
            }
        }
        safe_serial_printf("    [PROBE-REQ] CH:%d RSSI:%ddBm | SA=%s | SSID=\"%s\"\n",
            ch, rssi, sa_s, ssid);
    } else if (is_probe_resp) {
        safe_serial_printf("    [PROBE-RESP] CH:%d RSSI:%ddBm | SA=%s -> DA=%s\n",
            ch, rssi, sa_s, da_s);
    } else if (is_eapol) {
        safe_serial_printf("*** [EAPOL/HANDSHAKE] CH:%d RSSI:%ddBm | SA=%s -> DA=%s | BSSID=%s — WPA HANDSHAKE!\n",
            ch, rssi, sa_s, da_s, bs_s);
    } else if (is_auth) {
        safe_serial_printf("    [AUTH] CH:%d RSSI:%ddBm | SA=%s -> DA=%s\n", ch, rssi, sa_s, da_s);
    } else if (is_assoc) {
        safe_serial_printf("    [ASSOC] CH:%d RSSI:%ddBm | SA=%s -> DA=%s\n", ch, rssi, sa_s, da_s);
    } else if (frame_type == 2) {
        safe_serial_printf("    [DATA] CH:%d RSSI:%ddBm | SA=%s -> DA=%s\n", ch, rssi, sa_s, da_s);
    } else if (frame_type == 1) {
        safe_serial_printf("    [CTRL] CH:%d RSSI:%ddBm | Sub=%u\n", ch, rssi, subtype);
    } else {
        safe_serial_printf("    [MGMT] CH:%d RSSI:%ddBm | Sub=%u | SA=%s\n", ch, rssi, subtype, sa_s);
    }

    // Recording: dump raw hex in a single atomic string to prevent UART interleaving
    if (recording) {
        pkt_recorded++;
        char hex_buf[256];
        int pos = snprintf(hex_buf, sizeof(hex_buf), "REC[%u] len=%d: ", pkt_recorded, len);
        for (int i = 0; i < len && i < 64 && pos < (int)sizeof(hex_buf) - 4; i++) {
            pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", p[i]);
        }
        if (pos < (int)sizeof(hex_buf) - 1) {
            hex_buf[pos++] = '\n';
            hex_buf[pos] = '\0';
        }
        safe_serial_print(hex_buf);
    }
}

// ─── Help Menu ────────────────────────────────────────────────────────────────
void print_help() {
    safe_serial_println();
    safe_serial_println("╔══════════════════════════════════════════════════════╗");
    safe_serial_println("║      ESP32 802.11 Interactive Sniffer — HELP         ║");
    safe_serial_println("╠══════════════════════════════════════════════════════╣");
    safe_serial_println("║ CONTROL                                               ║");
    safe_serial_println("║  .start          Resume sniffing                      ║");
    safe_serial_println("║  .stop           Pause sniffing (stats preserved)     ║");
    safe_serial_println("║  .stats          Show packet counters                 ║");
    safe_serial_println("║  .reset          Reset all counters to zero           ║");
    safe_serial_println("║  .clear          Clear screen                         ║");
    safe_serial_println("╠══════════════════════════════════════════════════════╣");
    safe_serial_println("║ CHANNEL                                               ║");
    safe_serial_println("║  .channel <1-14> Lock to specific channel             ║");
    safe_serial_println("║  .hop            Start channel hopping (1-13, fast)  ║");
    safe_serial_println("║  .hop stop       Stop hopping                         ║");
    safe_serial_println("╠══════════════════════════════════════════════════════╣");
    safe_serial_println("║ FILTERING                                             ║");
    safe_serial_println("║  .filter all     Show all 802.11 frames               ║");
    safe_serial_println("║  .filter mgmt    Only management frames               ║");
    safe_serial_println("║  .filter data    Only data frames                     ║");
    safe_serial_println("║  .filter ctrl    Only control frames                  ║");
    safe_serial_println("║  .filter deauth  Only Deauth frames (attack detector) ║");
    safe_serial_println("║  .filter disassoc  Only Disassoc frames               ║");
    safe_serial_println("║  .filter probe   Only Probe Req/Resp frames           ║");
    safe_serial_println("║  .filter beacon  Only Beacon frames (AP discovery)    ║");
    safe_serial_println("║  .filter eapol   Only EAPOL/WPA handshake frames      ║");
    safe_serial_println("║  .listen <type>  Alias for .filter                    ║");
    safe_serial_println("╠══════════════════════════════════════════════════════╣");
    safe_serial_println("║ TARGET LOCKING                                        ║");
    safe_serial_println("║  .lock-target <BSSID>    Lock to AP by MAC            ║");
    safe_serial_println("║     e.g: .lock-target AA:BB:CC:DD:EE:FF               ║");
    safe_serial_println("║  .lock-target <SSID>     Lock to AP by name           ║");
    safe_serial_println("║     e.g: .lock-target MyHomeWifi                      ║");
    safe_serial_println("║     (requires beacon to be seen first)                ║");
    safe_serial_println("║  .unlock         Remove target lock (show all)        ║");
    safe_serial_println("╠══════════════════════════════════════════════════════╣");
    safe_serial_println("║ RECORDING                                             ║");
    safe_serial_println("║  .rec            Start raw hex frame recording        ║");
    safe_serial_println("║     Output: REC[n] len=X: <hex bytes>                 ║");
    safe_serial_println("║     Copy to host and decode with: text2pcap or        ║");
    safe_serial_println("║     pipe to scapy for live analysis                   ║");
    safe_serial_println("║  .stoprec        Stop recording                       ║");
    safe_serial_println("╠══════════════════════════════════════════════════════╣");
    safe_serial_println("║ TIPS                                                  ║");
    safe_serial_println("║  - Set .filter deauth + .hop to detect deauth attacks ║");
    safe_serial_println("║    across ALL channels in real time                   ║");
    safe_serial_println("║  - Set .filter eapol on target channel to catch       ║");
    safe_serial_println("║    WPA handshakes while running airmon-inject         ║");
    safe_serial_println("║  - .rec + .lock-target dumps raw frames for offline   ║");
    safe_serial_println("║    analysis with Wireshark                            ║");
    safe_serial_println("║  - EAPOL frames marked *** are WPA 4-way handshakes  ║");
    safe_serial_println("║  - Heartbeat prints every 5s with live stats          ║");
    safe_serial_println("╚══════════════════════════════════════════════════════╝");
    safe_serial_println();
}

// ─── Command Parser ───────────────────────────────────────────────────────────
void handle_command(String &cmd) {
    // Strip \r, \n, and any non-printable/non-ASCII garbage
    String clean = "";
    for (int i = 0; i < (int)cmd.length(); i++) {
        char c = cmd[i];
        if (c >= 0x20 && c < 0x7F) clean += c;
    }
    clean.trim();
    cmd = clean;

    // Echo back clean input so user has feedback
    if (cmd.length() > 0) {
        safe_serial_printf(">> %s\n", cmd.c_str());
    }

    if (cmd.length() == 0) return;

    // .help
    if (cmd == ".help" || cmd == "help") {
        print_help();
        return;
    }

    // .start
    if (cmd == ".start") {
        sniffing = true;
        safe_serial_printf("[CMD] Sniffing STARTED | CH:%d | Filter:%s\n",
            current_channel, filter_name());
        return;
    }

    // .stop
    if (cmd == ".stop") {
        sniffing = false;
        safe_serial_println("[CMD] Sniffing PAUSED. Send .start to resume.");
        return;
    }

    // .clear
    if (cmd == ".clear") {
        safe_serial_print("\033[2J\033[H");
        return;
    }

    // .reset
    if (cmd == ".reset") {
        pkt_total = pkt_mgmt = pkt_data = pkt_ctrl = 0;
        pkt_deauth = pkt_disassoc = pkt_probe_req = pkt_probe_resp = 0;
        pkt_beacon = pkt_eapol = pkt_recorded = 0;
        safe_serial_println("[CMD] All counters reset.");
        return;
    }

    // .stats
    if (cmd == ".stats") {
        safe_serial_println();
        safe_serial_println("┌─ ESP32 Sniffer Stats ──────────────────────────┐");
        safe_serial_printf( "│  Channel  : %-3d %s                            \n", current_channel, hopping?"(HOPPING)":"(LOCKED) ");
        safe_serial_printf( "│  Filter   : %-10s                             \n", filter_name());
        safe_serial_printf( "│  Target   : %-32s      \n", target_locked ? target_label : "(none — show all)");
        safe_serial_printf( "│  Sniffing : %s                                  \n", sniffing?"YES":"PAUSED");
        safe_serial_printf( "│  Recording: %s                                  \n", recording?"YES":"NO");
        safe_serial_println("├─────────────────────────────────────────────────┤");
        safe_serial_printf( "│  Total Pkts  : %u\n", pkt_total);
        safe_serial_printf( "│  Mgmt        : %u  |  Data : %u  |  Ctrl : %u\n", pkt_mgmt, pkt_data, pkt_ctrl);
        safe_serial_printf( "│  Deauths     : %u  |  Disassoc: %u\n", pkt_deauth, pkt_disassoc);
        safe_serial_printf( "│  Probe Req   : %u  |  Beacons : %u\n", pkt_probe_req, pkt_beacon);
        safe_serial_printf( "│  EAPOL/WPA   : %u  |  Recorded: %u\n", pkt_eapol, pkt_recorded);
        safe_serial_println("└─────────────────────────────────────────────────┘");
        return;
    }

    // .channel <n>
    if (cmd.startsWith(".channel ")) {
        int ch = cmd.substring(9).toInt();
        if (ch >= 1 && ch <= 14) {
            hopping = false;
            set_channel(ch);
            safe_serial_printf("[CMD] Locked to Channel %d\n", ch);
        } else {
            safe_serial_println("[ERR] Channel must be 1-14");
        }
        return;
    }

    // .hop stop
    if (cmd == ".hop stop") {
        hopping = false;
        safe_serial_println("[CMD] Channel hopping STOPPED.");
        return;
    }

    // .hop
    if (cmd == ".hop") {
        hopping = true;
        safe_serial_println("[CMD] Channel hopping STARTED (1-13, 120ms/ch).");
        return;
    }

    // .filter / .listen
    String filter_cmd = "";
    if (cmd.startsWith(".filter "))  filter_cmd = cmd.substring(8);
    if (cmd.startsWith(".listen "))  filter_cmd = cmd.substring(8);
    if (filter_cmd.length() > 0) {
        filter_cmd.toLowerCase();
        if      (filter_cmd == "all")      filter_mode = FILTER_ALL;
        else if (filter_cmd == "mgmt")     filter_mode = FILTER_MGMT;
        else if (filter_cmd == "data")     filter_mode = FILTER_DATA;
        else if (filter_cmd == "ctrl")     filter_mode = FILTER_CTRL;
        else if (filter_cmd == "deauth")   filter_mode = FILTER_DEAUTH;
        else if (filter_cmd == "probe")    filter_mode = FILTER_PROBE;
        else if (filter_cmd == "beacon")   filter_mode = FILTER_BEACON;
        else if (filter_cmd == "eapol")    filter_mode = FILTER_EAPOL;
        else if (filter_cmd == "disassoc") filter_mode = FILTER_DISASSOC;
        else { safe_serial_printf("[ERR] Unknown filter: %s — use .help\n", filter_cmd.c_str()); return; }
        safe_serial_printf("[CMD] Filter set to: %s\n", filter_name());
        return;
    }

    // .unlock
    if (cmd == ".unlock") {
        target_locked = false;
        memset(target_bssid, 0, 6);
        memset(target_label, 0, 64);
        safe_serial_println("[CMD] Target lock removed. Showing all traffic.");
        return;
    }

    // .lock-target <BSSID|SSID>
    if (cmd.startsWith(".lock-target ")) {
        String arg = cmd.substring(13);
        arg.trim();
        uint8_t mac[6] = {0};
        if (parse_mac(arg.c_str(), mac)) {
            // Direct BSSID lock
            memcpy(target_bssid, mac, 6);
            target_locked = true;
            snprintf(target_label, 64, "%s", arg.c_str());
            safe_serial_printf("[CMD] Locked to BSSID: %s\n", target_label);
        } else {
            // SSID lookup from beacon table
            bool found = false;
            for (int i = 0; i < 16; i++) {
                if (strcasecmp(ssid_table[i].ssid, arg.c_str()) == 0) {
                    memcpy(target_bssid, ssid_table[i].bssid, 6);
                    target_locked = true;
                    snprintf(target_label, 64, "%s", arg.c_str());
                    char bs[18]; mac_str(target_bssid, bs);
                    safe_serial_printf("[CMD] Locked to SSID \"%s\" -> BSSID %s\n", arg.c_str(), bs);
                    found = true;
                    break;
                }
            }
            if (!found) {
                safe_serial_printf("[WARN] SSID \"%s\" not seen yet.\n", arg.c_str());
                safe_serial_println("       Run .filter beacon + .hop to scan for it first,");
                safe_serial_println("       then retry .lock-target once it appears.");
            }
        }
        return;
    }

    // .rec
    if (cmd == ".rec") {
        recording = true;
        pkt_recorded = 0;
        safe_serial_println("[CMD] Recording STARTED. Raw hex appended after each matching frame.");
        safe_serial_println("      Copy REC[n] lines to host, strip prefix, use text2pcap to decode.");
        return;
    }

    // .stoprec
    if (cmd == ".stoprec") {
        recording = false;
        safe_serial_printf("[CMD] Recording STOPPED. %u frames captured.\n", pkt_recorded);
        return;
    }

    safe_serial_printf("[ERR] Unknown command: '%s' — send .help\n", cmd.c_str());
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    serial_mutex = xSemaphoreCreateMutex();
    delay(800);
    safe_serial_println();
    safe_serial_println("╔══════════════════════════════════════════════════════╗");
    safe_serial_println("║   ESP32 802.11 Interactive Sniffer  v2.1             ║");
    safe_serial_println("║   RMX3171 / MT6768 Ground Truth Engine               ║");
    safe_serial_println("║   Send .help for command reference                   ║");
    safe_serial_println("╚══════════════════════════════════════════════════════╝");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(&sniffer_callback);
    esp_wifi_set_promiscuous(true);
    set_channel(current_channel);

    safe_serial_printf("[ESP32] Ready on Channel %d | Filter: ALL | .help for commands\n\n",
        current_channel);
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
static char cmd_in_buf[128];
static uint8_t cmd_in_pos = 0;

void loop() {
    // Non-blocking serial command input (instant response, zero 1000ms timeouts)
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (cmd_in_pos > 0) {
                cmd_in_buf[cmd_in_pos] = '\0';
                String cmd = String(cmd_in_buf);
                cmd_in_pos = 0;
                handle_command(cmd);
            }
        } else if (c >= 0x20 && c < 0x7F) {
            if (cmd_in_pos < sizeof(cmd_in_buf) - 1) {
                cmd_in_buf[cmd_in_pos++] = c;
            }
        }
    }

    // Channel hopper
    if (hopping && (millis() - last_hop > 120)) {
        last_hop = millis();
        set_channel(HOP_CHANNELS[hop_idx % HOP_LEN]);
        hop_idx++;
    }

    // Heartbeat: every 5s while sniffing, every 15s when paused (less spam)
    unsigned long hb_interval = sniffing ? 5000 : 15000;
    if (millis() - last_hb > hb_interval) {
        last_hb = millis();
        safe_serial_printf("[HB] CH:%d%s | Filter:%-8s | Total:%u | Deauth:%u | EAPOL:%u | %s\n",
            current_channel, hopping?"(HOP)":"     ",
            filter_name(), pkt_total, pkt_deauth, pkt_eapol,
            sniffing ? "SNIFFING" : "PAUSED");
        if (!sniffing) safe_serial_println("     Send .start to resume sniffing.");
    }

    delay(5);
}
