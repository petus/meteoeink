# MeteoEink426 - changelog

## v4.4.0

Konfigurace i historie zůstávají (`CFG_MAGIC` je pořád `MEK7`).

### Nové

- **Nápověda k nastavení na displeji.** Nad zápatím je řádek
  `Pri restartu drz PUSH: 2 s = servis (USB), 5 s = WiFi`. Zabírá 16 px,
  při čtyřech grafech tedy 4 px z každého. Mazání historie tlačítkem DOWN se
  záměrně neuvádí, protože je nevratné.
- **Více veličin v jednom grafu.** V `ch=` slučuje veličiny znak `+`:

  ```
  ch=temp.sht+temp.sen,co2      dva grafy, v prvním dvě křivky
  ch=co2+temp,pm25              první graf má dvě osy Y
  ```

  Stejné jednotky sdílejí jednu osu (nejvýš tři křivky), různé jednotky
  dostanou druhou osu Y vpravo (nejvýš dvě křivky). Křivky se rozlišují stylem
  čáry (plná, čárkovaná, tečkovaná) a popiskem na konci. Graf má víc místa:
  čtyři veličiny ve dvou grafech mají 231 px na výšku místo 94 px.

  Slučuje se jen na výslovné přání, automatický výběr kanálů to nedělá. Když
  je společný rozsah osy víc než trojnásobkem rozsahu nejširší z křivek
  (třeba pokojová a venkovní teplota v zimě), napíše se to do záhlaví grafu.

  V konfigurátoru i na stránce hotspotu má každá vybraná veličina volbu
  *Graf 1* až *Graf 4*, veličiny se stejným číslem jdou do jednoho grafu.
  Stránka ukáže výsledné rozložení, nepustí víc křivek, než graf unese,
  a upozorní na DS18B20 ve společné ose s pokojovou teplotou. Hotspot
  rozebírá výběr stejnou funkcí `chParse()` jako servis a odmítnutý výběr
  vrátí stránce s důvodem. JSON stavu má nové pole `plots` (počet křivek
  v jednotlivých grafech).

  Délka historie se nemění, řídí se počtem kanálů, ne grafů. Seskupení se
  ukládá do horního bitu `cfg.chSel[]` a nevstupuje do podpisu sestavy.
  Sloučit jde jen sousední kanály, takže sloučení často mění pořadí kanálů.
  Když zůstanou stejné veličiny, jen v jiném pořadí, `histPermute()` přehodí
  bloky v poolu a historie zůstane (dřív se smazala). Platí to pro servis
  i hotspot, i když servis skončí timeoutem bez uložení. `list` vypisuje
  `ch = ...` beze změny (čte ho konfigurátor) a sloučení na zvláštním řádku
  `grafy = ...`.
- **Obnovení tovární kalibrace SCD41.** Příkaz `co2reset`, tlačítko
  v hotspotu a v konfigurátoru. Volá `perform_factory_reset`, který smaže
  všechny korekce FRC i historii ASC. Pak se znovu nastaví `asc` podle
  konfigurace. SEN6x tento příkaz nemá.

### Kalibrace CO₂

Kalibrace (FRC) dřív u SCD41 nefungovala vůbec a u obou čidel zapisovala
špatnou korekci. Postup je přepracovaný a platí pro SCD41 i SEN6x.

- **Opraveno: SCD41 kalibraci odmítal.** FRC se posílalo asi sekundu po
  zapnutí čidla. Datasheet požaduje nejméně 3 minuty měření v ustáleném
  prostředí. Čidlo vrátilo 0xFFFF a deska přešla rovnou do měření.
- **FRC se pošle až po ustálení hodnot.** FRC zapíše jako korekci rozdíl mezi
  aktuálním údajem čidla a referencí. Odezva SCD41 je τ63 = 60 s,
  v krabičce několikanásobně delší. Kalibrace po pevné době proto zapsala
  korekci, dokud čidlo ještě klesalo (v testu −90 ppm, venku pak 330 ppm).
  Nový postup:
  1. odklad startu, výchozí 90 s, nastavitelný 0 až 600 s
     (`co2ref=420,180`), na displeji odpočet,
  2. měření každých 5 s,
  3. FRC se pošle, když v okně posledních 3 minut platí současně: sklon
     CO₂ (lineární regrese) pod 5 ppm/min, sklon teploty pod 0,2 °C/min,
     rozptyl CO₂ kolem regresní přímky pod 20 ppm RMS, a to nepřetržitě
     aspoň 60 s,
  4. nejdřív po 3 minutách měření, nejpozději po 20 minutách. Pak se
     kalibrace neprovede a výsledek je *neustalilo se*.

  Hlídání teploty je nutné kvůli SCD41: při měření každých 5 s se čidlo
  samo ohřívá, v testu o 1,8 °C za 5 minut, a kompenzace CO₂ na teplotě
  závisí. Kalibrace u SCD41 proto trvá i venku asi 6 minut.
  V simulaci s modelem čidla (exponenciální odezva, šum, dech při nesení)
  klesla chyba kalibrace ze 140 ppm na 4 ppm. Ověřeno na hardwaru se SEN63C
  i SCD41; SCD41 po kalibraci na 420 ppm ukazoval venku 405 až 437 ppm.
- **Velká korekce jen varuje.** Korekce nad ±150 ppm se provede, výsledek se
  označí `(!)` a hotspot i `list` vypíšou vysvětlení. Blokování by znemožnilo
  opravit předchozí špatnou kalibraci, kdy je velká korekce správně.
- **Opraveno: požadavek na kalibraci se mohl ztratit.** Když SCD41
  neodpověděl na `begin()`, SEN6x se nespustil, byla slabá baterie nebo SEN6x
  při `senmult > 1` přeskakoval cyklus, funkce skončila před kalibrací
  a deep sleep požadavek zahodil. Teď každá cesta zapíše výsledek
  a kalibrace má přednost před `senmult`. `begin()` u SCD41 se při selhání
  jednou zopakuje.
- **Průběh a výsledek na displeji.** Po ukončení servisu nebo hotspotu se
  ukáže obrazovka *Prave probiha kalibrace CO2* s typem čidla a referencí.
  Stav, CO₂, teplota a sklon se obnovují částečným překreslením každých 30 s.
  Výsledek se pak 12 překreslení ukazuje místo nápovědy. Ukládá se do NVS,
  ukazuje ho hotspot (panel CO₂) a `list` (řádek `posledni:`).
- **Opraveno: SCD41 nedostával tlak z barometru.** Barometr se četl až po
  SCD41. Teď se čte před ním a SCD41 dostane `set_ambient_pressure` při
  každém zapnutí (nastavení je volatilní).
- **Opraveno: nadmořská výška nešla nastavit bez barometru.** Konfigurátor
  i hotspot pole ukazovaly jen s BME/BMP280, přitom bez barometru je výška
  jediný zdroj kompenzace tlaku. Sestavy jen se SCD41 měly vždy `alt = 0`,
  ve 250 m n. m. to dělá chybu asi 3 %.

### Další opravy

- **První vzorek po servisu nebo hotspotu neukládá teplotu a vlhkost
  z čidel na desce.** Deska byla minuty vzhůru, často na nabíjejícím USB,
  a SHT40 bylo ohřáté o ~4 °C. V grafu je místo špičky mezera. Čidla na
  kabelu se ukládají normálně.
- **Osa vlhkosti jednotlivých čidel.** `hum.sht` až `hum.bme` dostávaly
  rozsah osy jako teplota (minimální rozpětí 2 místo 5 %RH), takže šum
  vypadal jako velký výkyv. Třídu osy teď určuje jedno místo, `qAxisClass()`.
- **Varování o slabé baterii.** Mazací obdélník byl o 4 px nižší a nad
  varováním zůstával okraj předchozího řádku.

---

## v4.3.2

Konfigurace i historie zůstávají.

- **Opraveno: servisní režim se často nespustil napoprvé.** Stav tlačítka se
  četl až za `detectSensors()` a `loadHistory()`, tedy stovky milisekund až
  přes sekundu po restartu - a doba stisku se počítala teprve od té chvíle.
  Kdo držel PUSH od resetu a pustil ho po dvou a půl sekundách, měl naměřeno
  jen něco přes sekundu. Délka detekce navíc kolísá podle připojených čidel,
  takže to jednou vyšlo a podruhé ne. Tlačítka se teď vyhodnocují hned na
  začátku `setup()`, ještě před detekcí; vlastní akce se provede až potom,
  kdy jsou čidla i historie načtená. Přibylo krátké okno 600 ms, aby se
  trefil i stisk těsně po resetu. Výpis nově uvádí naměřenou dobu v ms.
- **Displej už neukazuje `--` v přeskočených cyklech.** Při `senmult > 1`
  běží SEN6x jen každé N-té probuzení, ale displej se překresluje při každém,
  takže u CO₂, prachu a při `tsrc=sen` i u hlavní teploty svítily pomlčky.
  Nově se použije poslední známá hodnota. **Do historie se nepřenáší** -
  tam mezera zůstat musí, jinak by graf dostal falešné zdvojené vzorky.
  Drží se jen po dobu očekávaného přeskočení; když čidlo vypadne nadobro,
  pomlčky se objeví.
- **Samokalibrace CO₂ se u SEN6x na webu nenabízí.** Při odpojovaném napájení
  nemá jak fungovat. Volba zůstává, když CO₂ dodává SCD41, a taky když ji
  někdo má zapnutou ze starší konfigurace - jinak by ji nešlo z webu vypnout.
  Platí pro konfigurátor i hotspot.

---

## v4.3.1

Konfigurace i historie zůstávají.

- **Opraveno: graf veličiny ze SEN6x se kreslil jako řada teček.** Týkalo se
  všeho, co dodává SEN6x - `temp.sen`, `hum.sen`, `pm25`, `pm10`, `co2` bez
  SCD41 i hlavní `temp`/`hum` při `tsrc=sen`/`hsrc=sen`. Při `senmult > 1`
  běží čidlo jen každé N-té probuzení, takže má v historii pravidelné mezery,
  a vykreslování spojovalo jen sousední vzorky. Nově se zjistí typický
  rozestup vzorků kanálu a mezery do této délky se přemostí čarou. Tečka
  zbývá jen na vzorek, který opravdu nemá souseda, tedy jedno měření po
  delším výpadku čidla.
- Rozestup se bere jako **nejčastější** mezera, ne nejmenší. Po výpadku
  napájení se `senTick` vynuluje a fáze se posune, takže v historii vznikne
  jedna kratší mezera - jako minimum by rozhodla za celou historii.
- Mezery delší než 32 vzorků se nepřemosťují. To už není rozestup měření,
  ale výpadek čidla, a spojit ho čarou by lhalo.
- Opraveno shodně ve firmwaru, ve webovém konfigurátoru i v hotspotu.

---

## v4.3.0

Konfigurace zůstává (`CFG_MAGIC` = `MEK7`). Kdo už má uloženou konfiguraci,
ať si ručně nastaví `asc=0`.

- **Opraveno CO₂ zaseknuté na jedné hodnotě.** Deska čidlo mezi měřeními
  odpojuje, takže každý odečet byl „první po zapnutí“ - ten datasheet říká
  zahodit. Nově se první odečet zahazuje: SCD41 udělá dva single shoty
  a použije druhý, SEN6x počká na další cyklus měřicího článku (~5 s navíc).
- **Samokalibrace CO₂ je nově vypnutá** (`asc=0`). Při odpojovaném napájení
  nefunguje a u SEN6x může přesnost i zhoršit. `list` na zapnutou upozorní.
- **`senwarm` jde nastavit až na 120 s** (dřív 60).
- **Hlavička displeje**: první řádek nese oba popisky vedle sebe včetně jména
  čidla - „Teplota SEN63C   Vlhkost SEN63C“. Jméno se připisuje jen když je
  z čeho vybírat; řádek s vlhkostí nese už jen hodnotu.
- Webový konfigurátor: zrušen přepínač **normální / expertní režim**,
  zobrazuje se vždy všechno. Hotspot ho nikdy neměl, tam se nic nemění.

Přesnost podle datasheetu z čidla bez trvalého napájení nedostanete. Chce to
12 h (SEN63C) až 2 dny (SEN66) souvislého provozu. Použijte jednorázovou
kalibraci venku (`co2ref=420`), ta odpojení přežije.

---

## v4.2.1

- **Opraveno: `temp` a `temp.sen` ukazovaly totéž.** Když se hlavní teplota
  přepnula na SEN63C, kanál `temp` kreslil stejná data jako `temp.sen`.
  Nabídka veličin teď duplicity nezobrazuje a ruční `ch=` je zahodí.
- Kořen problému: hlavní hodnota se vybírala podle dostupnosti čidel, ale
  jinde podle nastaveného `tsrc`/`hsrc`. Volba zdroje je teď na jednom místě.
- Stejná chyba opravena i v jednotkách grafů, v CSV exportu a v hotspotu.

---

## v4.2.0

> **Smaže historii a vrátí konfiguraci na výchozí** (`CFG_MAGIC` = `MEK7`).

- **Každé čidlo má vlastní veličinu do grafu**: `temp.sht`, `temp.sen`,
  `temp.scd`, `temp.bme`, `temp.ds` a stejně `hum.*`. Jde tak porovnat
  SHT40 a SEN63C vedle sebe (`ch=temp.sht,temp.sen,co2,pm25`).
- **Volba hlavního zdroje**: `tsrc=<čidlo>`, `hsrc=<čidlo>`. `auto` = podle
  priority SHT40 → SEN6x → SCD41 → BME280. DS18B20 se do auta nepočítá,
  ručně zvolit jde. Změna zdroje založí novou historii.
- Zvolené čidlo se nenahrazuje jiným, když zrovna nic nevrátí - v grafu
  vznikne mezera. Míchat vzorky z různých čidel by bylo horší.
- Popisek hlavní hodnoty je jen **„Teplota“** místo „Teplota uvnitř“.
- Nabídku veličin i políčka kompenzací si obě webové konfigurace generují
  samy podle toho, co deska hlásí.

**Opraveno:** konfigurátor spadl po načtení, když v sestavě chybělo CO₂, PM,
tlak nebo DS18B20 · stránka nerozpoznala kanály s tečkou · změna sady kanálů
při stejném počtu nemazala historii (z teploty 21,5 °C se stal tlak 1115 hPa)
· `tsrc=` mazal historii i beze změny · `ch=` s víc než čtyřmi veličinami
zbytek tiše zahodilo · grafy veličin konkrétních čidel neměly jednotku.

---

## v4.1.0

> **Smaže historii a vrátí konfiguraci na výchozí** (`CFG_MAGIC` = `MEK6`).

- **Kompenzace zvlášť pro každé čidlo.** Dřív jeden `toff`/`hoff` pro celou
  stanici - korekce podle SHT40 se použila i na SEN6x, který se mýlí jinak.
  Nově `toff.sht=`, `toff.sen=`, `toff.scd=`, `toff.bme=`, `toff.ds=` a stejně
  `hoff.*`. Bez tečky se nastaví to čidlo, které hodnotu opravdu dodává.
  Nabízejí se jen připojená čidla. DS18B20 má konečně vlastní korekci.
- **Opraveny neviditelné grafy PM.** Osa měla natvrdo minimum 10 µg/m³, ale
  v čistém vzduchu se měří desetiny - horní mez se teď odvíjí od dat. A při
  `senmult > 1` se osamocené vzorky nekreslily vůbec (bod nemá s čím spojit
  čáru), teď se kreslí puntíkem. Opraveno ve firmwaru, hotspotu i webu.
- **Čtvrtý graf** (`MAX_CHANNELS` 3 → 4). Historie na kanál klesne
  z 864 na 648 vzorků.
- **`°C` místo `degC`**, `µg/m³` místo `ug/m3`. Fonty Adafruit GFX umí jen
  ASCII, takže se ty tři znaky dokreslují z primitiv - žádný další font.
- Jednotky menším písmem, hlavička posunutá nahoru (grafům to přidalo 20 px).

**Opraveno:** šipky u intervalu ve webu házely výjimku · hotspot nepustil
čtvrtý kanál · teplota barometru v řádku „Teplota 2“ ignorovala offset ·
FAQ mělo špatný popis držení tlačítka PUSH.

---

## v4.0.0

> **Smaže historii a vrátí konfiguraci na výchozí** (`CFG_MAGIC` = `MEK5`).

### Nové čidlo: Sensirion SEN6x

Používají se **oficiální knihovny Sensirionu**, ve skeči je jen tenká
rozbočovací vrstva. Detekce přes *Get Product Name* (`0xD014`) - ten příkaz je
v celé řadě stejný, takže se ptá libovolná třída a podle vráceného jména se
sáhne po správné knihovně.

| Model  | Měří                          | Knihovna  |
|--------|-------------------------------|-----------|
| SEN63C | PM, RH/T, CO₂                 | povinná   |
| SEN66  | PM, RH/T, VOC, NOx, CO₂       | povinná   |
| SEN65  | PM, RH/T, VOC, NOx            | volitelná |
| SEN68  | PM, RH/T, VOC, NOx, HCHO      | volitelná |
| SEN69C | PM, RH/T, VOC, NOx, HCHO, CO₂ | volitelná |

**VOC a NOx se záměrně nečtou** - jejich index počítá adaptivní algoritmus,
který potřebuje běžet nepřetržitě hodiny až dny. Při odpojovaném napájení by
vždy vracel konstantu kolem 100.

### Dva intervaly

- **Základní** (`int`, 1-60 min) pro teplotu, vlhkost a tlak.
- **Násobek pro PM a CO₂** (`senmult`, 1× až 4×) - ventilátor čidla bere
  90 mA, tak ať se točí co nejméně. Při 5 min a 3× je teplota po pěti
  minutách a prach po patnácti.

### Úspora energie

- Čidlo je napájené jen po dobu měření (spínač µŠup na GPIO47).
- Během zahřívání ESP32 spí v light sleepu (~1 mA místo ~40 mA), `lsleep=0|1`.
- Pod 3,45 V se SEN6x vůbec nespustí - rozběh ventilátoru je špička přes
  100 mA a na vybité baterii by shodil desku.
- Doba spánku se krátí o dobu strávenou vzhůru, rozestup vzorků tak sedí.

### Ostatní

- **Automatické čištění ventilátoru** `senauto=<dny>` (0-90, výchozí 7),
  `senclean` vyčistí hned. Odpočet přežije i výměnu baterie.
- **Nové veličiny** `pm25` a `pm10`. Priorita kanálů: CO₂ → PM2.5 → teplota →
  tlak → teplota 2 → vlhkost.
- **Verzování** `FW_VERSION` + datum překladu - na displeji, v hotspotu,
  v servisu (`ver`), v hlavičce CSV a ve webovém konfigurátoru.
- Naměřený tlak z BME/BMP280 se posílá do SEN6x jako vstup pro kompenzaci CO₂
  (lepší než nadmořská výška, zohlední i počasí). Posílá se staniční tlak.
- Řádky hlavičky displeje se řídí tím, co je připojeno, ne tím, co se zrovna
  podařilo změřit - výpadek čidla už nepřekreslí grafy jinak vysoké.
- Nové servisní příkazy: `ver`, `senwarm`, `senmult`, `senauto`, `senclean`,
  `senstat`, `lsleep`. `list` vypisuje odhad denní spotřeby čidla prachu.
- I²C je natvrdo na 100 kHz (limit SEN6x).
- Konfigurátor i hotspot: panel **Čidlo prachu SEN6x**, panel **CO₂**
  v hotspotu (dosud chyběl, přestože firmware parametry přijímal), karta
  *Firmware*, odhad spotřeby a výdrže živě podle nastavení.
