#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

// C6 wiring:
// GPS TX -> D7 (GPIO17), GPS RX <- D6 (GPIO16)
// C6 -> C5: D0 (GPIO0 TX), optional C5 -> C6: D1 (GPIO1 RX)

#define GPS_UART_NUM        UART_NUM_1
#define LINK_UART_NUM       UART_NUM_2

#define GPS_RX_GPIO         17
#define GPS_TX_GPIO         16

#define LINK_TX_GPIO        0
#define LINK_RX_GPIO        1

#define GPS_BAUD            9600
#define LINK_BAUD           115200

static const char *TAG = "C6_GPS";

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
            if (b == '\n') {
                line[idx] = '\0';
                if (idx > 6 && line[0] == '$') {
                    char tmp[128];
                    strncpy(tmp, line, sizeof(tmp) - 1);
                    tmp[sizeof(tmp) - 1] = '\0';
                    if (strstr(tmp, "RMC")) parse_rmc(tmp);
                    if (strstr(tmp, "GGA")) parse_gga(tmp);
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
            uart_write_bytes(LINK_UART_NUM, out, strlen(out));
            ESP_LOGI(TAG, "-> %s", out);
        }
    }
}

void app_main(void) {
    uart_init();
    ESP_LOGI(TAG, "C6 GPS bridge started");
    xTaskCreate(gps_task, "gps_task", 4096, NULL, 5, NULL);
}
