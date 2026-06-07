#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"

// C6 wiring:
// GPS TX -> D7 (GPIO17), GPS RX <- D6 (GPIO16)
// C6 -> C5: D0 (GPIO0 TX), optional C5 -> C6: D1 (GPIO1 RX)

#define GPS_UART_NUM        UART_NUM_1
#define LINK_UART_NUM       UART_NUM_0

#define GPS_RX_GPIO         17
#define GPS_TX_GPIO         16

#define LINK_TX_GPIO        0
#define LINK_RX_GPIO        1

#define GPS_BAUD            9600
#define LINK_BAUD           115200

#define AP24_MAX_SEEN               768
#define AP24_FLUSH_BATCH            24
#define AP24_RSSI_RESEND_DELTA_DB   5
#define AP24_RESEND_INTERVAL_MS     15000

#define AP24_DWELL_PRIMARY_MS       160
#define AP24_DWELL_SECONDARY_MS     100
#define AP24_DUCB_GAMMA             0.99
#define AP24_DUCB_C                 0.85

static const char *TAG = "C6_GPS";

typedef enum {
    AP24_TIER_PRIMARY = 0,
    AP24_TIER_SECONDARY,
} ap24_tier_t;

typedef struct {
    int channel;
    ap24_tier_t tier;
    double discounted_reward;
    double discounted_pulls;
    int total_pulls;
} ap24_channel_t;

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t authmode;
    int64_t last_sent_ms;
    bool dirty;
} ap24_seen_t;

typedef struct {
    bool valid;
    double lat;
    double lon;
    double alt_m;
    int sats;
    double hdop;
    char date_utc[16];
    char time_utc[16];
} gps_fix_t;

static gps_fix_t g_fix = {
    .valid = false, .lat = 0, .lon = 0, .alt_m = 0, .sats = 0, .hdop = 99.99,
    .date_utc = "0-0-0", .time_utc = "0:0:0"
};

static const uint8_t ap24_primary_channels[] = {1, 6, 11};
static const uint8_t ap24_secondary_channels[] = {2, 3, 4, 5, 7, 8, 9, 10, 12, 13};

#define AP24_PRIMARY_COUNT   (sizeof(ap24_primary_channels) / sizeof(ap24_primary_channels[0]))
#define AP24_SECONDARY_COUNT (sizeof(ap24_secondary_channels) / sizeof(ap24_secondary_channels[0]))
#define AP24_CHANNEL_COUNT   (AP24_PRIMARY_COUNT + AP24_SECONDARY_COUNT)

static SemaphoreHandle_t g_link_uart_lock = NULL;
static ap24_seen_t *g_ap24_seen = NULL;
static ap24_seen_t *g_ap24_flush_buf = NULL;
static int g_ap24_seen_count = 0;
static uint32_t g_ap24_dropped = 0;
static uint32_t g_gps_rx_bytes = 0;
static uint32_t g_gps_rx_lines = 0;
static uint32_t g_gps_rmc_lines = 0;
static uint32_t g_gps_gga_lines = 0;
static volatile uint32_t g_ap24_new_since_dwell = 0;
static ap24_channel_t g_ap24_channels[AP24_CHANNEL_COUNT];
static int g_ap24_channel_count = 0;
static double g_ap24_discounted_total = 0.0;
static portMUX_TYPE g_ap24_lock = portMUX_INITIALIZER_UNLOCKED;

static void link_uart_write(const char *line) {
    if (!line) return;

    if (g_link_uart_lock) {
        xSemaphoreTake(g_link_uart_lock, portMAX_DELAY);
    }
    uart_write_bytes(LINK_UART_NUM, line, strlen(line));
    if (g_link_uart_lock) {
        xSemaphoreGive(g_link_uart_lock);
    }
}

static void uart_init(void) {
    const uart_config_t gps_cfg = {
        .baud_rate = GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &gps_cfg));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM, GPS_TX_GPIO, GPS_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    const uart_config_t link_cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART_NUM, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART_NUM, &link_cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART_NUM, LINK_TX_GPIO, LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static esp_err_t init_nvs_wifi(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

static void parse_rmc(char *line) {
    // $GPRMC,time,status,lat,N,lon,E,speed,course,date,...
    char *tokens[16] = {0};
    int n = 0;
    for (char *p = strtok(line, ","); p && n < 16; p = strtok(NULL, ",")) tokens[n++] = p;
    if (n < 10) return;
    if (tokens[2] && tokens[2][0] == 'A') g_fix.valid = true;
    if (tokens[1] && strlen(tokens[1]) >= 6) {
        snprintf(g_fix.time_utc, sizeof(g_fix.time_utc), "%.2s:%.2s:%.2s", tokens[1], tokens[1] + 2, tokens[1] + 4);
    }
    if (tokens[9] && strlen(tokens[9]) >= 6) {
        snprintf(g_fix.date_utc, sizeof(g_fix.date_utc), "20%.2s-%.2s-%.2s", tokens[9] + 4, tokens[9] + 2, tokens[9]);
    }
}

static double ddmm_to_decimal(const char *v, bool is_lat) {
    if (!v || !*v) return 0.0;
    double raw = atof(v);
    int deg = is_lat ? (int)(raw / 100.0) : (int)(raw / 100.0);
    double mins = raw - (deg * 100.0);
    return deg + mins / 60.0;
}

static void parse_gga(char *line) {
    // $GPGGA,time,lat,N,lon,E,fix,sats,hdop,alt,...
    char *tokens[16] = {0};
    int n = 0;
    for (char *p = strtok(line, ","); p && n < 16; p = strtok(NULL, ",")) tokens[n++] = p;
    if (n < 10) return;

    if (tokens[2] && tokens[3] && tokens[4] && tokens[5]) {
        double lat = ddmm_to_decimal(tokens[2], true);
        double lon = ddmm_to_decimal(tokens[4], false);
        if (tokens[3][0] == 'S') lat *= -1.0;
        if (tokens[5][0] == 'W') lon *= -1.0;
        g_fix.lat = lat;
        g_fix.lon = lon;
    }
    if (tokens[6]) g_fix.valid = atoi(tokens[6]) > 0;
    if (tokens[7]) g_fix.sats = atoi(tokens[7]);
    if (tokens[8]) g_fix.hdop = atof(tokens[8]);
    if (tokens[9]) g_fix.alt_m = atof(tokens[9]);
}

static void gps_task(void *arg) {
    (void)arg;
    uint8_t b;
    char line[128];
    int idx = 0;
    int64_t msg_ms = 0;

    while (1) {
        int r = uart_read_bytes(GPS_UART_NUM, &b, 1, pdMS_TO_TICKS(50));
        if (r == 1) {
            g_gps_rx_bytes++;
            if (b == '\n') {
                line[idx] = '\0';
                if (idx > 6 && line[0] == '$') {
                    g_gps_rx_lines++;
                    char tmp[128];
                    strncpy(tmp, line, sizeof(tmp) - 1);
                    tmp[sizeof(tmp) - 1] = '\0';
                    if (strstr(tmp, "RMC")) {
                        g_gps_rmc_lines++;
                        parse_rmc(tmp);
                    }
                    if (strstr(tmp, "GGA")) {
                        g_gps_gga_lines++;
                        parse_gga(tmp);
                    }
                }
                idx = 0;
            } else if (b != '\r' && idx < (int)sizeof(line) - 1) {
                line[idx++] = (char)b;
            }
        }

        static int64_t last_pub = 0;
        int64_t now = esp_log_timestamp();
        if ((now - last_pub) >= 1000) {
            last_pub = now;
            msg_ms = now;
            char out[192];
            snprintf(out, sizeof(out),
                     "GPS,%lld,%.6f,%.6f,%.2f,%d,%.2f,%s,%s,%d\n",
                     (long long)msg_ms, g_fix.lat, g_fix.lon, g_fix.alt_m, g_fix.sats, g_fix.hdop,
                     g_fix.date_utc, g_fix.time_utc, g_fix.valid ? 1 : 0);
            link_uart_write(out);
        }

        static int64_t last_diag = 0;
        if ((now - last_diag) >= 5000) {
            last_diag = now;
            ESP_LOGI(TAG,
                     "GPS diag: bytes=%" PRIu32 " lines=%" PRIu32 " rmc=%" PRIu32 " gga=%" PRIu32
                     " valid=%d sats=%d lat=%.6f lon=%.6f hdop=%.2f",
                     g_gps_rx_bytes,
                     g_gps_rx_lines,
                     g_gps_rmc_lines,
                     g_gps_gga_lines,
                     g_fix.valid ? 1 : 0,
                     g_fix.sats,
                     g_fix.lat,
                     g_fix.lon,
                     g_fix.hdop);
        }
    }
}

static void parse_beacon_for_network(const wifi_promiscuous_pkt_t *pkt,
                                     char ssid[33],
                                     uint8_t *channel,
                                     wifi_auth_mode_t *authmode,
                                     bool *ok) {
    *ok = false;
    ssid[0] = '\0';

    const uint8_t *frame = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if (len < 36) {
        return;
    }

    uint8_t frame_type = frame[0] & 0xFC;
    if (frame_type != 0x80) {
        return;
    }

    *channel = pkt->rx_ctrl.channel;
    *authmode = WIFI_AUTH_OPEN;

    const uint8_t *body = frame + 24 + 12;
    int body_len = len - 24 - 12;
    if (body_len < 2) {
        return;
    }

    int offset = 0;
    while (offset + 2 <= body_len) {
        uint8_t tag = body[offset];
        uint8_t tag_len = body[offset + 1];
        if (offset + 2 + tag_len > body_len) {
            break;
        }

        if (tag == 0 && tag_len > 0 && tag_len <= 32) {
            memcpy(ssid, &body[offset + 2], tag_len);
            ssid[tag_len] = '\0';
        } else if (tag == 3 && tag_len == 1) {
            *channel = body[offset + 2];
        } else if (tag == 48) {
            *authmode = WIFI_AUTH_WPA2_PSK;
        } else if (tag == 221) {
            if (tag_len >= 4 && body[offset + 2] == 0x00 && body[offset + 3] == 0x50 &&
                body[offset + 4] == 0xF2 && body[offset + 5] == 0x01) {
                if (*authmode == WIFI_AUTH_OPEN) {
                    *authmode = WIFI_AUTH_WPA_PSK;
                }
            }
        }

        offset += 2 + tag_len;
    }

    *ok = true;
}

static int ap24_find_by_bssid_unsafe(const uint8_t bssid[6]) {
    for (int i = 0; i < g_ap24_seen_count; i++) {
        if (memcmp(g_ap24_seen[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static bool ap24_upsert_unsafe(const uint8_t bssid[6],
                               const char *ssid,
                               uint8_t channel,
                               int8_t rssi,
                               wifi_auth_mode_t authmode) {
    int64_t now = esp_log_timestamp();
    int idx = ap24_find_by_bssid_unsafe(bssid);

    if (idx >= 0) {
        ap24_seen_t *entry = &g_ap24_seen[idx];
        bool should_resend = false;

        if (rssi > entry->rssi + AP24_RSSI_RESEND_DELTA_DB) {
            entry->rssi = rssi;
            should_resend = true;
        }
        if (channel != 0 && entry->channel != channel) {
            entry->channel = channel;
            should_resend = true;
        }
        if (ssid && ssid[0] != '\0' && strcmp(entry->ssid, ssid) != 0) {
            strncpy(entry->ssid, ssid, sizeof(entry->ssid) - 1);
            entry->ssid[sizeof(entry->ssid) - 1] = '\0';
            should_resend = true;
        }
        if (authmode != WIFI_AUTH_OPEN && entry->authmode != authmode) {
            entry->authmode = authmode;
            should_resend = true;
        }
        if ((now - entry->last_sent_ms) >= AP24_RESEND_INTERVAL_MS) {
            should_resend = true;
        }

        if (should_resend) {
            entry->dirty = true;
        }

        return false;
    }

    if (g_ap24_seen_count >= AP24_MAX_SEEN) {
        g_ap24_dropped++;
        return false;
    }

    ap24_seen_t *entry = &g_ap24_seen[g_ap24_seen_count++];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->bssid, bssid, 6);
    if (ssid && ssid[0]) {
        strncpy(entry->ssid, ssid, sizeof(entry->ssid) - 1);
        entry->ssid[sizeof(entry->ssid) - 1] = '\0';
    }
    entry->rssi = rssi;
    entry->channel = channel;
    entry->authmode = authmode;
    entry->last_sent_ms = 0;
    entry->dirty = true;

    return true;
}

static void ap24_rx_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT || !buf || !g_ap24_seen) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    char ssid[33];
    uint8_t channel = 0;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
    bool ok = false;

    parse_beacon_for_network(pkt, ssid, &channel, &authmode, &ok);
    if (!ok || channel == 0 || channel > 13) {
        return;
    }

    const uint8_t *bssid = &pkt->payload[10];

    portENTER_CRITICAL(&g_ap24_lock);
    bool is_new = ap24_upsert_unsafe(bssid, ssid, channel, (int8_t)pkt->rx_ctrl.rssi, authmode);
    if (is_new) {
        g_ap24_new_since_dwell++;
    }
    portEXIT_CRITICAL(&g_ap24_lock);
}

static void ap24_init_buffers(void) {
    g_ap24_seen = calloc(AP24_MAX_SEEN, sizeof(ap24_seen_t));
    g_ap24_flush_buf = calloc(AP24_FLUSH_BATCH, sizeof(ap24_seen_t));
    if (!g_ap24_seen || !g_ap24_flush_buf) {
        ESP_LOGE(TAG, "Failed to allocate 2.4GHz AP buffers");
        abort();
    }
}

static void ap24_ducb_init(void) {
    g_ap24_channel_count = 0;
    g_ap24_discounted_total = 0.0;

    for (int i = 0; i < (int)AP24_PRIMARY_COUNT; i++) {
        ap24_channel_t *slot = &g_ap24_channels[g_ap24_channel_count++];
        slot->channel = ap24_primary_channels[i];
        slot->tier = AP24_TIER_PRIMARY;
        slot->discounted_reward = 0.4;
        slot->discounted_pulls = 0.0;
        slot->total_pulls = 0;
    }

    for (int i = 0; i < (int)AP24_SECONDARY_COUNT; i++) {
        ap24_channel_t *slot = &g_ap24_channels[g_ap24_channel_count++];
        slot->channel = ap24_secondary_channels[i];
        slot->tier = AP24_TIER_SECONDARY;
        slot->discounted_reward = 0.0;
        slot->discounted_pulls = 0.0;
        slot->total_pulls = 0;
    }
}

static int ap24_ducb_select_index(void) {
    g_ap24_discounted_total *= AP24_DUCB_GAMMA;
    for (int i = 0; i < g_ap24_channel_count; i++) {
        g_ap24_channels[i].discounted_reward *= AP24_DUCB_GAMMA;
        g_ap24_channels[i].discounted_pulls *= AP24_DUCB_GAMMA;
    }

    int best_idx = 0;
    double best_ucb = -1.0;

    for (int i = 0; i < g_ap24_channel_count; i++) {
        if (g_ap24_channels[i].discounted_pulls < 0.001) {
            return i;
        }

        double avg_reward = g_ap24_channels[i].discounted_reward / g_ap24_channels[i].discounted_pulls;
        double exploration = AP24_DUCB_C * sqrt(log(g_ap24_discounted_total + 1.0) / g_ap24_channels[i].discounted_pulls);
        double ucb = avg_reward + exploration;

        if (ucb > best_ucb) {
            best_ucb = ucb;
            best_idx = i;
        }
    }

    return best_idx;
}

static void ap24_ducb_update(int channel_idx, double reward) {
    g_ap24_channels[channel_idx].discounted_pulls += 1.0;
    g_ap24_channels[channel_idx].discounted_reward += reward;
    g_ap24_channels[channel_idx].total_pulls++;
    g_ap24_discounted_total += 1.0;
}

static int ap24_dwell_ms_for_tier(ap24_tier_t tier) {
    switch (tier) {
        case AP24_TIER_PRIMARY:
            return AP24_DWELL_PRIMARY_MS;
        case AP24_TIER_SECONDARY:
        default:
            return AP24_DWELL_SECONDARY_MS;
    }
}

static int ap24_copy_dirty(ap24_seen_t *out, int out_cap) {
    if (!out || out_cap <= 0) {
        return 0;
    }

    int copied = 0;
    int64_t now = esp_log_timestamp();

    portENTER_CRITICAL(&g_ap24_lock);
    for (int i = 0; i < g_ap24_seen_count && copied < out_cap; i++) {
        if (!g_ap24_seen[i].dirty) {
            continue;
        }
        out[copied++] = g_ap24_seen[i];
        g_ap24_seen[i].dirty = false;
        g_ap24_seen[i].last_sent_ms = now;
    }
    portEXIT_CRITICAL(&g_ap24_lock);

    return copied;
}

static void ap24_format_bssid(const uint8_t bssid[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

static void ap24_hex_encode_ssid(const char *ssid, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (!ssid || ssid[0] == '\0') {
        snprintf(out, out_sz, "-");
        return;
    }

    size_t j = 0;
    for (size_t i = 0; ssid[i] != '\0' && i < 32 && j + 2 < out_sz; i++) {
        snprintf(&out[j], out_sz - j, "%02X", (unsigned char)ssid[i]);
        j += 2;
    }
    out[j] = '\0';
}

static void ap24_publish_dirty(void) {
    int copied = ap24_copy_dirty(g_ap24_flush_buf, AP24_FLUSH_BATCH);
    for (int i = 0; i < copied; i++) {
        char bssid[18];
        char ssid_hex[65];
        char line[160];

        ap24_format_bssid(g_ap24_flush_buf[i].bssid, bssid);
        ap24_hex_encode_ssid(g_ap24_flush_buf[i].ssid, ssid_hex, sizeof(ssid_hex));

        snprintf(line, sizeof(line), "AP24,%lld,%s,%u,%d,%d,%s\n",
                 (long long)esp_log_timestamp(),
                 bssid,
                 g_ap24_flush_buf[i].channel,
                 (int)g_ap24_flush_buf[i].rssi,
                 (int)g_ap24_flush_buf[i].authmode,
                 ssid_hex);
        link_uart_write(line);
    }
}

static void wifi24_task(void *arg) {
    (void)arg;

    ap24_ducb_init();

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(ap24_rx_callback));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    ESP_LOGI(TAG, "C6 2.4GHz scanner started: channels=%d primary_dwell=%dms secondary_dwell=%dms",
             g_ap24_channel_count, AP24_DWELL_PRIMARY_MS, AP24_DWELL_SECONDARY_MS);

    while (1) {
        int idx = ap24_ducb_select_index();
        int channel = g_ap24_channels[idx].channel;
        int dwell_ms = ap24_dwell_ms_for_tier(g_ap24_channels[idx].tier);

        esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "2.4GHz scanner: failed to set channel %d: %s", channel, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        portENTER_CRITICAL(&g_ap24_lock);
        g_ap24_new_since_dwell = 0;
        portEXIT_CRITICAL(&g_ap24_lock);

        vTaskDelay(pdMS_TO_TICKS(dwell_ms));

        uint32_t reward = 0;
        portENTER_CRITICAL(&g_ap24_lock);
        reward = g_ap24_new_since_dwell;
        portEXIT_CRITICAL(&g_ap24_lock);

        ap24_ducb_update(idx, (double)reward);
        ap24_publish_dirty();
    }
}

void app_main(void) {
    g_link_uart_lock = xSemaphoreCreateMutex();
    uart_init();
    ESP_LOGI(TAG, "C6 GPS bridge started");
    xTaskCreate(gps_task, "gps_task", 4096, NULL, 8, NULL);

    ESP_ERROR_CHECK(init_nvs_wifi());
    ap24_init_buffers();
    xTaskCreate(wifi24_task, "wifi24_task", 8192, NULL, 5, NULL);
}
