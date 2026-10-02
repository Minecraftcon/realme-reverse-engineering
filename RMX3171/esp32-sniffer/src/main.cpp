#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

static uint8_t current_channel = 11;
static uint32_t pkt_count = 0;
static uint32_t deauth_count = 0;

void sniffer_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    int rssi = pkt->rx_ctrl.rssi;
    int ch = pkt->rx_ctrl.channel;

    if (len < 24) return;

    uint8_t fc0 = payload[0];
    uint8_t frame_type = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;

    pkt_count++;

    // Addresses
    const uint8_t *da = &payload[4];
    const uint8_t *sa = &payload[10];
    const uint8_t *bssid = &payload[16];

    // Highlight Deauth (Type 0, Subtype 12 / 0x0C) and Disassoc (Type 0, Subtype 10 / 0x0A)
    if (frame_type == 0 && subtype == 12) {
        deauth_count++;
        uint16_t reason = 0;
        if (len >= 26) {
            reason = payload[24] | (payload[25] << 8);
        }
        Serial.printf(">>> [ESP32-DEAUTH #%u] RSSI:%d dBm CH:%d DA=%02X:%02X:%02X:%02X:%02X:%02X SA=%02X:%02X:%02X:%02X:%02X:%02X BSSID=%02X:%02X:%02X:%02X:%02X:%02X Reason=%u <<<\n",
            deauth_count, rssi, ch,
            da[0], da[1], da[2], da[3], da[4], da[5],
            sa[0], sa[1], sa[2], sa[3], sa[4], sa[5],
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
            reason);
        return;
    }

    if (frame_type == 0 && subtype == 10) {
        Serial.printf(">>> [ESP32-DISASSOC] RSSI:%d dBm CH:%d DA=%02X:%02X:%02X:%02X:%02X:%02X SA=%02X:%02X:%02X:%02X:%02X:%02X <<<\n",
            rssi, ch,
            da[0], da[1], da[2], da[3], da[4], da[5],
            sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
        return;
    }

    // Only log Deauth (12) and Disassoc (10) above to prevent UART buffer saturation
}

void set_channel(uint8_t ch) {
    if (ch >= 1 && ch <= 14) {
        current_channel = ch;
        esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[ESP32] Tuned to Channel %d\n", current_channel);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n==============================================");
    Serial.println(" ESP32 802.11 Raw OTA Sniffer & Ground Truth ");
    Serial.println("==============================================");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&sniffer_callback);
    esp_wifi_set_promiscuous(true);

    set_channel(current_channel);
    Serial.println("[ESP32] Promiscuous Sniffer Active. Ready for OTA packets.");
    Serial.println("[ESP32] Send '1'..'13' via Serial to switch channel.");
}

static unsigned long last_heartbeat = 0;

void loop() {
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        int ch = input.toInt();
        if (ch >= 1 && ch <= 14) {
            set_channel(ch);
        }
    }

    if (millis() - last_heartbeat > 5000) {
        last_heartbeat = millis();
        Serial.printf("[ESP32 HEARTBEAT] Ch:%d | Total Pkts:%u | Deauths Seen:%u\n",
            current_channel, pkt_count, deauth_count);
    }
    delay(10);
}
