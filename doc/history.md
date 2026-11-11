# Historique des défauts corrigés

Un défaut corrigé quitte `known-bugs.md` et arrive ici, avec ce qui a été fait. Le plus
récent est en premier.

## 2026-11-11

### Port série très lent sous Windows

- **Défaut.** `espjs deploy` mettait 16 à 26 s pour envoyer 47 Ko à la carte à 921600 bauds,
  et une tentative a dépassé 60 s. Écrire un seul octet prenait de 51 ms à 6 s, toujours par
  pas d'environ 100 ms. Une requête pouvait dépasser son délai de 3 s et être renvoyée.
- **Cause.** Dans `host/modules/serial_module.cpp`, le port était ouvert en mode synchrone
  (`CreateFileA` sans `FILE_FLAG_OVERLAPPED`). Le thread lecteur (`readLoop`) restait bloqué
  dans `ReadFile` jusqu'à 100 ms (`COMMTIMEOUTS`), puis recommençait aussitôt. Sur un handle
  synchrone, Windows fait passer les opérations une par une : le `WriteFile` du thread JS
  attendait la fin du `ReadFile` en cours, et le lecteur lui repassait souvent devant.
  Pendant un envoi, la carte attend la requête et n'émet rien, donc chaque lecture allait au
  bout de ses 100 ms. La carte et le protocole n'étaient pas en cause.
- **Correction.** Le port est ouvert avec `FILE_FLAG_OVERLAPPED` ; chaque lecture et chaque
  écriture a sa structure `OVERLAPPED` et son événement (`beginOperation`, `endOperation`).
  Lectures et écritures sont indépendantes. Le reste ne change pas (lecture qui rend la main
  au bout de 100 ms, drapeau `closing`).
- **Vérifié.** Sur la carte : une écriture d'un octet prend 0 à 1 ms ; l'envoi de 47 Ko passe
  de 16–26 s à 1,5 s (50 ms par morceau de 2 Ko, régulier) ; le debugger passe 52/52 en
  1,7 s. Tests du PC en Release et en Debug.

### Test du debugger avec un programme déjà en cours

- **Défaut.** `tests/debugger.test.js` échouait si un programme tournait déjà sur le device
  quand il se connectait (vu sur la carte, avec `debuggee` installé et sa boucle `spin` en
  cours). Son `STOP` initial arrêtait ce programme, d'où un événement `EXITED` ; le test
  attendait ensuite `STARTED` et recevait cet `EXITED` (« program started: expected 32,
  got 31 »), puis tout se décalait.
- **Correction.** `expectStarted()` saute les `EXITED` qui précèdent le `STARTED` attendu :
  le device termine toujours un programme avant de lancer le suivant, donc l'`EXITED` de
  l'ancien arrive forcément avant. Tout autre événement fait échouer la vérification.
- **Vérifié.** Tests du PC en Release et en Debug ; sur la carte, 52/52 avec un programme en
  cours au moment de la connexion.

### Chien de garde pendant un calcul long (ESP32)

- **Défaut.** Un programme JS qui calculait sans rendre la main affamait la tâche idle du
  cœur 1 : ESP-IDF affichait « task_wdt ... IDLE1 » toutes les 5 s.
- **Correction.** `platform::yieldIfDue()`, appelée par `Runner::interrupt` : une pause d'un
  tick toutes les 100 ms. Explications dans `architecture.md` 5.5.
- **Vérifié.** Sur la carte, aucun « task_wdt » en 17 s pendant la boucle `spin`.

### Défauts « graves » relevés par les agents qui ont documenté le code

Les tests de `run-tests.sh` passent en Release et en Debug, mais aucun test n'a été écrit pour
ces défauts en particulier.

1. **`fs.rmSync` avec `recursive` suivait les liens symboliques et les jonctions**
   (`common/runtime/modules/fs.cpp`, `removeTree`).
   - Défaut : il utilisait `files::stat`, qui suit les liens. Un lien vers un dossier à
     l'intérieur de l'arbre faisait effacer le contenu de la cible. Node utilise `lstat` et ne
     supprime que le lien.
   - Correction : `removeTree` utilise `files::linkStatus` (lstat ; jonctions et liens Windows
     par l'attribut point d'analyse). Un lien est supprimé lui-même, jamais le contenu de sa
     cible.
2. **`serial`, `SerialPort::write` après `close()`** (`host/modules/serial_module.cpp`).
   - Défaut : `_state` valait `nullptr` alors que `_open` était encore vrai, d'où un
     déréférencement de `nullptr`. Après une erreur de lecture, le thread lecteur libérait
     `PortState` alors que le JS le pointait encore : `write()` et `requestClose()` utilisaient
     de la mémoire libérée.
   - Correction : `PortState` est compté par références (côté JS + thread lecteur). Le dernier
     ferme le port et libère l'état ; `write` refuse un port dont le JS a lâché l'état.
3. **CBOR : bornes qui débordaient** (`common/agent/cbor.cpp`).
   - Défaut : `Reader::text`, `Reader::bytes` et `skip` (cas des chaînes) testaient
     `_position + argument > _size` avec une longueur 64 bits venue de la liaison. Une longueur
     énorme faisait déborder le test, d'où une lecture hors limites ; sur 32 bits, le cast
     `(size_t)` tronquait en plus la longueur. `skip` sur une map calculait `argument * 2`, qui
     pouvait déborder. La récursion de `skip` n'avait pas de limite de profondeur.
   - Correction : `fits()` borne toute longueur sans débordement ; `skip` est limité à 16
     niveaux ; les maps se parcourent par paires.
4. **Paquet zip mal formé** (`common/runtime/package.cpp`, `Reader::open`).
   - Défaut : les longueurs du nom, de l'extra et du commentaire n'étaient pas bornées par la
     fin du répertoire ; sur 32 bits, les additions de décalages pouvaient déborder. Résultat :
     des lectures hors limites, y compris dans `find()` (`memcmp`). Après un `open()` raté, les
     entrées déjà lues restaient dans la table.
   - Correction : décalages 64 bits et `within()` ; nom d'entrée borné ; pointeurs calculés
     après contrôle ; table vidée après un échec.
5. **Le registre commun enregistrait le module `net`**, qui n'existe que sur l'hôte
   (`common/runtime/modules/modules.cpp` référençait `jsNewModule_net`).
   - Défaut : le firmware ne se serait pas lié.
   - Correction : `net` est enregistré par `host/main.cpp`, plus par le registre commun.
6. **Débordements de calcul de taille dans le stub** (`common/agent/debugger.cpp`).
   - Défaut : les multiplications par une valeur venue du réseau pouvaient déborder sur
     32 bits, d'où un tampon trop petit et une écriture hors tas (`handleSetStops` :
     `(size_t)count * 4` et `sizeof(uint32_t) * (count + 1)`) ; dans `handleCall`,
     `argumentCount` n'était pas borné ; le `malloc` n'était pas vérifié.
   - Correction : le nombre d'offsets est comparé à ce qui reste avant toute multiplication ;
     `CALL` est limité à 256 arguments.

### Allocations jamais vérifiées

- **Défaut.** `malloc` / `realloc` jamais vérifiés (stub, `receiveBlock`, `registerName`...).
- **Correction.** Tout notre code alloue par `memory::allocate` / `reallocate` / `release`
  (`common/runtime/memory.h`). Un manque de mémoire y est fatal (`platform::fatal`), donc les
  `malloc` non vérifiés n'existent plus. Tests OK le 2026-11-11.

### Défauts « moyens »

Les tests de `run-tests.sh` passent en Release et en Debug, mais aucun test n'a été écrit pour
ces défauts en particulier. Les branches POSIX / macOS n'ont été que relues, pas compilées
(`pthread_condattr_setclock`, `poll`, `fseeko`, `MSG_NOSIGNAL`...). Le pas-à-pas over / out
n'est pas corrigé : il reste dans `known-bugs.md`.

- **Runner et debugger** :
  - STOP et DEBUG_ATTACH pendant le démarrage ;
  - PAUSE perdue après SCRIPT_LOADED ;
  - `hostGone` (`followHost`) ;
  - requêtes répétées (gardées dans la file pendant leur exécution, dernière réponse séparée
    agent / debugger) ;
  - double verrou de `onMessage` (`resendAnswer`) ;
  - `attach` (`forgetAnswer`) ;
  - garde `_requestDepth` dans `writeObject` ;
  - SET_STOPS tronqué (erreur, rien d'appliqué) ;
  - mode de SET_EXCEPTIONS borné ;
  - `runDevice` remet la sortie à zéro ;
  - `formatDate` sans `gmtime` (jours civils de Hinnant) ;
  - BigInt invalide ;
  - `flushSegment` sans copie quand il n'y a pas de callback texte.
- **Runtime et plateforme** :
  - rejet du dernier timer affiché ;
  - `net` : état de socket compté par références, `closing` + `shutdown`, threads qui
    attendent avec `waitReadable`, échec de démarrage d'un thread, `connectDone` ;
  - `MSG_NOSIGNAL` / `SO_NOSIGPIPE` ;
  - lien de l'agent sans lecteur (`endLink`) ;
  - `finishUpload` (ordre décrit dans le code) ;
  - `Mutex` récursif partout ;
  - `Signal::wait` sur horloge monotone ;
  - `files.cpp` : fichier sur le chemin, `errno`, fichiers de plus de 2 Go, racine de disque
    vide, erreur de `readdir`, `rename` ;
  - délais des timers (NaN, négatifs, trop grands : 1 ms comme Node) ;
  - `fs` : `writeFileSync`, getter `encoding`.
- **jsbind** :
  - `Resolver` déjà réglé ou vide ;
  - `js::fail` du constructeur dans `js::create` ;
  - argument `JS_EXCEPTION` dans `callWith` ;
  - appels hors d'un appel JS (`detail::propagateException`) ;
  - `registerName` (noms dédoublonnés, `thread_local`, 32768 au plus) ;
  - bornes exactes 2^63 / 2^64 ;
  - `JS_NewObject` vérifié ;
  - `static_assert` au-delà de 64 `JS_EXPORT`.

### Défauts « mineurs »

- **`process.exit` ne libérait pas le runtime** : il termine maintenant le programme, plus le
  processus (tests « process.exit » de `run-tests.sh`).
- **`setImportMeta` gardait les `\` des chemins Windows** dans l'URL `file:///` : corrigé par
  `files::useSlashes`.
