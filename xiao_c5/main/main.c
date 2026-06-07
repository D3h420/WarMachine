#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <sys/unistd.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

// C5 wiring:
// SD: SCK D8(GPIO8), MISO D9(GPIO9), MOSI D10(GPIO10), CS D2(GPIO25)
// C5 <- C6: D1(GPIO0 RX), optional C5 -> C6: D0(GPIO1 TX)

#define LINK_UART_NUM    UART_NUM_1
#define LINK_RX_GPIO     0
#define LINK_TX_GPIO     1
#define LINK_BAUD        115200

#define SD_MOSI_GPIO     10
#define SD_MISO_GPIO     9
#define SD_SCK_GPIO      8
#define SD_CS_GPIO       25

#define SCAN_PAUSE_MS             500
#define SD_RETRY_MS               2000
#define GPS_WAIT_POLL_MS          250
#define GPS_WAIT_LOG_MS           3000
#define GPS_STALE_MS              5000
#define PROMISC_FLUSH_INTERVAL_MS 30000
#define PROMISC_FLUSH_INTERVAL_AP 40
#define SCAN_FLUSH_INTERVAL_MS    15000

#define CHANNEL_TIME_MIN_LIMIT    250
#define CHANNEL_TIME_MAX_LIMIT    500
#define SCAN_TIME_NVS_NAMESPACE   "scancfg"
#define SCAN_TIME_NVS_KEY_MIN     "min_time"
#define SCAN_TIME_NVS_KEY_MAX     "max_time"

#define DEFAULT_SCAN_MIN_TIME_MS  80
#define DEFAULT_SCAN_MAX_TIME_MS  140

#define MAX_SEEN_APS              2048
#define FLUSH_BATCH_APS           96
#define WIFI_RSSI_RELOG_DELTA_DB  5
#define WIFI_LOCATION_RELOG_M     25.0f

static const char *TAG = "C5_LOGGER";
static const char *MOUNT_POINT = "/sdcard";
static char g_log_path[64] = "/sdcard/wardrive_1.log";
static const char *WIGLE_HEADER_1 =
    "WigleWifi-1.6,appRelease=v1.2,model=WarMachine,release=v1.1,device=C5&&C6,display=none,board=ESP32C5+ESP32C6,brand=D3h420";
static const char *WIGLE_HEADER_2 =
    "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type";

typedef struct {
    bool valid;
    int64_t msg_ms;
    float lat;
    float lon;
    float alt_m;
    int sats;
    float hdop;
    char date_utc[16];
    char time_utc[16];
} gps_state_t;

typedef enum {
    WARDRIVE_MODE_PROMISC = 0,
    WARDRIVE_MODE_SCAN = 1,
} wardrive_mode_t;

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t authmode;
    char first_seen[32];
    float lat;
    float lon;
    float alt_m;
    float hdop;
    bool gps_valid;
    bool dirty;
} seen_ap_t;

typedef enum {
    CH_TIER_5_NON_DFS = 0,
    CH_TIER_5_DFS,
} channel_tier_t;

typedef struct {
    int channel;
    channel_tier_t tier;
    double discounted_reward;
    double discounted_pulls;
    int total_pulls;
} ducb_channel_t;

static const uint8_t promisc_ch_5_non_dfs[]    = {36, 40, 44, 48, 149, 153, 157, 161, 165};
static const uint8_t promisc_ch_5_dfs[]        = {52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 169, 173, 177};

#define PROMISC_5_NON_DFS_COUNT    (sizeof(promisc_ch_5_non_dfs) / sizeof(promisc_ch_5_non_dfs[0]))
#define PROMISC_5_DFS_COUNT        (sizeof(promisc_ch_5_dfs) / sizeof(promisc_ch_5_dfs[0]))
#define PROMISC_TOTAL_CHANNELS     (PROMISC_5_NON_DFS_COUNT + PROMISC_5_DFS_COUNT)

#define DUCB_GAMMA                 0.99
#define DUCB_C                     1.0
#define DWELL_5_NON_DFS_MS         120
#define DWELL_5_DFS_MS             90

static bool g_sd_ready = false;
static bool g_sd_spi_bus_inited = false;
static bool g_nvs_ready = false;
static bool g_log_header_ready = false;

static gps_state_t g_gps = {0};
static int64_t g_last_gps_rx_local_ms = 0;

static uint32_t g_scan_min_channel_time = DEFAULT_SCAN_MIN_TIME_MS;
static uint32_t g_scan_max_channel_time = DEFAULT_SCAN_MAX_TIME_MS;
static wardrive_mode_t g_mode = WARDRIVE_MODE_PROMISC;

static seen_ap_t *g_seen_aps = NULL;
static seen_ap_t *g_flush_buf = NULL;
static int g_seen_count = 0;

static uint32_t g_total_new_aps = 0;
static uint32_t g_total_dedup_hits = 0;
static uint32_t g_total_dropped_aps = 0;
static uint32_t g_total_remote_24_aps = 0;
static volatile uint32_t g_promisc_new_since_dwell = 0;

static ducb_channel_t g_ducb_channels[PROMISC_TOTAL_CHANNELS];
static int g_ducb_channel_count = 0;
static double g_ducb_discounted_total = 0.0;

static portMUX_TYPE g_gps_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_cfg_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_seen_lock = portMUX_INITIALIZER_UNLOCKED;

static bool seen_upsert_unsafe(const uint8_t bssid[6],
                               const char *ssid,
                               uint8_t channel,
                               int8_t rssi,
                               wifi_auth_mode_t authmode,
                               const gps_state_t *gps,
                               bool gps_valid);

static const char *authmode_to_wigle(wifi_auth_mode_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:
            return "[Open]";
        case WIFI_AUTH_WEP:
            return "[WEP]";
        case WIFI_AUTH_WPA_PSK:
            return "[WPA_PSK]";
        case WIFI_AUTH_WPA2_PSK:
            return "[WPA2_PSK]";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "[WPA_WPA2_PSK]";
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "[WPA2_ENT]";
#ifdef WIFI_AUTH_WPA3_PSK
        case WIFI_AUTH_WPA3_PSK:
            return "[WPA3_PSK]";
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "[WPA2_WPA3_PSK]";
#endif
#ifdef WIFI_AUTH_WAPI_PSK
        case WIFI_AUTH_WAPI_PSK:
            return "[WAPI_PSK]";
#endif
#ifdef WIFI_AUTH_OWE
        case WIFI_AUTH_OWE:
            return "[OWE]";
#endif
#ifdef WIFI_AUTH_WPA3_ENT_192
        case WIFI_AUTH_WPA3_ENT_192:
            return "[WPA3_ENT_192]";
#endif
        default:
            return "[UNKNOWN]";
    }
}

static wardrive_mode_t get_mode(void)
{
    wardrive_mode_t mode;
    portENTER_CRITICAL(&g_cfg_lock);
    mode = g_mode;
    portEXIT_CRITICAL(&g_cfg_lock);
    return mode;
}

static void set_mode(wardrive_mode_t mode)
{
    portENTER_CRITICAL(&g_cfg_lock);
    g_mode = mode;
    portEXIT_CRITICAL(&g_cfg_lock);
}

static void get_channel_times(uint32_t *min_ms, uint32_t *max_ms)
{
    portENTER_CRITICAL(&g_cfg_lock);
    if (min_ms) *min_ms = g_scan_min_channel_time;
    if (max_ms) *max_ms = g_scan_max_channel_time;
    portEXIT_CRITICAL(&g_cfg_lock);
}

static void set_channel_min_time(uint32_t value_ms)
{
    portENTER_CRITICAL(&g_cfg_lock);
    g_scan_min_channel_time = value_ms;
    if (g_scan_min_channel_time > g_scan_max_channel_time) {
        g_scan_max_channel_time = g_scan_min_channel_time;
    }
    portEXIT_CRITICAL(&g_cfg_lock);
}

static void set_channel_max_time(uint32_t value_ms)
{
    portENTER_CRITICAL(&g_cfg_lock);
    g_scan_max_channel_time = value_ms;
    if (g_scan_max_channel_time < g_scan_min_channel_time) {
        g_scan_min_channel_time = g_scan_max_channel_time;
    }
    portEXIT_CRITICAL(&g_cfg_lock);
}

static void csv_escape_ssid(const uint8_t *raw_ssid, size_t raw_len, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < raw_len && j + 2 < out_sz; i++) {
        char c = (char)raw_ssid[i];
        if (c == '\0') break;
        if (c == '"') {
            if (j + 2 >= out_sz) break;
            out[j++] = '"';
            out[j++] = '"';
        } else if ((unsigned char)c >= 32) {
            out[j++] = c;
        }
    }
    if (j < out_sz - 1) out[j++] = '"';
    out[j] = '\0';
}

static void format_first_seen(const gps_state_t *gps, char out[32])
{
    if (!out) return;
    const char *date = "1970-01-01";
    const char *time = "00:00:00";

    if (gps && gps->date_utc[0]) {
        date = gps->date_utc;
    }
    if (gps && gps->time_utc[0]) {
        time = gps->time_utc;
    }
    snprintf(out, 32, "%s %s", date, time);
}

static int channel_to_frequency_mhz(uint8_t channel)
{
    if (channel >= 1 && channel <= 13) {
        return 2407 + ((int)channel * 5);
    }
    if (channel == 14) {
        return 2484;
    }
    if (channel >= 32 && channel <= 177) {
        return 5000 + ((int)channel * 5);
    }
    return 0;
}

static void select_next_log_path(void)
{
    for (int index = 1; index < 10000; index++) {
        snprintf(g_log_path, sizeof(g_log_path), "%s/wardrive_%d.log", MOUNT_POINT, index);
        if (access(g_log_path, F_OK) != 0) {
            ESP_LOGI(TAG, "Selected new wardrive session log: %s", g_log_path);
            return;
        }
    }

    snprintf(g_log_path, sizeof(g_log_path), "%s/wardrive_overflow.log", MOUNT_POINT);
    ESP_LOGW(TAG, "Log index limit reached, using fallback log: %s", g_log_path);
}

static float gps_distance_m(float lat_a, float lon_a, float lat_b, float lon_b)
{
    const float deg_to_rad = 0.01745329251994329577f;
    const float earth_radius_m = 6371000.0f;
    float lat_a_rad = lat_a * deg_to_rad;
    float lat_b_rad = lat_b * deg_to_rad;
    float x = (lon_b - lon_a) * deg_to_rad * cosf((lat_a_rad + lat_b_rad) * 0.5f);
    float y = (lat_b - lat_a) * deg_to_rad;
    return sqrtf((x * x) + (y * y)) * earth_radius_m;
}

static void gps_snapshot(gps_state_t *out, int64_t *last_rx_ms)
{
    if (!out) return;
    portENTER_CRITICAL(&g_gps_lock);
    *out = g_gps;
    if (last_rx_ms) {
        *last_rx_ms = g_last_gps_rx_local_ms;
    }
    portEXIT_CRITICAL(&g_gps_lock);
}

static bool gps_is_fresh_and_valid(gps_state_t *snapshot)
{
    gps_state_t local = {0};
    int64_t last_rx = 0;
    gps_snapshot(&local, &last_rx);

    if (snapshot) {
        *snapshot = local;
    }

    bool coords_present = (fabsf(local.lat) > 0.000001f) || (fabsf(local.lon) > 0.000001f);
    int64_t now = esp_log_timestamp();
    bool got_any_frame = last_rx > 0;
    bool gps_fresh = got_any_frame && ((now - last_rx) <= GPS_STALE_MS);

    return gps_fresh && local.valid && (local.sats > 0) && coords_present;
}

static void wait_for_gps_fix_startup(void)
{
    int64_t last_log_ms = -GPS_WAIT_LOG_MS;
    while (1) {
        gps_state_t gps = {0};
        int64_t last_rx = 0;
        gps_snapshot(&gps, &last_rx);

        int64_t now = esp_log_timestamp();
        bool got_any_frame = last_rx > 0;
        bool gps_fresh = got_any_frame && ((now - last_rx) <= GPS_STALE_MS);
        bool coords_present = (fabsf(gps.lat) > 0.000001f) || (fabsf(gps.lon) > 0.000001f);
        bool ready = gps_fresh && gps.valid && (gps.sats > 0) && coords_present;

        if (ready) {
            ESP_LOGI(TAG, "GPS fix ready: lat=%.6f lon=%.6f sats=%d hdop=%.2f",
                     gps.lat, gps.lon, gps.sats, gps.hdop);
            return;
        }

        if ((now - last_log_ms) >= GPS_WAIT_LOG_MS) {
            if (!got_any_frame) {
                ESP_LOGW(TAG, "Waiting for GPS data from C6 (no frames yet)");
            } else if (!gps_fresh) {
                ESP_LOGW(TAG, "Waiting for fresh GPS data (last frame %lld ms ago)",
                         (long long)(now - last_rx));
            } else {
                ESP_LOGW(TAG, "Waiting for GPS fix: valid=%d sats=%d lat=%.6f lon=%.6f",
                         gps.valid ? 1 : 0, gps.sats, gps.lat, gps.lon);
            }
            last_log_ms = now;
        }

        vTaskDelay(pdMS_TO_TICKS(GPS_WAIT_POLL_MS));
    }
}

static void wait_for_gps_recovery(void)
{
    ESP_LOGW(TAG, "GPS fix lost - pausing wardrive until fix recovers...");
    int64_t last_log_ms = 0;

    while (1) {
        gps_state_t gps = {0};
        if (gps_is_fresh_and_valid(&gps)) {
            ESP_LOGI(TAG, "GPS fix recovered: lat=%.6f lon=%.6f sats=%d hdop=%.2f",
                     gps.lat, gps.lon, gps.sats, gps.hdop);
            return;
        }

        int64_t now = esp_log_timestamp();
        if ((now - last_log_ms) >= GPS_WAIT_LOG_MS) {
            ESP_LOGW(TAG, "Still waiting for GPS recovery...");
            last_log_ms = now;
        }

        vTaskDelay(pdMS_TO_TICKS(GPS_WAIT_POLL_MS));
    }
}

static esp_err_t init_nvs_wifi(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    g_nvs_ready = true;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY));

    return ESP_OK;
}

static esp_err_t init_sd_spi(sdmmc_card_t **out_card)
{
    gpio_set_pull_mode(SD_MOSI_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_MISO_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_SCK_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_CS_GPIO, GPIO_PULLUP_ONLY);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_GPIO,
        .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    if (!g_sd_spi_bus_inited) {
        esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }
        g_sd_spi_bus_inited = true;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_GPIO;
    slot_config.host_id = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 4000;

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        return ret;
    }

    *out_card = card;
    return ESP_OK;
}

static bool ensure_log_header(void)
{
    if (g_log_header_ready) {
        return true;
    }

    FILE *f = fopen(g_log_path, "r");
    if (f) {
        char line1[256] = {0};
        char line2[256] = {0};
        char *r1 = fgets(line1, sizeof(line1), f);
        char *r2 = fgets(line2, sizeof(line2), f);
        fclose(f);
        if (r1 && r2 &&
            strncmp(line1, "WigleWifi-1.6,", strlen("WigleWifi-1.6,")) == 0 &&
            strncmp(line2, WIGLE_HEADER_2, strlen(WIGLE_HEADER_2)) == 0) {
            g_log_header_ready = true;
            return true;
        }
        ESP_LOGW(TAG, "Existing log is not WiGLE format, recreating %s", g_log_path);
    }

    f = fopen(g_log_path, "w");
    if (!f) {
        return false;
    }
    fprintf(f, "%s\n", WIGLE_HEADER_1);
    fprintf(f, "%s\n", WIGLE_HEADER_2);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    g_log_header_ready = true;
    ESP_LOGI(TAG, "Created WiGLE log header: %s", g_log_path);
    return true;
}

static void init_link_uart(void)
{
    uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(LINK_UART_NUM, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART_NUM, LINK_TX_GPIO, LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void parse_gps_line(const char *line)
{
    if (strncmp(line, "GPS,", 4) != 0) {
        return;
    }

    char buf[220];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *t[9] = {0};
    int n = 0;
    char *p = strtok(buf + 4, ",");
    while (p && n < 9) {
        t[n++] = p;
        p = strtok(NULL, ",");
    }
    if (n != 9) {
        return;
    }

    gps_state_t next = {0};
    next.msg_ms = atoll(t[0]);
    next.lat = strtof(t[1], NULL);
    next.lon = strtof(t[2], NULL);
    next.alt_m = strtof(t[3], NULL);
    next.sats = atoi(t[4]);
    next.hdop = strtof(t[5], NULL);
    strncpy(next.date_utc, t[6], sizeof(next.date_utc) - 1);
    next.date_utc[sizeof(next.date_utc) - 1] = '\0';
    strncpy(next.time_utc, t[7], sizeof(next.time_utc) - 1);
    next.time_utc[sizeof(next.time_utc) - 1] = '\0';
    next.valid = atoi(t[8]) == 1;

    portENTER_CRITICAL(&g_gps_lock);
    g_gps = next;
    g_last_gps_rx_local_ms = esp_log_timestamp();
    portEXIT_CRITICAL(&g_gps_lock);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_bssid_text(const char *text, uint8_t out[6])
{
    if (!text || strlen(text) != 17) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        int hi = hex_nibble(text[i * 3]);
        int lo = hex_nibble(text[i * 3 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
        if (i < 5 && text[i * 3 + 2] != ':') {
            return false;
        }
    }

    return true;
}

static bool decode_ssid_hex(const char *hex, char out[33])
{
    if (!out) {
        return false;
    }

    out[0] = '\0';
    if (!hex || strcmp(hex, "-") == 0) {
        return true;
    }

    size_t len = strlen(hex);
    if ((len % 2) != 0 || len > 64) {
        return false;
    }

    size_t out_len = len / 2;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (char)((hi << 4) | lo);
    }
    out[out_len] = '\0';

    return true;
}

static void parse_remote_ap24_line(const char *line)
{
    if (strncmp(line, "AP24,", 5) != 0 || !g_seen_aps) {
        return;
    }

    gps_state_t gps = {0};
    bool gps_ok = gps_is_fresh_and_valid(&gps);
    if (!gps_ok) {
        return;
    }

    char buf[180];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *t[6] = {0};
    int n = 0;
    char *p = strtok(buf + 5, ",");
    while (p && n < 6) {
        t[n++] = p;
        p = strtok(NULL, ",");
    }
    if (n != 6) {
        return;
    }

    uint8_t bssid[6];
    char ssid[33];
    if (!parse_bssid_text(t[1], bssid) || !decode_ssid_hex(t[5], ssid)) {
        return;
    }

    long channel = strtol(t[2], NULL, 10);
    long rssi = strtol(t[3], NULL, 10);
    long auth = strtol(t[4], NULL, 10);
    if (channel < 1 || channel > 14 || rssi < -127 || rssi > 0) {
        return;
    }

    portENTER_CRITICAL(&g_seen_lock);
    seen_upsert_unsafe(
        bssid,
        ssid,
        (uint8_t)channel,
        (int8_t)rssi,
        (wifi_auth_mode_t)auth,
        &gps,
        gps_ok
    );
    g_total_remote_24_aps++;
    portEXIT_CRITICAL(&g_seen_lock);
}

static void wait_for_sd_ready(void)
{
    int attempt = 0;
    while (!g_sd_ready) {
        attempt++;
        sdmmc_card_t *card = NULL;
        esp_err_t sd_ret = init_sd_spi(&card);
        if (sd_ret == ESP_OK) {
            g_sd_ready = true;
            sdmmc_card_print_info(stdout, card);
            select_next_log_path();
            ESP_LOGI(TAG, "SD ready after %d attempt(s)", attempt);
            return;
        }

        ESP_LOGW(TAG, "SD init attempt %d failed: %s (0x%x), retry in %d ms",
                 attempt, esp_err_to_name(sd_ret), (unsigned int)sd_ret, SD_RETRY_MS);
        vTaskDelay(pdMS_TO_TICKS(SD_RETRY_MS));
    }
}

static void channel_time_load_state_from_nvs(void)
{
    if (!g_nvs_ready) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCAN_TIME_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Channel time NVS read open failed: %s", esp_err_to_name(err));
        return;
    }

    uint32_t min_val = 0, max_val = 0;
    err = nvs_get_u32(handle, SCAN_TIME_NVS_KEY_MIN, &min_val);
    if (err == ESP_OK && min_val >= 1 && min_val <= CHANNEL_TIME_MIN_LIMIT) {
        set_channel_min_time(min_val);
    }
    err = nvs_get_u32(handle, SCAN_TIME_NVS_KEY_MAX, &max_val);
    if (err == ESP_OK && max_val >= 1 && max_val <= CHANNEL_TIME_MAX_LIMIT) {
        set_channel_max_time(max_val);
    }

    nvs_close(handle);

    uint32_t min_ms = 0, max_ms = 0;
    get_channel_times(&min_ms, &max_ms);
    ESP_LOGI(TAG, "Loaded channel_time from NVS: min=%u max=%u", (unsigned)min_ms, (unsigned)max_ms);
}

static void channel_time_persist_state(void)
{
    if (!g_nvs_ready) {
        return;
    }

    uint32_t min_ms = 0, max_ms = 0;
    get_channel_times(&min_ms, &max_ms);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCAN_TIME_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Channel time NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u32(handle, SCAN_TIME_NVS_KEY_MIN, min_ms);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, SCAN_TIME_NVS_KEY_MAX, max_ms);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Channel time NVS save failed: %s", esp_err_to_name(err));
    }
}

static int seen_find_by_bssid_unsafe(const uint8_t bssid[6])
{
    for (int i = 0; i < g_seen_count; i++) {
        if (memcmp(g_seen_aps[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static bool seen_upsert_unsafe(const uint8_t bssid[6],
                               const char *ssid,
                               uint8_t channel,
                               int8_t rssi,
                               wifi_auth_mode_t authmode,
                               const gps_state_t *gps,
                               bool gps_valid)
{
    int idx = seen_find_by_bssid_unsafe(bssid);
    if (idx >= 0) {
        seen_ap_t *entry = &g_seen_aps[idx];
        bool should_relog = false;

        if (abs((int)rssi - (int)entry->rssi) >= WIFI_RSSI_RELOG_DELTA_DB) {
            entry->rssi = rssi;
            should_relog = true;
        }
        if (channel != 0 && entry->channel != channel) {
            entry->channel = channel;
            should_relog = true;
        }
        if (ssid && ssid[0] != '\0' && strcmp(entry->ssid, ssid) != 0) {
            strncpy(entry->ssid, ssid, sizeof(entry->ssid) - 1);
            entry->ssid[sizeof(entry->ssid) - 1] = '\0';
            should_relog = true;
        }
        if (authmode != WIFI_AUTH_OPEN && entry->authmode != authmode) {
            entry->authmode = authmode;
            should_relog = true;
        }
        if (gps_valid && gps) {
            if (!entry->gps_valid) {
                should_relog = true;
            } else {
                float moved_m = gps_distance_m(entry->lat, entry->lon, gps->lat, gps->lon);
                if (moved_m >= WIFI_LOCATION_RELOG_M) {
                    should_relog = true;
                }
            }
        }

        if (should_relog) {
            if (gps_valid && gps) {
                entry->lat = gps->lat;
                entry->lon = gps->lon;
                entry->alt_m = gps->alt_m;
                entry->hdop = gps->hdop;
                entry->gps_valid = true;
            }
            entry->dirty = true;
        }

        g_total_dedup_hits++;
        return false;
    }

    if (g_seen_count >= MAX_SEEN_APS) {
        g_total_dropped_aps++;
        return false;
    }

    seen_ap_t *entry = &g_seen_aps[g_seen_count++];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->bssid, bssid, 6);

    if (ssid && ssid[0]) {
        strncpy(entry->ssid, ssid, sizeof(entry->ssid) - 1);
        entry->ssid[sizeof(entry->ssid) - 1] = '\0';
    }

    entry->rssi = rssi;
    entry->channel = channel;
    entry->authmode = authmode;
    entry->dirty = true;

    if (gps_valid && gps) {
        entry->lat = gps->lat;
        entry->lon = gps->lon;
        entry->alt_m = gps->alt_m;
        entry->hdop = gps->hdop;
        entry->gps_valid = true;
    }

    format_first_seen(gps, entry->first_seen);

    g_total_new_aps++;
    return true;
}

static void parse_beacon_for_network(const wifi_promiscuous_pkt_t *pkt,
                                     char ssid[33],
                                     uint8_t *channel,
                                     wifi_auth_mode_t *authmode,
                                     bool *ok)
{
    *ok = false;
    ssid[0] = '\0';

    const uint8_t *frame = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if (len < 36) {
        return;
    }

    uint8_t frame_type = frame[0] & 0xFC;
    if (frame_type != 0x80) { // beacon only
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

static void promisc_rx_callback(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT || !buf) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    char ssid[33];
    uint8_t channel = 0;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
    bool ok = false;

    parse_beacon_for_network(pkt, ssid, &channel, &authmode, &ok);
    if (!ok) {
        return;
    }

    const uint8_t *bssid = &pkt->payload[10];

    gps_state_t gps = {0};
    bool gps_valid = gps_is_fresh_and_valid(&gps);
    if (!gps_valid) {
        return;
    }

    portENTER_CRITICAL(&g_seen_lock);
    bool is_new = seen_upsert_unsafe(
        bssid,
        ssid,
        channel,
        (int8_t)pkt->rx_ctrl.rssi,
        authmode,
        &gps,
        gps_valid
    );
    if (is_new) {
        g_promisc_new_since_dwell++;
    }
    portEXIT_CRITICAL(&g_seen_lock);
}

static void ducb_init(void)
{
    g_ducb_channel_count = 0;
    g_ducb_discounted_total = 0.0;

    for (int i = 0; i < (int)PROMISC_5_NON_DFS_COUNT; i++) {
        ducb_channel_t *slot = &g_ducb_channels[g_ducb_channel_count++];
        slot->channel = promisc_ch_5_non_dfs[i];
        slot->tier = CH_TIER_5_NON_DFS;
        slot->discounted_reward = 0.25;
        slot->discounted_pulls = 0.0;
        slot->total_pulls = 0;
    }

    for (int i = 0; i < (int)PROMISC_5_DFS_COUNT; i++) {
        ducb_channel_t *slot = &g_ducb_channels[g_ducb_channel_count++];
        slot->channel = promisc_ch_5_dfs[i];
        slot->tier = CH_TIER_5_DFS;
        slot->discounted_reward = 0.0;
        slot->discounted_pulls = 0.0;
        slot->total_pulls = 0;
    }
}

static int ducb_select_index(void)
{
    g_ducb_discounted_total *= DUCB_GAMMA;
    for (int i = 0; i < g_ducb_channel_count; i++) {
        g_ducb_channels[i].discounted_reward *= DUCB_GAMMA;
        g_ducb_channels[i].discounted_pulls *= DUCB_GAMMA;
    }

    int best_idx = 0;
    double best_ucb = -1.0;

    for (int i = 0; i < g_ducb_channel_count; i++) {
        if (g_ducb_channels[i].discounted_pulls < 0.001) {
            return i;
        }

        double avg_reward = g_ducb_channels[i].discounted_reward / g_ducb_channels[i].discounted_pulls;
        double exploration = DUCB_C * sqrt(log(g_ducb_discounted_total + 1.0) / g_ducb_channels[i].discounted_pulls);
        double ucb = avg_reward + exploration;

        if (ucb > best_ucb) {
            best_ucb = ucb;
            best_idx = i;
        }
    }

    return best_idx;
}

static void ducb_update(int channel_idx, double reward)
{
    g_ducb_channels[channel_idx].discounted_pulls += 1.0;
    g_ducb_channels[channel_idx].discounted_reward += reward;
    g_ducb_channels[channel_idx].total_pulls++;
    g_ducb_discounted_total += 1.0;
}

static int dwell_ms_for_tier(channel_tier_t tier)
{
    switch (tier) {
        case CH_TIER_5_NON_DFS:
            return DWELL_5_NON_DFS_MS;
        case CH_TIER_5_DFS:
            return DWELL_5_DFS_MS;
        default:
            return DWELL_5_NON_DFS_MS;
    }
}

static int copy_dirty_entries(seen_ap_t *out, int out_cap)
{
    if (!out || out_cap <= 0) {
        return 0;
    }

    int copied = 0;

    portENTER_CRITICAL(&g_seen_lock);
    for (int i = 0; i < g_seen_count && copied < out_cap; i++) {
        if (!g_seen_aps[i].dirty) {
            continue;
        }
        out[copied++] = g_seen_aps[i];
        g_seen_aps[i].dirty = false;
    }
    portEXIT_CRITICAL(&g_seen_lock);

    return copied;
}

static void write_entries_to_log(const seen_ap_t *entries, int count)
{
    if (!entries || count <= 0 || !g_sd_ready) {
        return;
    }
    if (!ensure_log_header()) {
        ESP_LOGE(TAG, "Cannot prepare WiGLE log header: %s", g_log_path);
        return;
    }

    FILE *f = fopen(g_log_path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", g_log_path);
        return;
    }

    for (int i = 0; i < count; i++) {
        char mac[18];
        char ssid_esc[80];

        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 entries[i].bssid[0], entries[i].bssid[1], entries[i].bssid[2],
                 entries[i].bssid[3], entries[i].bssid[4], entries[i].bssid[5]);

        csv_escape_ssid((const uint8_t *)entries[i].ssid, strnlen(entries[i].ssid, 32), ssid_esc, sizeof(ssid_esc));
        int frequency_mhz = channel_to_frequency_mhz(entries[i].channel);

        fprintf(f, "%s,%s,%s,%s,%u,%d,%d,%.7f,%.7f,%.2f,%.2f,,,WIFI\n",
                mac,
                ssid_esc,
                authmode_to_wigle(entries[i].authmode),
                entries[i].first_seen[0] ? entries[i].first_seen : "1970-01-01 00:00:00",
                entries[i].channel,
                frequency_mhz,
                (int)entries[i].rssi,
                entries[i].gps_valid ? entries[i].lat : 0.0f,
                entries[i].gps_valid ? entries[i].lon : 0.0f,
                entries[i].gps_valid ? entries[i].alt_m : 0.0f,
                entries[i].gps_valid ? entries[i].hdop : 0.0f);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void flush_dirty_entries_if_needed(bool force, uint32_t min_dirty, uint32_t min_interval_ms)
{
    static uint32_t last_flush_ms = 0;

    uint32_t dirty_count = 0;
    uint32_t seen_count = 0;
    uint32_t dedup_hits = 0;
    uint32_t dropped = 0;

    portENTER_CRITICAL(&g_seen_lock);
    for (int i = 0; i < g_seen_count; i++) {
        if (g_seen_aps[i].dirty) {
            dirty_count++;
        }
    }
    seen_count = (uint32_t)g_seen_count;
    dedup_hits = g_total_dedup_hits;
    dropped = g_total_dropped_aps;
    portEXIT_CRITICAL(&g_seen_lock);

    if (dirty_count == 0) {
        return;
    }

    uint32_t now_ms = (uint32_t)esp_log_timestamp();
    bool interval_ok = (last_flush_ms == 0) || ((now_ms - last_flush_ms) >= min_interval_ms);

    if (!force && dirty_count < min_dirty && !interval_ok) {
        return;
    }

    int copied = copy_dirty_entries(g_flush_buf, FLUSH_BATCH_APS);
    if (copied <= 0) {
        return;
    }

    write_entries_to_log(g_flush_buf, copied);
    last_flush_ms = now_ms;

    ESP_LOGI(TAG,
             "Flushed %d AP rows to log (seen=%" PRIu32 ", dedup=%" PRIu32 ", dropped=%" PRIu32 ")",
             copied, seen_count, dedup_hits, dropped);
}

static uint32_t run_scan_channel(uint8_t channel, const gps_state_t *gps, bool gps_ok, uint32_t min_ms, uint32_t max_ms)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = channel,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = min_ms,
        .scan_time.active.max = max_ms,
    };

    esp_err_t s = esp_wifi_scan_start(&scan_cfg, true);
    if (s != ESP_OK) {
        ESP_LOGW(TAG, "5GHz scan start failed on channel %u: %s", (unsigned)channel, esp_err_to_name(s));
        vTaskDelay(pdMS_TO_TICKS(50));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_err_t c = esp_wifi_scan_get_ap_num(&ap_count);
    if (c != ESP_OK) {
        ESP_LOGW(TAG, "5GHz scan get ap num failed on channel %u: %s", (unsigned)channel, esp_err_to_name(c));
        return 0;
    }
    if (ap_count == 0) {
        return 0;
    }

    wifi_ap_record_t *recs = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!recs) {
        ESP_LOGE(TAG, "scan alloc failed for %u records", (unsigned)ap_count);
        return 0;
    }

    c = esp_wifi_scan_get_ap_records(&ap_count, recs);
    if (c != ESP_OK) {
        ESP_LOGW(TAG, "5GHz scan get records failed on channel %u: %s", (unsigned)channel, esp_err_to_name(c));
        free(recs);
        return 0;
    }

    portENTER_CRITICAL(&g_seen_lock);
    for (int i = 0; i < ap_count; i++) {
        char ssid_local[33] = {0};
        size_t ssid_len = strnlen((const char *)recs[i].ssid, sizeof(recs[i].ssid));
        if (ssid_len > 32) ssid_len = 32;
        memcpy(ssid_local, recs[i].ssid, ssid_len);
        ssid_local[ssid_len] = '\0';

        seen_upsert_unsafe(
            recs[i].bssid,
            ssid_local,
            recs[i].primary,
            (int8_t)recs[i].rssi,
            recs[i].authmode,
            gps,
            gps_ok
        );
    }
    portEXIT_CRITICAL(&g_seen_lock);

    free(recs);
    return ap_count;
}

static void run_scan_cycle(void)
{
    gps_state_t gps = {0};
    bool gps_ok = gps_is_fresh_and_valid(&gps);
    if (!gps_ok) {
        wait_for_gps_recovery();
        gps_ok = gps_is_fresh_and_valid(&gps);
    }

    uint32_t min_ms = 0;
    uint32_t max_ms = 0;
    get_channel_times(&min_ms, &max_ms);

    uint32_t total_ap_count = 0;
    for (int i = 0; i < (int)PROMISC_5_NON_DFS_COUNT; i++) {
        total_ap_count += run_scan_channel(promisc_ch_5_non_dfs[i], &gps, gps_ok, min_ms, max_ms);
    }
    for (int i = 0; i < (int)PROMISC_5_DFS_COUNT; i++) {
        total_ap_count += run_scan_channel(promisc_ch_5_dfs[i], &gps, gps_ok, min_ms, max_ms);
    }

    ESP_LOGI(TAG, "5GHz scan mode: processed %" PRIu32 " APs (channel_time min=%u max=%u)",
             total_ap_count, (unsigned)min_ms, (unsigned)max_ms);
}

static void run_promisc_cycle(void)
{
    gps_state_t gps = {0};
    if (!gps_is_fresh_and_valid(&gps)) {
        wait_for_gps_recovery();
    }

    int idx = ducb_select_index();
    int channel = g_ducb_channels[idx].channel;
    int dwell_ms = dwell_ms_for_tier(g_ducb_channels[idx].tier);

    esp_err_t e = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "promisc: failed to set channel %d: %s", channel, esp_err_to_name(e));
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }

    g_promisc_new_since_dwell = 0;
    vTaskDelay(pdMS_TO_TICKS(dwell_ms));

    ducb_update(idx, (double)g_promisc_new_since_dwell);
}

static void init_seen_buffers(void)
{
    g_seen_aps = calloc(MAX_SEEN_APS, sizeof(seen_ap_t));
    g_flush_buf = calloc(FLUSH_BATCH_APS, sizeof(seen_ap_t));
    if (!g_seen_aps || !g_flush_buf) {
        ESP_LOGE(TAG, "Failed to allocate dedup buffers");
        abort();
    }
}

static void print_help(void)
{
    printf("\nCommands:\n");
    printf("  C5 scans/logs 5GHz; C6 streams 2.4GHz as AP24 records\n");
    printf("  help\n");
    printf("  status\n");
    printf("  mode read\n");
    printf("  mode set promisc\n");
    printf("  mode set scan\n");
    printf("  channel_time read min|max\n");
    printf("  channel_time set min|max <ms>\n\n");
}

static void print_status(void)
{
    uint32_t min_ms = 0, max_ms = 0;
    get_channel_times(&min_ms, &max_ms);

    const char *mode = (get_mode() == WARDRIVE_MODE_PROMISC) ? "promisc" : "scan";

    gps_state_t gps = {0};
    bool gps_ok = gps_is_fresh_and_valid(&gps);

    uint32_t seen = 0;
    uint32_t dirty = 0;
    uint32_t dedup = 0;
    uint32_t discovered = 0;
    uint32_t dropped = 0;
    uint32_t remote24 = 0;

    portENTER_CRITICAL(&g_seen_lock);
    seen = (uint32_t)g_seen_count;
    discovered = g_total_new_aps;
    dedup = g_total_dedup_hits;
    dropped = g_total_dropped_aps;
    remote24 = g_total_remote_24_aps;
    for (int i = 0; i < g_seen_count; i++) {
        if (g_seen_aps[i].dirty) {
            dirty++;
        }
    }
    portEXIT_CRITICAL(&g_seen_lock);

    ESP_LOGI(TAG,
             "status: mode=%s gps=%s lat=%.6f lon=%.6f sats=%d log=%s channel_time[min=%u,max=%u] seen=%" PRIu32 " dirty=%" PRIu32 " new=%" PRIu32 " remote24=%" PRIu32 " dedup=%" PRIu32 " dropped=%" PRIu32,
             mode,
             gps_ok ? "ok" : "lost",
             gps.lat,
             gps.lon,
             gps.sats,
             g_log_path,
             (unsigned)min_ms,
             (unsigned)max_ms,
             seen,
             dirty,
             discovered,
             remote24,
             dedup,
             dropped);
}

static void command_task(void *arg)
{
    (void)arg;

    print_help();

    char line[160];
    while (1) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        char *argv[8] = {0};
        int argc = 0;
        char *tok = strtok(line, " \t");
        while (tok && argc < 8) {
            argv[argc++] = tok;
            tok = strtok(NULL, " \t");
        }

        if (argc == 0) {
            continue;
        }

        if (strcasecmp(argv[0], "help") == 0) {
            print_help();
            continue;
        }

        if (strcasecmp(argv[0], "status") == 0) {
            print_status();
            continue;
        }

        if (strcasecmp(argv[0], "mode") == 0) {
            if (argc == 2 && strcasecmp(argv[1], "read") == 0) {
                ESP_LOGI(TAG, "mode=%s", get_mode() == WARDRIVE_MODE_PROMISC ? "promisc" : "scan");
                continue;
            }

            if (argc == 3 && strcasecmp(argv[1], "set") == 0) {
                if (strcasecmp(argv[2], "promisc") == 0) {
                    set_mode(WARDRIVE_MODE_PROMISC);
                    ESP_LOGI(TAG, "mode set to promisc");
                } else if (strcasecmp(argv[2], "scan") == 0) {
                    set_mode(WARDRIVE_MODE_SCAN);
                    ESP_LOGI(TAG, "mode set to scan");
                } else {
                    ESP_LOGW(TAG, "Unknown mode '%s'", argv[2]);
                }
                continue;
            }

            ESP_LOGI(TAG, "Usage: mode read | mode set promisc|scan");
            continue;
        }

        if (strcasecmp(argv[0], "channel_time") == 0) {
            if (argc >= 3 && strcasecmp(argv[1], "read") == 0) {
                uint32_t min_ms = 0, max_ms = 0;
                get_channel_times(&min_ms, &max_ms);

                if (strcasecmp(argv[2], "min") == 0) {
                    ESP_LOGI(TAG, "%u", (unsigned)min_ms);
                } else if (strcasecmp(argv[2], "max") == 0) {
                    ESP_LOGI(TAG, "%u", (unsigned)max_ms);
                } else {
                    ESP_LOGI(TAG, "Usage: channel_time read min|max");
                }
                continue;
            }

            if (argc >= 4 && strcasecmp(argv[1], "set") == 0) {
                long value = strtol(argv[3], NULL, 10);
                if (value < 1) {
                    ESP_LOGW(TAG, "Value must be >= 1");
                    continue;
                }

                if (strcasecmp(argv[2], "min") == 0) {
                    if (value > CHANNEL_TIME_MIN_LIMIT) {
                        value = CHANNEL_TIME_MIN_LIMIT;
                    }
                    set_channel_min_time((uint32_t)value);
                    channel_time_persist_state();
                    uint32_t min_ms = 0, max_ms = 0;
                    get_channel_times(&min_ms, &max_ms);
                    ESP_LOGI(TAG, "channel_time min=%u max=%u", (unsigned)min_ms, (unsigned)max_ms);
                } else if (strcasecmp(argv[2], "max") == 0) {
                    if (value > CHANNEL_TIME_MAX_LIMIT) {
                        value = CHANNEL_TIME_MAX_LIMIT;
                    }
                    set_channel_max_time((uint32_t)value);
                    channel_time_persist_state();
                    uint32_t min_ms = 0, max_ms = 0;
                    get_channel_times(&min_ms, &max_ms);
                    ESP_LOGI(TAG, "channel_time min=%u max=%u", (unsigned)min_ms, (unsigned)max_ms);
                } else {
                    ESP_LOGI(TAG, "Usage: channel_time set min|max <ms>");
                }
                continue;
            }

            ESP_LOGI(TAG, "Usage: channel_time set min|max <ms> | channel_time read min|max");
            continue;
        }

        ESP_LOGW(TAG, "Unknown command: %s", argv[0]);
        print_help();
    }
}

static void uart_rx_task(void *arg)
{
    (void)arg;

    uint8_t b;
    char line[220];
    int idx = 0;

    while (1) {
        int r = uart_read_bytes(LINK_UART_NUM, &b, 1, pdMS_TO_TICKS(100));
        if (r != 1) {
            continue;
        }

        if (b == '\n') {
            line[idx] = '\0';
            if (strncmp(line, "GPS,", 4) == 0) {
                parse_gps_line(line);
            } else if (strncmp(line, "AP24,", 5) == 0) {
                parse_remote_ap24_line(line);
            }
            idx = 0;
            continue;
        }

        if (b != '\r' && idx < (int)sizeof(line) - 1) {
            line[idx++] = (char)b;
        }
    }
}

static void wardrive_task(void *arg)
{
    (void)arg;

    ducb_init();

    wardrive_mode_t active_mode = WARDRIVE_MODE_SCAN;
    bool promisc_on = false;
    uint32_t last_status_ms = 0;

    while (1) {
        wardrive_mode_t desired_mode = get_mode();

        if (desired_mode != active_mode) {
            if (promisc_on) {
                esp_wifi_set_promiscuous(false);
                promisc_on = false;
            }
            active_mode = desired_mode;
            ESP_LOGI(TAG, "Switched wardrive mode -> %s", active_mode == WARDRIVE_MODE_PROMISC ? "promisc" : "scan");
        }

        if (active_mode == WARDRIVE_MODE_PROMISC) {
            if (!promisc_on) {
                wifi_promiscuous_filter_t filter = {
                    .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
                };
                esp_wifi_set_promiscuous_filter(&filter);
                esp_wifi_set_promiscuous_rx_cb(promisc_rx_callback);
                esp_wifi_set_promiscuous(true);
                promisc_on = true;

                ESP_LOGI(TAG,
                         "5GHz promisc wardrive started. Channels=%d (nonDFS:%d DFS:%d dwell=%d/%dms)",
                         g_ducb_channel_count,
                         (int)PROMISC_5_NON_DFS_COUNT,
                         (int)PROMISC_5_DFS_COUNT,
                         DWELL_5_NON_DFS_MS,
                         DWELL_5_DFS_MS);
            }

            run_promisc_cycle();
            flush_dirty_entries_if_needed(false, PROMISC_FLUSH_INTERVAL_AP, PROMISC_FLUSH_INTERVAL_MS);
        } else {
            if (promisc_on) {
                esp_wifi_set_promiscuous(false);
                promisc_on = false;
            }

            run_scan_cycle();
            flush_dirty_entries_if_needed(false, 1, SCAN_FLUSH_INTERVAL_MS);
            vTaskDelay(pdMS_TO_TICKS(SCAN_PAUSE_MS));
        }

        uint32_t now = (uint32_t)esp_log_timestamp();
        if ((now - last_status_ms) >= 15000) {
            last_status_ms = now;
            print_status();
        }
    }
}

void app_main(void)
{
    init_link_uart();
    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 8, NULL);

    ESP_LOGI(TAG, "Stage 1/4: waiting for SD card mount");
    wait_for_sd_ready();

    ESP_LOGI(TAG, "Stage 2/4: waiting for valid GPS fix from C6");
    wait_for_gps_fix_startup();

    ESP_LOGI(TAG, "Stage 3/4: init NVS + Wi-Fi");
    ESP_ERROR_CHECK(init_nvs_wifi());
    channel_time_load_state_from_nvs();

    init_seen_buffers();

    ESP_LOGI(TAG, "Stage 4/4: starting wardrive engine (C5=5GHz promisc, C6=2.4GHz AP24 ingest)");
    xTaskCreate(wardrive_task, "wardrive_task", 8192, NULL, 5, NULL);
    xTaskCreate(command_task, "command_task", 6144, NULL, 2, NULL);
}
