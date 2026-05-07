# WarMachine (ESP-IDF) 🚀

This project has been migrated to **pure ESP-IDF** (no Arduino).

## Repository Structure

- `fw_c6_idf/` -> firmware for XIAO ESP32-C6 (GPS bridge)
- `fw_c5_idf/` -> firmware for XIAO ESP32-C5 (Wi‑Fi scanner + SD logger)

## Wiring (Final)

### XIAO C6 <-> XIAO C5
| C6 | C5 | Description |
|---|---|---|
| `D0 (TX)` | `D1 (RX)` | GPS data from C6 -> C5 |
| `D1 (RX)` | `D0 (TX)` | optional (2-way communication) |
| `GND` | `GND` | required |

Do not connect `5V <-> 5V` if both boards are powered separately via USB.

### GPS -> C6
| GPS | C6 |
|---|---|
| `TX` | `D7 (RX)` |
| `RX` | `D6 (TX)` (optional) |
| `GND` | `GND` |
| `VCC` | `3V3` (if your GPS module supports it) |

### SD -> C5 (SPI)
| SD | C5 |
|---|---|
| `SCK` | `D8` |
| `MISO` | `D9` |
| `MOSI` | `D10` |
| `CS` | `D2` |
| `GND` | `GND` |
| `VCC` | `3V3` or according to your SD module specs |

## Software Overview

### `fw_c6_idf`
- reads NMEA from GPS over UART,
- parses basic `RMC/GGA` data,
- sends one line every second to C5:
`GPS,msgMs,lat,lon,alt,sats,hdop,date,time,valid`

### `fw_c5_idf`
- receives GPS lines from C6 over UART,
- scans Wi‑Fi every 5s,
- appends records to `/sdcard/wardrive.csv`.

## Requirements

1. ESP-IDF `v5.x` (recommended)
2. Python + ESP-IDF tools (`idf.py`)
3. Two USB data cables

## ESP-IDF Setup (Short Version)

1. Install ESP-IDF using the official Espressif guide (installer or manual setup).
2. Activate the environment:
   - macOS/Linux: `source ~/esp/esp-idf/export.sh`
3. Verify:
   - `idf.py --version`

## Build & Flash: C6

1. Go to the project folder:
   - `cd /Users/dominikhrycaj/Documents/GitHub/WarMachine/fw_c6_idf`
2. Set target:
   - `idf.py set-target esp32c6`
3. Build:
   - `idf.py build`
4. Flash:
   - `idf.py -p /dev/cu.usbmodemXXXX flash`
5. Monitor:
   - `idf.py -p /dev/cu.usbmodemXXXX monitor`

Expected logs:
- `C6 GPS bridge started`
- `GPS,...` lines

## Build & Flash: C5

1. Go to the project folder:
   - `cd /Users/dominikhrycaj/Documents/GitHub/WarMachine/fw_c5_idf`
2. Set target:
   - `idf.py set-target esp32c5`
3. Build:
   - `idf.py build`
4. Flash:
   - `idf.py -p /dev/cu.usbmodemYYYY flash`
5. Monitor:
   - `idf.py -p /dev/cu.usbmodemYYYY monitor`

Expected logs:
- `C5 logger started`
- `Logged N AP entries`

## Quick End-to-End Test

1. Power both XIAO boards separately via USB.
2. Verify `D0/D1 + GND` interconnect between boards.
3. Place the GPS module outdoors.
4. Check SD card output:
   - `/wardrive.csv`

## Clean Build Artifacts

- in `fw_c6_idf`: `idf.py fullclean`
- in `fw_c5_idf`: `idf.py fullclean`

## Sources

- https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/
- https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/
- https://wiki.seeedstudio.com/xiao_esp32c5_pin_multiplexing/
- https://wiki.seeedstudio.com/xiao_pin_multiplexing_esp32c6/

---

<p align="center">
  <img src="./assets/labs-logo-transparent.png" alt="LABS" width="320" />
</p>
