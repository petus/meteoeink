/*
 * MeteoEink426 v4 — LaskaKit ESPink-4.26 (ESP32-S3, ePaper GDEQ0426T82 800x480)
 * ============================================================================
 * Offline meteostanice s autodetekci vice cidel a volitelnymi kanaly grafu.
 *
 * SHT40 je vzdy zaklad (teplota + vlhkost, na desce). Soucasne se detekuji:
 *   - SHT4x   : teplota + vlhkost         (I2C 0x44 / 0x45 / 0x46)
 *   - SCD41   : CO2                       (I2C 0x62)
 *   - SEN6x   : PM1/2.5/4/10, RH/T, CO2   (I2C 0x6B) - viz nize
 *   - BME280  : tlak (+ vlhkost zalozne)  (I2C 0x76/0x77, chip ID 0x60)
 *   - BMP280  : tlak                      (I2C 0x76/0x77, chip ID 0x56-0x58)
 *   - DS18B20 : druha teplota             (OneWire GPIO4)
 *
 * SENSIRION SEN6x (novinka ve v4)
 * -------------------------------
 * Pouzivaji se OFICIALNI KNIHOVNY SENSIRION, ve skeci je jen tenka rozbocovaci
 * vrstva (sekce "SENSIRION SEN6x" nize). Podporovane varianty, jak je pozna
 * senKindFromName():
 *
 *   SEN63C : PM + RH/T + CO2                    knihovna povinna
 *   SEN66  : PM + RH/T + VOC + NOx + CO2        knihovna povinna
 *   SEN65  : PM + RH/T + VOC + NOx              knihovna volitelna
 *   SEN68  : PM + RH/T + VOC + NOx + HCHO       knihovna volitelna
 *   SEN69C : PM + RH/T + VOC + NOx + HCHO + CO2 knihovna volitelna
 *
 * Varianta se pozna za behu z prikazu "Get Product Name" (0xD014) - ten je
 * v cele rade stejny, takze se ptat muze kterakoli trida. Volitelne varianty
 * staci nainstalovat ve Spravci knihoven a skec znovu prelozit.
 * (SEN60 a SEN62 podporovane nejsou - SEN60 ma jinou adresu i sadu prikazu
 * a pro SEN62 Sensirion knihovnu nedodava.)
 *
 * VOC a NOx se zamerne NECTOU. Jejich index pocita adaptivni algoritmus, ktery
 * potrebuje bezet nepretrzite hodiny az dny. Deska se mezi merenimi kompletne
 * odpojuje od napajeni, takze by algoritmus vzdy startoval od nuly a vracel
 * jen nesmyslnou konstantu kolem 100. Ze stejneho duvodu neni pouzitelne HCHO
 * u SEN68/SEN69C - datasheet mu dava 60 s od zapnuti, nez zacne hlasit cislo.
 *
 * DVA INTERVALY: teplota, vlhkost a tlak se meri v zakladnim intervalu (int),
 * prach a CO2 v jeho nasobku (senmult, 1x az 4x). Duvodem je spotreba - viz
 * nize.
 *
 * SPOTREBA: SEN6x ma ventilator, ktery nejde vypnout, a bezici cidlo bere
 * 75-110 mA (SEN66 typicky 90 mA). Proto se cidlo napaji jen po dobu mereni
 * pres spinac uSup (GPIO47) a ESP32 po dobu zahrivani spi v light-sleep.
 * Datasheet zada ~30 s behu, nez je PM platne. Pri intervalu 5 min to znamena
 * radove 200 mAh/den - viz varovani v servisu i na webu.
 *
 * Z dostupnych velicin se kresli az 4 grafy. Vyber je automaticky podle
 * priority, nebo rucne prikazem  ch=co2,pm25,temp  v servisnim rezimu.
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
 *           QRCode (Richard Moore) - QR kod pro pripojeni k hotspotu,
 *           Sensirion I2C SEN66 + SEN63C (+ volitelne SEN65 / SEN68 / SEN69C),
 *           Sensirion Core - zavislost predchozich
 * SHT4x ma vlastni ovladac primo v tomto souboru, protoze Adafruit_SHT4x umi
 * jen adresu 0x44 a SHT40 se vyrabi i pro 0x45 a 0x46.
 * Board: "ESP32S3 Dev Module", Flash 16MB, PSRAM: Disabled (neni potreba).
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_sleep.h>

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
// VERZE FIRMWARU
// Jedna definice, kterou vidi displej, servisni rezim, hotspot i webova
// konfigurace. Datum se bere z prekladu, takze ho neni treba rucne hlidat.
//
// Funkce fwBuildDate() a fwVersionFull() jsou az za definicemi typu -
// duvod viz varovani v sekci VELICINY A CIDLA.
// ============================================================================
#define FW_NAME     "MeteoEink426"
#define FW_VERSION  "4.3.0"

// ============================================================================
// SYMBOLY, KTERE FONTY ADAFRUIT GFX NEMAJI
// ============================================================================
// Fonty z Adafruit_GFX pokryvaji jen ASCII 0x20-0x7E, takze v nich chybi
// stupen, mikro i horni index 3. Misto pridavani celeho dalsiho fontu je
// dokresluje richPrint() z primitiv - viz sekce TEXT SE SYMBOLY.
//
// V retezcich se zapisuji OSMICKOVE, ne hexadecimalne: "\xB0C" by prekladac
// precetl jako jeden znak 0xB0C (hex escape je "hladovy"), kdezto osmickovy
// escape bere nejvyse tri cislice, takze "\260C" je spravne stupen + C.
//
// Definice jsou takhle vysoko schvalne: pouziva je uz qUnit() v sekci
// NAZVY VELICIN, dlouho pred vlastnim vykreslovanim.
// ============================================================================
#define SYM_DEG "\260"              // ° stupen
#define SYM_MU  "\265"              // µ mikro
#define SYM_CUB "\263"              // ³ horni index 3
#define SYM_DOT "\267"              // · oddelovac

#define U_DEGC  SYM_DEG "C"          // °C
#define U_UGM3  SYM_MU "g/m" SYM_CUB // µg/m³

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
#define MAX_CHANNELS      4        // displej i historie zvladnou 4 grafy
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

// --- SEN6x ------------------------------------------------------------------
#define SEN_WARM_LO       5        // dolni mez doby zahrivani [s]
#define SEN_WARM_HI       120      // horni mez doby zahrivani [s]
#define DEF_SEN_WARM      30       // datasheet: PM je platne po ~30 s
#define SEN_MULT_HI       4        // nasobek zakladniho intervalu (1x az 4x)
#define SEN_READ_TIMEOUT  5000UL   // cekani na priznak data-ready po zahrati
#define SEN_VBAT_MIN      3.45f    // pod tim SEN6x radeji vubec nespoustet
                                   // (90 mA odber by desku shodil do brownoutu)
#define SEN_CLEAN_HI      90       // automaticke cisteni nejvyse jednou za 90 dni
#define DEF_SEN_CLEAN     7        // Sensirion doporucuje tydne az mesicne
#define I2C_HZ_SEN6X      100000UL // datasheet SEN6x: max 100 kbit/s

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
// !!! POZOR PRI UPRAVACH !!!
// Arduino IDE si samo generuje prototypy vsech funkci a vklada je do souboru
// TESNE PRED PRVNI DEFINICI FUNKCE. Kdyby se nekterý typ pouzity v hlavicce
// funkce (Quantity, Reading, Detected, SenKind, SenRaw, ...) definoval az za
// touto prvni funkci, prototyp by ho jeste nevidel a preklad by spadl na
// hromade hlasek "'Quantity' was not declared in this scope".
//
// PRAVIDLO: v tomto souboru nesmi byt zadna definice funkce driv nez tenhle
// blok typu. Prvni funkce je az cfgDefaults() v sekci KONFIGURACE.
// (Cistym g++ se to neprojevi - chyba vznikne az v prostredi Arduino IDE.)
// ============================================================================
// Q_TEMP a Q_HUM jsou "hlavni" teplota a vlhkost - tedy ta, kterou vybere
// prioritni poradi nebo rucne zvoleny zdroj (tsrc / hsrc). Krome nich ma
// KAZDE cidlo jeste svou vlastni velicinu, takze jde do grafu dat treba
// teplotu ze SHT40 a ze SEN63C zaroven a porovnat je.
//
// Q_TEMP2 je historicky nazev pro teplotu z DS18B20 na kabelu; zustava,
// protoze pod nim je na displeji i v CSV.
enum Quantity : uint8_t {
  Q_NONE = 0, Q_TEMP, Q_HUM, Q_CO2, Q_PRESS, Q_TEMP2, Q_VBAT, Q_PM25, Q_PM10,
  // teploty jednotlivych cidel
  Q_T_SHT, Q_T_SEN, Q_T_SCD, Q_T_BME,
  // vlhkosti jednotlivych cidel
  Q_H_SHT, Q_H_SEN, Q_H_SCD, Q_H_BME
};
#define Q_LAST Q_H_BME              // pojistka pri validaci konfigurace

enum BoschType : uint8_t { BOSCH_NONE = 0, BOSCH_BME280, BOSCH_BMP280 };

// Varianta pripojeneho cidla Sensirion SEN6x. Pouziva ji hlavicka nekolika
// funkci v sekci SENSIRION SEN6x, proto musi byt tady nahore.
enum SenKind : uint8_t {
  SEN_NONE = 0, SEN_66, SEN_63C, SEN_65, SEN_68, SEN_69C
};

// Surova mereni ze SEN6x ve tvaru, ktery vraci kterakoli varianta.
struct SenRaw {
  uint16_t pm1 = 0xFFFF, pm25 = 0xFFFF, pm4 = 0xFFFF, pm10 = 0xFFFF;
  int16_t  rh = 0x7FFF, t = 0x7FFF;
  uint16_t co2 = 0xFFFF, hcho = 0xFFFF;
};

// ----------------------------------------------------------------------------
// ZDROJE TEPLOTY A VLHKOSTI
// Teplotu umi hlasit az pet cidel najednou a kazde se myli jinak: SHT40 se
// ohriva od desky, SEN6x od vlastniho ventilatoru, SCD41 od vyhrivaneho
// mericiho clanku, cip barometru od pouzdra a DS18B20 na kabelu se nemyli
// skoro vubec. Jeden spolecny offset je proto k nicemu - kazdy zdroj ma svuj.
// Poradi v enumu je zaroven prioritou pri skladani vysledku.
// ----------------------------------------------------------------------------
enum TempSrc : uint8_t {
  TSRC_SHT = 0, TSRC_SEN, TSRC_SCD, TSRC_BOSCH, TSRC_DS, TSRC_COUNT
};
enum HumSrc : uint8_t {
  HSRC_SHT = 0, HSRC_SEN, HSRC_SCD, HSRC_BME, HSRC_COUNT
};

struct Detected {
  bool      scd41   = false;
  bool      ds18b20 = false;
  bool      sen6x   = false;
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

// ----------------------------------------------------------------------------
// Namerene hodnoty
//
// Teplotu i vlhkost umi hlasit vic cidel najednou (SHT40 na desce, SEN6x,
// SCD41, BME280). Kazdy zdroj ma proto vlastni pole a teprve na konci mereni
// se z nich podle pevneho poradi slozi vysledne temp / hum. Diky tomu je
// v servisu i na webu videt, ODKUD hodnota je, a nemuze se stat, ze by cidlo
// pripojene pozdeji tise prepsalo presnejsi udaj.
//
// Poradi (od nejlepsiho):
//   teplota:  SHT40 -> SEN6x -> SCD41 -> BME280
//   vlhkost:  SHT40 -> SEN6x -> SCD41 -> BME280
//   tlak:     jen BME280 / BMP280 (SEN6x ani SCD41 tlak nemeri)
//   teplota2: VYHRADNE DS18B20 (cidlo na kabelu)
//
// SHT40 je prvni proto, ze sedi na desce mimo proud ventilatoru SEN6x
// a mimo vlastni ohrev CO2 cidla. Navic se cte pri KAZDEM mereni, kdezto
// SEN6x jen kazde N-te (senMult), takze by teplota z nej byla schodovita.
// ----------------------------------------------------------------------------
struct Reading {
  float temp = NAN, hum = NAN, co2 = NAN, press = NAN;
  float temp2 = NAN;                // VYHRADNE z DS18B20
  float vbat = NAN;                 // napeti baterie [V] - take grafovatelne

  // syrove hodnoty jednotlivych zdroju (pred slozenim)
  float tSht  = NAN, hSht  = NAN;   // SHT40 na desce
  float tSen  = NAN, hSen  = NAN;   // RH/T uvnitr SEN6x
  float tScd  = NAN, hScd  = NAN;   // SCD41
  float tBosch = NAN, hBosch = NAN; // BME280 (BMP280 vlhkost nema)
  float tDs   = NAN;                // DS18B20 na kabelu (syrove)
  float co2Scd = NAN, co2Sen = NAN;

  float pressRaw = NAN;             // tlak tak, jak ho cidlo vidi [hPa]
                                    // (BEZ prepoctu na hladinu more!)
  float pm1 = NAN, pm25 = NAN, pm4 = NAN, pm10 = NAN;   // SEN6x [ug/m3]
  float hcho = NAN;                 // SEN68/SEN69C [ppb] - jen do vypisu

  const char *srcTemp  = "-";       // kdo nakonec dodal teplotu
  const char *srcHum   = "-";
  const char *srcCo2   = "-";
  bool  senFresh = false;           // bezelo v tomto cyklu mereni SEN6x?
};

// Hodnota jedne veliciny z mereni; definice je az v sekci NAZVY VELICIN,
// ale historie ji potrebuje driv.
float qValue(const Reading &r, Quantity q);

// ============================================================================
// KONFIGURACE (NVS)
// ============================================================================
// "MEK7" - v4 pridala polozky pro SEN6x, v4.1 rozdelila offsety podle cidel
// a v4.2 pribyla volba hlavniho zdroje teploty a vlhkosti. Struktura se tim
// zmenila, takze se stara konfigurace zamerne ignoruje a nabehnou vychozi
// hodnoty - cist ji do nove struktury by nacetlo nesmysly.
#define CFG_MAGIC 0x4D454B37u

struct Config {
  uint32_t magic;
  float    tOff[TSRC_COUNT];       // offset teploty pro kazdy zdroj [degC]
  float    hOff[HSRC_COUNT];       // offset vlhkosti pro kazdy zdroj [%RH]
  float    pressOff;
  float    altitude;
  uint8_t  scdAsc;
  uint8_t  intervalMin;
  uint8_t  chAuto;                 // 1 = automaticky vyber kanalu
  uint8_t  chSel[MAX_CHANNELS];    // rucni vyber (Quantity)
  char     apPass[13];             // heslo WiFi hotspotu (8-12 znaku)
  uint8_t  senWarmS;               // doba behu SEN6x pred odectem [s]
  uint8_t  senMult;                // nasobek intervalu pro PM a CO2 (1x az 4x)
  uint8_t  lightSleep;             // 1 = behem zahrivani uspat ESP32
  uint8_t  senCleanDays;           // automaticke cisteni ventilatoru (0 = vyp)
  // Ktere cidlo dodava hlavni teplotu a vlhkost. TSRC_COUNT / HSRC_COUNT
  // znamena "automaticky", tedy podle prioritniho poradi.
  uint8_t  tSrcPref;
  uint8_t  hSrcPref;
};
Config  cfg;
int16_t pendingCo2Ref = -1;        // jednorazovy pozadavek, neuklada se
bool    pendingFanClean = false;   // jednorazove cisteni ventilatoru SEN6x

// ----------------------------------------------------------------------------
// Verze firmwaru
// Tohle je PRVNI FUNKCE v souboru - viz varovani v sekci VELICINY A CIDLA.
// Nic pred ni definovat, jinak se prototypy vlozi pred definice typu.
// ----------------------------------------------------------------------------

// __DATE__ ma tvar "Aug 25 2026" (u jednociferneho dne "Aug  5 2026").
// Prevedeme na ISO 2026-08-25, at se to dobre radi a vsude vypada stejne.
const char* fwBuildDate() {
  static char iso[11] = { 0 };
  if (iso[0]) return iso;
  const char *d = __DATE__;
  static const char *MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon3[4] = { d[0], d[1], d[2], 0 };
  const char *p = strstr(MON, mon3);
  int m = p ? (int)((p - MON) / 3 + 1) : 0;
  int day = (d[4] == ' ') ? (d[5] - '0') : ((d[4] - '0') * 10 + (d[5] - '0'));
  snprintf(iso, sizeof(iso), "%c%c%c%c-%02d-%02d", d[7], d[8], d[9], d[10], m, day);
  return iso;
}

// "4.1.0 (2026-08-25)" - jednotny retezec pro vsechny vypisy
const char* fwVersionFull() {
  static char buf[32] = { 0 };
  if (!buf[0]) snprintf(buf, sizeof(buf), "%s (%s)", FW_VERSION, fwBuildDate());
  return buf;
}

void cfgDefaults() {
  cfg.magic       = CFG_MAGIC;
  for (uint8_t i = 0; i < TSRC_COUNT; i++) cfg.tOff[i] = 0.0f;
  for (uint8_t i = 0; i < HSRC_COUNT; i++) cfg.hOff[i] = 0.0f;
  cfg.pressOff    = 0.0f;
  cfg.altitude    = 0.0f;
  // Samokalibrace je VYPNUTA zamerne. Datasheet SCD4x: "for power-cycled
  // single shot operation, ASC is not available in either case." Deska cidlo
  // mezi merenimi odpojuje, takze se algoritmus nikdy nedobere tydenniho
  // minima. Zapnuta jen zabira misto v uvahach a u SEN6x muze podle
  // datasheetu presnost i zhorsit.
  cfg.scdAsc      = 0;
  cfg.intervalMin = DEF_INTERVAL;
  cfg.chAuto      = 1;
  for (uint8_t i = 0; i < MAX_CHANNELS; i++) cfg.chSel[i] = Q_NONE;
  cfg.apPass[0]   = 0;             // prazdne = vygeneruje se pri prvnim pouziti
  cfg.senWarmS      = DEF_SEN_WARM;
  cfg.senMult       = 1;           // PM a CO2 pri kazdem mereni
  cfg.lightSleep    = 1;
  cfg.senCleanDays  = DEF_SEN_CLEAN;
  cfg.tSrcPref      = TSRC_COUNT;  // automaticky
  cfg.hSrcPref      = HSRC_COUNT;
}

// Osetri nesmyslne hodnoty (ochrana proti spatnemu nastaveni).
void cfgSanitize() {
  if (cfg.intervalMin < INTERVAL_MIN_LO || cfg.intervalMin > INTERVAL_MIN_HI)
    cfg.intervalMin = DEF_INTERVAL;
  for (uint8_t i = 0; i < TSRC_COUNT; i++)
    if (!isfinite(cfg.tOff[i]) || fabsf(cfg.tOff[i]) > 20.0f) cfg.tOff[i] = 0;
  for (uint8_t i = 0; i < HSRC_COUNT; i++)
    if (!isfinite(cfg.hOff[i]) || fabsf(cfg.hOff[i]) > 30.0f) cfg.hOff[i] = 0;
  if (!isfinite(cfg.pressOff) || fabsf(cfg.pressOff) > 50.0f)   cfg.pressOff = 0;
  if (!isfinite(cfg.altitude) || cfg.altitude < 0 || cfg.altitude > 4000)
    cfg.altitude = 0;
  cfg.scdAsc = cfg.scdAsc ? 1 : 0;
  cfg.chAuto = cfg.chAuto ? 1 : 0;
  cfg.lightSleep = cfg.lightSleep ? 1 : 0;
  if (cfg.senWarmS < SEN_WARM_LO || cfg.senWarmS > SEN_WARM_HI)
    cfg.senWarmS = DEF_SEN_WARM;
  if (cfg.senMult < 1 || cfg.senMult > SEN_MULT_HI) cfg.senMult = 1;
  if (cfg.senCleanDays > SEN_CLEAN_HI) cfg.senCleanDays = DEF_SEN_CLEAN;
  if (cfg.tSrcPref > TSRC_COUNT) cfg.tSrcPref = TSRC_COUNT;
  if (cfg.hSrcPref > HSRC_COUNT) cfg.hSrcPref = HSRC_COUNT;
  for (uint8_t i = 0; i < MAX_CHANNELS; i++)
    if (cfg.chSel[i] > Q_LAST) cfg.chSel[i] = Q_NONE;
  cfg.apPass[12] = 0;                               // vzdy ukoncene
  size_t pl = strlen(cfg.apPass);
  if (pl > 0 && pl < 8) cfg.apPass[0] = 0;          // kratsi nez WPA2 minimum
}

#define NVS_NS      "meteo"
#define NVS_CFG     "cfg"
#define NVS_HIST    "hist"
#define NVS_SIGPEND "sigp"         // kandidat na novou sestavu
#define NVS_SIGCNT  "sigc"         // kolikrat uz se potvrdil
#define NVS_CLEAN   "clmin"        // minut od posledniho cisteni ventilatoru

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

// Pocitadla pro SEN6x. Jsou v RTC RAM, takze prezijou deep sleep;
// senCleanMin se navic odklada do NVS (prezije i vymenu baterie).
RTC_DATA_ATTR uint16_t senTick     = 0;   // probuzeni, pro nasobek senMult
RTC_DATA_ATTR uint32_t senCleanMin = 0;   // minut od posledniho cisteni

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
    case Q_PM25:
    case Q_PM10:
      // ug/m3 v desetinach - int16 staci do 3276.7 ug/m3, coz je hluboko
      // nad rozsahem cidla (SEN6x konci na 1000 ug/m3).
      s = lroundf(v * 10.0f);
      if (s < 0) s = 0;
      if (s > 30000) s = 30000;
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
    case Q_PM25:
    case Q_PM10:  return s / 10.0f;
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
    float v = qValue(r, channels[c].q);
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
    // Pocitadlo do automatickeho cisteni ventilatoru se veze s historii.
    // Zapis navic nic nestoji a diky nemu cisteni prezije i vymenu baterie.
    prefs.putUInt(NVS_CLEAN, senCleanMin);
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
// SENSIRION SEN6x — tenka vrstva nad oficialnimi knihovnami Sensirion
// ============================================================================
// Cela rada SEN6x mluvi stejnym protokolem na adrese 0x6B a knihovny se lisi
// jen tim, kolik hodnot vrati prikaz pro cteni. Konfiguracni prikazy (start,
// stop, data-ready, jmeno vyrobku, cisteni ventilatoru, offset teploty) maji
// vsechny varianty identicke, jen kazda ve sve vlastni tride.
//
// Detekce proto probiha ve dvou krocich:
//   1) prikazem "Get Product Name" (0xD014) se zeptame, co je pripojeno -
//      tenhle prikaz je u vsech variant stejny, takze staci libovolna trida
//   2) podle vraceneho jmena se dal pracuje s tou spravnou knihovnou
//
// Nova varianta se pridava na tri mistech: #include, vetev v senKindFromName()
// a vetev v senReadRaw(). Nic jineho neni treba menit.
//
// KNIHOVNY (Spravce knihoven Arduino IDE):
//   "Sensirion I2C SEN66"  - povinna, pouziva se i pro detekci
//   "Sensirion I2C SEN63C" - povinna
//   "Sensirion I2C SEN65"  - volitelna
//   "Sensirion I2C SEN68"  - volitelna
//   "Sensirion I2C SEN69C" - volitelna
//   Volitelne staci nainstalovat a skec znovu prelozit, nic dalsiho.
//   Vsechny zavisi na "Sensirion Core".
// ----------------------------------------------------------------------------

#include <SensirionI2cSen66.h>
#include <SensirionI2cSen63c.h>

// Volitelne varianty. Kdyz knihovna nainstalovana neni, cast kodu se proste
// neprelozi a cidlo se nedetekuje - skec se prelozi i tak.
#if __has_include(<SensirionI2cSen65.h>)
  #include <SensirionI2cSen65.h>
  #define HAVE_SEN65 1
#endif
#if __has_include(<SensirionI2cSen68.h>)
  #include <SensirionI2cSen68.h>
  #define HAVE_SEN68 1
#endif
#if __has_include(<SensirionI2cSen69c.h>)
  #include <SensirionI2cSen69c.h>
  #define HAVE_SEN69C 1
#endif

// (enum SenKind a struct SenRaw jsou nahore v sekci VELICINY A CIDLA -
//  Arduino IDE vyzaduje, aby typy z hlavicek funkci byly pred prvni funkci.)

SensirionI2cSen66  sen66;
SensirionI2cSen63c sen63c;
#ifdef HAVE_SEN65
SensirionI2cSen65  sen65;
#endif
#ifdef HAVE_SEN68
SensirionI2cSen68  sen68;
#endif
#ifdef HAVE_SEN69C
SensirionI2cSen69c sen69c;
#endif

SenKind  senKind    = SEN_NONE;
char     senName[16]   = { 0 };        // jmeno hlasene cidlem
char     senSerial[24] = { 0 };
bool     senRunning = false;
uint32_t senStartMs = 0;

// Co dana varianta umi. Vse ostatni je v cele rade stejne.
static inline bool senHasCo2Kind(SenKind k)  { return k == SEN_66 || k == SEN_63C || k == SEN_69C; }
static inline bool senHasGasKind(SenKind k)  { return k == SEN_66 || k == SEN_65 || k == SEN_68 || k == SEN_69C; }
static inline bool senHasHchoKind(SenKind k) { return k == SEN_68 || k == SEN_69C; }

const char* senKindName(SenKind k) {
  switch (k) {
    case SEN_66:  return "SEN66";
    case SEN_63C: return "SEN63C";
    case SEN_65:  return "SEN65";
    case SEN_68:  return "SEN68";
    case SEN_69C: return "SEN69C";
    default:      return "SEN6x";
  }
}

// Prevede jmeno hlasene cidlem na variantu. Neznama jmena skonci jako SEN_NONE
// - radeji nic nez hadat rozlozeni dat, ktere by tise vratilo nesmysly.
static SenKind senKindFromName(const char *n) {
  if (strcasecmp(n, "SEN66")  == 0) return SEN_66;
  if (strcasecmp(n, "SEN63C") == 0) return SEN_63C;
#ifdef HAVE_SEN65
  if (strcasecmp(n, "SEN65")  == 0) return SEN_65;
#endif
#ifdef HAVE_SEN68
  if (strcasecmp(n, "SEN68")  == 0) return SEN_68;
#endif
#ifdef HAVE_SEN69C
  if (strcasecmp(n, "SEN69C") == 0) return SEN_69C;
#endif
  return SEN_NONE;
}

// ---------------------------------------------------------------------------
// Prevody podle datasheetu.
// Neplatnou hodnotu cidlo hlasi 0xFFFF (unsigned) nebo 0x7FFF (signed).
// Knihovni readMeasuredValues() vraci float bez teto kontroly, takze by z
// "neplatno" udelal 6553.5 ug/m3. Proto cteme celociselnou variantu a prevod
// si delame sami - pak je z neplatne hodnoty poctive NAN.
// ---------------------------------------------------------------------------
static float senPm(uint16_t v)   { return (v == 0xFFFF) ? NAN : v / 10.0f; }
static float senRh(int16_t v)    { return (v == 0x7FFF) ? NAN : v / 100.0f; }
static float senT(int16_t v)     { return (v == 0x7FFF) ? NAN : v / 200.0f; }
static float senCo2(uint16_t v)  { return (v == 0xFFFF || v == 0x7FFF) ? NAN : (float)v; }
static float senHcho(uint16_t v) { return (v == 0xFFFF) ? NAN : v / 10.0f; }

// Jedine misto, kde se varianty opravdu lisi: kolik hodnot prikaz vrati.
// (SenRaw je definovana nahore v sekci VELICINY A CIDLA.)
// VOC a NOx se ctou jen proto, ze je knihovna vyzaduje jako parametry -
// dal se nikam nepredavaji (viz vysvetleni v hlavicce souboru).
static bool senReadRaw(SenRaw &o) {
  int16_t err = 1;
  int16_t voc = 0, nox = 0;
  // SEN63C a SEN69C hlasi CO2 jako int16, SEN66 jako uint16. Pretypovavat
  // reference by bylo osklive, tak si drzime obe a slozime az na konci.
  int16_t co2Signed = 0x7FFF;
  switch (senKind) {
    case SEN_66:
      err = sen66.readMeasuredValuesAsIntegers(o.pm1, o.pm25, o.pm4, o.pm10,
                                               o.rh, o.t, voc, nox, o.co2);
      break;
    case SEN_63C:
      err = sen63c.readMeasuredValuesAsIntegers(o.pm1, o.pm25, o.pm4, o.pm10,
                                                o.rh, o.t, co2Signed);
      o.co2 = (uint16_t)co2Signed;
      break;
#ifdef HAVE_SEN65
    case SEN_65:
      err = sen65.readMeasuredValuesAsIntegers(o.pm1, o.pm25, o.pm4, o.pm10,
                                               o.rh, o.t, voc, nox);
      break;
#endif
#ifdef HAVE_SEN68
    case SEN_68:
      err = sen68.readMeasuredValuesAsIntegers(o.pm1, o.pm25, o.pm4, o.pm10,
                                               o.rh, o.t, voc, nox, o.hcho);
      break;
#endif
#ifdef HAVE_SEN69C
    case SEN_69C:
      err = sen69c.readMeasuredValuesAsIntegers(o.pm1, o.pm25, o.pm4, o.pm10,
                                                o.rh, o.t, voc, nox, o.hcho,
                                                co2Signed);
      o.co2 = (uint16_t)co2Signed;
      break;
#endif
    default: return false;
  }
  (void)voc; (void)nox;
  return err == 0;
}

// --- ostatni prikazy: u vsech variant stejne, jen v jine tride --------------
// Makro jen proto, aby se sestnact temer stejnych switchu neopisovalo rucne.
// Volitelne varianty musi byt v samostatnych makrech - #ifdef uvnitr makra
// prekladac nepovoluje.
#ifdef HAVE_SEN65
  #define SEN_CASE_65(call)  case SEN_65:  return sen65.call;
#else
  #define SEN_CASE_65(call)
#endif
#ifdef HAVE_SEN68
  #define SEN_CASE_68(call)  case SEN_68:  return sen68.call;
#else
  #define SEN_CASE_68(call)
#endif
#ifdef HAVE_SEN69C
  #define SEN_CASE_69C(call) case SEN_69C: return sen69c.call;
#else
  #define SEN_CASE_69C(call)
#endif

#define SEN_DISPATCH(call, fallback)          \
  switch (senKind) {                          \
    case SEN_66:  return sen66.call;          \
    case SEN_63C: return sen63c.call;         \
    SEN_CASE_65(call)                         \
    SEN_CASE_68(call)                         \
    SEN_CASE_69C(call)                        \
    default: return (fallback);               \
  }

static int16_t senStartCmd()  { SEN_DISPATCH(startContinuousMeasurement(), -1); }
static int16_t senStopCmd()   { SEN_DISPATCH(stopMeasurement(), -1); }
static int16_t senCleanCmd()  { SEN_DISPATCH(startFanCleaning(), -1); }
static int16_t senReadyCmd(uint8_t &pad, bool &rdy) { SEN_DISPATCH(getDataReady(pad, rdy), -1); }
static int16_t senSerialCmd(int8_t *b, uint16_t n)  { SEN_DISPATCH(getSerialNumber(b, n), -1); }

// Nastaveni CO2 existuji jen u variant, ktere CO2 meri. Ostatni tridy je
// nemaji ani deklarovane, takze je nesmi videt ani prekladac.
static int16_t senSetAmbientPressure(uint16_t hPa) {
  switch (senKind) {
    case SEN_66:  return sen66.setAmbientPressure(hPa);
    case SEN_63C: return sen63c.setAmbientPressure(hPa);
#ifdef HAVE_SEN69C
    case SEN_69C: return sen69c.setAmbientPressure(hPa);
#endif
    default: return -1;
  }
}
static int16_t senSetAltitude(uint16_t m) {
  switch (senKind) {
    case SEN_66:  return sen66.setSensorAltitude(m);
    case SEN_63C: return sen63c.setSensorAltitude(m);
#ifdef HAVE_SEN69C
    case SEN_69C: return sen69c.setSensorAltitude(m);
#endif
    default: return -1;
  }
}
static int16_t senSetAsc(uint16_t on) {
  switch (senKind) {
    case SEN_66:  return sen66.setCo2SensorAutomaticSelfCalibration(on);
    case SEN_63C: return sen63c.setCo2SensorAutomaticSelfCalibration(on);
#ifdef HAVE_SEN69C
    case SEN_69C: return sen69c.setCo2SensorAutomaticSelfCalibration(on);
#endif
    default: return -1;
  }
}
static int16_t senFrcCmd(uint16_t target, uint16_t &correction) {
  switch (senKind) {
    case SEN_66:  return sen66.performForcedCo2Recalibration(target, correction);
    case SEN_63C: return sen63c.performForcedCo2Recalibration(target, correction);
#ifdef HAVE_SEN69C
    case SEN_69C: return sen69c.performForcedCo2Recalibration(target, correction);
#endif
    default: return -1;
  }
}

// Stavove bity. Kazda varianta ma vlastni typ unie, ale rozlozeni bitu je
// spolecne, tak si vezmeme jen ciselnou hodnotu.
static bool senStatusRaw(uint32_t &out) {
  switch (senKind) {
    case SEN_66:  { SEN66DeviceStatus s;  if (sen66.readDeviceStatus(s))  return false; out = s.value; return true; }
    case SEN_63C: { SEN63CDeviceStatus s; if (sen63c.readDeviceStatus(s)) return false; out = s.value; return true; }
#ifdef HAVE_SEN65
    case SEN_65:  { SEN65DeviceStatus s;  if (sen65.readDeviceStatus(s))  return false; out = s.value; return true; }
#endif
#ifdef HAVE_SEN68
    case SEN_68:  { SEN68DeviceStatus s;  if (sen68.readDeviceStatus(s))  return false; out = s.value; return true; }
#endif
#ifdef HAVE_SEN69C
    case SEN_69C: { SEN69CDeviceStatus s; if (sen69c.readDeviceStatus(s)) return false; out = s.value; return true; }
#endif
    default: return false;
  }
}

// ---------------------------------------------------------------------------
// Detekce
// ---------------------------------------------------------------------------
bool sen6xDetect() {
  senKind = SEN_NONE;
  senName[0] = 0;
  senSerial[0] = 0;

  // Rychla kontrola, jestli na adrese vubec neco je - usetri to cekani
  // v knihovne, kdyz zadne cidlo pripojene neni.
  Wire.beginTransmission(SEN66_I2C_ADDR_6B);
  if (Wire.endTransmission() != 0) return false;

  // Prikaz "Get Product Name" je u cele rady stejny, takze je jedno, kterou
  // tridou se zeptame. Bereme SEN66 jako univerzalni "tazatele".
  sen66.begin(Wire, SEN66_I2C_ADDR_6B);

  int8_t name[32];
  for (uint8_t attempt = 0; attempt < 2 && senKind == SEN_NONE; attempt++) {
    if (attempt == 1) {
      // Napoprve to neslo - cidlo nejspis zustalo v mereni (reset ESP bez
      // odpojeni napajeni). Knihovni stopMeasurement() uz v sobe ma
      // predepsanou pauzu 1000 ms.
      sen66.stopMeasurement();
    }
    memset(name, 0, sizeof(name));
    if (sen66.getProductName(name, sizeof(name)) != 0) continue;
    name[sizeof(name) - 1] = 0;
    snprintf(senName, sizeof(senName), "%s", (const char *)name);
    // orez mezer na konci
    for (int i = (int)strlen(senName) - 1; i >= 0 && senName[i] == ' '; i--) senName[i] = 0;
    senKind = senKindFromName(senName);
  }

  if (senKind == SEN_NONE) {
    if (senName[0])
      Serial.printf("Na 0x6B je '%s', ale chybi jeho knihovna nebo ho firmware nezna.\n",
                    senName);
    return false;
  }

  // Od ted uz mluvime s tou spravnou tridou.
  switch (senKind) {
    case SEN_63C: sen63c.begin(Wire, SEN63C_I2C_ADDR_6B); break;
#ifdef HAVE_SEN65
    case SEN_65:  sen65.begin(Wire, SEN65_I2C_ADDR_6B);   break;
#endif
#ifdef HAVE_SEN68
    case SEN_68:  sen68.begin(Wire, SEN68_I2C_ADDR_6B);   break;
#endif
#ifdef HAVE_SEN69C
    case SEN_69C: sen69c.begin(Wire, SEN69C_I2C_ADDR_6B); break;
#endif
    default: break;                       // SEN66 uz zacaty je
  }

  int8_t sn[32];
  memset(sn, 0, sizeof(sn));
  if (senSerialCmd(sn, sizeof(sn)) == 0) {
    sn[sizeof(sn) - 1] = 0;
    snprintf(senSerial, sizeof(senSerial), "%s", (const char *)sn);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Rizeni mereni
// ---------------------------------------------------------------------------
bool sen6xStart() {
  if (senKind == SEN_NONE) return false;
  if (senStartCmd() != 0) return false;
  senRunning = true;
  senStartMs = millis();
  return true;
}

void sen6xStop() {
  if (senKind == SEN_NONE || !senRunning) return;
  senStopCmd();                     // knihovna sama ceka predepsanych 1400 ms
  senRunning = false;
}

bool sen6xDataReady() {
  uint8_t pad = 0;
  bool rdy = false;
  if (senReadyCmd(pad, rdy) != 0) return false;
  return rdy;
}

// Precte hodnoty do Reading. VOC/NOx se zamerne zahazuji (viz hlavicka).
// CO2 se prevezme jen tehdy, kdyz v sestave neni SCD41 - ten ma prednost.
bool sen6xReadValues(Reading &r) {
  if (senKind == SEN_NONE || !senRunning) return false;
  SenRaw raw;
  if (!senReadRaw(raw)) return false;

  r.pm1  = senPm(raw.pm1);
  r.pm25 = senPm(raw.pm25);
  r.pm4  = senPm(raw.pm4);
  r.pm10 = senPm(raw.pm10);
  r.hSen = senRh(raw.rh);
  r.tSen = senT(raw.t);
  if (senHasCo2Kind(senKind) && !det.scd41) r.co2Sen = senCo2(raw.co2);
  if (senHasHchoKind(senKind))              r.hcho   = senHcho(raw.hcho);
  return true;
}

// Nastaveni, ktera cidlo prijima jen kdyz NEMERI.
// pressHpa = skutecny (staniceni) tlak z BME/BMP280, NAN kdyz barometr neni.
void sen6xApplyConfig(float pressHpa) {
  if (senKind == SEN_NONE) return;
  if (!senHasCo2Kind(senKind)) return;     // ostatni varianty tohle nemaji

  // Tlak zpresnuje vypocet CO2 vic nez nadmorska vyska, protoze pocita
  // i s pocasim. Kdyz je na desce barometr, ma prednost; jinak zbyde vyska.
  // POZOR: musi to byt tlak, jak ho cidlo opravdu vidi - NE prepocteny
  // na hladinu more.
  if (!isnan(pressHpa) && pressHpa > 300.0f && pressHpa < 1200.0f)
    senSetAmbientPressure((uint16_t)lroundf(pressHpa));
  else if (cfg.altitude > 0)
    senSetAltitude((uint16_t)cfg.altitude);

  senSetAsc(cfg.scdAsc ? 1 : 0);
}

bool sen6xForcedCo2(uint16_t targetPpm, int32_t &correction) {
  if (!senHasCo2Kind(senKind)) return false;
  uint16_t raw = 0;
  if (senFrcCmd(targetPpm, raw) != 0) return false;
  if (raw == 0xFFFF) return false;         // cidlo kalibraci odmitlo
  correction = (int32_t)raw - 32768;       // datasheet: hodnota je posunuta
  return true;
}

// Vraci text pro vypis stavovych bitu (rozlozeni je spolecne cele rade).
const char* sen6xStatusText() {
  static char buf[96];
  buf[0] = 0;
  uint32_t st = 0;
  if (!senStatusRaw(st)) { strcpy(buf, "nelze precist"); return buf; }
  if (st == 0) { strcpy(buf, "OK"); return buf; }
  if (st & (1UL << 21)) strcat(buf, "otacky_ventilatoru ");
  if (st & (1UL << 4))  strcat(buf, "ventilator ");
  if (st & (1UL << 6))  strcat(buf, "RH/T ");
  if (st & (1UL << 7))  strcat(buf, "plyn ");
  if (st & (1UL << 9))  strcat(buf, "CO2_2 ");
  if (st & (1UL << 10)) strcat(buf, "HCHO ");
  if (st & (1UL << 11)) strcat(buf, "PM ");
  if (st & (1UL << 12)) strcat(buf, "CO2 ");
  if (!buf[0]) snprintf(buf, sizeof(buf), "bity 0x%08lX", (unsigned long)st);
  return buf;
}

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
// SEN6x se hleda jen jednou: jeho detekce trva pres dve sekundy (nutna pauza
// po prikazu stop) a opakovat ji by zbytecne prodluzovalo dobu vzhuru.
void detectSensors() {
  // SHT40 se zkousi dvakrat. Je to jedine cidlo, jehoz vypadek meni VYZNAM
  // veliciny "temp" (prepnul by se hlavni zdroj na jine cidlo), takze jedno
  // zaruseni na I2C nesmi rozhodnout.
  shtAddr = sht4xDetect();          // 0x44 / 0x45 / 0x46
  if (!shtAddr) { delay(30); shtAddr = sht4xDetect(); }
  det.sen6x = sen6xDetect();        // 0x6B, varianta podle jmena vyrobku
  Detected a = detectOnce();
  delay(30);
  Detected b = detectOnce();
  // sjednoceni: co bylo videt aspon jednou, povazujeme za pritomne
  det.scd41   = a.scd41   || b.scd41;
  det.ds18b20 = a.ds18b20 || b.ds18b20;
  // SEN6x umi CO2 sam, dva zdroje CO2 by si v grafu prekazely.
  // Prednost ma SCD41 - je presnejsi a levnejsi na energii.
  if (det.sen6x && det.scd41)
    Serial.println("POZOR: SCD41 i SEN6x meri CO2. Pouzije se SCD41,"
                   " CO2 ze SEN6x se ignoruje.");
  if (a.bosch != BOSCH_NONE)      { det.bosch = a.bosch; det.boschAddr = a.boschAddr; det.boschChip = a.boschChip; }
  else if (b.bosch != BOSCH_NONE) { det.bosch = b.bosch; det.boschAddr = b.boschAddr; det.boschChip = b.boschChip; }
  else                            { det.bosch = BOSCH_NONE; }
}

// Umi SEN6x v teto variante CO2? (SEN63C, SEN66, SEN69C)
static inline bool senHasCo2() { return det.sen6x && senHasCo2Kind(senKind); }

const char* boschName() {
  if (det.bosch == BOSCH_BME280) return "BME280";
  if (det.bosch == BOSCH_BMP280) return "BMP280";
  return "";
}

// ----------------------------------------------------------------------------
// KTERE CIDLO DODAVA KTEROU VELICINU
// Teplotu a vlhkost umi hlasit az ctyri cidla najednou. Poradi je pevne
// (viz komentar u struktury Reading) a tyhle funkce ho vraci v podobe, kterou
// jde vypsat do servisu i posilat do webove konfigurace. Pocitaji se
// z DETEKOVANE sestavy, takze davaji odpoved i pred prvnim merenim.
// ----------------------------------------------------------------------------
// --- zdroje teploty -------------------------------------------------------
// Klic je to, co se pise v prikazu (toff.sht), jmeno to, co vidi uzivatel.
const char* tsrcKey(uint8_t i) {
  switch (i) {
    case TSRC_SHT:   return "sht";
    case TSRC_SEN:   return "sen";
    case TSRC_SCD:   return "scd";
    case TSRC_BOSCH: return "bme";
    case TSRC_DS:    return "ds";
    default:         return "?";
  }
}
const char* tsrcName(uint8_t i) {
  switch (i) {
    case TSRC_SHT:   return "SHT40";
    case TSRC_SEN:   return det.sen6x ? senKindName(senKind) : "SEN6x";
    case TSRC_SCD:   return "SCD41";
    case TSRC_BOSCH: return det.bosch != BOSCH_NONE ? boschName() : "BME280";
    case TSRC_DS:    return "DS18B20";
    default:         return "?";
  }
}
bool tsrcAvailable(uint8_t i) {
  switch (i) {
    case TSRC_SHT:   return shtAddr != 0;
    case TSRC_SEN:   return det.sen6x;
    case TSRC_SCD:   return det.scd41;
    case TSRC_BOSCH: return det.bosch != BOSCH_NONE;
    case TSRC_DS:    return det.ds18b20;
    default:         return false;
  }
}

// --- zdroje vlhkosti ------------------------------------------------------
const char* hsrcKey(uint8_t i) {
  switch (i) {
    case HSRC_SHT: return "sht";
    case HSRC_SEN: return "sen";
    case HSRC_SCD: return "scd";
    case HSRC_BME: return "bme";
    default:       return "?";
  }
}
const char* hsrcName(uint8_t i) {
  switch (i) {
    case HSRC_SHT: return "SHT40";
    case HSRC_SEN: return det.sen6x ? senKindName(senKind) : "SEN6x";
    case HSRC_SCD: return "SCD41";
    // Vlhkost umi jen BME280, ale nazev bereme z detekce - at nehlasime
    // "BME280 neni pripojene" na desce, kde je BMP280.
    case HSRC_BME: return det.bosch != BOSCH_NONE ? boschName() : "BME280";
    default:       return "?";
  }
}
bool hsrcAvailable(uint8_t i) {
  switch (i) {
    case HSRC_SHT: return shtAddr != 0;
    case HSRC_SEN: return det.sen6x;
    case HSRC_SCD: return det.scd41;
    // BMP280 vlhkost nemeri, offset by nemel co ovlivnit
    case HSRC_BME: return det.bosch == BOSCH_BME280;
    default:       return false;
  }
}

// Zdroj, ktery hodnotu opravdu dodava.
//
// Kdyz si uzivatel zvolil konkretni cidlo (tsrc / hsrc) a to je pripojene,
// ma prednost - i kdyz je v prioritnim poradi az za jinym. Kdyz zvolene cidlo
// zmizi, rozhodne prioritni poradi, takze stanice nezustane bez teploty.
//
// DS18B20 se do automatickeho vyberu nepocita: sonda na kabelu obvykle meri
// nekde jinde (venku, v akvariu) a jako "teplota" by mátla. Zvolit ji rucne
// ale jde - nekomu se hodi mit hlavni teplotu prave z ni.
uint8_t tsrcPrimary() {
  if (cfg.tSrcPref < TSRC_COUNT && tsrcAvailable(cfg.tSrcPref)) return cfg.tSrcPref;
  for (uint8_t i = 0; i < TSRC_COUNT; i++)
    if (i != TSRC_DS && tsrcAvailable(i)) return i;
  return tsrcAvailable(TSRC_DS) ? (uint8_t)TSRC_DS : (uint8_t)TSRC_COUNT;
}
uint8_t hsrcPrimary() {
  if (cfg.hSrcPref < HSRC_COUNT && hsrcAvailable(cfg.hSrcPref)) return cfg.hSrcPref;
  for (uint8_t i = 0; i < HSRC_COUNT; i++) if (hsrcAvailable(i)) return i;
  return HSRC_COUNT;
}

const char* srcTempName() {
  uint8_t i = tsrcPrimary();
  return (i < TSRC_COUNT) ? tsrcName(i) : "-";
}
const char* srcHumName() {
  uint8_t i = hsrcPrimary();
  return (i < HSRC_COUNT) ? hsrcName(i) : "-";
}
const char* srcCo2Name() {
  if (det.scd41)   return "SCD41";
  if (senHasCo2()) return senKindName(senKind);
  return "-";
}
const char* srcPressName() {
  // Tlak umi jen barometr - SEN6x ani SCD41 ho nemeri. Namerena hodnota
  // se ale posila do SEN6x jako vstup pro kompenzaci CO2 (setAmbientPressure).
  // SCD41 dostava jen nadmorskou vysku: jeho cteni v doMeasurement() probiha
  // driv nez barometr, takze by tlak v tu chvili jeste nebyl k dispozici.
  if (det.bosch != BOSCH_NONE) return boschName();
  return "-";
}

bool qAvailable(Quantity q) {
  switch (q) {
    // Hlavni teplota a vlhkost existuji, dokud je aspon jeden zdroj.
    case Q_TEMP:  return tsrcPrimary() < TSRC_COUNT;
    case Q_HUM:   return hsrcPrimary() < HSRC_COUNT;
    case Q_CO2:   return det.scd41 || senHasCo2();
    case Q_PRESS: return det.bosch != BOSCH_NONE;
    case Q_TEMP2: return det.ds18b20;
    case Q_VBAT:  return true;                    // delic je vzdy na desce
    case Q_PM25:
    case Q_PM10:  return det.sen6x;
    // Veliciny konkretnich cidel jsou k dispozici, kdyz je cidlo pripojene.
    case Q_T_SHT: return tsrcAvailable(TSRC_SHT);
    case Q_T_SEN: return tsrcAvailable(TSRC_SEN);
    case Q_T_SCD: return tsrcAvailable(TSRC_SCD);
    case Q_T_BME: return tsrcAvailable(TSRC_BOSCH);
    case Q_H_SHT: return hsrcAvailable(HSRC_SHT);
    case Q_H_SEN: return hsrcAvailable(HSRC_SEN);
    case Q_H_SCD: return hsrcAvailable(HSRC_SCD);
    case Q_H_BME: return hsrcAvailable(HSRC_BME);
    default:      return false;
  }
}

// Vsechny veliciny v poradi, v jakem se nabizeji uzivateli. Jedno misto,
// odkud cerpa servisni vypis, JSON hotspotu i obe webove stranky.
static const Quantity Q_ALL[] = {
  Q_TEMP, Q_HUM, Q_CO2, Q_PM25, Q_PM10, Q_PRESS, Q_TEMP2, Q_VBAT,
  Q_T_SHT, Q_T_SEN, Q_T_SCD, Q_T_BME,
  Q_H_SHT, Q_H_SEN, Q_H_SCD, Q_H_BME
};
#define Q_ALL_COUNT (sizeof(Q_ALL) / sizeof(Q_ALL[0]))

// ----------------------------------------------------------------------------
// ALIASY VELICIN
//
// "temp" NENI samostatne mereni - je to jen jine jmeno pro teplotu toho cidla,
// ktere je prave zvolene jako hlavni. Pri tsrc=sen tedy temp a temp.sen nesou
// UPLNE STEJNA CISLA. Kdyby sly obe do grafu, kreslily by se dve totozne
// krivky, zabraly dva ze ctyr slotu a v historii by zbytecne lezela tataz data
// dvakrat. Tohle je jedine misto, kde se ten prevod dela.
// ----------------------------------------------------------------------------
Quantity tsrcQuantity(uint8_t i) {
  switch (i) {
    case TSRC_SHT:   return Q_T_SHT;
    case TSRC_SEN:   return Q_T_SEN;
    case TSRC_SCD:   return Q_T_SCD;
    case TSRC_BOSCH: return Q_T_BME;
    case TSRC_DS:    return Q_TEMP2;     // sonda na kabelu ma vlastni nazev
    default:         return Q_NONE;
  }
}
Quantity hsrcQuantity(uint8_t i) {
  switch (i) {
    case HSRC_SHT: return Q_H_SHT;
    case HSRC_SEN: return Q_H_SEN;
    case HSRC_SCD: return Q_H_SCD;
    case HSRC_BME: return Q_H_BME;
    default:       return Q_NONE;
  }
}

// Na kterou skutecnou velicinu se q mapuje.
Quantity qCanonical(Quantity q) {
  if (q == Q_TEMP) {
    uint8_t i = tsrcPrimary();
    return (i < TSRC_COUNT) ? tsrcQuantity(i) : Q_TEMP;
  }
  if (q == Q_HUM) {
    uint8_t i = hsrcPrimary();
    return (i < HSRC_COUNT) ? hsrcQuantity(i) : Q_HUM;
  }
  return q;
}

// Kolik zdroju je vubec k dispozici - podle toho se rozhoduje, jestli ma smysl
// pripisovat k popisku jmeno cidla. Pri jedinem cidle by to byl jen sum.
uint8_t tsrcCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < TSRC_COUNT; i++) if (tsrcAvailable(i)) n++;
  return n;
}
uint8_t hsrcCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < HSRC_COUNT; i++) if (hsrcAvailable(i)) n++;
  return n;
}

// Kterou velicinu ma smysl NABIZET. Velicina, na kterou zrovna ukazuje "temp"
// nebo "hum", se uz nenabizi zvlast - byla by to tataz vec pod dvema jmeny.
bool qOffered(Quantity q) {
  if (!qAvailable(q)) return false;
  if (q == Q_TEMP || q == Q_HUM) return true;
  if (q == qCanonical(Q_TEMP)) return false;
  if (q == qCanonical(Q_HUM))  return false;
  return true;
}

// Sestavi kanaly: bud rucni vyber, nebo automaticka priorita.
void buildChannels() {
  channelCount = 0;
  auto add = [&](Quantity q) {
    if (channelCount >= MAX_CHANNELS || q == Q_NONE || !qAvailable(q)) return;
    // Bez duplicit - a to i skrytych. "temp" pri tsrc=sen je tataz vec jako
    // "temp.sen", takze by se jinak kreslily dve totozne krivky a zabraly
    // dva ze ctyr slotu.
    Quantity canon = qCanonical(q);
    for (uint8_t i = 0; i < channelCount; i++)
      if (qCanonical(channels[i].q) == canon) return;
    channels[channelCount].q = q;
    channels[channelCount].dashed = false;   // kazdy kanal ma vlastni graf
    channelCount++;
  };

  if (!cfg.chAuto) {
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) add((Quantity)cfg.chSel[i]);
  }
  if (channelCount == 0) {
    // Automaticka priorita: CO2 > PM2.5 > teplota > tlak > 2. teplota > vlhkost.
    // PM2.5 je hned za CO2 - je to hlavni duvod, proc si nekdo SEN6x poridi,
    // a na rozdil od PM10 na nej existuji bezne limity (WHO 15 ug/m3 / 24 h).
    // Externi teplomer je vzdy vedoma volba uzivatele, vlhkost je z cidla
    // na desce k dispozici sama - proto ma temp2 prednost pred hum.
    add(Q_CO2); add(Q_PM25); add(Q_TEMP); add(Q_PRESS); add(Q_TEMP2); add(Q_HUM);
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
  // Nestaci hlidat ROZLOZENI poolu - musi se hlidat i VYZNAM kanalu.
  // Zamena ch=co2,temp za ch=press,hum ma stejny pocet kanalu, ale ulozene
  // vzorky by se nove cetly jako jina velicina s jinym meritkem: z teploty
  // 21,5 by byl tlak 1115 hPa. Se sestnacti velicinami je to bezny pripad,
  // takze si pamatujeme, ktere veliciny v kanalech byly.
  uint16_t before = histPerCh;
  Quantity had[MAX_CHANNELS];
  uint8_t  hadCount = channelCount;
  for (uint8_t i = 0; i < MAX_CHANNELS; i++)
    had[i] = (i < channelCount) ? channels[i].q : Q_NONE;

  buildChannels();

  bool sameSet = (hadCount == channelCount);
  if (sameSet)
    for (uint8_t i = 0; i < channelCount; i++)
      if (had[i] != channels[i].q) { sameSet = false; break; }

  if (histPerCh != before || histPerCh != histSlots || intervalChanged || !sameSet) {
    histClear();
    histEraseNVS();
    Serial.println("Zmenilo se rozlozeni nebo obsah kanalu - zalozena nova historie.");
  }
}

uint32_t buildSignature() {
  uint32_t s = 0x9E3779B1u;
  s = s * 31u + (uint32_t)shtAddr;
  s = s * 31u + (uint32_t)det.scd41;
  s = s * 31u + (uint32_t)det.ds18b20;
  s = s * 31u + (uint32_t)det.bosch;
  // Vymena SEN66 za SEN63C meni sadu velicin, proto varianta patri do podpisu.
  s = s * 31u + (uint32_t)senKind;
  s = s * 31u + (uint32_t)channelCount;
  for (uint8_t c = 0; c < channelCount; c++) s = s * 31u + (uint32_t)channels[c].q;
  s = s * 31u + (uint32_t)cfg.intervalMin;
  s = s * 31u + (uint32_t)histPerCh;
  // Zmena zdroje meni vyznam veliciny "temp" / "hum" v historii - stara data
  // z jineho cidla by se michala s novymi.
  s = s * 31u + (uint32_t)cfg.tSrcPref;
  s = s * 31u + (uint32_t)cfg.hSrcPref;
  return s;
}

// Nazvy detekovanych cidel bez adres - pro vypis na displej pod sebe.
// Vraci pocet zapsanych polozek.
uint8_t sensorNames(const char *out[], uint8_t maxItems) {
  uint8_t n = 0;
  if (shtAddr    && n < maxItems) out[n++] = "SHT4x";
  if (det.sen6x  && n < maxItems) out[n++] = senKindName(senKind);
  if (det.scd41  && n < maxItems) out[n++] = "SCD41";
  if (det.bosch == BOSCH_BME280 && n < maxItems) out[n++] = "BME280";
  if (det.bosch == BOSCH_BMP280 && n < maxItems) out[n++] = "BMP280";
  if (det.ds18b20 && n < maxItems) out[n++] = "DS18B20";
  return n;
}

const char* sensorSummary() {
  static char buf[64];
  buf[0] = 0;
  if (shtAddr) { char t[16]; snprintf(t, sizeof(t), "SHT4x@0x%02X ", shtAddr); strcat(buf, t); }
  if (det.sen6x) { strcat(buf, senKindName(senKind)); strcat(buf, " "); }
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
    case Q_TEMP2: return "temp.ds";     // teplota z DS18B20 na kabelu
    case Q_VBAT:  return "vbat";
    case Q_PM25:  return "pm25";
    case Q_PM10:  return "pm10";
    case Q_T_SHT: return "temp.sht";
    case Q_T_SEN: return "temp.sen";
    case Q_T_SCD: return "temp.scd";
    case Q_T_BME: return "temp.bme";
    case Q_H_SHT: return "hum.sht";
    case Q_H_SEN: return "hum.sen";
    case Q_H_SCD: return "hum.scd";
    case Q_H_BME: return "hum.bme";
    default:      return "-";
  }
}
Quantity qFromKey(const String &s) {
  if (s == "temp")  return Q_TEMP;
  if (s == "hum")   return Q_HUM;
  if (s == "co2")   return Q_CO2;
  if (s == "press") return Q_PRESS;
  // "temp.ds" je prirozenejsi nazev, "temp2" zustava kvuli starym navodum
  if (s == "temp2" || s == "temp.ds") return Q_TEMP2;
  if (s == "vbat")  return Q_VBAT;
  if (s == "pm25" || s == "pm2.5") return Q_PM25;
  if (s == "pm10")  return Q_PM10;
  if (s == "temp.sht") return Q_T_SHT;
  if (s == "temp.sen") return Q_T_SEN;
  if (s == "temp.scd") return Q_T_SCD;
  if (s == "temp.bme") return Q_T_BME;
  if (s == "hum.sht")  return Q_H_SHT;
  if (s == "hum.sen")  return Q_H_SEN;
  if (s == "hum.scd")  return Q_H_SCD;
  if (s == "hum.bme")  return Q_H_BME;
  return Q_NONE;
}

// Popisky velicin jednotlivych cidel se skladaji za behu (jmeno SEN6x zavisi
// na variante, jmeno barometru na cipu). Kruh ctyr bufferu staci: nikde se
// qLabel() nevola vic nez dvakrat v jednom vyrazu.
const char* qLabel(Quantity q) {
  static char buf[4][24];
  static uint8_t next = 0;
  auto composed = [&](const char *what, const char *who) -> const char* {
    char *o = buf[next]; next = (next + 1) & 3;
    snprintf(o, sizeof(buf[0]), "%s %s", what, who);
    return o;
  };
  switch (q) {
    // U hlavni teploty a vlhkosti se pripisuje jmeno cidla, ze ktereho prave
    // jsou. Bez toho by graf "Teplota" pri tsrc=sen ukazoval cisla ze SEN6x,
    // ale tvaril se jako neco jineho nez graf "Teplota SEN66" vedle nej.
    // Pri jedinem zdroji by prilepek jen zabiral misto, tak se vynechava.
    case Q_TEMP:  return (tsrcCount() > 1 && tsrcPrimary() < TSRC_COUNT)
                         ? composed("Teplota", tsrcName(tsrcPrimary())) : "Teplota";
    case Q_HUM:   return (hsrcCount() > 1 && hsrcPrimary() < HSRC_COUNT)
                         ? composed("Vlhkost", hsrcName(hsrcPrimary())) : "Vlhkost";
    // CO2 umi hlasit SCD41 i SEN6x; kdyz jsou oba, at je videt ktery vyhral.
    case Q_CO2:   return (det.scd41 && senHasCo2())
                         ? composed("CO2", srcCo2Name()) : "CO2";
    case Q_PRESS: return "Tlak";
    case Q_TEMP2: return composed("Teplota", tsrcName(TSRC_DS));
    case Q_VBAT:  return "Baterie";
    case Q_PM25:  return "PM2.5";
    case Q_PM10:  return "PM10";
    case Q_T_SHT: return composed("Teplota", tsrcName(TSRC_SHT));
    case Q_T_SEN: return composed("Teplota", tsrcName(TSRC_SEN));
    case Q_T_SCD: return composed("Teplota", tsrcName(TSRC_SCD));
    case Q_T_BME: return composed("Teplota", tsrcName(TSRC_BOSCH));
    case Q_H_SHT: return composed("Vlhkost", hsrcName(HSRC_SHT));
    case Q_H_SEN: return composed("Vlhkost", hsrcName(HSRC_SEN));
    case Q_H_SCD: return composed("Vlhkost", hsrcName(HSRC_SCD));
    case Q_H_BME: return composed("Vlhkost", hsrcName(HSRC_BME));
    default:      return "";
  }
}
const char* qUnit(Quantity q) {
  switch (q) {
    case Q_TEMP:
    case Q_TEMP2:
    case Q_T_SHT:
    case Q_T_SEN:
    case Q_T_SCD:
    case Q_T_BME: return U_DEGC;
    case Q_VBAT:  return "V";
    case Q_HUM:
    case Q_H_SHT:
    case Q_H_SEN:
    case Q_H_SCD:
    case Q_H_BME: return "%";
    case Q_CO2:   return "ppm";
    case Q_PRESS: return "hPa";
    // Stupen, mikro i horni index dokresluje richPrint() - viz sekce
    // TEXT SE SYMBOLY. Na displej se tedy dostane "°C" a "µg/m³".
    case Q_PM25:
    case Q_PM10:  return U_UGM3;
    default:      return "";
  }
}

// Hodnota veliciny z jednoho mereni. Jedine misto, kde se rozhoduje, ktere
// pole struktury Reading ktere velicine odpovida - pouziva to histPush()
// i vypis dostupnych velicin.
float qValue(const Reading &r, Quantity q) {
  switch (q) {
    case Q_TEMP:  return r.temp;
    case Q_HUM:   return r.hum;
    case Q_CO2:   return r.co2;
    case Q_PRESS: return r.press;
    case Q_TEMP2: return r.temp2;
    case Q_VBAT:  return r.vbat;
    case Q_PM25:  return r.pm25;
    case Q_PM10:  return r.pm10;
    // Veliciny jednotlivych cidel jsou syrove hodnoty PLUS jejich vlastni
    // offset - stejne jako kdyz totez cidlo dodava hlavni teplotu.
    case Q_T_SHT: return isnan(r.tSht)   ? NAN : r.tSht   + cfg.tOff[TSRC_SHT];
    case Q_T_SEN: return isnan(r.tSen)   ? NAN : r.tSen   + cfg.tOff[TSRC_SEN];
    case Q_T_SCD: return isnan(r.tScd)   ? NAN : r.tScd   + cfg.tOff[TSRC_SCD];
    case Q_T_BME: return isnan(r.tBosch) ? NAN : r.tBosch + cfg.tOff[TSRC_BOSCH];
    case Q_H_SHT: return isnan(r.hSht)   ? NAN : constrain(r.hSht   + cfg.hOff[HSRC_SHT], 0.0f, 100.0f);
    case Q_H_SEN: return isnan(r.hSen)   ? NAN : constrain(r.hSen   + cfg.hOff[HSRC_SEN], 0.0f, 100.0f);
    case Q_H_SCD: return isnan(r.hScd)   ? NAN : constrain(r.hScd   + cfg.hOff[HSRC_SCD], 0.0f, 100.0f);
    case Q_H_BME: return isnan(r.hBosch) ? NAN : constrain(r.hBosch + cfg.hOff[HSRC_BME], 0.0f, 100.0f);
    default:      return NAN;
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

// CRC-8 Sensirion: polynom 0x31, init 0xFF.
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
  r.tSht = t;
  r.hSht = h;
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

  // DVA single shoty, pouzije se az druhy.
  //
  // Datasheet SCD4x: "After a power cycle, the initial single shot reading
  // should be discarded to maximize accuracy." Deska cidlo mezi merenimi
  // odpojuje od napajeni, takze KAZDE mereni je "po power cyclu" - prvni
  // odectena hodnota je vzdy ta, kterou ma clovek zahodit. Bez toho cidlo
  // hlasi porad skoro totez cislo blizko vychozi kalibrace bez ohledu na
  // skutecny vzduch. Stoji to ~5 s navic, ale jinak je udaj bezcenny.
  bool ready = false;
  for (uint8_t pass = 0; pass < 2; pass++) {
    if (!scd4x.measureSingleShot()) return;
    ready = false;
    unsigned long t0 = millis();
    while (millis() - t0 < SCD_TIMEOUT_MS) {
      delay(100);
      if (scd4x.getDataReadyStatus()) { ready = true; break; }
    }
    if (!ready) return;
    if (!scd4x.readMeasurement()) return;
  }

  uint16_t co2 = scd4x.getCO2();
  if (co2 == 0) return;
  r.co2Scd = co2;
  // Ulozime i teplotu a vlhkost, ale jen jako jeden ze zdroju - o tom,
  // ktery se pouzije, rozhoduje az mergeSources().
  r.tScd = scd4x.getTemperature();
  r.hScd = scd4x.getHumidity();
}

// Prepocet na hladinu more + uzivatelsky offset. POUZIVA SE JEN PRO ZOBRAZENI.
// Kompenzace CO2 v SEN6x i v SCD41 chce naopak tlak, jak ho cidlo skutecne
// vidi - ten je v r.pressRaw.
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
    r.pressRaw = bme.readPressure() / 100.0f;
    r.press    = adjustPressure(r.pressRaw);
    r.hBosch   = bme.readHumidity();
    r.tBosch   = bme.readTemperature();  // NE do temp2 - to patri DS18B20
  } else if (det.bosch == BOSCH_BMP280) {
    // POZOR: Adafruit_BME280 odmita chip ID 0x58, nutna Adafruit_BMP280.
    if (!bmp.begin(det.boschAddr, det.boschChip)) return;
    bmp.setSampling(Adafruit_BMP280::MODE_FORCED,
                    Adafruit_BMP280::SAMPLING_X1,
                    Adafruit_BMP280::SAMPLING_X1,
                    Adafruit_BMP280::FILTER_OFF,
                    Adafruit_BMP280::STANDBY_MS_1);
    bmp.takeForcedMeasurement();
    r.pressRaw = bmp.readPressure() / 100.0f;
    r.press    = adjustPressure(r.pressRaw);
    r.tBosch   = bmp.readTemperature();  // NE do temp2 - to patri DS18B20
  }
}

void readDS18B20(Reading &r) {
  ds18b20.begin();
  if (ds18b20.getDeviceCount() == 0) return;
  ds18b20.setResolution(12);
  ds18b20.requestTemperatures();
  float v = ds18b20.getTempCByIndex(0);
  // Syrova hodnota; korekci (toff.ds) pricte az mergeSources.
  if (v != DEVICE_DISCONNECTED_C && v > -100.0f) r.tDs = v;
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

// ---------------------------------------------------------------------------
// Cekani po dobu zahrivani SEN6x
// ESP32-S3 v aktivnim stavu bere ~40 mA. Kdyz cekani prospi v light-sleep
// (~1 mA), usetri se pri 30 s zahrivani a peti minutovem intervalu radove
// 30 % celkove spotreby. Light sleep se preskoci pri startu z tlacitka -
// tam byva pripojene USB a uspani by shodilo seriovou konzoli.
// ---------------------------------------------------------------------------
bool allowLightSleep = false;       // nastavi setup() podle duvodu probuzeni

static void senWaitWarmup(uint32_t warmMs) {
  uint32_t elapsed = millis() - senStartMs;
  if (elapsed >= warmMs) return;
  uint32_t rest = warmMs - elapsed;

  if (allowLightSleep && cfg.lightSleep && rest > 2000) {
    uint32_t nap = rest - 500;      // posledni pul sekundy uz probdime
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)nap * 1000ULL);
    esp_light_sleep_start();
    // Po probuzeni pojistky: napajeci spinac uSup i sbernici nastavime znovu.
    // Stav periferii light-sleep zachovava, ale kdyby ne, cidlo by se vyplo
    // uprostred mereni a tise by vratilo nesmysl.
    digitalWrite(PIN_POWER, HIGH);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(I2C_HZ_SEN6X);
    // Casovac na probuzeni z deep sleep se nastavuje az v goToSleep(),
    // takze ho tu neni potreba rusit.
    elapsed = millis() - senStartMs;
    rest = (elapsed >= warmMs) ? 0 : (warmMs - elapsed);
  }
  if (rest) delay(rest);
}

// ---------------------------------------------------------------------------
// SEN6x: cele mereni od spusteni po odpojeni
//
// DVA INTERVALY
// Zakladni interval (cfg.intervalMin) plati pro teplotu, vlhkost a tlak -
// ta mereni stoji zlomek energie a maji byt co nejjemnejsi. Prach a CO2
// ze SEN6x se meri jen kazde cfg.senMult-te probuzeni, protoze ventilator
// cidla bere 90 mA po celou dobu zahrivani. Pri intervalu 5 min a nasobku 3
// je teplota po peti minutach a prach po patnacti.
//
// Pocitadlo je v RTC RAM (prezije deep sleep). Po vypadku napajeni zacne
// od nuly, takze prvni mereni po startu je vzdy plne - to je zamer, at
// uzivatel hned po zapnuti neco uvidi.
// ---------------------------------------------------------------------------
// Ma se v probuzeni cislo tick spustit SEN6x?
// Tick se predava parametrem zamerne: pocitadlo se inkrementuje driv, nez se
// rozhoduje, a cist ho tady znovu by posunulo fazi o jedno probuzeni
// (prvni mereni po startu by se preskocilo).
static bool senDueAtTick(uint16_t tick) {
  if (cfg.senMult <= 1) return true;
  return (tick % cfg.senMult) == 0;
}

void readSen6x(Reading &r) {
  if (!det.sen6x) return;

  const uint16_t myTick = senTick;
  // Tik ubehne i pri preskoceni. Modulo 12 je delitelne vsemi povolenymi
  // nasobky (1, 2, 3, 4), takze se faze nikdy neposune pretecenim.
  senTick = (uint16_t)((senTick + 1) % 12);

  if (!senDueAtTick(myTick)) {
    Serial.printf("SEN6x: preskoceno (nasobek %dx, tik %u) - PM a CO2 az za %d min.\n",
                  cfg.senMult, myTick,
                  cfg.intervalMin * (cfg.senMult - (myTick % cfg.senMult)));
    return;
  }

  // Ochrana proti podpeti: rozbeh ventilatoru je proudova spicka pres 100 mA
  // a na vybite baterii by shodil celou desku (brownout uprostred zapisu
  // do flash je nejhorsi myslitelny okamzik).
  if (!isnan(r.vbat) && r.vbat < SEN_VBAT_MIN) {
    Serial.printf("SEN6x preskocen: baterie %.2f V < %.2f V.\n", r.vbat, SEN_VBAT_MIN);
    return;
  }

  // Kompenzace CO2: skutecny tlak z barometru je lepsi vstup nez nadmorska
  // vyska, protoze zohledni i pocasi. Kdyz barometr neni, zbyde vyska.
  sen6xApplyConfig(r.pressRaw);

  // --- cisteni ventilatoru ---------------------------------------------------
  // Automaticky po cfg.senCleanDays dnech, nebo na vyzadani ze servisu/webu.
  //
  // POZOR: startFanCleaning() je podle datasheetu dostupne JEN V IDLE, tedy
  // pred spustenim mereni. V rezimu mereni cidlo prikaz odmitne - a kdyby se
  // navic pocitadlo vynulovalo, deska by si myslela, ze uklizeno je, a dalsich
  // N dni by cisteni nezopakovala. Proto se ceka na navratovy kod a pocitadlo
  // se nuluje jen pri uspechu.
  //
  // Datasheet zada po cisteni aspon 10 s pauzu pred startem mereni - tu pokryva
  // delay(11000) nize. Vedlejsi efekt je vitany: prach zviren ventilatorem
  // stihne sednout a neovlivni nasledujici vzorek.
  bool autoClean = (cfg.senCleanDays > 0 &&
                    senCleanMin >= (uint32_t)cfg.senCleanDays * 1440UL);
  if (pendingFanClean || autoClean) {
    Serial.printf("SEN6x: cisteni ventilatoru (~10 s, %s)...\n",
                  pendingFanClean ? "rucne" : "automaticky");
    if (senCleanCmd() == 0) {
      delay(11000);                 // 10 s cisteni + predepsana pauza
      senCleanMin = 0;
      Serial.println("SEN6x: cisteni hotovo.");
    } else {
      Serial.println("SEN6x: cisteni cidlo odmitlo, zkusi se pri dalsim mereni.");
    }
    pendingFanClean = false;
  }

  if (!sen6xStart()) {
    Serial.println("SEN6x: mereni se nepodarilo spustit.");
    return;
  }

  uint32_t warmMs = (uint32_t)cfg.senWarmS * 1000UL;

  // Kalibrace CO2 je vyjimka: datasheet zada aspon 3 minuty behu ve stalem
  // prostredi. Jednorazove tedy zahrivani prodlouzime, jinak by FRC selhala
  // nebo (hure) prosla se spatnou referenci.
  bool wantFrc = (pendingCo2Ref >= 0 && senHasCo2() && !det.scd41);
  if (wantFrc) {
    warmMs = 210000UL;              // 3,5 minuty
    Serial.println("SEN6x: kalibrace CO2 - cidlo pobezi 3,5 min. Nechte ho"
                   " v ustalenem prostredi.");
  }

  senWaitWarmup(warmMs);

  // Cekani na priznak data-ready - PM se pocita z klouzaveho okna
  // a prvni platny vzorek muze prijit o par set milisekund pozdeji.
  uint32_t t0 = millis();
  bool ready = false;
  while (millis() - t0 < SEN_READ_TIMEOUT) {
    if (sen6xDataReady()) { ready = true; break; }
    delay(200);
  }
  // senFresh se nastavuje az tady - "cerstve" znamena, ze hodnoty opravdu
  // dorazily, ne jen ze se cidlo rozeblo.
  if (ready) {
    // Kdyz CO2 bere ze SEN6x, prvni odecet se zahodi a pocka se na dalsi.
    // Merici clanek CO2 je stejne rodiny jako SCD4x a jeho datasheet rika:
    // "After a power cycle, the initial single shot reading should be
    // discarded to maximize accuracy." Deska cidlo mezi merenimi odpojuje,
    // takze prvni hodnota je vzdy ta k zahozeni - a cidlo pak hlasi porad
    // skoro totez cislo blizko vychozi kalibrace.
    if (senHasCo2() && !det.scd41) {
      Reading discard;
      sen6xReadValues(discard);
      uint32_t t1 = millis();
      while (millis() - t1 < 6000) {          // clanek CO2 meri po ~5 s
        delay(250);
        if (sen6xDataReady()) break;
      }
    }
    if (sen6xReadValues(r)) r.senFresh = true;
    else                    Serial.println("SEN6x: cteni hodnot selhalo.");
  } else {
    Serial.println("SEN6x: data nebyla pripravena vcas.");
  }

  sen6xStop();                      // FRC datasheet chce po zastaveni mereni

  if (wantFrc) {
    int32_t corr = 0;
    if (sen6xForcedCo2((uint16_t)pendingCo2Ref, corr))
      Serial.printf("SEN6x FRC hotova, korekce %ld ppm\n", (long)corr);
    else
      Serial.println("SEN6x FRC selhala - cidlo musi bezet aspon 3 min"
                     " ve stalem prostredi.");
    pendingCo2Ref = -1;
  }
}

// ---------------------------------------------------------------------------
// Slozeni vysledku z jednotlivych zdroju
//
// Teplotu a vlhkost hlasi az ctyri cidla najednou. Poradi je pevne a je
// popsane u struktury Reading. Krome hodnoty si pamatujeme i zdroj, aby bylo
// v servisu a na webu videt, odkud cislo je.
//
// Offsety se pricitaji az TADY, na jednom miste, a to KAZDEMU ZDROJI TEN SVUJ.
// Drive to delala kazda cteci funkce zvlast jednim spolecnym offsetem, coz
// bylo spatne hned dvakrat: pri vice cidlech se korekce nastavena podle SHT40
// pouzila i na SEN6x, ktery se myli uplne jinak, a kdyz cidlo vypadlo,
// prevzalo hodnotu jine s cizi korekci.
// ---------------------------------------------------------------------------
static void mergeSources(Reading &r) {
  // Poradi odpovida enumu TempSrc / HumSrc, tedy i prioritam.
  const float hRaw[HSRC_COUNT] = { r.hSht, r.hSen, r.hScd, r.hBosch };

  // --- druha teplota: vyhradne DS18B20, se svym vlastnim offsetem ---
  // Sonda na kabelu visi mimo desku, takze korekce pro SHT40 by pro ni byla
  // nesmysl. Proto ma vlastni polozku (toff.ds), ktera je vychozi nula.
  if (!isnan(r.tDs)) r.temp2 = r.tDs + cfg.tOff[TSRC_DS];

  // --- teplota vzduchu ---
  //
  // Zdroj urcuje VYHRADNE tsrcPrimary(), tedy dostupnost cidel a volba
  // uzivatele - NE to, ktere cidlo zrovna neco vratilo. Kdyby se pri jednom
  // neuspesnem cteni sahlo po jinem cidle, rozesly by se tri veci najednou:
  // popisek na displeji ("Teplota - SHT40") by lhal, qCanonical() by ukazoval
  // na jinou velicinu nez odkud data opravdu jsou, a graf "Teplota" by
  // obsahoval smes dvou cidel s jinymi offsety. Mezera v grafu je poctivejsi.
  const float tAll[TSRC_COUNT] = { r.tSht, r.tSen, r.tScd, r.tBosch, r.tDs };
  const uint8_t ti = tsrcPrimary();
  if (ti < TSRC_COUNT) {
    r.srcTemp = tsrcName(ti);                 // zdroj plati i kdyz vzorek chybi
    if (!isnan(tAll[ti])) r.temp = tAll[ti] + cfg.tOff[ti];
  }

  // --- vlhkost ---
  const uint8_t hi = hsrcPrimary();
  if (hi < HSRC_COUNT) {
    r.srcHum = hsrcName(hi);
    if (!isnan(hRaw[hi]))
      r.hum = constrain(hRaw[hi] + cfg.hOff[hi], 0.0f, 100.0f);
  }

  // --- CO2: SCD41 ma prednost, je presnejsi a levnejsi na energii ---
  if (!isnan(r.co2Scd))      { r.co2 = r.co2Scd; r.srcCo2 = "SCD41"; }
  else if (!isnan(r.co2Sen)) { r.co2 = r.co2Sen; r.srcCo2 = senKindName(senKind); }

  // Tlak umi jen barometr; r.press uz ma prepocet i offset z readBosch().
}

Reading doMeasurement() {
  Reading r;
  // Baterie se cte jako prvni, jeste bez zateze ventilatoru - jinak by
  // namereny pokles vypadal jako vybita baterie.
  r.vbat = readVBat();
  readSHT40(r);
  if (det.scd41)                 readSCD41(r);
  // Barometr PRED SEN6x: jeho tlak se posila do SEN6x jako vstup pro
  // kompenzaci CO2, takze uz musi byt zmereny.
  if (det.bosch != BOSCH_NONE)   readBosch(r);
  if (det.ds18b20)               readDS18B20(r);
  // SEN6x uplne nakonec - je zdaleka nejpomalejsi a nejzravejsi, at se
  // ventilator toci co nejkratsi dobu.
  if (det.sen6x)                 readSen6x(r);

  mergeSources(r);
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

// ============================================================================
// TEXT SE SYMBOLY, KTERE FONTY ADAFRUIT GFX NEMAJI
// ============================================================================
// Vlastni vykreslovani znaku SYM_DEG / SYM_MU / SYM_CUB (definice viz sekce
// SYMBOLY nahore). Jsou to tri znaky, takze se vyplati je dokreslit
// z primitiv misto pridavani celeho dalsiho fontu.
//
// Kazdy znak je potreba umet dvakrat: zmerit (richWidth) a vykreslit
// (richPrint). Kdyby se ty dve funkce rozesly, rozsypalo by se zarovnani
// na pravy okraj i stredovani hodnot v radcich.
// ============================================================================
// GFX neumi rict, jaky font je prave nastaveny, a horni index ho potrebuje
// docasne prepnout. Vedeme si ho proto sami - VSECHNA nastaveni fontu musi
// jit pres useFont(), jinak se sledovana hodnota rozejde se skutecnosti.
const GFXfont *curFont = nullptr;
static inline void useFont(const GFXfont *f) { curFont = f; display.setFont(f); }

// Vyska cislic aktualniho fontu - podle ni se skaluji dokreslovane znaky.
static int fontCapHeight() {
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("0", 0, 0, &x1, &y1, &w, &h);
  return h ? h : 10;
}

// Mensi font pro horni index. Vraci nullptr, kdyz zmensovat neni z ceho.
static const GFXfont* smallerFont(const GFXfont *f) {
  if (f == &FreeSansBold24pt7b) return &FreeSansBold12pt7b;
  if (f == &FreeSansBold18pt7b) return &FreeSansBold12pt7b;
  if (f == &FreeSansBold12pt7b) return &FreeSans9pt7b;
  if (f == &FreeSans12pt7b)     return &FreeSans9pt7b;
  return nullptr;                   // 9pt uz nezmensujeme, bylo by necitelne
}

static int symWidth(char c, int cap) {
  switch ((uint8_t)c) {
    case 0267: return cap / 2 + 2;                   // · oddelovac
    case 0260: return cap / 3 + 3;                   // ° prstenec + odsazeni
    case 0265: {                                     // µ je siroke jako 'u'
      int16_t x1, y1; uint16_t w, h;
      display.getTextBounds("u", 0, 0, &x1, &y1, &w, &h);
      return w + 1;
    }
    case 0263: {                                     // ³ mensim fontem
      const GFXfont *sf = smallerFont(curFont);
      if (!sf) return cap / 2;
      display.setFont(sf);
      int16_t x1, y1; uint16_t w, h;
      display.getTextBounds("3", 0, 0, &x1, &y1, &w, &h);
      display.setFont(curFont);
      return w + 1;
    }
    default: return 0;
  }
}

// Sirka retezce vcetne dokreslovanych znaku.
int richWidth(const char *s) {
  int cap = fontCapHeight();
  int total = 0;
  char plain[64]; uint8_t n = 0;
  auto flush = [&]() {
    if (!n) return;
    plain[n] = 0;
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(plain, 0, 0, &x1, &y1, &w, &h);
    total += w;
    n = 0;
  };
  for (const char *p = s; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if (c == 0260 || c == 0265 || c == 0263 || c == 0267) { flush(); total += symWidth(*p, cap); }
    else if (n < sizeof(plain) - 1) plain[n++] = *p;
  }
  flush();
  return total;
}

// Vykresli retezec od (x, y) na uctari, vcetne dokreslovanych znaku.
void richPrint(int x, int y, const char *s) {
  int cap = fontCapHeight();
  char plain[64]; uint8_t n = 0;
  auto flush = [&]() {
    if (!n) return;
    plain[n] = 0;
    display.setCursor(x, y);
    display.print(plain);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(plain, 0, 0, &x1, &y1, &w, &h);
    x += w;
    n = 0;
  };
  for (const char *p = s; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if (c == 0267) {                                 // · oddelovac v pulce vysky
      flush();
      int r = cap / 10; if (r < 1) r = 1;
      display.fillCircle(x + symWidth(*p, cap) / 2, y - cap / 3, r, GxEPD_BLACK);
      x += symWidth(*p, cap);
    } else if (c == 0260) {                          // ° prstenec u horni hrany
      flush();
      int r = cap / 6; if (r < 2) r = 2;
      int cx = x + r + 1, cy = y - cap + r;
      display.drawCircle(cx, cy, r, GxEPD_BLACK);
      if (r >= 3) display.drawCircle(cx, cy, r - 1, GxEPD_BLACK);  // silnejsi obrys
      x += symWidth(*p, cap);
    } else if (c == 0265) {                          // µ = 'u' s nozickou vlevo
      flush();
      display.setCursor(x, y);
      display.print('u');
      int16_t x1, y1; uint16_t w, h;
      display.getTextBounds("u", 0, 0, &x1, &y1, &w, &h);
      int stem = cap / 2;
      display.drawFastVLine(x, y - stem / 2, stem, GxEPD_BLACK);
      display.drawFastVLine(x + 1, y - stem / 2, stem, GxEPD_BLACK);
      x += w + 1;
    } else if (c == 0263) {                          // ³ mensi '3' zvednuta
      flush();
      const GFXfont *sf = smallerFont(curFont);
      if (sf) {
        display.setFont(sf);
        display.setCursor(x, y - cap / 2);
        display.print('3');
        display.setFont(curFont);
      }
      x += symWidth(*p, cap);
    } else if (n < sizeof(plain) - 1) {
      plain[n++] = *p;
    }
  }
  flush();
}

// Levy okraj bloku s baterii - popisky v hlavicce se musi vejit pred nej.
int batteryLeftX = W;

void drawBattery(float vb) {
  const int bw = 34, bh = 16;
  const bool low = (!isnan(vb) && vb < VBAT_LOW);
  // Text drzime kratky - vedle nej vlevo je popisek hlavicky a dlouhy
  // retezec by do nej mohl zasahnout. Varovani je ve spodnim radku hlavicky.
  char buf[24];
  int pct = vbatPercent(vb);
  if (isnan(vb)) snprintf(buf, sizeof(buf), "--.- V");
  else           snprintf(buf, sizeof(buf), "%.2f V  %d%%", vb, pct);

  useFont(&FreeSans9pt7b);
  int16_t x1, y1; uint16_t tw, th;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
  const int by = 8;                 // blize k horni hrane displeje
  const int tx = W - 20 - tw;
  const int bx = tx - 8 - (bw + 3);
  batteryLeftX = bx;

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
  richPrint(rightX - richWidth(txt), baselineY, txt);
}

// Radek namerene hodnoty ve trech sloupcich:
// vlevo nazev, uprostred hodnota, vpravo jednotka.
// Hodnota se drzi na spolecnem stredu, ale kdyby se dotkla nazvu nebo
// jednotky, odsune se - text se tak nikdy neprekryje.
//
// Jednotka se kresli MENSIM fontem: nese nejmene informace, a kdyz je drobna,
// oko sklouzne rovnou na cislo. Nazev i hodnota zustavaji v puvodni velikosti.
void drawValueRow(const char *name, const char *value, const char *unit, int y) {
  const int L = 20, R = W - 20, GAP = 12, CENTER = 274;
  const GFXfont *big = curFont;
  const GFXfont *small = smallerFont(big);
  if (!small) small = big;

  int nw = richWidth(name);
  int vw = richWidth(value);
  useFont(small);
  int uw = richWidth(unit);
  useFont(big);

  richPrint(L, y, name);

  useFont(small);
  richPrint(R - uw, y, unit);
  useFont(big);

  int vx = CENTER - vw / 2;
  int lo = L + nw + GAP;                  // nesmi zasahovat do nazvu
  int hi = R - uw - GAP - vw;             // ani do jednotky
  if (vx < lo) vx = lo;
  if (vx > hi) vx = hi;
  if (vx < lo) vx = lo;                   // kdyby bylo misto opravdu malo
  richPrint(vx, y, value);
}

// Ktere radky hlavicky se maji kreslit. Spolecne pro drawHeader i
// headerBottom - kdyby se rozesly, grafy by lezly do hlavicky.
static bool rowCo2(const Reading &r)   { return qAvailable(Q_CO2)   || !isnan(r.co2); }
static bool rowPm(const Reading &r)    { return det.sen6x || !isnan(r.pm25) || !isnan(r.pm10); }
static bool rowPress(const Reading &r) { return qAvailable(Q_PRESS) || !isnan(r.press); }
// Sonda na kabelu ma vlastni radek - ale ne kdyz uz je zvolena jako hlavni
// teplota nahore. Dvakrat totez cislo je jen matouci.
static bool rowDs(const Reading &r) {
  // Ptame se na SKUTECNY zdroj, ne jen na volbu uzivatele: pri tsrc=auto
  // a DS18B20 jako jedinem teplomeru je hlavni teplota taky z nej, takze
  // by radek "Teplota 2" ukazoval totez cislo podruhe.
  if (tsrcPrimary() == TSRC_DS) return false;
  return det.ds18b20 || !isnan(r.temp2);
}
// Teplota cipu barometru se ukazuje jen jako nahrada za chybejici DS18B20 -
// a jen kdyz uz neni pouzita jako hlavni teplota nahore.
static bool rowBoschT(const Reading &r) {
  return !isnan(r.tBosch) && strcmp(r.srcTemp, boschName()) != 0;
}

// ---- ROZVRZENI HLAVICKY ----------------------------------------------------
// Hlavicka je posunuta co nejvys, aby na grafy zbylo maximum mista. Konstanty
// jsou na jednom miste, protoze je pouziva i headerBottom() - kdyby se
// rozesly, grafy by lezly do hlavicky nebo by nahore zustala prazdna diera.
#define HDR_LABEL_Y   20            // uctari popisku "Teplota"
#define HDR_BIG_Y     74            // uctari velke teploty
#define HDR_HUM_Y     104           // uctari radku s vlhkosti
#define HDR_LINE_Y    120           // delici cara
#define HDR_ROWS_Y    152           // uctari prvniho radku hodnot
#define HDR_ROW_H     40            // rozestup radku hodnot (do 3 radku)
#define HDR_ROW_H4    36            // pri 4 radcich stesnano, at zbyde na grafy

// Kolik radku hodnot se bude kreslit (CO2, PM, tlak, druha teplota).
static uint8_t headerRowCount(const Reading &r) {
  uint8_t n = 0;
  if (rowCo2(r))   n++;
  if (rowPm(r))    n++;
  if (rowPress(r)) n++;
  if (rowDs(r) || rowBoschT(r)) n++;
  return n ? n : 1;                 // aspon radek "(pripojeno jen SHT4x)"
}
static inline int headerRowStep(const Reading &r) {
  return headerRowCount(r) >= 4 ? HDR_ROW_H4 : HDR_ROW_H;
}

void drawHeader(const Reading &r, float vb) {
  char buf[64], val[16];
  const int step = headerRowStep(r);

  // ---- PRAVY SLOUPEC: baterie, pod ni cidla (kazde na svem radku) ----
  //
  // Sloupec nesmi prerust delici caru na HDR_LINE_Y. Pri peti cidlech plus
  // radku s intervalem a varovanim o baterii by se pri kroku 17 px dostal az
  // na 142, tedy pres caru i do prvniho radku hodnot. Krok je proto 15 px
  // a pred kazdym dalsim radkem se kontroluje, jestli se jeste vejde.
  drawBattery(vb);
  useFont(&FreeSans9pt7b);
  const char *sn[6];
  uint8_t sCnt = sensorNames(sn, 6);
  const int RY_STEP = 15;
  const int RY_MAX  = HDR_LINE_Y - 3;   // posledni uctara, ktera se jeste vejde
  int ry = 38;                          // prvni radek pod baterii
  for (uint8_t i = 0; i < sCnt && ry <= RY_MAX; i++) {
    drawRight(sn[i], W - 20, ry);
    ry += RY_STEP;
  }
  if (ry <= RY_MAX) {
    snprintf(buf, sizeof(buf), "interval %d min", cfg.intervalMin);
    drawRight(buf, W - 20, ry);
    ry += RY_STEP;
  }
  // Varovani o baterii je dulezitejsi nez posledni radek seznamu - kdyz uz
  // pro nej neni misto, prepise posledni vypsany radek.
  if (!isnan(vb) && vb < VBAT_LOW) {
    int wy = (ry <= RY_MAX) ? ry : RY_MAX;
    display.fillRect(W / 2, wy - 12, W / 2 - 18, 15, GxEPD_WHITE);
    drawRight("BATERIE SLABA", W - 20, wy);
  }

  // ---- LEVY SLOUPEC: hlavni namerena hodnota ----
  // Prvni radek nese OBA popisky vedle sebe a u kazdeho jmeno cidla, ze
  // ktereho hodnota je. Jinak by uzivatel, ktery si prepnul hlavni teplotu
  // na SEN6x, cetl "Teplota" a myslel si, ze je porad z cidla na desce.
  // Jmeno cidla se pripisuje jen kdyz je z ceho vybirat - pri jedinem zdroji
  // by to byl jen sum.
  useFont(&FreeSans9pt7b);          // explicitne, at popisek nezavisi na poradi
  char lblT[32], lblH[32];
  if (tsrcCount() > 1 && tsrcPrimary() < TSRC_COUNT)
    snprintf(lblT, sizeof(lblT), "Teplota %s", tsrcName(tsrcPrimary()));
  else
    snprintf(lblT, sizeof(lblT), "Teplota");
  if (hsrcCount() > 1 && hsrcPrimary() < HSRC_COUNT)
    snprintf(lblH, sizeof(lblH), "Vlhkost %s", hsrcName(hsrcPrimary()));
  else
    snprintf(lblH, sizeof(lblH), "Vlhkost");

  richPrint(20, HDR_LABEL_Y, lblT);
  // Druhy popisek jen kdyz se pred baterii jeste vejde - na uzkem displeji
  // je dulezitejsi napeti nez jmeno cidla.
  int hx = 20 + richWidth(lblT) + 18;
  if (hx + richWidth(lblH) < batteryLeftX - 10)
    richPrint(hx, HDR_LABEL_Y, lblH);

  // Velka teplota: cislo velkym fontem, jednotka mensim hned za nim.
  qFormat(Q_TEMP, r.temp, val, sizeof(val));
  useFont(&FreeSansBold24pt7b);
  int tw = richWidth(val);
  richPrint(20, HDR_BIG_Y, val);
  useFont(&FreeSansBold18pt7b);   // jednotka mensim fontem nez samotne cislo
  richPrint(20 + tw + 8, HDR_BIG_Y, U_DEGC);

  // Zdroj uz je v prvnim radku, tady jen hodnota.
  qFormat(Q_HUM, r.hum, val, sizeof(val));
  useFont(&FreeSansBold12pt7b);
  snprintf(buf, sizeof(buf), "Vlhkost: %s %%", val);
  richPrint(20, HDR_HUM_Y, buf);

  display.drawFastHLine(20, HDR_LINE_Y, W - 40, GxEPD_BLACK);

  // ---- Dalsi namerene hodnoty ----
  // Radky se ridi TIM, CO JE PRIPOJENO, ne tim, co se zrovna podarilo zmerit.
  // Jinak by pri jednom vypadku cidla radek zmizel, hlavicka by se zkratila,
  // grafy by se prekreslily jinak vysoke a displej by pri kazde chybe poskocil.
  useFont(&FreeSansBold18pt7b);
  int y = HDR_ROWS_Y;
  bool any = false;
  if (rowCo2(r)) {
    qFormat(Q_CO2, r.co2, val, sizeof(val));
    drawValueRow("CO2", val, "ppm", y);
    y += step; any = true;
  }
  // PM2.5 a PM10 sdili jeden radek - kazde zvlast by ubralo grafum dalsi radek
  // a obe cisla spolu stejne ctete najednou.
  if (rowPm(r)) {
    char a[16], b[16];
    qFormat(Q_PM25, r.pm25, a, sizeof(a));
    qFormat(Q_PM10, r.pm10, b, sizeof(b));
    snprintf(buf, sizeof(buf), "%s / %s", a, b);
    drawValueRow("PM2.5/10", buf, U_UGM3, y);
    y += step; any = true;
  }
  if (rowPress(r)) {
    qFormat(Q_PRESS, r.press, val, sizeof(val));
    drawValueRow("Tlak", val, "hPa", y);
    y += step; any = true;
  }
  if (rowDs(r)) {
    // Skutecne externi cidlo na kabelu.
    qFormat(Q_TEMP2, r.temp2, val, sizeof(val));
    drawValueRow("Teplota 2", val, U_DEGC, y);
    y += step; any = true;
  } else if (rowBoschT(r)) {
    // DS18B20 chybi nebo neodpovedel. Ukazeme teplotu cipu tlakomeru,
    // ale pod jeho vlastnim nazvem - at je jasne, ze to neni cidlo na kabelu.
    // Offset se pricita i tady, at je stejne cislo z tehoz cipu vzdy stejne
    // korigovane, at uz slouzi jako hlavni teplota nebo jako nahrada.
    qFormat(Q_TEMP2, r.tBosch + cfg.tOff[TSRC_BOSCH], val, sizeof(val));
    drawValueRow(boschName(), val, U_DEGC, y);
    y += step; any = true;
  }
  // Kdyz uz je teplota cipu tlakomeru hlavni teplotou nahore (zadne jine
  // cidlo neodpovedelo), druhy radek s toutez hodnotou by byl jen matouci.
  if (!any) {
    useFont(&FreeSans12pt7b);
    richPrint(20, y, "(pripojeno jen SHT4x)");
    y += step;
  }

  display.drawFastHLine(20, y - 24, W - 40, GxEPD_BLACK);
}

// Vraci Y souradnici, kde hlavicka konci (aby grafy zacaly pod ni).
// Musi souhlasit s drawHeader() - proto stejne konstanty i stejne pocitadlo.
int headerBottom(const Reading &r) {
  return HDR_ROWS_Y + headerRowStep(r) * headerRowCount(r) - 24;
}

// Zaokrouhli nahoru na "hezke" cislo (1-2-5 x mocnina desiti), aby popisky
// osy vychazely na kulate hodnoty i u velmi malych rozsahu.
static float niceCeil(float v) {
  if (!(v > 0)) return 1.0f;
  float mag = powf(10.0f, floorf(log10f(v)));
  float n = v / mag;
  if (n <= 1.0f) n = 1.0f;
  else if (n <= 2.0f) n = 2.0f;
  else if (n <= 5.0f) n = 5.0f;
  else n = 10.0f;
  return n * mag;
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
    case Q_PM25:
    case Q_PM10:
      // Prach byva vetsinu casu u nuly a obcas vyskoci. Osa proto zacina
      // na nule - jinak by kazdy sum vypadal jako smogova epizoda.
      //
      // Horni mez se ODVIJI OD DAT, ne od pevneho minima. V cistem vzduchu
      // se meri desetiny ug/m3 a pevny rozsah 0-10 by prubeh slepil s dolni
      // hranou grafu, takze by nebylo videt vubec nic. Bereme proto 25 %
      // rezervu nad maximem a zaokrouhlujeme na "hezke" cislo.
      mn = 0;
      mx = any ? niceCeil(dataMax * 1.25f) : 1.0f;
      minSpan = 0.5f;                  // uplne plocha nula je stale citelna
      break;
    default:                            // teploty
      mn = floorf(mn) - 0.5f;          mx = ceilf(mx) + 0.5f;
      minSpan = 2.0f; break;
  }
  if (mx - mn < minSpan) { float c = (mn + mx) / 2; mn = c - minSpan/2; mx = c + minSpan/2; }
  // Zaporny prach neexistuje - dorovnani rozsahu vyse by ho dokazalo
  // pod nulu poslat.
  if ((q == Q_PM25 || q == Q_PM10) && mn < 0) { mx -= mn; mn = 0; }

  // --- radek nad grafem: vlevo velicina, vpravo extremy za obdobi ---
  char title[56];
  snprintf(title, sizeof(title), "%s [%s]", qLabel(q), qUnit(q));
  useFont(&FreeSans9pt7b);
  richPrint(gx, gy - 8, title);

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
  // popisek hodnoty: u desetinnych velicin jedno misto, u CO2 cele cislo.
  // U velmi malych rozsahu (cisty vzduch, PM v desetinach) by jedno desetinne
  // misto slepilo vsechny popisky na "0.0", proto se tam prepina na dve.
  const bool fine = (mx - mn) < 2.0f;
  auto fmtVal = [&](float v, char *o, size_t n) {
    if (q == Q_CO2)       snprintf(o, n, "%d", (int)lroundf(v));
    else if (q == Q_VBAT) snprintf(o, n, "%.2f", v);
    else if (fine)        snprintf(o, n, "%.2f", v);
    else                  snprintf(o, n, "%.1f", v);
  };

  // --- osa Y: 3 hlavni carkovane + 2 vedlejsi teckovane mezi nimi ---
  useFont(&FreeSans9pt7b);
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
  //
  // OSAMOCENE VZORKY: prach a CO2 se pri nasobku intervalu (senmult) meri
  // rid[c]eji nez zbytek, takze mezi nimi jsou v historii mezery. Samotny bod
  // obklopeny mezerami nema s cim spojit caru a driv se nevykreslil vubec -
  // graf pak zustal prazdny, i kdyz data byla. Takovy vzorek proto kreslime
  // jako pu[n]tik.
  int px = -1, py = -1;
  bool prevValid = false;
  for (uint16_t i = 0; i < histCount; i++) {
    float v = storeToVal(q, histAt(ch, i));
    if (isnan(v)) { px = -1; prevValid = false; continue; }
    int x = xFor(i), y = yFor(v);

    bool nextValid = false;
    if (i + 1 < histCount) nextValid = !isnan(storeToVal(q, histAt(ch, i + 1)));

    if (prevValid) {
      if (channels[ch].dashed) {
        drawDashedLine(px, py, x, y, 5, 4);
      } else {
        display.drawLine(px, py, x, y, GxEPD_BLACK);
        display.drawLine(px, py - 1, x, y - 1, GxEPD_BLACK);   // tloustka 2 px
      }
    } else if (!nextValid) {
      display.fillCircle(x, y, 2, GxEPD_BLACK);   // vzorek bez sousedu
    }
    px = x; py = y; prevValid = true;
  }
}

// Zapati s verzi firmwaru. Drzi se uplne dole, aby si nekonkurovalo
// s pravym sloupcem hlavicky (tam uz je baterie, seznam cidel a interval).
void drawFooter() {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s v%s", FW_NAME, FW_VERSION);
  useFont(&FreeSans9pt7b);
  richPrint(20, H - 8, buf);
  drawRight(fwBuildDate(), W - 15, H - 8);
}

void drawGraphs(int top) {
  // Spodni okraj je nad zapatim s verzi (popisky casove osy sedi 16 px
  // pod ramem grafu, takze grafy musi skoncit driv).
  const int bottom = H - 46, left = 55, right = W - 15;
  const int gw = right - left;
  int n = channelCount > 0 ? channelCount : 1;

  // Mezera mezi grafy musi pojmout popisky casove osy (uctara gBottom+16,
  // podpatek jeste 3 px pod ni) a nadpis dalsiho grafu (uctara gy-8, horni
  // dotah 13 px nad ni). Minimum je tedy 19 + 21 = 40 px; 42 nechava rezervu
  // a i pri ctyrech grafech zbyde na kazdy 78 px vysky.
  const int gap = 42;
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
    drawFooter();
  } while (display.nextPage());
}

// ============================================================================
// SERVISNI REZIM
// ============================================================================
// Seznam dostupnych velicin ve tvaru "klic (Popis)", oddeleny carkami.
// Format je pevny - parsuje ho webovy konfigurator a sklada z nej nabidku
// velicin, takze o pripojenych cidlech nemusi nic vedet dopredu.
void svcAvailable() {
  Serial.print("Dostupne veliciny: ");
  bool first = true;
  for (uint8_t i = 0; i < Q_ALL_COUNT; i++) {
    if (!qOffered(Q_ALL[i])) continue;
    if (!first) Serial.print(", ");
    Serial.printf("%s (%s)", qKey(Q_ALL[i]), qLabel(Q_ALL[i]));
    first = false;
  }
  Serial.println();
}

// Zvoleny zdroj hlavni teploty a vlhkosti - opet v pevnem formatu pro web.
void svcSources() {
  Serial.printf("  tsrc = %s (%s), moznosti: auto",
                cfg.tSrcPref < TSRC_COUNT ? tsrcKey(cfg.tSrcPref) : "auto",
                srcTempName());
  for (uint8_t i = 0; i < TSRC_COUNT; i++)
    if (tsrcAvailable(i)) Serial.printf(" %s", tsrcKey(i));
  Serial.println();
  Serial.printf("  hsrc = %s (%s), moznosti: auto",
                cfg.hSrcPref < HSRC_COUNT ? hsrcKey(cfg.hSrcPref) : "auto",
                srcHumName());
  for (uint8_t i = 0; i < HSRC_COUNT; i++)
    if (hsrcAvailable(i)) Serial.printf(" %s", hsrcKey(i));
  Serial.println();
}

void svcHelp() {
  Serial.println();
  Serial.println("========================================");
  Serial.printf ("  %s v%s - SERVISNI REZIM\n", FW_NAME, FW_VERSION);
  Serial.printf ("  build %s\n", fwBuildDate());
  Serial.println("========================================");
  Serial.printf ("Detekovana cidla: %s\n", sensorSummary());
  if (det.sen6x)
    Serial.printf("SEN6x:            %s  sn %s\n",
                  senName[0] ? senName : "?", senSerial[0] ? senSerial : "?");
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
  Serial.println("  Kazde cidlo ma vlastni offset - kazde se totiz myli jinak.");
  Serial.println("  Bez tecky se nastavi to cidlo, ktere hodnotu opravdu dodava.");
  Serial.println();
  Serial.print  ("  toff.<cidlo>=<x>  Offset teploty [degC] (-20 az 20), cidlo: ");
  for (uint8_t i = 0; i < TSRC_COUNT; i++)
    if (tsrcAvailable(i)) Serial.printf("%s ", tsrcKey(i));
  Serial.println();
  for (uint8_t i = 0; i < TSRC_COUNT; i++)
    if (tsrcAvailable(i))
      Serial.printf("      toff.%-3s = %6.2f  %s%s\n", tsrcKey(i), cfg.tOff[i],
                    i == TSRC_DS ? "sonda na kabelu, radek Teplota 2 - " : "",
                    tsrcName(i));
  Serial.print  ("  hoff.<cidlo>=<x>  Offset vlhkosti [%RH] (-30 az 30), cidlo: ");
  for (uint8_t i = 0; i < HSRC_COUNT; i++)
    if (hsrcAvailable(i)) Serial.printf("%s ", hsrcKey(i));
  Serial.println();
  for (uint8_t i = 0; i < HSRC_COUNT; i++)
    if (hsrcAvailable(i))
      Serial.printf("      hoff.%-3s = %6.2f  %s\n", hsrcKey(i), cfg.hOff[i], hsrcName(i));
  if (det.bosch != BOSCH_NONE) {
    Serial.println("  poff=<x>    Offset tlaku [hPa]      (-50 az 50)");
    Serial.println("  alt=<x>     Nadmorska vyska [m]     (0 az 4000)");
  }
  if (det.scd41 || senHasCo2()) {
    Serial.println();
    Serial.printf ("  CO2 (%s)\n", det.scd41 ? "SCD41" : senKindName(senKind));
    Serial.println("  co2ref=<x>  Kalibrace na hodnotu [ppm] (venku ~420, 3+ min)");
    Serial.println("  asc=0|1     Automaticka samokalibrace");
  }
  if (det.sen6x) {
    Serial.println();
    Serial.printf ("  CIDLO PRACHU (%s)\n", senKindName(senKind));
    Serial.printf ("  senwarm=<s> Doba behu pred odectem (%d az %d s, ted %d)\n",
                   SEN_WARM_LO, SEN_WARM_HI, cfg.senWarmS);
    Serial.printf ("  senmult=<n> Nasobek intervalu pro PM a CO2 (1 az %d, ted %dx)\n",
                   SEN_MULT_HI, cfg.senMult);
    Serial.printf ("              -> teplota a vlhkost %d min, PM a CO2 %d min\n",
                   cfg.intervalMin, cfg.intervalMin * cfg.senMult);
    Serial.printf ("  senauto=<d> Automaticke cisteni ventilatoru po d dnech\n"
                   "              (0 az %d, 0 = vypnuto, ted %d)\n",
                   SEN_CLEAN_HI, cfg.senCleanDays);
    Serial.println("  senclean    Procistit ventilator hned pri dalsim mereni");
    Serial.println("  senstat     Stavove bity cidla");
    Serial.println("  lsleep=0|1  Uspat ESP32 behem zahrivani cidla (setri ~30 %)");
  }
  Serial.println();
  Serial.println("  MERENI A GRAF");
  Serial.printf ("  int=<min>   Zakladni interval (%d az %d). Meni delku historie!\n",
                 INTERVAL_MIN_LO, INTERVAL_MIN_HI);
  Serial.println("  tsrc=<cidlo>  Ktere cidlo dodava hlavni Teplotu (auto = podle priority)");
  Serial.println("  hsrc=<cidlo>  Ktere cidlo dodava hlavni Vlhkost");
  Serial.println("  ch=auto     Automaticky vyber kanalu grafu");
  Serial.printf ("  ch=a,b,c    Rucni vyber az %d velicin, napr.\n", MAX_CHANNELS);
  Serial.println("              ch=co2,pm25,temp.sht,temp.sen");
  Serial.println("              (velicina konkretniho cidla ma tecku - viz seznam vyse)");
  Serial.println();
  Serial.println("  SPRAVA");
  Serial.println("  list        Vypise aktualni nastaveni");
  Serial.println("  ver         Verze firmwaru");
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

// Jednotny format, ktery parsuje i webovy konfigurator:
//   ver = 4.1.0 (2026-08-25)
void svcVer() {
  Serial.printf("  ver  = %s\n", fwVersionFull());
}

// Odhad spotreby SEN6x, aby bylo hned videt, co dane nastaveni stoji.
// Pocitame typickych 90 mA po dobu behu cidla; ESP32 a e-ink jsou proti tomu
// zanedbatelne (a v light-sleep se scitaji jen jednotky mA).
float senDailyMah() {
  if (!det.sen6x) return 0;
  float runS   = cfg.senWarmS + 3.0f;                  // + start a odecet
  float perDay = 1440.0f / (cfg.intervalMin * (float)cfg.senMult);
  return 90.0f * (runS / 3600.0f) * perDay;
}

// Jak casto se opravdu meri prach a CO2 [min].
static inline uint16_t senIntervalMin() {
  return (uint16_t)cfg.intervalMin * cfg.senMult;
}

void svcList() {
  Serial.println("--- Aktualni nastaveni ---");
  svcVer();
  // Offsety: jeden radek na kazde pripojene cidlo. Format je pevny, protoze
  // ho parsuje i webovy konfigurator a podle nej si sklada ovladaci prvky:
  //   toff.<klic> = <hodnota> (<jmeno cidla>)
  for (uint8_t i = 0; i < TSRC_COUNT; i++)
    if (tsrcAvailable(i))
      Serial.printf("  toff.%-3s = %6.2f degC (%s)\n",
                    tsrcKey(i), cfg.tOff[i], tsrcName(i));
  for (uint8_t i = 0; i < HSRC_COUNT; i++)
    if (hsrcAvailable(i))
      Serial.printf("  hoff.%-3s = %6.2f %%RH  (%s)\n",
                    hsrcKey(i), cfg.hOff[i], hsrcName(i));
  Serial.printf("  poff = %.2f hPa\n", cfg.pressOff);
  Serial.printf("  alt  = %.1f m\n",   cfg.altitude);
  if (det.scd41 || senHasCo2()) {
    Serial.printf("  asc  = %d\n", cfg.scdAsc);
    if (cfg.scdAsc)
      Serial.println("  POZOR: samokalibrace pri uspavani cidla nefunguje"
                     " (datasheet). Doporuceno asc=0 + obcas co2ref=420.");
  }
  if (det.sen6x) {
    Serial.printf("  cidlo    = %s  sn %s\n",
                  senKindName(senKind), senSerial[0] ? senSerial : "?");
    Serial.printf("  senwarm  = %d s\n", cfg.senWarmS);
    Serial.printf("  senmult  = %d\n",   cfg.senMult);
    Serial.printf("  senauto  = %d dni\n", cfg.senCleanDays);
    Serial.printf("  lsleep   = %d\n",   cfg.lightSleep);
    Serial.printf("  senint   = %d min (PM a CO2)\n", senIntervalMin());
    Serial.printf("  odber SEN6x: ~%.0f mAh/den (cidlo bezi %.1f %% casu)\n",
                  senDailyMah(),
                  100.0f * (cfg.senWarmS + 3.0f) / (senIntervalMin() * 60.0f));
    if (cfg.senCleanDays)
      Serial.printf("  od posledniho cisteni: %.1f dne\n", senCleanMin / 1440.0f);
  }
  Serial.printf("  int  = %d min\n", cfg.intervalMin);
  svcSources();
  // Zdroje teploty a vlhkosti - at je videt, ktere cidlo hodnotu opravdu dodava
  Serial.printf("  zdroje: teplota=%s vlhkost=%s co2=%s tlak=%s\n",
                srcTempName(), srcHumName(), srcCo2Name(), srcPressName());
  svcAvailable();
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
  Serial.printf("#fw=%s\n", fwVersionFull());
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
  Quantity sel[MAX_CHANNELS];
  for (uint8_t i = 0; i < MAX_CHANNELS; i++) sel[i] = Q_NONE;
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
      // Velicina, na kterou zrovna ukazuje temp/hum, se nenabizi - jinak by
      // se do kanalu dostalo neco, co konfigurator nezna a pri prvnim
      // "Pouzit" by to tise vypadlo.
      if (!qOffered(q)) {
        const char *alias = (qCanonical(Q_TEMP) == q) ? qKey(Q_TEMP) : qKey(Q_HUM);
        Serial.printf("%s je ted totez co '%s' - pouzij '%s'.\n",
                      part.c_str(), alias, alias);
        return;
      }
      // Duplicita muze byt i skryta: "temp" je pri tsrc=sen tataz vec jako
      // "temp.sen", jen pod jinym jmenem.
      for (uint8_t i = 0; i < n; i++)
        if (qCanonical(sel[i]) == qCanonical(q)) {
          Serial.printf("%s je tataz velicina jako %s - staci jednou.\n",
                        part.c_str(), qKey(sel[i]));
          return;
        }
      sel[n++] = q;
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  if (n == 0) { Serial.println("Zadny platny kanal."); return; }
  if (n >= MAX_CHANNELS && v.indexOf(',', start) >= 0)
    Serial.printf("POZOR: do grafu jde nejvyse %d velicin, zbytek se ignoruje.\n",
                  MAX_CHANNELS);
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
  if (det.sen6x)
    Serial.printf("PM a CO2 se pri nasobku %dx meri kazdych %d min,"
                  " odber ~%.0f mAh/den.\n",
                  cfg.senMult, senIntervalMin(), senDailyMah());
}

bool svcHandle(String line, bool &dirty) {
  line.trim();
  if (!line.length()) return false;
  String low = line; low.toLowerCase();

  if (low == "help" || low == "?") { svcHelp(); return false; }
  if (low == "list")  { svcList(); return false; }
  if (low == "ver" || low == "version") { svcVer(); return false; }
  if (low == "dump")  { svcDump(); return false; }
  if (low == "senstat") {
    if (!det.sen6x) { Serial.println("SEN6x neni pripojen."); return false; }
    Serial.printf("SEN6x %s sn %s, stav: %s\n",
                  senName[0] ? senName : "?", senSerial[0] ? senSerial : "?",
                  sen6xStatusText());
    Serial.printf("Meri: PM1/2.5/4/10%s%s, RH/T\n",
                  senHasCo2Kind(senKind)  ? ", CO2"  : "",
                  senHasHchoKind(senKind) ? ", HCHO" : "");
    if (senHasGasKind(senKind))
      Serial.println("VOC/NOx cidlo ma, ale firmware je necte - jejich index"
                     " potrebuje nepretrzity beh, ktery se s uspavanim vylucuje.");
    return false;
  }
  if (low == "senclean") {
    if (!det.sen6x) { Serial.println("SEN6x neni pripojen."); return false; }
    pendingFanClean = true;
    Serial.println("senclean: ventilator se procisti pri dalsim mereni (~10 s).");
    return false;
  }
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

    // toff / hoff s tecknou: konkretni cidlo (toff.sht). Bez tecky se nastavi
    // to cidlo, ktere hodnotu opravdu dodava - at stary zpusob dal funguje.
    if (key.startsWith("toff")) {
      if (fabsf(f) > 20.0f) { Serial.println("Rozsah -20 az 20. Nezmeneno."); return false; }
      uint8_t idx = TSRC_COUNT;
      if (key == "toff") idx = tsrcPrimary();
      else if (key.charAt(4) == '.') {
        String k = key.substring(5);
        for (uint8_t i = 0; i < TSRC_COUNT; i++) if (k == tsrcKey(i)) { idx = i; break; }
      }
      if (idx >= TSRC_COUNT) {
        Serial.println("Neznamy zdroj teploty. Pouzij 'list' pro seznam.");
        return false;
      }
      if (!tsrcAvailable(idx)) {
        Serial.printf("Cidlo %s neni pripojene. Nezmeneno.\n", tsrcName(idx));
        return false;
      }
      cfg.tOff[idx] = f; dirty = true;
      Serial.printf("toff.%s=%.2f (%s)\n", tsrcKey(idx), f, tsrcName(idx));
    } else if (key.startsWith("hoff")) {
      if (fabsf(f) > 30.0f) { Serial.println("Rozsah -30 az 30. Nezmeneno."); return false; }
      uint8_t idx = HSRC_COUNT;
      if (key == "hoff") idx = hsrcPrimary();
      else if (key.charAt(4) == '.') {
        String k = key.substring(5);
        for (uint8_t i = 0; i < HSRC_COUNT; i++) if (k == hsrcKey(i)) { idx = i; break; }
      }
      if (idx >= HSRC_COUNT) {
        Serial.println("Neznamy zdroj vlhkosti. Pouzij 'list' pro seznam.");
        return false;
      }
      if (!hsrcAvailable(idx)) {
        Serial.printf("Cidlo %s neni pripojene. Nezmeneno.\n", hsrcName(idx));
        return false;
      }
      cfg.hOff[idx] = f; dirty = true;
      Serial.printf("hoff.%s=%.2f (%s)\n", hsrcKey(idx), f, hsrcName(idx));
    } else if (key == "poff") {
      if (fabsf(f) > 50.0f) { Serial.println("Rozsah -50 az 50. Nezmeneno."); return false; }
      cfg.pressOff = f; dirty = true; Serial.printf("poff=%.2f\n", f);
    } else if (key == "alt") {
      if (f < 0 || f > 4000) { Serial.println("Rozsah 0 az 4000. Nezmeneno."); return false; }
      cfg.altitude = f; dirty = true; Serial.printf("alt=%.1f\n", f);
    } else if (key == "asc") {
      cfg.scdAsc = val.toInt() ? 1 : 0; dirty = true;
      Serial.printf("asc=%d\n", cfg.scdAsc);
      if (det.sen6x && !det.scd41 && cfg.scdAsc)
        Serial.println("POZOR: samokalibrace SEN6x pocita s nepretrzitym behem."
                       " Pri mereni po intervalech ji radeji vypnete (asc=0)"
                       " a jednou za cas pouzijte co2ref.");
    } else if (key == "co2ref") {
      // Bez CO2 cidla by pozadavek jen tise zapadl - deep sleep ho stejne
      // zahodi, pendingCo2Ref neni v RTC RAM.
      if (!det.scd41 && !senHasCo2()) {
        Serial.println("V sestave neni zadne cidlo CO2. Nezmeneno.");
        return false;
      }
      int c = val.toInt();
      if (c < 300 || c > 2000) { Serial.println("Rozsah 300 az 2000 ppm. Nezmeneno."); return false; }
      pendingCo2Ref = (int16_t)c;
      Serial.printf("co2ref=%d (provede se pri dalsim mereni)\n", c);
      if (det.sen6x && !det.scd41)
        Serial.println("Cidlo kvuli tomu jednorazove pobezi 3,5 minuty.");
    } else if (key == "senwarm") {
      int s = val.toInt();
      if (s < SEN_WARM_LO || s > SEN_WARM_HI) {
        Serial.printf("Rozsah %d az %d s. Nezmeneno.\n", SEN_WARM_LO, SEN_WARM_HI);
        return false;
      }
      cfg.senWarmS = (uint8_t)s; dirty = true;
      Serial.printf("senwarm=%d s -> odber SEN6x ~%.0f mAh/den\n", s, senDailyMah());
      // Kazda velicina se "rozjizdi" jinak dlouho (datasheet SEN6x):
      // PM ~30 s, CO2 u SEN63C 22-24 s, u SEN66 5-6 s.
      if (s < 30) Serial.println("POZOR: datasheet zada 30 s, nez je PM platne."
                                 " Kratsi doba znamena podhodnocene hodnoty.");
      if (s < 25 && senHasCo2() && !det.scd41)
        Serial.println("POZOR: CO2 potrebuje az 24 s. Pod tuto hranici ho cidlo"
                       " hlasi jako neplatne a v grafu bude mezera.");
    } else if (key == "senmult") {
      int n = val.toInt();
      if (n < 1 || n > SEN_MULT_HI) {
        Serial.printf("Rozsah 1 az %d. Nezmeneno.\n", SEN_MULT_HI);
        return false;
      }
      cfg.senMult = (uint8_t)n; dirty = true;
      Serial.printf("senmult=%dx -> teplota a vlhkost kazdych %d min,"
                    " PM a CO2 kazdych %d min\n",
                    n, cfg.intervalMin, senIntervalMin());
      Serial.printf("Odber SEN6x ~%.0f mAh/den.\n", senDailyMah());
      if (n > 1) Serial.println("Vzorky mezi merenimi maji u PM a CO2 prazdno,"
                                " graf je v tech mistech prerusovany.");
    } else if (key == "senauto") {
      int d = val.toInt();
      if (d < 0 || d > SEN_CLEAN_HI) {
        Serial.printf("Rozsah 0 az %d dni. Nezmeneno.\n", SEN_CLEAN_HI);
        return false;
      }
      cfg.senCleanDays = (uint8_t)d; dirty = true;
      if (d) Serial.printf("senauto=%d dni (od posledniho cisteni ubehlo %.1f dne)\n",
                           d, senCleanMin / 1440.0f);
      else   Serial.println("senauto=0 - automaticke cisteni vypnuto.");
    } else if (key == "lsleep") {
      cfg.lightSleep = val.toInt() ? 1 : 0; dirty = true;
      Serial.printf("lsleep=%d\n", cfg.lightSleep);
    } else if (key == "tsrc" || key == "hsrc") {
      bool isT = (key == "tsrc");
      String v2 = val; v2.toLowerCase();
      uint8_t cnt = isT ? (uint8_t)TSRC_COUNT : (uint8_t)HSRC_COUNT;
      uint8_t idx = cnt;                       // cnt = automaticky
      if (v2 != "auto") {
        idx = cnt + 1;                         // priznak "nenalezeno"
        for (uint8_t i = 0; i < cnt; i++)
          if (v2 == (isT ? tsrcKey(i) : hsrcKey(i))) { idx = i; break; }
        if (idx > cnt) {
          Serial.println("Neznamy zdroj. Napis 'list' pro seznam moznosti.");
          return false;
        }
        bool ok = isT ? tsrcAvailable(idx) : hsrcAvailable(idx);
        if (!ok) {
          Serial.printf("Cidlo %s neni pripojene. Nezmeneno.\n",
                        isT ? tsrcName(idx) : hsrcName(idx));
          return false;
        }
      }
      uint8_t cur = isT ? cfg.tSrcPref : cfg.hSrcPref;
      if (idx != cur) {
        if (isT) cfg.tSrcPref = idx; else cfg.hSrcPref = idx;
        dirty = true;
        // Vzorky "temp" / "hum" v historii pochazeji z predchoziho cidla,
        // takze uz neplati. Je to vedoma zmena, historii zakladame znovu.
        // Potvrzeni beze zmeny historii nechava byt.
        histClear();
        histEraseNVS();
        // Zmenil se i vyznam velicin, takze se kanaly musi poskladat znovu -
        // jinak by "list" hlasil kanal, ktery uz neni v nabidce.
        buildChannels();
      }
      Serial.printf("%s=%s -> hlavni %s bere ze %s\n",
                    key.c_str(), (idx >= cnt) ? "auto" : (isT ? tsrcKey(idx) : hsrcKey(idx)),
                    isT ? "teplotu" : "vlhkost",
                    isT ? srcTempName() : srcHumName());
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
<div class="c"><div class="k">Firmware</div><div class="v" id="vF" style="font-size:14px">—</div></div>
<div class="c hd" id="cSen"><div class="k">Odběr čidla PM</div><div class="v" id="vSen" style="font-size:14px">—</div></div>
</div>
<div class="cl" style="margin-top:10px">
<div><span>Teplota z</span><b id="sT">—</b></div>
<div><span>Vlhkost z</span><b id="sH">—</b></div>
<div><span>CO2 z</span><b id="sC">—</b></div>
<div><span>Tlak z</span><b id="sP">—</b></div>
</div>
</div></div>

<div class="p hd" id="pCo2"><h2>CO2</h2><div class="b">
<div class="r"><label>Automatická samokalibrace</label>
<input type=checkbox id=asc style="width:20px;height:20px;accent-color:var(--ac)"></div>
<p class="wn" id="ascwn"></p>
<div class="r"><label>Kalibrovat na hodnotu [ppm]</label>
<input type=number id=co2ref step=5 min=300 max=2000 value=420></div>
<div class="acts"><button class="pr" onclick="applyCo2(this)">Použít</button>
<button onclick="calCo2(this)">Kalibrovat</button></div>
<p class="n">Kalibrace se provede při dalším měření. Nechte desku aspoň
3 minuty v ustáleném prostředí - venku na čerstvém vzduchu odpovídá 420 ppm.
Kalibrace přežije odpojení napájení, takže stačí jednou za čas.</p>
</div></div>

<div class="p hd" id="pSen"><h2>Čidlo prachu SEN6x</h2><div class="b">
<p class="n" style="margin:0 0 12px">Teplota, vlhkost a tlak se měří v základním
intervalu. Prach a CO2 mají vlastní, delší interval - jeho násobek nastavíte tady.</p>
<div class="r"><label>Interval PM a CO2</label>
<div class="iv" id="smb"></div></div>
<div class="cl">
<div><span>Teplota a vlhkost každých</span><b id="cT">—</b></div>
<div><span>PM a CO2 každých</span><b id="cP">—</b></div>
</div>
<div class="r" style="margin-top:14px"><label>Doba běhu před odečtem [s]</label>
<input type=number id=senwarm step=5 min=5 max=120></div>
<div class="r"><label>Automatické čištění po [dnech]<br><span style="font-size:12px">0 = vypnuto</span></label>
<input type=number id=senauto step=1 min=0 max=90></div>
<div class="r"><label>Uspat desku během zahřívání</label>
<input type=checkbox id=lsleep style="width:20px;height:20px;accent-color:var(--ac)"></div>
<p class="wn" id="senwn"></p>
<div class="acts"><button class="pr" onclick="applySen(this)">Použít</button>
<button onclick="req('/api/senclean',this,'Naplánováno ✓')">Pročistit hned</button></div>
<p class="n">Ventilátor čidla bere 90 mA a datasheet žádá 30 s běhu, než je PM
platné. To je zdaleka největší položka spotřeby celé stanice.</p>
</div></div>

<div class="p"><h2>Kompenzace</h2><div class="b">
<p class="n" style="margin:0 0 10px">Každé čidlo se mýlí jinak, proto má vlastní
korekci. Nastavte tu, kterou právě porovnáváte s referenčním přístrojem.</p>
<div id="toffs"></div>
<div id="hoffs" style="margin-top:12px"></div>
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

<div class="p"><h2>Hlavní teplota a vlhkost</h2><div class="b">
<p class="n" style="margin:0 0 10px">Které čidlo dodává velkou hodnotu na displeji
a veličiny <b>Teplota</b> a <b>Vlhkost</b>. Automaticky se bere první dostupné
podle priority; ruční volba ji přebije.</p>
<div class="r"><label>Teplota z čidla</label><div class="iv" id="tsb"></div></div>
<div class="r"><label>Vlhkost z čidla</label><div class="iv" id="hsb"></div></div>
<p class="wn">Změna zdroje založí novou historii - vzorky z předchozího čidla
už by neplatily.</p>
<div class="acts"><button class="pr" onclick="applySrc(this)">Použít</button></div>
</div></div>

<div class="p"><h2>Veličiny v grafu</h2><div class="b">
<p class="n" style="margin:0 0 10px">Kromě hlavní teploty a vlhkosti jde do grafu
dát i každé čidlo zvlášť - třeba teplotu ze SHT40 a ze SEN63C vedle sebe.</p>
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
// Q se sklada z JSON ("quants") - stranka nemusi o cidlech nic vedet dopredu.
var S={},Q=[],TS="auto",HS="auto";
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
// Deska uklada nejvyse MAX_CHANNELS veliciny. Kdyby se tady pocitalo s vic,
// ukazovala by stranka delku historie, ktera nikdy nenastane - firmware
// v /api/set stejne prebere jen prvnich MAXCH.
var MAXCH=4;
function nch(){if(g("auto").checked)return S.autoCount||2;
var n=0;Q.forEach(function(q){var e=g("q_"+q.k);if(e&&e.checked)n++});
return Math.min(n||1,MAXCH)}
function calc(){var iv=+g("ivv").value||5,n=Math.min(Math.floor(POOL/nch()),CAP),m=n*iv;
g("cH").textContent=m<60?m+" min":(m<2880?(m/60).toFixed(1)+" h":(m/1440).toFixed(1)+" dne");
g("cC").textContent=n+" × "+nch()+" kan.";
var w=[];if(iv<5)w.push("Krátký interval výrazně zkrátí výdrž baterie.");
if(m<1440)w.push("Historie nepokryje ani celý den.");
if(S.sen)w.push("S čidlem PM odběr ~"+Math.round(senMah())+" mAh/den.");
g("wn").textContent=w.join(" ");g("wn").className=w.length?"wn":"wn hd";senUpd()}
var SM=1;
function fmtMin(m){return m<60?m+" min":(m%60?(m/60).toFixed(1)+" h":(m/60)+" h")}
function senIv(){return (+g("ivv").value||5)*SM}
function senMah(){if(!S.sen)return 0;
return 90*(((+g("senwarm").value||30)+3)/3600)*(1440/senIv())}
function senUpd(){if(!S.sen)return;
var m=senMah();g("vSen").textContent=Math.round(m)+" mAh/den";
g("cT").textContent=fmtMin(+g("ivv").value||5);g("cP").textContent=fmtMin(senIv());
Array.prototype.forEach.call(g("smb").querySelectorAll("button"),function(b){
b.className=(+b.dataset.m===SM)?"on":""});
var w=[];if(m>100)w.push("Při tomto nastavení vydrží běžný akumulátor 2000 mAh jen "+(2000/m).toFixed(1)+" dne.");
if((+g("senwarm").value||30)<30)w.push("Datasheet žádá 30 s, jinak je PM podhodnocené.");
if(SM>1)w.push("PM a CO2 mají v historii mezi měřeními prázdno.");
g("senwn").textContent=w.join(" ")}
function setSM(n){SM=n;senUpd()}
// Nabidka velicin se sklada z toho, co deska posila v "quants".
function qGrid(){var nq=S.quants||[];
var same=nq.length===Q.length&&nq.every(function(o,i){return Q[i]&&Q[i].k===o.k&&Q[i].n===o.n});
Q=nq;if(same)return;
var h="";Q.forEach(function(x){
h+='<label><input type=checkbox id="q_'+x.k+'" onchange="qPick(this)">'+x.n+"</label>"});
g("qg").innerHTML=h||"<span>Zatím nic k měření</span>"}
// Tlacitka pro volbu hlavniho zdroje teploty a vlhkosti.
function srcBtns(){
[["tsb","tsrcs","TS"],["hsb","hsrcs","HS"]].forEach(function(cfg2){
var box=g(cfg2[0]),list=(S[cfg2[1]]||[]),cur=(cfg2[2]==="TS"?TS:HS);
var h='<button data-k="auto" onclick="setSrc(\''+cfg2[2]+'\',\'auto\')">auto</button>';
list.forEach(function(o){
h+='<button data-k="'+o.k+'" onclick="setSrc(\''+cfg2[2]+'\',\''+o.k+'\')">'+o.n+"</button>"});
if(box.innerHTML!==h)box.innerHTML=h;
Array.prototype.forEach.call(box.querySelectorAll("button"),function(b){
b.className=(b.dataset.k===cur)?"on":""})})}
function setSrc(which,k){if(which==="TS")TS=k;else HS=k;srcBtns()}
function applySrc(b){
if(!confirm("Změna zdroje založí novou historii. Pokračovat?"))return;
req("/api/set?tsrc="+TS+"&hsrc="+HS,b,"Použito ✓")}
function applyCo2(b){req("/api/set?asc="+(g("asc").checked?1:0),b,"Použito ✓")}
function calCo2(b){var v=+g("co2ref").value||420;
if(v<300||v>2000){toast("Rozsah 300 až 2000 ppm",1);return}
req("/api/set?co2ref="+v,b,"Naplánováno ✓")}
function applySen(b){req("/api/set?senwarm="+(+g("senwarm").value||30)
+"&senmult="+SM+"&senauto="+(+g("senauto").value||0)
+"&lsleep="+(g("lsleep").checked?1:0),b,"Použito ✓")}
function draw(){g("vS").textContent=S.sensors||"—";g("vI").textContent=S.interval+" min";
g("vH").textContent=S.count+"/"+S.perCh;g("vB").textContent=(S.vbat||0).toFixed(2)+" V";
g("vF").textContent="v"+(S.fw||"?");
["poff","alt"].forEach(function(k){if(document.activeElement!==g(k))g(k).value=S[k]});
offRows("toffs",S.toffs||[],"t","°C",.1,20);offRows("hoffs",S.hoffs||[],"h","%RH",.5,30);
var hasP=(S.srcP||"-")!=="-";
g("rP").className=hasP?"r":"r hd";g("rA").className=hasP?"r":"r hd";
g("pSen").className=S.sen?"p":"p hd";g("cSen").className=S.sen?"c":"c hd";
g("pCo2").className=((S.srcC||"-")!=="-")?"p":"p hd";
g("asc").checked=!!S.asc;
// Samokalibrace pocita s nepretrzitym behem, ktery se s uspavanim vylucuje.
g("ascwn").textContent=S.asc?"Samokalibrace hleda tydenni minimum a potřebuje"
+" nepřetržitý běh. Při měření po intervalech ji raději vypněte a jednou za čas"
+" použijte kalibraci na 420 ppm.":"";
g("sT").textContent=S.srcT||"—";g("sH").textContent=S.srcH||"—";
g("sC").textContent=S.srcC||"—";g("sP").textContent=S.srcP||"—";
if(S.sen){if(document.activeElement!==g("senwarm"))g("senwarm").value=S.senwarm;
if(document.activeElement!==g("senauto"))g("senauto").value=S.senauto;
SM=S.senmult||1;g("lsleep").checked=!!S.lsleep}
TS=S.tsrc||"auto";HS=S.hsrc||"auto";
g("auto").checked=!!S.chAuto;
qGrid();
Q.forEach(function(q){var e=g("q_"+q.k);if(e)e.checked=(S.channels||[]).indexOf(q.k)>=0});
srcBtns();
g("ivv").value=S.interval;
Array.prototype.forEach.call(document.querySelectorAll(".iv button"),function(b){
b.className=(+b.dataset.v===S.interval)?"on":""});qUpd();calc();senUpd()}
function qUpd(){var a=g("auto").checked;
Array.prototype.forEach.call(g("qg").querySelectorAll("input"),function(i){i.disabled=a});calc()}
// Jeden radek na kazde pripojene cidlo. Firmware posila seznam v JSON,
// stranka o cidlech nemusi nic vedet dopredu.
function offRows(box,list,pre,unit,step,lim){var e=g(box);
if(!list.length){e.innerHTML="";return}
var h="";list.forEach(function(o){
h+='<div class="r"><label>'+(pre=="t"?"Teplota":"Vlhkost")+" &middot; "+o.n+" ["+unit+']</label>'
+'<input type=number id="o_'+pre+'_'+o.k+'" step='+step+' min=-'+lim+' max='+lim+'></div>'});
if(e.innerHTML!==h)e.innerHTML=h;
list.forEach(function(o){var i=g("o_"+pre+"_"+o.k);
if(i&&document.activeElement!==i)i.value=o.v})}
function offParams(pre,list){return (list||[]).map(function(o){
var i=g("o_"+pre+"_"+o.k);return pre+"off."+o.k+"="+(i?(+i.value||0):0)})}
function apply(b){var p=["poff","alt"].map(function(k){return k+"="+(+g(k).value||0)})
.concat(offParams("t",S.toffs)).concat(offParams("h",S.hoffs));
req("/api/set?"+p.join("&"),b,"Použito ✓")}
// Zaskrtnuti nad limit se hned vrati zpet - jinak by uzivatel odeslal vyber,
// z ktereho deska tise pouzije jen prvnich MAXCH.
function qPick(el){
var n=0;Q.forEach(function(q){var e=g("q_"+q.k);if(e&&e.checked)n++});
if(n>MAXCH){el.checked=false;toast("Nejvýše "+MAXCH+" veličiny",1)}
calc()}
function applyCh(b){var s=g("auto").checked?"auto":
Q.filter(function(q){var e=g("q_"+q.k);return e&&e.checked}).map(function(q){return q.k}).join(",");
if(s!="auto"&&!s){toast("Vyberte aspoň jednu veličinu",1);return}
req("/api/set?ch="+s,b,"Použito ✓")}
function setIv(v,b){req("/api/set?int="+v,b,"Použito ✓")}
function clr(b){if(!confirm("Opravdu smazat celou historii měření?"))return;
req("/api/clear",b,"Smazáno ✓")}
function fin(b){ok(b,"Vypínám…");fetch("/api/exit").then(function(){
document.body.innerHTML='<div class="w"><h1>Hotovo</h1><p class="s">Hotspot se vypíná, deska pokračuje v měření. Tuto stránku můžete zavřít.</p></div>'})}
var GD=null;
// Jednotka se odvozuje z popisku - veliciny konkretnich cidel maji slozene
// nazvy ("Teplota SHT40") a do pevne tabulky se nevejdou.
var UNX={"CO2":"ppm","Tlak":"hPa","Baterie":"V","PM2.5":"\u00b5g/m\u00b3","PM10":"\u00b5g/m\u00b3"};
// Popisky jsou slozene ("Teplota SHT40", "CO2 SCD41"), takze se pozna
// jen predpona - jinak by veliciny s uvedenym cidlem zustaly bez jednotky.
function un(n){if(UNX[n])return UNX[n];
if(/^Teplota/i.test(n))return"\u00b0C";
if(/^Vlhkost/i.test(n))return"%";
if(/^PM/i.test(n))return"\u00b5g/m\u00b3";
if(/^CO2/i.test(n))return"ppm";
if(/^Tlak/i.test(n))return"hPa";return""}
function isCo2(n){return /^CO2/i.test(n)}
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
  var u=un(n);d.innerHTML='<h5>'+n+(u?" ["+u+"]":"")+" · "+GD.rows.length+" vz.</h5>";
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
 // Prach byva v desetinach ug/m3. Osa proto zacina na nule a horni mez se
 // odviji od dat - jinak by prubeh splynul s dolni hranou a nebylo by nic vidět.
 var pm=/^PM/.test(GD.cols[ci]||"");
 if(pm){mn=0;mx=Math.max(mx*1.25,0.5)}
 else{var sp=mx-mn,pv=sp<1e-6?(Math.abs(mx)*0.05||1):sp*0.12;mn-=pv;mx+=pv}
 if(mx-mn<1e-9)mx=mn+1;
 var dec=(mx-mn)<2?2:((mx-mn)<5?1:0);
 x.font="10px ui-monospace,monospace";x.textAlign="right";x.lineWidth=1;
 for(var i=0;i<=3;i++){var v=mn+(mx-mn)*i/3,yy=Math.round(pt+gh-gh*i/3)+0.5;
  x.strokeStyle="#2f353f";x.globalAlpha=.4;x.beginPath();x.moveTo(pl,yy);x.lineTo(pl+gw,yy);x.stroke();
  x.globalAlpha=1;x.fillStyle="#6b7482";x.fillText(v.toFixed(dec),pl-5,yy+3)}
 x.textAlign="center";
 for(var i=0;i<=2;i++){var idx=Math.round((GD.rows.length-1)*i/2);
  x.fillText(fb(GD.rows[idx][0]),Math.min(Math.max(pl+gw*i/2,22),w-22),h-4)}
 var XP=function(i){return pl+(GD.rows.length<=1?gw:gw*i/(GD.rows.length-1))};
 var YP=function(v){return pt+gh-(v-mn)/(mx-mn)*gh};
 x.strokeStyle="#ffb524";x.lineWidth=2;x.lineJoin="round";x.beginPath();var st=false;
 GD.rows.forEach(function(r,i){var v=r[ci+1];if(v===null||isNaN(v)){st=false;return}
  var px=XP(i),py=YP(v);if(!st){x.moveTo(px,py);st=true}else x.lineTo(px,py)});
 x.stroke();
 // Osamocené vzorky: prach a CO2 se při násobku intervalu měří řidčeji, takže
 // mezi nimi jsou mezery. Samotný bod nemá s čím spojit čáru a bez puntíku by
 // se nevykreslil vůbec - graf pak vypadá prázdný, i když data jsou.
 x.fillStyle="#ffb524";
 GD.rows.forEach(function(r,i){var v=r[ci+1];if(v===null||isNaN(v))return;
  var pv2=i>0?GD.rows[i-1][ci+1]:null,nv=i<GD.rows.length-1?GD.rows[i+1][ci+1]:null;
  var lone=(pv2===null||isNaN(pv2))&&(nv===null||isNaN(nv));
  if(lone){x.beginPath();x.arc(XP(i),YP(v),2.5,0,6.284);x.fill()}});
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
 GD.cols.forEach(function(n,ci){var v=r[ci+1],u=un(n);
  html+="<span>"+n+": <b>"+(v===null||isNaN(v)?"—":(isCo2(n)?Math.round(v):v.toFixed(2)))+(u?" "+u:"")+"</b></span>"});
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
var s="";[1,2,3,4].forEach(function(n){
s+='<button data-m="'+n+'" onclick="setSM('+n+')">'+n+'x</button>'});
g("smb").innerHTML=s;
req("/api/status")})();
setInterval(function(){fetch("/api/ping")},60000);
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// JSON odpoved se stavem
// ---------------------------------------------------------------------------
String apStatusJson(float vbat) {
  String j = "{";
  j += "\"fw\":\"" + String(fwVersionFull()) + "\",";
  j += "\"sen\":" + String(det.sen6x ? 1 : 0) + ",";
  j += "\"senwarm\":" + String(cfg.senWarmS) + ",";
  j += "\"senmult\":" + String(cfg.senMult) + ",";
  j += "\"senauto\":" + String(cfg.senCleanDays) + ",";
  j += "\"lsleep\":" + String(cfg.lightSleep) + ",";
  j += "\"srcT\":\""  + String(srcTempName())  + "\",";
  j += "\"srcH\":\""  + String(srcHumName())   + "\",";
  j += "\"srcC\":\""  + String(srcCo2Name())   + "\",";
  j += "\"srcP\":\""  + String(srcPressName()) + "\",";
  j += "\"sensors\":\"" + String(sensorSummary()) + "\",";
  j += "\"interval\":" + String(cfg.intervalMin) + ",";
  j += "\"count\":" + String(histCount) + ",";
  j += "\"perCh\":" + String(histPerCh) + ",";
  // NAN by se do JSON vypsal jako "nan" a prohlizec by cely objekt odmitl.
  j += "\"vbat\":" + String(isnan(vbat) ? 0.0f : vbat, 2) + ",";
  // Offsety jako seznam - stranka si podle nej sama vyrobi ovladaci prvky
  // a nemusi o pripojenych cidlech nic vedet dopredu.
  j += "\"toffs\":[";
  {
    bool first = true;
    for (uint8_t i = 0; i < TSRC_COUNT; i++) {
      if (!tsrcAvailable(i)) continue;
      if (!first) j += ",";
      j += "{\"k\":\"" + String(tsrcKey(i)) + "\",\"n\":\"" + String(tsrcName(i)) +
           "\",\"v\":" + String(cfg.tOff[i], 2) + "}";
      first = false;
    }
  }
  j += "],\"hoffs\":[";
  {
    bool first = true;
    for (uint8_t i = 0; i < HSRC_COUNT; i++) {
      if (!hsrcAvailable(i)) continue;
      if (!first) j += ",";
      j += "{\"k\":\"" + String(hsrcKey(i)) + "\",\"n\":\"" + String(hsrcName(i)) +
           "\",\"v\":" + String(cfg.hOff[i], 2) + "}";
      first = false;
    }
  }
  j += "],";
  j += "\"poff\":" + String(cfg.pressOff, 2) + ",";
  j += "\"alt\":" + String(cfg.altitude, 0) + ",";
  j += "\"asc\":" + String(cfg.scdAsc) + ",";
  j += "\"chAuto\":" + String(cfg.chAuto) + ",";
  j += "\"autoCount\":" + String(channelCount) + ",";
  j += "\"channels\":[";
  for (uint8_t c = 0; c < channelCount; c++)
    j += String(c ? "," : "") + "\"" + qKey(channels[c].q) + "\"";
  // Seznam velicin, ktere je z ceho merit - stranka z nej sklada nabidku
  // kanalu a nemusi o cidlech nic vedet dopredu.
  j += "],\"quants\":[";
  {
    bool first = true;
    for (uint8_t i = 0; i < Q_ALL_COUNT; i++) {
      if (!qOffered(Q_ALL[i])) continue;
      if (!first) j += ",";
      j += "{\"k\":\"" + String(qKey(Q_ALL[i])) + "\",\"n\":\"" +
           String(qLabel(Q_ALL[i])) + "\"}";
      first = false;
    }
  }
  // Volba hlavniho zdroje teploty a vlhkosti
  j += "],\"tsrc\":\"" + String(cfg.tSrcPref < TSRC_COUNT ? tsrcKey(cfg.tSrcPref) : "auto") + "\",";
  j += "\"hsrc\":\"" + String(cfg.hSrcPref < HSRC_COUNT ? hsrcKey(cfg.hSrcPref) : "auto") + "\",";
  j += "\"tsrcs\":[";
  {
    bool first = true;
    for (uint8_t i = 0; i < TSRC_COUNT; i++) {
      if (!tsrcAvailable(i)) continue;
      if (!first) j += ",";
      j += "{\"k\":\"" + String(tsrcKey(i)) + "\",\"n\":\"" + String(tsrcName(i)) + "\"}";
      first = false;
    }
  }
  j += "],\"hsrcs\":[";
  {
    bool first = true;
    for (uint8_t i = 0; i < HSRC_COUNT; i++) {
      if (!hsrcAvailable(i)) continue;
      if (!first) j += ",";
      j += "{\"k\":\"" + String(hsrcKey(i)) + "\",\"n\":\"" + String(hsrcName(i)) + "\"}";
      first = false;
    }
  }
  j += "]}";
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

    useFont(&FreeSansBold18pt7b);
    display.setCursor(20, 50);
    display.print("Konfigurace WiFi");

    display.drawFastHLine(20, 66, W - 40, GxEPD_BLACK);

    useFont(&FreeSans9pt7b);
    display.setCursor(20, 96);
    display.print("Sit (SSID)");
    useFont(&FreeSansBold12pt7b);
    display.setCursor(20, 120);
    display.print(apSsid);

    useFont(&FreeSans9pt7b);
    display.setCursor(20, 152);
    display.print("Heslo");
    useFont(&FreeSansBold12pt7b);
    display.setCursor(20, 176);
    display.print(cfg.apPass);

    useFont(&FreeSans9pt7b);
    display.setCursor(20, 208);
    display.print("Adresa v prohlizeci");
    useFont(&FreeSansBold12pt7b);
    display.setCursor(20, 232);
    display.print("http://192.168.4.1");

    // QR kod pro pripojeni k siti (naskenujte fotoaparatem telefonu)
    const int qsize = QR_MODS;
    const int scale = 9;
    int qx = (W - qsize * scale) / 2;
    int qy = 300;
    drawQr(wifiQr, qx, qy, scale);

    useFont(&FreeSans9pt7b);
    display.setCursor(20, qy + qsize * scale + 34);
    display.print("Naskenujte QR fotoaparatem - telefon se pripoji k siti.");
    display.setCursor(20, qy + qsize * scale + 56);
    display.print("Pak otevrete http://192.168.4.1 (obvykle se otevre samo).");

    display.drawFastHLine(20, H - 72, W - 40, GxEPD_BLACK);
    useFont(&FreeSans9pt7b);
    display.setCursor(20, H - 48);
    display.print("Hotspot se vypne po 5 minutach necinnosti.");
    display.setCursor(20, H - 28);
    display.print("Pak deska pokracuje v mereni.");
    drawFooter();
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
    // toff.<klic> / hoff.<klic>; bez tecky se nastavi to cidlo, ktere hodnotu
    // opravdu dodava (stejne pravidlo jako v servisnim rezimu).
    if (k.startsWith("toff") && fabsf(f) <= 20.0f) {
      uint8_t idx = TSRC_COUNT;
      if (k == "toff") idx = tsrcPrimary();
      else if (k.charAt(4) == '.') {
        String kk = k.substring(5);
        for (uint8_t z = 0; z < TSRC_COUNT; z++) if (kk == tsrcKey(z)) { idx = z; break; }
      }
      if (idx < TSRC_COUNT && tsrcAvailable(idx)) { cfg.tOff[idx] = f; dirty = true; }
    }
    else if (k.startsWith("hoff") && fabsf(f) <= 30.0f) {
      uint8_t idx = HSRC_COUNT;
      if (k == "hoff") idx = hsrcPrimary();
      else if (k.charAt(4) == '.') {
        String kk = k.substring(5);
        for (uint8_t z = 0; z < HSRC_COUNT; z++) if (kk == hsrcKey(z)) { idx = z; break; }
      }
      if (idx < HSRC_COUNT && hsrcAvailable(idx)) { cfg.hOff[idx] = f; dirty = true; }
    }
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
    else if (k == "senwarm") {
      int s = v.toInt();
      if (s >= SEN_WARM_LO && s <= SEN_WARM_HI) { cfg.senWarmS = (uint8_t)s; dirty = true; }
    }
    else if (k == "senmult") {
      int n = v.toInt();
      if (n >= 1 && n <= SEN_MULT_HI) { cfg.senMult = (uint8_t)n; dirty = true; }
    }
    else if (k == "senauto") {
      int d = v.toInt();
      if (d >= 0 && d <= SEN_CLEAN_HI) { cfg.senCleanDays = (uint8_t)d; dirty = true; }
    }
    else if (k == "lsleep") { cfg.lightSleep = v.toInt() ? 1 : 0; dirty = true; }
    else if (k == "tsrc" || k == "hsrc") {
      bool isT = (k == "tsrc");
      uint8_t cnt = isT ? (uint8_t)TSRC_COUNT : (uint8_t)HSRC_COUNT;
      uint8_t idx = cnt;                       // cnt = automaticky
      if (v != "auto") {
        idx = cnt + 1;
        for (uint8_t z = 0; z < cnt; z++)
          if (v == (isT ? tsrcKey(z) : hsrcKey(z))) { idx = z; break; }
      }
      bool ok = (idx == cnt) ||
                (idx < cnt && (isT ? tsrcAvailable(idx) : hsrcAvailable(idx)));
      uint8_t cur = isT ? cfg.tSrcPref : cfg.hSrcPref;
      if (ok && idx != cur) {
        if (isT) cfg.tSrcPref = idx; else cfg.hSrcPref = idx;
        dirty = true;
        // Vzorky v historii pochazeji z predchoziho cidla, uz neplati.
        histClear();
        histEraseNVS();
      }
    }
    else if (k == "ch") {
      if (v == "auto") {
        cfg.chAuto = 1;
        for (uint8_t x = 0; x < MAX_CHANNELS; x++) cfg.chSel[x] = Q_NONE;
        dirty = true;
      } else {
        Quantity sel[MAX_CHANNELS];
        for (uint8_t z = 0; z < MAX_CHANNELS; z++) sel[z] = Q_NONE;
        uint8_t n = 0; int start = 0;
        while (start <= (int)v.length() && n < MAX_CHANNELS) {
          int comma = v.indexOf(',', start);
          String part = (comma < 0) ? v.substring(start) : v.substring(start, comma);
          part.trim(); part.toLowerCase();
          if (part.length()) {
            Quantity q = qFromKey(part);
            // Stejne pravidlo jako v servisu: duplicita muze byt skryta,
            // "temp" je pri tsrc=sen tataz vec jako "temp.sen".
            if (q != Q_NONE && qOffered(q)) {
              bool dup = false;
              for (uint8_t z = 0; z < n; z++)
                if (qCanonical(sel[z]) == qCanonical(q)) dup = true;
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
  httpd.on("/api/senclean", [](){
    apTouch();
    pendingFanClean = det.sen6x;     // provede se az pri dalsim mereni
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
    String csv = "#fw=" + String(fwVersionFull()) + "\n";
    csv += "min_zpet";
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
  sen6xStop();                    // pojistka, kdyby cidlo z nejakeho duvodu bezelo
  powerOff();
  uint8_t iv = cfg.intervalMin;
  if (iv < INTERVAL_MIN_LO || iv > INTERVAL_MIN_HI) iv = DEF_INTERVAL;  // pojistka

  // Od intervalu odecteme dobu, kterou deska stravila vzhuru. Se SEN6x je to
  // pres pul minuty a bez korekce by se vzorky rozjizdely - casova osa grafu
  // pocita s presnym rozestupem cfg.intervalMin.
  uint64_t period = (uint64_t)iv * 60ULL * uS_TO_S;
  uint64_t awake  = (uint64_t)millis() * 1000ULL;
  uint64_t sleepUs = (awake + 5ULL * uS_TO_S < period) ? (period - awake)
                                                       : (5ULL * uS_TO_S);
  Serial.printf("Vzhuru %.1f s, spanek %.1f s.\n",
                awake / 1e6, sleepUs / 1e6);
  Serial.flush();

  esp_sleep_enable_timer_wakeup(sleepUs);
  esp_deep_sleep_start();
}

// ============================================================================
// NACTENI A VALIDACE HISTORIE
// Musi probehnout PRED servisnim rezimem i hotspotem - jinak by prikazy
// list a dump videly prazdnou historii, i kdyz data v pameti jsou.
// ============================================================================
void loadHistory(uint32_t sig) {
  bool pendingKeepMsg = false;
  if (!rtcInited) {
    // Studeny start nebo reset: RTC RAM je prazdna, zkusime NVS.
    // Pocitadlo cisteni ventilatoru prezije i vypadek napajeni.
    if (prefs.begin(NVS_NS, true)) {
      senCleanMin = prefs.getUInt(NVS_CLEAN, 0);
      prefs.end();
    }
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
      // Hlaska prijde az za pojistkou nize - kdyz se zmenil pocet kanalu,
      // zmenilo se i histPerCh a pojistka historii stejne zalozi znovu.
      // Tvrdit tady "zachovana" by bylo matouci.
      pendingKeepMsg = true;
    }
  }
  // pojistka proti poskozenym indexum
  bool wiped = false;
  if (histPerCh == 0 || histHead >= histPerCh || histCount > histPerCh ||
      histSlots != histPerCh) {
    histClear();
    wiped = true;
  }
  if (pendingKeepMsg) {
    Serial.println(wiped
      ? "Odlisna sestava (1. vyskyt) - zmenilo se rozlozeni, historie zalozena znovu."
      : "Odlisna sestava (1. vyskyt) - historie zachovana.");
    pendingKeepMsg = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(50);

  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  bool hardStart = (wake != ESP_SLEEP_WAKEUP_TIMER);

  // Light sleep behem zahrivani SEN6x jen pri probuzeni casovacem. Pri startu
  // z tlacitka nebo po nahrani firmwaru byva pripojene USB a uspani ESP32
  // by shodilo seriovou konzoli uprostred vypisu.
  allowLightSleep = !hardStart;

  Serial.printf("\n%s v%s (build %s)\n", FW_NAME, FW_VERSION, fwBuildDate());

  powerOn();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  // SEN6x zvlada nejvys 100 kbit/s. Ostatni cidla na desce se stejnou
  // rychlosti nemaji problem, tak ji nastavime napevno.
  Wire.setClock(I2C_HZ_SEN6X);
  // Datasheet SEN6x: 100 ms od pripojeni napajeni, nez cidlo odpovi na I2C.
  delay(120);

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
        allowLightSleep = false;     // v servisu bezi USB konzole
        runService();
        cfgLoad();
        buildChannels();
        sig = buildSignature();
        rtcSignature = sig;        // zmeny v servisu jsou vedome
        // Kdyz uzivatel v servisu menil ch nebo int a pak nechal dobehnout
        // timeout bez ulozeni, zustane histSlots od docasneho rozlozeni.
        // Bez teto kontroly by nesoulad prezil do dalsiho bootu.
        if (histSlots != histPerCh) histClear();
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
  // Cas do automatickeho cisteni ventilatoru bezi po zakladnim intervalu.
  senCleanMin += cfg.intervalMin;

  Reading r;
  float vb;

  if (wantHotspot) {
    // Pred hotspotem se NEMERI. Se SEN6x by to znamenalo pul minuty tocit
    // ventilatorem, nez by se na displeji vubec objevilo SSID s QR kodem -
    // uzivatel pusti tlacitko a civi na starou obrazovku. Baterii precteme
    // primo, hotspot ji potrebuje do stavove karty.
    vb = readVBat();
    r.vbat = vb;
  } else {
    r = doMeasurement();
    vb = r.vbat;                    // doMeasurement uz baterii precetl
  }

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

  Serial.printf("T=%.2f (%s) RH=%.1f (%s) CO2=%.0f (%s) P=%.1f T2(DS)=%.2f"
                " VBAT=%.2f%s | %s | int=%d\n",
                r.temp, r.srcTemp, r.hum, r.srcHum, r.co2, r.srcCo2,
                r.press, r.temp2, vb,
                (vb < VBAT_LOW ? " NIZKE!" : ""), sensorSummary(), cfg.intervalMin);
  if (det.sen6x)
    // T a RH uz s korekci (toff.sen / hoff.sen), at sedi s grafem i displejem
    Serial.printf("%s%s: PM1=%.1f PM2.5=%.1f PM4=%.1f PM10=%.1f ug/m3"
                  "  T=%.2f RH=%.1f  HCHO=%.1f ppb\n",
                  senKindName(senKind), r.senFresh ? "" : " (preskoceno)",
                  r.pm1, r.pm25, r.pm4, r.pm10,
                  qValue(r, Q_T_SEN), qValue(r, Q_H_SEN), r.hcho);

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
