# WarMachine 🚀
<img width="1983" height="793" alt="WarMachine" src="https://github.com/user-attachments/assets/d3dbda82-bfa6-4225-8157-7a5522b26137" />

Dual-board wardriving setup based on XIAO ESP32-C5 + XIAO ESP32-C6 + GPS + SD.

## Project Layout
- `xiao_c6/` - firmware for XIAO ESP32-C6 (GPS bridge)
- `xiao_c5/` - firmware for XIAO ESP32-C5 (Wi-Fi scanner + SD logger, WiGLE CSV)

## Hardware

### Wiring

#### XIAO C6 <-> XIAO C5
| C6 | C5 | Description |
|---|---|---|
| `D0 (TX, GPIO0)` | `D1 (RX, GPIO0)` | GPS data from C6 -> C5 |
| `D1 (RX, GPIO1)` | `D0 (TX, GPIO1)` | optional (2-way communication) |
| `GND` | `GND` | required |

Do not connect `5V <-> 5V` if both boards are powered separately via USB.

#### GPS -> C6
| GPS | C6 |
|---|---|
| `TX` | `D7 (RX)` |
| `RX` | `D6 (TX)` (optional) |
| `GND` | `GND` |
| `VCC` | `3V3` (if your GPS module supports it) |

#### SD -> C5 (SPI)
| SD | C5 |
|---|---|
| `SCK` | `D8 (GPIO8)` |
| `MISO` | `D9 (GPIO9)` |
| `MOSI` | `D10 (GPIO10)` |
| `CS` | `D2 (GPIO25)` |
| `GND` | `GND` |
| `VCC` | `3V3` or according to your SD module specs |

### Hardware Build
[photo soon]


### Enclosure (MakerWorld)
[MakerWorld enclosure - dedicated WarMachine enclosure](https://makerworld.com/pl/models/2781343-warmachine-xiao-c5-c6-wardriving-setup#profileId-3091165)


## Firmware Behavior

### `xiao_c6`
- parses GPS NMEA (`RMC/GGA`),
- sends to C5 once per second:
`GPS,msgMs,lat,lon,alt,sats,hdop,date,time,valid`

### `xiao_c5`
- waits for SD mount (with retries),
- waits for valid GPS fix from C6,
- starts Wi-Fi scanning,
- writes WiGLE-compatible data to `/sdcard/wardrive.csv`.

## Prerequisites
1. ESP-IDF `v5.x`
2. VS Code + Espressif IDF extension (recommended)
3. Two USB data cables

## Flashing (VS Code)

Repeat separately for `xiao_c6` and `xiao_c5`.

1. Open the board folder in VS Code (`File -> Open Folder`):
   - for C6: `xiao_c6`
   - for C5: `xiao_c5`
2. In ESP-IDF extension:
   - `Set Espressif Device Target`
   - choose `esp32c6` for C6 or `esp32c5` for C5
3. Run:
   - `Build`
   - `Flash`
   - `Monitor`

Expected monitor logs:
- C6: `C6 GPS bridge started`
- C5:
  - `Stage 1/3: waiting for SD card mount`
  - `Stage 2/3: waiting for valid GPS fix from C6`
  - `Stage 3/3: starting Wi-Fi scan + SD logging`

## Flashing (CLI Alternative)

### C6
```bash
cd xiao_c6
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

### C5
```bash
cd xiao_c5
idf.py set-target esp32c5
idf.py build
idf.py -p /dev/cu.usbmodemYYYY flash monitor
```

## Quick Test
1. Power both boards.
2. Place GPS outdoors and wait for fix.
3. Verify `wardrive.csv` appears on SD card and grows over time.

## Sources
- https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/
- https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/
- https://wiki.seeedstudio.com/xiao_esp32c5_pin_multiplexing/
- https://wiki.seeedstudio.com/xiao_pin_multiplexing_esp32c6/
