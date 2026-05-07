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
| TX->RX | D6 (TX) | D7 (RX) | Główny kanał przesyłu pozycji GPS |
| RX<-TX (opcjonalnie) | D7 (RX) | D6 (TX) | Przydatne do ACK/komend/debug |
| Masa | GND | GND | Bez wspólnej masy UART będzie niestabilny |

## 5) Weryfikacja konfliktów pinów

### C5 (Wi-Fi + SD + link do C6)
- Zajęte: `D2`, `D6`, `D7`, `D8`, `D9`, `D10`
- Wolne: `D0`, `D1`, `D3`, `D4`, `D5`
- Brak konfliktu z mapą SPI/UART.

### C6 (GPS + link do C5)
- Zajęte: `D6`, `D7` (+ opcjonalnie `D1` dla PPS)
- Wolne: `D0`, `D2`, `D3`, `D4`, `D5`, `D8`, `D9`, `D10`
- Uwaga: `GPIO3` i `GPIO14` są związane z przełączaniem anteny RF (nie używaj ich przypadkowo do innych krytycznych funkcji).

## 6) Zasilanie w aucie (ważne)

### Zalecany tor zasilania
1. `12V/ACC` (po zapłonie) ->
2. przetwornica buck `12V -> 5V` automotive (stabilna, filtrująca zakłócenia) ->
3. rozdział `5V` do obu XIAO (pin 5V) ->
4. peryferia z 3V3 z odpowiedniego XIAO lub z osobnego stabilizatora 3.3V, jeśli moduły są prądożerne.

### Dobre praktyki
- Dodaj bezpiecznik na linii zasilania.
- Dodaj filtrację (kondensatory low-ESR blisko modułów).
- Przewody UART/SPI prowadź możliwie krótko.
- W aucie unikaj "latających" przewodów bez mocowania (wibracje + resety).

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
3. Czy chcesz zasilanie po `ACC` (po zapłonie), czy stałe 12V + soft power-off.
4. Czy potrzebujesz tylko GPS->C5 (1 kierunek), czy pełny UART 2-kierunkowy.

## 10) Źródła pinout/spec (oficjalne)
- XIAO ESP32-C5 Getting Started: https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/
- XIAO ESP32-C6 Getting Started: https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/
- XIAO ESP32-C5 Pin Multiplexing: https://wiki.seeedstudio.com/xiao_esp32c5_pin_multiplexing/
- XIAO ESP32-C6 Pin Multiplexing: https://wiki.seeedstudio.com/xiao_pin_multiplexing_esp32c6/

## 11) Uwaga prawna
Projektuj i używaj urządzenie wyłącznie zgodnie z prawem lokalnym. Pasywny nasłuch i geolokalizacja sieci Wi‑Fi mogą podlegać ograniczeniom prawnym zależnie od kraju.
