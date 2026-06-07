<div align="center">

# WarMachine 🚀

**Dual-board ESP32-C5/C6 wardriving rig for parallel 2.4 GHz + 5 GHz collection.**

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-E7352C?style=for-the-badge&logo=espressif)
![XIAO C5](https://img.shields.io/badge/XIAO-ESP32--C5-111827?style=for-the-badge)
![XIAO C6](https://img.shields.io/badge/XIAO-ESP32--C6-111827?style=for-the-badge)
![WiGLE](https://img.shields.io/badge/Output-WiGLE%20.log-2563EB?style=for-the-badge)

<img width="1983" height="793" alt="WarMachine" src="https://github.com/user-attachments/assets/d3dbda82-bfa6-4225-8157-7a5522b26137" />

</div>

WarMachine splits the job between two XIAO boards: **C6 handles GPS + 2.4 GHz**, while **C5 handles 5 GHz + SD logging**. Both streams are merged into one WiGLE-compatible `.log` file.

## Architecture

| Board | Role | Data |
|---|---|---|
| `xiao_c6` | GPS bridge + 2.4 GHz scanner | `GPS,...`, `AP24,...` over UART |
| `xiao_c5` | 5 GHz scanner + SD logger | `/sdcard/wardrive_<n>.log` |

```text
GPS ──UART──> XIAO C6 ──GPS/AP24 UART──> XIAO C5 ──SPI──> SD
                         2.4 GHz scan       5 GHz scan
```

## Hardware

### C6 ↔ C5

| C6 | C5 | Notes |
|---|---|---|
| `D0 / TX / GPIO0` | `D1 / RX / GPIO0` | GPS + 2.4 GHz stream |
| `D1 / RX / GPIO1` | `D0 / TX / GPIO1` | optional return link |
| `GND` | `GND` | required |

> ⚠️ Do not connect `5V ↔ 5V` when both boards are powered separately over USB.

### GPS → C6

| GPS | C6 |
|---|---|
| `TX` | `D7 / RX / GPIO17` |
| `RX` | `D6 / TX / GPIO16` optional |
| `GND` | `GND` |
| `VCC` | `3V3` if supported |

### SD → C5

| SD | C5 |
|---|---|
| `SCK` | `D8 / GPIO8` |
| `MISO` | `D9 / GPIO9` |
| `MOSI` | `D10 / GPIO10` |
| `CS` | `D2 / GPIO25` |
| `GND` | `GND` |
| `VCC` | `3V3` or according to module |

## Firmware

| Firmware | Target | Purpose |
|---|---|---|
| `xiao_c6` | `esp32c6` | GPS parsing + 2.4 GHz AP discovery |
| `xiao_c5` | `esp32c5` | 5 GHz wardriving + WiGLE `.log` writing |

C5 creates a new session file by picking the first free number:

```text
/sdcard/wardrive_1.log
/sdcard/wardrive_2.log
/sdcard/wardrive_3.log
```

Log files are created only when the first real AP record is written, so empty boot attempts should not produce header-only logs.

## Scan Timing

| Band | Board | Channels | Dwell |
|---|---|---|---|
| 2.4 GHz primary | C6 | `1/6/11` | `160 ms` |
| 2.4 GHz secondary | C6 | `2-5/7-10/12/13` | `100 ms` |
| 5 GHz non-DFS | C5 | `36/40/44/48/149/153/157/161/165` | `120 ms` |
| 5 GHz DFS/extended | C5 | `52-144/169/173/177` | `90 ms` |

Known APs are re-logged when RSSI changes by `5 dBm`, position changes by about `25 m`, or SSID/channel/security changes.

## Flashing

Requirements: **ESP-IDF v5.x**, two USB data cables, and VS Code Espressif extension or `idf.py`.

### VS Code

Open each firmware folder separately:

| Folder | Target | Expected Boot Log |
|---|---|---|
| `xiao_c6` | `esp32c6` | `C6 GPS bridge started` |
| `xiao_c5` | `esp32c5` | `Stage 4/4: starting wardrive engine ...` |

Flow:

1. `File → Open Folder` and select `xiao_c6` or `xiao_c5`.
2. Run `Set Espressif Device Target`.
3. Build, Flash, then Monitor.

### CLI

```bash
cd xiao_c6
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

```bash
cd xiao_c5
idf.py set-target esp32c5
idf.py build
idf.py -p /dev/cu.usbmodemYYYY flash monitor
```

## Field Test

1. Insert SD card into C5.
2. Power both boards.
3. Place GPS outdoors and wait for fix.
4. Watch C5 monitor for `GPS fix ready`.
5. Confirm `/sdcard/wardrive_<n>.log` appears and grows.
6. Run `status` and check `gps`, `remote24`, `seen`, `dirty`, and `log`.

Expected C5 startup:

```text
Stage 1/4: waiting for SD card mount
Stage 2/4: waiting for valid GPS fix from C6
Stage 3/4: init NVS + Wi-Fi
Stage 4/4: starting wardrive engine (C5=5GHz promisc, C6=2.4GHz AP24 ingest)
```

## Build

40×60mm proto PCB:

<img width="760" alt="WarMachine proto PCB" src="https://github.com/user-attachments/assets/aa39db76-0496-4d51-99df-27f507c1c020" />

Dedicated enclosure:

🔗 [MakerWorld - WarMachine XIAO C5/C6 wardriving setup](https://makerworld.com/pl/models/2781343-warmachine-xiao-c5-c6-wardriving-setup#profileId-3091165)

<img width="326" alt="WarMachine enclosure" src="https://github.com/user-attachments/assets/20194e14-bea8-4370-ba52-26269c2abd15" />

## References

- [XIAO ESP32-C5 Getting Started](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/)
- [XIAO ESP32-C6 Getting Started](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/)
- [ESP32 Dual Band Wardriver](https://github.com/justcallmekoko/ESP32DualBandWardriver)
