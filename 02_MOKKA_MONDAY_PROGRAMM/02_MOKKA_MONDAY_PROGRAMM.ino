//----------------------------------------------------
// MOKKA DISPLAY - MONDAY PROGRAMM V1.0
//----------------------------------------------------
// Zeigt das heutige bzw. naechste Event aus dem
// Monday-Board "MOKKA PROGRAMM" auf dem E-Paper an.
//
// Hardware: LilyGo T5 4.7" E-Paper, ESP32-S3
//
// Ablauf:
//   - WLAN verbinden
//   - Uhrzeit/Datum per NTP holen
//   - Monday GraphQL-API per HTTPS abfragen
//   - Heutiges oder naechstes Event ermitteln
//   - Mit AMSI-Fonts aufs Display rendern
//   - Aktualisierung manuell ueber die Weboberflaeche (kein Auto-Refresh, Akku-Betrieb)
//
// BENOETIGTE BIBLIOTHEKEN (Library Manager):
//   - LilyGo EPD47 (epd_driver.h / utilities.h)  <- wie beim Original
//   (Keine weitere Bibliothek noetig - JSON wird selbst geparst.)
//----------------------------------------------------


#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM: Tools -> PSRAM -> OPI PSRAM"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <SPI.h>
#include <SD.h>
#include <TJpg_Decoder.h>

#include "epd_driver.h"
#include "utilities.h"
#include "MokkaAmsiFonts.h"
#include "MokkaWebAssets.h"   // LOGO_B64 + WORDMARK_B64


//====================================================
// KONFIGURATION  --  HIER ANPASSEN
//====================================================

// --- WLAN (wie im Original) ---
struct WifiCred {
  const char* ssid;
  const char* pass;
};

WifiCred networks[] = {
  {"RESPECT", "MOKKA@home2022!"},
  {"MSA-2.4G", "C7Zw57RdDdJtTsAd7Z"}
};
const int NETWORK_COUNT = sizeof(networks) / sizeof(networks[0]);

// --- Direkt-WLAN (Accesspoint) + lokale Adresse ---
// Erreichbar ueber http://mokka-display-kasse.local  (oder die IP)
const char* AP_SSID   = "MOKKA-DISPLAY";
const char* AP_PASS   = "mokka1234";
const char* MDNS_NAME = "mokka-display-kasse";   // -> mokka-display-kasse.local

// --- SD-Karte (wie Original Skript 3) ---
#define SD_SCK  11
#define SD_MISO 16
#define SD_MOSI 15
#define SD_CS   42

// --- Monday API ---
// WICHTIG: Persoenlichen API-Token bei Monday holen:
//   Profilbild (oben rechts) -> Developers -> My Access Tokens
//   ODER: Administration -> Connections -> API -> persoenliches Token
// Den Token hier eintragen:
const char* MONDAY_TOKEN = "eyJhbGciOiJIUzI1NiJ9.eyJ0aWQiOjEzNDU4MjE0NCwiYWFpIjoxMSwidWlkIjoxNjk1OTMzOSwiaWFkIjoiMjAyMS0xMS0yNFQyMjo1ODowNC4wMDBaIiwicGVyIjoibWU6d3JpdGUiLCJhY3RpZCI6NzQ2MDM2MSwicmduIjoidXNlMSJ9.i6BloXVCKukSSWwBeYGKUmFLQySTLuM5G5KSlYhgIMw";

// Zwei Quellen: MOKKA PROGRAMM (Shows) und MOKKA DANCE (DJ/Disco) - umschaltbar.
struct Source {
  const char* label;
  const char* boardId;
  const char* groupToday;
  const char* groupNext;
  const char* colTime;     // SHOWTIME-Spalte (unterscheidet sich je Board!)
};
Source SOURCES[] = {
  {"PROGRAMM", "5297894413", "neue_gruppe88878", "neue_gruppe16505", "drop_down8"},
  {"DANCE",    "2228897408", "neue_gruppe63884", "neue_gruppe",      "drop_down2"},
};
const int SOURCE_COUNT = sizeof(SOURCES) / sizeof(SOURCES[0]);   // beide werden geladen

// Spalten-IDs (auf beiden Boards gleich, ausser SHOWTIME oben)
const char* COL_DATE     = "datum4";      // DATUM            -> "2026-06-26"
const char* COL_PRICE    = "zahlen2";     // PREIS/EINTRITT   -> "30"
const char* COL_TICKETART = "status_10";  // PROGRAMM: "KOLLEKTE/EINTRITT FREI" (bei DANCE anderer Sinn)

// --- Zeitzone (Schweiz, automatische Sommerzeit) ---
const char* TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";


//====================================================
// GLOBALS
//====================================================

uint8_t *framebuffer = NULL;

WebServer server(80);

unsigned long lastRefresh = 0;
String lastError = "";
String connectedSSID = "nur Direkt-WLAN";

// Ein Event
struct Event {
  bool   valid;
  bool   isToday;
  int    source;    // 0 = PROGRAMM (Konzert), 1 = DANCE
  String name;
  String dateRaw;   // YYYY-MM-DD
  String price;     // Roh-Zahl, z.B. "30" oder "0"
  String ticketart; // z.B. "KOLLEKTE/EINTRITT FREI"
  String time;      // SHOWTIME, z.B. "20:00"
};

// Liste der zuletzt geladenen, kommenden Events (beide Quellen zusammen)
const int MAX_EVENTS = 24;
Event   events[MAX_EVENTS];
int     eventCount = 0;
int     autoIndex  = -1;   // Index des automatisch gewaehlten Events
int     shownIndex = -1;   // Index des aktuell auf dem Display gezeigten Events

// SD-Karte / JPG (wie Original Skript 3)
bool    sdOK = false;
String  sdMessage = "Noch nicht geprueft";
String  currentFile = "";
String  lastMessage = "";
const int MAX_FILES = 160;
String  files[MAX_FILES];
int     fileCount = 0;

// Dekodierte Web-Bilder (Logo/Wordmark) im PSRAM -> als gecachte Endpunkte ausliefern
uint8_t* logoPng = nullptr;     size_t logoPngLen = 0;
uint8_t* wordmarkPng = nullptr; size_t wordmarkPngLen = 0;

// Wochentage (voll, fuer MOKKA-Datumsformat)
const char* WEEKDAYS_FULL_DE[7] = {
  "SONNTAG", "MONTAG", "DIENSTAG", "MITTWOCH", "DONNERSTAG", "FREITAG", "SAMSTAG"
};
// Kurzform (nur Fallback, wenn die Datums+Zeit-Zeile sonst zu breit waere)
const char* WEEKDAYS_SHORT_DE[7] = {
  "SO", "MO", "DI", "MI", "DO", "FR", "SA"
};

// Monate (Index 1..12)
const char* MONTHS_DE[13] = {
  "", "JANUAR", "FEBRUAR", "MÄRZ", "APRIL", "MAI", "JUNI",
  "JULI", "AUGUST", "SEPTEMBER", "OKTOBER", "NOVEMBER", "DEZEMBER"
};


//====================================================
// UTF-8 / CODEPOINTS  (aus Original)
//====================================================

uint16_t nextCodepoint(const char* s, int &i) {
  uint8_t c = (uint8_t)s[i++];
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) {
    uint8_t c2 = (uint8_t)s[i++];
    return ((c & 0x1F) << 6) | (c2 & 0x3F);
  }
  if ((c & 0xF0) == 0xE0) {
    uint8_t c2 = (uint8_t)s[i++];
    uint8_t c3 = (uint8_t)s[i++];
    return ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
  }
  return '?';
}


//====================================================
// BASE64 DECODER (fuer gecachte Web-Bilder)
//====================================================

static int b64val(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;   // '=', Whitespace, Zeilenumbruch -> ignorieren
}

// Dekodiert einen PROGMEM-Base64-String nach out[]. Gibt die Byte-Laenge zurueck.
size_t b64decode(const char* src, uint8_t* out) {
  size_t outLen = 0, i = 0;
  int buf = 0, bits = 0;
  uint8_t c;
  while ((c = pgm_read_byte(src + i++)) != 0) {
    int v = b64val(c);
    if (v < 0) continue;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out[outLen++] = (uint8_t)((buf >> bits) & 0xFF); }
  }
  return outLen;
}


//====================================================
// DISPLAY LOW LEVEL  (aus Original)
//====================================================

void setPixel4(int x, int y, uint8_t gray) {
  if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
  uint32_t index = y * (EPD_WIDTH / 2) + (x / 2);
  if (x & 1) {
    framebuffer[index] = (framebuffer[index] & 0xF0) | (gray & 0x0F);
  } else {
    framebuffer[index] = (framebuffer[index] & 0x0F) | ((gray & 0x0F) << 4);
  }
}

void fillScreen4(uint8_t gray) {
  uint8_t packed = (gray << 4) | (gray & 0x0F);
  memset(framebuffer, packed, EPD_WIDTH * EPD_HEIGHT / 2);
}

void pushFramebuffer() {
  epd_poweron();
  epd_clear();
  epd_draw_grayscale_image(epd_full_screen(), framebuffer);
  epd_poweroff();
}

void drawWhiteFrame() {
  epd_draw_rect(24, 24, EPD_WIDTH - 48, EPD_HEIGHT - 48, 15, framebuffer);
}


//====================================================
// AMSI FONT RENDERER  (aus Original)
//====================================================

MokkaGlyph getGlyph(const MokkaFont &font, uint16_t cp, bool &found) {
  for (uint16_t i = 0; i < font.glyphCount; i++) {
    MokkaGlyph g;
    memcpy_P(&g, &font.glyphs[i], sizeof(MokkaGlyph));
    if (g.codepoint == cp) { found = true; return g; }
  }
  for (uint16_t i = 0; i < font.glyphCount; i++) {
    MokkaGlyph g;
    memcpy_P(&g, &font.glyphs[i], sizeof(MokkaGlyph));
    if (g.codepoint == '?') { found = true; return g; }
  }
  found = false;
  MokkaGlyph empty = {0,0,0,0,0,0,0};
  return empty;
}

int textWidth(const MokkaFont &font, String text) {
  int w = 0;
  const char* s = text.c_str();
  int i = 0;
  while (s[i] != 0) {
    uint16_t cp = nextCodepoint(s, i);
    if (cp == '\n' || cp == '\r') continue;
    bool found;
    MokkaGlyph g = getGlyph(font, cp, found);
    if (found) w += g.xAdvance;
  }
  return w;
}

void drawGlyph(const MokkaFont &font, const MokkaGlyph &g, int x, int y, uint8_t color) {
  uint16_t byteWidth = (g.width + 7) / 8;
  for (uint16_t yy = 0; yy < g.height; yy++) {
    for (uint16_t xx = 0; xx < g.width; xx++) {
      uint32_t dataIndex = g.offset + yy * byteWidth + (xx / 8);
      uint8_t b = pgm_read_byte(font.bitmap + dataIndex);
      bool on = b & (0x80 >> (xx & 7));
      if (on) setPixel4(x + g.xOffset + xx, y + g.yOffset + yy, color);
    }
  }
}

void drawTextLine(const MokkaFont &font, String text, int x, int baselineY, uint8_t color) {
  const char* s = text.c_str();
  int i = 0;
  int cursor = x;
  while (s[i] != 0) {
    uint16_t cp = nextCodepoint(s, i);
    if (cp == '\n' || cp == '\r') continue;
    bool found;
    MokkaGlyph g = getGlyph(font, cp, found);
    if (found) {
      drawGlyph(font, g, cursor, baselineY, color);
      cursor += g.xAdvance;
    }
  }
}

// Zeile horizontal zentriert zeichnen
void drawCentered(const MokkaFont &font, String text, int baselineY, uint8_t color) {
  int w = textWidth(font, text);
  int x = (EPD_WIDTH - w) / 2;
  drawTextLine(font, text, x, baselineY, color);
}

// --- Skaliertes Zeichnen (Nearest-Neighbor, ueber Zielpixel -> keine Loecher) ---
void drawGlyphScaled(const MokkaFont &font, const MokkaGlyph &g, int x, int baselineY, uint8_t color, float s) {
  uint16_t byteWidth = (g.width + 7) / 8;
  int dW = (int)(g.width  * s + 0.5f);
  int dH = (int)(g.height * s + 0.5f);
  int ox = x         + (int)(g.xOffset * s + 0.5f);
  int oy = baselineY + (int)(g.yOffset * s + 0.5f);
  for (int dy = 0; dy < dH; dy++) {
    int sy = (int)(dy / s); if (sy >= g.height) sy = g.height - 1;
    for (int dx = 0; dx < dW; dx++) {
      int sx = (int)(dx / s); if (sx >= g.width) sx = g.width - 1;
      uint8_t b = pgm_read_byte(font.bitmap + g.offset + sy * byteWidth + (sx / 8));
      if (b & (0x80 >> (sx & 7))) setPixel4(ox + dx, oy + dy, color);
    }
  }
}

void drawTextLineScaled(const MokkaFont &font, String text, int x, int baselineY, uint8_t color, float s) {
  const char* str = text.c_str();
  int i = 0;
  float cursor = x;
  while (str[i] != 0) {
    uint16_t cp = nextCodepoint(str, i);
    if (cp == '\n' || cp == '\r') continue;
    bool found;
    MokkaGlyph g = getGlyph(font, cp, found);
    if (found) {
      drawGlyphScaled(font, g, (int)(cursor + 0.5f), baselineY, color, s);
      cursor += g.xAdvance * s;
    }
  }
}


//====================================================
// TEXT WRAP  (aus Original, leicht gekuerzt)
//====================================================

void wrapManualLine(const MokkaFont &font, String input, String outLines[], int &outCount, int maxLines, int maxWidth) {
  input.trim();
  if (input.length() == 0) return;
  String current = "";
  while (input.length() > 0 && outCount < maxLines) {
    int p = input.indexOf(' ');
    String word;
    if (p < 0) { word = input; input = ""; }
    else { word = input.substring(0, p); input = input.substring(p + 1); }
    word.trim();
    if (word.length() == 0) continue;
    String test = current;
    if (test.length() > 0) test += " ";
    test += word;
    if (current.length() == 0 || textWidth(font, test) <= maxWidth) {
      current = test;
    } else {
      outLines[outCount++] = current;
      current = word;
    }
  }
  if (current.length() > 0 && outCount < maxLines) outLines[outCount++] = current;
}

bool wrapTextForFont(const MokkaFont &font, String text, String outLines[], int &outCount, int maxLines, int maxWidth) {
  text.replace("\r", "");
  outCount = 0;
  while (text.length() > 0 && outCount < maxLines) {
    int p = text.indexOf('\n');
    String line;
    if (p < 0) { line = text; text = ""; }
    else { line = text.substring(0, p); text = text.substring(p + 1); }
    wrapManualLine(font, line, outLines, outCount, maxLines, maxWidth);
  }
  for (int i = 0; i < outCount; i++) {
    if (textWidth(font, outLines[i]) > maxWidth) return false;
  }
  return outCount <= maxLines;
}


//====================================================
// ZEIT / DATUM
//====================================================

// Holt das heutige Datum als "YYYY-MM-DD". Leer bei Fehler.
String todayDate() {
  struct tm t;
  if (!getLocalTime(&t, 5000)) return "";
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}

// "2026-06-26" -> "FREITAG 26. JUNI 2026"  (MOKKA-Format)
String formatDateMokka(String dateRaw, bool shortWeekday = false) {
  if (dateRaw.length() < 10) return dateRaw;
  int year  = dateRaw.substring(0, 4).toInt();
  int month = dateRaw.substring(5, 7).toInt();
  int day   = dateRaw.substring(8, 10).toInt();

  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = day;
  t.tm_hour = 12;
  mktime(&t);                       // fuellt tm_wday
  int wd = t.tm_wday;               // 0=So .. 6=Sa

  String out = "";
  if (wd >= 0 && wd <= 6) out += String(shortWeekday ? WEEKDAYS_SHORT_DE[wd] : WEEKDAYS_FULL_DE[wd]) + " ";
  out += String(day) + ". ";
  if (month >= 1 && month <= 12) out += String(MONTHS_DE[month]) + " ";
  out += String(year);
  return out;
}


//====================================================
// MONDAY API
//====================================================

// Baut die GraphQL-Abfrage (ohne JSON-Escaping)
String buildQuery(int s) {
  const Source &src = SOURCES[s];
  String q = "query { boards(ids: ";
  q += src.boardId;
  q += ") { groups(ids: [\"";
  q += src.groupToday; q += "\",\""; q += src.groupNext;
  q += "\"]) { id items_page(limit: 15) { items { name column_values(ids: [\"";
  q += COL_DATE; q += "\",\""; q += COL_PRICE; q += "\",\""; q += COL_TICKETART; q += "\",\""; q += src.colTime;
  q += "\"]) { id text } } } } } }";
  return q;
}

// Verpackt die Query JSON-sicher in den Request-Body
String buildBody(int s) {
  String q = buildQuery(s);
  String body = "{\"query\":\"";
  for (unsigned int i = 0; i < q.length(); i++) {
    char c = q.charAt(i);
    if (c == '"')      body += "\\\"";
    else if (c == '\\') body += "\\\\";
    else                body += c;
  }
  body += "\"}";
  return body;
}

// --- Mini-JSON-Parser (genuegt fuer diese feste Antwortstruktur) ---

// Liest einen JSON-String. s[i] steht auf dem ersten Zeichen NACH dem
// oeffnenden Anfuehrungszeichen; danach zeigt i hinter das schliessende.
String readJsonString(const String &s, int &i) {
  String out = "";
  int n = s.length();
  while (i < n) {
    char c = s.charAt(i++);
    if (c == '\\') {
      if (i >= n) break;
      char e = s.charAt(i++);
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += ' ';  break;
        case 'r': break;
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'u': {
          if (i + 4 <= n) {
            long cp = strtol(s.substring(i, i + 4).c_str(), NULL, 16);
            i += 4;
            if (cp < 0x80) {
              out += (char)cp;
            } else if (cp < 0x800) {
              out += (char)(0xC0 | (cp >> 6));
              out += (char)(0x80 | (cp & 0x3F));
            } else {
              out += (char)(0xE0 | (cp >> 12));
              out += (char)(0x80 | ((cp >> 6) & 0x3F));
              out += (char)(0x80 | (cp & 0x3F));
            }
          }
          break;
        }
        default: out += e;
      }
    } else if (c == '"') {
      break;
    } else {
      out += c;
    }
  }
  return out;
}

// Sucht in einem Ausschnitt (einem Item) die Spalte mit gegebener id und
// liefert deren "text"-Wert ("" wenn null oder nicht gefunden).
String colInSlice(const String &slice, const char* colId) {
  String key = String("\"id\":\"") + colId + "\"";
  int p = slice.indexOf(key);
  if (p < 0) return "";
  int t = slice.indexOf("\"text\":", p);
  if (t < 0) return "";
  t += 7;                                  // hinter "text":
  while (t < (int)slice.length() && slice.charAt(t) == ' ') t++;
  if (t < (int)slice.length() && slice.charAt(t) == '"') {
    t++;                                   // hinter oeffnendes Anfuehrungszeichen
    return readJsonString(slice, t);
  }
  return "";                               // text war null
}

// Laedt BEIDE Quellen (PROGRAMM + DANCE) in events[], tagt e.source,
// und bestimmt autoIndex (heutiges -> sonst naechstes Event).
bool fetchEvents() {
  eventCount = 0;
  autoIndex  = -1;

  if (WiFi.status() != WL_CONNECTED) { lastError = "Kein WLAN"; return false; }

  String today = todayDate();   // kann leer sein, dann nur "naechstes"

  WiFiClientSecure client;
  client.setInsecure();         // Monday-Zertifikat nicht pruefen (genuegt hier)

  bool gotAny = false;
  String err = "";

  for (int s = 0; s < SOURCE_COUNT && eventCount < MAX_EVENTS; s++) {
    HTTPClient https;
    https.setTimeout(15000);
    if (!https.begin(client, "https://api.monday.com/v2")) { err = "HTTPS begin"; continue; }
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", MONDAY_TOKEN);
    https.addHeader("API-Version", "2024-01");

    int code = https.POST(buildBody(s));
    if (code != 200) {
      err = "HTTP " + String(code);
      String resp = https.getString();
      if (resp.length() > 0) err += " / " + resp.substring(0, 100);
      https.end();
      continue;
    }
    String payload = https.getString();
    https.end();

    if (payload.indexOf("\"errors\"") >= 0 && payload.indexOf("\"name\":\"") < 0) {
      err = "Monday/Token? " + payload.substring(0, 100);
      continue;
    }
    gotAny = true;

    // Items dieser Quelle einlesen (Datum >= heute), e.source = s
    int searchFrom = 0;
    while (eventCount < MAX_EVENTS) {
      int n = payload.indexOf("\"name\":\"", searchFrom);
      if (n < 0) break;
      int ni = n + 8;
      String name = readJsonString(payload, ni);
      int next = payload.indexOf("\"name\":\"", ni);
      int sliceEnd = (next < 0) ? payload.length() : next;
      String slice = payload.substring(ni, sliceEnd);
      searchFrom = (next < 0) ? payload.length() : next;

      String dateRaw = colInSlice(slice, COL_DATE);
      if (dateRaw.length() >= 10) {
        bool future = (today.length() != 10) || (dateRaw >= today);
        if (future) {
          Event &e = events[eventCount];
          e.valid     = true;
          e.isToday   = (today.length() == 10 && dateRaw == today);
          e.source    = s;
          e.name      = name;
          e.dateRaw   = dateRaw;
          e.price     = colInSlice(slice, COL_PRICE);
          e.ticketart = colInSlice(slice, COL_TICKETART);
          e.time      = colInSlice(slice, SOURCES[s].colTime);
          eventCount++;
        }
      }
      if (next < 0) break;
    }
  }

  if (eventCount == 0) {
    lastError = gotAny ? "Keine kommenden Events" : (err.length() ? err : "Fehler");
    return false;
  }

  // Auto-Auswahl fuers Display: heutiges Event hat Vorrang, sonst kleinstes Datum.
  autoIndex = 0;
  for (int i = 0; i < eventCount; i++) {
    if (events[i].isToday) { autoIndex = i; break; }
    if (events[i].dateRaw < events[autoIndex].dateRaw) autoIndex = i;
  }

  lastError = "";
  return true;
}


//====================================================
// RENDERING
//====================================================

// Preis-Text aufbereiten.
//   isNumeric = true  -> Zahl/CHF, darstellbar mit dem Preis-Font
//   isNumeric = false -> Text wie "EINTRITT FREI" (normaler Font noetig)
String formatPrice(const Event &ev, bool &isNumeric) {
  String p = ev.price; p.trim();
  String art = ev.ticketart; art.toUpperCase();

  // Gratis / Kollekte
  if (p.length() == 0 || p == "0" || art.indexOf("FREI") >= 0) {
    isNumeric = false;
    if (art.indexOf("KOLLEKTE") >= 0) return "KOLLEKTE";
    return "EINTRITT FREI";
  }

  isNumeric = true;
  return p + ".–";          // MOKKA-Format, z.B. "30.–"
}

// Bandname: IMMER eine Zeile, automatisch die groesste Schrift, die in die
// Breite passt (Cond120 -> 88 -> 60). Kein Umbruch.
const MokkaFont* fitName(String name, String outLines[], int &outCount) {
  outCount = 1;
  outLines[0] = name;
  const int maxW = EPD_WIDTH - 80;
  if (textWidth(AmsiCond120, name) <= maxW) return &AmsiCond120;
  if (textWidth(AmsiCond88,  name) <= maxW) return &AmsiCond88;
  return &AmsiCond60;                 // kleinste; eine Zeile, ggf. randnah
}

// Vertikale Ausdehnung NUR der tatsaechlich gezeichneten Zeichen einer Zeile
// (top/bottom relativ zur Baseline). Wichtig, weil z.B. der Preis-Font ganz
// andere Glyph-Maße hat als die Cond-Fonts -- so stimmt die Hoehe pro Zeile.
void lineVExtent(const MokkaFont &font, String text, int &top, int &bottom) {
  top = 100000; bottom = -100000;
  const char* s = text.c_str();
  int i = 0;
  while (s[i] != 0) {
    uint16_t cp = nextCodepoint(s, i);
    if (cp == '\n' || cp == '\r') continue;
    bool found;
    MokkaGlyph g = getGlyph(font, cp, found);
    if (!found) continue;
    if (g.yOffset < top) top = g.yOffset;
    if (g.yOffset + (int)g.height > bottom) bottom = g.yOffset + (int)g.height;
  }
  if (top > bottom) { top = 0; bottom = 0; }   // leere Zeile
}

// Skaliert ein Wort aus AmsiUltraBig auf die Zielbreite (nur verkleinern -> scharf).
// Liefert die skalierte Hoehe; s und sTop (skalierte Oberkante rel. Baseline) per Ref.
int scaledWordMetrics(String word, float &s, int &sTop, int targetW) {
  int baseW = textWidth(AmsiUltraBig, word);
  if (baseW <= 0) { s = 1.0f; sTop = 0; return 0; }
  s = (float)targetW / (float)baseW;
  if (s > 1.0f) s = 1.0f;                  // nie vergroessern (Bitmap bliebe scharf)
  int t, b; lineVExtent(AmsiUltraBig, word, t, b);
  sTop = (int)(t * s + (t < 0 ? -0.5f : 0.5f));
  return (int)((b - t) * s + 0.5f);
}

// Zeichnet ein skaliertes Wort zentriert, Oberkante bei topY.
void drawWordScaled(String word, int topY, float s, int sTop, uint8_t color) {
  int wScaled = (int)(textWidth(AmsiUltraBig, word) * s + 0.5f);
  int x = (EPD_WIDTH - wScaled) / 2;
  drawTextLineScaled(AmsiUltraBig, word, x, topY - sTop, color, s);
}

// Block-Oberkante (rel. zur Baseline der 1. Zeile) + Gesamthoehe ueber alle Zeilen.
void blockVExtent(const MokkaFont &font, String lines[], int count, int &blockTop, int &blockH) {
  if (count <= 0) { blockTop = 0; blockH = 0; return; }
  int lineHeight = font.size * 0.86;
  int t0, b0;       lineVExtent(font, lines[0], t0, b0);
  int tLast, bLast; lineVExtent(font, lines[count - 1], tLast, bLast);
  blockTop = t0;
  int blockBottom = (count - 1) * lineHeight + bLast;
  blockH = blockBottom - blockTop;
}

// Zeichnet einen (ggf. mehrzeiligen) Block so, dass seine OBERKANTE bei topY liegt.
void drawBlockTop(const MokkaFont &font, String lines[], int count, int topY, uint8_t color) {
  if (count <= 0) return;
  int lineHeight = font.size * 0.86;
  int bt, bh; blockVExtent(font, lines, count, bt, bh);
  int baseline0 = topY - bt;
  for (int i = 0; i < count; i++) drawCentered(font, lines[i], baseline0 + i * lineHeight, color);
}

void renderEvent(const Event &ev) {
  fillScreen4(0);      // schwarzer Hintergrund (wie Original-Textmodus)
  drawWhiteFrame();    // weisser Rahmen

  // 1) PREIS (gross)
  bool priceNumeric = false;
  String priceText = formatPrice(ev, priceNumeric);
  priceText.toUpperCase();
  String priceLines[1]; priceLines[0] = priceText;

  const MokkaFont* pf = &AmsiUltraPrice;   // nur fuer numerische Preise
  bool  priceScaled = false;               // Text-Preise werden skaliert
  float pScale = 1.0f; int pScaledTop = 0;
  int pH;
  if (priceNumeric) {
    int ptmp; blockVExtent(*pf, priceLines, 1, ptmp, pH);
  } else {
    // KOLLEKTE / EINTRITT FREI / GRATIS: aus AmsiUltraBig auf Breite skalieren
    priceScaled = true;
    pH = scaledWordMetrics(priceText, pScale, pScaledTop, EPD_WIDTH - 80);
  }

  // 2) BANDNAME (kleiner)
  String name = ev.name; name.toUpperCase();   // '#' wird jetzt korrekt gerendert (Font ergaenzt)
  String nameLines[3];
  int nameCount = 0;
  const MokkaFont* nf = fitName(name, nameLines, nameCount);

  // 3) DATUM (am kleinsten) + SHOWTIME -- IMMER eine Zeile
  String dateLines[1];
  int dateCount = 1;
  String t = ev.time; t.trim(); t.toUpperCase();   // PROGRAMM "20:00" | DANCE "23:00 UHR"
  String timeSuffix = "";
  if (t.length() > 0) timeSuffix = "  " + t + (t.indexOf("UHR") >= 0 ? "" : " UHR");  // kein doppeltes UHR
  dateLines[0] = formatDateMokka(ev.dateRaw) + timeSuffix;                      // FREITAG 26. JUNI 2026  20:00 UHR
  // Zu breit (lange Wochentag+Monat-Kombi)? -> Wochentag kuerzen, bleibt 1 Zeile
  if (textWidth(AmsiCond60, dateLines[0]) > EPD_WIDTH - 60) {
    dateLines[0] = formatDateMokka(ev.dateRaw, true) + timeSuffix;              // z.B. DO 26. SEPTEMBER 2026  20:00 UHR
  }

  //--------------------------------------------------
  // VERTIKALE ZENTRIERUNG des gesamten Stapels
  // (Hoehen aus den ECHTEN Zeichen, pro Font)
  //--------------------------------------------------
  int nt, nH; blockVExtent(*nf, nameLines, nameCount, nt, nH);
  int dt, dH; blockVExtent(AmsiCond60, dateLines, dateCount, dt, dH);

  // Reihenfolge oben->unten: DATUM/SHOWTIME, PREIS, BANDNAME
  int GAP1 = 44;   // Abstand Datum -> Preis
  int GAP2 = 44;   // Abstand Preis -> Name
  int total = dH + GAP1 + pH + GAP2 + nH;

  // Bei sehr hohem Inhalt (langer 2-zeiliger Name) Abstaende verkleinern
  while (total > EPD_HEIGHT - 60 && GAP1 > 12) {
    GAP1 -= 4; GAP2 -= 4;
    total = dH + GAP1 + pH + GAP2 + nH;
  }

  int top = (EPD_HEIGHT - total) / 2;
  if (top < 30) top = 30;

  // 1) DATUM + SHOWTIME (oben)
  drawBlockTop(AmsiCond60, dateLines, dateCount, top, 15);   top += dH + GAP1;
  // 2) PREIS (Mitte)
  if (priceScaled) drawWordScaled(priceText, top, pScale, pScaledTop, 15);
  else             drawBlockTop(*pf, priceLines, 1, top, 15);
  top += pH + GAP2;
  // 3) BANDNAME (unten)
  drawBlockTop(*nf, nameLines, nameCount, top, 15);

  pushFramebuffer();
}

// Fallback-Anzeige (kein Event / Fehler)
void renderMessage(String title, String sub) {
  fillScreen4(0);
  drawWhiteFrame();
  String t = title; t.toUpperCase();
  drawCentered(AmsiCond120, t, EPD_HEIGHT / 2, 15);
  if (sub.length() > 0) {
    String s = sub; s.toUpperCase();
    String lines[2]; int n = 0;
    wrapTextForFont(AmsiCond60, s, lines, n, 2, EPD_WIDTH - 120);
    int base = EPD_HEIGHT - 90;
    int lh = AmsiCond60.size * 0.86;
    for (int i = 0; i < n; i++) drawCentered(AmsiCond60, lines[i], base + i * lh, 15);
  }
  pushFramebuffer();
}


//====================================================
// URL / DATEI-HELFER  (1:1 aus Skript 3)
//====================================================

String urlDecode(String input) {
  String s = "";
  char c;
  for (int i = 0; i < input.length(); i++) {
    c = input.charAt(i);
    if (c == '+') {
      s += ' ';
    } else if (c == '%' && i + 2 < input.length()) {
      String hex = input.substring(i + 1, i + 3);
      char decoded = (char) strtol(hex.c_str(), NULL, 16);
      s += decoded;
      i += 2;
    } else {
      s += c;
    }
  }
  return s;
}

// Minimaler URL-Encoder (fuer Dateinamen in <img src> / Links)
String urlEncode(String s) {
  String out = "";
  const char* hex = "0123456789ABCDEF";
  for (uint16_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else { out += '%'; out += hex[(c >> 4) & 0x0F]; out += hex[c & 0x0F]; }
  }
  return out;
}

bool isHiddenMacFile(String n) {
  n.toLowerCase();
  if (n.startsWith("/._") || n.startsWith("._")) return true;
  if (n.indexOf(".spotlight") >= 0) return true;
  if (n.indexOf(".trashes") >= 0) return true;
  if (n.indexOf(".fseventsd") >= 0) return true;
  if (n.indexOf(".ds_store") >= 0) return true;
  return false;
}

bool isJpg(String n) {
  String l = n;
  l.toLowerCase();
  if (isHiddenMacFile(l)) return false;
  return l.endsWith(".jpg") || l.endsWith(".jpeg");
}

String cleanName(String n) {
  n.replace("/", "");
  n.replace("KASSEN-ANZEIGE-", "");
  n.replace(".jpg", "");
  n.replace(".jpeg", "");
  n.replace(".JPG", "");
  n.replace(".JPEG", "");
  return n;
}

String labelFromName(String n) {
  String c = cleanName(n);
  String lc = c;
  lc.toLowerCase();
  if (lc == "ausverkauft") return "AUSVERKAUFT";
  if (lc == "privat") return "PRIVAT";
  if (lc == "mokkalogo") return "LOGO";
  if (lc == "welcome") return "WELCOME";
  if (lc == "gratis") return "GRATIS";
  return c + ".–";   // "30.–"
}


//====================================================
// SD-KARTE  (1:1 aus Skript 3)
//====================================================

bool initSD() {
  SD.end();
  SPI.end();
  delay(250);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(100);
  bool ok = SD.begin(SD_CS, SPI, 1000000);
  sdMessage = ok ? "OK" : "FEHLER";
  return ok;
}

void scanSD() {
  fileCount = 0;
  if (!sdOK) { sdMessage = "SD nicht bereit"; return; }

  File dir = SD.open("/");
  if (!dir || !dir.isDirectory()) {
    sdOK = false;
    sdMessage = "Root nicht geoeffnet";
    return;
  }
  File file = dir.openNextFile();
  while (file && fileCount < MAX_FILES) {
    String name = String(file.name());
    if (!file.isDirectory() && isJpg(name)) files[fileCount++] = name;
    file = dir.openNextFile();
  }
  sdMessage = "OK · " + String(fileCount) + " JPG-Dateien";
}


//====================================================
// JPG ANZEIGE  (1:1 aus Skript 3)
//====================================================

bool jpgCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  for (uint16_t yy = 0; yy < h; yy++) {
    for (uint16_t xx = 0; xx < w; xx++) {
      uint16_t c = bitmap[yy * w + xx];
      uint8_t r = ((c >> 11) & 0x1F) << 3;
      uint8_t g = ((c >> 5) & 0x3F) << 2;
      uint8_t b = (c & 0x1F) << 3;
      uint8_t lum = (r * 30 + g * 59 + b * 11) / 100;
      setPixel4(x + xx, y + yy, lum >> 4);
    }
  }
  return true;
}

bool showJpg(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  if (!sdOK) { lastMessage = "SD nicht bereit"; return false; }

  File testFile = SD.open(filename);
  if (!testFile) { lastMessage = "Datei nicht geoeffnet: " + filename; return false; }
  size_t fileSize = testFile.size();
  testFile.close();

  fillScreen4(15);
  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpgCallback);

  JRESULT rc = TJpgDec.drawSdJpg(0, 0, filename.c_str());
  if (rc != JDR_OK) {
    lastMessage = "Decoder Fehler: " + filename + " / Code: " + String(rc) + " / Size: " + String(fileSize);
    return false;
  }
  pushFramebuffer();
  lastMessage = "Angezeigt: " + filename + " / Size: " + String(fileSize);
  return true;
}


//====================================================
// TEXT-ENGINE  (1:1 aus Skript 3)
//====================================================

bool isMaxMode(String text) {
  bool hasLowercase = false;
  for (int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (c >= 'a' && c <= 'z') { hasLowercase = true; break; }
  }
  return !hasLowercase;
}

bool isPriceText(String text) {
  String t = text; t.trim();
  if (t.length() == 0) return false;
  for (int i = 0; i < t.length(); i++) {
    char c = t.charAt(i);
    if (isDigit(c) || c == '.' || c == ':' || c == '-' || c == ' ' || c == 'C' || c == 'H' || c == 'F') continue;
    if (((uint8_t)c) >= 128) continue;
    return false;
  }
  return true;
}

void splitLines(String text, String lines[], int &lineCount, int maxLines) {
  text.replace("\r", "");
  lineCount = 0;
  while (text.length() > 0 && lineCount < maxLines) {
    int p = text.indexOf('\n');
    if (p < 0) {
      text.trim();
      if (text.length() > 0) lines[lineCount++] = text;
      break;
    } else {
      String line = text.substring(0, p);
      line.trim();
      if (line.length() > 0) lines[lineCount++] = line;
      text = text.substring(p + 1);
    }
  }
  if (lineCount == 0) lines[lineCount++] = "WILLKOMMEN";
}

const MokkaFont* chooseTextFont(String lines[], int lineCount) {
  const int budget = EPD_WIDTH - 80;   // echte nutzbare Breite (statt fix 820)

  int maxW = 0;
  for (int i = 0; i < lineCount; i++) {
    int w = textWidth(AmsiCond120, lines[i]);
    if (w > maxW) maxW = w;
  }
  if (maxW <= budget) return &AmsiCond120;

  maxW = 0;
  for (int i = 0; i < lineCount; i++) {
    int w = textWidth(AmsiCond88, lines[i]);
    if (w > maxW) maxW = w;
  }
  if (maxW <= budget) return &AmsiCond88;
  return &AmsiCond60;
}

int textBlockHeight(const MokkaFont &font, int lineCount) {
  int top = 10000;
  int bottom = -10000;
  for (uint16_t i = 0; i < font.glyphCount; i++) {
    MokkaGlyph g;
    memcpy_P(&g, &font.glyphs[i], sizeof(MokkaGlyph));
    if (g.yOffset < top) top = g.yOffset;
    if (g.yOffset + g.height > bottom) bottom = g.yOffset + g.height;
  }
  int glyphHeight = bottom - top;
  int lineHeight = font.size * 0.86;
  return glyphHeight + (lineCount - 1) * lineHeight;
}

void showAmsiText(String text) {
  text = urlDecode(text);
  text.trim();

  bool maxMode = isMaxMode(text);
  bool priceMode = isPriceText(text);

  text.toUpperCase();

  fillScreen4(0);     // schwarz
  drawWhiteFrame();   // weiss

  // GROSSBUCHSTABEN-Einzelwort (kein Leerzeichen/Umbruch): auf Breite skalieren,
  // damit auch lange Woerter wie GESCHLOSSEN/KOLLEKTE die Breite fuellen.
  if (maxMode && !priceMode && text.indexOf(' ') < 0 && text.indexOf('\n') < 0) {
    float s; int sTop;
    int hgt = scaledWordMetrics(text, s, sTop, EPD_WIDTH - 80);
    int topY = (EPD_HEIGHT - hgt) / 2;
    drawWordScaled(text, topY, s, sTop, 15);
    pushFramebuffer();
    currentFile = "TEXT";
    lastMessage = "AMSI Text angezeigt";
    return;
  }

  const int MAX_LINES = 5;
  String lines[MAX_LINES];
  int lineCount = 0;

  const MokkaFont* font;

  if (priceMode) {
    font = &AmsiUltraPrice;
    splitLines(text, lines, lineCount, MAX_LINES);
    int widest = 0;
    for (int i = 0; i < lineCount; i++) {
      int w = textWidth(*font, lines[i]);
      if (w > widest) widest = w;
    }
    if (widest > EPD_WIDTH - 80) font = &AmsiUltraBig;

  } else if (maxMode) {
    font = &AmsiUltraBig;
    splitLines(text, lines, lineCount, MAX_LINES);
    int widest = 0;
    for (int i = 0; i < lineCount; i++) {
      int w = textWidth(*font, lines[i]);
      if (w > widest) widest = w;
    }
    if (widest > EPD_WIDTH - 70) font = chooseTextFont(lines, lineCount);

  } else {
    if (wrapTextForFont(AmsiCond120, text, lines, lineCount, MAX_LINES, EPD_WIDTH - 120)) {
      font = &AmsiCond120;
    } else if (wrapTextForFont(AmsiCond88, text, lines, lineCount, MAX_LINES, EPD_WIDTH - 120)) {
      font = &AmsiCond88;
    } else {
      wrapTextForFont(AmsiCond60, text, lines, lineCount, MAX_LINES, EPD_WIDTH - 120);
      font = &AmsiCond60;
    }
  }

  int lineHeight = font->size * 0.86;
  int blockHeight = textBlockHeight(*font, lineCount);

  int yAdjust = 0;
  if (priceMode)      yAdjust = -7;
  else if (maxMode)   yAdjust = 30;
  else                yAdjust = 0;

  int firstY = (EPD_HEIGHT - blockHeight) / 2 + yAdjust;

  for (int i = 0; i < lineCount; i++) {
    int w = textWidth(*font, lines[i]);
    int x = (EPD_WIDTH - w) / 2;
    int y = firstY + i * lineHeight;
    drawTextLine(*font, lines[i], x, y, 15);
  }

  pushFramebuffer();
  currentFile = "TEXT";
  lastMessage = "AMSI Text angezeigt";
}


//====================================================
// WIFI
//====================================================

void startWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);          // Modem-Sleep aus -> Web-UI reagiert sofort (mehr Strom)
  WiFi.softAP(AP_SSID, AP_PASS);     // Direkt-WLAN immer aktiv
  connectedSSID = "nur Direkt-WLAN";

  for (int i = 0; i < NETWORK_COUNT; i++) {
    WiFi.begin(networks[i].ssid, networks[i].pass);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
      delay(300);
      tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      connectedSSID = networks[i].ssid;
      return;
    }
    WiFi.disconnect(false);
    delay(200);
  }
}


//====================================================
// REFRESH / ANZEIGE-STEUERUNG
//====================================================

// Holt frische Daten von Monday und zeigt das Auto-Event an.
// render=true: zeigt nach dem Laden das Auto-Event aufs Display (nur beim Start).
// render=false: laedt nur die Liste neu, Display bleibt unveraendert (manuell waehlen).
void refreshFromMonday(bool render) {
  bool ok = fetchEvents();
  if (render) {
    if (ok) {
      shownIndex = autoIndex;
      renderEvent(events[autoIndex]);
    } else {
      shownIndex = -1;
      renderMessage("MOKKA", lastError.length() ? lastError : "Kein Programm");
    }
  }
  lastRefresh = millis();
}

// Zeigt ein bestimmtes Event aus der Liste an (Web-Auswahl).
void showEventByIndex(int i) {
  if (i < 0 || i >= eventCount) return;
  shownIndex = i;
  renderEvent(events[i]);
  lastRefresh = millis();          // Auto-Timer zuruecksetzen
}


//====================================================
// WEB UI  (erreichbar ueber http://mokka-display.local)
//====================================================

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

// Kurzes Preis-Label ("30.–" / "EINTRITT FREI")
String priceLabel(const Event &ev) {
  bool num;
  return formatPrice(ev, num);
}

// CSS als String (wird gecacht ueber /ui.css ausgeliefert)
String uiCss() {
  String c = "";
  c += "body{background:#000;color:#fff;font-family:Arial,sans-serif;text-align:center;padding:16px;margin:0;}";
  c += ".logo{width:86px;max-width:28vw;margin:10px auto 8px;display:block;}";
  c += "h1{font-size:26px;margin:6px 0 8px;letter-spacing:1px;}";
  c += ".panel{border:1px solid #555;border-radius:14px;padding:12px;margin:12px 0;color:#aaa;font-size:13px;line-height:1.45;}";
  c += ".grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;margin-top:12px;}";
  c += ".tile{aspect-ratio:16/9;width:100%;box-sizing:border-box;overflow:hidden;border-radius:14px;border:2px solid #fff;display:flex;align-items:center;justify-content:center;text-decoration:none;text-align:center;font-weight:800;font-size:clamp(14px,5vw,34px);line-height:0.95;white-space:normal;word-break:break-word;hyphens:auto;padding:6px;cursor:pointer;}";
  c += ".tile span{max-width:100%;display:block;}";
  c += ".inactive{background:#000;color:#fff;}";
  c += ".active{background:#fff;color:#000;}";
  c += ".pics .tile{padding:0;overflow:hidden;border:none;border-radius:0;background:#000;}";
  c += ".thumb{width:100%;height:100%;object-fit:contain;background:#fff;display:block;}";
  c += ".pics .tile.inactive .thumb{filter:invert(1);}";
  c += ".link{color:#aaa;font-size:14px;text-decoration:underline;}";
  c += ".textBox{width:92%;height:120px;font-size:22px;border-radius:12px;padding:14px;box-sizing:border-box;}";
  c += ".btn{display:block;width:100%;box-sizing:border-box;background:#fff;color:#000;border:none;border-radius:14px;padding:16px;font-size:20px;font-weight:800;margin:10px 0;text-decoration:none;cursor:pointer;}";
  c += ".nav{display:block;border:2px solid #fff;border-radius:14px;padding:20px;margin:12px 0;font-size:21px;font-weight:800;text-decoration:none;color:#fff;}";
  c += ".back{display:inline-block;margin:4px 0 10px;color:#aaa;text-decoration:none;font-size:15px;}";
  c += ".cols{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:8px;}";
  c += ".col h3{font-size:15px;letter-spacing:1px;margin:6px 0;}";
  c += ".evc{display:block;border:1px solid #555;border-radius:10px;padding:8px;margin:8px 0;text-decoration:none;color:#fff;text-align:left;}";
  c += ".evc.active{background:#fff;color:#000;}";
  c += ".evc .nm{display:block;font-weight:800;font-size:14px;line-height:1.1;}";
  c += ".evc .dt{display:block;font-size:11px;color:#aaa;margin-top:3px;}";
  c += ".evc.active .dt{color:#333;}";
  c += ".evc .pr{display:block;font-weight:800;font-size:15px;margin-top:3px;}";
  c += ".msg{font-size:12px;color:#777;margin-top:8px;}";
  c += ".wordmark{width:70%;max-width:420px;margin:26px auto 8px;display:block;}";
  c += ".support{font-size:12px;color:#aaa;margin-bottom:20px;}";
  // Sofort-Feedback beim Klick
  c += ".nav:active,.btn:active,.tile:active,.evc:active,.link:active{opacity:.55;}";
  c += "#load{position:fixed;inset:0;background:rgba(0,0,0,.78);color:#fff;display:none;align-items:center;justify-content:center;font-size:26px;font-weight:800;letter-spacing:3px;z-index:9999;}";
  return c;
}

// JS als String (gecacht ueber /ui.js)
String uiJs() {
  String j = "";
  j += "function showFile(el,file){";
  j += "document.querySelectorAll('.tile').forEach(function(a){a.classList.remove('active');a.classList.add('inactive');});";
  j += "el.classList.remove('inactive');el.classList.add('active');";
  j += "var m=document.getElementById('msg');if(m)m.innerText='Lade: '+file;";
  j += "fetch('/showjpg?file='+encodeURIComponent(file)).then(function(r){return r.text();}).then(function(t){if(m)m.innerText=t;}).catch(function(e){if(m)m.innerText='FEHLER: '+e;});";
  j += "return false;}";
  j += "function sendText(){";
  j += "var t=document.getElementById('txt').value;var m=document.getElementById('msg');";
  j += "fetch('/textshow?t='+encodeURIComponent(t)).then(function(r){return r.text();}).then(function(x){if(m)m.innerText=x;});";
  j += "return false;}";
  j += "function quick(w){var e=document.getElementById('txt');if(e){e.value=w;}sendText();}";
  // Beim Navigieren (Links) sofort das LÄDT-Overlay zeigen -> Klick wirkt sofort
  j += "document.addEventListener('click',function(e){var a=e.target.closest('a');if(a){var hr=a.getAttribute('href');if(hr&&hr.charAt(0)!='#'){var l=document.getElementById('load');if(l)l.style.display='flex';}}});";
  // Overlay ausblenden, wenn die Seite (auch aus dem Cache) erscheint
  j += "window.addEventListener('pageshow',function(){var l=document.getElementById('load');if(l)l.style.display='none';});";
  return j;
}

void handleCss() {
  server.sendHeader("Cache-Control", "public, max-age=604800");
  server.send(200, "text/css", uiCss());
}
void handleJs() {
  server.sendHeader("Cache-Control", "public, max-age=604800");
  server.send(200, "application/javascript", uiJs());
}

// Gemeinsamer Seitenkopf -- CSS/JS nur verlinkt (gecacht) -> Seite winzig & schnell
String htmlHead(String title) {
  String h = "";
  h += "<!doctype html><html><head><meta charset='UTF-8'>";
  h += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  h += "<title>" + title + "</title>";
  // Favicon + iOS-Webapp ("Zum Startbildschirm hinzufuegen")
  h += "<link rel='icon' href='/logo.png'>";
  h += "<link rel='apple-touch-icon' href='/logo.png'>";
  h += "<meta name='apple-mobile-web-app-capable' content='yes'>";
  h += "<meta name='mobile-web-app-capable' content='yes'>";
  h += "<meta name='apple-mobile-web-app-title' content='MOKKA'>";
  h += "<meta name='apple-mobile-web-app-status-bar-style' content='black-translucent'>";
  h += "<meta name='theme-color' content='#000000'>";
  h += "<link rel='stylesheet' href='/ui.css'>";
  h += "<script src='/ui.js' defer></script>";
  h += "</head><body>";
  h += "<div id='load'>L&Auml;DT&hellip;</div>";   // Sofort-Feedback-Overlay
  h += "<img class='logo' src='/logo.png'>";
  h += "<h1>" + title + "</h1>";
  return h;
}

String htmlFoot() {
  String h = "";
  h += "<img class='wordmark' src='/wordmark.png'>";   // gecachter Endpunkt
  h += "<div class='support'>mokka-display-kasse.local · msanliker@mokka.ch</div>";
  h += "</body></html>";
  return h;
}

// Was laeuft gerade auf dem Display?
String displayStatus() {
  if (currentFile == "TEXT") return "Text";
  if (currentFile.length() > 0) return currentFile;
  if (shownIndex >= 0 && shownIndex < eventCount) return events[shownIndex].name;
  return lastError.length() ? lastError : "--";
}


// Buttons zu den jeweils ANDEREN zwei Funktionen (auf jeder Seite, nach den Infos)
String navButtons(const char* cur) {
  String h = "";
  if (strcmp(cur, "preise")) h += "<a class='nav' href='/'>PREISE</a>";
  if (strcmp(cur, "monday")) h += "<a class='nav' href='/monday'>PROGRAMM / DANCE</a>";
  if (strcmp(cur, "text"))   h += "<a class='nav' href='/text'>TEXT</a>";
  return h;
}

//--------------------- MONDAY: KONZERT + DANCE (/monday) ---------------------
// Eine Event-Spalte fuer eine Quelle rendern (Karten, antippbar)
String eventColumn(int src, const char* title) {
  String h = "<div class='col'><h3>" + String(title) + "</h3>";
  int shown = 0;
  for (int i = 0; i < eventCount; i++) {
    if (events[i].source != src) continue;
    shown++;
    String cls = (i == shownIndex) ? "evc active" : "evc";
    String d = events[i].dateRaw;                 // kompaktes Datum TT.MM.
    String dshort = (d.length() >= 10) ? (d.substring(8, 10) + "." + d.substring(5, 7) + ".") : d;
    String t = events[i].time; t.trim();
    h += "<a class='" + cls + "' href='/showmonday?i=" + String(i) + "'>";
    h += "<span class='nm'>" + htmlEscape(events[i].name) + "</span>";
    h += "<span class='dt'>" + dshort;
    if (t.length()) h += " · " + htmlEscape(t);
    if (events[i].isToday) h += " · HEUTE";
    h += "</span>";
    h += "<span class='pr'>" + htmlEscape(priceLabel(events[i])) + "</span>";
    h += "</a>";
  }
  if (shown == 0) h += "<div class='dt' style='padding:8px;'>--</div>";
  h += "</div>";
  return h;
}

void handleMonday() {
  String h = htmlHead("PROGRAMM & DANCE");

  h += "<div class='panel'>";
  h += "Auf dem Display: <b>" + htmlEscape(displayStatus()) + "</b>";
  h += "<br><a class='link' href='/refresh'>aktualisieren</a>";
  h += "</div>";

  h += navButtons("monday");

  if (eventCount == 0) {
    h += "<div class='panel'>Keine Events geladen.<br>" + htmlEscape(lastError) + "</div>";
  } else {
    h += "<div class='cols'>";
    h += eventColumn(0, "KONZERT");
    h += eventColumn(1, "DANCE");
    h += "</div>";
  }
  h += htmlFoot();
  server.send(200, "text/html", h);
}

//--------------------- TEXT (/text) ---------------------
void handleText() {
  String h = htmlHead("TEXT");

  h += "<div class='panel'>";
  h += "WLAN: " + htmlEscape(connectedSSID) + "<br>";
  h += "Auf dem Display: <b>" + htmlEscape(displayStatus()) + "</b>";
  h += "</div>";

  h += navButtons("text");

  h += "<div class='panel' style='padding:18px;'>";
  h += "<form onsubmit='return sendText();'>";
  h += "<textarea id='txt' class='textBox'>GESCHLOSSEN</textarea><br>";
  h += "<button class='btn' type='submit'>TEXT ANZEIGEN</button>";
  h += "<div style='margin-top:12px;font-size:14px;opacity:0.8;line-height:1.5;'>";
  h += "SCHREIBREGEL: GROSSBUCHSTABEN = maximale Grösse | Kleinbuchstaben = automatische Grösse";
  h += "</div></form>";
  h += "<div id='msg' class='msg'>" + htmlEscape(lastMessage) + "</div>";
  h += "</div>";

  // SCHNELLTEXTE (BigWords) -- ein Tipp zeigt das Wort gross aufs Display.
  // Liste einfach hier erweitern:
  const char* bigwords[] = {"GESCHLOSSEN", "KOLLEKTE", "EINTRITT FREI", "DANKE", "WILLKOMMEN", "RESERVIERT"};
  const int bigwordCount = sizeof(bigwords) / sizeof(bigwords[0]);

  h += "<div class='panel'><b>SCHNELLTEXTE</b></div>";
  h += "<div class='grid'>";
  for (int i = 0; i < bigwordCount; i++) {
    h += "<button type='button' class='tile inactive' onclick=\"quick('";
    h += bigwords[i];
    h += "')\"><span>";
    h += bigwords[i];
    h += "</span></button>";
  }
  h += "</div>";

  h += htmlFoot();
  server.send(200, "text/html", h);
}

//--------------------- PREISE / SD (/preise) ---------------------
void handlePreise() {
  String h = htmlHead("PREISE");

  h += "<div class='panel'>";
  h += "WLAN: " + htmlEscape(connectedSSID) + "<br>";
  h += "Auf dem Display: <b>" + htmlEscape(displayStatus()) + "</b><br>";
  h += "SD: " + htmlEscape(sdMessage);
  if (currentFile.length() > 0 && currentFile != "TEXT") h += "<br>Aktuell: " + htmlEscape(currentFile);
  h += "<br><a class='link' href='/rescan'>SD neu einlesen</a>";
  h += "<div id='msg' class='msg'>" + htmlEscape(lastMessage) + "</div>";
  h += "</div>";

  h += navButtons("preise");

  // FAVORITEN
  const char* favorites[] = {
    "30.jpg","25.jpg","12.jpg","10.jpg","8.jpg","5.jpg",
    "merci.jpg","mokkalogo.jpg","AUSVERKAUFT.jpg","gratis.jpg"
  };
  const int favoriteCount = sizeof(favorites) / sizeof(favorites[0]);

  h += "<div class='panel'><b>FAVORITEN</b></div>";
  h += "<div class='grid pics'>";
  for (int f = 0; f < favoriteCount; f++) {
    for (int i = 0; i < fileCount; i++) {
      if (files[i].equalsIgnoreCase(favorites[f])) {
        String cls = (files[i] == currentFile) ? "tile active" : "tile inactive";
        h += "<button type='button' class='" + cls + "' onclick=\"return showFile(this,'" + files[i] + "');\">";
        h += "<img class='thumb' loading='lazy' src='/sdimg?file=" + urlEncode(files[i]) + "' alt='" + htmlEscape(labelFromName(files[i])) + "'>";
        h += "</button>";
        break;
      }
    }
  }
  h += "</div>";

  // ALLE PREISE
  h += "<div class='panel'><b>ALLE PREISE</b></div>";
  h += "<div class='grid pics'>";
  if (sdOK && fileCount > 0) {
    for (int i = 0; i < fileCount; i++) {
      String name = files[i];
      String cls = (name == currentFile) ? "tile active" : "tile inactive";
      h += "<button type='button' class='" + cls + "' onclick=\"return showFile(this,'" + name + "');\">";
      h += "<img class='thumb' loading='lazy' src='/sdimg?file=" + urlEncode(name) + "' alt='" + htmlEscape(labelFromName(name)) + "'>";
      h += "</button>";
    }
  } else {
    h += "<div class='panel'>Keine JPG-Bilder gefunden oder SD nicht bereit.</div>";
  }
  h += "</div>";

  h += htmlFoot();
  server.send(200, "text/html", h);
}


//--------------------- BILDER (gecacht) ---------------------
void handleLogo() {
  server.sendHeader("Cache-Control", "public, max-age=604800, immutable");
  if (logoPng && logoPngLen) server.send_P(200, "image/png", (PGM_P)logoPng, logoPngLen);
  else server.send(404, "text/plain", "no logo");
}

void handleWordmark() {
  server.sendHeader("Cache-Control", "public, max-age=604800, immutable");
  if (wordmarkPng && wordmarkPngLen) server.send_P(200, "image/png", (PGM_P)wordmarkPng, wordmarkPngLen);
  else server.send(404, "text/plain", "no wordmark");
}

// Streamt ein JPG von der SD-Karte (echte Vorschau auf der PREISE-Seite)
void handleSdImg() {
  String file = server.arg("file");
  if (!file.startsWith("/")) file = "/" + file;
  if (!sdOK) { server.send(503, "text/plain", "SD nicht bereit"); return; }
  File f = SD.open(file);
  if (!f || f.isDirectory()) { if (f) f.close(); server.send(404, "text/plain", "nicht gefunden"); return; }
  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.streamFile(f, "image/jpeg");
  f.close();
}

//--------------------- AKTIONEN ---------------------
void handleRefresh() {
  refreshFromMonday(false);   // nur Liste neu laden, Display NICHT umschalten
  server.sendHeader("Location", "/monday");
  server.send(303);
}

void handleShowMonday() {
  if (server.hasArg("i")) showEventByIndex(server.arg("i").toInt());
  server.sendHeader("Location", "/monday");
  server.send(303);
}

void handleTextShow() {
  String t = server.arg("t");
  if (t.length() == 0) t = "WILLKOMMEN";
  showAmsiText(t);
  lastRefresh = millis();        // Auto-Refresh zuruecksetzen
  shownIndex = -1;
  server.send(200, "text/plain", "TEXT OK");
}

void handleShowJpg() {
  String file = server.arg("file");
  currentFile = file;
  shownIndex = -1;
  lastRefresh = millis();        // Auto-Refresh zuruecksetzen
  bool ok = showJpg(file);
  server.send(ok ? 200 : 500, "text/plain", lastMessage);
}

void handleRescan() {
  sdOK = initSD();
  if (sdOK) scanSD(); else fileCount = 0;
  server.sendHeader("Location", "/preise");
  server.send(303);
}


//====================================================
// SETUP / LOOP
//====================================================

void setup() {
  delay(800);

  framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
  if (!framebuffer) { while (1); }

  epd_init();

  // Web-Bilder einmalig dekodieren (PSRAM) -> spaeter gecacht ausliefern
  {
    size_t lb = strlen_P(LOGO_B64);
    logoPng = (uint8_t *)ps_malloc(lb * 3 / 4 + 4);
    if (logoPng) logoPngLen = b64decode(LOGO_B64, logoPng);
    size_t wb = strlen_P(WORDMARK_B64);
    wordmarkPng = (uint8_t *)ps_malloc(wb * 3 / 4 + 4);
    if (wordmarkPng) wordmarkPngLen = b64decode(WORDMARK_B64, wordmarkPng);
  }

  startWifi();

  // Zeit per NTP (fuer "heute"-Erkennung)
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov", "ch.pool.ntp.org");

  // SD-Karte einlesen (fuer Bilder-Seite)
  sdOK = initSD();
  if (sdOK) scanSD();

  // mDNS -> http://mokka-display.local
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
  }

  // Webserver-Routen
  server.on("/", handlePreise);          // Startseite = SD-PREISE
  server.on("/ui.css", handleCss);           // gecachtes CSS
  server.on("/ui.js", handleJs);             // gecachtes JS
  server.on("/logo.png", handleLogo);        // gecachtes Logo
  server.on("/wordmark.png", handleWordmark);// gecachter Schriftzug
  server.on("/sdimg", handleSdImg);          // SD-JPG-Vorschau
  server.on("/monday", handleMonday);    // Monday-Event-Liste
  server.on("/text", handleText);        // Text-Editor
  server.on("/preise", handlePreise);    // SD-Preisbilder
  server.on("/refresh", handleRefresh);     // Monday neu laden
  server.on("/showmonday", handleShowMonday); // Event -> Display
  server.on("/textshow", handleTextShow);   // Text -> Display
  server.on("/showjpg", handleShowJpg);     // JPG -> Display
  server.on("/rescan", handleRescan);       // SD neu einlesen
  server.begin();

  // Erste Anzeige beim Start: naechstes Event aufs Display
  refreshFromMonday(true);
}

void loop() {
  server.handleClient();
  // Kein automatischer Refresh mehr (Strom sparen / Akku-Betrieb).
  // Aktualisierung erfolgt manuell ueber die Weboberflaeche ("aktualisieren").
}
