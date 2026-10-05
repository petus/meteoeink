# MeteoEink

Offline meteostanice s e-ink displejem a velmi nízkou spotřebou. Běží na baterii, nepotřebuje internet ani žádný server - všechno měření i historie zůstává v zařízení.

Postavená na desce **LaskaKit ESPink-4.26** (ESP32-S3 + 4,26" ePaper 800×480).

Podrobný článek: <https://chiptron.cz/prenosna-offline-meteostanice-s-jednoduchym-nastavenim-a-velmi-nizkou-spotrebou/>

---

## Co to umí

- Měří teplotu, vlhkost, CO₂, prach (PM2.5 / PM10) a tlak - podle toho, jaká čidla jsou připojená.
- **Čidla se detekují automaticky** při startu. Připojení nebo odebrání čidla nevyžaduje překompilování firmwaru.
- Vykresluje aktuální hodnoty a až **4 grafy historie** přímo na e-ink displej.
- **Každé čidlo má vlastní veličinu**, takže jde porovnat třeba teplotu ze SHT40 a ze SEN63C - buď každou ve svém grafu, nebo obě v jednom. Zároveň si volíte, které čidlo dodává hlavní hodnotu na displeji.
- Historie se ukládá lokálně a **přežije vybití i restart** (RTC RAM + NVS).
- Mezi měřeními je deska v deep sleepu - při rozumném intervalu vydrží baterie týdny.
- Konfigurace přes web (USB nebo WiFi hotspot), export dat do CSV.

### Podporovaná čidla

| Čidlo | Veličina | Připojení |
|---|---|---|
| SHT4x | teplota, vlhkost | I2C 0x44 / 0x45 / 0x46 (základní, je na desce) |
| SEN63C / SEN66 | PM1/2.5/4/10, teplota, vlhkost, CO₂ | I2C 0x6B |
| SEN65 / SEN68 / SEN69C | PM, teplota, vlhkost (+ CO₂ u 69C) | I2C 0x6B, volitelná knihovna |
| SCD41 | CO₂ | I2C 0x62 |
| BME280 | tlak (+ vlhkost záložně) | I2C 0x76 / 0x77 |
| BMP280 | tlak | I2C 0x76 / 0x77 |
| DS18B20 | druhá teplota | 1-Wire, GPIO4 + pull-up 4,7 kΩ |

I2C čidla se připojují konektorem **uSup** (kompatibilní se SparkFun Qwiic / Adafruit STEMMA QT), bez pájení.

**VOC, NOx a HCHO se u SEN6x záměrně nečtou.** Jejich index počítá adaptivní algoritmus, který potřebuje běžet nepřetržitě hodiny až dny. Deska čidlo mezi měřeními odpojuje, takže by algoritmus vždy startoval od nuly.

### Délka historie

Paměť je společný pool - čím méně kanálů grafu a delší interval, tím delší historie. Rozhoduje počet **kanálů**, ne počet grafů: sloučení dvou veličin do jednoho grafu historii neprodlouží.

| Interval | 1 kanál | 2 kanály | 3 kanály | 4 kanály |
|---|---|---|---|---|
| 5 min | 7 dní | 4,5 dne | 3 dny | 2,2 dne |
| 30 min | 42 dní | 27 dní | 18 dní | 13,5 dne |
| 60 min | 84 dní | 54 dní | 36 dní | 27 dní |

> **Pozor:** zařízení není určené do venkovního prostředí. Elektronika ani baterie nejsou chráněné proti vlhkosti a mrazu. Venkovní teplotu měřte čidlem DS18B20 v radiačním štítu, jednotka zůstane uvnitř.

---

## Po připojení SEN63C nebo SEN66

Čidlo se detekuje samo, ale **bez nastavení nebude měřit správně.** Bez těchto kroků dostanete nesmyslné CO₂ a podhodnocený prach.

### 1. Ověřte detekci

Restartujte desku a v konfigurátoru (USB nebo WiFi) zkontrolujte, že se mezi čidly objevil SEN63C nebo SEN66.

### 2. Nastavte spotřebu

Mezi měřeními deska čidlo odpojí od napájení (spínač uSup na GPIO47), takže tehdy nebere nic. Každé měření ale znamená roztočit ventilátor a nechat čidlo běžet asi 30 s, než je PM platné - po celou tu dobu do něj jde 90 mA. Při intervalu 5 min a měření prachu při každém probuzení to dělá zhruba **200 mAh/den**.

V konfigurátoru v sekci **Čidlo prachu SEN6x** nastavte:

- **Interval pro prach a CO₂**, např. každé 3. měření. Teplota a vlhkost se měří dál podle základního intervalu.
- **Doba běhu před odečtem** 30 s. Kratší doba dává podhodnocené PM.
- **Uspat desku během zahřívání** zapnuto. ESP32 pak během zahřívání čidla bere asi 1 mA místo 40 mA.

Konfigurátor ukáže odhad denní spotřeby podle aktuálního nastavení.

### 3. Zkalibrujte CO₂ - tohle je nutné

Deska čidlo mezi měřeními odpojuje od napájení, což má dva důsledky:

- **Automatická samokalibrace (ASC) nefunguje.** Datasheet: *„for power-cycled single shot operation, ASC is not available."* Nechte ji vypnutou (od v4.3.0 výchozí).
- **Deklarovaná přesnost platí až po dlouhém souvislém běhu**, 12 h u SEN63C, 2 dny u SEN66. Na baterii toho nedosáhnete.

Čidlo proto zkalibrujte ručně, postup je níže v kapitole **Kalibrace CO₂**.

### 4. Zkompenzujte teplotu

SEN6x se sám ohřívá a jeho teplota bývá o 1-3 °C nad skutečnou. Porovnejte s referenčním teploměrem a rozdíl nastavte v konfigurátoru v sekci **Kompenzace měření** jako offset teploty SEN6x. Pokud chcete hlavní teplotu z jiného čidla (SHT40 nebo sondy DS18B20 na kabelu), zvolte ho v sekci **Hlavní teplota a vlhkost**.

### 5. Nechte zapnuté čištění ventilátoru

**Automatické čištění ventilátoru** po 7 dnech (výchozí) pročistí ventilátor jednou týdně. Tlačítko **Naplánovat pročištění** ho spustí hned při dalším měření.

### Co čekat

Trend CO₂ (vyvětráno / dusno) je po kalibraci spolehlivý, absolutní hodnotu berte s rezervou. **První měření po nahrání firmwaru není směrodatné.**

---

## Kalibrace CO₂

Platí pro SCD41 i SEN6x. Kalibruje se čidlo, které dodává CO₂ (je-li v sestavě SCD41, je to on). Kalibrace je FRC (forced recalibration): čidlo dostane referenční koncentraci a rozdíl proti svému údaji si uloží jako korekci. Venkovní vzduch má přibližně 420 ppm.

### Před kalibrací

V konfigurátoru nastavte **nadmořskou výšku**. Bez ní čidlo počítá s tlakem u hladiny moře a ve 250 m n. m. měří asi o 3 % méně. Máte-li barometr (BME/BMP280), posílá se do čidla místo výšky naměřený tlak, automaticky.

### Postup

1. Naplánujte kalibraci:
   - **USB:** v konfigurátoru v řádku **Kalibrace CO₂** nechte 420 ppm, klikněte na **Použít** a pak dole na **Uložit a ukončit servis**.
   - **WiFi:** na stránce hotspotu v panelu CO₂ nechte 420 a klikněte na **Kalibrovat**, pak dole na **Uložit a vypnout hotspot**. Tlačítko **Použít** v tom panelu patří k samokalibraci, pro kalibraci ho nepotřebujete.
2. Začne **odklad**, výchozí 90 s, na displeji *Odneste desku* s odpočtem. Desku odneste ven do stínu, mimo výfuky a dál od obličeje (vydechovaný vzduch má desítky tisíc ppm).
3. Čidlo měří každých 5 s a displej ukazuje *Ustaluje se*, CO₂, teplotu a sklon. Nechte desku v klidu.
4. Výsledek se ukáže dole na displeji místo nápovědy, např. *Kalibrace CO2 (SCD41) OK, korekce -65 ppm*. Znovu ho uvidíte v konfigurátoru i v hotspotu.

Odklad se nastavuje vedle hodnoty reference, 0 až 600 s. Když už je deska na místě, nastavte 0.

### Kdy se kalibruje

FRC zapíše rozdíl mezi tím, co čidlo ukazuje *právě teď*, a referencí. Odezva SCD41 je podle datasheetu τ63 = 60 s, v krabičce několikanásobně delší. Kalibrace po pevné době by zapsala korekci, dokud čidlo po přenesení z místnosti ještě klesá. Firmware proto počítá lineární regresi přes posledních 3 minuty a FRC pošle, až současně platí:

| Podmínka | Mez |
|---|---|
| sklon CO₂ | < 5 ppm/min |
| sklon teploty čidla | < 0,2 °C/min |
| rozptyl CO₂ kolem regresní přímky | < 20 ppm RMS |
| podmínky platí nepřetržitě | ≥ 60 s |
| doba měření | 3 až 20 min |

Když se hodnoty do 20 minut neustálí (kolem desky chodí lidé, ohřívá ji slunce), kalibrace se neprovede a displej ukáže *neustalilo se*.

Obvyklá doba je 5 až 15 minut. SCD41 se při měření každých 5 s sám ohřívá, takže i když je deska venku předem, čeká se na ustálení teploty čidla, typicky 6 minut.

### Výsledek

Korekci si ukládá **čidlo** (SCD41 do EEPROM, SEN6x do NVS). Přežije odpojení napájení i nahrání firmwaru.

Korekce nad ±150 ppm se provede, ale výsledek se označí `(!)` a konfigurátor i hotspot připíšou vysvětlení. Tolerance SCD41 je ±(40 ppm + 5 %), takže tak velká korekce znamená jedno z dvou:

- čidlo bylo dřív špatně zkalibrované a teď se to opravuje,
- deska nebyla na čerstvém vzduchu. Pak kalibraci zopakujte, nová ji přepíše.

Venkovní CO₂ kolísá, u domu nebo ráno při inverzi bývá 450 až 500 ppm. Rozdíl několika desítek ppm po kalibraci není důvod ji opakovat. Kalibrujte na volném prostranství a opakujte jednou za několik měsíců.

Pod 3,45 V se SEN6x nespouští, kalibrace pak skončí *neprobehla - slaba baterie*.

### Obnovení tovární kalibrace (jen SCD41)

Tlačítko **Obnovit tovární kalibraci** v konfigurátoru i v hotspotu. Smaže všechny dosavadní korekce FRC i historii samokalibrace. Hodí se, když nevíte, co se s čidlem dělo: hned je vidět, co měří s výrobní kalibrací. Potom ho zkalibrujte venku.

SEN6x tovární reset nemá, rozladěný SEN6x opravíte novou kalibrací.

---

## Pro začátečníka

Nepotřebujete žádné vývojové prostředí ani kompilátor. Stačí nahrát hotový BIN soubor.

### 1. Nahrání firmwaru

1. Stáhněte z tohoto repozitáře soubor `MeteoEink426.ino.merged.bin`.
2. Připojte ESPink-4.26 k počítači kabelem USB-C.
3. Otevřete <http://esp32flasher.chiptron.cz/> (v prohlížeči Chrome nebo Edge).
4. Zvolte čip **ESP32-S3**, nahrajte stažený BIN a spusťte flashování.
5. Po dokončení se deska sama restartuje.

Celé to trvá asi dvě minuty.

> Nahrání nové verze **smaže historii měření** a u větších verzí i konfiguraci - viz [CHANGELOG](CHANGELOG.md). Data si předtím vyexportujte do CSV.

### 2. Nastavení

**Přes USB (počítač):** otevřete <http://meteoeink.chiptron.cz/>, připojte desku kabelem a nastavte, co potřebujete. Stránka umí i export grafů a dat do CSV a obrázku.

**Přes WiFi (telefon):** při restartu podržte tlačítko **PUSH** déle než 5 s. Deska vytvoří zabezpečený hotspot `MeteoEink-XXXX` a na displeji ukáže SSID, heslo a QR kód pro připojení. Hotspot se po 5 minutách nečinnosti sám vypne, aby nevybíjel baterii.

Nastavit lze interval měření, kalibrační offsety jednotlivých čidel, které čidlo dodává hlavní teplotu a vlhkost, a které veličiny se kreslí do grafů. Každá vybraná veličina má volbu *Graf 1* až *Graf 4*: veličiny se stejným číslem se nakreslí do jednoho grafu (stejné jednotky až tři křivky, různé jednotky dvě a druhá osa Y). Přeskládání grafů historii nesmaže, přidání nebo odebrání veličiny ano.

### 3. Tlačítka (držet při restartu)

| Tlačítko | Doba | Co udělá |
|---|---|---|
| PUSH (GPIO40) | 2-5 s | nastavení přes USB (konfigurátor na webu) |
| PUSH (GPIO40) | > 5 s | WiFi hotspot s konfigurační stránkou |
| DOWN (GPIO41) | 5 s | smaže celou historii měření |

První dva řádky připomíná i nápověda dole na displeji.

---

## Pro vývojáře

Celý firmware je jeden soubor: [`MeteoEink426/MeteoEink426.ino`](MeteoEink426/MeteoEink426.ino) (~5700 řádků, Arduino framework). Součástí je i konfigurační stránka hotspotu jako raw string.

### Build

Arduino IDE s podporou ESP32 (arduino-esp32).

- Board: **ESP32S3 Dev Module**
- Flash Size: **16 MB**
- PSRAM: **Disabled** (není potřeba)

Knihovny:

- GxEPD2, Adafruit GFX
- Adafruit BME280, Adafruit BMP280
- SparkFun SCD4x Arduino Library
- OneWire, DallasTemperature
- QRCode (Richard Moore)
- **Sensirion I2C SEN66** a **Sensirion I2C SEN63C** - povinné
- Sensirion I2C SEN65 / SEN68 / SEN69C - volitelné, stačí nainstalovat a přeložit znovu
- Sensirion Core - nainstaluje se jako závislost

Ovladač SHT4x je napsaný přímo v souboru - Adafruit knihovna umí jen adresu 0x44, tady jsou potřeba i 0x45 a 0x46. SEN6x naopak používá **oficiální knihovny Sensirionu**; ve skeči je jen tenká rozbočovací vrstva, která podle *Get Product Name* (`0xD014`) vybere správnou třídu.

### Pinout (ESPink-4.26)

```
POWER    47      EPD_CS   10     ONEWIRE  4
I2C_SDA  42      EPD_DC   48     BTN_PUSH 40
I2C_SCL   2      EPD_RST  45     BTN_DOWN 41
                 EPD_BUSY 38     VBAT      9  (dělič 1.769388)
                 EPD_MOSI 11
                 EPD_CLK  12
                 EPD_MISO 21
```

I²C je natvrdo na 100 kHz - limit SEN6x.

### Struktura kódu

Soubor je rozdělený komentářovými hlavičkami na sekce, v tomto pořadí:

`PINY` → `LIMITY A VÝCHOZÍ HODNOTY` → `DISPLEJ` → `VELIČINY A ČIDLA` → `KONFIGURACE (NVS)` → `HISTORIE` → `NVS: historie` → `NAPÁJENÍ` → `SENSIRION SEN6x` → `DETEKCE ČIDEL` → `MĚŘENÍ` → `KRESLENÍ` → `SERVISNÍ REŽIM` → `OCHRANA HISTORIE` → `WIFI HOTSPOT`

Klíčové věci, které je dobré znát před úpravami:

- **Arduino IDE vkládá vygenerované prototypy těsně před první definici funkce v souboru.** Když se typ použitý v hlavičce funkce (`Quantity`, `Reading`, `Detected`, `SenKind`, ...) definuje až za tou první funkcí, překlad spadne na desítkách hlášek `'Quantity' was not declared in this scope`. **V souboru nesmí být žádná definice funkce dřív než blok typů v sekci VELIČINY A ČIDLA.** Čistý `g++` tuhle chybu neodhalí.
- **Kanál vs. panel.** Kanál je jedna veličina v historii, panel je jeden rám grafu. Do v4.3 to bylo 1:1, od v4.4 může panel nést až tři kanály. Rozdělení dělá `buildPlots()`, sloučit lze jen sousední kanály, takže panel je prostě rozsah v `channels[]`. Seskupení se veze v horním bitu `cfg.chSel[]` a **nevstupuje do `buildSignature()`**. Pořadí kanálů v podpisu je a sloučení ho často mění. Když jde jen o jiné pořadí stejných veličin, `histPermute()` přehodí bloky v poolu a historie se hned uloží do NVS pod novým podpisem. Výběr kanálů rozebírá pro servis i hotspot jedna funkce, `chParse()`.
- **Historie** žije v `RTC_DATA_ATTR int16_t histPool[POOL_SLOTS]` jako společný kruhový pool. Kanál `c` zabírá rozsah `histPool[c*histPerCh ... c*histPerCh + histPerCh-1]`, `histPerCh` se dopočítává z počtu aktivních kanálů. RTC RAM přežije deep sleep; do NVS se zapisuje jen každé `NVS_SAVE_EVERY` měření, aby se šetřilo flash.
- **Konfigurace** je struktura `Config` v NVS (namespace `meteo`, klíč `cfg`), chráněná hodnotou `CFG_MAGIC`. Když strukturu změníte, změňte i magic - stará konfigurace se pak ignoruje místo toho, aby se přečetla špatně. `cfgSanitize()` ošetřuje nesmyslné hodnoty, `cfgSave()` po zápisu ověřuje zpětným čtením.
- **Zdroje veličin.** Teplotu a vlhkost hlásí až pět čidel. Offsety se přičítají na jednom místě v `mergeSources()`, ne v jednotlivých čtecích funkcích. Které čidlo je hlavní, odpovídá výhradně `tsrcPrimary()` / `hsrcPrimary()` - nikdy dostupnost čidel.
- **Ochrana historie**: v NVS je podpis sestavy (čidla + kanály + interval). Při neshodě se historie hned nemaže - odlišná sestava se musí potvrdit dvěma po sobě jdoucími starty (`SIG_CONFIRM`), aby výpadek čidla nesmazal data.
- **Ochrana baterie**: pod `VBAT_LOW` (3,50 V) se na displeji zobrazí varování, pod `VBAT_NO_WRITE` (3,30 V) se přestane zapisovat do flash. Pod `SEN_VBAT_MIN` (3,45 V) se vůbec nespustí SEN6x.
- **Displej** je v portrait orientaci, `W = 480`, `H = 800`. Znaky `°`, `µ` a `³` fonty Adafruit GFX neobsahují, kreslí se z primitiv (`richPrint()` / `richWidth()`).

Nejdůležitější konstanty na jednom místě v sekci `LIMITY A VÝCHOZÍ HODNOTY`:

```c
#define MAX_CHANNELS      4      // max. počet veličin v grafech
#define MAX_PER_PLOT      3      // max. křivek v jednom grafu
#define MAX_PER_PLOT_MIX  2      // totéž při různých jednotkách (dvě osy Y)
#define POOL_SLOTS        2592   // 5184 B v RTC RAM
#define HISTORY_CAP       2016   // strop vzorků na kanál
#define DEF_INTERVAL      5      // výchozí interval [min]
#define INTERVAL_MIN_HI   60
#define SEN_WARM_HI       120    // max. doba běhu SEN6x před odečtem [s]
#define SEN_MULT_HI       4      // max. násobek intervalu pro PM a CO₂
#define SEN_VBAT_MIN      3.45f  // pod tím SEN6x nespouštět
#define AP_TIMEOUT_MS     300000UL
```

### Přidání nového čidla

1. Přidejte položku do `enum Quantity` a do `Detected`.
2. Doplňte detekci v sekci `DETEKCE ČIDEL` (`i2cPresent(addr)` pro I2C).
3. Doplňte čtení v sekci `MĚŘENÍ` a název veličiny do `qKey()` / `qLabel()`.
4. Zohledněte veličinu v `qAvailable()` a v automatickém výběru kanálů.
5. Dodává-li teplotu nebo vlhkost, přidejte ji do `TempSrc` / `HumSrc` - tím dostane vlastní offset i vlastní veličinu do grafu.

### Komunikace s konfigurátorem

Konfigurátor přes USB posílá desce textové příkazy po sériové lince (115200 Bd), stránka hotspotu volá `/api/set`. Obě cesty zpracovávají stejné funkce v sekcích `SERVISNÍ REŽIM` a `WIFI HOTSPOT`.

---

## Licence

MIT - viz [LICENSE](LICENSE).
