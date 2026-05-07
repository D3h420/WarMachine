# WarMachine (ESP-IDF) 🚀

Projekt został przebudowany na **czyste ESP-IDF** (bez Arduino).

## Struktura repo

- `fw_c6_idf/` -> firmware dla XIAO ESP32-C6 (GPS bridge)
- `fw_c5_idf/` -> firmware dla XIAO ESP32-C5 (Wi‑Fi scan + SD logger)

## Wiring (final)

### XIAO C6 <-> XIAO C5
| C6 | C5 | Opis |
|---|---|---|
| `D0 (TX)` | `D1 (RX)` | GPS data C6 -> C5 |
| `D1 (RX)` | `D0 (TX)` | opcjonalnie (2-way) |
| `GND` | `GND` | obowiązkowo |

Nie łącz `5V <-> 5V`, jeśli oba zasilasz osobno po USB.

### GPS -> C6
| GPS | C6 |
|---|---|
| `TX` | `D7 (RX)` |
| `RX` | `D6 (TX)` (opcjonalnie) |
| `GND` | `GND` |
| `VCC` | `3V3` (jeśli moduł wspiera) |

### SD -> C5 (SPI)
| SD | C5 |
|---|---|
| `SCK` | `D8` |
| `MISO` | `D9` |
| `MOSI` | `D10` |
| `CS` | `D2` |
| `GND` | `GND` |
| `VCC` | `3V3` lub wg modułu |

## Co robi software

### `fw_c6_idf`
- czyta NMEA z GPS po UART,
- parsuje podstawowe `RMC/GGA`,
- wysyła co 1s linię do C5:
`GPS,msgMs,lat,lon,alt,sats,hdop,date,time,valid`

### `fw_c5_idf`
- odbiera linie GPS z C6 po UART,
- skanuje Wi‑Fi co 5s,
- dopisuje rekordy do `/sdcard/wardrive.csv`.

## Wymagania

1. ESP-IDF `v5.x` (zalecane).
2. Python + narzędzia IDF (`idf.py`).
3. Dwa kable USB data.

## Instalacja ESP-IDF (skrót)

Najprościej:
1. Zainstaluj ESP-IDF wg oficjalnej instrukcji Espressif (installer lub manual).
2. Aktywuj środowisko:
   - macOS/Linux: `source ~/esp/esp-idf/export.sh`
3. Sprawdź: `idf.py --version`

## Build i flash: C6

1. Wejdź do projektu:
   - `cd /Users/dominikhrycaj/Documents/GitHub/WarMachine/fw_c6_idf`
2. Ustaw target:
   - `idf.py set-target esp32c6`
3. Zbuduj:
   - `idf.py build`
4. Flash:
   - `idf.py -p /dev/cu.usbmodemXXXX flash`
5. Monitor:
   - `idf.py -p /dev/cu.usbmodemXXXX monitor`

Oczekiwany log:
- `C6 GPS bridge started`
- linie `GPS,...`

## Build i flash: C5

1. Wejdź do projektu:
   - `cd /Users/dominikhrycaj/Documents/GitHub/WarMachine/fw_c5_idf`
2. Ustaw target:
   - `idf.py set-target esp32c5`
3. Zbuduj:
   - `idf.py build`
4. Flash:
   - `idf.py -p /dev/cu.usbmodemYYYY flash`
5. Monitor:
   - `idf.py -p /dev/cu.usbmodemYYYY monitor`

Oczekiwany log:
- `C5 logger started`
- `Logged N AP entries`

## Szybki test end-to-end

1. Zasil oba XIAO osobno po USB.
2. Sprawdź połączenia `D0/D1 + GND` między płytkami.
3. Wystaw GPS na zewnątrz.
4. Po chwili sprawdź kartę SD:
   - plik `/wardrive.csv`

## Czyszczenie buildów

- w `fw_c6_idf`: `idf.py fullclean`
- w `fw_c5_idf`: `idf.py fullclean`

## Źródła

- https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/
- https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/
- https://wiki.seeedstudio.com/xiao_esp32c5_pin_multiplexing/
- https://wiki.seeedstudio.com/xiao_pin_multiplexing_esp32c6/

---

<p align="center">
  <img src="./assets/labs-logo-transparent.png" alt="LABS" width="320" />
</p>
