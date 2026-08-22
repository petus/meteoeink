/*
 * MeteoEink426 v3 — LaskaKit ESPink-4.26 (ESP32-S3, ePaper GDEQ0426T82 800x480)
 * ============================================================================
 * Offline meteostanice s autodetekci vice cidel a volitelnymi kanaly grafu.
 *
 * SHT40 je vzdy zaklad (teplota + vlhkost, na desce). Soucasne se detekuji:
 *   - SHT4x   : teplota + vlhkost         (I2C 0x44 / 0x45 / 0x46)
 *   - SCD41   : CO2                       (I2C 0x62)
 *   - BME280  : tlak (+ vlhkost zalozne)  (I2C 0x76/0x77, chip ID 0x60)
 *   - BMP280  : tlak                      (I2C 0x76/0x77, chip ID 0x56-0x58)
 *   - DS18B20 : druha teplota             (OneWire GPIO4)
 *
 * Z dostupnych velicin se kresli az 3 grafy. Vyber je automaticky podle
 * priority, nebo rucne prikazem  ch=co2,temp,press  v servisnim rezimu.
 *
 * ADAPTIVNI HISTORIE: pamet je spolecny pool. Cim min kanalu, tim delsi
 * historie. Delka take zavisi na nastavenem intervalu mereni.
 *   3 kanaly / 5 min  = 72 h      1 kanal / 5 min  = 7 dni
 *   3 kanaly / 1 min  = 14 h      3 kanaly / 30min = 18 dni
 *
 * OCHRANA HISTORIE: v NVS je podpis sestavy (cidla + kanaly + interval).
 * Pri neshode se historie NEMAZE hned - odlisna sestava se musi potvrdit
 * dvema po sobe jdoucimi starty, aby vypadek cidla nesmazal data.
 *
 * TLACITKA (drzet pri restartu):
 *   PUSH (GPIO40) pustit mezi 2-5 s = servisni rezim pres USB (115200)
 *   PUSH (GPIO40) drzet pres 5 s    = WiFi hotspot s konfiguracni strankou
 *                                     (SSID, heslo a QR kod se ukazou na displeji,
 *                                      vypne se po 5 min necinnosti)
 *   DOWN (GPIO41) drzet 5 s         = smazani cele historie mereni
 *
 * Knihovny: GxEPD2, Adafruit GFX, Adafruit BME280, Adafruit BMP280,
 *           SparkFun SCD4x, OneWire, DallasTemperature,
 *           QRCode (Richard Moore) - QR kod pro pripojeni k hotspotu
 * SHT4x ma vlastni ovladac primo v tomto souboru (kvuli adresam 0x44/45/46).
 * Board: "ESP32S3 Dev Module", Flash 16MB, PSRAM: Disabled (neni potreba).
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>

#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <SparkFun_SCD4x_Arduino_Library.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <qrcode.h>

#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

// ============================================================================
// PINY — LaskaKit ESPink-4.26
// ============================================================================
#define PIN_POWER    47
#define PIN_I2C_SDA  42
#define PIN_I2C_SCL  2

#define EPD_CS   10
#define EPD_DC   48
#define EPD_RST  45
#define EPD_BUSY 38
#define EPD_MOSI 11
#define EPD_CLK  12
#define EPD_MISO 21

#define PIN_ONEWIRE 4
#define BTN_PUSH    40     // drzet 2 s po restartu = servisni rezim
#define BTN_DOWN    41     // drzet 5 s po restartu = smazani historie
#define PIN_VBAT    9
#define VBAT_RATIO  1.769388f

// ============================================================================
// LIMITY A VYCHOZI HODNOTY
// ============================================================================
#define MAX_CHANNELS      3
#define POOL_SLOTS        2592     // 5184 B v RTC RAM (~74 % bezpec. rozpoctu)
#define HISTORY_CAP       2016     // strop vzorku na kanal (7 dni pri 5 min)

#define INTERVAL_MIN_LO   1        // dolni mez intervalu [min]
#define INTERVAL_MIN_HI   60       // horni mez intervalu [min]
#define DEF_INTERVAL      5        // vychozi interval [min]
#define INTERVAL_WARN     5        // pod touto hodnotou varuj na spotrebu

#define NVS_SAVE_EVERY    6        // zapis do NVS kazde N. mereni (opotrebeni)
#define VBAT_LOW          3.50f    // varovani na displeji
#define VBAT_NO_WRITE     3.30f    // pod tim uz nezapisovat do flash

#define SERVICE_HOLD_MS   2000     // PUSH 2-5 s: servis pres USB
#define AP_HOLD_MS        5000     // PUSH >= 5 s: WiFi hotspot s konfiguraci
#define CLEAR_HOLD_MS     5000     // DOWN 5 s: smazani historie
#define AP_TIMEOUT_MS     300000UL // hotspot se vypne po 5 min neaktivity
#define SERVICE_TIMEOUT_MS 60000
#define SCD_TIMEOUT_MS    7000
#define uS_TO_S           1000000ULL

// ============================================================================
// DISPLEJ
// ============================================================================
GxEPD2_BW<GxEPD2_426_GDEQ0426T82, GxEPD2_426_GDEQ0426T82::HEIGHT>
    display(GxEPD2_426_GDEQ0426T82(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

const int W = 480;
const int H = 800;

// ============================================================================
// VELICINY A CIDLA
// ============================================================================
enum Quantity : uint8_t {
  Q_NONE = 0, Q_TEMP, Q_HUM, Q_CO2, Q_PRESS, Q_TEMP2, Q_VBAT
};

enum BoschType : uint8_t { BOSCH_NONE = 0, BOSCH_BME280, BOSCH_BMP280 };

struct Detected {
  bool      scd41   = false;
  bool      ds18b20 = false;
  BoschType bosch   = BOSCH_NONE;
  uint8_t   boschAddr = 0x76;
  uint8_t   boschChip = 0x58;
};
Detected det;
uint8_t  shtAddr = 0;               // 0x44 / 0x45 / 0x46, 0 = nenalezeno
uint8_t  sht4xDetect();             // definovano nize v sekci MERENI

struct Channel { Quantity q; bool dashed; };
Channel  channels[MAX_CHANNELS];
uint8_t  channelCount = 0;
uint16_t histPerCh = 0;            // vzorku na kanal (dopocitano)

Adafruit_BME280   bme;
Adafruit_BMP280   bmp(&Wire);
SCD4x             scd4x(SCD4x_SENSOR_SCD41);   // single-shot umi jen SCD41
OneWire           oneWire(PIN_ONEWIRE);
DallasTemperature ds18b20(&oneWire);
Preferences       prefs;

struct Reading {
  float temp = NAN, hum = NAN, co2 = NAN, press = NAN;
  float temp2 = NAN;                // VYHRADNE z DS18B20
  float tBosch = NAN;               // teplota cipu BME/BMP280 - jen jako zaloha
  float vbat = NAN;                 // napeti baterie [V] - take grafovatelne
};

// ============================================================================
// KONFIGURACE (NVS)
// ============================================================================
#define CFG_MAGIC 0x4D454B34u      // "MEK4" - zmena = stara konfig. se ignoruje

struct Config {
  uint32_t magic;
  float    tempOff;
  float    humOff;
  float    pressOff;
  float    altitude;
  uint8_t  scdAsc;
  uint8_t  intervalMin;
  uint8_t  chAuto;                 // 1 = automaticky vyber kanalu
  uint8_t  chSel[MAX_CHANNELS];    // rucni vyber (Quantity)
  char     apPass[13];             // heslo WiFi hotspotu (8-12 znaku)
};
Config  cfg;
int16_t pendingCo2Ref = -1;        // jednorazovy pozadavek, neuklada se

void cfgDefaults() {
  cfg.magic       = CFG_MAGIC;
  cfg.tempOff     = 0.0f;
  cfg.humOff      = 0.0f;
  cfg.pressOff    = 0.0f;
  cfg.altitude    = 0.0f;
  cfg.scdAsc      = 1;
  cfg.intervalMin = DEF_INTERVAL;
  cfg.chAuto      = 1;
  for (uint8_t i = 0; i < MAX_CHANNELS; i++) cfg.chSel[i] = Q_NONE;
  cfg.apPass[0]   = 0;             // prazdne = vygeneruje se pri prvnim pouziti
}

// Osetri nesmyslne hodnoty (ochrana proti spatnemu nastaveni).
void cfgSanitize() {
  if (cfg.intervalMin < INTERVAL_MIN_LO || cfg.intervalMin > INTERVAL_MIN_HI)
    cfg.intervalMin = DEF_INTERVAL;
  if (!isfinite(cfg.tempOff)  || fabsf(cfg.tempOff)  > 20.0f)   cfg.tempOff  = 0;
  if (!isfinite(cfg.humOff)   || fabsf(cfg.humOff)   > 30.0f)   cfg.humOff   = 0;
  if (!isfinite(cfg.pressOff) || fabsf(cfg.pressOff) > 50.0f)   cfg.pressOff = 0;
  if (!isfinite(cfg.altitude) || cfg.altitude < 0 || cfg.altitude > 4000)
    cfg.altitude = 0;
  cfg.scdAsc = cfg.scdAsc ? 1 : 0;
  cfg.chAuto = cfg.chAuto ? 1 : 0;
  for (uint8_t i = 0; i < MAX_CHANNELS; i++)
    if (cfg.chSel[i] > Q_VBAT) cfg.chSel[i] = Q_NONE;
  cfg.apPass[12] = 0;                               // vzdy ukoncene
  size_t pl = strlen(cfg.apPass);
  if (pl > 0 && pl < 8) cfg.apPass[0] = 0;          // kratsi nez WPA2 minimum
}

#define NVS_NS      "meteo"
#define NVS_CFG     "cfg"
#define NVS_HIST    "hist"
#define NVS_SIGPEND "sigp"         // kandidat na novou sestavu
#define NVS_SIGCNT  "sigc"         // kolikrat uz se potvrdil

void cfgLoad() {
  cfgDefaults();
  if (prefs.begin(NVS_NS, true)) {
    if (prefs.isKey(NVS_CFG)) {
      Config tmp;
      if (prefs.getBytes(NVS_CFG, &tmp, sizeof(tmp)) == sizeof(tmp) &&
          tmp.magic == CFG_MAGIC) {
        cfg = tmp;
      }
    }
    prefs.end();
  }
  cfgSanitize();
}

// Ulozi konfiguraci a zpetnym ctenim overi, ze se opravdu zapsala.
// Vraci false, kdyz se zapis nepovedl - volajici to muze ohlasit uzivateli.
bool cfgSave() {
  cfgSanitize();
  bool ok = false;
  if (prefs.begin(NVS_NS, false)) {
    size_t w = prefs.putBytes(NVS_CFG, &cfg, sizeof(cfg));
    prefs.end();
    ok = (w == sizeof(cfg));
  }
  if (ok) {
    // kontrolni zpetne cteni - at nehlasime uspech pri tiche chybe flash
    Config back;
    if (prefs.begin(NVS_NS, true)) {
      ok = (prefs.getBytes(NVS_CFG, &back, sizeof(back)) == sizeof(back)) &&
           (memcmp(&back, &cfg, sizeof(cfg)) == 0);
      prefs.end();
    } else ok = false;
  }
  if (!ok) Serial.println("CHYBA: konfiguraci se nepodarilo ulozit do NVS!");
  return ok;
}

// ============================================================================
// HISTORIE — spolecny pool v RTC RAM
// Rozlozeni: kanal c zabira histPool[c*histPerCh .. c*histPerCh+histPerCh-1]
// Hodnoty int16: teplota/vlhkost x100, tlak (hPa-900)x10, CO2 primo v ppm.
// ============================================================================
#define STORE_INVALID INT16_MIN

RTC_DATA_ATTR int16_t  histPool[POOL_SLOTS];
RTC_DATA_ATTR uint16_t histCount = 0;
RTC_DATA_ATTR uint16_t histHead  = 0;
RTC_DATA_ATTR uint16_t histSlots = 0;      // histPerCh platne pro data v poolu
RTC_DATA_ATTR uint32_t rtcSignature = 0;
RTC_DATA_ATTR uint16_t saveTick = 0;
RTC_DATA_ATTR bool     rtcInited = false;

static inline int16_t valToStore(Quantity q, float v) {
  if (isnan(v) || isinf(v)) return STORE_INVALID;
  long s;
  switch (q) {
    case Q_CO2:
      s = lroundf(v);
      if (s < 0) s = 0;
      if (s > 30000) s = 30000;
      break;
    case Q_PRESS:
      s = lroundf((v - 900.0f) * 10.0f);
      if (s < -5000) s = -5000;
      if (s > 20000) s = 20000;
      break;
    case Q_VBAT:
      s = lroundf(v * 100.0f);          // volty v setinach (3.85 V = 385)
      if (s < 0) s = 0;
      if (s > 6000) s = 6000;
      break;
    default:
      if (v < -100.0f) v = -100.0f;
      if (v >  150.0f) v =  150.0f;
      s = lroundf(v * 100.0f);
      if (s < -30000) s = -30000;
      if (s >  30000) s =  30000;
      break;
  }
  return (int16_t)s;
}

static inline float storeToVal(Quantity q, int16_t s) {
  if (s == STORE_INVALID) return NAN;
  switch (q) {
    case Q_CO2:   return (float)s;
    case Q_PRESS: return s / 10.0f + 900.0f;
    default:      return s / 100.0f;
  }
}

// Vzorek i (0 = nejstarsi) kanalu ch.
static int16_t histAt(uint8_t ch, uint16_t i) {
  if (histPerCh == 0 || ch >= channelCount || i >= histCount) return STORE_INVALID;
  uint16_t start = (histCount < histPerCh) ? 0 : histHead;
  uint16_t idx   = (start + i) % histPerCh;
  uint32_t pos   = (uint32_t)ch * histPerCh + idx;
  if (pos >= POOL_SLOTS) return STORE_INVALID;      // pojistka
  return histPool[pos];
}

static void histClear() {
  for (uint32_t i = 0; i < POOL_SLOTS; i++) histPool[i] = STORE_INVALID;
  histCount = 0;
  histHead  = 0;
  histSlots = histPerCh;
}

static void histPush(const Reading &r) {
  if (histPerCh == 0) return;
  for (uint8_t c = 0; c < channelCount; c++) {
    float v;
    switch (channels[c].q) {
      case Q_TEMP:  v = r.temp;  break;
      case Q_HUM:   v = r.hum;   break;
      case Q_CO2:   v = r.co2;   break;
      case Q_PRESS: v = r.press; break;
      case Q_TEMP2: v = r.temp2; break;
      case Q_VBAT:  v = r.vbat;  break;
      default:      v = NAN;     break;
    }
    uint32_t pos = (uint32_t)c * histPerCh + histHead;
    if (pos < POOL_SLOTS) histPool[pos] = valToStore(channels[c].q, v);
  }
  histHead = (histHead + 1) % histPerCh;
  if (histCount < histPerCh) histCount++;
}

// Rozpeti historie v minutach.
static uint32_t histSpanMin() {
  return (histCount > 1) ? (uint32_t)(histCount - 1) * cfg.intervalMin : 0;
}

// ============================================================================
// NVS: historie
// ============================================================================
struct HistBlob {
  uint32_t signature;
  uint16_t count;
  uint16_t head;
  uint16_t perCh;
  uint16_t pad;
  int16_t  pool[POOL_SLOTS];
};

bool histLoadNVS(uint32_t wantSig) {
  if (!prefs.begin(NVS_NS, true)) return false;
  static HistBlob blob;
  size_t got = prefs.getBytes(NVS_HIST, &blob, sizeof(blob));
  prefs.end();
  if (got != sizeof(blob)) return false;
  if (blob.signature != wantSig) return false;
  if (blob.perCh != histPerCh) return false;
  if (blob.count > histPerCh || blob.head >= histPerCh) return false;
  histCount = blob.count;
  histHead  = blob.head;
  histSlots = blob.perCh;
  memcpy(histPool, blob.pool, sizeof(histPool));
  return true;
}

void histSaveNVS(uint32_t sig, float vbat) {
  // Pri podpeti se do flash nezapisuje - hrozi poskozeni dat.
  if (!isnan(vbat) && vbat < VBAT_NO_WRITE) return;
  static HistBlob blob;
  blob.signature = sig;
  blob.count = histCount;
  blob.head  = histHead;
  blob.perCh = histPerCh;
  blob.pad   = 0;
  memcpy(blob.pool, histPool, sizeof(histPool));
  if (prefs.begin(NVS_NS, false)) {
    prefs.putBytes(NVS_HIST, &blob, sizeof(blob));
    prefs.end();
  }
}

void histEraseNVS() {
  if (prefs.begin(NVS_NS, false)) {
    prefs.remove(NVS_HIST);
    prefs.remove(NVS_SIGPEND);
    prefs.remove(NVS_SIGCNT);
    prefs.end();
  }
}

// ============================================================================
// NAPAJENI
// ============================================================================
void powerOn() {
  pinMode(PIN_POWER, OUTPUT);
  digitalWrite(PIN_POWER, HIGH);
  delay(50);
}
void powerOff() { digitalWrite(PIN_POWER, LOW); }

// ============================================================================
// DETEKCE CIDEL
// ============================================================================
bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

uint8_t boschChipIdAt(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(0xD0);
  if (Wire.endTransmission() != 0) return 0;
  Wire.requestFrom(addr, (uint8_t)1);
  if (!Wire.available()) return 0;
  return Wire.read();
}

// Jeden pruchod detekci.
Detected detectOnce() {
  Detected d;
  if (i2cPresent(0x62)) d.scd41 = true;
  for (uint8_t a = 0x76; a <= 0x77; a++) {
    if (!i2cPresent(a)) continue;
    uint8_t id = boschChipIdAt(a);
    if (id == 0x60) { d.bosch = BOSCH_BME280; d.boschAddr = a; d.boschChip = id; break; }
    if (id == 0x56 || id == 0x57 || id == 0x58) {
      d.bosch = BOSCH_BMP280; d.boschAddr = a; d.boschChip = id; break;
    }
  }
  ds18b20.begin();
  if (ds18b20.getDeviceCount() > 0) d.ds18b20 = true;
  return d;
}

// Detekce s opakovanim - jeden vypadek I2C nesmi zmenit sestavu.
void detectSensors() {
  shtAddr = sht4xDetect();          // 0x44 / 0x45 / 0x46
  Detected a = detectOnce();
  delay(30);
  Detected b = detectOnce();
  // sjednoceni: co bylo videt aspon jednou, povazujeme za pritomne
  det.scd41   = a.scd41   || b.scd41;
  det.ds18b20 = a.ds18b20 || b.ds18b20;
  if (a.bosch != BOSCH_NONE)      { det.bosch = a.bosch; det.boschAddr = a.boschAddr; det.boschChip = a.boschChip; }
  else if (b.bosch != BOSCH_NONE) { det.bosch = b.bosch; det.boschAddr = b.boschAddr; det.boschChip = b.boschChip; }
  else                            { det.bosch = BOSCH_NONE; }
}

bool qAvailable(Quantity q) {
  switch (q) {
    case Q_TEMP:
    case Q_HUM:   return true;                    // SHT40 je vzdy
    case Q_CO2:   return det.scd41;
    case Q_PRESS: return det.bosch != BOSCH_NONE;
    case Q_TEMP2: return det.ds18b20;
    case Q_VBAT:  return true;                    // delic je vzdy na desce
    default:      return false;
  }
}

// Sestavi kanaly: bud rucni vyber, nebo automaticka priorita.
void buildChannels() {
  channelCount = 0;
  auto add = [&](Quantity q) {
    if (channelCount >= MAX_CHANNELS || q == Q_NONE || !qAvailable(q)) return;
    for (uint8_t i = 0; i < channelCount; i++)
      if (channels[i].q == q) return;             // bez duplicit
    channels[channelCount].q = q;
    channels[channelCount].dashed = false;   // kazdy kanal ma vlastni graf
    channelCount++;
  };

  if (!cfg.chAuto) {
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) add((Quantity)cfg.chSel[i]);
  }
  if (channelCount == 0) {
    // Automaticka priorita: CO2 > teplota > tlak > vlhkost > druha teplota.
    // Externi teplomer je vzdy vedoma volba uzivatele, vlhkost je z cidla
    // na desce k dispozici sama - proto ma temp2 prednost pred hum.
    add(Q_CO2); add(Q_TEMP); add(Q_PRESS); add(Q_TEMP2); add(Q_HUM);
  }

  // Adaptivni delka historie podle poctu kanalu.
  uint16_t per = (channelCount > 0) ? (POOL_SLOTS / channelCount) : POOL_SLOTS;
  if (per > HISTORY_CAP) per = HISTORY_CAP;
  histPerCh = per;
}

// Prepocita kanaly a pokud se zmenilo rozlozeni historie nebo casovy krok,
// zalozi historii znovu. Bez toho by data v poolu zustala v puvodnim
// rozlozeni a export CSV i graf by cetly nesmysly.
void relayoutHistory(bool intervalChanged) {
  uint16_t before = histPerCh;
  buildChannels();
  if (histPerCh != before || histPerCh != histSlots || intervalChanged) {
    histClear();
    histEraseNVS();
    Serial.println("Rozlozeni historie se zmenilo - zalozena nova historie.");
  }
}

uint32_t buildSignature() {
  uint32_t s = 0x9E3779B1u;
  s = s * 31u + (uint32_t)shtAddr;
  s = s * 31u + (uint32_t)det.scd41;
  s = s * 31u + (uint32_t)det.ds18b20;
  s = s * 31u + (uint32_t)det.bosch;
  s = s * 31u + (uint32_t)channelCount;
  for (uint8_t c = 0; c < channelCount; c++) s = s * 31u + (uint32_t)channels[c].q;
  s = s * 31u + (uint32_t)cfg.intervalMin;
  s = s * 31u + (uint32_t)histPerCh;
  return s;
}

// Nazvy detekovanych cidel bez adres - pro vypis na displej pod sebe.
// Vraci pocet zapsanych polozek.
uint8_t sensorNames(const char *out[], uint8_t maxItems) {
  uint8_t n = 0;
  if (shtAddr    && n < maxItems) out[n++] = "SHT4x";
  if (det.scd41  && n < maxItems) out[n++] = "SCD41";
  if (det.bosch == BOSCH_BME280 && n < maxItems) out[n++] = "BME280";
  if (det.bosch == BOSCH_BMP280 && n < maxItems) out[n++] = "BMP280";
  if (det.ds18b20 && n < maxItems) out[n++] = "DS18B20";
  return n;
}

const char* boschName() {
  if (det.bosch == BOSCH_BME280) return "BME280";
  if (det.bosch == BOSCH_BMP280) return "BMP280";
  return "";
}

const char* sensorSummary() {
  static char buf[64];
  buf[0] = 0;
  if (shtAddr) { char t[16]; snprintf(t, sizeof(t), "SHT4x@0x%02X ", shtAddr); strcat(buf, t); }
  if (det.scd41) strcat(buf, "SCD41 ");
  if (det.bosch == BOSCH_BME280) strcat(buf, "BME280 ");
  if (det.bosch == BOSCH_BMP280) strcat(buf, "BMP280 ");
  if (det.ds18b20) strcat(buf, "DS18B20 ");
  if (!buf[0]) strcpy(buf, "zadne");
  return buf;
}

// ============================================================================
// NAZVY VELICIN
// ============================================================================
const char* qKey(Quantity q) {          // klic pro prikaz ch=
  switch (q) {
    case Q_TEMP:  return "temp";
    case Q_HUM:   return "hum";
    case Q_CO2:   return "co2";
    case Q_PRESS: return "press";
    case Q_TEMP2: return "temp2";
    case Q_VBAT:  return "vbat";
    default:      return "-";
  }
}
Quantity qFromKey(const String &s) {
  if (s == "temp")  return Q_TEMP;
  if (s == "hum")   return Q_HUM;
  if (s == "co2")   return Q_CO2;
  if (s == "press") return Q_PRESS;
  if (s == "temp2") return Q_TEMP2;
  if (s == "vbat")  return Q_VBAT;
  return Q_NONE;
}
const char* qLabel(Quantity q) {
  switch (q) {
    case Q_TEMP:  return "Teplota";
    case Q_HUM:   return "Vlhkost";
    case Q_CO2:   return "CO2";
    case Q_PRESS: return "Tlak";
    case Q_TEMP2: return "Teplota 2";
    case Q_VBAT:  return "Baterie";
    default:      return "";
  }
}
const char* qUnit(Quantity q) {
  switch (q) {
    case Q_TEMP:
    case Q_TEMP2: return "degC";
    case Q_VBAT:  return "V";
    case Q_HUM:   return "%";
    case Q_CO2:   return "ppm";
    case Q_PRESS: return "hPa";
    default:      return "";
  }
}
void qFormat(Quantity q, float v, char *o, size_t n) {
  if (isnan(v)) { snprintf(o, n, "--"); return; }
  if (q == Q_CO2) snprintf(o, n, "%d", (int)lroundf(v));
  else            snprintf(o, n, "%.1f", v);
}

// ============================================================================
// MERENI
// ============================================================================
// ---------------------------------------------------------------------------
// SHT4x - vlastni minimalni ovladac
// Adafruit_SHT4x umi jen adresu 0x44 (begin() adresu neprijima), ale
// SHT40 se vyrabi ve trech variantach (datasheet Sensirion SHT4x):
//   SHT40-AD1B = 0x44   SHT40-BD1B = 0x45   SHT40-CD1B = 0x46
// SHT41 a SHT45 pouzivaji 0x44. Protokol je trivialni, tak si ho udelame sami.
// ---------------------------------------------------------------------------
static const uint8_t SHT4X_ADDRS[] = { 0x44, 0x45, 0x46 };
#define SHT4X_CMD_HIGHPREC 0xFD     // mereni ve vysoke presnosti, ~8.2 ms

static uint8_t sht4xCrc(const uint8_t *d, uint8_t n) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

// Precte teplotu a vlhkost ze zadane adresy. Vraci false pri chybe nebo CRC.
bool sht4xRead(uint8_t addr, float &tC, float &rh) {
  Wire.beginTransmission(addr);
  Wire.write(SHT4X_CMD_HIGHPREC);
  if (Wire.endTransmission() != 0) return false;
  delay(12);                                   // datasheet: max 8.2 ms
  if (Wire.requestFrom(addr, (uint8_t)6) != 6) return false;
  uint8_t b[6];
  for (uint8_t i = 0; i < 6; i++) b[i] = Wire.read();
  if (sht4xCrc(&b[0], 2) != b[2]) return false;
  if (sht4xCrc(&b[3], 2) != b[5]) return false;
  uint16_t rawT  = ((uint16_t)b[0] << 8) | b[1];
  uint16_t rawRH = ((uint16_t)b[3] << 8) | b[4];
  tC = -45.0f + 175.0f * rawT  / 65535.0f;
  rh =  -6.0f + 125.0f * rawRH / 65535.0f;
  return true;
}

// Najde adresu, na ktere SHT4x skutecne odpovida (0 = nenalezeno).
uint8_t sht4xDetect() {
  for (uint8_t i = 0; i < sizeof(SHT4X_ADDRS); i++) {
    uint8_t a = SHT4X_ADDRS[i];
    if (!i2cPresent(a)) continue;
    float t, h;
    if (sht4xRead(a, t, h)) return a;         // odpovedelo i platnymi daty
  }
  return 0;
}

void readSHT40(Reading &r) {
  if (shtAddr == 0) return;
  float t, h;
  if (!sht4xRead(shtAddr, t, h)) return;
  r.temp = t + cfg.tempOff;
  r.hum  = constrain(h + cfg.humOff, 0.0f, 100.0f);
}

void readSCD41(Reading &r) {
  // begin(port, measBegin, autoCalibrate):
  //   measBegin=false  -> nespoustet periodicke mereni (setri energii),
  //                       pouzijeme jednorazovy single shot
  //   autoCalibrate    -> ASC podle konfigurace
  // begin() sam zastavi pripadne bezici periodicke mereni.
  if (!scd4x.begin(Wire, false, cfg.scdAsc != 0)) return;

  // Nadmorska vyska zpresnuje vypocet CO2 primo v cidle.
  if (cfg.altitude > 0) scd4x.setSensorAltitude((uint16_t)cfg.altitude);

  // Jednorazova kalibrace na znamou koncentraci, pokud byla vyzadana.
  if (pendingCo2Ref >= 0) {
    scd4x.stopPeriodicMeasurement();
    float corr = 0;
    if (scd4x.performForcedRecalibration((uint16_t)pendingCo2Ref, &corr))
      Serial.printf("FRC hotova, korekce %.1f ppm\n", corr);
    else
      Serial.println("FRC selhala - cidlo musi bezet 3+ min ve stalem prostredi.");
    pendingCo2Ref = -1;
  }

  if (!scd4x.measureSingleShot()) return;

  // Single shot trva ~5 s. Polujeme priznak, aby deska sla spat co nejdriv.
  bool ready = false;
  unsigned long t0 = millis();
  while (millis() - t0 < SCD_TIMEOUT_MS) {
    delay(100);
    if (scd4x.getDataReadyStatus()) { ready = true; break; }
  }
  if (!ready || !scd4x.readMeasurement()) return;

  uint16_t co2 = scd4x.getCO2();
  if (co2 == 0) return;
  r.co2 = co2;
  // SHT40 ma prednost (SCD41 se sam ohriva) - jen zaloha
  if (isnan(r.temp)) r.temp = scd4x.getTemperature() + cfg.tempOff;
  if (isnan(r.hum))
    r.hum = constrain(scd4x.getHumidity() + cfg.humOff, 0.0f, 100.0f);
}

static float adjustPressure(float hPa) {
  if (cfg.altitude != 0.0f)
    hPa = hPa / powf(1.0f - (cfg.altitude / 44330.0f), 5.255f);
  return hPa + cfg.pressOff;
}

void readBosch(Reading &r) {
  if (det.bosch == BOSCH_BME280) {
    if (!bme.begin(det.boschAddr, &Wire)) return;
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::FILTER_OFF);
    bme.takeForcedMeasurement();
    r.press = adjustPressure(bme.readPressure() / 100.0f);
    if (isnan(r.hum))
      r.hum = constrain(bme.readHumidity() + cfg.humOff, 0.0f, 100.0f);
    r.tBosch = bme.readTemperature();   // NE do temp2 - to patri DS18B20
  } else if (det.bosch == BOSCH_BMP280) {
    // POZOR: Adafruit_BME280 odmita chip ID 0x58, nutna Adafruit_BMP280.
    if (!bmp.begin(det.boschAddr, det.boschChip)) return;
    bmp.setSampling(Adafruit_BMP280::MODE_FORCED,
                    Adafruit_BMP280::SAMPLING_X1,
                    Adafruit_BMP280::SAMPLING_X1,
                    Adafruit_BMP280::FILTER_OFF,
                    Adafruit_BMP280::STANDBY_MS_1);
    bmp.takeForcedMeasurement();
    r.press = adjustPressure(bmp.readPressure() / 100.0f);
    r.tBosch = bmp.readTemperature();   // NE do temp2 - to patri DS18B20
  }
}

void readDS18B20(Reading &r) {
  ds18b20.begin();
  if (ds18b20.getDeviceCount() == 0) return;
  ds18b20.setResolution(12);
  ds18b20.requestTemperatures();
  float v = ds18b20.getTempCByIndex(0);
  if (v != DEVICE_DISCONNECTED_C && v > -100.0f) r.temp2 = v + cfg.tempOff;
}

float readVBat() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VBAT, ADC_11db);
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) { acc += analogReadMilliVolts(PIN_VBAT); delay(2); }
  return (acc / 16.0f * VBAT_RATIO) / 1000.0f;
}

int vbatPercent(float v) {
  if (isnan(v)) return -1;
  return (int)lroundf(constrain((v - 3.30f) / 0.90f * 100.0f, 0.0f, 100.0f));
}

Reading doMeasurement() {
  Reading r;
  r.vbat = readVBat();              // baterie je take grafovatelna velicina
  readSHT40(r);
  if (det.scd41)                 readSCD41(r);
  if (det.bosch != BOSCH_NONE)   readBosch(r);
  if (det.ds18b20)               readDS18B20(r);
  return r;
}

// ============================================================================
// KRESLENI
// ============================================================================
void drawDashedLine(int x0, int y0, int x1, int y1, int dash, int gap) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) { display.drawPixel(x0, y0, GxEPD_BLACK); return; }
  float ux = dx / len, uy = dy / len, pos = 0;
  bool draw = true;
  while (pos < len) {
    float end = pos + (draw ? dash : gap);
    if (end > len) end = len;
    if (draw)
      display.drawLine(x0 + (int)lroundf(ux*pos), y0 + (int)lroundf(uy*pos),
                       x0 + (int)lroundf(ux*end), y0 + (int)lroundf(uy*end),
                       GxEPD_BLACK);
    pos = end; draw = !draw;
  }
}

void drawBattery(float vb) {
  const int bw = 34, bh = 16;
  const bool low = (!isnan(vb) && vb < VBAT_LOW);
  // Text drzime kratky - vedle nej vlevo je popisek hlavicky a dlouhy
  // retezec by do nej mohl zasahnout. Varovani je ve spodnim radku hlavicky.
  char buf[24];
  int pct = vbatPercent(vb);
  if (isnan(vb)) snprintf(buf, sizeof(buf), "--.- V");
  else           snprintf(buf, sizeof(buf), "%.2f V  %d%%", vb, pct);

  display.setFont(&FreeSans9pt7b);
  int16_t x1, y1; uint16_t tw, th;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
  const int by = 8;                 // blize k horni hrane displeje
  const int tx = W - 20 - tw;
  const int bx = tx - 8 - (bw + 3);

  display.drawRect(bx, by, bw, bh, GxEPD_BLACK);
  display.fillRect(bx + bw, by + 4, 3, bh - 8, GxEPD_BLACK);
  if (pct >= 0) {
    int fw = (int)((bw - 4) * pct / 100.0f);
    if (fw > 0) display.fillRect(bx + 2, by + 2, fw, bh - 4, GxEPD_BLACK);
  }
  display.setCursor(tx, by + bh - 3);
  display.print(buf);

  // Pri nizkem napeti jeste zvyraznujici ramecek kolem cele indikace.
  if (low) display.drawRect(bx - 4, by - 5, (W - 16) - bx, bh + 10, GxEPD_BLACK);
}

// Text zarovnany na pravy okraj (pro pravy sloupec hlavicky).
void drawRight(const char *txt, int rightX, int baselineY) {
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(rightX - w, baselineY);
  display.print(txt);
}

// Radek namerene hodnoty ve trech sloupcich:
// vlevo nazev, uprostred hodnota, vpravo jednotka.
// Hodnota se drzi na spolecnem stredu, ale kdyby se dotkla nazvu nebo
// jednotky, odsune se - text se tak nikdy neprekryje.
void drawValueRow(const char *name, const char *value, const char *unit, int y) {
  const int L = 20, R = W - 20, GAP = 12, CENTER = 274;
  int16_t x1, y1; uint16_t nw, uw, vw, h;

  display.getTextBounds(name,  0, 0, &x1, &y1, &nw, &h);
  display.getTextBounds(unit,  0, 0, &x1, &y1, &uw, &h);
  display.getTextBounds(value, 0, 0, &x1, &y1, &vw, &h);

  display.setCursor(L, y);
  display.print(name);

  display.setCursor(R - uw, y);
  display.print(unit);

  int vx = CENTER - (int)vw / 2;
  int lo = L + (int)nw + GAP;             // nesmi zasahovat do nazvu
  int hi = R - (int)uw - GAP - (int)vw;   // ani do jednotky
  if (vx < lo) vx = lo;
  if (vx > hi) vx = hi;
  if (vx < lo) vx = lo;                   // kdyby bylo misto opravdu malo
  display.setCursor(vx, y);
  display.print(value);
}

void drawHeader(const Reading &r, float vb) {
  char buf[64], val[16];

  // ---- PRAVY SLOUPEC: baterie, pod ni cidla (kazde na svem radku) ----
  drawBattery(vb);
  display.setFont(&FreeSans9pt7b);
  const char *sn[5];
  uint8_t sCnt = sensorNames(sn, 5);
  int ry = 40;                        // prvni radek pod baterii
  for (uint8_t i = 0; i < sCnt; i++) { drawRight(sn[i], W - 20, ry); ry += 17; }
  snprintf(buf, sizeof(buf), "interval %d min", cfg.intervalMin);
  drawRight(buf, W - 20, ry); ry += 17;
  if (!isnan(vb) && vb < VBAT_LOW) drawRight("BATERIE SLABA", W - 20, ry);

  // ---- LEVY SLOUPEC: hlavni namerena hodnota ----
  display.setCursor(20, 26);
  display.print("Teplota vzduchu");

  qFormat(Q_TEMP, r.temp, val, sizeof(val));
  display.setFont(&FreeSansBold24pt7b);
  snprintf(buf, sizeof(buf), "%s degC", val);
  display.setCursor(20, 84);
  display.print(buf);

  qFormat(Q_HUM, r.hum, val, sizeof(val));
  display.setFont(&FreeSansBold12pt7b);
  snprintf(buf, sizeof(buf), "Vlhkost: %s %%", val);
  display.setCursor(20, 118);
  display.print(buf);

  display.drawFastHLine(20, 136, W - 40, GxEPD_BLACK);

  // ---- Dalsi namerene hodnoty, rozestup 40 px kvuli vzdusnosti ----
  display.setFont(&FreeSansBold18pt7b);
  int y = 172;
  bool any = false;
  if (!isnan(r.co2)) {
    qFormat(Q_CO2, r.co2, val, sizeof(val));
    drawValueRow("CO2", val, "ppm", y);
    y += 40; any = true;
  }
  if (!isnan(r.press)) {
    qFormat(Q_PRESS, r.press, val, sizeof(val));
    drawValueRow("Tlak", val, "hPa", y);
    y += 40; any = true;
  }
  if (!isnan(r.temp2)) {
    // Skutecne externi cidlo na kabelu.
    qFormat(Q_TEMP2, r.temp2, val, sizeof(val));
    drawValueRow("Teplota 2", val, "degC", y);
    y += 40; any = true;
  } else if (!isnan(r.tBosch)) {
    // DS18B20 chybi nebo neodpovedel. Ukazeme teplotu cipu tlakomeru,
    // ale pod jeho vlastnim nazvem - at je jasne, ze to neni cidlo na kabelu.
    qFormat(Q_TEMP2, r.tBosch, val, sizeof(val));
    drawValueRow(boschName(), val, "degC", y);
    y += 40; any = true;
  }
  if (!any) {
    display.setFont(&FreeSans12pt7b);
    display.setCursor(20, y); display.print("(pripojeno jen SHT4x)");
    y += 40;
  }

  display.drawFastHLine(20, y - 24, W - 40, GxEPD_BLACK);
}

// Vraci Y souradnici, kde hlavicka konci (aby grafy zacaly pod ni).
int headerBottom(const Reading &r) {
  int n = 0;
  if (!isnan(r.co2))   n++;
  if (!isnan(r.press)) n++;
  if (!isnan(r.temp2) || !isnan(r.tBosch)) n++;   // druha teplota nebo zaloha
  if (n == 0) n = 1;                 // radek "(pripojeno jen SHT4x)"
  return 172 + 40 * n - 24;          // musi souhlasit s drawHeader
}

void drawOneGraph(uint8_t ch, int gx, int gy, int gw, int gh) {
  const int gBottom = gy + gh, gRight = gx + gw;
  Quantity q = channels[ch].q;
  uint32_t spanMin = histSpanMin();

  // --- rozsah osy Y: kazda velicina ma jinou rozumnou rezervu ---
  float mn = 1e9f, mx = -1e9f; bool any = false;
  for (uint16_t i = 0; i < histCount; i++) {
    float v = storeToVal(q, histAt(ch, i));
    if (!isnan(v)) { any = true; if (v < mn) mn = v; if (v > mx) mx = v; }
  }
  const float dataMin = mn, dataMax = mx;   // syrove extremy pred rozsirenim osy
  if (!any) { mn = 0; mx = 1; }

  float minSpan;                     // nejmensi rozumny rozsah osy
  switch (q) {
    case Q_CO2:
      mn = floorf(mn / 50) * 50 - 25;  mx = ceilf(mx / 50) * 50 + 25;
      minSpan = 100.0f; break;
    case Q_PRESS:
      mn = floorf(mn) - 1;             mx = ceilf(mx) + 1;
      minSpan = 4.0f; break;
    case Q_VBAT:
      // Baterie se meni pomalu - hruba osa by ukazovala rovnou caru.
      mn -= 0.05f;                     mx += 0.05f;
      minSpan = 0.20f; break;
    case Q_HUM:
      mn = floorf(mn) - 1;             mx = ceilf(mx) + 1;
      minSpan = 5.0f; break;
    default:                            // teploty
      mn = floorf(mn) - 0.5f;          mx = ceilf(mx) + 0.5f;
      minSpan = 2.0f; break;
  }
  if (mx - mn < minSpan) { float c = (mn + mx) / 2; mn = c - minSpan/2; mx = c + minSpan/2; }

  // --- radek nad grafem: vlevo velicina, vpravo extremy za obdobi ---
  char title[56];
  snprintf(title, sizeof(title), "%s [%s]", qLabel(q), qUnit(q));
  display.setFont(&FreeSans9pt7b);
  display.setCursor(gx, gy - 8);
  display.print(title);

  if (any) {
    char lo[14], hi[14], mm[40];
    if (q == Q_CO2)       { snprintf(lo, sizeof(lo), "%d", (int)lroundf(dataMin));
                            snprintf(hi, sizeof(hi), "%d", (int)lroundf(dataMax)); }
    else if (q == Q_VBAT) { snprintf(lo, sizeof(lo), "%.2f", dataMin);
                            snprintf(hi, sizeof(hi), "%.2f", dataMax); }
    else                  { snprintf(lo, sizeof(lo), "%.1f", dataMin);
                            snprintf(hi, sizeof(hi), "%.1f", dataMax); }
    snprintf(mm, sizeof(mm), "min %s | max %s", lo, hi);
    int16_t mx1, my1; uint16_t mw, mh;
    display.getTextBounds(mm, 0, 0, &mx1, &my1, &mw, &mh);
    // 4 px rezerva - getTextBounds vraci tesny obrys, skutecny posun
    // kurzoru byva o par pixelu vetsi.
    display.setCursor(gRight - mw - 4, gy - 8);
    display.print(mm);
  }

  display.drawRect(gx, gy, gw, gh, GxEPD_BLACK);

  // Nahore je vyhrazeny pruh pro popisky namerenych hodnot. Prubeh se mapuje
  // az pod nej, takze se cara s popisky neprekryje ani pri velkem rozkmitu.
  const int labBand = 18;
  const int plotTop = gy + labBand;
  const int plotH   = gBottom - plotTop;

  auto yFor = [&](float v) -> int {
    return gBottom - (int)((v - mn) / (mx - mn) * plotH);
  };
  auto xFor = [&](uint16_t i) -> int {
    if (histCount <= 1) return gRight;
    return gx + (int)lroundf((float)i * gw / (histCount - 1));
  };
  // popisek hodnoty: u desetinnych velicin jedno misto, u CO2 cele cislo
  auto fmtVal = [&](float v, char *o, size_t n) {
    if (q == Q_CO2)       snprintf(o, n, "%d", (int)lroundf(v));
    else if (q == Q_VBAT) snprintf(o, n, "%.2f", v);
    else                  snprintf(o, n, "%.1f", v);
  };

  // --- osa Y: 3 hlavni carkovane + 2 vedlejsi teckovane mezi nimi ---
  display.setFont(&FreeSans9pt7b);
  for (int i = 0; i <= 2; i++) {
    float val = mn + (mx - mn) * i / 2;
    int yy = yFor(val);
    for (int xx = gx + 2; xx < gRight; xx += 6) display.drawPixel(xx, yy, GxEPD_BLACK);
    char lab[12]; fmtVal(val, lab, sizeof(lab));
    int16_t x1, y1; uint16_t bw2, bh2;
    display.getTextBounds(lab, 0, 0, &x1, &y1, &bw2, &bh2);
    display.setCursor(gx - bw2 - 4, yy + bh2 / 2);
    display.print(lab);
  }
  // vedlejsi (mezi hlavnimi) - jemnejsi tecky, bez popisku
  for (int i = 0; i < 2; i++) {
    float val = mn + (mx - mn) * (2 * i + 1) / 4.0f;
    int yy = yFor(val);
    for (int xx = gx + 4; xx < gRight; xx += 12) display.drawPixel(xx, yy, GxEPD_BLACK);
  }

  // --- osa X: carkovane svisle linky + hodnota v danem case nahore ---
  for (int i = 0; i <= 2; i++) {
    int xx = gx + gw * i / 2;
    display.drawFastVLine(xx, gBottom, 4, GxEPD_BLACK);

    // svisla carkovana mrizka (krajni linky splyvaji s ramem, ty vynechame)
    if (i == 1)
      for (int yy = plotTop + 2; yy < gBottom; yy += 7) display.drawPixel(xx, yy, GxEPD_BLACK);

    // popisek casu pod osou
    uint32_t back = spanMin - (spanMin * i / 2);
    char lab[14];
    if (back == 0)           snprintf(lab, sizeof(lab), "0");
    else if (back < 60)      snprintf(lab, sizeof(lab), "-%lum", (unsigned long)back);
    else if (back % 60 == 0) snprintf(lab, sizeof(lab), "-%luh", (unsigned long)(back / 60));
    else snprintf(lab, sizeof(lab), "-%luh%02lu",
                  (unsigned long)(back / 60), (unsigned long)(back % 60));
    int16_t x1, y1; uint16_t bw2, bh2;
    display.getTextBounds(lab, 0, 0, &x1, &y1, &bw2, &bh2);
    int lx = constrain(xx - bw2 / 2, gx, gRight - (int)bw2);
    display.setCursor(lx, gBottom + 16);
    display.print(lab);

    // namerena hodnota v tomto okamziku, vypsana u horni hrany grafu
    if (histCount > 0) {
      uint16_t idx = (histCount <= 1) ? 0
                   : (uint16_t)lroundf((float)i * (histCount - 1) / 2.0f);
      float v = storeToVal(q, histAt(ch, idx));
      if (!isnan(v)) {
        char vb[14]; fmtVal(v, vb, sizeof(vb));
        display.getTextBounds(vb, 0, 0, &x1, &y1, &bw2, &bh2);
        // Krajni popisky posuneme dovnitr, at nelezou na ram grafu.
        const int inset = 10;
        int vx;
        if (i == 0)      vx = gx + inset;                       // zarovnat zleva
        else if (i == 2) vx = gRight - inset - (int)bw2;        // zarovnat zprava
        else             vx = xx - (int)bw2 / 2;                // na stred
        vx = constrain(vx, gx + 3, gRight - (int)bw2 - 3);
        display.fillRect(vx - 2, gy + 2, bw2 + 4, bh2 + 4, GxEPD_WHITE);
        display.setCursor(vx, gy + bh2 + 4);
        display.print(vb);
      }
    }
  }

  // --- prubeh: silnejsi cara (dva pixely vedle sebe) ---
  int px = -1, py = -1;
  for (uint16_t i = 0; i < histCount; i++) {
    float v = storeToVal(q, histAt(ch, i));
    if (isnan(v)) { px = -1; continue; }
    int x = xFor(i), y = yFor(v);
    if (px >= 0) {
      if (channels[ch].dashed) {
        drawDashedLine(px, py, x, y, 5, 4);
      } else {
        display.drawLine(px, py, x, y, GxEPD_BLACK);
        display.drawLine(px, py - 1, x, y - 1, GxEPD_BLACK);   // tloustka 2 px
      }
    }
    px = x; py = y;
  }
}

void drawGraphs(int top) {
  const int bottom = H - 30, left = 55, right = W - 15;
  const int gw = right - left, gap = 42;   // popisky osy X se nedotknou
                                           // nadpisu nasledujiciho grafu
  int n = channelCount > 0 ? channelCount : 1;
  int gh = (bottom - top - gap * n) / n;
  if (gh < 40) gh = 40;
  int y = top + 26;          // odstup od delici cary nad prvnim nadpisem
  for (uint8_t c = 0; c < channelCount; c++) {
    drawOneGraph(c, left, y, gw, gh);
    y += gh + gap;
  }
}

void render(const Reading &r, float vb) {
  display.setRotation(3);
  display.setTextColor(GxEPD_BLACK);
  // Bez tohoto GFX zalamuje text, jakmile kurzor + sirka znaku presahne
  // sirku displeje - posledni slovo pak spadne na dalsi radek a rozbije layout.
  display.setTextWrap(false);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeader(r, vb);
    drawGraphs(headerBottom(r));
  } while (display.nextPage());
}

// ============================================================================
// SERVISNI REZIM
// ============================================================================
void svcAvailable() {
  Serial.print("Dostupne veliciny: ");
  const Quantity all[] = { Q_TEMP, Q_HUM, Q_CO2, Q_PRESS, Q_TEMP2, Q_VBAT };
  bool first = true;
  for (uint8_t i = 0; i < 6; i++) {
    if (!qAvailable(all[i])) continue;
    if (!first) Serial.print(", ");
    Serial.printf("%s (%s)", qKey(all[i]), qLabel(all[i]));
    first = false;
  }
  Serial.println();
}

void svcHelp() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("  MeteoEink426 v3 - SERVISNI REZIM");
  Serial.println("========================================");
  Serial.printf ("Detekovana cidla: %s\n", sensorSummary());
  Serial.print  ("Kanaly grafu:     ");
  for (uint8_t c = 0; c < channelCount; c++) {
    Serial.printf("%s%s", qKey(channels[c].q), (c + 1 < channelCount) ? ", " : "");
  }
  Serial.printf(" (%s)\n", cfg.chAuto ? "auto" : "rucne");
  svcAvailable();
  Serial.printf("Interval mereni:  %d min\n", cfg.intervalMin);
  Serial.printf("Kapacita:         %u vzorku/kanal = %.1f h\n",
                histPerCh, histPerCh * cfg.intervalMin / 60.0f);
  Serial.println();
  Serial.println("Prikazy (desetinna tecka, napr. toff=-1.5):");
  Serial.println();
  Serial.println("  KOMPENZACE");
  Serial.println("  toff=<x>    Offset teploty [degC]   (-20 az 20)");
  Serial.println("  hoff=<x>    Offset vlhkosti [%RH]   (-30 az 30)");
  if (det.bosch != BOSCH_NONE) {
    Serial.println("  poff=<x>    Offset tlaku [hPa]      (-50 az 50)");
    Serial.println("  alt=<x>     Nadmorska vyska [m]     (0 az 4000)");
  }
  if (det.scd41) {
    Serial.println();
    Serial.println("  CO2 (SCD41)");
    Serial.println("  co2ref=<x>  Kalibrace na hodnotu [ppm] (venku ~420, 3+ min)");
    Serial.println("  asc=0|1     Automaticka samokalibrace");
  }
  Serial.println();
  Serial.println("  MERENI A GRAF");
  Serial.printf ("  int=<min>   Interval mereni (%d az %d). Meni delku historie!\n",
                 INTERVAL_MIN_LO, INTERVAL_MIN_HI);
  Serial.println("  ch=auto     Automaticky vyber kanalu grafu");
  Serial.println("  ch=a,b,c    Rucni vyber, napr. ch=co2,temp,press");
  Serial.println();
  Serial.println("  SPRAVA");
  Serial.println("  list        Vypise aktualni nastaveni");
  Serial.println("  dump        Vypise celou historii jako CSV");
  Serial.println("  help nebo ? Tato napoveda");
  Serial.println("  save        Ulozi nastaveni do NVS");
  Serial.println("  clear       Smaze historii mereni");
  Serial.println("  exit        Ulozi a ukonci servis");
  Serial.println();
  Serial.printf ("Servis se ukonci po %d s neaktivity.\n", SERVICE_TIMEOUT_MS / 1000);
  Serial.printf ("Tlacitka pri restartu: PUSH %d-%d s = servis, PUSH pres %d s = WiFi,\n"
                 "                       DOWN %d s = smazat historii.\n",
                 SERVICE_HOLD_MS/1000, AP_HOLD_MS/1000, AP_HOLD_MS/1000, CLEAR_HOLD_MS/1000);
  Serial.println("========================================");
}

void svcList() {
  Serial.println("--- Aktualni nastaveni ---");
  Serial.printf("  toff = %.2f degC\n", cfg.tempOff);
  Serial.printf("  hoff = %.2f %%RH\n", cfg.humOff);
  Serial.printf("  poff = %.2f hPa\n", cfg.pressOff);
  Serial.printf("  alt  = %.1f m\n",   cfg.altitude);
  if (det.scd41) Serial.printf("  asc  = %d\n", cfg.scdAsc);
  Serial.printf("  int  = %d min\n", cfg.intervalMin);
  Serial.print ("  ch   = ");
  for (uint8_t c = 0; c < channelCount; c++)
    Serial.printf("%s%s", qKey(channels[c].q), (c + 1 < channelCount) ? "," : "");
  Serial.printf(" (%s)\n", cfg.chAuto ? "auto" : "rucne");
  Serial.printf("  historie: %u vzorku z %u (%.1f h z %.1f h)\n",
                histCount, histPerCh, histSpanMin() / 60.0f,
                histPerCh * cfg.intervalMin / 60.0f);
}

// Export historie jako CSV mezi znackami #CSV a #END (parsuje web).
void svcDump() {
  Serial.println("#CSV");
  Serial.printf("#interval=%d\n", cfg.intervalMin);
  Serial.printf("#count=%u\n", histCount);
  Serial.print("min_zpet");
  for (uint8_t c = 0; c < channelCount; c++)
    Serial.printf(";%s", qLabel(channels[c].q));
  Serial.println();
  for (uint16_t i = 0; i < histCount; i++) {
    long back = -(long)(histCount - 1 - i) * cfg.intervalMin;
    Serial.print(back);
    for (uint8_t c = 0; c < channelCount; c++) {
      float v = storeToVal(channels[c].q, histAt(c, i));
      if (isnan(v)) Serial.print(";");
      else if (channels[c].q == Q_CO2) Serial.printf(";%d", (int)lroundf(v));
      else Serial.printf(";%.2f", v);
    }
    Serial.println();
    if ((i & 0x1F) == 0x1F) delay(2);      // nezahltit seriovy buffer
  }
  Serial.println("#END");
}

// Zpracuje prikaz ch=...
void svcSetChannels(String v, bool &dirty) {
  v.toLowerCase(); v.trim();
  if (v == "auto") {
    cfg.chAuto = 1;
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) cfg.chSel[i] = Q_NONE;
    dirty = true;
    relayoutHistory(false);
    Serial.print("ch=auto -> ");
    for (uint8_t c = 0; c < channelCount; c++)
      Serial.printf("%s%s", qKey(channels[c].q), (c + 1 < channelCount) ? "," : "");
    Serial.println();
    return;
  }
  Quantity sel[MAX_CHANNELS] = { Q_NONE, Q_NONE, Q_NONE };
  uint8_t n = 0;
  int start = 0;
  while (start <= (int)v.length() && n < MAX_CHANNELS) {
    int comma = v.indexOf(',', start);
    String part = (comma < 0) ? v.substring(start) : v.substring(start, comma);
    part.trim();
    if (part.length()) {
      Quantity q = qFromKey(part);
      if (q == Q_NONE) { Serial.printf("Neznama velicina: %s\n", part.c_str()); return; }
      if (!qAvailable(q)) {
        Serial.printf("Velicina %s neni dostupna - chybi cidlo.\n", part.c_str());
        return;
      }
      for (uint8_t i = 0; i < n; i++)
        if (sel[i] == q) { Serial.println("Duplicitni velicina."); return; }
      sel[n++] = q;
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  if (n == 0) { Serial.println("Zadny platny kanal."); return; }
  cfg.chAuto = 0;
  for (uint8_t i = 0; i < MAX_CHANNELS; i++) cfg.chSel[i] = (i < n) ? sel[i] : Q_NONE;
  dirty = true;
  relayoutHistory(false);
  Serial.printf("ch nastaveno (%u kanalu), kapacita %u vzorku = %.1f h\n",
                channelCount, histPerCh, histPerCh * cfg.intervalMin / 60.0f);
}

void svcSetInterval(int v, bool &dirty) {
  if (v < INTERVAL_MIN_LO || v > INTERVAL_MIN_HI) {
    Serial.printf("Interval musi byt %d az %d min. Nezmeneno.\n",
                  INTERVAL_MIN_LO, INTERVAL_MIN_HI);
    return;
  }
  cfg.intervalMin = (uint8_t)v;
  dirty = true;
  relayoutHistory(true);
  float hours = histPerCh * v / 60.0f;
  Serial.printf("int=%d min -> historie %u vzorku = %.1f h (%.1f dne)\n",
                v, histPerCh, hours, hours / 24.0f);
  if (v < INTERVAL_WARN)
    Serial.println("POZOR: kratky interval vyrazne zkracuje vydrz baterie"
                   " a delku historie.");
}

bool svcHandle(String line, bool &dirty) {
  line.trim();
  if (!line.length()) return false;
  String low = line; low.toLowerCase();

  if (low == "help" || low == "?") { svcHelp(); return false; }
  if (low == "list")  { svcList(); return false; }
  if (low == "dump")  { svcDump(); return false; }
  if (low == "save")  { cfgSave(); dirty = false; Serial.println("Ulozeno do NVS."); return false; }
  if (low == "clear") { histClear(); histEraseNVS(); Serial.println("Historie smazana."); return false; }
  if (low == "exit")  {
    if (dirty) { cfgSave(); Serial.println("Ulozeno."); }
    Serial.println("Ukoncuji servis.");
    return true;
  }

  int eq = line.indexOf('=');
  if (eq > 0) {
    String key = line.substring(0, eq); key.trim(); key.toLowerCase();
    String val = line.substring(eq + 1); val.trim();
    float f = val.toFloat();

    if (key == "toff") {
      if (fabsf(f) > 20.0f) { Serial.println("Rozsah -20 az 20. Nezmeneno."); return false; }
      cfg.tempOff = f; dirty = true; Serial.printf("toff=%.2f\n", f);
    } else if (key == "hoff") {
      if (fabsf(f) > 30.0f) { Serial.println("Rozsah -30 az 30. Nezmeneno."); return false; }
      cfg.humOff = f; dirty = true; Serial.printf("hoff=%.2f\n", f);
    } else if (key == "poff") {
      if (fabsf(f) > 50.0f) { Serial.println("Rozsah -50 az 50. Nezmeneno."); return false; }
      cfg.pressOff = f; dirty = true; Serial.printf("poff=%.2f\n", f);
    } else if (key == "alt") {
      if (f < 0 || f > 4000) { Serial.println("Rozsah 0 az 4000. Nezmeneno."); return false; }
      cfg.altitude = f; dirty = true; Serial.printf("alt=%.1f\n", f);
    } else if (key == "asc") {
      cfg.scdAsc = val.toInt() ? 1 : 0; dirty = true;
      Serial.printf("asc=%d\n", cfg.scdAsc);
    } else if (key == "co2ref") {
      int c = val.toInt();
      if (c < 300 || c > 2000) { Serial.println("Rozsah 300 az 2000 ppm. Nezmeneno."); return false; }
      pendingCo2Ref = (int16_t)c;
      Serial.printf("co2ref=%d (provede se pri dalsim mereni)\n", c);
    } else if (key == "int") {
      svcSetInterval(val.toInt(), dirty);
    } else if (key == "ch") {
      svcSetChannels(val, dirty);
    } else {
      Serial.println("Neznamy prikaz. Napis 'help'.");
    }
    return false;
  }
  Serial.println("Neznamy prikaz. Napis 'help'.");
  return false;
}

void runService() {
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  svcHelp();
  svcList();
  Serial.print("\n> ");

  bool dirty = false;
  String line;
  unsigned long last = millis();
  while (true) {
    if (millis() - last > SERVICE_TIMEOUT_MS) {
      Serial.println("\nTimeout - servis ukoncen bez ulozeni.");
      return;
    }
    while (Serial.available()) {
      char ch = Serial.read();
      last = millis();
      if (ch == '\n' || ch == '\r') {
        if (line.length()) {
          if (svcHandle(line, dirty)) return;
          Serial.print("> ");
        }
        line = "";
      } else if (line.length() < 80) {
        line += ch;
      }
    }
    delay(20);
  }
}

// ============================================================================
// OCHRANA HISTORIE PRI ZMENE SESTAVY
// Odlisny podpis se musi potvrdit dvakrat za sebou, aby vypadek cidla
// (zaruseni I2C, podpeti) nesmazal nasbirana data.
// ============================================================================
#define SIG_CONFIRM 2

bool signatureConfirmed(uint32_t sig) {
  uint32_t pend = 0; uint8_t cnt = 0;
  if (prefs.begin(NVS_NS, true)) {
    pend = prefs.getUInt(NVS_SIGPEND, 0);
    cnt  = prefs.getUChar(NVS_SIGCNT, 0);
    prefs.end();
  }
  if (pend == sig) cnt++; else { pend = sig; cnt = 1; }
  bool confirmed = (cnt >= SIG_CONFIRM);
  if (prefs.begin(NVS_NS, false)) {
    if (confirmed) { prefs.remove(NVS_SIGPEND); prefs.remove(NVS_SIGCNT); }
    else { prefs.putUInt(NVS_SIGPEND, pend); prefs.putUChar(NVS_SIGCNT, cnt); }
    prefs.end();
  }
  return confirmed;
}

// Ceka, zda je tlacitko drzeno nepretrzite po celou dobu holdMs.
// Vraci false, jakmile ho uzivatel pusti driv (zabrani nechtene akci).
bool holdFor(uint8_t pin, uint32_t holdMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < holdMs) {
    if (digitalRead(pin) != LOW) return false;
    delay(10);
  }
  return true;
}

// ============================================================================
// WIFI HOTSPOT S KONFIGURACNI STRANKOU
// Spusti se podrzenim PUSH >= 5 s po restartu. Vytvori zabezpecenou WPA2 sit,
// na displeji ukaze SSID, heslo, adresu a QR kod pro pripojeni telefonu.
// Po AP_TIMEOUT_MS bez jedineho pozadavku se sam vypne a deska jde merit.
// ============================================================================
WebServer  httpd(80);
DNSServer  dnsd;
String     apSsid;
uint32_t   apLastActivity = 0;
bool       apShouldStop   = false;

// Vygeneruje stabilni heslo z MAC adresy, pokud jeste zadne neni.
void ensureApPassword() {
  if (strlen(cfg.apPass) >= 8) return;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  const char *abc = "abcdefghijkmnpqrstuvwxyz23456789";   // bez matoucich znaku
  uint32_t seed = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                  ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
  seed ^= 0xA5C3F17Bu;
  for (uint8_t i = 0; i < 10; i++) {
    seed = seed * 1664525u + 1013904223u;
    cfg.apPass[i] = abc[(seed >> 17) % 32];
  }
  cfg.apPass[10] = 0;
  cfgSave();
}

// ---------------------------------------------------------------------------
// Konfiguracni stranka ulozena ve flash. Stejny vzhled jako web, ale
// komunikuje pres HTTP/JSON (Web Serial pres hotspot nefunguje).
// ---------------------------------------------------------------------------
static const char AP_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="cs"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Meteo E-ink konfigurátor</title><style>
:root{--bg:#1a1d23;--el:#21252d;--in:#171a20;--bd:#2f353f;--bs:#3d4552;
--tx:#e6e9ef;--dm:#9aa3b2;--ft:#6b7482;--ac:#ffb524;--ok:#3ecf8e;--er:#ff6b6b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);
font:16px/1.6 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
.w{max-width:640px;margin:0 auto;padding:0 14px 40px}
h1{font-size:22px;margin:20px 0 4px}p.s{margin:0 0 16px;color:var(--dm);font-size:14px}
.p{border:1px solid var(--bd);background:var(--el);border-radius:10px;margin:14px 0}
.p>h2{margin:0;padding:12px 14px;font-size:15px;border-bottom:1px solid var(--bd)}
.b{padding:14px}
.g{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:1px;
background:var(--bd);border:1px solid var(--bd);border-radius:8px;overflow:hidden}
.c{background:var(--in);padding:10px 12px}
.k{font-size:10px;text-transform:uppercase;letter-spacing:.08em;color:var(--ft);font-weight:700}
.v{font-size:17px;font-weight:700;font-family:ui-monospace,monospace}
.r{display:flex;gap:6px;align-items:center;margin:10px 0 0}
.r label{flex:1;font-size:14px;color:var(--dm)}
input[type=number]{width:96px;height:38px;text-align:center;font-family:ui-monospace,monospace;
font-size:15px;font-weight:700;background:var(--in);border:1px solid var(--bs);
color:var(--tx);border-radius:6px}
input:focus{border-color:var(--ac);outline:none}
button{font:600 14px inherit;padding:10px 14px;border-radius:6px;border:1px solid var(--bs);
background:var(--in);color:var(--tx);cursor:pointer;transition:transform .07s}
button:active{transform:translateY(1px) scale(.985)}
button.pr{background:var(--ac);border-color:var(--ac);color:#1a1d23}
button.dn{background:var(--ok)!important;border-color:var(--ok)!important;color:#06130d!important}
button.dg{border-color:var(--er);color:var(--er)}
.acts{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
.iv{display:flex;gap:6px;flex-wrap:wrap}
.iv button{min-width:52px;font-family:ui-monospace,monospace}
.iv button.on{background:var(--ac);border-color:var(--ac);color:#1a1d23}
.cl{margin-top:12px;border:1px solid var(--bd);background:var(--in);border-radius:8px;padding:2px 12px}
.cl div{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--bd);font-size:13px}
.cl div:last-child{border:0}.cl b{font-family:ui-monospace,monospace}
.wn{color:var(--ac);font-size:13px;margin:8px 0 0}
.n{font-size:13px;color:var(--dm);margin:10px 0 0}
.t{position:fixed;left:0;right:0;bottom:0;background:var(--ok);color:#06130d;
text-align:center;padding:12px;font-weight:700;transform:translateY(100%);transition:.25s}
.t.on{transform:none}.t.er{background:var(--er);color:#fff}
.q{display:flex;gap:8px;flex-wrap:wrap}
.q label{display:flex;gap:7px;align-items:center;border:1px solid var(--bs);
background:var(--in);border-radius:6px;padding:9px 11px;font-size:13.5px;cursor:pointer}
.q label.off{opacity:.35;pointer-events:none}
.hd{display:none}
.ro{border:1px solid var(--bd);background:var(--in);border-radius:6px;padding:8px 11px;
margin-bottom:10px;font-family:ui-monospace,monospace;font-size:12.5px;color:var(--dm);
min-height:34px;display:flex;gap:12px;flex-wrap:wrap;align-items:center}
.ro b{color:var(--tx)}.ro .tm{color:var(--ac);font-weight:700}
.ch{border:1px solid var(--bd);background:var(--in);border-radius:8px;padding:8px 10px 4px;margin-bottom:10px}
.ch h5{margin:0 0 5px;font-size:12px;font-weight:600;color:var(--dm)}
.ch canvas{display:block;width:100%;height:130px;touch-action:none}
</style></head><body><div class="w">
<h1>Meteo E-ink konfigurátor</h1>
<p class="s">Nastavení přes WiFi. Deska hotspot sama vypne po 5 minutách nečinnosti.</p>

<div class="p"><h2>Stav</h2><div class="b"><div class="g">
<div class="c"><div class="k">Čidla</div><div class="v" id="vS">—</div></div>
<div class="c"><div class="k">Interval</div><div class="v" id="vI">—</div></div>
<div class="c"><div class="k">Historie</div><div class="v" id="vH">—</div></div>
<div class="c"><div class="k">Baterie</div><div class="v" id="vB">—</div></div>
</div></div></div>

<div class="p"><h2>Kompenzace</h2><div class="b">
<div class="r"><label>Offset teploty [°C]</label><input type=number id=toff step=.1 min=-20 max=20></div>
<div class="r"><label>Offset vlhkosti [%RH]</label><input type=number id=hoff step=.5 min=-30 max=30></div>
<div class="r hd" id="rP"><label>Offset tlaku [hPa]</label><input type=number id=poff step=.1 min=-50 max=50></div>
<div class="r hd" id="rA"><label>Nadmořská výška [m]</label><input type=number id=alt step=10 min=0 max=4000></div>
<div class="acts"><button class="pr" onclick="apply(this)">Použít</button></div>
</div></div>

<div class="p"><h2>Interval měření</h2><div class="b">
<div class="iv" id="ivb"></div>
<div class="cl">
<div><span>Délka historie</span><b id="cH">—</b></div>
<div><span>Kapacita</span><b id="cC">—</b></div>
</div>
<p class="wn hd" id="wn"></p>
<p class="n">Změna intervalu automaticky založí novou historii - vzorky s jiným rozestupem by rozbily časovou osu.</p>
</div></div>

<div class="p"><h2>Veličiny v grafu</h2><div class="b">
<div class="r"><label>Vybrat automaticky</label>
<input type=checkbox id=auto onchange="qUpd()" style="width:20px;height:20px;accent-color:var(--ac)"></div>
<div class="q" id="qg" style="margin-top:10px"></div>
<div class="acts"><button onclick="applyCh(this)">Použít veličiny</button></div>
</div></div>

<div class="p"><h2>Grafy</h2><div class="b">
<div class="acts" style="margin-top:0">
<button class="pr" onclick="loadG(this)">Načíst grafy</button>
<button onclick="location='/api/csv'">Stáhnout CSV</button>
<button id="bpng" onclick="pngG(this)" disabled>Stáhnout obrázek</button>
</div>
<p class="n" id="gmsg">Grafy se zatím nenačetly.</p>
<div id="gwrap" class="hd"><div class="ro" id="ro">Dotkněte se grafu</div><div id="chs"></div></div>
</div></div>

<div class="p"><h2>Uložení a správa</h2><div class="b">
<div class="acts">
<button class="pr" onclick="req('/api/save',this,'Uloženo ✓')">Uložit do desky</button>
<button onclick="location='/api/csv'">Stáhnout CSV</button>
<button class="dg" onclick="clr(this)">Smazat historii</button>
<button onclick="fin(this)">Uložit a vypnout hotspot</button>
</div>
<p class="n">Změny platí hned, ale trvale se uloží až tlačítkem Uložit.</p>
</div></div>
</div><div class="t" id="tst"></div>
<script>
var S={},AV={},Q=[["temp","Teplota"],["hum","Vlhkost"],["co2","CO2"],["press","Tlak"],["temp2","Teplota 2"],["vbat","Baterie"]];
var POOL=2592,CAP=2016;
function toast(m,e){var t=document.getElementById("tst");t.textContent=m;
t.className="t on"+(e?" er":"");setTimeout(function(){t.className="t"+(e?" er":"")},2200)}
function ok(b,l){if(!b)return;var o=b.textContent;b.textContent=l;b.classList.add("dn");
setTimeout(function(){b.textContent=o;b.classList.remove("dn")},1600)}
function g(i){return document.getElementById(i)}
function req(u,b,l,cb){fetch(u).then(function(r){
if(!r.ok)throw new Error("http "+r.status);return r.json()}).then(function(j){
if(!j||j.error)throw new Error(j&&j.error||"err");
S=j;draw();if(b)ok(b,l||"Hotovo ✓");if(cb)cb(j)})
.catch(function(e){toast(String(e).indexOf("http 500")>0?"Uložení selhalo!":"Chyba spojení",1)})}
function nch(){if(g("auto").checked)return S.autoCount||2;
var n=0;Q.forEach(function(q){var e=g("q_"+q[0]);if(e&&e.checked)n++});return n||1}
function calc(){var iv=+g("ivv").value||5,n=Math.min(Math.floor(POOL/nch()),CAP),m=n*iv;
g("cH").textContent=m<60?m+" min":(m<2880?(m/60).toFixed(1)+" h":(m/1440).toFixed(1)+" dne");
g("cC").textContent=n+" × "+nch()+" kan.";
var w=[];if(iv<5)w.push("Krátký interval výrazně zkrátí výdrž baterie.");
if(m<1440)w.push("Historie nepokryje ani celý den.");
g("wn").textContent=w.join(" ");g("wn").className=w.length?"wn":"wn hd"}
function draw(){g("vS").textContent=S.sensors||"—";g("vI").textContent=S.interval+" min";
g("vH").textContent=S.count+"/"+S.perCh;g("vB").textContent=(S.vbat||0).toFixed(2)+" V";
["toff","hoff","poff","alt"].forEach(function(k){if(document.activeElement!==g(k))g(k).value=S[k]});
AV=S.avail||{};g("rP").className=AV.press?"r":"r hd";g("rA").className=AV.press?"r":"r hd";
g("auto").checked=!!S.chAuto;
Q.forEach(function(q){var e=g("q_"+q[0]);if(!e)return;
var av=(q[0]=="temp"||q[0]=="hum")?1:AV[q[0]];
e.parentNode.className=av?"":"off";e.checked=(S.channels||[]).indexOf(q[0])>=0});
g("ivv").value=S.interval;
Array.prototype.forEach.call(document.querySelectorAll(".iv button"),function(b){
b.className=(+b.dataset.v===S.interval)?"on":""});qUpd();calc()}
function qUpd(){var a=g("auto").checked;
Array.prototype.forEach.call(g("qg").querySelectorAll("input"),function(i){i.disabled=a});calc()}
function apply(b){var p=["toff","hoff","poff","alt"].map(function(k){return k+"="+(+g(k).value||0)});
req("/api/set?"+p.join("&"),b,"Použito ✓")}
function applyCh(b){var s=g("auto").checked?"auto":
Q.filter(function(q){var e=g("q_"+q[0]);return e&&e.checked}).map(function(q){return q[0]}).join(",");
if(s!="auto"&&!s){toast("Vyberte aspoň jednu veličinu",1);return}
req("/api/set?ch="+s,b,"Použito ✓")}
function setIv(v,b){req("/api/set?int="+v,b,"Použito ✓")}
function clr(b){if(!confirm("Opravdu smazat celou historii měření?"))return;
req("/api/clear",b,"Smazáno ✓")}
function fin(b){ok(b,"Vypínám…");fetch("/api/exit").then(function(){
document.body.innerHTML='<div class="w"><h1>Hotovo</h1><p class="s">Hotspot se vypíná, deska pokračuje v měření. Tuto stránku můžete zavřít.</p></div>'})}
var GD=null;
var UN={"Teplota":"degC","Teplota 2":"degC","Vlhkost":"%","CO2":"ppm","Tlak":"hPa","Baterie":"V"};
function fb(m){m=Math.abs(m);if(!m)return"teď";if(m<60)return"-"+m+" min";
var h=Math.floor(m/60),r=m%60;return"-"+h+" h"+(r?" "+r+" m":"")}
function loadG(b){ok(b,"Načítám…");
fetch("/api/csv").then(function(r){return r.text()}).then(function(t){
 var L=t.trim().split("\n").filter(function(l){return l&&l.indexOf("#")!==0});
 if(L.length<2){g("gmsg").textContent="Deska zatím nemá žádná měření.";toast("Zatím prázdné",1);return}
 var cols=L[0].split(";").slice(1),rows=[];
 for(var i=1;i<L.length;i++){var p=L[i].split(";");var bk=parseInt(p[0],10);if(isNaN(bk))continue;
  rows.push([bk].concat(p.slice(1).map(function(v){return v===""?null:parseFloat(v)})))}
 if(!rows.length){g("gmsg").textContent="Deska zatím nemá žádná měření.";return}
 GD={cols:cols,rows:rows};g("gmsg").className="n hd";g("gwrap").className="";
 g("bpng").disabled=false;mkCh();ok(b,"Načteno ✓")})
 .catch(function(){toast("Chyba spojení",1)})}
function mkCh(){var box=g("chs");box.innerHTML="";
 GD.cols.forEach(function(n,ci){var d=document.createElement("div");d.className="ch";
  var u=UN[n]||"";d.innerHTML='<h5>'+n+(u?" ["+u+"]":"")+" · "+GD.rows.length+" vz.</h5>";
  var cv=document.createElement("canvas");d.appendChild(cv);box.appendChild(d);cv._ci=ci;
  var mv=function(e){var r=cv.getBoundingClientRect();
   var cx=(e.touches?e.touches[0].clientX:e.clientX)-r.left;
   var gg=cv._g;if(!gg)return;var t=Math.min(1,Math.max(0,(cx-gg.pl)/gg.gw));
   cur(Math.round(t*(GD.rows.length-1)));if(e.cancelable)e.preventDefault()};
  cv.addEventListener("mousemove",mv);cv.addEventListener("touchmove",mv,{passive:false});
  cv.addEventListener("touchstart",mv,{passive:false});
  cv.addEventListener("mouseleave",function(){cur(null)})});
 drawAllG()}
function drawG(cv,ci){var dpr=window.devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;
 cv.width=w*dpr;cv.height=h*dpr;var x=cv.getContext("2d");x.setTransform(dpr,0,0,dpr,0,0);
 x.clearRect(0,0,w,h);var pl=42,pr=6,pt=6,pb=18,gw=w-pl-pr,gh=h-pt-pb;
 var vs=GD.rows.map(function(r){return r[ci+1]}).filter(function(v){return v!==null&&!isNaN(v)});
 if(!vs.length)return;var mn=Math.min.apply(null,vs),mx=Math.max.apply(null,vs);
 var sp=mx-mn,pv=sp<1e-6?1:sp*0.12;mn-=pv;mx+=pv;
 x.font="10px ui-monospace,monospace";x.textAlign="right";x.lineWidth=1;
 for(var i=0;i<=3;i++){var v=mn+(mx-mn)*i/3,yy=Math.round(pt+gh-gh*i/3)+0.5;
  x.strokeStyle="#2f353f";x.globalAlpha=.4;x.beginPath();x.moveTo(pl,yy);x.lineTo(pl+gw,yy);x.stroke();
  x.globalAlpha=1;x.fillStyle="#6b7482";x.fillText(v.toFixed((mx-mn)<5?1:0),pl-5,yy+3)}
 x.textAlign="center";
 for(var i=0;i<=2;i++){var idx=Math.round((GD.rows.length-1)*i/2);
  x.fillText(fb(GD.rows[idx][0]),Math.min(Math.max(pl+gw*i/2,22),w-22),h-4)}
 x.strokeStyle="#ffb524";x.lineWidth=2;x.lineJoin="round";x.beginPath();var st=false;
 GD.rows.forEach(function(r,i){var v=r[ci+1];if(v===null||isNaN(v)){st=false;return}
  var px=pl+(GD.rows.length<=1?gw:gw*i/(GD.rows.length-1));
  var py=pt+gh-(v-mn)/(mx-mn)*gh;if(!st){x.moveTo(px,py);st=true}else x.lineTo(px,py)});
 x.stroke();
 if(cv._c!==undefined&&cv._c!==null){var i=cv._c;
  var px=pl+(GD.rows.length<=1?gw:gw*i/(GD.rows.length-1));
  x.globalAlpha=.55;x.lineWidth=1;x.beginPath();x.moveTo(px+.5,pt);x.lineTo(px+.5,pt+gh);x.stroke();
  x.globalAlpha=1;var v=GD.rows[i][ci+1];
  if(v!==null&&!isNaN(v)){var py=pt+gh-(v-mn)/(mx-mn)*gh;
   x.fillStyle="#ffb524";x.beginPath();x.arc(px,py,3,0,6.284);x.fill()}}
 cv._g={pl:pl,gw:gw}}
function drawAllG(){Array.prototype.forEach.call(document.querySelectorAll("#chs canvas"),
 function(cv){drawG(cv,cv._ci)})}
function cur(i){Array.prototype.forEach.call(document.querySelectorAll("#chs canvas"),
 function(cv){cv._c=i});drawAllG();
 if(i===null||!GD){g("ro").innerHTML="Dotkněte se grafu";return}
 var r=GD.rows[i],html='<span class="tm">'+fb(r[0])+"</span>";
 GD.cols.forEach(function(n,ci){var v=r[ci+1],u=UN[n]||"";
  html+="<span>"+n+": <b>"+(v===null||isNaN(v)?"—":(n=="CO2"?Math.round(v):v.toFixed(2)))+(u?" "+u:"")+"</b></span>"});
 g("ro").innerHTML=html}
window.addEventListener("resize",function(){if(GD)drawAllG()});
function pngG(b){if(!GD)return;var cvs=Array.prototype.slice.call(document.querySelectorAll("#chs canvas"));
 if(!cvs.length)return;var pad=12,head=26,gy=10,w=cvs[0].clientWidth+pad*2,he=cvs[0].clientHeight+20;
 var o=document.createElement("canvas"),d=2;o.width=w*d;o.height=(head+cvs.length*(he+gy)+pad)*d;
 var x=o.getContext("2d");x.setTransform(d,0,0,d,0,0);
 x.fillStyle="#21252d";x.fillRect(0,0,w,o.height/d);
 x.fillStyle="#e6e9ef";x.font="600 13px system-ui,sans-serif";x.textAlign="left";
 x.fillText("Meteo E-ink - historie ("+GD.rows.length+" vzorků)",pad,18);
 var y=head;cvs.forEach(function(cv){
  x.fillStyle="#9aa3b2";x.font="11px system-ui,sans-serif";
  x.fillText(cv.parentNode.querySelector("h5").textContent,pad,y+10);
  x.drawImage(cv,pad,y+14,cv.clientWidth,cv.clientHeight);y+=he+gy});
 o.toBlob(function(bl){var a=document.createElement("a");a.href=URL.createObjectURL(bl);
  a.download="meteo-eink-grafy.png";a.click();
  setTimeout(function(){URL.revokeObjectURL(a.href)},1000)});ok(b,"Uloženo ✓")}
(function(){var h="";[1,2,5,10,15,30,60].forEach(function(v){
h+='<button data-v="'+v+'" onclick="setIv('+v+',this)">'+v+'</button>'});
h+='<input type=hidden id=ivv value=5>';g("ivb").innerHTML=h;
var q="";Q.forEach(function(x){q+='<label><input type=checkbox id="q_'+x[0]+'" onchange="calc()">'+x[1]+'</label>'});
g("qg").innerHTML=q;req("/api/status")})();
setInterval(function(){fetch("/api/ping")},60000);
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// JSON odpoved se stavem
// ---------------------------------------------------------------------------
String apStatusJson(float vbat) {
  String j = "{";
  j += "\"sensors\":\"" + String(sensorSummary()) + "\",";
  j += "\"interval\":" + String(cfg.intervalMin) + ",";
  j += "\"count\":" + String(histCount) + ",";
  j += "\"perCh\":" + String(histPerCh) + ",";
  j += "\"vbat\":" + String(vbat, 2) + ",";
  j += "\"toff\":" + String(cfg.tempOff, 2) + ",";
  j += "\"hoff\":" + String(cfg.humOff, 2) + ",";
  j += "\"poff\":" + String(cfg.pressOff, 2) + ",";
  j += "\"alt\":" + String(cfg.altitude, 0) + ",";
  j += "\"asc\":" + String(cfg.scdAsc) + ",";
  j += "\"chAuto\":" + String(cfg.chAuto) + ",";
  j += "\"autoCount\":" + String(channelCount) + ",";
  j += "\"channels\":[";
  for (uint8_t c = 0; c < channelCount; c++)
    j += String(c ? "," : "") + "\"" + qKey(channels[c].q) + "\"";
  j += "],\"avail\":{";
  j += "\"co2\":"   + String(qAvailable(Q_CO2)   ? 1 : 0) + ",";
  j += "\"press\":" + String(qAvailable(Q_PRESS) ? 1 : 0) + ",";
  j += "\"temp2\":" + String(qAvailable(Q_TEMP2) ? 1 : 0) + ",";
  j += "\"vbat\":1";
  j += "}}";
  return j;
}

// ---------------------------------------------------------------------------
// Vykresleni informacni obrazovky s QR kodem
// ---------------------------------------------------------------------------
// QR kod: verze 4 = 33 modulu. Velikost bufferu musi byt kompilacni
// konstanta - qrcode_getBufferSize() je bezna funkce a nelze ji pouzit
// pro rozmer statickeho pole. Vzorec z knihovny: ((size*size)+7)/8.
#define QR_VER    4
#define QR_MODS   (4 * QR_VER + 17)                  // 33
#define QR_BUFSZ  (((QR_MODS) * (QR_MODS) + 7) / 8)  // 137 B

void drawQr(const char *text, int x0, int y0, int scale) {
  QRCode qr;
  static uint8_t buf[QR_BUFSZ];
  if (qrcode_initText(&qr, buf, QR_VER, ECC_MEDIUM, text) < 0) return;
  // svetle pozadi s okrajem (quiet zone), aby ctecka QR nasla
  int quiet = 4 * scale;
  display.fillRect(x0 - quiet, y0 - quiet,
                   qr.size * scale + 2 * quiet, qr.size * scale + 2 * quiet,
                   GxEPD_WHITE);
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        display.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, GxEPD_BLACK);
}

void renderApScreen() {
  char wifiQr[96];
  snprintf(wifiQr, sizeof(wifiQr), "WIFI:T:WPA;S:%s;P:%s;;",
           apSsid.c_str(), cfg.apPass);

  display.setRotation(3);
  display.setTextColor(GxEPD_BLACK);
  display.setTextWrap(false);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(20, 50);
    display.print("Konfigurace WiFi");

    display.drawFastHLine(20, 66, W - 40, GxEPD_BLACK);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, 96);
    display.print("Sit (SSID)");
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(20, 120);
    display.print(apSsid);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, 152);
    display.print("Heslo");
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(20, 176);
    display.print(cfg.apPass);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, 208);
    display.print("Adresa v prohlizeci");
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(20, 232);
    display.print("http://192.168.4.1");

    // QR kod pro pripojeni k siti (naskenujte fotoaparatem telefonu)
    const int qsize = QR_MODS;
    const int scale = 9;
    int qx = (W - qsize * scale) / 2;
    int qy = 300;
    drawQr(wifiQr, qx, qy, scale);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, qy + qsize * scale + 34);
    display.print("Naskenujte QR fotoaparatem - telefon se pripoji k siti.");
    display.setCursor(20, qy + qsize * scale + 56);
    display.print("Pak otevrete http://192.168.4.1 (obvykle se otevre samo).");

    display.drawFastHLine(20, H - 62, W - 40, GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, H - 38);
    display.print("Hotspot se vypne po 5 minutach necinnosti.");
    display.setCursor(20, H - 18);
    display.print("Pak deska pokracuje v mereni.");
  } while (display.nextPage());
}

// ---------------------------------------------------------------------------
// Obsluha pozadavku
// ---------------------------------------------------------------------------
static float apVbat = NAN;

void apTouch() { apLastActivity = millis(); }

void apHandleSet() {
  apTouch();
  bool dirty = false;
  bool intervalChanged = false;
  for (uint8_t i = 0; i < httpd.args(); i++) {
    String k = httpd.argName(i), v = httpd.arg(i);
    float f = v.toFloat();
    if      (k == "toff" && fabsf(f) <= 20.0f)  { cfg.tempOff  = f; dirty = true; }
    else if (k == "hoff" && fabsf(f) <= 30.0f)  { cfg.humOff   = f; dirty = true; }
    else if (k == "poff" && fabsf(f) <= 50.0f)  { cfg.pressOff = f; dirty = true; }
    else if (k == "alt"  && f >= 0 && f <= 4000){ cfg.altitude = f; dirty = true; }
    else if (k == "asc") { cfg.scdAsc = v.toInt() ? 1 : 0; dirty = true; }
    else if (k == "int") {
      int iv = v.toInt();
      if (iv >= INTERVAL_MIN_LO && iv <= INTERVAL_MIN_HI) {
        if (cfg.intervalMin != (uint8_t)iv) intervalChanged = true;
        cfg.intervalMin = (uint8_t)iv; dirty = true;
      }
    }
    else if (k == "co2ref") {
      int c = v.toInt();
      if (c >= 300 && c <= 2000) pendingCo2Ref = (int16_t)c;
    }
    else if (k == "ch") {
      if (v == "auto") {
        cfg.chAuto = 1;
        for (uint8_t x = 0; x < MAX_CHANNELS; x++) cfg.chSel[x] = Q_NONE;
        dirty = true;
      } else {
        Quantity sel[MAX_CHANNELS] = { Q_NONE, Q_NONE, Q_NONE };
        uint8_t n = 0; int start = 0;
        while (start <= (int)v.length() && n < MAX_CHANNELS) {
          int comma = v.indexOf(',', start);
          String part = (comma < 0) ? v.substring(start) : v.substring(start, comma);
          part.trim(); part.toLowerCase();
          if (part.length()) {
            Quantity q = qFromKey(part);
            if (q != Q_NONE && qAvailable(q)) {
              bool dup = false;
              for (uint8_t z = 0; z < n; z++) if (sel[z] == q) dup = true;
              if (!dup) sel[n++] = q;
            }
          }
          if (comma < 0) break;
          start = comma + 1;
        }
        if (n > 0) {
          cfg.chAuto = 0;
          for (uint8_t x = 0; x < MAX_CHANNELS; x++) cfg.chSel[x] = (x < n) ? sel[x] : Q_NONE;
          dirty = true;
        }
      }
    }
  }
  if (dirty) { cfgSanitize(); relayoutHistory(intervalChanged); }
  httpd.send(200, "application/json", apStatusJson(apVbat));
}

void apSetupRoutes() {
  httpd.on("/", [](){ apTouch(); httpd.send_P(200, "text/html", AP_PAGE); });
  httpd.on("/api/status", [](){ apTouch(); httpd.send(200, "application/json", apStatusJson(apVbat)); });
  httpd.on("/api/ping",   [](){ apTouch(); httpd.send(200, "text/plain", "ok"); });
  httpd.on("/api/set",    apHandleSet);
  httpd.on("/api/save",   [](){
    apTouch();
    bool ok = cfgSave();
    if (!ok) { httpd.send(500, "application/json", "{\"error\":\"save\"}"); return; }
    httpd.send(200, "application/json", apStatusJson(apVbat));
  });
  httpd.on("/api/clear",  [](){
    apTouch(); histClear(); histEraseNVS();
    httpd.send(200, "application/json", apStatusJson(apVbat));
  });
  httpd.on("/api/exit",   [](){
    apTouch();
    cfgSave();                 // stejne jako seriovy prikaz exit - ulozit
    apShouldStop = true;
    httpd.send(200, "text/plain", "bye");
  });
  httpd.on("/api/csv",    [](){
    apTouch();
    String csv = "min_zpet";
    for (uint8_t c = 0; c < channelCount; c++) csv += ";" + String(qLabel(channels[c].q));
    csv += "\n";
    for (uint16_t i = 0; i < histCount; i++) {
      csv += String(-(long)(histCount - 1 - i) * cfg.intervalMin);
      for (uint8_t c = 0; c < channelCount; c++) {
        float v = storeToVal(channels[c].q, histAt(c, i));
        csv += ";";
        if (!isnan(v)) csv += (channels[c].q == Q_CO2)
                              ? String((int)lroundf(v)) : String(v, 2);
      }
      csv += "\n";
    }
    httpd.sendHeader("Content-Disposition", "attachment; filename=meteo-eink-historie.csv");
    httpd.send(200, "text/csv", csv);
  });
  // captive portal - cokoliv jineho presmeruj na hlavni stranku
  httpd.onNotFound([](){
    apTouch();
    httpd.sendHeader("Location", "http://192.168.4.1/", true);
    httpd.send(302, "text/plain", "");
  });
}

void runHotspot(float vbat) {
  apVbat = vbat;
  ensureApPassword();

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ss[32];
  snprintf(ss, sizeof(ss), "MeteoEink-%02X%02X", mac[4], mac[5]);
  apSsid = String(ss);

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(apSsid.c_str(), cfg.apPass)) {
    Serial.println("Hotspot se nepodarilo spustit.");
    WiFi.mode(WIFI_OFF);
    return;
  }
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("Hotspot: SSID=%s heslo=%s adresa=%s\n",
                apSsid.c_str(), cfg.apPass, ip.toString().c_str());

  dnsd.start(53, "*", ip);
  apSetupRoutes();
  httpd.begin();

  // Informacni obrazovka s QR kodem
  SPI.begin(EPD_CLK, EPD_MISO, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  renderApScreen();

  apShouldStop = false;
  apTouch();
  while (!apShouldStop) {
    dnsd.processNextRequest();
    httpd.handleClient();
    if (millis() - apLastActivity > AP_TIMEOUT_MS) {
      cfgSave();               // neztratit zmeny provedene pred timeoutem
      Serial.println("Hotspot: 5 min bez aktivity - ulozeno a vypinam.");
      break;
    }
    delay(2);
  }

  httpd.stop();
  dnsd.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("Hotspot ukoncen, pokracuji v mereni.");
}

// ============================================================================
// SPANEK
// ============================================================================
void goToSleep() {
  display.hibernate();
  powerOff();
  uint8_t iv = cfg.intervalMin;
  if (iv < INTERVAL_MIN_LO || iv > INTERVAL_MIN_HI) iv = DEF_INTERVAL;  // pojistka
  esp_sleep_enable_timer_wakeup((uint64_t)iv * 60ULL * uS_TO_S);
  esp_deep_sleep_start();
}

// ============================================================================
// SETUP
// ============================================================================
// ============================================================================
// NACTENI A VALIDACE HISTORIE
// Musi probehnout PRED servisnim rezimem i hotspotem - jinak by prikazy
// list a dump videly prazdnou historii, i kdyz data v pameti jsou.
// ============================================================================
void loadHistory(uint32_t sig) {
  if (!rtcInited) {
    // Studeny start nebo reset: RTC RAM je prazdna, zkusime NVS.
    if (histLoadNVS(sig)) {
      Serial.printf("Historie z NVS: %u vzorku.\n", histCount);
    } else {
      // Neshoda muze byt docasna (vypadek cidla) - potvrdit dvakrat.
      if (signatureConfirmed(sig)) {
        histClear();
        Serial.println("Nova sestava potvrzena - zalozena nova historie.");
      } else {
        histClear();
        Serial.println("Historie zatim nedostupna, cekam na potvrzeni sestavy.");
      }
    }
    rtcSignature = sig;
    rtcInited = true;
  } else if (rtcSignature != sig) {
    if (signatureConfirmed(sig)) {
      histClear();
      histEraseNVS();
      rtcSignature = sig;
      Serial.println("Zmena sestavy potvrzena - historie resetovana.");
    } else {
      Serial.println("Odlisna sestava (1. vyskyt) - historie zachovana.");
    }
  }
  // pojistka proti poskozenym indexum
  if (histPerCh == 0 || histHead >= histPerCh || histCount > histPerCh ||
      histSlots != histPerCh) {
    histClear();
  }
}

void setup() {
  Serial.begin(115200);
  delay(50);

  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  bool hardStart = (wake != ESP_SLEEP_WAKEUP_TIMER);

  powerOn();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  cfgLoad();
  detectSensors();
  buildChannels();
  uint32_t sig = buildSignature();

  // Historie se nacita jeste pred tlacitky, aby ji servisni rezim i hotspot
  // videly (prikazy list/dump by jinak hlasily 0 vzorku).
  loadHistory(sig);

  // --- Tlacitka po restartu ---
  // PUSH pusteno mezi 2-5 s = servis pres USB
  // PUSH drzeno >= 5 s      = WiFi hotspot s konfiguraci
  // DOWN drzeno >= 5 s      = smazani historie
  bool wantHotspot = false;
  if (hardStart) {
    pinMode(BTN_PUSH, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    delay(20);                              // ustaleni pull-upu

    if (digitalRead(BTN_PUSH) == LOW) {
      Serial.printf("PUSH: pust mezi %d-%d s = servis, drz pres %d s = WiFi...\n",
                    SERVICE_HOLD_MS / 1000, AP_HOLD_MS / 1000, AP_HOLD_MS / 1000);
      // Merime az do puštění, nejdele vsak AP_HOLD_MS.
      uint32_t t0 = millis();
      while (digitalRead(BTN_PUSH) == LOW && millis() - t0 < AP_HOLD_MS) delay(10);
      uint32_t held = millis() - t0;

      if (held >= AP_HOLD_MS) {
        wantHotspot = true;                 // spusti se az po zmereni baterie
        Serial.println("PUSH >= 5 s - spoustim WiFi hotspot.");
      } else if (held >= SERVICE_HOLD_MS) {
        runService();
        cfgLoad();
        buildChannels();
        sig = buildSignature();
        rtcSignature = sig;        // zmeny v servisu jsou vedome
      } else {
        Serial.println("PUSH pusteno prilis brzy - pokracuji v mereni.");
      }
    }
    else if (digitalRead(BTN_DOWN) == LOW) {
      Serial.printf("DOWN drzeno - podrz %d s pro smazani historie...\n",
                    CLEAR_HOLD_MS / 1000);
      if (holdFor(BTN_DOWN, CLEAR_HOLD_MS)) {
        histClear();
        histEraseNVS();
        rtcSignature = sig;
        Serial.println("Historie smazana (DOWN).");
      } else {
        Serial.println("DOWN pusteno prilis brzy - historie zachovana.");
      }
    }
  }

  // --- Mereni ---
  Reading r = doMeasurement();
  float vb = r.vbat;                // doMeasurement uz baterii precetl

  // --- WiFi hotspot (pokud byl vyzadan podrzenim PUSH) ---
  if (wantHotspot) {
    runHotspot(vb);
    // Behem konfigurace se mohl zmenit interval nebo kanaly.
    // NEnacitame znovu z NVS: kopie v RAM je autoritativni a hotspot ji pri
    // ukonceni i pri timeoutu ulozil. Opetovne cteni by jen pridalo riziko,
    // ze se pri chybe flash vratime na vychozi hodnoty.
    cfgSanitize();
    buildChannels();
    uint32_t newSig = buildSignature();
    if (newSig != sig) {
      // Vedoma zmena uzivatele - na rozdil od vypadku cidla se nepotvrzuje.
      // Vzorky se starym rozestupem nebo jinym rozlozenim kanalu by rozbily
      // casovou osu, proto zakladame historii znovu.
      sig = newSig;
      rtcSignature = sig;
      histClear();
      histEraseNVS();
      Serial.println("Nastaveni zmeneno pres WiFi - zalozena nova historie.");
    }
    r = doMeasurement();          // po konfiguraci cerstve mereni
    vb = r.vbat;
  }

  Serial.printf("T=%.2f RH=%.1f CO2=%.0f P=%.1f T2(DS)=%.2f T(bosch)=%.2f VBAT=%.2f%s | %s | int=%d\n",
                r.temp, r.hum, r.co2, r.press, r.temp2, r.tBosch, vb,
                (vb < VBAT_LOW ? " NIZKE!" : ""), sensorSummary(), cfg.intervalMin);

  histPush(r);

  // Zapis do NVS jen obcas (opotrebeni flash) a ne pri podpeti.
  // Zapis do NVS: prvni vzorky ulozime hned, aby se pri restartu hned po
  // nahrani firmwaru neztratil zacatek historie. Potom uz jen kazde N.
  // mereni, aby se zbytecne neopotrebovavala flash.
  saveTick++;
  if (histCount <= 3 || saveTick >= NVS_SAVE_EVERY) {
    saveTick = 0;
    histSaveNVS(sig, vb);
  }

  // --- Vykresleni ---
  SPI.begin(EPD_CLK, EPD_MISO, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  render(r, vb);

  goToSleep();
}

void loop() {}
