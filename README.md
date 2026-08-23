# MeteoEink

Offline meteostanice s e-ink displejem a velmi nízkou spotřebou. Běží na baterii, nepotřebuje internet ani žádný server — všechno měření i historie zůstává v zařízení.

Postavená na desce **LaskaKit ESPink-4.26** (ESP32-S3 + 4,26" ePaper 800×480).

Podrobný článek: <https://chiptron.cz/prenosna-offline-meteostanice-s-jednoduchym-nastavenim-a-velmi-nizkou-spotrebou/>

---

## Co to umí

- Měří teplotu, vlhkost, CO₂ a tlak — podle toho, jaká čidla jsou připojená.
- **Čidla se detekují automaticky** při startu. Připojení nebo odebrání čidla nevyžaduje překompilování firmwaru.
- Vykresluje aktuální hodnoty a až **3 grafy historie** přímo na e-ink displej.
- Historie se ukládá lokálně a **přežije vybití i restart** (RTC RAM + NVS).
- Mezi měřeními je deska v deep sleepu — při rozumném intervalu vydrží baterie týdny.
- Konfigurace přes web (USB nebo WiFi hotspot), export dat do CSV.

### Podporovaná čidla

| Čidlo | Veličina | Připojení |
|---|---|---|
| SHT4x | teplota, vlhkost | I2C 0x44 / 0x45 / 0x46 (základní, je na desce) |
| SCD41 | CO₂ | I2C 0x62 |
| BME280 | tlak (+ vlhkost záložně) | I2C 0x76 / 0x77 |
| BMP280 | tlak | I2C 0x76 / 0x77 |
| DS18B20 | druhá teplota | 1-Wire, GPIO4 + pull-up 4,7 kΩ |

I2C čidla se připojují konektorem **uSup** (kompatibilní se SparkFun Qwiic / Adafruit STEMMA QT), bez pájení.

### Délka historie

Paměť je společný pool — čím méně kanálů grafu a delší interval, tím delší historie.

| Interval | 1 kanál | 2 kanály | 3 kanály |
|---|---|---|---|
| 5 min | 7 dní | 4,5 dne | 3 dny |
| 30 min | 42 dní | 27 dní | 18 dní |
| 60 min | 84 dní | 54 dní | 36 dní |

> **Pozor:** zařízení není určené do venkovního prostředí. Elektronika ani baterie nejsou chráněné proti vlhkosti a mrazu. Venkovní teplotu měřte čidlem DS18B20 v radiačním štítu, jednotka zůstane uvnitř.

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

### 2. Nastavení (pokud vám nevyhovuje to výchozí)

**Přes USB (počítač):** otevřete <http://meteoeink.chiptron.cz/>, připojte desku kabelem a nastavte, co potřebujete. Stránka umí i export grafů a dat do CSV a obrázku.

**Přes WiFi (telefon):** při restartu podržte tlačítko **PUSH** déle než 5 s. Deska vytvoří zabezpečený hotspot `MeteoEink-XXXX` a na displeji ukáže SSID, heslo a QR kód pro připojení. Hotspot se po 5 minutách nečinnosti sám vypne, aby nevybíjel baterii.

Nastavit lze zejména interval měření, kalibrační offsety čidel a to, které veličiny se mají kreslit do grafů.

### 3. Tlačítka (držet při restartu)

| Tlačítko | Doba | Co udělá |
|---|---|---|
| PUSH (GPIO40) | 2–5 s | servisní režim přes USB (sériová linka, 115200 Bd) |
| PUSH (GPIO40) | > 5 s | WiFi hotspot s konfigurační stránkou |
| DOWN (GPIO41) | 5 s | smaže celou historii měření |

---

## Pro vývojáře

Celý firmware je jeden soubor: [`MeteoEink426/MeteoEink426.ino`](MeteoEink426/MeteoEink426.ino) (~2100 řádků, Arduino framework).

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

Ovladač SHT4x je napsaný přímo v souboru — Adafruit knihovna umí jen adresu 0x44, tady jsou potřeba i 0x45 a 0x46.

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

### Struktura kódu

Soubor je rozdělený komentářovými hlavičkami na sekce, v tomto pořadí:

`PINY` → `LIMITY A VÝCHOZÍ HODNOTY` → `DISPLEJ` → `VELIČINY A ČIDLA` → `KONFIGURACE (NVS)` → `HISTORIE` → `NVS: historie` → `NAPÁJENÍ` → `DETEKCE ČIDEL` → `MĚŘENÍ` → `KRESLENÍ` → `SERVISNÍ REŽIM` → `OCHRANA HISTORIE` → `WIFI HOTSPOT`

Klíčové věci, které je dobré znát před úpravami:

- **Historie** žije v `RTC_DATA_ATTR int16_t histPool[POOL_SLOTS]` jako společný kruhový pool. Kanál `c` zabírá rozsah `histPool[c*histPerCh … c*histPerCh + histPerCh-1]`, `histPerCh` se dopočítává z počtu aktivních kanálů. RTC RAM přežije deep sleep; do NVS se zapisuje jen každé `NVS_SAVE_EVERY` měření, aby se šetřilo flash.
- **Konfigurace** je struktura `Config` v NVS (namespace `meteo`, klíč `cfg`), chráněná hodnotou `CFG_MAGIC`. Když strukturu změníte, změňte i magic — stará konfigurace se pak ignoruje místo toho, aby se přečetla špatně. `cfgSanitize()` ošetřuje nesmyslné hodnoty, `cfgSave()` po zápisu ověřuje zpětným čtením.
- **Ochrana historie**: v NVS je podpis sestavy (čidla + kanály + interval). Při neshodě se historie hned nemaže — odlišná sestava se musí potvrdit dvěma po sobě jdoucími starty (`SIG_CONFIRM`), aby výpadek čidla nesmazal data.
- **Ochrana baterie**: pod `VBAT_LOW` (3,50 V) se na displeji zobrazí varování, pod `VBAT_NO_WRITE` (3,30 V) se přestane zapisovat do flash.
- **Displej** je v portrait orientaci, `W = 480`, `H = 800`.

Nejdůležitější konstanty na jednom místě v sekci `LIMITY A VÝCHOZÍ HODNOTY`:

```c
#define MAX_CHANNELS      3      // max. počet grafů
#define POOL_SLOTS        2592   // 5184 B v RTC RAM
#define HISTORY_CAP       2016   // strop vzorků na kanál
#define DEF_INTERVAL      5      // výchozí interval [min]
#define INTERVAL_MIN_LO   1
#define INTERVAL_MIN_HI   60
#define AP_TIMEOUT_MS     300000UL
```

### Přidání nového čidla

1. Přidejte položku do `enum Quantity` a do `Detected`.
2. Doplňte detekci v sekci `DETEKCE ČIDEL` (`i2cPresent(addr)` pro I2C).
3. Doplňte čtení v sekci `MĚŘENÍ` a název veličiny do `qKey()` / `qLabel()`.
4. Zohledněte veličinu v `qAvailable()` a v automatickém výběru kanálů.

### Servisní režim (USB, 115200 Bd)

Rychlá cesta, jak si při vývoji sáhnout na nastavení a data bez webu:

```
toff=<x>    offset teploty [°C]      (-20 … 20)
hoff=<x>    offset vlhkosti [%RH]    (-30 … 30)
poff=<x>    offset tlaku [hPa]       (-50 … 50)
alt=<x>     nadmořská výška [m]      (0 … 4000)
co2ref=<x>  kalibrace SCD41 na hodnotu [ppm]
asc=0|1     automatická samokalibrace SCD41
int=<min>   interval měření (1 … 60) — mění délku historie
ch=auto     automatický výběr kanálů grafu
ch=a,b,c    ruční výběr, např. ch=co2,temp,press
list        vypíše aktuální nastavení
dump        vypíše celou historii jako CSV
save        uloží nastavení do NVS
clear       smaže historii
exit        uloží a ukončí servis
help / ?    nápověda
```

Režim se ukončí po 60 s nečinnosti.

---

## Licence

MIT — viz [LICENSE](LICENSE).
