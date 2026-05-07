# WarMachine: Hardware plan (XIAO ESP32-C5 + XIAO ESP32-C6)

## 1) Cel projektu
Urządzenie ma po starcie auta uruchamiać tryb "autostart" i wykonywać pasywne skanowanie Wi-Fi (wardriving), a następnie zapisywać wykryte sieci razem z pozycją GPS.

Ten dokument dotyczy wyłącznie **hardware i połączeń**.

## 2) Rekomendowana architektura (v1)

### Podział ról
- `XIAO ESP32-C5`: skanowanie Wi-Fi + zapis na kartę SD
- `XIAO ESP32-C6`: odbiór GPS (UART) + wysyłanie danych pozycji do C5

Dlaczego taki podział:
- C5 ma dual-band Wi‑Fi 6 (2.4/5 GHz) i tryb promiscuous/monitor, więc lepiej nadaje się na "radio" do wardrivingu.
- C6 bierze na siebie GPS i odciąża C5.

## 3) Topologia połączeń

```text
[GPS] --UART--> [XIAO C6] --UART--> [XIAO C5] --SPI--> [microSD]
                         \___________________________/
                           wspólna masa (GND)
```

## 4) Wiring (sprawdzone mapowanie pinów)

## 4.0 Ostateczne połączenie XIAO <-> XIAO (Twoja wersja)
Założenia:
- `D6/D7` na C6 są zajęte przez GPS.
- Oba XIAO zasilasz osobno przez USB (bez mostka `5V<->5V`).

Połącz tylko to:
1. `C6 D0 (TX) -> C5 D1 (RX)`
2. `C6 D1 (RX) <- C5 D0 (TX)` (opcjonalnie, ale zalecane dla 2-kierunkowej komunikacji)
3. `C6 GND <-> C5 GND` (obowiązkowo)

Nie łącz:
- `5V C6` z `5V C5` (przy osobnym zasilaniu USB).

Uwaga:
- `D6/D7` to domyślne piny UART na pinoucie, ale w tym projekcie używamy remap UART w firmware na `D0/D1`, żeby nie kolidować z GPS.

## 4.1 XIAO C5 <-> moduł microSD (SPI)
| Funkcja | microSD module | XIAO ESP32-C5 | Uwagi |
|---|---|---|---|
| Zasilanie | VCC | 3V3 (zalecane) | Jeśli moduł SD wymaga 5V, podaj 5V tylko na VCC modułu, logika nadal musi być bezpieczna dla 3.3V |
| Masa | GND | GND | Wspólna masa obowiązkowa |
| SPI CLK | SCK/CLK | D8 | Sprzętowy SPI |
| SPI MISO | MISO/DO | D9 | Sprzętowy SPI |
| SPI MOSI | MOSI/DI | D10 | Sprzętowy SPI |
| Chip Select | CS | D2 | CS może być inny wolny GPIO, ale D2 jest bezpieczny |

## 4.2 XIAO C6 <-> GPS (UART)
| Funkcja | GPS module | XIAO ESP32-C6 | Uwagi |
|---|---|---|---|
| Zasilanie | VCC | 3V3 (preferowane) | Zależy od konkretnego modułu GPS (nie każdy lubi 5V) |
| Masa | GND | GND | Wspólna masa obowiązkowa |
| Dane GPS do MCU | TX | D7 (RX) | Najważniejsza linia |
| Konfiguracja GPS (opcjonalnie) | RX | D6 (TX) | Potrzebne tylko jeśli chcesz konfigurować moduł z MCU |
| Impuls czasu (opcjonalnie) | PPS | D1 (opcjonalnie) | Na później, jeśli chcesz precyzyjny timing |

## 4.3 Połączenie między C6 i C5 (UART)
| Funkcja | XIAO ESP32-C6 | XIAO ESP32-C5 | Uwagi |
|---|---|---|---|
| TX->RX | D0 (TX) | D1 (RX) | Główny kanał przesyłu pozycji GPS |
| RX<-TX (opcjonalnie) | D1 (RX) | D0 (TX) | Przydatne do ACK/komend/debug |
| Masa | GND | GND | Bez wspólnej masy UART będzie niestabilny |

Uwaga praktyczna:
- `D6/D7` na C6 są już zajęte przez GPS, więc link C6<->C5 celowo przeniesiony na `D0/D1`.

## 5) Weryfikacja konfliktów pinów

### C5 (Wi-Fi + SD + link do C6)
- Zajęte: `D0`, `D1`, `D2`, `D8`, `D9`, `D10`
- Wolne: `D3`, `D4`, `D5`, `D6`, `D7`
- Brak konfliktu z mapą SPI/UART.

### C6 (GPS + link do C5)
- Zajęte: `D0`, `D1`, `D6`, `D7`
- Wolne: `D2`, `D3`, `D4`, `D5`, `D8`, `D9`, `D10`
- Uwaga: `GPIO3` i `GPIO14` są związane z przełączaniem anteny RF (nie używaj ich przypadkowo do innych krytycznych funkcji).

## 6) Zasilanie z powerbanku (jedno USB do dowolnej płytki)

### Wymaganie użytkowe
Masz podpiąć jeden kabel USB-C z powerbanku do `C5` **albo** do `C6`, a cały układ ma działać.

### Najprostsze i skuteczne połączenie
Połącz między płytkami:
- `5V (C5) <-> 5V (C6)`
- `GND (C5) <-> GND (C6)`

Wtedy:
- gdy USB podłączysz do `C5`, zasilisz też `C6`,
- gdy USB podłączysz do `C6`, zasilisz też `C5`.

```text
Powerbank USB-C -> [XIAO C5 lub XIAO C6]
                     |            |
                    5V ---------- 5V
                    GND --------- GND
```

### Bardzo ważne ograniczenie (backfeed)
- Nie podłączaj jednocześnie obu płytek do dwóch różnych źródeł USB 5V (np. dwóch portów PC) przy zwartym `5V<->5V`.
- Jeżeli chcesz czasem debugować/programować obie płytki jednocześnie po USB, dodaj izolację zasilania (np. Schottky/power-path) albo rozłączany mostek 5V.

### Dobre praktyki dla powerbanku
- Użyj krótkich kabli USB-C i solidnych połączeń masy.
- Zostaw zapas prądowy powerbanku (minimum ~1 A ciągłego oddawania).
- Jeśli moduł GPS/SD zachowuje się niestabilnie, dołóż kondensator 100 uF blisko zasilania modułu.

## 7) Plan uruchomienia i testów (hardware first)

1. Test zasilania:
- Sprawdź multimetrem 5V i 3.3V pod obciążeniem.
- Potwierdź wspólną masę wszystkich modułów.

2. Test SD na C5:
- Uruchom prosty test `SD.begin(CS)` i zapis pliku testowego.
- Jeśli init fail: obniż SPI clock i sprawdź poziomy napięć.

3. Test GPS na C6:
- Odczytaj NMEA po UART (min. `$GPRMC`/`$GPGGA`).
- Test wykonaj na zewnątrz (fix satelitarny).

4. Test C6 -> C5:
- C6 wysyła po UART jedną linię pozycji co 1s.
- C5 odbiera i wypisuje na serial USB.

5. Test end-to-end:
- C5 zapisuje: `timestamp,lat,lon,ssid,rssi,channel,bssid` do CSV na SD.

## 8) Ryzyka i pułapki
- Moduły SD/GPS bywają różne elektrycznie mimo podobnego opisu handlowego.
- Błędne zasilanie GPS (5V vs 3.3V zależnie od płytki) to najczęstszy problem.
- Brak wspólnej masy = losowe błędy UART/SPI.
- Długi kabel między modułami w aucie = zakłócenia i błędne dane.

## 9) Co trzeba potwierdzić przed lutowaniem (blokery)
1. Dokładny model modułu GPS (np. NEO-6M/NEO-M8N/ATGM336H).
2. Dokładny model slotu SD (goły socket vs moduł z konwerterem poziomów).
3. Czy będziesz kiedykolwiek podpinać obie płytki USB naraz do debugowania.
4. Czy potrzebujesz tylko GPS->C5 (1 kierunek), czy pełny UART 2-kierunkowy.

## 10) Źródła pinout/spec (oficjalne)
- XIAO ESP32-C5 Getting Started: https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/
- XIAO ESP32-C6 Getting Started: https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/
- XIAO ESP32-C5 Pin Multiplexing: https://wiki.seeedstudio.com/xiao_esp32c5_pin_multiplexing/
- XIAO ESP32-C6 Pin Multiplexing: https://wiki.seeedstudio.com/xiao_pin_multiplexing_esp32c6/

## 11) Uwaga prawna
Projektuj i używaj urządzenie wyłącznie zgodnie z prawem lokalnym. Pasywny nasłuch i geolokalizacja sieci Wi‑Fi mogą podlegać ograniczeniom prawnym zależnie od kraju.
