# MOKKA Display – Monday Programm

Zeigt das **heutige bzw. nächste Event** aus dem Monday-Board *MOKKA PROGRAMM*
auf dem LilyGo T5 4.7" E-Paper (ESP32-S3) an. Aktualisiert sich alle 10 Minuten.

## 1. Bibliotheken
- **LilyGo EPD47** (`epd_driver.h` / `utilities.h` – schon installiert)
- **TJpg_Decoder** (für die SD-Bilder – schon installiert)

Monday-Antwort wird im Code selbst geparst (kein ArduinoJson nötig).

Board-Einstellungen (Werkzeuge):
- Board: `ESP32S3 Dev Module`
- **PSRAM: OPI PSRAM**
- **ESP32-Core: 2.0.14** (NICHT 3.x — sonst Bibliotheks-Fehler)
- **Partition Scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`** ← nötig, seit Logos +
  SD/JPG + Text-Engine wieder drin sind (sonst „Sketch too big").

Im Projekt liegt zusätzlich eine **`partitions.csv`** (3MB-App). Diese hat im
ESP32-Build automatisch Vorrang vor der Menü-Auswahl, d.h. die echte
Flash-Aufteilung ist fest 3MB-App – egal was im Menü steht. Die Menü-Einstellung
„Huge APP" trotzdem setzen, damit auch die **Grössen-Prüfung** der IDE auf 3MB
geht (sonst meldet die IDE evtl. fälschlich „zu gross", obwohl genug Platz ist).

## Aufbau der Weboberfläche (mokka-display-kasse.local)
Eine **Überseite** (Logo, Status, drei Buttons) → drei Funktionsseiten:
- **/monday** – Monday-Event-Liste + „Jetzt aktualisieren"; Event antippen → aufs Display
- **/text** – Text-Editor (1:1 aus Skript 3, inkl. Preis-/Grossbuchstaben-/Auto-Modus)
- **/preise** – FAVORITEN + ALLE PREISE von der SD-Karte; antippen → JPG aufs Display

Dateien: `02_MOKKA_MONDAY_PROGRAMM.ino`, `MokkaAmsiFonts.h`, `MokkaWebAssets.h`
(Logo + Wordmark, 1:1 aus Skript 3).

## 2. Monday API-Token eintragen
In `02_MOKKA_MONDAY_PROGRAMM.ino` die Zeile anpassen:

```cpp
const char* MONDAY_TOKEN = "HIER_DEINEN_MONDAY_API_TOKEN_EINFUEGEN";
```

Token holen: Monday → Profilbild (oben rechts) → **Developers → My Access Tokens**
(oder Administration → Connections → API). Der Token ist wie ein Passwort –
nicht öffentlich teilen / nicht auf GitHub committen.

## 3. Hochladen
Sketch öffnen, kompilieren, auf den T5 flashen. Fertig.

## Welche Daten werden gezeigt (von oben nach unten)
| Position            | Inhalt                   | Monday-Spalte | Spalten-ID     |
|---------------------|--------------------------|---------------|----------------|
| Gross, zentriert    | PREIS (z.B. `30.–`)      | TICKET PREIS  | `zahlen2`      |
| Darunter, kleiner   | BANDNAME                 | Event-Name    | `name`         |
| Zuunterst, klein    | `FREITAG 26. JUNI 2026`  | DATUM         | `datum4`       |

Preis = 0 bzw. „KOLLEKTE/EINTRITT FREI" (`status_10`) → es wird automatisch
**EINTRITT FREI** bzw. **KOLLEKTE** statt „0.–" angezeigt.

Logik: Gibt es ein Event mit **heutigem Datum** (Gruppe *TODAY*), wird das gezeigt.
Sonst das **nächste kommende** Event (Gruppe *DIE NÄCHSTEN 2 WOCHEN*).

## Weboberfläche
Das Display spannt ein eigenes WLAN auf (**MOKKA-DISPLAY** / `mokka1234`) und
hängt zusätzlich im Haus-WLAN. Im Browser erreichbar unter:

- **http://mokka-display-kasse.local**  (oder die IP-Adresse des Geräts)

Dort gibt es:
- **Status** (verbundenes WLAN, heutiges Datum, was gerade auf dem Display läuft)
- **„Jetzt von Monday aktualisieren"** – holt frische Daten und zeigt das
  heutige bzw. nächste Event
- **Liste der kommenden Events** – ein Event antippen → es wird sofort auf dem
  E-Paper angezeigt (überschreibt für 10 Min die automatische Auswahl)

Automatisch aktualisiert sich das Display ohnehin alle 10 Minuten.

## Anpassen
- **Intervall:** `REFRESH_MS` (Standard 10 Min).
- **Anderes Board/Spalten:** `BOARD_ID`, `GROUP_*`, `COL_*` oben im Code.
- **Layout/Schriften:** Funktion `renderEvent()`.

## Wichtig: Patch in der EPD47-Bibliothek (einmalig)
Mit ESP32-Core 3.x / ESP-IDF 5 gibt es den ROM-JPG-Decoder `tjpgd.h` nicht mehr,
weshalb die Bibliotheks-Datei `LilyGo-EPD47/src/libjpeg/libjpeg.c` nicht mehr
kompiliert (`fatal error: esp32/rom/tjpgd.h: No such file or directory`).

Diese Datei wurde gepatcht: ist kein `tjpgd`-Header vorhanden, wird sie inert
kompiliert. Wir nutzen den JPG-Decoder der Bibliothek nicht (eigener Renderer),
darum ist das gefahrlos. Backup liegt daneben als `libjpeg.c.bak`.

➜ **Achtung:** Ein Update der LilyGo-EPD47-Bibliothek überschreibt den Patch –
dann den Fehler ggf. erneut so beheben.

## Hinweise
- HTTPS-Zertifikat wird mit `client.setInsecure()` nicht geprüft (für diesen
  Zweck ausreichend). Für höhere Sicherheit Monday-Root-CA hinterlegen.
- Der Token liegt im Klartext im Flash. Wer das vermeiden will, nutzt die
  Variante „über Zwischen-Server" (kleiner Proxy liefert nur die nötigen Felder).
