# TODO

## Où on en est (2026-11-13, à relire en reprenant le travail)

- [x] **Dernier état (2026-11-13 après-midi ; tout le matin est commité dans 3d95c7d ;
      l'utilisateur commite)** :
      - **tas JS d'une board sans PSRAM** (2026-11-16, architecture.md « JS heap of a board
        without PSRAM », engine-changes.md Q) : région de RAM interne réservée au premier
        runtime (plus grand bloc libre moins 16 Ko), gérée par `RegionHeap` ; pages de
        QuickJS de 1 Ko pour les blocs jusqu'à 128 o, réserve de 4 pages vides
        (`QJSX_SMALL_HEAP_DEFINES`, donnés par le port ESP32 sans PSRAM) ; `device info`
        affiche la taille de la région. Sur PC : suite Debug 109 OK avec ces réglages et
        `QJSX_JS_HEAP_MB=64`, contexte vide 133 → 72 Ko, microbench +3,9 % (grandes
        chaînes, malloc de Windows dessous). ESSAYÉ sur la carte cliente le 2026-11-16 :
        région de 232 Ko, application 255,8 → 205,8 Ko de tas JS, firmware 46,7 Ko libres
        (23,7 avant), programme vide 62,0 Ko ; un programme qui dépasse la région reçoit
        une exception et l'agent répond toujours. `-DQJSX_JS_HEAP_SHARED=ON` = ancien tas
        partagé (pour comparer). VITESSE sur la carte (temp/allocbench, contre l'ancien
        tas) : objets −9 %, tableaux =, JSON +8 %, propriétés +27 %, chaînes +62 %,
        fonctions créées en boucle +85 % ; cause NON trouvée (pas de ralentissement des
        mêmes boucles sur PC ; page créée en temps constant et marge au
        redimensionnement : sans effet). Piste : le cache d'instructions du firmware
        fait 16 Ko pour un `JS_CallInternal` de 25 Ko (comme sur la RP2350), la place du
        code compte peut-être plus que l'allocateur : essayer 32 Ko (coûte de la RAM) et
        la flash en QIO. Reste aussi : régler la réserve (paquet en RAM de 44 Ko
        impossible : plus grand bloc libre du firmware 31 Ko), message d'un manque de
        mémoire (« Uncaught (in promise) null »), RP2350 sans PSRAM, manuel JS.
      - **pile de la tâche JS réglable par board** (2026-11-16, choix de l'utilisateur :
        32 Ko pour la carte cliente) : `JS_TASK_STACK` dans le `board_config.h` de chaque
        board ESP32 (YD : 48 Ko), limite du moteur = pile − 8 Ko (`JS_NATIVE_STACK_MARGIN`,
        board_esp32.h). Mesuré sur la carte avec 48 Ko : 112 appels JS imbriqués (366 o par
        appel), 480 niveaux pour JSON.parse, l'application n'utilise que 8 Ko. FLASHÉ et
        mesuré avec 32 Ko : 66 appels imbriqués, 270 niveaux pour JSON.parse, région JS
        232 → 248 Ko, l'application tourne (156 Ko de tas JS, 92 Ko de marge).
      - **carte mémoire de la carte cliente** (2026-11-16, option de mesure
        `-DQJSX_MEMORY_MAP=ON`, globale `__memoryMap(titre)` ; relevé brut dans
        temp/histo/map-clean.txt) : trois poches de RAM trouvées, rien de corrigé :
        1. pile de la tâche JS 48 Ko dont 41,3 Ko jamais utilisés ; 2. un tas de 32 Ko
        (0x3fcf0000) et le tas RTC de 7,8 Ko entièrement libres, que la région JS (un seul
        bloc du tas principal) ne peut pas prendre ; 3. pic du tas JS au chargement :
        211 Ko, pour 166 Ko une fois les modules chargés (45 Ko transitoires, cause à
        trouver). Statique : 72,6 Ko de RAM (code en RAM 43,5 Ko, .data 16,5, .bss 12,6).
      - **optimiseur de bytecode : constantes, FAIT le 2026-11-16** (engine-changes.md R,
        `compiler/fold.c`) : marche 1 (calcul des expressions de littéraux) et marche 2
        (`const` de module remplacées par leur valeur, sans variable si non exportées),
        valeurs dans le fichier de débogage (`constants`), montrées par le débogueur.
        Sur l'application cliente : 86 variables de module en moins, tas JS sur la carte
        206,0 → 195,6 Ko, paquet 44,2 → 41,7 Ko. Tests : fold 82, modconst 21. Reste :
        constantes exportées entre modules (3 dans l'application), booléens, opcodes
        fusionnés (après mesure sur la carte). Proposition d'origine : marche 1, calcul des expressions constantes à la compilation
        (nombres, concaténation de chaînes littérales : rien n'est calculé aujourd'hui,
        sauf le moins unaire) ; marche 2, `const` de module à valeur littérale remplacées
        par leur valeur (89 sur 141 variables de module dans l'application cliente, 123
        lectures, ≈ 8 Ko de RAM), la valeur allant dans le fichier de débogage ; acceptés :
        lire une constante avant sa déclaration ne lève plus d'erreur, un point d'arrêt
        sur la ligne glisse à l'instruction suivante. Opcodes fusionnés : plus tard,
        après mesure sur la carte. Sondes : temp/heapmap/optprobe.c, ophisto.c.
      - **tâches préemptées (« faux threads »)** (idée de l'utilisateur, 2026-11-16, à
        étudier, rien de fait) : une tâche = un générateur ; au point de contrôle que
        l'interpréteur a déjà (saut en arrière, appel : interpreter.c:81, celui du
        débogueur et de yieldIfDue), si la tranche de temps est écoulée et que le cadre en
        cours est celui d'un générateur, sortir avec un nouveau code de retour
        « préempté » (à côté de FUNC_RET_YIELD, interpreter.c:2760) ; reprise sans valeur
        poussée ni attendue (generators.c:719) ; ordonnanceur en JS dans stdlib, tranche
        en millisecondes. Pas d'opcode, pas de changement de version du bytecode. Limite :
        pas de coupure pendant une fonction ordinaire ou native appelée par la tâche. À
        lire avant de proposer : la reprise des générateurs, comment savoir que le cadre
        en cours est suspendable. Écartés : threads à la Python (une pile C par thread,
        fin de la garantie « un callback n'est pas interrompu »), interpréteur sans pile
        (réécriture du cœur), workers (≈ 62 Ko par moteur).
      - **optimiseur de bytecode** (utilisateur, 2026-11-16 : « pour la suite, réfléchir
        à un optimiseur de bytecode ») : à étudier, rien de fait.
      - **paquet exécuté en place, marches 1 et 2 FAITES le 2026-11-16** (engine-changes.md
        S) : `qjsx build` lie les modules (`link()` de sys/compiler) : une table de noms
        pour tout le paquet (entrée `names.bin`, alignée sur 8, `names` dans le manifeste),
        bytecode écrit avec les numéros définitifs ; le runtime donne les noms au moteur
        avant de créer le contexte (`JS_SetPackageAtoms`, program.cpp) et lit le bytecode
        en place. BC_VERSION 0x46. MESURÉ sur la carte cliente : tas JS 195,6 → 156,3 Ko
        (paquet envoyé 41,7 → 55,9 Ko, en flash) ; l'application redémarre et tourne.
        DÉBOGUEUR sur la carte cliente (sans PSRAM, messages de 8 Ko), 2026-11-16 :
        debugger 72/72, proxy 33/33, cdp 53/53. VITESSE sur la carte (temp/allocbench) :
        tout est identique au firmware d'avant le paquet en place, à la ms près (boucle de
        calcul, tableaux, chaînes, petits objets), SAUF la création de fonctions en boucle,
        deux fois plus lente (532 → 1146 ms pour 20 000) ; cause non prouvée, le paquet du
        banc était en RAM (la flash n'y est pour rien). Piste inchangée : cache
        d'instructions de 16 Ko, la place du code change d'un firmware à l'autre.
        Reste : la marche 3 (tables de variables en place),
        l'empreinte des noms vérifiée dès l'upload, la RP2350. Proposition d'origine
        (retenue par l'utilisateur le 2026-11-16, après l'allocateur) : sur les 142 Ko que le programme de la carte ajoute, ~76 Ko sont
        chargés avant toute exécution (216 fonctions : structures 18 Ko, bytecode 19 Ko,
        tables de variables 12 Ko ; 593 noms ≈ 20 Ko). Trois marches : 1. noms du paquet
        en flash, numéros fixes (moteur, paquet, reste) ; 2. bytecode en place
        (`JS_READ_OBJ_ROM_DATA`, les numéros de noms n'étant plus corrigés) ; 3. tables
        de variables en place. ≈ −51 Ko. Coûts : format du paquet (alignement 8),
        empreinte de la table des noms du moteur dans le manifeste, BC_VERSION, même
        disposition de JSString sur PC (20 o sous Windows contre 12). Pistes plus
        petites : fonctions natives de jsbind paresseuses (−7 Ko), `length` / `name` des
        fonctions calculés (−5 à −7 Ko, risqué), lien de liste du ramasse-miettes (−12 Ko,
        lourd). Sondes : temp/heapmap/appstatic.c.
      - **noms des objets intégrés en mémoire morte** (FAIT le 2026-11-13 soir, ESSAI sur PC
        seulement, doc/engine-changes.md P) : `QJSX_ROM_ATOMS` (quickjs.h, 1 ; 0 = QuickJS
        d'origine ; copie des fichiers d'avant dans temp/rom-atoms-backup/), compteur
        immortel, `core/rom-atoms.{h,c}` générés par `tools/rom-atoms/generate.sh`.
        Contexte vide −22,5 Ko sur PC, programme vide sur l'agent 175,0 → 155,6 Ko ;
        microbench +2,7 %. Suite Debug 109 OK, tests de Bellard OK. GARDÉ par
        l'utilisateur après lecture du code. Reste : essai sur la carte sans PSRAM, puis les paquets de 4 Ko de
        l'allocateur de QuickJS (52 % vides sur un contexte vide, −64 Ko sur PC sans eux),
        noms des globales et modules de qjsx, vérification automatique des fichiers générés.
      - **taille des messages par board et mesure mémoire** (FAIT le 2026-11-13 soir,
        architecture.md « Robustness » et « Memory of a device ») : chaque board choisit le
        plus grand message de l'agent (`board::agentMaxMessage` : 64 Ko avec PSRAM, 8 Ko
        sans) ; tampon de réception au plus juste (8,2 Ko au lieu de 128 Ko) ; l'outil
        refuse une requête trop grosse (EMSGSIZE) et découpe `SET_STOPS` par fonctions
        entières ; l'agent coupe les journaux (UTF-8 gardé), change une réponse trop grosse
        en erreur, borne GET_PROPERTIES et les chaînes ; `qjsx agent --max-message`
        (caché) ; `qjsx device info` donne le tas JS (actuel, maximum de l'exécution) et la
        RAM libre de la carte ; allocateurs comptés (ESP32 PSRAM ou RAM interne, PC) ;
        `tests/maxmessage.test.js` (12) + cdp à 4 Ko dans la suite. Jumeau de `app/` : tas
        JS 380 Ko, 400 Ko au plus (chiffre du PC, 64 bits). RESTE : flasher la board client
        et mesurer ; RP2350 (`memoryStatus`, tas sans PSRAM non mesuré) à sa passe ;
      - [ ] **débuts d'instruction à la demande, par module** (accord de l'utilisateur,
            2026-11-13) : la carte les demande quand un pas ou une pause tombe dans un module
            qui ne les a pas (événement + attente comme SCRIPT_LOADED ; drapeau par fonction
            dans le moteur, testé à chaque instruction en pas à pas) ; à décider après la
            mesure sur la board client (RAM des tables : 1 bit par octet de bytecode) ;
      - [ ] **FILE_LIST paginé** : un dossier de beaucoup de fichiers dépasse un message de
            8 Ko (la carte répond alors « answer too large ») ;
      - [ ] **tas JS dans la page du jumeau** (idée de l'utilisateur, 2026-11-13) : aujourd'hui
            seulement `qjsx device info` ; peut-être une taille de message dans le JSON de
            la board (le jumeau d'une board sans PSRAM à 8 Ko) ;
      - **depuis 3d95c7d** (non commité) : `host/twin/ui/dist/sources.crc32` remis (perdu
        quand la page avait été reconstruite à la main par `npm run build`, qui vide
        `dist/` ; le build Debug l'a réécrit) ; `intr_type` (obsolète dans ESP-IDF 6.1)
        retiré de `hal_pwm_esp32.cpp` ; `tools/idf.ps1` met `PYTHONUTF8=1` (plus de
        message « not configured to handle Unicode ») ;
      - **firmware ESP32 avec `device/pwm`** : COMPILÉ (949 Ko ; aucun avertissement masqué :
        restent ceux d'ESP-IDF et le `fallthrough` de json.c, plus bas) et FLASHÉ sur la YD le 2026-11-13, par COM14 ce jour-là = l'USB NATIF de la YD (le port COM
        CH343 est COM11, branché ensuite) ; PWM
        VÉRIFIÉE à l'oscilloscope par l'utilisateur le 2026-11-13 ;
      - **manuel du développeur JS et `.d.ts`** (FAIT le 2026-11-13, voir le Backlog) :
        `.d.ts` à côté des modules, `qjsx build` les écrit dans `.qjsx/types/`, site
        `doc/jsmanual/` (`npm run dev` : http://localhost:4321/) ; suites Debug ET
        Release 102 OK ; RIEN de commité ;
      - **hasard** (FAIT le 2026-11-13, CLAUDE.md « Random numbers », architecture.md
        5.4, moteur O) : `crypto.getRandomValues` / `randomUUID`, `randomBytes` de
        `sys/crypto`, `Math.random` amorcé par la source, `"twin.randomSeed"` ;
        `tests/random.test.js` ; firmware ESP32 FLASHÉ et ESSAYÉ sur la YD (COM11) : valeurs
        différentes à chaque exécution et après chaque redémarrage de la puce ; RP2350 PAS fait
        (utilisateur : une passe plus tard ; il ne se lie plus sans
        `platform::randomBytes` : `pico_rand`) ; tests d'origine de QuickJS et
        microbench pas refaits (fonction ajoutée, rien d'autre ne change) ;
      - **`device/board`** (FAIT le 2026-11-13, CLAUDE.md) : `id()`, `mac(kind)`,
        `restart()` (vide les sorties, 500 ms au plus, puis redémarre ; agent de bureau
        et jumeau : comme RESET) ; `tests/board.test.js` (15) ; firmware ESP32 FLASHÉ et ESSAYÉ sur la YD (COM11) :
        id = la MAC lue par esptool, dernière ligne avant restart() reçue, redémarrage OK ;
        RP2350 pas fait ;
      - **stockage persistant** (FAIT le 2026-11-13, CLAUDE.md « Persistent storage on a
        device ») : `qjsx device ls | read | write | rm | mkdir | mv`, `deploy --data` et
        `"data"` de `qjsx.json`, place de `/data` dans `device info` ; ESP32 : LittleFS
        1.23.1 sur `/data`, `fs` confiné ; `tests/datafiles.test.js` (34) ; suite Debug
        105 OK ; firmware FLASHÉ et ESSAYÉ sur la YD (COM11) : /data de
        7,9 Mo, fichiers gardés après un reset, deploy --data relu par le programme ; RP2350 pas fait ;
      - **site du manuel** : `npm audit` signale 4 vulnérabilités « moderate » (minimatch,
        GHSA-g3hv-75wg-m249, tirée par TypeDoc ; outil de construction du site seulement) ;
        le correctif de npm rétrograde TypeDoc (casse le site) : à trancher (attendre une
        version corrigée, ou un « override » de minimatch) ;
      - **« minimal build » d'ESP-IDF** (utilisateur, FAIT le 2026-11-13) :
        `idf_build_set_property(MINIMAL_BUILD ON)` dans `ports/esp32/CMakeLists.txt`, seuls
        `main` et ses REQUIRES (77 composants au lieu d'environ 150) : plus les
        avertissements CMake esp_wifi / wpa_supplicant d'ESP-IDF (ils reviendront avec le
        WiFi), firmware 943 Ko ; compilé (`fullclean build`), PAS flashé. Reste un
        avertissement `fallthrough` dans `engine/qjsx/builtins/json.c:1205` (code de
        Bellard, pas touché) ;
      - **tests** : suites Debug ET Release 98 OK le 2026-11-13 au matin ; Debug 99 OK
        après PWM, le RMT et les modes de l'analyseur (`tests/pwm.test.js` 30/30) ;
        Release 99 OK l'après-midi (exécutables reconstruits) ; types de la
        page vérifiés (`tsc --noEmit -p host/twin/ui`, TypeScript 7.0.2 en global, chemin
        complet dans la mémoire de Claude ; le build x4js ne vérifie pas les types) ;
      - **`device/pwm`** (FAIT le 2026-11-13, CLAUDE.md, architecture.md 5.4) : module,
        hal, ESP32 sur LEDC, jumeau avec les limites du S3 (`"pwm"` du JSON de la carte),
        colonne « pwm 1 kHz 25 % » du tableau des broches ; firmware compilé et flashé
        l'après-midi du 13, vérifié à l'oscilloscope par l'utilisateur. RESTE : `pin.pwm`
        pour les modèles des pièces,
        le fondu et `stdlib/servo` (plus tard), le RP2350 ;
      - **analyseur** (FAIT le 2026-11-13, architecture.md 6.6) : PWM et RMT dessinés à
        partir de leurs changements / émissions (fronts calculés, à tout zoom, bande avec
        texte quand c'est trop rapide), VCD en ns plafonné à 1 000 000 de changements ;
        modes Roll / Normal / Single, déclenchement sur un front d'une broche affichée,
        à 10 %, trait pointillé, holdoff d'une fenêtre ; barre sur deux lignes (fonctions,
        broches) ; Save / Clear dans un menu contextuel des traces ; colonnes du tableau
        des broches retaillables (`Header` x4js, « part » flexible). Démos : `temp/pwmdemo`
        (4 PWM + LED RGB qui tourne, déployée sur l'agent de `temp/togglestress`, page sur
        http://127.0.0.1:45711/) ; `temp/togglestress` (boucle infinie de `toggle()` :
        l'interface ne sature pas, `twin.state` renvoyé à chaque tour = 127 Ko/s, à
        améliorer si l'utilisateur le veut) ;
      - tooltip de Run corrigé (utilisateur, 2026-11-13 : Maj + molette ne fait rien
        pendant Run, seulement sur une vue figée) ;
      - **build** : configuration `Profile` (= Release + infos de débogage, pour
        `tools/profile/profile.sh`) ; Release sans infos de débogage et strippé (qjsx
        10,9 → 2,5 Mo) ;
      - **jumeau, UART branchées sur de vrais ports série du PC** (détail dans « Jumeaux
        numériques » plus bas) : `host/twin/sim/twin_uart_serial.cpp`, `twin_choices.cpp`
        (commun pour `twin.json`), `--uart-serial 1=COM5`, `"twin.uarts"` de `qjsx.json`,
        `tests/twinwire.test.js` + 2 vérifications dans run-tests.sh. PAS essayé avec un
        vrai port (aucun sur le PC) : à essayer avec un adaptateur USB-série, TX relié à RX ;
      - **page du jumeau** (`host/twin/ui`) :
        - UART et I2C : icône réglages (sans bordure ni fond) dans la barre de titre de la
          section ; menu avec les sous-menus « TCP » et « PC port » (champs dans les menus),
          « Show in hexadecimal », « Clear the traffic » ; I2C : « Clear the transfers » ;
          un refus s'affiche en Notification (`notify.ts`) ;
        - sections déplaçables par leur barre de titre (dragManager de x4js), ordre gardé
          par le navigateur ;
        - analyseur (`scopeview.ts`) : barre en bas, Run et Record en boutons à bascule
          remplis ; Record n'efface pas l'affichage, son arrêt arrête Run et fige la vue sur
          l'enregistrement ; Save → VCD (`vcd.ts`) ; états compacts en tableaux typés
          (`scopetrace.ts`) ; voies défilantes (molette), axe des temps fixe, Ctrl + molette
          = zoom, Maj + molette = déplacement dans le temps (vue figée seulement, rien
          pendant Run) ; échelle analogique (lignes vref / 0 V, police 10px) ; vue figée :
          la grille et les temps suivent le déplacement, 0 = début de l'enregistrement
          (comme le VCD), sinon l'instant de l'arrêt (utilisateur, 2026-11-13) ;
        - panneau des broches : défilement horizontal quand il est étroit ; crédits x4js
          dans README.md ;
        - journaux (trace I2C, trafic UART, console des pièces) : `controls/logview.ts`,
          seul le journal défile (`scrollIntoView` faisait descendre toute la vue de la
          carte à chaque ligne) ; il suit sa dernière ligne tant que l'utilisateur le
          laisse en bas, remonté il reste où il est ;
        - menu du bouton d'une broche de l'analyseur : sous-menu « Color », sous-menu
          « Switch pin » (une autre broche à sa place, même couleur ; celles que la liste
          « add a pin » propose), puis « Remove » ;
      - **démo de l'analyseur** : `temp/scopedemo` (`qjsx agent -p temp/scopedemo`, page sur
        http://127.0.0.1:45701/) ; autre projet d'essai : `tests/fixtures/twinparts` (page
        sur http://127.0.0.1:45695/) ;
      - **en attente des décisions de l'utilisateur** : appliquer le menu réglages aux
        lignes TCP des fonctions USB ;
      - **défauts x4js relevés (pour l'utilisateur, x4js est en lecture seule pour nous)** :
        `.x4canvas` sans `position: relative` et `CanvasProps` non documenté ; un sous-menu
        qui ne tient pas à droite ne s'ouvre pas à gauche (`Popup.displayAt`) ; la bande du
        `DockingView` ne rétrécit pas avec la fenêtre ; pas d'outil de téléchargement (à
        mettre à côté de `MessageBox`) ; la copie de glisser du dragManager déborde
        (`position: fixed`) ; À VÉRIFIER : `dispatchEvent` (`core_dom.ts`) semble
        distribuer deux fois un événement qui remonte quand un ancêtre a un gestionnaire
        du même type (seul `preventDefault` l'évite) ;
      - **à essayer sur la carte** : `device/adc` (un potentiomètre sur GPIO1-10),
        `device/i2c` avec un vrai composant, `device/serial` sur un UART ;
      - **prochaines pistes** (détail plus bas) : SSD1306 dans le jumeau, vitesse de la carte
        simulée, DAC (avec l'analyseur), mode Auto et décodeurs de l'analyseur, scénarios
        et signaux, le jumeau dans VS Code, manuel utilisateur, éditeur Monaco pour les
        devices, composant « flash », assistant de conception d'une carte.

- [x] **État précédent (2026-11-12, commit e4fa348)** : jumeau numérique (architecture.md
      6.6) : tout dans `host/twin/`, page x4js embarquée, composants = modèle (`qjsx:twin`,
      API publique) + vue, modèles button, ws2812, bme280, potentiometer, composants du
      projet (`model.js`, `view.js`), section `"twin"` de `qjsx.json`, `"target": "twin"`,
      trace I2C, UART et USB par TCP, analyseur des broches ; `device/adc` (ESP32 ADC1 et
      jumeau, firmware compilé, pas essayé sur la carte).

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
      et le debugger. PROPOSITION du 2026-11-13 (pas encore validée) :
      - Web Workers (API du web, comme Deno : `new Worker("./x.js", { type: "module" })`,
        `postMessage`, `onmessage`, `terminate()`, `self` dans le worker) ; un runtime
        QuickJS par worker, sur sa propre tâche (QuickJS : un runtime = un thread) ;
      - messages copiés par la sérialisation de QuickJS (`JS_WriteObject` /
        `JS_ReadObject`), déposés dans la boîte aux lettres de la boucle de l'autre thread
        (le mécanisme existe) ; plus tard `SharedArrayBuffer` + `Atomics` ;
      - le module du worker est un module du programme (déjà dans le paquet ; `import()`
        reste interdit) ;
      - mémoire sur une carte : un runtime + une pile de tâche (16 à 32 Ko de RAM interne ?)
        par worker ; limite de workers, pile et cœur (ESP32 : cœur 0) dans la description
        de la carte, refus clair au-delà ; cher sans PSRAM (carte client) ;
      - modules matériels (gpio, i2c...) réservés au thread principal en première version ;
      - debugger : seul le thread principal en première version (js-debug suit les workers
        de Node par `NodeWorker`, à faire plus tard) ;
      - `process.exit()` et la fin du programme arrêtent les workers ; une exception non
        rattrapée d'un worker = événement `error` côté principal.
      Questions posées : les cas d'usage (calcul long, isolement, second cœur ?), API web ou
      `worker_threads` de Node, modules matériels au principal seulement, debug des workers,
      limites par carte.
      À NOTER (utilisateur, 2026-11-13) : le firmware C de la carte client avait des tâches
      FreeRTOS (LED d'état, bandeau, USB, boucle principale) ; l'agent de conversion les a
      remplacées sur le thread JS unique par `Repeater` (un pas répété à période fixe, qui ne
      se chevauche jamais) et `JobQueue` (des travaux l'un après l'autre), dans
      `app/src/tasks.js` de la carte. Ça suffit tant que ces tâches attendent des E/S ou des
      délais ; seul du calcul long justifierait des workers. Piste : mettre `sleep`,
      `Repeater` et `JobQueue` dans la stdlib (avec la lecture série avec délai de
      `serialreader.js`).
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
- [ ] **Carte client ESP32-S3** (utilisateur, 2026-11-12) : module ESP32-S3-MINI-1U-N8
      (lu par esptool le 2026-11-13) : 8 Mo de flash intégrée (son firmware C n'en
      configure que 4), PAS de PSRAM (tas JS en RAM interne, chemin jamais essayé : à
      mesurer), deux ports USB comme
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
      - [x] la board (FAIT le 2026-11-13, dossier et JSON ignorés par git) : firmware
            compilé (968 Ko), PAS flashé ; `sdkconfig.board` (8 Mo, `partitions/8MB.csv`, sans PSRAM),
            `board_config.h` et le JSON du
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
            2026-11-12 ; liste de ce qu'il faut porter dans les notes de la carte). Analyse
            du 2026-11-13 (firmware C relu en entier, détail dans les notes de la carte) :
            environ 80 % se fait avec les modules actuels (i2c, serial, rgbled, gpio, usb,
            crypto). MANQUE, bloquant, à concevoir avec l'utilisateur :
            - un stockage persistant (quelques chaînes : uid, plan des LED, réglages).
              DÉCIDÉ (utilisateur, 2026-11-13) : `sys/fs` sur LittleFS dans la partition
              `data`, montée sur `/data` (= `board::dataDirectory()`, le module `fs` ne
              change pas : il passe par fopen / stat) ; composant `joltwallet/littlefs`
              (version fixée dans `idf_component.yml`) ; partition formatée automatiquement
              si le montage échoue ; PAS de reprise des réglages de la NVS du firmware C
              (l'uid se réenregistre à la première authentification) ; `fs` CONFINÉ à
              `/data` sur les cartes (`confineFiles()` vrai, comme l'agent de bureau et le
              jumeau : la VFS de l'ESP32 contient aussi `/dev/uart/0`) ; commandes `qjsx
              device ls | read | write | rm | mkdir | mv` et dossier de données du projet
              (`"data"` de `qjsx.json`, `qjsx deploy --data`) : voir CLAUDE.md « Persistent
              storage on a device ». Carte SD plus tard : FatFS sur `/sd` (pas LittleFS) ;
            - l'adresse MAC de la carte, lue du JS : DÉCIDÉ (2026-11-13), module
              `device/board` (`id()`, `mac(kind)`, `restart()`, CLAUDE.md) ; jumeau et
              agent de bureau : identifiant gardé dans `<storage>`, tiré au hasard s'il
              manque ;
            - un hasard cryptographique : `crypto.getRandomValues` (standard du web,
              accord de principe) ; la source reste à concevoir (sur l'ESP32, le
              générateur matériel n'est vraiment aléatoire qu'avec la radio ou la source
              d'entropie de l'ADC : à vérifier) ;
            - redémarrer la carte depuis le programme : `board.restart()` ; ce qui part
              avant (réponse USB en cours...) reste à convenir ;
            - une lecture série avec délai (trames du lecteur RFID), en JS dans la stdlib.
            PLUS TARD (utilisateur, 2026-11-13) : le WiFi et un serveur TCP sur la carte
            (mode debug du firmware C) ; la mise à jour du programme par la tablette (une
            API JS qui reçoit et installe un `.qjspkg`, signature à décider). Le secret
            d'authentification en clair dans le code : rien à faire (utilisateur).
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
            Buttons, console, analyseur des broches. Reste : SSD1306, SPI / UART joués par des
            modèles, vitesse de la carte, scénarios, signaux. Ce qui suit est l'historique de la
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
      - [x] **analyseur logique / oscilloscope sur les broches** (utilisateur, 2026-11-12) :
            FAIT le 2026-11-12 en version simple (utilisateur : « tu vas trop loin ») :
            panneau « Analyzer » dockable en bas, en onglet avec la console
            (`host/twin/ui/src/scopeview.ts`) ; broches ajoutées depuis sa barre, une trace
            chacune sur fond noir, une couleur par trace (menu du bouton de la broche),
            base de temps, Run / Stop, Clear ; une entrée analogique lue par le
            programme ou avec un composant dessus est tracée en volts (ADC) ; changements
            horodatés en µs par le jumeau (`host/twin/sim/twin_scope.cpp`, message
            `twin.scope.data`), 256 par broche toutes les 50 ms, au-delà une bande
            « occupé » ; architecture.md 6.6. Ajouté le 2026-11-13 (utilisateur) : vue figée
            mobile (Ctrl + molette = zoom autour du pointeur, Maj + molette = défiler
            dans le temps ; pas de glisser), voies défilantes verticalement (molette) au-dessus de l'axe des temps
            fixe, échelle des voies analogiques (lignes à vref et 0 V, petite police), Record (bascule :
            tout garder à partir de maintenant sans effacer l'affichage, sans limite de 20 s,
            jusqu'à 1 000 000 de changements ; arrêté : l'enregistrement est copié à part et
            Run s'arrête aussi, la vue figée dessus ; états rangés en
            tableaux typés, ~11 Mo pour un million) et Save
            (fichier VCD pour PulseView / GTKWave). Ajouté le 2026-11-13 après midi : PWM et
            impulsions RMT (fronts calculés par la page), modes Roll / Normal / Single avec
            déclenchement (voir « Où on en est »). Pas fait : mode Auto, holdoff et position
            du déclenchement réglables, décodeurs dans la page ; le temps est celui du PC ;
      - [ ] **le jumeau dans VS Code** (utilisateur, 2026-11-12) : embarquer la page du
            jumeau à l'intérieur de VS Code (aujourd'hui dans un navigateur, sur
            `http://127.0.0.1:<ports.ui>/`) ; à concevoir ;
      - [x] **x4js dans les crédits** (utilisateur, 2026-11-12) : FAIT le 2026-11-13, un
            paragraphe dans « Credits and license » de README.md, après QuickJS ;
      - [x] **redimensionnement du panneau des broches** (utilisateur, 2026-11-12) : FAIT le
            2026-11-13 (utilisateur : « mets une scroll hz ») : le tableau garde sa largeur, un
            panneau plus étroit défile horizontalement, en-tête et lignes ensemble. Reste, côté
            x4js (à toi) : la bande de droite garde sa largeur quand la fenêtre rétrécit et
            écrase la zone du milieu (`DockingView` : le milieu a `min-width: 0` ; la borne
            « place − MIN_AREA » du séparateur n'est pas appliquée au redimensionnement de la
            fenêtre) ;
      - [x] **UART du jumeau branchées sur de vrais ports série du PC** (utilisateur,
            2026-11-13) : FAIT le même jour (`host/twin/sim/twin_uart_serial.cpp`,
            architecture.md 6.6) : sous-menu « PC port » (port, connect, real speed) du menu
            d'une icône réglages dans la barre de titre de chaque UART, à côté du sous-menu
            « TCP » (utilisateur, 2026-11-13 : moins de place, des champs dans les menus ;
            avertissements par Notification), choix gardé dans `twin.json` (`twin_choices.cpp`, commun
            aux liens TCP) ; `--uart-serial 1=COM5` et `"twin.uarts"` de `qjsx.json`
            branchent dès le démarrage sans rien garder ; vitesse réelle par défaut ou
            immédiate ; TCP et port du PC ensemble. Testé sans port réel
            (`tests/twinwire.test.js`, 18 vérifications : états, erreurs, `twin.json`) ;
            À ESSAYER avec un adaptateur USB-série (TX relié à RX) ;
      - [x] jumeau choisi dans `qjsx.json` (FAIT le 2026-11-12 : section `"twin"`, `qjsx
            agent` dans le dossier ou `-p`) ;
      - [ ] d'autres cartes (PICO2-XXL...) ;
      - [ ] **assistant de conception d'une carte** (utilisateur, 2026-11-13) : un
            assistant qui fabrique la description JSON d'une carte du jumeau
            (`boards/<carte>.json` : broches, UART, ADC, PWM, natifs, pièces) ; à préciser ;
      - [ ] **composant « flash »** (utilisateur, 2026-11-13) : créer un composant (modèle
            + vue) flash ; à préciser avec l'utilisateur (quel composant, quel bus) ;
      - [x] la page reconstruit le tableau des broches à chaque changement (RÉGLÉ le
            2026-11-12 : la page x4js construit les lignes une fois et les met à jour).

- [ ] **Éditeur Monaco pour les devices** (utilisateur, 2026-11-12) : à préciser. x4js a un
      composant `MonacoEditor` (`x4js/monaco`).

- [x] **Manuel du développeur JS + `.d.ts`** (utilisateur, 2026-11-12 ; FAIT le 2026-11-13,
      CLAUDE.md « JS developer manual and typings » et règle « TYPINGS », architecture.md
      6.5) : un `.d.ts` écrit à la main À CÔTÉ DE CHAQUE MODULE (15 fichiers), embarqués
      dans `qjsx` (`qjsx:types`) ; `qjsx build` les écrit dans `<projet>/.qjsx/types/`
      (+ `jsconfig.json` avec `"types": ["./.qjsx/types"]` s'il manque ; sinon
      avertissement encadré avec la ligne à ajouter) ; essayé avec tsc sur un projet ;
      `tests/types.test.js` (297 vérifications) ; site Astro + Starlight dans
      `doc/jsmanual/` (7 guides + référence générée par TypeDoc), vu dans le navigateur.
      Suite Debug 102 OK. Restent : publier le site (GitHub Pages, avec la CI) ; les
      défauts relevés par les agents en écrivant les `.d.ts` (known-bugs.md) ; un module
      de plus = son `.d.ts` + sa ligne dans `doc/jsmanual/tsconfig.typedoc.json` et
      `astro.config.mjs`.

- [ ] **Livetrace des ports** (utilisateur, 2026-11-13) : à préciser.

- [ ] **Retirer le mode standalone** (utilisateur, 2026-11-13 : « ça a un intérêt
      discutable ») : compris comme `qjsx build --executable` (le runner `qjsx-runtime` +
      le paquet collé à la fin, CLAUDE.md « qjsx-runtime ») ; à retirer avec ses tests
      (run-tests.sh), sa doc (architecture.md, guide « Programs of the PC » du manuel,
      aide de `qjsx`) et la section `desktop` de `qjsx.json` si elle ne sert plus qu'à
      lui (elle sert aussi à `qjsx run` d'un paquet : à voir).

- [ ] **« Fusible » contre le debug d'une carte en production** (utilisateur, 2026-11-13) :
      un réglage, façon eFuse, qui empêche de déboguer (et sans doute de déployer, de lire
      les fichiers...) une carte livrée ; à concevoir : irréversible ou non (vrai eFuse de
      l'ESP32, ou drapeau en flash), ce qu'il bloque dans l'agent, comment une carte
      revient en atelier ; lien avec « Protéger le code d'un programme sur le device ».

- [ ] **Évaluer le gain à retirer BigInt** (utilisateur, 2026-11-13 : « trop pour une
      board ») : mesurer ce que BigInt coûte au firmware (taille du code : `builtins/bigint.c`,
      `core/bigint-arith.c`, les chemins BigInt de l'interpréteur et des conversions ; RAM)
      et ce qui casse sans lui (`BigUint64Array`, `crypto.getRandomValues` sur un tableau
      BigInt, `JSON`, littéraux `123n` refusés à la compilation) ; changement du moteur à
      discuter avant de coder.

- [ ] **Le jumeau en option, version pro payante** (utilisateur, 2026-11-13) : mettre le
      jumeau numérique sur option (une version « pro » payante de qjsx) ; à concevoir
      (licence, activation, ce qui reste dans la version libre).

- [ ] **Article LinkedIn** (utilisateur, 2026-11-13) : rédiger un article assez complet pour
      LinkedIn, avec de belles images : le jumeau numérique, l'architecture, etc.

- [x] **Entrées ADC** (utilisateur, 2026-11-12 ; FAIT le même jour, pas encore essayé sur la
      carte) : `device/adc` (`new Adc(pin, { range })`, `read()` brut, `readVoltage()` en volts,
      `bits`, `close()`), ESP32 sur ADC1 seulement (`hal_adc_esp32.cpp`, adc_oneshot +
      calibration ; le firmware compile), jumeau (`"adc"` dans `boards/*.json`, `pin.voltage`
      de `qjsx:twin`, composant `potentiometer`).

- [ ] **Sorties DAC** (utilisateur, 2026-11-12) : REPORTÉ, à faire avec l'oscilloscope du
      jumeau. Nos cartes n'ont pas de DAC (ESP32-S3, RP2350) ; l'ESP32 classique et l'ESP32-S2
      en ont un (8 bits), le STM32H743 aussi (12 bits).

- [ ] **Protéger le code d'un programme sur le device** (utilisateur, 2026-11-12) : PLUS TARD.
      Aujourd'hui les chaînes du bytecode sont en clair (littéraux, noms de propriétés,
      imports, nom du module ; le strip n'enlève que source, positions et noms de variables)
      et le bytecode se décompile. Piste ESP32 : chiffrement matériel de la flash (clé
      XTS-AES dans les eFuses, déchiffrement à la volée par le cache : l'exécution en place
      via esp_partition_mmap continue de marcher), flag `encrypted` sur la partition
      `packages` (écrire avec esp_partition_write, pas _raw), et Secure Boot avec (sinon un
      autre firmware lit la flash en clair). Modes Development / Release (irréversible).
      Reste en clair : la RAM et la liaison pendant `qjsx deploy`. RP2350 : boot signé + clé
      en OTP, pas de déchiffrement transparent de la XIP.

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
