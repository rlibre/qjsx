# Modifications du moteur QuickJS pour le debugger

Ce document décrit les modifications faites à la main dans `engine/qjsx/` pour le
debugger. Il complète [architecture.md](architecture.md) (sections 2.6, 2.7 et 3).

- Chaque modification est commentée `espjs:` dans le code.
- Le diff exact par rapport au découpage non modifié est
  [engine-changes.patch](engine-changes.patch). On le régénère avec
  `bash tools/quickjs-split/engine-diff.sh`.
- `engine/qjs-bellard/` (QuickJS d'origine) n'est pas touché.
- `tools/quickjs-split/regen.sh` refuse d'écraser `engine/qjsx/` sans `--force`, car
  cela effacerait ces modifications. Pour passer à une nouvelle version de QuickJS : régénérer
  dans un autre dossier, puis réappliquer `engine-changes.patch`.
- Le format du bytecode change : `BC_VERSION` passe de 5 à `0x45`. Un bytecode d'une autre
  version est refusé à la lecture.

Les modifications A à F ci-dessous sont dans l'ordre où elles ont été faites. La section
« Limites » liste ce qui n'est pas fait ou se comporte autrement que prévu.

## A. Nom du module conservé sans les infos de debug

**Problème.** Avec `JS_STRIP_DEBUG`, QuickJS jette aussi `debug.filename`. Or `import.meta`,
`import()` et les backtraces retrouvent le module courant par ce nom. Les paquets étaient donc
compilés avec `JS_STRIP_SOURCE` seulement, ce qui gardait les noms de variables et le pc2line
dans le bytecode envoyé à l'ESP.

**Modification.**
- `JSFunctionBytecode.filename` sort de la structure `debug`. Il est toujours rempli et toujours
  sérialisé.
- Fichiers : `quickjs-internal.h`, `bytecode/reader.c`, `bytecode/writer.c`, `core/gc.c`,
  `core/errors.c`, `core/iteration.c`, `core/modules.c`, `compiler/finalize.c`,
  `compiler/funcdef.c`.
- `espjs build` compile maintenant avec `strip: "all"` : le bytecode du paquet ne contient plus
  ni source, ni positions, ni noms de variables.

**Test.** `run-tests.sh` : « package runs like the sources », avec un module qui utilise
`import.meta` et `import()`.

## B. Appels terminaux désactivés

**Modification.**
- `resolve_labels` ne fusionne plus `call; return` en `OP_tail_call` ni
  `call_method; return` en `OP_tail_call_method`.
- Fichier : `compiler/resolve-labels.c`.
- Les opcodes `tail_call*` restent dans l'interpréteur, mais le compilateur ne les produit plus.

**Correction d'une affirmation faite pendant la conception.** J'avais dit que l'appel terminal
de QuickJS supprimait la frame de l'appelant, ce qui aurait tronqué les piles d'appels. C'est
faux. QuickJS appelle `JS_CallInternal` normalement, puis saute à `done` : la frame de
l'appelant reste sur la pile pendant l'appel. Le gain de la fusion se limitait à un dispatch.

La désactivation reste utile : le `OP_return` séparé est l'endroit où le debugger s'arrête pour
montrer la valeur de `return f()` (voir E). Elle respecte aussi la décision « toujours »
(même bytecode en debug et en production).

## C. `debugger;` → `OP_debugger`

**Modification.**
- Nouvel opcode `OP_debugger` (1 octet), placé juste avant `nop` dans `quickjs-opcode.h`. Il y a
  maintenant 245 opcodes, pour une limite de 256.
- Le parseur l'émet pour `debugger;` (`compiler/parser-stmt.c`). Avant, le mot-clé était ignoré.
- L'interpréteur appelle le hook avec la raison `JS_DEBUG_REASON_STATEMENT`. Sans hook, l'opcode
  ne fait rien.

**Test.** `tests/debug.test.js` : section « debugger statements ».

## D. Numéro de fonction et recherche d'une fonction

**Modification.**
- `JSFunctionBytecode.function_number` (16 bits, sérialisé) : numéro de la fonction dans son
  script, dans l'ordre de création. Les fonctions filles sont créées avant leur parente : la
  fonction de premier niveau reçoit donc le dernier numéro.
- Le compteur vit dans la `JSFunctionDef` racine (`function_counter`, dans
  `compiler/compiler.h`). Le numéro est donné dans `js_create_function`
  (`compiler/finalize.c`).
- `JS_DebugSetStops(ctx, module, number, ...)` retrouve la fonction en parcourant
  `ctx->loaded_modules`. Pour le module trouvé, il descend l'arbre des fonctions, qui sont
  rangées dans le pool de constantes (`core/debug.c`).
- `JS_DebugGetFunctionLocation(ctx, fn, &module, &number)` donne l'inverse. Le stub en aura
  besoin pour `[[FunctionLocation]]` en CDP.

**Écart par rapport à la conception.** Il n'y a ni build id dans le bytecode, ni table des
fonctions construite par le lecteur.
- Le nom du module est maintenant toujours présent (A).
- Le stub identifie donc un script par le build id du paquet (dans le manifeste), plus le nom du
  module.
- Il identifie une fonction par le nom de son module et son numéro.
- La recherche se fait à la demande, quand le proxy pose des arrêts. Elle est linéaire en nombre
  de fonctions du module, ce qui est rare et peu coûteux.
- Rien n'est à invalider quand une fonction est libérée : les arrêts vivent dans la fonction
  elle-même.

**Test.** `tests/debug.test.js` : `functionLocation`, puis `setStops` sur une fonction
inconnue et sur un module inconnu.

## E. Positions et portées pour l'annexe de debug (sidecar)

### Positions

QuickJS émet des `OP_line_num <offset source>` aux endroits qui peuvent lever une exception, en
phase 1 (le parseur). `resolve_labels` les retire et en tire la table pc2line.

**Modification.**
- Les deux bits hauts de l'opérande donnent une nature (`LINE_NUM_KIND_SHIFT`,
  `line_num_pos()` et `line_num_kind()` dans `compiler/compiler.h`) :
  - 0 : expression (les positions d'origine) ;
  - 1 : début d'instruction ;
  - 2 : retour.
- `emit_debug_pos()` (`compiler/emitter.c`) émet un marqueur de nature 1 ou 2.
- Les marqueurs sont **toujours** émis, avec ou sans collecte. Le bytecode ne dépend donc pas de
  l'option.

**Où sont émis les marqueurs** (`compiler/parser-stmt.c`, `parser-func.c`, `parser-class.c`,
`parser-expr.c`) :

- **Début d'instruction (1)** :
  - au début de `js_parse_statement_or_decl`, après les étiquettes ;
  - à l'entrée d'une clause `catch` ;
  - au test de `while`, `do…while` et `for` (un arrêt à chaque tour) ;
  - à l'expression d'incrément du `for`. Ce code est déplacé après le corps de la boucle, et
    son marqueur le suit.
  - Une fonction fléchée à corps expression reçoit aussi un marqueur au début de l'expression.
- **Retour (2)** : juste avant l'opcode de retour (`OP_return`, `OP_return_undef`,
  `OP_return_async`), donc après les clauses `finally`.
  - Pour un `return` explicite, la position est celle du mot-clé `return`.
  - Pour le retour implicite, c'est l'accolade fermante.
  - Pour une fléchée à corps expression, c'est le dernier caractère du corps.
  - `emit_return` reçoit maintenant cette position. `NULL` signifie « pas d'arrêt », pour les
    retours générés : `return()` des générateurs et constructeurs par défaut.

**Enregistrement** (`compiler/resolve-labels.c`, `compiler/debug-info.c`) :
- En phase 3, un marqueur de nature 1 ou 2 est enregistré au pc de la prochaine instruction
  émise, c'est-à-dire à la taille courante de la sortie.
- Les positions d'expression sont enregistrées au même endroit que le pc2line
  (`add_pc2line_info`), y compris quand les infos de debug sont retirées.
- Deux arrêts au même pc : le dernier gagne. Par exemple `{` suivi de sa première instruction.
- Les masques de nature sont retirés avant le pc2line d'origine.

**Pour que les optimisations ne cassent pas les arrêts :**
- Les optimisations à trous (`code_match`) traversent les marqueurs, comme les positions
  d'origine. Exemple : `put_loc x; get_loc x` → `set_loc x`, à cheval sur deux instructions.
- Quand une passe (`resolve_variables` ou `resolve_labels`) saute plusieurs instructions d'un
  coup parce qu'une optimisation les a fusionnées, `js_debug_skipped_marker` cherche un marqueur
  dans la partie sautée :
  - en phase 2, il est réémis ;
  - en phase 3, il est enregistré au pc de l'instruction émise suivante.
  - L'arrêt tombe donc juste après l'instruction fusionnée. La partie absorbée de l'instruction
    suivante est sans effet (lecture d'une variable locale, par exemple).
- Un saut qui se termine sur un label est une suppression de code mort : ses marqueurs sont
  abandonnés.
- `find_jump_target` s'arrête sur un marqueur. Sinon, un saut vers le retour implicite serait
  remplacé par un `return` sans arrêt.
- `skip_dead_code` oublie les marqueurs du code mort : le code mort n'a pas d'arrêt.

**Historique.** Une première version interdisait à `code_match` de traverser un marqueur.
`global_read` du microbench était alors 60 % plus lent, à cause des fusions `put_loc` /
`get_loc` perdues entre instructions. D'où le mécanisme ci-dessus.

**Conséquences à connaître :**
- Si une fusion absorbe le début d'un `return x;`, son arrêt « instruction » rejoint l'arrêt
  « retour » au même pc, et seul ce dernier reste. Le pas-à-pas s'arrête toujours sur la ligne
  du `return`.
- Les marqueurs mettent à jour la « position courante » de `resolve_labels`. Les backtraces des
  scripts exécutés depuis les sources (`espjs run file.js`) peuvent donc donner une colonne
  différente de l'original, en général plus juste.

### Portées

**Écart par rapport à la conception.** La conception prévoyait des marqueurs d'entrée et de
sortie de portée jusqu'en phase 3, pour obtenir des plages de pc. Ces marqueurs auraient dû
traverser `resolve_variables`, qui supprime `enter_scope`/`leave_scope`, puis survivre à
l'élimination du code mort. C'est fragile.

À la place, chaque portée garde sa **plage source** `[début, fin)` :
- `JSVarScope.source_start` et `source_end`, remplis par `push_scope` et `pop_scope` ;
- la portée 0 couvre toute la fonction.

Le proxy connaît la position source de l'arrêt grâce à la table des positions. Il en déduit les
portées qui la contiennent, puis les variables visibles. C'est aussi ce que fait V8 (plages
source dans ScopeInfo).

### Collecte

API dans `quickjs-debug.h` : `JS_SetDebugInfoCollector(rt, fn, opaque)`.
- `js_create_function` appelle la fonction de collecte pour chaque fonction compilée, filles
  d'abord.
- L'appel a lieu avant que les noms de variables soient retirés (`js_debug_info_collect`, dans
  `compiler/debug-info.c`).
- La fonction de collecte reçoit :
  - le numéro, le nom et la plage source de la fonction, et la taille du bytecode ;
  - les positions triées par pc ;
  - les arguments ;
  - les variables locales, avec leur portée et leurs drapeaux (`LEXICAL`, `CONST`, `CAPTURED`) ;
  - les variables de closure (drapeau `GLOBAL` pour les références globales) ;
  - les portées (parent et plage source).

Côté hôte :
- `compile(source, name, { debug: true })` rend `{ bytecode, debug }`
  (`host/modules/compiler_module.cpp`, format décrit en tête du fichier). Sans `debug`, `debug` vaut
  `null`.
- `espjs build` écrit `functions` dans `debug/<path>.json` (`SIDECAR_FORMAT` 2). L'indice dans
  `functions` est le numéro de fonction.

**Test.** `tests/debug.test.js` vérifie les positions de `add()` : trois débuts d'instruction
et un retour. Il vérifie aussi la suite exacte des arrêts en pas-à-pas dans une boucle `for` :
instruction, test, corps, incrément, …, `return`. Tous les pc vus par le hook sont retrouvés
dans l'annexe.

## F. Interpréteur : arrêts, hook, frames

API publique dans `engine/qjsx/quickjs-debug.h`, implémentée dans `core/debug.c`. L'état
est dans `JSRuntime.debug` (`JSDebugState`, dans `quickjs-internal.h`).

### Double table de dispatch (`core/interpreter.c`)

- Une variable locale `dispatch` pointe sur la table normale ou sur `debug_dispatch_table`.
  Toutes les entrées de cette dernière mènent à `debug_check`, qui vérifie les arrêts puis saute
  via la table normale.
- Une frame prend la table de debug seulement si les trois conditions sont réunies
  (`js_debug_frame_has_stops`) :
  - un hook est installé ;
  - le hook n'est pas en cours ;
  - la fonction a des points d'arrêt, ou bien des débuts d'instruction alors que le pas-à-pas
    est actif.
- Le reste du code ne paie rien.
- Le choix de table est refait (`REFRESH_DEBUG_DISPATCH`) :
  - à `restart` : entrée, reprise de générateur, saut vers un `catch` ;
  - après `OP_call`, `OP_call_method` et `OP_call_constructor` ;
  - après un appel au hook ;
  - quand `js_poll_interrupts` appelle vraiment le gestionnaire d'interruption, toutes les
    10 000 itérations environ. C'est le moyen pour une demande de pause venue du transport
    d'atteindre une boucle en cours.
- Sans « computed goto », un test avant chaque instruction remplace la seconde table.
- Par fonction, deux bitmaps (un bit par octet de bytecode) : `debug_breakpoints` et
  `debug_statements`. Ils ne sont pas sérialisés et sont libérés avec la fonction. Le bytecode
  peut rester en ROM : les bitmaps sont dans la structure en RAM.

### Hook

- `JS_SetDebugHook(rt, hook, opaque)`.
- Le hook reçoit un `JSDebugEvent` :
  - `reason` : `BREAKPOINT`, `STEP`, `STATEMENT` ou `EXCEPTION` ;
  - `exception` : la valeur levée ;
  - `at_return` et `return_value` : renseignés quand l'instruction d'arrêt est un retour.
- Pendant le hook :
  - pas d'appel imbriqué ;
  - le hook peut appeler du JS (évaluation).
- Le hook rend -1 pour lever l'exception qu'il a posée. Ce n'est pas encore testé.
- `JS_DebugSetStepping(rt, on)` : en pas-à-pas, le hook est appelé à chaque début d'instruction
  des fonctions qui en ont. Le stub filtre « over » et « out » en comparant les frames. Une
  demande de pause, c'est le pas-à-pas activé.
- `JS_DebugSetStops(ctx, module, number, BREAKPOINTS | STATEMENTS, offsets, count)` remplace
  les arrêts d'une fonction.

### Frames et emplacements

- `JS_DebugGetFrame(ctx, level, &frame)` : niveau 0 = frame arrêtée. La structure donne :
  - `id` : adresse de la `JSStackFrame`, sur la pile C ou dans l'état d'un générateur ;
  - le module et le numéro de la fonction ;
  - le `pc` ;
  - les nombres d'arguments, de variables locales et de variables de closure.
- `JS_DebugGetFrameFunction` : la fonction de la frame.
- `JS_DebugGetSlot` et `JS_DebugSetSlot` : argument, variable locale ou closure, par indice. Une
  variable en zone morte temporelle rend une valeur de tag `JS_TAG_UNINITIALIZED`.
- Sens du `pc` :
  - au niveau 0, c'est l'instruction d'arrêt exacte ;
  - pour une exception, il est quelque part dans l'instruction qui lève ;
  - pour les niveaux > 0, il est juste après l'instruction d'appel.
  - Le proxy doit donc prendre la dernière position ≤ `pc - 1`, sauf au niveau 0 pour un arrêt.

### Exceptions

`JS_DebugSetExceptionReporting(rt, on)` active le signalement. Le hook est appelé une seule fois
par valeur levée, dans la frame qui la lève :

- dans le label `exception:`, après la construction de la backtrace ;
- pendant le hook, l'exception en attente est mise de côté, puis remise ensuite ;
- la valeur signalée est mémorisée (`reported_exception`, référence comptée). Le même objet ne
  repasse pas par le hook pendant qu'il remonte les frames appelantes.
- La valeur est oubliée quand :
  - une clause `catch` la rattrape ;
  - du code C la prend (`JS_GetException`).
- Elle n'est **pas** oubliée quand le handler est un « rethrow » : `gosub finally; throw`, ou
  simplement `throw` quand le `finally` est vide et que l'optimiseur a supprimé le `gosub`.
- Une valeur relancée dans une fonction async par un `await` rejeté, ou dans un générateur par
  `throw()`, n'est pas signalée à nouveau.

**Tests** (`tests/debug.test.js`, 41 vérifications) :
- point d'arrêt : frames, arguments, variables locales, zone morte temporelle, modification
  d'une variable ;
- arrêt sur le retour avec la valeur rendue ;
- retrait des arrêts ;
- suite des arrêts en pas-à-pas ;
- `debugger;` ;
- exception rattrapée, traversant un `finally` vide ou non vide, traversant un `await` ;
- aucun appel sans hook.

Le module natif de test est `tests/native/debug_module.cpp` (`espjs-tests` seulement).

### Aides pour le stub (ajoutées avec le stub du debugger)

Toutes ces aides sont dans `quickjs-debug.h` et `core/debug.c` :

- `JS_DebugGetObjectInfo(ctx, obj, &info)` : nature de l'objet (tableau, fonction, Map, Proxy,
  erreur…), nom de sa classe dans le moteur, taille (Map/Set, tableau typé, ArrayBuffer). Rien
  n'exécute de code JS : pas de getter, pas de trap de Proxy.
- `JS_DebugGetInternal(ctx, obj, which)` : les emplacements internes. On y trouve la cible et le
  handler d'un Proxy, la valeur primitive d'un objet `Number`/`String`… ou d'une `Date`, et la
  cible d'une fonction liée par `bind`.
- `JS_DebugClearAllStops(ctx)` : retire tous les arrêts quand le debugger se détache.

## G. Compiler un module sans charger ses imports

**Problème.** Avec `JS_EVAL_FLAG_COMPILE_ONLY`, QuickJS résout quand même les imports d'un module,
et donc les charge. `espjs build` échouait (« could not load module ») dès qu'un module était
compilé avant ceux qu'il importe. La démo passait uniquement parce que l'ordre alphabétique de
ses fichiers compilait les dépendances d'abord. Le bug a été trouvé avec la fixture du test du
stub (`main.js` importe `shapes.js`).

**Modification.**
- Nouveau drapeau public `JS_EVAL_FLAG_NO_RESOLVE` (`quickjs.h`). `compiler/eval.c` saute alors
  `js_resolve_module`. Le commentaire d'origine prévoyait d'ailleurs ce cas : « Could add a flag
  to avoid resolution if necessary ».
- Le module hôte `compiler` l'utilise pour les modules.
- Le bytecode produit ne change pas. Les imports sont résolus quand le module est chargé pour
  être exécuté.

**Test.** `tests/fixtures/debuggee` se construit, dans `tests/debugger.test.js`.

## H. Moteur sans compilateur (cibles)

**Modification.** Dans `core/runtime.c`, `JS_NewContext` n'appelle plus `JS_AddIntrinsicEval`
quand `QJSX_NO_COMPILER` est défini. Sans cette fonction, tout le dossier `compiler/` est
inutile. En dehors de `compiler/`, le moteur n'a qu'une seule dépendance vers le compilateur,
`JS_AddIntrinsicEval` ; c'est vérifié en croisant les symboles des fichiers objets. Sur un
moteur sans compilateur, `eval()`, `Function()` et `JS_Eval()` lèvent
« eval is not supported ».

**Build.** Le projet premake `qjsx-target` est le moteur sans `compiler/`, avec
`QJSX_NO_COMPILER`. L'exécutable `espjs-target` est l'outil lié avec ce moteur, sans son module
`compiler` (`ESPJS_NO_COMPILER`). Il lance les paquets comme le fera une cible.

**Test** (`run-tests.sh`). La fixture `tests/fixtures/nocompiler` couvre classes et champs
privés, générateurs, async, Map/Set, RegExp, JSON, tableaux typés, BigInt et exceptions. Elle
donne la même sortie avec `espjs` et avec `espjs-target`, sauf `eval`, qui échoue sur la cible.

**Reste à faire pour les cibles.** Les modules JS embarqués (par exemple `path`) sont stockés en
source et compilés au chargement : sur une cible, il faudra les embarquer en bytecode.

## I. Compilation avec newlib (ESP32, RP2350)

Deux modifications, chacune avec l'accord de l'utilisateur (2026-11-11, port ESP32). Rien ne
change sur le PC : Linux, macOS et Windows ne définissent pas `__NEWLIB__`.

**`malloc_usable_size`.** Dans `core/runtime.c`, l'allocateur par défaut de QuickJS
(`js_def_malloc_usable_size`) appelle `malloc_usable_size`, que newlib ne déclare pas. Une
branche `#elif defined(__NEWLIB__)` rend 0, comme le propose le commentaire d'origine. Les
devices fournissent leur propre allocateur (`board::jsAllocator`, en PSRAM sur l'ESP32) : cet
allocateur par défaut n'y sert pas.

**Fuseau horaire sans `tm_gmtoff`.** Dans `builtins/date.c`,
`getTimezoneOffset` lit `tm.tm_gmtoff` hors de Windows, champ absent de newlib, la bibliothèque
C de l'ESP32 et du RP2350. La condition `#if defined(_WIN32)` devient
`#if defined(_WIN32) || defined(__NEWLIB__)` : newlib prend le chemin de Windows (`gmtime`,
`localtime`, `mktime`), du C standard. Rien ne change sur le PC (Windows prenait déjà ce chemin,
Linux et macOS ne définissent pas `__NEWLIB__`).

## J. Imports d'un module et refus de `import()` (bibliothèques `.qjslib`)

Deux modifications, avec l'accord de l'utilisateur (2026-11-11), pour `espjs build` et
`espjs mklib` : un programme emporte dans son paquet les modules de bibliothèque qu'il importe,
donc le build doit connaître tous ses imports (voir `architecture.md`, 4.5).

**Liste des imports d'un module compilé.** QuickJS ne donnait aucun moyen public de lire les
modules qu'un module demande.
- Deux fonctions publiques dans `quickjs.h`, écrites dans `core/modules.c` :
  `JS_GetModuleRequestCount(m)` et `JS_GetModuleRequestName(ctx, m, index)` (un atome à
  libérer, `JS_ATOM_NULL` hors limites).
- Les noms sont ceux du source, non normalisés, dans l'ordre du source. Un module importé par
  plusieurs instructions revient plusieurs fois : l'appelant dédoublonne.
- Le module hôte `compiler` les rend dans `imports` ; `host/js/compile.js` les normalise comme
  le fait le moteur au chargement (`js_default_module_normalize_name`).

**Refus de `import()`.** Un `import()` charge un module dont le nom n'est connu qu'à
l'exécution : le build ne pourrait pas l'emporter.
- Nouveau drapeau public `JS_EVAL_FLAG_NO_DYNAMIC_IMPORT` (`quickjs.h`). `compiler/eval.c` le
  copie dans un nouveau champ `no_dynamic_import` de `JSParseState` (`compiler/compiler.h`).
- Dans `compiler/parser-expr.c`, `import(` devient alors une erreur de syntaxe
  (« 'import()' is not allowed: use a static import »), signalée à la position de l'appel.
  `import.meta` n'est pas touché.
- Le module hôte `compiler` passe le drapeau quand son option `dynamicImport` vaut `false`
  (`espjs build` et `espjs mklib` le font toujours ; le JS de l'outil n'est pas concerné).
- Sans le drapeau, rien ne change : le bytecode produit est le même.

**Tests.** `tests/tools.test.js` (liste des imports, refus et ligne signalée) ;
`run-tests.sh` (« import() refused with its position », fixture
`tests/fixtures/dynamicimport`). Les tests d'origine de QuickJS passent avec le moteur modifié.

## K. Numéro de bytecode public et avertissements de compilation

Deux modifications, avec l'accord de l'utilisateur (2026-11-11).

**Numéro de bytecode.** Un moteur ne lit que le bytecode de sa propre version (`BC_VERSION`,
vérifié par `JS_ReadObject`). Pour refuser un paquet compilé pour un autre moteur avant de
l'installer ou de le charger, il faut connaître ce numéro de l'extérieur.
- Nouvelle fonction publique `JS_GetBytecodeVersion()` (`quickjs.h`, écrite dans
  `bytecode/reader.c`).
- `espjs build` l'écrit dans le manifeste du paquet (`bytecode`) ; l'agent refuse un envoi
  d'un autre numéro, et le chargeur refuse de lancer un tel paquet (`architecture.md`, 4.1).
- **Règle : tout changement du moteur qui modifie le bytecode (opcodes, sérialisation)
  augmente `BC_VERSION`.** Sans cela, un paquet d'avant le changement serait accepté et lu de
  travers.

**Avertissements de compilation.** Sur un device, sans compilateur, `eval()` et le
constructeur `Function` lèvent « eval is not supported » ; sur le PC ils marchent. Le
compilateur signale donc ces usages quand il compile un source.
- API dans `quickjs-debug.h` : `JS_SetCompileWarningHandler(rt, handler, opaque)` ; le handler
  reçoit la nature (`JS_COMPILE_WARNING_EVAL`, `JS_COMPILE_WARNING_FUNCTION`), la ligne et la
  colonne (à partir de 1, comme dans une `SyntaxError`). Deux champs de plus dans
  `JSDebugState` (`quickjs-internal.h`).
- `compiler/debug-info.c` : `js_compile_warning()` appelle le handler, et
  `js_last_op_is_scope_var()` dit si la dernière instruction émise lit une variable par son
  nom.
- `compiler/parser-expr.c` signale : l'appel direct `eval(...)` (là où il choisit `OP_eval`),
  l'appel `Function(...)`, et `new Function`, avec ou sans arguments.
- La détection est syntaxique. Un usage indirect (`(0, eval)(x)`, `globalThis.eval`, un
  alias) n'est pas vu ; une variable du programme qui porte un de ces noms est signalée aussi.
- Sans handler, rien ne change : le bytecode produit est le même.

**Tests.** `tests/tools.test.js` (natures et positions) ; `run-tests.sh` (avertissement au
build de `tests/fixtures/nocompiler`, paquet d'un autre numéro refusé par l'agent et par le
chargeur). Les tests d'origine de QuickJS passent avec le moteur modifié. Le microbench n'a
pas donné de mesure exploitable ce jour-là (trop de bruit entre deux passes du même binaire)
et n'a pas été refait ; ces changements ne touchent pas l'interpréteur.

## Limites et points non faits

- **Prédiction « non rattrapée » (`sf->call_sp`) : non faite.** Le hook est appelé pour toutes
  les exceptions ; savoir si elle sera rattrapée reste à faire. Deux pistes :
  - `call_sp` sauvegardé avant chaque appel, comme prévu ;
  - ou l'annexe donne les plages `try` au proxy, sans changer le moteur.
- **Rafraîchissement de la table.** Une frame qui a appelé un getter, un `valueOf` ou un
  callback natif ne change de table qu'à son prochain appel, saut arrière ou retour. Un « step
  out » depuis un getter peut donc s'arrêter un peu plus loin que prévu.
- **`this`** n'est visible que si la fonction utilise `this` : QuickJS ne crée alors une variable
  locale `this` que dans ce cas. Ailleurs, le debugger ne peut pas l'afficher.
- **Seuls les modules sont trouvés** par `JS_DebugSetStops`. Les paquets ne contiennent que des
  modules.
- **Pas-à-pas.** Le filtrage « over » / « out » est dans le stub : un « step over » d'un appel
  long appelle le hook à chaque instruction de l'appelé, si l'appelé a ses débuts d'instruction
  posés. Un filtre par profondeur de frame dans le moteur viendra si c'est trop lent.
- **Identité des exceptions.** Deux cas où une exception n'est pas signalée :
  - `throw` d'une valeur primitive identique à la précédente non encore oubliée ;
  - un `finally` qui avale l'exception par `return`, puis la même valeur relancée plus tard.
- **`return` du hook à -1** : non testé.
- **Warnings.**
  - En Release, `array.c`, `interpreter.c`, `properties.c` et `runtime.c` donnent des
    « unused variable ». Ils existent aussi dans le découpage non modifié : ce sont des
    variables qui ne servent que dans des `assert`, retirés par `NDEBUG`. Ils ne viennent pas
    de ces modifications.
  - `-Wmaybe-uninitialized` dans `common/jsbind/jsbind_class.h:192`, pour un argument `bool` (vu en
    compilant `debug_module.cpp`). Faux positif probable, non corrigé.

## Performances

Mesures du 2026-11-10 :
- `tests/microbench.js` de Bellard, 3 passes par version, même méthode que
  [quickjs-split-perf.md](quickjs-split-perf.md) ;
- `qjs` construit avec le moteur d'origine et avec le moteur découpé et modifié ;
- aucun debugger branché ;
- les temps sont des ns par opération : un écart négatif veut dire plus rapide que l'original.

| Version du découpage | Total original | Total découpé | Écart |
|---|---:|---:|---:|
| découpage seul (avant ces modifications, quickjs-split-perf.md) | — | — | −3,8 % |
| + A, B, C et double table de dispatch | 4 645,6 | 4 455,6 | −4,1 % |
| + positions (E), version finale | 4 665,4 | 4 546,3 | −2,6 % |

- La version finale coûte environ 2 % de plus que la version sans marqueurs. Une partie de cet
  écart reste dans le bruit d'une passe à l'autre : `regexp_replace`, purement en C, varie de
  ±10 %.
- Les tests de Bellard (`test_closure`, `test_language`, `test_builtin`, `test_loop`,
  `test_bigint`, `test_cyclic_import`, `test_worker`) passent avec le moteur modifié.
- Tableaux complets : `C:\temp\qjs-engine-test\bench-table-dispatch.md` et
  `bench-table-final.md`.

## Fichiers touchés

| Fichier | Modifications |
|---|---|
| `quickjs-debug.h` (nouveau) | API publique du debugger et de la collecte |
| `quickjs-internal.h` | `filename`, `function_number`, bitmaps dans `JSFunctionBytecode` ; `JSDebugState` ; `js_debug_frame_has_stops` ; `BC_VERSION` |
| `quickjs-opcode.h` | `OP_debugger` |
| `quickjs.h` | `JS_EVAL_FLAG_NO_RESOLVE` (G) ; `JS_EVAL_FLAG_NO_DYNAMIC_IMPORT`, `JS_GetModuleRequestCount`, `JS_GetModuleRequestName` (J) ; `JS_GetBytecodeVersion` (K) |
| `bytecode/reader.c` (aussi) | `JS_GetBytecodeVersion` (K) |
| `quickjs-debug.h`, `quickjs-internal.h`, `compiler/debug-info.c`, `compiler/parser-expr.c` (aussi) | avertissements de compilation (K) |
| `compiler/eval.c` | pas de résolution des imports avec `JS_EVAL_FLAG_NO_RESOLVE` (G) ; `no_dynamic_import` (J) |
| `compiler/parser-expr.c` (aussi) | refus de `import()` (J) |
| `core/modules.c` (aussi) | lecture des imports d'un module (J) |
| `core/debug.c` (nouveau) | hook, arrêts, frames, emplacements, exceptions |
| `core/core.h` | prototypes `js_debug_*` |
| `core/interpreter.c` | double table, `debug_check`, `OP_debugger`, rafraîchissements, signalement des exceptions |
| `core/errors.c` | `filename` ; `JS_GetException` oublie l'exception signalée |
| `core/runtime.c` | initialisation et libération de `reported_exception` ; `JS_NewContext` sans eval avec `QJSX_NO_COMPILER` (H) |
| `core/gc.c`, `core/iteration.c`, `core/modules.c` | `filename` hors debug ; libération des bitmaps |
| `bytecode/reader.c`, `bytecode/writer.c` | `filename` et `function_number` sérialisés |
| `compiler/compiler.h` | portées avec plage source, nature des `OP_line_num`, champs de collecte |
| `compiler/debug-info.c` (nouveau) | enregistrement des positions, collecte |
| `compiler/emitter.c` | `emit_debug_pos`, plages des portées |
| `compiler/parser-*.c` | marqueurs de position, `OP_debugger`, `emit_return` avec position, fin des fonctions |
| `compiler/resolve-labels.c` | appels terminaux, enregistrement, `code_match` / `find_jump_target` / `skip_dead_code` |
| `compiler/finalize.c` | numéro de fonction, appel de la collecte, `filename` |
| `compiler/funcdef.c` | portée 0, libération des positions |
| `builtins/date.c` | `getTimezoneOffset` sans `tm_gmtoff` avec newlib (I) |
| `core/runtime.c` (aussi) | `js_def_malloc_usable_size` rend 0 avec newlib (I) |
