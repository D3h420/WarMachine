#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <sys/unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"

// C5 wiring:
// SD: SCK D8(GPIO19), MISO D9(GPIO20), MOSI D10(GPIO18), CS D2(GPIO2)
// C5 <- C6: D1(GPIO1 RX), optional C5 -> C6: D0(GPIO0 TX)

#define LINK_UART_NUM    UART_NUM_1
#define LINK_RX_GPIO     1
#define LINK_TX_GPIO     0
#define LINK_BAUD        115200

#define SD_MOSI_GPIO     18
#define SD_MISO_GPIO     20
#define SD_SCK_GPIO      19
#define SD_CS_GPIO       2

#define SCAN_PERIOD_MS   5000
#define SD_RETRY_MS      2000
#define GPS_WAIT_POLL_MS 250
#define GPS_WAIT_LOG_MS  3000
#define GPS_STALE_MS     5000

static const char *TAG = "C5_LOGGER";
static const char *MOUNT_POINT = "/sdcard";
static const char *CSV_PATH = "/sdcard/wardrive.csv";
static bool g_sd_ready = false;
static int64_t g_last_gps_rx_local_ms = 0;

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

static gps_state_t g_gps = {0};

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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

static esp_err_t init_sd_spi(sdmmc_card_t **out_card) {
    // Basic pull-ups help SD cards enter SPI mode reliably on power-up.
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
        .max_transfer_sz = 4000
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_GPIO;
    slot_config.host_id = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 4000;

    sdmmc_card_t *card = NULL;
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) return ret;
    *out_card = card;
    return ESP_OK;
}

static void ensure_csv_header(void) {
    FILE *f = fopen(CSV_PATH, "r");
    if (f) {
        fclose(f);
        return;
    }
    f = fopen(CSV_PATH, "w");
    if (!f) return;
    fprintf(f, "device_ms,gps_msg_ms,gps_valid,lat,lon,alt_m,sats,hdop,date_utc,time_utc,ssid,bssid,rssi,channel,authmode\n");
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    ESP_LOGI(TAG, "Created CSV header: %s", CSV_PATH);
}

static void init_link_uart(void) {
    uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART_NUM, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART_NUM, LINK_TX_GPIO, LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void parse_gps_line(const char *line) {
    // GPS,msgMs,lat,lon,alt,sats,hdop,date,time,valid
    if (strncmp(line, "GPS,", 4) != 0) return;
    char buf[220];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *t[9] = {0};
    int n = 0;
    char *p = strtok(buf + 4, ",");
    while (p && n < 9) { t[n++] = p; p = strtok(NULL, ","); }
    if (n != 9) return;

    g_gps.msg_ms = atoll(t[0]);
    g_gps.lat = strtof(t[1], NULL);
    g_gps.lon = strtof(t[2], NULL);
    g_gps.alt_m = strtof(t[3], NULL);
    g_gps.sats = atoi(t[4]);
    g_gps.hdop = strtof(t[5], NULL);
    strncpy(g_gps.date_utc, t[6], sizeof(g_gps.date_utc) - 1);
    g_gps.date_utc[sizeof(g_gps.date_utc) - 1] = '\0';
    strncpy(g_gps.time_utc, t[7], sizeof(g_gps.time_utc) - 1);
    g_gps.time_utc[sizeof(g_gps.time_utc) - 1] = '\0';
    g_gps.valid = atoi(t[8]) == 1;
    g_last_gps_rx_local_ms = esp_log_timestamp();
}

static bool gps_fix_ready(void) {
    bool coords_present = (fabsf(g_gps.lat) > 0.000001f) || (fabsf(g_gps.lon) > 0.000001f);
    return g_gps.valid && (g_gps.sats > 0) && coords_present;
}

static void wait_for_sd_ready(void) {
    int attempt = 0;
    while (!g_sd_ready) {
        attempt++;
        sdmmc_card_t *card = NULL;
        esp_err_t sd_ret = init_sd_spi(&card);
        if (sd_ret == ESP_OK) {
            g_sd_ready = true;
            sdmmc_card_print_info(stdout, card);
            ensure_csv_header();
            ESP_LOGI(TAG, "SD ready after %d attempt(s)", attempt);
            return;
        }

        ESP_LOGW(TAG, "SD init attempt %d failed: %s (0x%x), retry in %d ms",
                 attempt, esp_err_to_name(sd_ret), (unsigned int)sd_ret, SD_RETRY_MS);
        vTaskDelay(pdMS_TO_TICKS(SD_RETRY_MS));
    }
}

static void wait_for_gps_fix(void) {
    int64_t last_log_ms = -GPS_WAIT_LOG_MS;
    while (1) {
        int64_t now = esp_log_timestamp();
        bool got_any_frame = g_last_gps_rx_local_ms > 0;
        bool gps_fresh = got_any_frame && ((now - g_last_gps_rx_local_ms) <= GPS_STALE_MS);

        if (gps_fresh && gps_fix_ready()) {
            ESP_LOGI(TAG, "GPS fix ready: lat=%.6f lon=%.6f sats=%d hdop=%.2f",
                     g_gps.lat, g_gps.lon, g_gps.sats, g_gps.hdop);
            return;
        }

        if ((now - last_log_ms) >= GPS_WAIT_LOG_MS) {
            if (!got_any_frame) {
                ESP_LOGW(TAG, "Waiting for GPS data from C6 (no frames yet)");
            } else if (!gps_fresh) {
                ESP_LOGW(TAG, "Waiting for fresh GPS data (last frame %lld ms ago)",
                         (long long)(now - g_last_gps_rx_local_ms));
            } else {
                ESP_LOGW(TAG, "Waiting for GPS fix: valid=%d sats=%d lat=%.6f lon=%.6f",
                         g_gps.valid ? 1 : 0, g_gps.sats, g_gps.lat, g_gps.lon);
            }
            last_log_ms = now;
        }

        vTaskDelay(pdMS_TO_TICKS(GPS_WAIT_POLL_MS));
    }
}

static void uart_rx_task(void *arg) {
    (void)arg;
    uint8_t b;
    char line[220];
    int idx = 0;
    while (1) {
        int r = uart_read_bytes(LINK_UART_NUM, &b, 1, pdMS_TO_TICKS(100));
        if (r == 1) {
            if (b == '\n') {
                line[idx] = '\0';
                parse_gps_line(line);
                idx = 0;
            } else if (b != '\r' && idx < (int)sizeof(line) - 1) {
                line[idx++] = (char)b;
            }
        }
    }
}

static void scan_and_log_task(void *arg) {
    (void)arg;
    while (1) {
        wifi_scan_config_t scan_cfg = {
            .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true
        };
        esp_err_t s = esp_wifi_scan_start(&scan_cfg, true);
        if (s != ESP_OK) {
            ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(s));
            vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
            continue;
        }

        uint16_t ap_count = 0;
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
        if (ap_count == 0) {
            ESP_LOGW(TAG, "No APs found in this scan, nothing to log");
            vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
            continue;
        }

        wifi_ap_record_t *recs = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (!recs) {
            vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
            continue;
        }
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, recs));

        if (!g_sd_ready) {
            ESP_LOGW(TAG, "SD not ready, skipping log write");
            free(recs);
            vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
            continue;
        }

        FILE *f = fopen(CSV_PATH, "a");
        if (f) {
            int64_t now = esp_log_timestamp();
            for (int i = 0; i < ap_count; i++) {
                char bssid[18];
                snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                         recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                         recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5]);
                fprintf(f, "%lld,%lld,%d,%.6f,%.6f,%.2f,%d,%.2f,%s,%s,\"%s\",%s,%d,%d,%d\n",
                        (long long)now, (long long)g_gps.msg_ms, g_gps.valid ? 1 : 0,
                        g_gps.lat, g_gps.lon, g_gps.alt_m, g_gps.sats, g_gps.hdop,
                        g_gps.date_utc[0] ? g_gps.date_utc : "0-0-0",
                        g_gps.time_utc[0] ? g_gps.time_utc : "0:0:0",
                        (char *)recs[i].ssid, bssid, recs[i].rssi, recs[i].primary, recs[i].authmode);
            }
            fflush(f);
            fsync(fileno(f));
            fclose(f);
            ESP_LOGI(TAG, "Logged %u AP entries", ap_count);
        } else {
            ESP_LOGE(TAG, "Cannot open %s", CSV_PATH);
        }
        free(recs);
        vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
    }
}

void app_main(void) {
    init_link_uart();
    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 8, NULL);

    ESP_LOGI(TAG, "Stage 1/3: waiting for SD card mount");
    wait_for_sd_ready();

    ESP_LOGI(TAG, "Stage 2/3: waiting for valid GPS fix from C6");
    wait_for_gps_fix();

    ESP_ERROR_CHECK(init_nvs_wifi());

    ESP_LOGI(TAG, "Stage 3/3: starting Wi-Fi scan + SD logging");
    xTaskCreate(scan_and_log_task, "scan_and_log_task", 8192, NULL, 5, NULL);
}
