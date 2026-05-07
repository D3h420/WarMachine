# WarMachine (XIAO ESP32-C5 + XIAO ESP32-C6)

## Cel
Układ dwóch płytek:
- `C6`: GPS bridge
- `C5`: Wi‑Fi scanner + SD logger

Po starcie urządzenie zbiera sieci Wi‑Fi i zapisuje je z pozycją GPS do CSV.

## Finalne połączenia

### XIAO <-> XIAO
1. `C6 D0 (TX) -> C5 D1 (RX)`
2. `C5 D0 (TX) -> C6 D1 (RX)` (opcjonalnie, zalecane)
3. `C6 GND <-> C5 GND` (obowiązkowo)

Nie łącz:
- `5V C6` z `5V C5`, jeśli oba zasilasz osobno po USB.

### C6 <-> GPS
1. `GPS TX -> C6 D7 (RX)`
2. `GPS RX <- C6 D6 (TX)` (opcjonalnie)
3. `GPS GND -> C6 GND`
4. `GPS VCC -> C6 3V3` (jeśli Twój moduł GPS wspiera 3.3V)

### C5 <-> SD (SPI)
1. `SD SCK -> C5 D8`
2. `SD MISO -> C5 D9`
3. `SD MOSI -> C5 D10`
4. `SD CS -> C5 D2`
5. `SD GND -> C5 GND`
6. `SD VCC -> C5 3V3` (lub wg spec modułu)

## Firmware

- `firmware/c6_gps_bridge/c6_gps_bridge.ino`
- `firmware/c5_wardrive_logger/c5_wardrive_logger.ino`

### Co robi C6
- Odczytuje GPS z UART (`D7/D6`).
- Parsuje przez `TinyGPSPlus`.
- Co 1 s wysyła do C5:
`GPS,msgMs,lat,lon,alt,sats,hdop,date,time,valid`

### Co robi C5
- Odbiera ramki GPS z C6 przez UART (`D1/D0`).
- Skanuje Wi‑Fi co 5 s.
- Zapisuje CSV na SD: `/wardrive.csv`.

## Flash krok po kroku (Arduino IDE 2.x)

### 1) Przygotowanie
1. Zainstaluj `Arduino IDE 2.x`.
2. W `Boards Manager` zainstaluj `esp32 by Espressif Systems`.
3. W `Library Manager` zainstaluj `TinyGPSPlus`.

### 2) Flash C6
1. Podłącz do USB tylko `XIAO ESP32-C6`.
2. Otwórz `firmware/c6_gps_bridge/c6_gps_bridge.ino`.
3. Ustaw:
- `Tools -> Board -> XIAO ESP32C6`
- `Tools -> Port -> port C6`
4. Kliknij `Upload`.
5. `Serial Monitor` 115200 i sprawdź:
- `[C6] GPS bridge started`
- `[C6->C5] GPS,...`

### 3) Flash C5
1. Podłącz do USB tylko `XIAO ESP32-C5`.
2. Otwórz `firmware/c5_wardrive_logger/c5_wardrive_logger.ino`.
3. Ustaw:
- `Tools -> Board -> XIAO ESP32C5`
- `Tools -> Port -> port C5`
4. Kliknij `Upload`.
5. `Serial Monitor` 115200 i sprawdź:
- `[C5] SD init OK`
- `[C5] Wardrive logger started`
- `[C5] GPS update: GPS,...`
- `[C5] Logged N AP entries`

### 4) Gdy upload nie działa
1. Zrób podwójny reset płytki (bootloader mode).
2. Spróbuj upload ponownie.

## Test end-to-end
1. Zasil osobno oba XIAO po USB.
2. Sprawdź UART i GND między płytkami.
3. Wystaw GPS na zewnątrz.
4. Po kilkudziesięciu sekundach sprawdź na SD plik `wardrive.csv`.

## Źródła
- https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/
- https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/
- https://wiki.seeedstudio.com/xiao_esp32c5_pin_multiplexing/
- https://wiki.seeedstudio.com/xiao_pin_multiplexing_esp32c6/
