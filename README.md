# MeteoEink

Offline meteostanice s e-ink displejem a velmi nízkou spotřebou. Běží na baterii, nepotřebuje internet ani žádný server - všechno měření i historie zůstává v zařízení.

Postavená na desce **LaskaKit ESPink-4.26** (ESP32-S3 + 4,26" ePaper 800×480).

Podrobný článek: <https://chiptron.cz/prenosna-offline-meteostanice-s-jednoduchym-nastavenim-a-velmi-nizkou-spotrebou/>

---

## Co to umí

- Měří teplotu, vlhkost, CO₂, prach (PM2.5 / PM10) a tlak - podle toho, jaká čidla jsou připojená.
- **Čidla se detekují automaticky** při startu. Připojení nebo odebrání čidla nevyžaduje překompilování firmwaru.
- Vykresluje aktuální hodnoty a až **4 grafy historie** přímo na e-ink displej.
- **Každé čidlo má vlastní veličinu**, takže jde porovnat třeba teplotu ze SHT40 a ze SEN63C v jednom grafu. Zároveň si volíte, které čidlo dodává hlavní hodnotu na displeji.
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

Paměť je společný pool - čím méně kanálů grafu a delší interval, tím delší historie.

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

Restartujte desku a v servisním režimu (nebo v konfigurátoru) zkontrolujte, že se objevil řádek `SEN6x: SEN63C sn 0x...`. Pokud je na `0x6B` čidlo, které firmware nezná, napíše to.

### 2. Nastavte spotřebu

Mezi měřeními deska čidlo odpojí od napájení (spínač uSup na GPIO47), takže tehdy nebere nic. Každé měření ale znamená roztočit ventilátor a nechat čidlo běžet asi 30 s, než je PM platné - po celou tu dobu do něj jde 90 mA. Při intervalu 5 min a měření prachu při každém probuzení to dělá zhruba **200 mAh/den**.

```
senmult=3     PM a CO2 jen každé 3. probuzení (teplota zůstává po 5 min)
senwarm=30    doba běhu před odečtem; méně než 30 s = podhodnocené PM
lsleep=1      ESP32 spí během zahřívání čidla (~1 mA místo 40 mA)
```

`list` vypíše odhad denní spotřeby podle aktuálního nastavení, stejně tak konfigurátor.

### 3. Zkalibrujte CO₂ - tohle je nutné

Deska čidlo mezi měřeními odpojuje od napájení, což má dva důsledky:

- **Automatická samokalibrace (ASC) nefunguje.** Datasheet: *„for power-cycled single shot operation, ASC is not available."* Nechte ji vypnutou (`asc=0`, od v4.3.0 výchozí).
- **Deklarovaná přesnost platí až po dlouhém souvislém běhu** - 12 h u SEN63C, 2 dny u SEN66. Na baterii toho nedosáhnete.

Zbývá tedy jediné - zkalibrovat čidlo ručně:

```
1. Vezměte desku ven, mimo dech a výfuky (venkovní vzduch je stabilně ~420 ppm).
2. V servisním režimu zadejte  co2ref=420
3. Čidlo poběží 3,5 minuty. Nechte ho v klidu na jednom místě.
4. Vypíše se korekce v ppm. Uložte příkazem  save
```

Korekce se ukládá do NVS **čidla**, takže přežije i odpojení napájení a nové nahrání firmwaru. Opakujte jednou za pár měsíců.

Máte-li barometr (BME/BMP280), tlak se posílá do čidla automaticky jako vstup pro kompenzaci CO₂. Bez barometru nastavte `alt=<metry>`.

### 4. Zkompenzujte teplotu

SEN6x se sám ohřívá a jeho teplota bývá o 1-3 °C nad skutečnou. Porovnejte s referenčním teploměrem a nastavte `toff.sen=<rozdíl>`. Pokud chcete teplotu z jiného čidla, přepněte `tsrc=sht` (nebo `tsrc=ds` pro sondu na kabelu).

### 5. Nechte zapnuté čištění ventilátoru

`senauto=7` (výchozí) pročistí ventilátor jednou týdně. `senclean` spustí čištění hned při dalším měření.

### Co čekat

Trend CO₂ (vyvětráno / dusno) je po kalibraci spolehlivý, absolutní hodnotu berte s rezervou. **První měření po nahrání firmwaru není směrodatné.**

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

Nastavit lze interval měření, kalibrační offsety jednotlivých čidel, které čidlo dodává hlavní teplotu a vlhkost, a které veličiny se kreslí do grafů.

### 3. Tlačítka (držet při restartu)

| Tlačítko | Doba | Co udělá |
|---|---|---|
| PUSH (GPIO40) | 2-5 s | servisní režim přes USB (sériová linka, 115200 Bd) |
| PUSH (GPIO40) | > 5 s | WiFi hotspot s konfigurační stránkou |
| DOWN (GPIO41) | 5 s | smaže celou historii měření |

---

## Pro vývojáře

Celý firmware je jeden soubor: [`MeteoEink426/MeteoEink426.ino`](MeteoEink426/MeteoEink426.ino) (~4300 řádků, Arduino framework). Součástí je i konfigurační stránka hotspotu jako raw string.

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
- **Historie** žije v `RTC_DATA_ATTR int16_t histPool[POOL_SLOTS]` jako společný kruhový pool. Kanál `c` zabírá rozsah `histPool[c*histPerCh ... c*histPerCh + histPerCh-1]`, `histPerCh` se dopočítává z počtu aktivních kanálů. RTC RAM přežije deep sleep; do NVS se zapisuje jen každé `NVS_SAVE_EVERY` měření, aby se šetřilo flash.
- **Konfigurace** je struktura `Config` v NVS (namespace `meteo`, klíč `cfg`), chráněná hodnotou `CFG_MAGIC`. Když strukturu změníte, změňte i magic - stará konfigurace se pak ignoruje místo toho, aby se přečetla špatně. `cfgSanitize()` ošetřuje nesmyslné hodnoty, `cfgSave()` po zápisu ověřuje zpětným čtením.
- **Zdroje veličin.** Teplotu a vlhkost hlásí až pět čidel. Offsety se přičítají na jednom místě v `mergeSources()`, ne v jednotlivých čtecích funkcích. Které čidlo je hlavní, odpovídá výhradně `tsrcPrimary()` / `hsrcPrimary()` - nikdy dostupnost čidel.
- **Ochrana historie**: v NVS je podpis sestavy (čidla + kanály + interval). Při neshodě se historie hned nemaže - odlišná sestava se musí potvrdit dvěma po sobě jdoucími starty (`SIG_CONFIRM`), aby výpadek čidla nesmazal data.
- **Ochrana baterie**: pod `VBAT_LOW` (3,50 V) se na displeji zobrazí varování, pod `VBAT_NO_WRITE` (3,30 V) se přestane zapisovat do flash. Pod `SEN_VBAT_MIN` (3,45 V) se vůbec nespustí SEN6x.
- **Displej** je v portrait orientaci, `W = 480`, `H = 800`. Znaky `°`, `µ` a `³` fonty Adafruit GFX neobsahují, kreslí se z primitiv (`richPrint()` / `richWidth()`).

Nejdůležitější konstanty na jednom místě v sekci `LIMITY A VÝCHOZÍ HODNOTY`:

```c
#define MAX_CHANNELS      4      // max. počet grafů
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

### Servisní režim (USB, 115200 Bd)

```
KOMPENZACE  (každé čidlo má vlastní; bez tečky se nastaví to aktivní)
toff.<čidlo>=<x>  offset teploty [°C]   (-20 až 20), čidlo: sht sen scd bme ds
hoff.<čidlo>=<x>  offset vlhkosti [%RH] (-30 až 30), čidlo: sht sen scd bme
poff=<x>          offset tlaku [hPa]    (-50 až 50)
alt=<x>           nadmořská výška [m]   (0 až 4000)

CO₂
co2ref=<x>  kalibrace na známou hodnotu [ppm] (venku ~420, čidlo běží 3,5 min)
asc=0|1     automatická samokalibrace (nechat na 0, viz výše)

ČIDLO PRACHU SEN6x
senwarm=<s> doba běhu před odečtem (5 až 120 s, výchozí 30)
senmult=<n> násobek intervalu pro PM a CO₂ (1 až 4)
senauto=<d> automatické čištění ventilátoru po d dnech (0 až 90, 0 = vypnuto)
senclean    pročistit ventilátor hned při dalším měření
senstat     stavové bity čidla
lsleep=0|1  uspat ESP32 během zahřívání čidla (šetří ~30 %)

MĚŘENÍ A GRAF
int=<min>     základní interval (1 až 60) - mění délku historie
tsrc=<čidlo>  které čidlo dodává hlavní teplotu (auto = podle priority)
hsrc=<čidlo>  které čidlo dodává hlavní vlhkost
ch=auto       automatický výběr kanálů grafu
ch=a,b,c      ruční výběr až 4 veličin, např. ch=co2,pm25,temp.sht,temp.sen

SPRÁVA
list   nastavení    ver    verze firmwaru    dump   historie jako CSV
save   uložit       clear  smazat historii   exit   uložit a ukončit
help / ?  nápověda
```

Režim se ukončí po 60 s nečinnosti.

---

## Licence

MIT - viz [LICENSE](LICENSE).
