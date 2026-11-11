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
- [ ] **Port RP2350** (Olimex PICO2-XXL, PSRAM sur GPIO8) après l'ESP32-S3 : chaîne pico-sdk et
      FreeRTOS-Kernel à confirmer avec l'utilisateur (pas de téléchargement sans accord).
- [ ] **Debugger, étape 2** : le cœur du proxy en JS, sans CDP (lien entre le sidecar et le
      stub, variables visibles par portée, évaluation compilée sur l'hôte). Puis l'étape 3
      (serveur CDP, `qjsx debug`), puis l'étape 4 (points d'arrêt conditionnels, prédiction
      « non rattrapée », `--wait-debugger`, liaison série).
- [ ] `C:\temp` contient des sauvegardes et des scripts des agents (doc-orig, pseudo-*,
      jsbind-orig, doc-trial, final-style, espjs-backup-before-style…), qu'on peut supprimer.

## Backlog

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

- [ ] **Version de qjsx visible du JS** (utilisateur, 2026-11-11 : « je ne sais pas encore
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

- [ ] **Points d'arrêt conditionnels, logpoints et compteurs de passages** (reportés par
      l'utilisateur le 2026-11-11, quand on a repris l'étape 4 du debugger sans eux) :
      évalués sur le device, sans aller-retour par passage (conception 3.6). Aujourd'hui
      le serveur CDP les refuse et VS Code les montre non résolus.

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
