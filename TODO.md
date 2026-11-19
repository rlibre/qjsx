# TODO

Ce qui est fait est décrit dans CLAUDE.md (« Decisions taken »), `doc/architecture.md`,
`doc/engine-changes.md` et `doc/history.md` ; les défauts ouverts dans `doc/known-bugs.md`.
Ici : seulement ce qui reste à faire, à essayer ou à décider.

## Où on en est

- Derniers travaux (18/11, soir) : **carte sous Linux**, la STM32MP157-DK2 (OpenSTLinux,
  armv7l). Pas de firmware : l'outil `qjsx` compilé en croisé (`premake5 ... --cross=linux-arm`,
  `bin/linux-arm/Release/qjsx`, statique) y tourne en `qjsx agent --screen /dev/dri/card0
  --touch /dev/input/event1` : écran par DRM, tactile du noyau, `Display` et `Touch` de
  type `"builtin"` ; débogueur 72/72, proxy 33/33, cdp 53/53 par TCP ; démo `temp/dk2/ui`.
  `--screen-size` (image centrée dans un mode plus grand, tactile converti) essayé sur la
  DK2 en 400x600. Souris jouée comme un doigt, avec curseur : écrite, PAS essayée.
  Correctif du modèle gt911 du jumeau (file de rapports). Doc : `doc/architecture.md` 5.5
  « A Linux board ». Suite : Raspberry Pi 4 B (HDMI + souris), voir « Ports ».
- Derniers travaux (18/11) : images de l'écran (PNG importé `with { type: "image" }`,
  converti par `qjsx build` : RLE par ligne, alpha sur les bords, tramage ;
  `lcd.image()` / `drawImage()`), essayé sur la WT32-SC01 (démo `temp/wtimage`) et dans
  le jumeau. Puis la M5Stack Tab5 (ESP32-P4 v1.0, COM30) : firmware minimal, liaison de
  l'agent sur l'USB Serial/JTAG ; débogueur 72/72, proxy 33/33, cdp 53/53. Écran intégré
  (ILI9881C, MIPI-DSI, `type: "builtin"`) fait : plein écran 116 ms, zone 300 x 60 ~4 ms
  (232 images/s, JS 91 %), puis rendu dans une tâche : 185-189 images/s, JS 60 %. Reste
  sur la Tab5 : PPA pour la rotation, double image, alléger le côté JS d'une image (~3 ms) ; radio (ESP32-C6 en SDIO) ;
  fréquence du PWM à vérifier ; jumeau.
  Puis le tactile (`stdlib/touch` : GT911 de la Tab5, démarré par son firmware, et FT6336
  de la WT32-SC01), essayé sur les deux cartes ; démo façon LVGL `temp/tab5ui` ; dans le
  jumeau, modèles `ft6336` et `gt911` touchés à la souris (un doigt),
  `tests/touch.test.js`. Article LinkedIn : `temp/linkedin-article.md`, couverture
  `temp/cover/cover.png`, schéma `temp/overview-fr.png`. Puis le jumeau de la Tab5
  (écran intégré dessiné en mémoire, GT911 ; `tests/tab5.test.js`) et le correctif du
  lecteur de bytecode (modification Z). L'écran réel de la Tab5 vérifié avec le firmware
  reflashé (`flushScreen` par zone) ; son rétroéclairage réglable (PWM sur G22, curseur
  de la démo `temp/tab5ui`) vérifié sur la carte ; celui de la WT32-SC01 aussi (PWM sur
  GPIO23, démo `temp/wt32ui`, qui tourne sur la carte et dans son jumeau).
- (17/11) : écran de la WT32-SC01 (`stdlib/display` + `sys/draw` : liste
  de commandes rendue par bandes pendant l'envoi SPI, texte avec polices LVGL lues en
  flash, arcs, rectangles arrondis ; 15,5 images/s sur deux jauges), import de fichiers en
  octets (`with { type: "bytes" }`), `device/dac` et ses cadrans dans le jumeau, jumeau de
  la WT32-SC01 avec son écran et un SPI qui prend le temps du fil.
- Avant : mémoire et vitesse des cartes sans PSRAM (tas JS en région, paquet
  exécuté en place, constantes du compilateur, élagage des déclarations inutilisées au
  build, `await` sans allocation, stock d'états des fonctions async), port Teensy 4.0
  (jalons 1 à 3 + `device/pwm`), profileurs (`qjsx profile`, profileur C de la Teensy),
  passe des règles de codage.
- Carte cliente : application réécrite en JS avec `stdlib/tasks`, fil JS chargé à 27 % ;
  son programme a ensuite été remplacé par les bancs de mesure (à réinstaller au besoin).
- Prochaines pistes : chemin d'appel de jsbind sans `thread_local`, placement du code
  chaud guidé par profil (Teensy, peut-être ESP32), marche 3 du paquet en place, puis le
  chargement paresseux des fonctions ; passe RP2350 (`device/board`, `/data`, mesures
  mémoire) ; essais sur carte des modules jamais branchés (ADC, I2C, UART).

## À faire rapidement (utilisateur)

- [ ] **`device/spi`, suite** : essayé sur l'écran de la WT32-SC01 (fait) ; reste Teensy
      (LPSPI) et RP2350, et la lenteur de `transactionSync` (1,3 ms + 0,35 ms par segment :
      la conversion des segments, à confirmer) si un programme en a besoin.
- [ ] **Écran, suite** : tactile fait (reste : plusieurs doigts dans le jumeau, gestes) ;
      images : JPEG plus tard,
      PNG entrelacé refusé, import d'une image dans une bibliothèque non essayé ;
      vérifier le modèle `st7796-320x480` du jumeau contre la fiche du ST7796
      (known-bugs.md) ; d'autres contrôleurs (ILI9341, ST7789…) : une entrée de `TYPES`.
- [ ] **DAC, suite** : mesurer la sortie sur la WT32-SC01 (démo `temp/dacdemo` installée :
      GPIO25 à 1,00 V, rampe sur GPIO26 ; la démo du dossier est depuis un sinus 2 Hz
      de 0,7 à 2,8 V sur GPIO25 et une rampe d'1 s sur GPIO26, pas redéployée sur la
      carte ; vérifier que ces broches sortent sur un
      connecteur) ; les valeurs de `boards/esp32-devkitc-wroom32.json` écrites de mémoire
      (à vérifier) ; le JSON de jumeau de la WT32-SC01 est fait (`boards/wt32-sc01.json`).
      Plus tard : `play()` en DMA, `tone()`
      (générateur de sinus de l'ESP32).
- [ ] **WiFi** : `device/wifi` (API ACCORDÉE, CLAUDE.md « device/wifi ») ; composant
      `esp_wifi` à ajouter aux REQUIRES (minimal build).
- [ ] **`sys/net` sur les cartes** : le TCP de `sys/net` (aujourd'hui PC seulement) sur
      lwIP, au-dessus du WiFi ; ensuite le lien de l'agent en TCP sur WiFi (jeton dans
      HELLO) et peut-être UDP.
- [ ] **Passe « erreurs de l'API JS »** (règles ACCORDÉES, CLAUDE.md « Errors of the JS
      API ») sur tous les modules, par jsbind : codes uniques (`EBADF` pour un objet fermé),
      une méthode à promesse ne lève jamais (tout est rejeté), entiers stricts
      (`RangeError`) ; aussi, à confirmer : champs du matériel en lecture seule, « Illegal
      constructor », `close()` immédiat, `onX(null)`, alignement sur Node. Vérifier le
      programme de la carte cliente après. Détail : known-bugs.md « Found while writing the
      typings ».

## Moteur et vitesse

- [ ] **jsbind, chemin d'un appel sans `thread_local`** (≈ 5 accès par appel, coûteux sur
      la Teensy), valeur empruntée pour les arguments `js::Value` ; mesuré
      (`temp/bindbench`), pas fait. Autres pistes vues sur `device/gpio` (`write` 2,6 µs
      contre 1,7 µs pour `toggle`) : conversion directe de l'argument, recherche de la
      méthode.
- [ ] **Placement du code chaud guidé par profil** : sur la Teensy, l'expérience ITCM
      (chemin async, `region_heap.cpp`, `memset`) est dans les sources, marquée, gardée
      pour l'instant (utilisateur) ; à décider. Même effet probable sur l'ESP32-S3 (cache
      de 16 Ko pour un `JS_CallInternal` de 25 Ko). known-bugs.md « Speed of the Teensy
      4.0 ».
- [ ] **ESP32 sans PSRAM, causes non prouvées** : chaînes +64 % avec le tas en région
      (blocs > 128 o servis par `RegionHeap`, en flash ; piste `RegionHeap` en IRAM :
      REPORTÉE, utilisateur : « plus tard ») ; fonctions créées en boucle deux fois plus
      lentes depuis le paquet en place. Essai possible : cache d'instructions de 32 Ko
      (coûte de la RAM). engine-changes.md Q.
- [ ] **Optimiseur de bytecode, suite** : constantes exportées remplacées dans les modules
      qui les importent (3 dans l'application cliente), booléens, entrées mortes du pool de
      constantes, opcodes fusionnés (après une mesure sur la carte : cache de 16 Ko).
      engine-changes.md R.
- [ ] **Pistes notées par Bellard (« XXX » de quickjs.c)** : `for (x of tableau)` sans
      itérateur, lecture de propriété sans chemin lent pour les tableaux, `loc(a) += expr`
      en `add_loc`, `fill` / `includes` / `slice` / `sort` spécialisés pour les tableaux
      rapides. Mesures : `bash tools/profile/profile.sh`, `tools/profile/workload.js`.
- [ ] **Tests du microbench plus lents que l'original** (`prop_clone` +20 %, `prop_update`
      +14 %, `string_to_int` +14 %...) : déjà lents avec le seul découpage ; tableau et
      pistes dans `doc/quickjs-split-perf.md`.
- [ ] L'ordre des opcodes a-t-il une importance ? (numérotation de quickjs-opcode.h ;
      l'ajout de `OP_debugger` avant `nop` a décalé tous les opcodes courts).
- [ ] **Évaluer le gain à retirer BigInt** (utilisateur : « trop pour une board ») :
      taille du code (`builtins/bigint.c`, `core/bigint-arith.c`, chemins de
      l'interpréteur et des conversions), RAM, et ce qui casse (`BigUint64Array`,
      `getRandomValues` sur un tableau BigInt, littéraux `123n`) ; changement moteur à
      discuter avant de coder.

## Mémoire

- [ ] **Paquet exécuté en place, marche 3** : tables de variables (`vardefs`,
      `closure_var`) lues en place (≈ 12 Ko sur l'application cliente) ; aussi l'empreinte
      des noms vérifiée dès l'upload (aujourd'hui au lancement seulement), la RP2350.
      engine-changes.md S. Sondes : `temp/heapmap/appstatic.c`.
- [ ] **Chargement paresseux du bytecode des fonctions** (« delay-load », idée de
      l'utilisateur, accordée pour plus tard) : ≈ 150 o par fonction jamais appelée ;
      après la marche 3.
- [ ] **Pistes plus petites** : `length` / `name` des fonctions calculés (−5 à −7 Ko,
      risqué), lien de liste du ramasse-miettes (−12 Ko, lourd) ; noms des globales et
      modules de qjsx en mémoire morte, vérification automatique des fichiers générés par
      `tools/rom-atoms/generate.sh`.
- [ ] **Poches de RAM libres de la carte cliente** : un tas de 32 Ko (0x3fcf0000) et le tas
      RTC de 7,8 Ko, entièrement libres, que la région JS (un seul bloc) ne peut pas
      prendre.
- [ ] **Réserve de la région JS** (16 Ko) à régler : un paquet en RAM (`deploy --ram`,
      `qjsx debug`) de 44 Ko est impossible sur la carte cliente (plus grand bloc libre du
      firmware : 31 Ko).
- [ ] **GC au repos** : mesuré inutile pour l'application cliente (aucun cycle) ; à
      reconsidérer pour un programme qui fait des cycles.
- [ ] Compter / tracer les allocations de notre code dans `memory.cpp` (profilage).

## Débogueur et profileur

- [ ] **Points d'arrêt conditionnels, logpoints, compteurs de passages** (reportés par
      l'utilisateur) : compilés par le proxy, évalués sur le device sans aller-retour
      (architecture.md 3.6). Aujourd'hui refusés par le serveur CDP (non résolus dans VS
      Code).
- [ ] **Débuts d'instruction à la demande, par module** (accord de l'utilisateur) : la
      carte les demande quand un pas ou une pause tombe dans un module qui ne les a pas
      (événement + attente comme SCRIPT_LOADED ; drapeau par fonction testé en pas à pas) ;
      à décider après une mesure de la RAM des tables (1 bit par octet de bytecode) sur la
      carte cliente.
- [ ] Étape 4 du débogueur (`this`, aperçus d'objets, `--restart`) dans VS Code sur une
      carte : mesurer la vitesse des aperçus sur la liaison série (à vérifier : peut-être
      déjà fait).
- [ ] **Vue Profiler de VS Code** (CDP `Profiler`) sur la table de `qjsx profile`.
- [ ] Firmware de la carte cliente compilé avec `QJSX_PROFILER=ON` : repasser à OFF avant
      une livraison (à vérifier sur le firmware actuel).

## Outil, paquets et bibliothèques

- [ ] **FILE_LIST paginé** : un dossier de beaucoup de fichiers dépasse un message de
      8 Ko (« answer too large »).
- [ ] **`.d.ts` des bibliothèques** (`.qjslib`).
- [ ] `qjsx run script.js` ne connaît que `stdlib` : un script qui importe une autre
      bibliothèque doit être construit.
- [ ] **Accepter TypeScript** (utilisateur) : qui retire les types (transpileur embarqué ou
      outil externe), positions du débogueur (source maps ou lignes du `.ts`), `.d.ts`,
      vérification des types ; aucun téléchargement sans accord. À concevoir.
- [ ] **Releases** (décidé le 19/11, rien de fait ; détail dans `CLAUDE.md` « Releases and
      flashing from the tool ») : une release = l'outil et les firmwares du même commit,
      sous `QJSX_VERSION`, avec un tag git. Première release : pour des testeurs, sur
      GitHub en « pre-release » (dépôt public), Windows et Linux Arm, cartes ESP32 (YD,
      DevKitC N8R8, WT32-SC01, Tab5, Waveshare P4). Elle ATTEND le flasher ESP32. À faire,
      dans l'ordre :
  - [x] `serial.setSignals({ dtr, rts })` sur le PC (fait le 19/11, essayé sur COM32 : la
        carte Waveshare redémarre par RTS) et `serial.setBaudRate()` partout (PC, ESP32,
        jumeau ; demandé par l'utilisateur pour ses cartes ; pas essayé sur un UART) ;
  - [x] flasher ESP32 en JS externe (`flashers/esp32.js`) et commande `qjsx flash`
        (fait le 19/11, `doc/architecture.md` 6.8, `tests/flasher.test.js` 26 contrôles) :
        essayé sur la Waveshare P4 (USB Serial/JTAG, 1,38 Mo écrit et vérifié en ~19 s)
        et sur la WT32-SC01 (ESP32 d'origine, pont CP210x, 1,15 Mo en ~31 s).
        RESTE À ESSAYER : un ESP32-S3 (YD, DevKitC), la Tab5 ;
  - [ ] idée de l'utilisateur : une option pour appeler esptool à la place (l'outil ne
        sait pas lancer un autre programme aujourd'hui : à discuter) ;
  - [ ] `firmware/<carte>/` avec son `firmware.json` dans l'archive (la commande lit
        déjà ce format et le dossier de build ESP-IDF) ;
  - [ ] inventaire des licences des composants tiers, fichier de mentions dans l'archive ;
  - [ ] script d'assemblage des archives (Windows, Linux Arm), notes de version, montée
        de `QJSX_VERSION`, tag ;
  - [ ] ensuite : flasher UF2 (RP2350), puis PJRC (HID, natif).
- [ ] **CI/CD** (pour la release publique annoncée) : GitHub Actions sur un tag ; `qjsx`
      pour Linux x64 et macOS (jamais construits : portage à prévoir),
      `tests/run-tests.sh`, firmwares par carte (image ESP-IDF officielle) ; signature de
      l'exécutable ; manuel en ligne.
- [ ] **Linux et macOS** : rien n'est compilé (branches POSIX des correctifs,
      `host/platform/executable_path.cpp`, permissions des programmes du PC).
- [ ] **Éditeur Monaco pour les devices** (utilisateur) : à préciser ; x4js a un composant
      `MonacoEditor` (`x4js/monaco`).
- [ ] **Livetrace des ports** (utilisateur) : à préciser.

## Ports

### ESP32

- [ ] Liaisons suivantes : USB natif (Serial/JTAG), TCP sur WiFi (identifiants, jeton,
      NVS) ; journaux d'ESP-IDF en messages `Log` quand la session n'est pas sur UART0
      (`esp_log_set_vprintf`).
- [ ] SHA-256 matériel (accord de l'utilisateur, à faire avec chaque port) : garder
      `crypto::Sha256` (common/runtime/sha256.h), implémentation séparée : mbedTLS sur
      l'ESP32-S3, `pico_sha256` sur la RP2350, logicielle sur l'hôte. Pas urgent.
- [ ] ESP32-S3-DevKitC-1 N8R8 : pas de JSON de jumeau.
- [ ] Waveshare ESP32-P4-Module-DEV-KIT + 7-DSI-TOUCH-A (19/11, COM32) : firmware, écran
      et tactile faits (`doc/architecture.md` 5.5). Restent : une image fantôme vue UNE fois
      à travers la démo, non reproduite (ni reste en mémoire, ni rémanence : gris propre
      après une mire de 5 min), cause inconnue ; la page entière qu'on voit se dessiner
      (~0,3 s) : deux images en mémoire refusées (trop gourmand), reste le rendu plus
      rapide (rotation par le PPA) ; effacer la mémoire d'écran quand un programme ouvre l'écran
      (proposé, pas décidé) ; une option de `Display` pour un rétroéclairage par I2C ?
      radio, audio, Ethernet, SD, jumeau.

### RP2350 (Olimex PICO2-XXL)

- [ ] `device/board`, `/data` (LittleFS), `board::memoryStatus()` (renvoie false), tas sans
      PSRAM (allocateur par défaut de QuickJS, non mesuré), stock d'états X à couper
      (carte avec PSRAM), paquet exécuté en place.
- [ ] Vitesse : l'horloge (200 MHz ?) ; known-bugs.md « RP2350 port ».
- [ ] L'outil doit rouvrir le port USB après un reset de la puce (`device reset`,
      `monitor`, session de debug) ; known-bugs.md.
- [ ] `device/gpio` : `onChange` / `waitFor` pas essayés ; puis `device/rmt` sur PIO,
      `pwm`, `i2c`, `serial`, `adc`.

### Teensy 4.0

- [ ] `/data` (LittleFS : cœur littlefs à télécharger, ou la copie du composant
      ESP-IDF) ; `device/pwm` pas vérifié à l'oscilloscope.

### Cartes sous Linux (STM32MP157-DK2, Raspberry Pi 4 B)

- [ ] Souris en `--touch` et curseur DRM : jamais essayés (pas de souris sur la DK2) ; à
      essayer sur le Pi.
- [ ] Raspberry Pi 4 B (Raspberry Pi OS 64 bits avec bureau, utilisateur `eco`, clé SSH
      dans `temp/pi4/`) : vérifier que le binaire `linux-arm` (ARMv7 statique) tourne sur
      son noyau 64 bits ; `ls /dev/dri` (l'affichage est sans doute `card1`) ; souris par
      `/dev/input/by-id/*-event-mouse` ; arrêter le bureau avant l'agent ; démo
      `temp/pi4/ui` en rotation 0 ; moniteur HDMI avec `--screen-size 800x480` en attendant
      la nappe de l'écran 7" (sa taille à confirmer par l'étiquette).
- [ ] Production : l'agent lancé au démarrage (service systemd à la place du bureau) ; le
      port TCP de l'agent (jeton, écoute locale, ou pas d'agent : `--screen` / `--touch`
      n'existent que pour `qjsx agent`) ; confinement des fichiers ; à discuter.
- [ ] Rétroéclairage par Linux ; adresse fixe de la carte ; GPIO / I2C / SPI de la carte
      sous Linux ; build 64 bits.
- [ ] Mesures de vitesse de la DK2 (`temp/dk2/hello`) : pas notées dans la doc.

### WeAct Studio STM32H743 (utilisateur, après la RP2350)

- [ ] Mis de côté le 18/11 (on reprendra après la STM32MP157-DK2) : proposition et faits de
      la carte (H743VIT6) dans `doc/port-stm32h743-proposal.md`. Décidé : Cube, USB CDC,
      DFU, paquets dans la QSPI, ordre de la Teensy. Restent : TinyUSB, outil de flash,
      400 / 480 MHz, la liste des téléchargements.
- [ ] **microSD** (H743, PICO2-XXL) : `sys/fs` sur FatFS (`/sd`), confinement étendu aux
      points de montage (FatFS : vérifier CVE-2026-6685 / -6688 à ce moment-là).

## Modules matériels et bibliothèque standard

- [ ] **Défilement (scrolling) à l'écran** (utilisateur, 19/11) : à préciser avant de
      concevoir (`stdlib/display`).
- [ ] **Rétroéclairage par I2C dans `Display`** (Waveshare P4) : aujourd'hui le programme
      écrit lui-même le registre 0x96 du circuit 0x45 ; le firmware garde la lumière
      éteinte pendant l'init de l'écran et l'allume à la fin. Allumer la lumière au premier
      `show()` plutôt qu'à l'ouverture : REFUSÉ par l'utilisateur (19/11).
- [ ] Les ~70 µs de plus par `show()` attendu depuis le réveil « tous les fils » de
      `platform::Signal` (ESP32-P4, 19/11) : non expliqués.
- [ ] Un test de la suite qui rejoue « transfert I2C en file puis appel synchrone » (le
      blocage corrigé le 19/11).

- [ ] **Temps depuis le démarrage de la carte** (utilisateur : pas forcément accessible au
      JS, mais l'info quelque part) : une clé de plus dans la réponse INFO de l'agent,
      affichée par `qjsx device info` (ms depuis le démarrage de la puce,
      `platform::monotonicMs()` de chaque port ; jumeau / agent de bureau : depuis le
      dernier RESET) ; peut-être aussi la cause du dernier redémarrage (`esp_reset_reason`
      : alimentation, chien de garde, panique, restart()). Un `uptime()` dans
      `device/board` : en option, plus tard.
- [ ] **À essayer sur une carte** : `device/adc` (un potentiomètre sur GPIO1-10),
      `device/i2c` avec un vrai composant, `device/serial` sur un UART (rien de câblé pour
      l'instant, utilisateur), `gpio.waitFor` (known-bugs.md « To verify »).
- [ ] `device/pwm` : fondu, `stdlib/servo` (plus tard).
- [ ] Lecture série avec délai (`readLine` en promesse), en JS dans la stdlib (trames d'un
      lecteur RFID).
- [ ] `stdlib/tasks` : une option pour qu'une erreur n'arrête pas la boucle d'une tâche
      (aujourd'hui try / catch à la main).
- [ ] I2C hors du natif : helpers de registres (`readWord`...), pilote d'un switch
      PCA9548A, pilotes de cartes en JS ; pas de mode esclave ni d'adresse 10 bits pour
      l'instant.
- [ ] Question ouverte : firmware générique par carte ou par projet (modules activés par
      option, `--gc-sections`) ?

## Carte cliente

- [ ] **Relais USB pour le jumeau** (utilisateur : la tablette ne parle qu'en USB, son
      logiciel est trop gros pour un script) : un ESP32-S3 en « dongle » (USB natif vers la
      tablette, port COM vers le PC), petit firmware C dédié (pas qjsx) : descripteurs
      demandés par le programme, octets et DTR / RTS en trames `0x00 | COBS(...) | 0x00` ;
      côté jumeau `qjsx agent --board ... --usb-relay COM11`, un second hal USB.
- [ ] PLUS TARD (utilisateur) : WiFi et serveur TCP sur la carte (mode debug du firmware C
      d'origine) ; mise à jour du programme par la tablette (une API JS qui reçoit et
      installe un `.qjspkg`, signature à décider).
- [ ] Respiration de la LED (≈ 9 % du fil JS) et scrutation RFID (≈ 15 %) : utilisateur,
      on n'y touche pas pour l'instant.

## Jumeau numérique

- [ ] D'autres pièces : écran SSD1306 (`PixelDisplay`), composant « flash » (utilisateur :
      quel composant, quel bus, à préciser) ; SPI et UART joués par des modèles ;
      `pin.pwm` pour les modèles.
- [ ] Analyseur : mode Auto, holdoff et position du déclenchement réglables, décodeurs
      dans la page.
- [ ] **Simuler la vitesse de la carte** (utilisateur) : un réglage pour que le programme
      tourne à peu près à la vitesse de la vraie carte (ralentir l'interpréteur, budget
      d'instructions par tranche de temps...) ; à concevoir.
- [ ] **Scénarios de test** (REPORTÉS, utilisateur) : un fichier JS du projet ; une
      commande démarre le jumeau, déploie en RAM, lance le programme et le scénario
      (`board.part(id)`, `board.output.waitFor(regex, { timeout })`, `board.waitFor(fn)`,
      `press({ ms })`, fautes `fail("nack")`). Où il tourne : (a) dans le runtime du
      jumeau (sans navigateur, code de sortie pour la CI) ; (b) dans la page ; (c) même API
      des deux côtés. Recommandé : (a) avec une API asynchrone.
- [ ] **Signaux** (grandeurs qui évoluent : température du BME280...) : REPORTÉS
      (utilisateur). Idée de l'utilisateur : un script « environnement » du projet chargé
      par la page (`meteo.set({ temperature })`), débogable dans les devtools ; une page
      doit être ouverte, un seul onglet pilote. Mini-langage JSON (`sine`, `ramp`) REJETÉ.
- [ ] **Le jumeau dans VS Code** (utilisateur) : la page dans VS Code au lieu d'un
      navigateur ; à concevoir. Déboguer les modèles dans VS Code : plus tard.
- [ ] D'autres cartes (PICO2-XXL, DevKitC...) ; **assistant de conception d'une carte**
      (utilisateur) qui fabrique `boards/<carte>.json` (broches, UART, ADC, PWM, natifs,
      pièces) ; à préciser.
- [ ] **Tas JS dans la page du jumeau** (idée de l'utilisateur) : aujourd'hui seulement
      `qjsx device info` ; peut-être une taille de message dans le JSON de la carte (8 Ko
      pour le jumeau d'une carte sans PSRAM).
- [ ] En attente de l'utilisateur : appliquer le menu réglages aux lignes TCP des
      fonctions USB.
- [ ] `twin.state` renvoyé à chaque tour d'une boucle de `toggle()` (127 Ko/s,
      `temp/togglestress`) : à améliorer si l'utilisateur le veut.
- [ ] UART du jumeau sur un vrai port série du PC : à essayer avec un adaptateur
      USB-série, TX relié à RX.
- [ ] Couleur d'une LED : à affiner plus tard par une table (utilisateur).
- [ ] **Le jumeau en option, version pro payante** (utilisateur) : licence, activation, ce
      qui reste dans la version libre ; à concevoir.

### Défauts x4js relevés (pour l'utilisateur : x4js est en lecture seule pour nous)

- [ ] `.x4canvas` sans `position: relative` ; `CanvasProps` non documenté.
- [ ] Un sous-menu qui ne tient pas à droite ne s'ouvre pas à gauche (`Popup.displayAt`).
- [ ] `DockingView` : la bande de droite ne rétrécit pas avec la fenêtre et écrase le
      milieu (la borne « place − MIN_AREA » du séparateur n'est pas appliquée au
      redimensionnement de la fenêtre).
- [ ] Pas d'outil de téléchargement (à mettre à côté de `MessageBox`).
- [ ] La copie de glisser du dragManager déborde (`position: fixed`).
- [ ] `Switch` n'a ni événement `change` ni `getCheck()` (une `Checkbox` bouton à la
      place).
- [ ] À VÉRIFIER : `dispatchEvent` (`core_dom.ts`) semble distribuer deux fois un
      événement qui remonte quand un ancêtre a un gestionnaire du même type (seul
      `preventDefault` l'évite).

## Manuel et documentation

- [ ] **Publier le manuel** (`doc/jsmanual/`, GitHub Pages, avec la CI).
- [ ] **`npm audit` du site** : 4 vulnérabilités « moderate » (minimatch,
      GHSA-g3hv-75wg-m249, tirée par TypeDoc ; outil de construction seulement) ; le
      correctif de npm rétrograde TypeDoc et casse le site : attendre une version corrigée
      ou un « override » de minimatch, à trancher.
- [ ] **Documentation de jsbind + guide** (utilisateur) : `doc/jsbind.md` en anglais :
      principes (pas de std::, pas d'exceptions, « first error wins »), macros, types,
      erreurs, promesses, `init()`, `keepAlive`, durée de vie des objets, et un guide pas à
      pas pour écrire un module natif (exemple : `device/gpio`).
- [ ] **Article LinkedIn** (utilisateur) : assez complet, avec de belles images (jumeau,
      architecture...).

## Tests et défauts

- [ ] Défauts ouverts de `doc/known-bugs.md` : les « mineurs » attendent l'utilisateur ;
      ceux relevés pendant les corrections, en écrivant les `.d.ts` et pendant la passe
      des règles de codage.
- [ ] Un test ciblé par défaut corrigé, quand c'est possible sans matériel (demande
      initiale pour les « graves »).
- [ ] `C:\temp` contient des sauvegardes et des scripts des agents (doc-orig, pseudo-*,
      jsbind-orig, doc-trial, final-style, espjs-backup-before-style…) qu'on peut
      supprimer.

## Idées à discuter ou reportées

- [ ] **Threads ou workers** (utilisateur) : PROPOSITION non validée : Web Workers (API du
      web, comme Deno), un runtime QuickJS par worker sur sa tâche, messages copiés par
      `JS_WriteObject` / `JS_ReadObject` dans la boîte aux lettres de la boucle de l'autre
      thread (plus tard `SharedArrayBuffer` + `Atomics`), module du worker = module du
      programme, limites par carte (cher sans PSRAM : un runtime + une pile), modules
      matériels et débogueur au thread principal d'abord, `process.exit()` arrête les
      workers. Questions posées : cas d'usage, API web ou `worker_threads`, debug des
      workers, limites par carte. Note : les tâches du firmware C de la carte cliente
      tiennent sur le fil JS unique (`stdlib/tasks`) ; seul du calcul long justifierait
      des workers.
- [ ] **Tâches préemptées (« faux threads »)** (idée de l'utilisateur, ÉTUDIÉE, REPORTÉE :
      inutile pour les petites tâches) : une tâche = un générateur ; au point de contrôle
      de l'interpréteur (saut en arrière, appel), si la tranche est écoulée et que le cadre
      est celui d'un générateur marqué, sortir avec un nouveau code `FUNC_RET_PREEMPT` ;
      reprise sans valeur ; ordonnanceur en JS dans la stdlib. Pas d'opcode, pas de
      changement de BC_VERSION. Limites : pas de coupure dans une fonction ordinaire ou
      native appelée par la tâche, `return()` impossible sur une tâche préemptée. Écartés :
      threads à la Python (une pile C par thread, fin de « un callback n'est pas
      interrompu »), interpréteur sans pile, workers (≈ 62 Ko par moteur).
- [ ] **Budget de temps pour un script coincé** sur un device autonome (une boucle JS sans
      fin n'est pas vue par le chien de garde, exprès) ; architecture.md 5.5.
- [ ] **FFI** (utilisateur) : appeler du C depuis le JS sans module jsbind (déclarer une
      fonction native et ses types en JS) ; sur quels devices, quelle sécurité, lien avec
      jsbind et le « code C chargeable » abandonné.
- [ ] **OTA, mise à jour du firmware à distance** (utilisateur : DISCUTABLE ; la table de
      16 Mo réserve ota_0 / ota_1, à revoir si l'OTA est abandonnée ; un firmware de
      ~1 Mo tiendrait aussi sur 4 Mo). Pistes : messages Upload Begin / Chunk / End avec un
      type (paquet ou firmware), sur toutes les liaisons ; ESP32 `esp_ota_*` + retour
      arrière automatique (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) ; RP2350 images A / B
      de la boot ROM ; commande du genre `qjsx device update firmware.bin`.
- [ ] **« Fusible » contre le debug d'une carte en production** (utilisateur) : bloquer
      debug (et sans doute déploiement, lecture des fichiers...) d'une carte livrée ;
      irréversible ou non (eFuse ou drapeau en flash), ce que l'agent bloque, comment une
      carte revient en atelier ; lien avec l'entrée suivante.
- [ ] **Protéger le code d'un programme sur le device** (utilisateur : PLUS TARD) : les
      chaînes du bytecode sont en clair et il se décompile. ESP32 : chiffrement matériel de
      la flash (XTS-AES, clé en eFuse ; l'exécution en place continue de marcher), flag
      `encrypted` sur la partition `packages` (esp_partition_write, pas _raw) + Secure
      Boot ; modes Development / Release (irréversible). Restent en clair : la RAM et la
      liaison pendant `deploy`. RP2350 : boot signé + clé en OTP, pas de déchiffrement
      transparent de la XIP.
- [ ] **Secrets sur un device : NVS chiffrée ?** (idée de l'utilisateur) : nos fichiers
      sont sur LittleFS, pas dans la NVS. ESP-IDF : clés dans `nvs_keys` protégée par le
      chiffrement de la flash, ou clé dérivée d'une clé HMAC en eFuse ; les deux brûlent
      des eFuses : sur une carte de test, jamais sur la carte cliente sans accord.
