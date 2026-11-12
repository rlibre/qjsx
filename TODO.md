# TODO

## Où on en est (2026-11-11, à relire en reprenant le travail)

- [x] **Projet renommé `qjsx`** (ex-`espjs`) le 2026-11-11 : commande `qjsx`, fichier de
      projet `qjsx.json`, modules de l'outil `qjsx:nom`, macros `QJSX_*`, marqueur `qjsx:`
      dans le moteur, firmware `qjsx.bin`. Les bibliothèques du moteur sont les projets premake
      `qjsx-engine` et `qjsx-engine-target`. Seul le dossier de travail garde l'ancien nom
      (`C:\Sandbox\espjs`). Tests PC OK en Release et en Debug (49 vérifications), tests
      d'origine de QuickJS OK. Firmware `qjsx.bin` flashé et vérifié sur la carte le
      2026-11-11 : elle s'annonce « qjsx esp32s3 », `process.argv` vaut `["qjsx", ...]`,
      déploiement, refus d'un paquet d'un autre moteur et debugger (52/52) OK. Un projet
      existant doit renommer son `espjs.json` en `qjsx.json`.
- [x] **Dépôt git** : https://github.com/rlibre/qjsx.git, branche `main`. `README.md` en
      anglais. **Documentation en anglais** (2026-11-11) : tous les fichiers de `doc/` sont
      traduits ; `TODO.md` et `doc/prompt-reprise.md` (hors dépôt) restent en français.

- [x] Documentation du code terminée le 2026-11-11 : `/** */` court sur chaque fonction,
      déroulement en pseudo-code dans les corps, `//` sur chaque membre, `case` indentés,
      séparateurs `// ### titre ###`. Contrôles : `tools/undocumented.pl` et
      `tools/undocumented-members.pl` ne trouvent plus rien ; tests Release et Debug OK.
- [x] Style des commentaires tranché le 2026-11-11 : un commentaire au-dessus de chaque groupe
      (comme agent.cpp aujourd'hui) ; `/** */` décrit paramètres et retour seulement s'ils ne
      sont pas évidents. Noté dans CLAUDE.md et doc/coding-rules.md.
- [ ] **Défauts relevés par les agents** : doc/known-bugs.md (les défauts ouverts) et
      doc/history.md (les défauts corrigés, avec ce qui a été fait). « Graves » et « moyens »
      CORRIGÉS le 2026-11-11 ; les tests passent en Release et en Debug (chemins, allocations
      et `platformName` compris). Restent :
      - écrire un test ciblé par défaut corrigé, quand c'est possible sans matériel (demande
        initiale pour les « graves ») ;
      - les défauts relevés pendant les corrections (section de known-bugs.md) ;
      - les « mineurs », qui attendent l'utilisateur.
- [x] **Chemins Windows** (demande du 2026-11-11, fait le même jour) : tout en `/` (Windows
      les accepte). Une seule fonction, `files::useSlashes` (Windows seulement ; sur POSIX `\`
      est un caractère de nom), utilisée par `currentDirectory`, `qjsx run` (host.cpp) et
      l'URL de `import.meta` ; même règle dans `toSlashes` du module `path`. Les chemins
      `\\?\` (longs) et `\\.\` (périphériques, COM10...) gardent leurs `\` (accord de
      l'utilisateur). Tests OK le 2026-11-11.
- [x] **Allocations vérifiées** (demande du 2026-11-11, fait le même jour) :
      `common/runtime/memory.{h,cpp}` (`allocate`, `reallocate`, `release`, `copyText`), un
      manque de mémoire est fatal (`platform::fatal`) ; malloc / realloc / free remplacés dans
      tout notre code, tests de nullptr devenus morts retirés. Tests OK le 2026-11-11.
      Plus tard : compter / tracer les allocations
      (profilage) dans memory.cpp ; `platform::fatal` à écrire pour chaque port (redémarrage).
- [ ] **Port ESP32-S3** (YD-ESP32-S3, WROOM-1 N16R8, port COM / UART0 = COM11, ESP-IDF v6.1) :
      - [x] firmware COMPILÉ le 2026-11-11 (ports/esp32, 709 Ko sur 2,5 Mo) : structure, partitions
        (option B), carte, plateforme, lien UART0 ; deux changements moteur newlib (I) ;
        `board::jsStackSize()` ajouté à board.h.
      - [x] flashé et essayé le 2026-11-11 : agent par UART0, déploiement + exécution sur place,
        debugger 52/52 (`qjsx run tests/debugger.test.js serial:COM11@921600`). Structure par
        carte (ports/esp32/boards/, partitions/) faite et revalidée sur la carte.
      - [x] deux défauts trouvés sur la carte, CORRIGÉS le 2026-11-11 (tests run-tests.sh,
        vérifiés sur la carte) : l'arrêt d'un programme n'affiche plus « interrupted » (le
        runner termine la boucle avant l'interruption) ; `deploy --until-exit` attend la fin du
        nouveau programme ;
      - [x] chien de garde : un programme JS qui calcule sans rendre la main affame la tâche
        idle du cœur 1, ESP-IDF affiche « task_wdt: ... IDLE1 » toutes les 5 s (pas de
        redémarrage). DÉCIDÉ le 2026-11-11 : `platform::yieldIfDue()` appelé par le
        gestionnaire d'interruption (`Runner::interrupt`), pause d'un tick toutes les 100 ms
        (1 % au plus, tick de 1 ms). Codé (platform.h, platform_host.cpp vide,
        ports/freertos/platform_freertos.cpp, runner.cpp), tests PC OK en Release et en Debug,
        documenté dans architecture.md 5.5. VÉRIFIÉ sur la carte le 2026-11-11 : firmware
        flashé, `tests/fixtures/debuggee` déployé, console écoutée 17 s pendant la boucle
        `spin` : aucun « task_wdt » (une pause d'un tick suffit) ; debugger 52/52 sur la
        carte. À discuter plus tard : un budget de temps pour un script coincé sur un device
        autonome ;
      - [x] `import "path"` échouait sur la carte (module embarqué en source, pas de
        compilateur). FAIT le 2026-11-11 : bibliothèques `.qjslib` (architecture.md 4.5,
        CLAUDE.md « JS libraries »). Modules nommés `<bibliothèque>/<module>`
        (`stdlib/path`), `qjsx mklib`, `stdlib.qjslib` fabriqué à côté de l'exécutable
        depuis `lib/stdlib/`, modules importés copiés entiers dans le paquet par
        `qjsx build`, `import()` refusé par le compilateur (changement moteur J), plus
        aucun JS dans le firmware. Tests PC OK en Release et en Debug (10 vérifications de
        plus dans run-tests.sh, 43 en tout ; tools.test.js), tests d'origine de QuickJS OK,
        microbench −3,9 % contre l'original (pas de régression). Sur la carte :
        `tests/fixtures/uselib` (importe `testlib/greet` et `stdlib/path`) déployé, sortie
        identique à celle attendue. Firmware sans JS (700 Ko, 8 Ko de moins) FLASHÉ le
        2026-11-11 et revérifié sur la carte : `uselib` donne la sortie attendue, debugger
        52/52. Le port série lent sous Windows, trouvé pendant cet essai, est corrigé
        (history.md).
        Restent, non faits :
        - FAIT le 2026-11-11 : `qjsx deploy` compare `natives` du manifeste à la liste
          que le firmware donne dans `HELLO` (clé `NATIVES`), et n'envoie à la carte que le
          paquet réduit (manifeste + bytecode : demo 5 944 octets au lieu de 52 896) ;
          vérifié sur la carte, avec la réponse à `RESET` qui n'est plus perdue ;
        - les `.d.ts` des bibliothèques ;
        - `qjsx run script.js` ne connaît que `stdlib` (un script qui importe une autre
          bibliothèque doit être construit) ;
        - les branches Linux et macOS de `host/platform/executable_path.cpp` sont écrites
          mais pas compilées.
      - [x] FAIT le 2026-11-11 (architecture.md 4.1, engine-changes.md K) : le device
        refuse un paquet compilé pour un autre moteur (numéro de bytecode dans le manifeste,
        contrôlé par l'agent à l'envoi et par le chargeur au lancement) ; une bibliothèque
        d'un autre compilateur est recompilée depuis ses sources, avec un message ;
        `qjsx build` et `qjsx mklib` préviennent de `eval()` et du constructeur `Function`
        avec leur position. Tests PC OK en Release et en Debug (49 vérifications). Firmware
        FLASHÉ le 2026-11-11 et vérifié sur la carte : un paquet d'un autre numéro de
        bytecode est refusé à l'envoi avec son message, le paquet installé reste, le
        déploiement normal et le debugger (52/52) marchent.
        Microbench non refait (mesure trop bruitée, mis de côté par l'utilisateur) ;
      - [ ] liaisons suivantes : USB natif (Serial/JTAG), TCP sur WiFi (identifiants, jeton,
        NVS) ; journaux ESP-IDF en messages `Log` quand la session n'est pas sur UART0
        (`esp_log_set_vprintf`) ;
      - [ ] `fs` sur la partition `data` (LittleFS : composant à télécharger, accord requis),
        puis `board::dataDirectory()` ;
      - `process.platform` = "esp32", `argv` / `env` / `cwd` / `exit` faits (via l'agent de
        bureau, même code sur la carte).
      - firmware de 709 Ko : l'OTA (deux emplacements de 1,75 Mo) tiendrait sur 4 Mo.
      - SHA-256 matériel (accord du 2026-11-11, à faire avec chaque port) : garder l'interface
        `crypto::Sha256` de common/runtime/sha256.h et séparer son implémentation, logicielle
        par défaut (hôte). ESP32-S3 : mbedTLS `mbedtls_sha256_*` (accélérateur utilisé avec
        CONFIG_MBEDTLS_HARDWARE_SHA, partage géré par mbedTLS). RP2350 : `pico_sha256` du
        pico-sdk (un seul accélérateur, pris sous verrou). Pas urgent : sert à vérifier les
        paquets reçus et au `crypto.sha256` du JS.
- [ ] **Modules matériels par carte** (gpio, pwm, i2c, spi, uart, adc, rmt, ws2812, wifi...
      ; rmt et ws2812 ajoutés par l'utilisateur le 2026-11-11 : RMT = périphérique de
      trains d'impulsions de l'ESP32, PIO sur le RP2350 ; ws2812 = LED adressables, au-dessus
      de rmt sur l'ESP32). DÉCIDÉ le 2026-11-11 (CLAUDE.md « Names of the native
      modules », « device/serial », « device/gpio ») : préfixes `sys/` (fs, crypto,
      process, net, compiler) et `device/` (serial, gpio, i2c...), classes `new Gpio(pin,
      { mode, pull })` et `new Serial(port, { baudRate })`, broches par numéro, aucune
      vérification. On commence par `device/gpio` et un test de bascule sur GPIO1 mesuré à
      l'oscilloscope. RENOMMAGE FAIT le 2026-11-11 (sys/fs, sys/crypto, sys/net,
      sys/compiler, device/serial avec la classe Serial ; `process` reste un global ; un
      import à l'ancien nom est refusé par le build ; tests en Debug). Firmware reflashé.
      `device/gpio` CODÉ et FLASHÉ le 2026-11-11 (classe Gpio : registres W1TS/W1TC, toggle
      sur le registre de sortie, interruptions -> tâche gpio-events -> boucle, anti-rebond).
      Mesures à l'oscilloscope sur GPIO1, YD-ESP32-S3 à 240 MHz, tas JS en PSRAM :
      `write(1)`/`write(0)` en boucle 183 kHz, `toggle()` 247 kHz ; soit environ 1,7 µs par
      appel sans argument et 2,6 µs pour `write` (l'argument passe par js::Value).
      L'utilisateur : « ça explose tous mes précédents essais qjs » ; optimisations
      laissées de côté (pistes : conversion directe de l'argument de write, tas en RAM
      interne, chemin d'appel de jsbind, recherche de la méthode). Pas encore essayés sur la
      carte : onChange, waitFor, close. Ancienne note : proposition
      faite le 2026-11-11, en discussion. Trois couches (module JS commun écrit avec jsbind,
      interface `hal/*.h` en fonctions libres, implémentation par port), table de la carte
      (broches, fonctions, réservées, noms comme `LED`), un module par fonction (jamais par
      puce) enregistré seulement si le port le fournit et si l'option Kconfig / CMake est
      active (`--gc-sections` retire alors les piles du SDK), liste des modules dans `Hello`
      vérifiée par `qjsx deploy`, `.d.ts` par cible. Questions posées : firmware générique
      par carte ou par projet ? API JS à nous ou proche d'une existante ?
      Code C chargeable à la volée (« pseudo .lib ») : abandonné par l'utilisateur (2026-11-11).
- [x] **Doc « créer un nouveau device »** (demande de l'utilisateur, 2026-11-11) : un guide
      pas à pas, `doc/new-device.md`, pour deux cas :
      - une nouvelle carte d'une puce déjà portée (ESP32) : dossier `ports/esp32/boards/<carte>/`
        (`sdkconfig.board` : puce, flash, table de `partitions/`, PSRAM ; `board_config.h`),
        build avec `board=<carte>`, vérifications (esptool flash-id, `qjsx device info`,
        déploiement, tests du debugger sur la carte) ;
      - une nouvelle puce (nouveau port, comme le RP2350) : `ports/cmake/qjsx.cmake`,
        `ports/freertos/` ou équivalent, ce que `platform.h` et `board.h` demandent (fonction par
        fonction, threads, propriété), point d'entrée, liaisons, stockage des paquets, pièges
        rencontrés (int32_t = long, newlib, `-Werror` d'IDF, PSRAM octale ou quad).
      ÉCRIT le 2026-11-11 par un agent (`doc/new-device.md`, en anglais), CONTRÔLÉ en entier
      contre le code le 2026-11-11 (trois imprécisions corrigées : norme C++, renvoi d'un
      morceau d'envoi, en-tête valide), lié depuis `README.md`. Ce que l'agent a relevé dans
      le code (macro en double dans `qjsx.cmake`, commentaires d'en-tête périmés,
      `board::identity()` jamais appelée, une seule liaison à la fois, picolibc au lieu de
      newlib) est listé dans `doc/prompt-reprise.md`. Les écarts relevés pendant le
      contrôle sont dans `doc/known-bugs.md`.
- [x] **`doc/architecture.md` relu et remis à jour** par rapport au code le 2026-11-11
      (statuts, noms des messages du protocole, commandes, organisation du dépôt).
- [ ] **Debugger côté PC** (chantier choisi le 2026-11-11). D'abord le pas-à-pas over /
      out : CODÉ le 2026-11-11 (changement moteur L : numéro d'activation par frame,
      `is_async` ; en plus, les frames `async` et générateurs n'étaient pas vues comme du
      bytecode par core/debug.c, corrigé). Fin de chaîne comme V8 ; un « over » depuis une
      fonction `async` attend cette fonction ; un générateur rend la main à son appelant.
      `tests/debugger.test.js` : 72/72, tests en Debug seulement (demande de
      l'utilisateur). RESTE le test final : Release, tests d'origine de QuickJS, microbench
      au calme, firmware recompilé et debugger sur la carte. Puis le cœur du proxy sans CDP :
      CODÉ le 2026-11-11 (`host/js/debuginfo.js`, `host/js/debugsession.js`, API dans
      CLAUDE.md ; `tests/debuginfo.test.js` 20/20, `tests/proxy.test.js` 30/30, en Debug
      seulement ; à essayer aussi sur la carte). Ensuite le serveur CDP : CODÉ le 2026-11-11
      (`qjsx debug`, `host/js/websocket.js`, `host/js/cdp.js`, architecture.md 3.9 ;
      `crypto.sha1` ; base64 était déjà dans le moteur ; `websocket.test.js` 24/24,
      `cdp.test.js` 50/50 en Debug). Essayé dans VS Code par l'utilisateur le 2026-11-11
      (agent de bureau, projet d'essai C:\temp\qjsx-try) : « ça marche super ». Test final
      du 2026-11-11 : Release et Debug OK, tests d'origine de QuickJS OK, firmware compilé
      et FLASHÉ, sur la carte debugger 72/72, proxy 30/30, cdp 50/50 ; microbench −3,3 %
      contre l'original (pas de régression). Étape 4 (sans conditions, logpoints ni
      compteurs, reportés au backlog) : CODÉE le 2026-11-11 (CLAUDE.md « Pause on
      exceptions » et « Debugger step 4 », engine-changes.md M) : prédiction « uncaught »
      statique dans le moteur, `qjsx debug --restart` à la place de `--wait-debugger`, `this`
      et aperçus d'objets. Tests en Debug (debug.test.js 45/45, proxy 30/30, cdp 53/53) ;
      firmware FLASHÉ et sur la carte debugger 72/72, proxy 30/30, cdp 53/53. Pas encore
      essayée dans VS Code (aperçus : mesurer la vitesse sur la liaison série). Ensuite le
      serveur CDP, VS Code. Détail dans `doc/prompt-reprise.md`.
- [x] **Diagramme du principe de fonctionnement** (demande de l'utilisateur, 2026-11-11) : un
      diagramme en SVG qui explique le fonctionnement global du système (tout qjsx : PC,
      outil hôte, paquets, liaisons, agent, moteur sur le device, debugger). Emplacement décidé :
      un fichier SVG dans `doc/`, affiché en tête de doc/architecture.md. Dessiné le
      2026-11-11 dans `doc/architecture.svg`, affiché en tête de doc/architecture.md.
- [ ] **Threads ou workers** (demande de l'utilisateur, 2026-11-11) : réfléchir à la
      possibilité de faire tourner du JS sur plusieurs threads (ou des workers). À étudier :
      un runtime QuickJS par thread, ce que ça coûte en mémoire sur un device, comment les
      threads échangent (messages), et ce que ça change pour la boucle d'événements, l'agent
      et le debugger.
- [ ] **Statistiques du device** (demande de l'utilisateur, 2026-11-11) : l'agent doit pouvoir
      envoyer des statistiques (mémoire, etc.). À concevoir : quelles valeurs, quel message du
      protocole, quelle commande de `qjsx`.
- [ ] **FFI** (demande de l'utilisateur, 2026-11-11) : à discuter. Appeler du C depuis le JS
      sans écrire de module jsbind (déclarer une fonction native et ses types côté JS) ?
      Questions : sur quels devices, quelle sécurité (pas de protection mémoire), lien avec
      jsbind et avec le « code C chargeable » abandonné.
- [ ] **OTA : mise à jour du firmware à distance** (demande de l'utilisateur, 2026-11-11, à
      prévoir dès la conception des ports ; conception à discuter avant de coder). L'utilisateur
      la juge DISCUTABLE (2026-11-11) : la table de 16 Mo réserve aujourd'hui ota_0 / ota_1
      (3 Mo chacun), à revoir si l'OTA est abandonnée. Pistes :
      - même chemin que les paquets : messages Upload Begin / Chunk / End de l'agent, avec un
        type (paquet ou firmware), sur toutes les liaisons (série, TCP) ;
      - ESP32-S3 : partitions `ota_0` / `ota_1` + `otadata`, `esp_ota_begin/write/end`,
        retour arrière automatique si la nouvelle image ne confirme pas son démarrage
        (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) ;
      - RP2350 : la boot ROM gère des images A / B (table de partitions, essai avant
        validation, « try before you buy ») ;
      - table de partitions à prévoir dès le premier port : deux emplacements firmware +
        partition des paquets (+ données si `fs` existe sur cible) ;
      - compatibilité : le firmware annonce son identifiant dans `Hello`, un paquet compilé
        pour un autre moteur (format de bytecode) est refusé ; commande hôte du genre
        `qjsx device update firmware.bin`.
- [ ] **Port RP2350** (Olimex PICO2-XXL, PSRAM sur GPIO8) : conception CONVENUE le 2026-11-11
      (CLAUDE.md « RP2350 port design ») ; chaîne installée dans `C:\tools\pico` ; ÉCRIT le
      même jour (`ports/rp2350/`, `tools/pico.ps1`) : FreeRTOS SMP, liaison USB CDC, paquets
      lus en place en flash, tas JS en PSRAM avec l'allocateur maison `memory::RegionHeap`
      (testé sur le PC : 37 vérifications, et toute la suite Debug avec `QJSX_JS_HEAP_MB=64`).
      Le firmware compile (740 Ko). FLASHÉ et ESSAYÉ le 2026-11-12 (COM13) : deploy, run,
      tampon de 4 Mo en PSRAM, debugger 72/72, proxy 30/30, cdp 53/53. RESTE :
      - la vitesse (2026-11-12, known-bugs.md « Speed ») : code chaud du moteur en SRAM et
        `-fira-region=one` : jusqu'à ×3 ; toggle GPIO15 122 → 132 kHz ; par cycle, proche
        de l'ESP32-S3. Restent : l'horloge (200 MHz ?), et les pistes algorithmiques
        (entrée suivante) ;
      - l'outil PC doit rouvrir le port USB après un reset de la puce (known-bugs.md) ;
      - `device/gpio` ÉCRIT le 2026-11-12 (toggle natif SIO, drain ouvert par la direction,
        anti-rebond partagé avec l'ESP32 dans `ports/freertos/gpio_watch`) ; toggle de GPIO15
        mesuré à l'oscillo : 122 kHz avec le trou de yieldIfDue (ESP32-S3 : 247 kHz) ;
        `onChange` / `waitFor` pas essayés ;
        le firmware ESP32 recompile avec le module partagé, pas reflashé ;
      - ensuite `device/rmt` sur PIO.
- [ ] **Carte client ESP32-S3** (utilisateur, 2026-11-12) : ESP32-S3, 4 Mo de flash, PAS de
      PSRAM (tas JS en RAM interne, chemin jamais essayé : à mesurer), deux ports USB comme
      la YD : un port de debug (pont USB-série sur UART0, la liaison de l'agent) et l'USB
      natif pour la tablette du client, qui ne parle qu'en USB. Les détails (firmware C
      d'origine, brochage, liste de ce qu'il faut porter) sont dans les notes du dossier de
      la carte, ignoré par git (par `.git/info/exclude`, local, pas `.gitignore`). Ordre
      convenu :
      - [x] `device/usb`, générique (puces avec USB OTG : S2, S3, P4 ; pas le RP2350, dont
            l'USB porte l'agent). FAIT le 2026-11-12 (CLAUDE.md, architecture.md 5.4 et
            6.6) : `common/hal/hal_usb.h`, `common/device/usb_module.cpp`,
            `ports/esp32/main/hal_usb_esp32.cpp` (esp_tinyusb 2.4.0 téléchargé,
            `ports/esp32/main/idf_component.yml`), `host/twin/sim/twin_usb.cpp` + `qjsx agent
            --usb-port`, `lib/stdlib/src/usb.js` (`UsbSerial extends Usb`),
            `tests/usb.test.js` 18/18, suite Debug 87 OK. Correctif à côté :
            `js::failSystem` coupe `what` à 400 octets (-Werror=format-truncation de GCC 15
            sur `fs.cpp`, travail des permissions). FLASHÉ et ESSAYÉ sur la YD (USB natif =
            COM26, script `temp/usbboard.js`, le PC joue la tablette avec device/serial) :
            erreurs JS, écho, 3000 octets, reprise par un nouveau programme sans perdre le
            port, close() : tout OK. Décidé après l'essai : `write()` ne regarde plus DTR
            (Windows ouvre le port avec RTS seul) ; ce que l'hôte ne prend pas en 50 ms est
            perdu sans erreur. Pas de `.d.ts` : aucun module n'en a encore. Pas mesuré : le
            coût en RAM sans PSRAM ;
      - [ ] relais USB pour le jumeau (utilisateur, 2026-11-12 : la tablette ne parle qu'en
            USB et son logiciel est trop gros pour être joué par un script) : une YD (ou
            tout ESP32-S3) en « dongle », USB natif vers la tablette, port COM vers le PC ;
            un petit firmware C dédié (pas qjsx) présente les descripteurs demandés par le
            programme, fait passer les octets et remonte DTR / RTS, en trames `0x00 |
            COBS(...) | 0x00` ; côté jumeau `qjsx agent --board ... --usb-relay COM11`, un
            second hal USB qui parle au relais au lieu du TCP. La vraie tablette parle alors
            au programme JS du jumeau, debugger compris ;
      - [ ] la board : `sdkconfig.board` (4 Mo sans PSRAM), `board_config.h` et le JSON du
            jumeau (dossier et fichier ignorés par git) ;
      - [x] `device/serial` sur un UART de carte (un lecteur sur UART). FAIT le 2026-11-12
            pour l'ESP32 et le jumeau (CLAUDE.md, architecture.md 5.4 et 6.6) : un seul
            module `common/device/serial_module.cpp` sur un PILOTE (`common/hal/hal_serial.h`,
            table de fonctions : PC `host/platform/serial_host.cpp`, ESP32
            `hal_serial_esp32.cpp`, jumeau `twin_uart.cpp`) ; options `dataBits`, `parity`,
            `stopBits`, `flowControl` (PC) ou broches `tx` / `rx` / `rts` / `cts` (carte) ;
            `port()` remplace `path()` ; UART0 (liaison de l'agent) refusé (EBUSY). Jumeau :
            `"uarts"` / `"linkUart"` dans le JSON, un lien TCP par UART
            et une section par UART dans la page (journal texte / hexa, envoi texte ou
            hexa), essayé dans le navigateur. `tests/serial.test.js` 26/26, suite Debug 89 OK,
            firmware de la YD compilé (892 Ko) mais PAS flashé. Restent : un essai sur la
            carte (rien de câblé pour l'instant, utilisateur), RP2350 plus tard ;
      - [x] ports TCP du jumeau réglés dans la page (utilisateur, 2026-11-12, FAIT le même
            jour) : port et « listen » par UART (dans son panneau) et par fonction série
            USB (section USB de la vue de la carte, sous les pièces), gardés dans
            `<storage>/twin.json`, rien n'écoute par défaut, port pris = erreur et écoute
            coupée, USB écoute seulement appareil démarré ; `--uart-port` / `--usb-port`
            gardés pour les tests (rien d'enregistré). `host/twin/sim/twin_tcp.*`,
            `tests/twintcp.test.js` (15 + 7). Signalé : `Switch` de x4js n'a ni
            événement `change` ni `getCheck()` (une `Checkbox` bouton à la place) ;
      - [x] page du jumeau en SECTIONS repliables (utilisateur, 2026-11-12, FAIT le même
            jour) : une section par périphérique dans la vue de la carte (chaque pièce,
            USB, chaque UART), résumé de l'état sur la ligne de titre, ouvert / plié gardé
            par le navigateur ; composant à nous `controls/section.ts` (x4js n'en a pas) ;
            plus de panneaux UART ancrés ; lignes USB avec une icône et des textes clairs
            (« unused: the device is not started »...). Essayé dans le navigateur ;
      - [ ] `device/i2c` (demande de l'utilisateur, 2026-11-12) : bus maître, pour un
            switch I2C et les cartes derrière. ÉCRIT le 2026-11-12 pour l'ESP32 et le
            jumeau (`common/hal/hal_i2c.h`, `common/device/i2c_module.cpp`,
            `ports/esp32/main/hal_i2c_esp32.cpp`, `host/twin/sim/twin_i2c.cpp`,
            `tests/i2c.test.js` 25/25 avec `tests/fixtures/twini2c`, suite Debug 88 OK ;
            `ENXIO` ajouté aux noms d'erreurs de jsbind ; firmware de la YD, 891 Ko). FLASHÉ
            et ESSAYÉ sur la YD le 2026-11-12 sans rien de branché (bus 0 sur GPIO8 / 9,
            script `temp/i2cboard`) : sans pull-up, probe ETIMEDOUT et transferts ENXIO au
            bout des 100 ms ; avec `pullUp: true`, probe false en 1 ms, ENXIO en 1 à 2 ms,
            scan de 112 adresses en 21 ms. Reste : un essai avec un vrai composant (aucun
            sous la main). RP2350 plus tard. API convenue le 2026-11-12 (CLAUDE.md) :
            - `new I2c(port, { sda, scl, frequency = 100000, pullUp = false, timeout = 100 })`
              (port = numéro du contrôleur, comme l'UART de `Serial` ; `frequency` et
              `timeout` = valeurs par défaut des appareils) ; `bus.device(address, {
              frequency, timeout })` -> `I2cDevice` (adresse 7 bits, fréquence PAR APPAREIL,
              pas de constructeur public) ; `bus.probe(address)` / `probeSync` (booléen, ne
              lève rien) ; `bus.reset()` (récupération de bus) ; `bus.close()` (ferme aussi
              ses appareils) ;
            - `I2cDevice` : `write(bytes)`, `read(length)`, `writeRead(bytes, length)`
              (repeated start), promesses, ET leurs versions `writeSync` / `readSync` /
              `writeReadSync` ; `close()` ; les transferts se font SEULEMENT sur les
              appareils (pas de `bus.write(address, ...)` : doublon, fréquence ambiguë) ;
            - `bytes` = `Uint8Array` ou tableau de nombres 0..255 ; les lectures rendent un
              `Uint8Array` ;
            - erreurs avec `code` : `ENXIO` (NACK), `ETIMEDOUT`, `EIO` (promesse rejetée,
              ou exception pour les versions Sync) ;
            - transferts d'un bus un par un dans l'ordre des appels (un Sync attend les
              asynchrones déjà en file) ; transaction atomique (switch puis lecture) : au
              programme, par un verrou JS ;
            - hors du natif : helpers de registre (`readWord`...), pilote d'un switch
              PCA9548A, pilotes des cartes (en JS) ; pas de mode esclave ni d'adresse 10 bits
              pour l'instant ;
            - couches comme `device/rmt` (`common/hal/hal_i2c.h`, tâche du hal qui fait les
              transferts bloquants d'ESP-IDF, `common/device/i2c_module.cpp`) ; jumeau : au
              début aucune adresse ne répond (`ENXIO`), composants simulés au backlog ;
      - [ ] **convertir le firmware C de la carte en JS** (demande de l'utilisateur,
            2026-11-12 ; liste de ce qu'il faut porter dans les notes de la carte). Il manque
            encore pour ça un stockage sur la carte et le WiFi.
- [ ] **Vitesse du moteur, pistes algorithmiques** (2026-11-12, mesures sur le PC :
      `bash tools/profile/profile.sh`, test costaud `tools/profile/workload.js`) :
      - [x] contrôles de zone morte des `let` / `const` retirés quand inutiles (modification
        N, `compiler/tdz.c`) : boucle `let` 363 → 246 ms ; test costaud inchangé ;
      - pistes notées par Bellard en « XXX » dans quickjs.c : `for (x of tableau)` sans
        créer d'itérateur, lecture de propriété sans le chemin lent pour les tableaux,
        `loc(a) += expr` en `add_loc`, méthodes de tableau (`fill`, `includes`, `slice`,
        `sort`) spécialisées pour les tableaux rapides ;
      - [x] profil du test costaud (2026-11-12) : boucle de l'interpréteur 28 à 43 %,
        recherche de propriétés 8 à 16 % ;
      - [x] cache des dernières recherches de propriété essayé sur le PC (2026-11-12) :
        jusqu'à 82,6 % trouvées, aucun gain de temps ; code retiré (known-bugs.md « Speed »).
- [ ] **Debugger, étape 2** : le cœur du proxy en JS, sans CDP (lien entre le sidecar et le
      stub, variables visibles par portée, évaluation compilée sur l'hôte). Puis l'étape 3
      (serveur CDP, `qjsx debug`), puis l'étape 4 (points d'arrêt conditionnels, prédiction
      « non rattrapée », `--wait-debugger`, liaison série).
- [ ] `C:\temp` contient des sauvegardes et des scripts des agents (doc-orig, pseudo-*,
      jsbind-orig, doc-trial, final-style, espjs-backup-before-style…), qu'on peut supprimer.

## Backlog

- [x] **Exécutable pour le « device » desktop** (FAIT le 2026-11-12 : `qjsx build
      --executable`, `qjsx-runtime` + le paquet collé à la fin, 1,3 Mo pour la démo ;
      sans compilateur ; tests dans run-tests.sh) (utilisateur, 2026-11-12) : générer un
      exécutable autonome qui embarque un programme (son paquet), le PC jouant le rôle d'un
      device. Conception à proposer (point de départ possible : `qjsx-target`, le moteur sans
      compilateur, plus le paquet embarqué ou collé à la fin de l'exécutable).
- [x] **Jumeaux numériques sur le desktop** (utilisateur, 2026-11-12 ; FAIT le même jour pour
      la YD-ESP32-S3, architecture.md 6.6) : `qjsx agent --board yd-esp32-s3 [--ui-port
      8090]`, carte décrite dans `boards/<nom>.json` (copiée dans `bin/<config>/boards/`),
      `hal::gpio` et `hal::rmt` simulés dans `host/twin/sim/`, seuls les modules natifs de la carte
      offerts au programme, `process.platform` de la carte, page web (broches, bouton BOOT,
      couleur de la LED, forçage 0 / 1 / libre) servie par un runtime JS sur son propre
      thread. Testé en Debug (`tests/twin.test.js`, 17 vérifications) et essayé dans un
      navigateur. Pas d'exécutable : l'outil joue la carte (deploy, debug, monitor comme sur
      la vraie). Reste :
      - [ ] d'autres pièces (potentiomètre, écran...), I2C / SPI / UART simulés ;
      - [ ] **code de simulation sur les ports des jumeaux** (idée de l'utilisateur,
            2026-11-12) : pouvoir écrire du code qui joue ce qui est branché sur les ports
            du jumeau (un composant I2C qui répond à son adresse, un appareil sur un UART,
            l'hôte USB, des stimuli sur des broches...), au lieu des seuls clics de la
            page. COMPOSANTS FAITS le 2026-11-12 (suite Debug 94 OK + twinparts corrigé et
            relancé seul, 21/21 ; architecture.md 6.6) :
            API `qjsx:twin`, modèles button / ws2812 / bme280 / potentiometer, composants du
            projet (`model.js` + `view.js`), section `"twin"` de `qjsx.json`, `-p`,
            `"target": "twin"`, ports, trace I2C, ADC, vues à trois niveaux, section
            Buttons, console. Reste : SSD1306, SPI / UART joués par des modèles, oscilloscope,
            vitesse de la carte, scénarios, signaux. Ce qui suit est l'historique de la
            conception. Première étape (page x4js) FAITE le même jour (suite Debug 88 OK, page
            essayée dans le navigateur intégré, architecture.md 6.6) : tout dans
            `host/twin/` (`sim/`, `js/server.js` = `qjsx:twin/server`, `ui/`) ; x4js installé
            (accord de l'utilisateur) ; page x4js (DockingView carte + broches, `Led` à nous,
            table des broches mise à jour en place) ; messages `twin.board`, `twin.state`,
            `twin.pin.force` sur `/ws` (sous-protocole `messaging` géré par
            `qjsx:websocket`) ; zip embarqué (`tools/embed-twin-ui.lua`, CRC-32 des sources
            dans `dist/sources.crc32`) ; option cachée `--ui-dir`. RESTE : les composants
            (modèle + vue, API `qjsx:twin`, `"twin"` de `qjsx.json`, ids), BOOT et WS2812
            sur ce modèle, BME280 / SSD1306, trace et console, `PixelDisplay`. Pas de
            vérification de types TS (`tsc` absent : `typescript` non installé) :
            - un composant = un **modèle** d'émulation + sa **vue** (une div). Modèles en JS
              (accordé), exécutés dans le runtime du jumeau, jamais dans celui du programme.
              API de `qjsx:twin` (ACCORDÉE, utilisateur 2026-11-12) :
              - un modèle = module qui exporte par défaut une classe dérivée de `Part`,
                `constructor(config, board)` (config = son entrée du câblage, validée par
                lui : une erreur levée est affichée, l'agent refuse de démarrer) ; créé au
                démarrage de l'agent, vit autant que lui ; `onBoardReset()` optionnel ;
              - `Part` : `id`, `label`, `config`, `publish(state)` (-> `twin.part.state`,
                dernier état envoyé aux pages qui arrivent), `onAction(action, args)`
                (<- `twin.part.action`), `log(...)` (console des modèles de la page) ;
                timers standard, `board.now()` ;
              - `board.pin(n)` : `level`, `mode` (ce que le programme a configuré),
                `drive(0 | 1 | null)`, `pull("up" | "down" | null)`, `onChange(fn)`.
                PRIORITÉ sur la ligne (accordée) : sortie push-pull du programme > drain
                ouvert du programme à 0 > forçage de la page > `drive` d'un composant >
                pulls (programme, puis composants) > flottant = 0 ; programme et
                composant en sens contraire = court-circuit signalé dans la trace ;
              - `board.i2c({ sda, scl })` (bus repéré par ses broches, pas par le numéro de
                port) : `attach(address, { onTransfer(written, readLength) })` -> octets ou
                null (NACK), `detach(address)` ; un appel par transfert (écriture puis
                lecture après repeated start ; probe = written vide, readLength 0) ;
                `onTransfer` SYNCHRONE uniquement (accordé : une promesse donne `EIO`),
                timeout du bus -> `ETIMEDOUT` ; aide `Registers` (pointeur auto-incrémenté,
                `set`, `onWrite`) ;
              - `board.rmt(pin)` : `onBytes(fn)`, `onPulses(fn)` (le décodage GRB quitte
                `server.js` pour le modèle `ws2812`) ;
              - plus tard : `board.uart`, `board.spi`, `pin.voltage` (ADC) ;
              - threads : tout le code d'un modèle sur le thread du jumeau ; un modèle
                n'attend jamais le programme ;
              - `boards/*.json` (accordé) : `kind` -> `model`, un `id` par composant
                (`boot` / `button`, `rgb` / `ws2812`) ; le C++ ne connaît plus de bouton
                (le pull-up de BOOT vient du modèle `button`) ;
            - chaque composant a un **`id` unique** (utilisateur) en plus de son `label` :
              minuscules, chiffres, `-`, `_` ; doublon refusé par `qjsx build` ; sert aux
              scénarios, aux messages, à la trace, à la disposition retenue de la page ;
            - la logique électrique reste en C++ (`twin.cpp`) : un modèle qui pilote une
              broche écrit dans l'état partagé, la lecture d'une broche par le programme
              reste locale. Seules les transactions de bus appellent le modèle, en
              synchrone depuis le thread JS du programme (requête postée au runtime du
              jumeau, attente mutex + condition) ; RÈGLE : le runtime du jumeau n'attend
              jamais le thread du programme ;
            - VUES (ACCORDÉ, utilisateur 2026-11-12) : trois niveaux : (a) pas de vue -> la
              page montre l'état publié (nom / valeur, lecture seule) ; (b) contrôles
              déclarés par le modèle (`static controls = [{ name, type: "slider" | "knob"
              | "switch" | "select" | "button" | "value" | "led", min, max, step, unit,
              items }]`), vue construite par la page, un contrôle envoie `twin.part.action
              { id, action: "set", args: { name: value } }` ; (c) vue libre `export function
              createView(x4, part)` -> Component, `part` = `{ id, label, config,
              onState(fn), send(action, args) }` ; TS compilée dans la page pour les
              composants fournis (`host/twin/parts/<modèle>/view.ts`), `view.js` d'un
              projet chargé par `import()`. Chaque composant = un `Panel` DANS le panneau
              Board (utilisateur : il peut y en avoir beaucoup), pas un panneau du
              DockingView ;
            - `qjsx.json` `"twin"` : `board` (nom ou chemin `.json`), `parts` (modèle sans
              chemin = fourni par qjsx, `./dossier` = composant du projet : `model.js` +
              `view.js` optionnel) ; PAS de `remove` (utilisateur : superflu) ; `ports`
              (ACCORDÉ, OPTIONNEL) : `{ agent, ui, usb }`, chaque port pris dans l'ordre
              option de la ligne de commande > `twin.ports` > défaut (5555, 8090, 8091) ;
              À VOIR avec le travail des UART du jumeau (une autre session : un port TCP
              par UART) ; `"target": "twin"` (ACCORDÉ) = le jumeau local pour
              deploy / debug / monitor. Vérifié au démarrage de l'agent (ids uniques carte +
              projet, champs validés par chaque modèle, deux composants qui pilotent la
              même broche, même adresse sur un bus) : erreur affichée, l'agent ne démarre
              pas ; `qjsx build` ne la lit pas (pas dans le build id) ;
            - lancement (ACCORDÉ, utilisateur 2026-11-12) : `qjsx agent` lancé dans le dossier
              d'un projet lit la section `"twin"` de son `qjsx.json` (carte + composants) ;
              `-p <dossier>` / `--project <dossier>` désigne le projet d'ailleurs (qjsx
              installé globalement) ; `--board` reste pour une carte nue hors projet ;
            - câblage (proposé, exemple donné) : `boards/*.json` = la carte nue (BOOT, LED) ;
              ce que l'utilisateur branche dessus va dans une section `"twin"` de
              `qjsx.json` : `board`, `parts` (`id`, `model` = nom d'un composant fourni ou
              chemin d'un dossier du projet, `label`, `i2c: { sda, scl }`, `address` en
              chaîne hexa "0x76", champs propres au modèle validés par lui) ;
            - premiers composants : BOOT et WS2812 réécrits sur ce modèle (valide
              l'interface), puis BME280 et SSD1306 avec `device/i2c` (accordé) ;
            - **page en x4js** (utilisateur) : `DockingView` (au centre la carte et ses
              composants, un `Panel` chacun ; outils : table des broches en `Gridview`,
              scénarios ; en bas : trace des bus et console des modèles ; `persist`).
              Vues : contrôles standard x4js (`Slider`, `Knob`, `Switch`, `Gauge`,
              `Chart`, `Button`) ET vues libres (utilisateur : « les 2 ») ; composants
              custom À NOUS (pas dans x4js, utilisateur) : `Led`, `PixelDisplay` (sur un
              `Canvas`). Vues des composants fournis en TS compilées dans le bundle ; vue
              d'un composant du projet = module JS chargé par `import()`, `export function
              createView(x4, part)` (x4js passé par la coquille : le bundle est un IIFE, et
              `Application` est un singleton strict) ;
            - **messages** : `Application.setupSocketMessaging` de x4 (lu dans
              `src/core/core_application.ts`) : JSON texte `{ msg, params }` dans les deux
              sens, tout `fireGlobal` de la page part au serveur ; sous-protocole
              `messaging` demandé par le client : `host/js/websocket.js` doit renvoyer
              `Sec-WebSocket-Protocol: messaging` (sinon Chrome refuse) ; texte `ping`
              ignoré par le client (keep-alive) ; URL par défaut `/ws` (un `path` doit
              contenir hôte et port) ; pas de reconnexion automatique (`looseCallback` :
              afficher « jumeau déconnecté », retenter). Remplace le protocole `/twin`
              actuel. Messages proposés : `twin.pins`, `twin.pin.force { pin, level }`,
              `twin.part.state { id, state }`, `twin.part.action { id, action, args }`,
              `twin.trace`, `twin.log`, `twin.scenario.*` ; tous les onglets reçoivent tout ;
            - **build** (ACCORDÉ, utilisateur 2026-11-12) : une étape premake lance
              `npm run build` dans `host/twin/ui/` quand Node et `node_modules` sont là et
              que les sources ont changé ; le `dist/` est versionné (une machine sans Node
              compile qjsx avec lui) ; sources changées sans Node = avertissement, pas
              d'échec. « Changées » = un hash des sources écrit dans le `dist/` (pas les
              dates : un clone ou un changement de branche les remet à l'heure du moment),
              dans le `dist/` ; l'interface est embarquée dans l'exécutable comme une archive
              = un ZIP NON COMPRESSÉ (utilisateur ; nom interne sans importance, jamais vu
              par l'utilisateur : un simple `.zip` dans le dossier objet du build) qui
              contient les fichiers du `dist/` (`index.html`, `main.js`, `main.css`,
              `assets/`...), comme un `.qjspkg` : lu en place par le lecteur de zip des
              paquets, chaque entrée servie par le serveur du jumeau avec un type MIME tiré
              de son extension ; une commande / option CACHÉE (utilisateur) sert à la place
              l'interface depuis un dossier, pour développer le jumeau sans relinker qjsx
              (avec `x4js build --debug` : source maps dans les devtools) ;
            - **dossier** (ACCORDÉ, utilisateur 2026-11-12 : tout le jumeau au même endroit,
              dans `host/`) : `host/twin/sim/` (le C++ de l'actuel `host/sim/` : premake,
              globs et `includedirs` à adapter, pas deux headers du même nom),
              `host/twin/ui/` (projet x4js : package.json, x4.config.json, src/ avec la
              coquille et `controls/`, `dist/`), `host/twin/parts/<modèle>/` (`model.js` +
              `view.ts`), `host/twin/js/` (serveur de la page = l'actuel `host/js/twin.js`,
              chargement des modèles) ; `host/js/twinpage.js` disparaît ;
            - **noms** (ACCORDÉ, utilisateur 2026-11-12) : `qjsx:twin` = l'API des modèles,
              PUBLIQUE (documentée, stable : les modèles d'un projet l'importent aussi) ;
              les composants fournis sont embarqués sous `qjsx:twin/parts/<modèle>`, nom
              tiré du DOSSIER (tous les fichiers s'appellent `model.js` : règle
              d'embarquement à adapter) ; un composant du projet (`"model": "./twin/relay"`)
              est chargé depuis le disque par `import()` (permis : JS de l'outil) ;
            - **debug** proposé : trace par composant et par bus (heure, adresse, octets
              écrits / lus, NACK ; changements de niveau) + console des modèles (le runtime
              du jumeau n'écrit pas sur la sortie) dans le panneau du bas ; déboguer les
              modèles dans VS Code : plus tard ;
            - **scénarios de test** : REPORTÉS (utilisateur, 2026-11-12 ; d'abord « maintenant,
              super idée », puis mis en TODO). Proposé : un fichier JS du projet ; une
              commande (nom à choisir) démarre le jumeau, déploie en RAM, lance le
              programme, exécute le scénario avec `board.part(id)`,
              `board.output.waitFor(regex, { timeout })`, `board.waitFor(fn, { timeout })`,
              `press({ ms })`, injection de fautes (`fail("nack")`) ; temps réel du PC. Où ils
              tournent, à trancher : (a) côté qjsx, dans le runtime du jumeau (sans
              navigateur, code de sortie pour la CI, accès direct aux modèles, timing
              précis ; console dans la trace) ; (b) dans la page (devtools, on voit le test,
              mais navigateur obligatoire, Chrome headless en CI, latence du WebSocket,
              onglet pilote) ; (c) la même API `board` des deux côtés (deux
              implémentations, API entièrement asynchrone). Recommandé : (a) avec une API
              asynchrone, pour pouvoir aller vers (c) quand les signaux arriveront dans la
              page ; la page affiche quand même les scénarios (`twin.scenario.*`) ;
            - **signaux** (grandeurs qui évoluent : température du BME280...) : REPORTÉS
              (utilisateur). Idée de l'utilisateur : du code exécuté dans la PAGE WEB (script
              « environnement » du projet chargé par la page, `meteo.set({ temperature })`
              envoyé au modèle qui garde la dernière valeur ; débogable dans les devtools ;
              à voir : une page doit être ouverte, un seul onglet pilote, grandeurs lentes
              seulement). Le mini-langage JSON (`sine`, `ramp`...) est REJETÉ ;
      - [ ] **simuler la vitesse de la carte** (utilisateur, 2026-11-12) : un réglage du
            jumeau (page de réglages) pour que le programme tourne à peu près à la vitesse
            de la vraie carte, pas à celle du PC ; trouver un moyen (ralentir l'interpréteur,
            budget d'instructions par tranche de temps...) ; à concevoir ;
      - [ ] **analyseur logique / oscilloscope sur les broches** (utilisateur, 2026-11-12) :
            les niveaux des broches dans le temps sur la page (plusieurs voies, le temps
            du jumeau), y compris les impulsions `send` du RMT ; à concevoir ;
      - [x] jumeau choisi dans `qjsx.json` (FAIT le 2026-11-12 : section `"twin"`, `qjsx
            agent` dans le dossier ou `-p`) ;
      - [ ] d'autres cartes (PICO2-XXL...) ;
      - [x] la page reconstruit le tableau des broches à chaque changement (RÉGLÉ le
            2026-11-12 : la page x4js construit les lignes une fois et les met à jour).

- [ ] **Éditeur Monaco pour les devices** (utilisateur, 2026-11-12) : à préciser. x4js a un
      composant `MonacoEditor` (`x4js/monaco`).

- [ ] **Manuel utilisateur** (utilisateur, 2026-11-12) : à préciser.

- [x] **Entrées ADC** (utilisateur, 2026-11-12 ; FAIT le même jour, pas encore essayé sur la
      carte) : `device/adc` (`new Adc(pin, { range })`, `read()` brut, `readVoltage()` en volts,
      `bits`, `close()`), ESP32 sur ADC1 seulement (`hal_adc_esp32.cpp`, adc_oneshot +
      calibration ; le firmware compile), jumeau (`"adc"` dans `boards/*.json`, `pin.voltage`
      de `qjsx:twin`, composant `potentiometer`).

- [ ] **Sorties DAC** (utilisateur, 2026-11-12) : REPORTÉ, à faire avec l'oscilloscope du
      jumeau. Nos cartes n'ont pas de DAC (ESP32-S3, RP2350) ; l'ESP32 classique et l'ESP32-S2
      en ont un (8 bits), le STM32H743 aussi (12 bits).

- [x] **Permissions d'un programme du PC** (utilisateur, 2026-11-12 : « éviter que l'exe puisse
      supprimer un disque complet en cas de bug JS » ; FAIT le même jour, architecture.md 6.7) :
      modèle de Deno, rien sans autorisation. Section `desktop` de `qjsx.json` (`rootFolder`,
      `permissions` : read / write / net / env / serial), variables `${executableFolder}`,
      `${userAppDataFolder}`, `${currentFolder}`, `${rootFolder}` ; options de `qjsx run`
      (`-A -R -W -N -E --allow-serial`) ; racine par `--qjsx-root-folder=` ou
      `QJSX_ROOT_FOLDER` ; racine d'un disque refusée ; vrais chemins (liens et jonctions
      résolus). Le device n'a pas de permissions, sauf l'agent desktop et le jumeau (même
      code, sur le disque du PC) : fichiers limités à `<storage>/data`, comme la partition
      d'une carte (`board::confineFiles()`). Testé en Debug (86 vérifications).
      Reste : build des cartes à refaire (nouveau fichier `common/runtime/permissions.cpp` :
      `tools/idf.ps1 reconfigure build`), tests en Release et sous Linux / macOS.

- [ ] **Troisième carte : WeAct Studio STM32H743** (utilisateur, 2026-11-11 ; caractéristiques
      citées de mémoire, à vérifier sur le schéma WeAct) : Cortex-M7 480 MHz, FPU double, 2 Mo
      de flash interne, ~1 Mo de RAM en plusieurs blocs (tas JS dans les 512 Ko d'AXI SRAM, pas
      de SDRAM), flash QSPI 8 Mo (paquets lus en place, mode memory-mapped), microSD (un vrai
      `sys/fs` avec FatFS), USB-C en USB FS (liaison USB CDC ou UART), écran TFT 0,96" ST7735
      160×80 RGB565 en SPI (`device/spi` + pilote JS en bibliothèque, primitives natives pour
      remplir / envoyer un tampon). WS2812 : timer + DMA ou SPI derrière `hal_rmt.h`. Cache du
      M7 et DMA : cohérence à gérer. Pack STM32CubeH7 + FreeRTOS à télécharger (accord).
      Après le RP2350.
- [ ] **microSD des cartes** : la PICO2-XXL (RP2350) en a une aussi (schéma Olimex Rev. C) ;
      même sujet que la H743 : `sys/fs` sur FatFS, et `board::dataDirectory()`.

- [x] **WS2812 sur GPIO48** (choix de l'utilisateur, 2026-11-11, CODÉ et ESSAYÉ sur la carte
      le même jour : couleurs et arc-en-ciel, 667 images en 10 s avec une pause de 10 ms ;
      `setHsv` / `fillHsv` ajoutés, idée de l'utilisateur) : `device/rmt` natif (`send`, `sendBytes`, promesses) et `RgbLed` en JS dans
      `stdlib/rgbled`, le type de LED en option (CLAUDE.md « device/rmt + stdlib/rgbled »).
      Vérifier le pont de soudure « RGB » de la carte. Ensuite : le port RP2350 (pico-sdk et
      FreeRTOS-Kernel à télécharger, avec accord).

- [x] **Version de qjsx visible du JS** (FAIT le 2026-11-12 : `process.versions.qjsx`,
      `.quickjs`, et `.build`, l'empreinte du paquet qui tourne ; demande de l'utilisateur,
      2026-11-11 : « je ne sais pas encore
      comment ») : le firmware la connaît (`QJSX_VERSION`, envoyée dans `HELLO`). Pistes :
      `process.versions.qjsx` / `process.versions.quickjs` (comme `process.versions.node`),
      ou `process.env` (mais `env` vient de `qjsx.json` sur un device et du système avec
      `qjsx run` : une valeur du firmware s'y mélangerait).

- [ ] **Documentation détaillée de jsbind + guide d'utilisation** (demande de
      l'utilisateur, 2026-11-11) : un document en anglais dans `doc/` (`jsbind.md`) :
      principes (pas de std::, pas d'exceptions, « first error wins »), chaque macro
      (`JS_CLASS`, `JS_EXPORT`, `JS_MODULE`, `JS_FUNCTION`, `JS_STRUCT`, `JS_ENUM`), les
      types (`js::Str`, `js::Bytes`, `js::Buffer`, `js::Function`, `js::Value`,
      `js::Range`, `js::Optional`), les erreurs (`js::fail`, `failSystem`), les promesses
      (`js::Promise`, `Resolver`), `init()`, `keepAlive`, la durée de vie des objets, et un
      guide pas à pas pour écrire un module natif (exemple complet : `device/gpio`).

- [ ] **CI/CD : créer la release dans GitHub** (demande de l'utilisateur, 2026-11-11) : à
      concevoir avant de coder : GitHub Actions déclenchées par un tag ; construire `qjsx`
      (Windows, et Linux / macOS une fois compilés), lancer `tests/run-tests.sh`,
      construire le firmware ESP32 par carte (image ESP-IDF officielle) ; attacher à la
      release les exécutables, `stdlib.qjslib` et les `qjsx.bin` (avec l'adresse de
      flashage). Questions : déclencheur (tag `v*` ?), plateformes, signature, numéro de
      version (`QJSX_VERSION`).

- [ ] **Accepter TypeScript** (demande de l'utilisateur, 2026-11-11) : des sources `.ts` dans
      un projet qjsx. À concevoir avant de coder : qui retire les types (un transpileur
      embarqué dans l'outil, en JS ou en C, ou un outil externe), ce que deviennent les
      positions du debugger (les annexes de debug pointent dans le JS produit : il faudrait
      des source maps, ou garder les lignes et colonnes du `.ts`), les `.d.ts` des modules
      natifs et des bibliothèques, et la vérification des types (aucune, ou `tsc` à part).
      Aucun téléchargement sans accord.
- [ ] Analyse des optimisations possibles du moteur QuickJS (plus tard)
      → point de départ : les écarts de microbench entre original et découpé
        (doc/quickjs-split-perf.md, de -48 % à +27 % selon les tests)
      → moteur modifié pour le debugger : -2,6 % au total contre l'original ; gains et pertes
        par test, et pistes, dans doc/quickjs-split-perf.md (section « Moteur modifié pour le
        debugger »)
- [ ] Retravailler les tests du microbench plus lents que l'original (moteur modifié pour le
      debugger, mesure du 2026-11-10, ns par opération, original → qjsx). Tous étaient déjà
      lents avec le découpage seul : c'est le découpage, pas le debugger.
      - `prop_clone` 42.97 → 51.40 (+19.6 %) : copie d'objet `{ ...ref }` (10 propriétés nommées
        + 10 indices)
      - `prop_update` 9.64 → 11.00 (+14.1 %) : `obj.a += j` (lecture + écriture de propriété)
      - `string_to_int` 56.51 → 64.19 (+13.6 %) : `s | 0` (conversion chaîne → entier)
      - `array_hole_length_decr` 42.51 → 47.19 (+11.0 %) : tableau à trous, `length` diminuée
      - `regexp_utf16` 152.51 → 168.01 (+10.2 %) : `/…/.exec(s)` sur une chaîne UTF-16
      - `prop_delete` 78.97 → 86.83 (+10.0 %) : copie `{ ...ref }` puis `delete`
      - `regexp_ascii` 148.84 → 157.06 (+5.5 %) : `/…/.exec(s)` sur une chaîne ASCII
      - `date_parse` 431.40 → 453.02 (+5.0 %) : `Date.parse` de dates formatées
      - `float_arith` 13.88 → 14.37 (+3.5 %) : arithmétique flottante
      - `prop_create` 34.22 → 35.40 (+3.4 %) : `obj = {}` puis 4 propriétés ajoutées
      (sous 3 % : string_build*, float_toString, map_*, sort_bench, regexp_replace : bruit
      probable.) Tableau complet : doc/quickjs-split-perf.md.
- [ ] Est-ce que l'ordre des codes d'opcode a une importance ? (numérotation dans
      quickjs-opcode.h, donc ordre de la table de dispatch et des cas dans JS_CallInternal ;
      l'ajout de OP_debugger avant nop a décalé tous les opcodes courts)
- [ ] Pouvoir uploader des sources JS (fichiers, dossiers) vers la cible, via le stub du debugger ou ailleurs
      → fait côté hôte et agent desktop : `qjsx deploy` (paquet .qjspkg) ; reste le stockage en flash sur l'ESP
- [ ] Faciliter l'écriture d'objets C visibles en JS (aujourd'hui fastidieux)
      → en cours : bibliothèque de liaison C++ common/jsbind (conception dans CLAUDE.md)
- [ ] Décider ce qu'on retire de la bibliothèque standard de qjs (quickjs-libc : fichiers, etc.)
      → décidé : quickjs-libc remplacée par nos modules (fs, path, process, net, serial, crypto, compiler), voir CLAUDE.md
- [ ] **Points d'arrêt conditionnels, logpoints et compteurs de passages** (reportés par
      l'utilisateur le 2026-11-11, quand on a repris l'étape 4 du debugger sans eux ;
      descendus en fin de TODO le 2026-11-12) :
      évalués sur le device, sans aller-retour par passage (conception 3.6). Aujourd'hui
      le serveur CDP les refuse et VS Code les montre non résolus.
