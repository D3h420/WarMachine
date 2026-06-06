# WarMachine 🚀

<img width="1983" height="793" alt="WarMachine" src="https://github.com/user-attachments/assets/d3dbda82-bfa6-4225-8157-7a5522b26137" />

Dual-board wardriving rig built around **XIAO ESP32-C6 + XIAO ESP32-C5 + GPS + SD**.
C6 owns **2.4 GHz + GPS**, C5 owns **5 GHz + logging**, and both streams land in one WiGLE-compatible CSV.

## ✨ Highlights

- 🛰️ GPS bridge with fresh-fix gating
- 📡 Parallel 2.4 GHz and 5 GHz scanning
- 🧠 Adaptive channel hopping with discounted UCB
- 💾 WiGLE CSV logging to SD card
- 🔁 AP dedupe with RSSI/location-based re-log
- 🧰 Runtime serial commands for status and scan tuning

## 🧭 Architecture

| Board | Role | Output |
|---|---|---|
| `xiao_c6` | GPS parser + 2.4 GHz scanner | `GPS,...` and `AP24,...` over UART |
| `xiao_c5` | 5 GHz scanner + SD logger | `/sdcard/wardrive.csv` |

```text
GPS ──UART──> XIAO C6 ──GPS/AP24 UART──> XIAO C5 ──SPI──> SD
                         2.4 GHz scan       5 GHz scan
```

## 🔌 Hardware

### XIAO C6 ↔ XIAO C5

| C6 | C5 | Notes |
|---|---|---|
| `D0 / TX / GPIO0` | `D1 / RX / GPIO0` | C6 streams GPS + 2.4 GHz APs |
| `D1 / RX / GPIO1` | `D0 / TX / GPIO1` | optional return link |
| `GND` | `GND` | required |

> ⚠️ Do not connect `5V ↔ 5V` if both boards are powered separately over USB.

### GPS → XIAO C6

| GPS | C6 |
|---|---|
| `TX` | `D7 / RX / GPIO17` |
| `RX` | `D6 / TX / GPIO16` optional |
| `GND` | `GND` |
| `VCC` | `3V3` if supported by your GPS module |

### SD → XIAO C5

| SD | C5 |
|---|---|
| `SCK` | `D8 / GPIO8` |
| `MISO` | `D9 / GPIO9` |
| `MOSI` | `D10 / GPIO10` |
| `CS` | `D2 / GPIO25` |
| `GND` | `GND` |
| `VCC` | `3V3` or according to your SD module |

### Build

40×60 proto PCB:

<img width="760" alt="WarMachine proto PCB" src="https://github.com/user-attachments/assets/aa39db76-0496-4d51-99df-27f507c1c020" />

Dedicated enclosure:

🔗 [MakerWorld - WarMachine XIAO C5/C6 wardriving setup](https://makerworld.com/pl/models/2781343-warmachine-xiao-c5-c6-wardriving-setup#profileId-3091165)

<img width="326" alt="WarMachine enclosure" src="https://github.com/user-attachments/assets/20194e14-bea8-4370-ba52-26269c2abd15" />

## 🧠 Firmware

### C6: GPS + 2.4 GHz Scout

- Parses GPS NMEA sentences: `RMC` and `GGA`
- Publishes GPS once per second:

```text
GPS,msgMs,lat,lon,alt,sats,hdop,date,time,valid
```

- Scans 2.4 GHz in promiscuous mode
- Publishes discovered 2.4 GHz APs:

```text
AP24,msgMs,bssid,channel,rssi,authmode,ssidHex
```

### C5: 5 GHz Logger

- Waits for SD card mount
- Waits for valid, fresh GPS from C6
- Forces Wi-Fi into `5 GHz only`
- Runs 5 GHz wardrive engine
- Merges local 5 GHz APs with remote C6 2.4 GHz APs
- Deduplicates by BSSID in RAM
- Writes WiGLE CSV batches to:

```text
/sdcard/wardrive.csv
```

### Scan Timing

| Band | Owner | Channels | Dwell |
|---|---|---|---|
| 2.4 GHz primary | C6 | `1/6/11` | `160 ms` |
| 2.4 GHz secondary | C6 | `2/3/4/5/7/8/9/10/12/13` | `100 ms` |
| 5 GHz non-DFS | C5 | `36/40/44/48/149/153/157/161/165` | `120 ms` |
| 5 GHz DFS/extended | C5 | `52-144/169/173/177` | `90 ms` |

First full sweep:

- 2.4 GHz: `~1.48 s`
- 5 GHz: `~2.79 s`

After the first sweep, both scanners use discounted UCB channel selection, so busy channels are revisited more often while quiet channels still get sampled.

### Re-Log Rules

Known APs are re-logged only when the new sample is useful:

- RSSI changes by at least `5 dBm`
- or position changes by about `25 m`
- or channel/security/SSID changes

This keeps WiGLE trilateration useful without flooding the CSV.

## 🕹️ Runtime Commands

Use these in the C5 ESP-IDF monitor:

| Command | Description |
|---|---|
| `help` | show commands |
| `status` | print GPS, mode, counters, dirty rows |
| `mode read` | show current scan mode |
| `mode set promisc` | fast passive 5 GHz hopping |
| `mode set scan` | active 5 GHz scan mode |
| `channel_time read min|max` | read active-scan timing |
| `channel_time set min|max <ms>` | persist active-scan timing in NVS |

## ⚡ Flashing

### Requirements

- ESP-IDF `v5.x`
- VS Code + Espressif IDF extension, or `idf.py`
- Two USB data cables

### VS Code

Open each firmware folder separately and flash it with the matching target:

| Folder | Target | Expected Boot Log |
|---|---|---|
| `xiao_c6` | `esp32c6` | `C6 GPS bridge started` |
| `xiao_c5` | `esp32c5` | `Stage 4/4: starting wardrive engine ...` |

Flow:

1. `File → Open Folder`
2. Select `xiao_c6` or `xiao_c5`
3. `Set Espressif Device Target`
4. `Build`
5. `Flash`
6. `Monitor`

### CLI

Flash C6:

```bash
cd xiao_c6
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Flash C5:

```bash
cd xiao_c5
idf.py set-target esp32c5
idf.py build
idf.py -p /dev/cu.usbmodemYYYY flash monitor
```

## ✅ Quick Test

1. Insert SD card into C5.
2. Power both boards.
3. Place GPS outdoors and wait for fix.
4. Watch C5 monitor for `GPS fix ready`.
5. Confirm `/sdcard/wardrive.csv` appears and grows.
6. Run `status` to verify `remote24`, `seen`, `dirty`, and GPS state.

## 📚 Sources

- [XIAO ESP32-C5 Getting Started](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/)
- [XIAO ESP32-C6 Getting Started](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/)
- [XIAO ESP32-C5 Pin Multiplexing](https://wiki.seeedstudio.com/xiao_esp32c5_pin_multiplexing/)
- [XIAO ESP32-C6 Pin Multiplexing](https://wiki.seeedstudio.com/xiao_pin_multiplexing_esp32c6/)
- [ESP32 Dual Band Wardriver](https://github.com/justcallmekoko/ESP32DualBandWardriver)
