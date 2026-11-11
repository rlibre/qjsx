# Carte de quickjs.c

> Le découpage réalisé à partir de cette carte est décrit à la fin, section
> [Version découpée](#version-découpée-srcquickjs-split).

Version cartographiée : QuickJS 2026-06-04 de Fabrice Bellard, `engine/qjs-bellard/quickjs.c`.
Le fichier fait 61 197 lignes : 1 562 fonctions, 142 types, 97 tables de données.
Les numéros de ligne renvoient à cette version.

La liste complète des éléments (module, nature, nom, lignes) est dans [quickjs-items.csv](quickjs-items.csv).

## Méthode et limites

- Les éléments de premier niveau (fonctions, types, tables) sont extraits en suivant les accolades,
  après suppression des commentaires et des chaînes.
- Un élément « référence » un autre si son nom apparaît dans son corps : appel, prise d'adresse,
  usage d'un type. Les compteurs du tableau sont des nombres de références, pas d'appels.
- Les références faites à travers une macro ne sont pas vues. Exemple : `short_opcode_info(op)`
  cache un accès à `opcode_info`.
- Les frontières entre modules ont été choisies à la main, en lisant la liste ordonnée des fonctions.
  Un module peut occuper plusieurs plages : le fichier n'est pas toujours rangé par thème.
- `prelude` regroupe les lignes 1 à 1415 : options, structures centrales (`JSRuntime`, `JSContext`,
  `JSObject`, `JSFunctionBytecode`, `JSStackFrame`…) et prototypes. Il est utilisé par tout le monde
  et n'apparaît pas dans les colonnes de dépendances.

## Modules

| Module | Lignes de quickjs.c | Lignes | Fonctions | Dépend surtout de | Utilisé surtout par |
|---|---|---:|---:|---|---|
| prelude | 1-1415 | 1415 | 0 | | builtins/typedarray (105), properties (98), gc (74), modules (64) |
| malloc | 1416-1956 | 541 | 32 | errors (4), gc (1) | gc (27), compiler/passes (24), shapes (21), runtime (20) |
| runtime | 1957-2861 | 905 | 49 | malloc (20), gc (19), call-support (7), builtins/iterator (6) | atoms (13), properties (11), builtins/string (11), modules (9) |
| atoms | 2862-3815 | 954 | 49 | malloc (16), runtime (13), strings (7), conversions (2) | properties (61), compiler/parser (40), modules (26), compiler/passes (20) |
| classes | 3816-3922 | 107 | 5 | atoms (4), malloc (1) | runtime (1), builtins/proxy (1), builtins/promise (1), builtins/weakref (1) |
| strings | 3923-5118 | 1196 | 49 | malloc (11), atoms (8), runtime (7), conversions (5) | builtins/string (81), builtins/regexp (36), builtins/json (20), builtins/uri (20) |
| shapes | 5119-5612 | 494 | 24 | malloc (21), gc (7), atoms (4) | properties (18), gc (8), objects (7), builtins/regexp (5) |
| objects | 5613-6096 | 484 | 26 | properties (7), shapes (7), malloc (6), atoms (5) | call-support (14), builtins/promise (13), builtins/regexp (9), properties (8) |
| gc | 6097-7348 | 1252 | 41 | malloc (27), shapes (8), objects (3), runtime (3) | runtime (19), properties (19), call-support (14), shapes (7) |
| errors | 7349-7885 | 537 | 36 | atoms (6), properties (4), objects (4), gc (2) | builtins/typedarray (40), properties (38), builtins/regexp (21), bytecode-io (20) |
| properties | 7886-11062 | 3177 | 84 | atoms (61), errors (38), gc (19), shapes (18) | builtins/array (57), builtins/object (55), builtins/proxy (52), call-support (36) |
| conversions | 11063-11292, 12763-13691 | 1159 | 45 | errors (7), strings (5), bigint (4), malloc (4) | builtins/string (46), builtins/typedarray (30), builtins/number (26), builtins/regexp (23) |
| bigint | 11293-12762 | 1470 | 52 | malloc (13), errors (4), conversions (1), strings (1) | operators (34), builtins/bigint (6), conversions (4), bytecode-io (3) |
| print | 13692-14555 | 864 | 31 | runtime (4), malloc (3), strings (3), atoms (3) | runtime (4), bytecode-io (4), gc (3), compiler/passes (3) |
| operators | 14556-16109 | 1554 | 37 | bigint (34), conversions (22), properties (8), strings (6) | interpreter (17), builtins/array (8), builtins/typedarray (8), builtins/atomics (6) |
| call-support | 16110-17771 | 1662 | 45 | properties (36), malloc (16), errors (15), objects (14) | interpreter (16), builtins/array (14), builtins/iterator (14), builtins/mapset (14) |
| interpreter | 17772-20775 | 3004 | 10 | properties (26), operators (17), call-support (16), errors (16) | builtins/promise (17), builtins/proxy (14), builtins/array (13), builtins/mapset (12) |
| generators | 20776-21667 | 892 | 31 | malloc (13), errors (11), properties (8), builtins/promise (8) | builtins/promise (8), runtime (3), gc (2), modules (2) |
| compiler/tokenizer | 21668-23155, 23530-23691 | 1650 | 24 | strings (18), atoms (4), malloc (3), conversions (2) | compiler/parser (148), compiler/passes (33), compiler/emitter (24), json-tokenizer (6) |
| json-tokenizer | 23156-23529 | 374 | 4 | compiler/tokenizer (6), strings (5), runtime (3), atoms (1) | builtins/json (3) |
| compiler/emitter | 23692-24357 | 666 | 37 | compiler/tokenizer (24), atoms (6), malloc (6), errors (2) | compiler/parser (189), compiler/passes (17), eval-api (1) |
| compiler/parser | 24358-29546, 31477-31944, 36210-37030 | 6478 | 70 | compiler/emitter (189), compiler/tokenizer (148), atoms (40), properties (10) | eval-api (2), compiler/passes (1), builtins/json (1) |
| modules | 29547-31476 | 1930 | 58 | atoms (26), malloc (19), errors (16), runtime (9) | compiler/parser (8), eval-api (7), gc (2), interpreter (2) |
| compiler/passes | 31945-36209 | 4265 | 47 | compiler/tokenizer (33), malloc (24), atoms (20), compiler/emitter (17) | compiler/parser (5), eval-api (4), gc (1) |
| eval-api | 37031-37261 | 231 | 8 | modules (7), compiler/tokenizer (4), compiler/passes (4), errors (2) | builtins/global (2), interpreter (1), modules (1), builtins/function (1) |
| bytecode-io | 37262-39400 | 2139 | 63 | errors (20), malloc (17), atoms (14), builtins/typedarray (6) | builtins/typedarray (2) |
| cfunc-lists | 39401-39762 | 362 | 12 | properties (6), objects (6), atoms (5), modules (2) | builtins/promise (9), builtins/array (7), intrinsics (5), builtins/mapset (4) |
| builtins/global | 39763-39789, 55947-55954 | 35 | 4 | eval-api (2), conversions (2) | builtins/uri (3), builtins/number (2), runtime (1) |
| builtins/object | 39790-40930 | 1141 | 36 | properties (55), errors (18), atoms (10), objects (8) | builtins/array (25), builtins/reflect (6), intrinsics (5), builtins/typedarray (5) |
| builtins/function | 40931-41323 | 393 | 12 | strings (10), properties (7), call-support (6), interpreter (4) | builtins/array (24), intrinsics (4), interpreter (3), builtins/regexp (3) |
| builtins/error | 41324-41481 | 158 | 4 | properties (4), objects (2), conversions (2), errors (2) | intrinsics (4), builtins/promise (2) |
| builtins/array | 41482-43468 | 1987 | 36 | properties (57), builtins/object (25), builtins/function (24), conversions (19) | builtins/iterator (28), builtins/typedarray (6), builtins/json (4), runtime (2) |
| builtins/iterator | 43469-44587 | 1119 | 21 | builtins/array (28), errors (16), call-support (14), properties (13) | intrinsics (9), runtime (6), builtins/promise (1) |
| builtins/number | 44588-44950 | 363 | 20 | conversions (26), errors (6), strings (3), objects (2) | intrinsics (5), builtins/uri (2), builtins/bigint (1) |
| builtins/string | 44951-46697 | 1747 | 48 | strings (81), conversions (46), errors (12), runtime (11) | builtins/regexp (11), intrinsics (4), runtime (2), compiler/parser (2) |
| builtins/math | 46698-47226 | 529 | 18 | call-support (3), conversions (2), errors (1), operators (1) | intrinsics (2) |
| builtins/date | 47227-47326, 54788-55946 | 1259 | 45 | conversions (13), errors (6), objects (4), interpreter (3) | runtime (1), print (1) |
| builtins/regexp | 47327-49091 | 1765 | 38 | strings (36), conversions (23), errors (21), properties (18) | runtime (4), builtins/string (2), interpreter (1) |
| builtins/json | 49092-50184 | 1093 | 24 | strings (20), properties (18), atoms (12), conversions (9) | runtime (1) |
| builtins/reflect | 50185-50352 | 168 | 8 | properties (10), errors (6), builtins/object (6), atoms (4) | intrinsics (1) |
| builtins/proxy | 50353-51340 | 988 | 25 | properties (52), errors (18), interpreter (14), conversions (7) | runtime (1), operators (1), interpreter (1) |
| builtins/symbol | 51341-51464 | 124 | 7 | atoms (4), errors (3), conversions (2), builtins/string (1) | intrinsics (3) |
| builtins/mapset | 51465-53130 | 1666 | 44 | properties (33), call-support (14), interpreter (12), malloc (11) | builtins/promise (17), builtins/weakref (14), runtime (5), intrinsics (2) |
| builtins/promise | 53131-54492 | 1362 | 43 | properties (24), errors (19), builtins/mapset (17), interpreter (17) | generators (8), modules (7), runtime (1), call-support (1) |
| builtins/uri | 54493-54787 | 295 | 11 | strings (20), runtime (6), conversions (4), builtins/global (3) | intrinsics (1) |
| builtins/bigint | 55955-56151 | 197 | 7 | bigint (6), errors (4), operators (3), conversions (2) | intrinsics (1) |
| intrinsics | 56152-56511 | 360 | 2 | builtins/iterator (9), cfunc-lists (5), objects (5), builtins/object (5) | builtins/typedarray (17), builtins/atomics (5), runtime (2), print (1) |
| builtins/typedarray | 56512-60298 | 3787 | 116 | errors (40), properties (30), conversions (30), malloc (17) | builtins/atomics (22), bytecode-io (6), builtins/array (5), runtime (3) |
| builtins/atomics | 60299-60912 | 614 | 12 | builtins/typedarray (22), errors (6), conversions (6), operators (6) | builtins/weakref (4), runtime (1) |
| builtins/weakref | 60913-fin | 285 | 12 | builtins/mapset (14), properties (9), malloc (7), runtime (6) | gc (2), runtime (1) |

Plus grosses fonctions : `JS_CallInternal` (2 823 lignes, ligne 17772), `resolve_labels` (902),
`js_parse_statement_or_decl` (761), `js_parse_postfix_expr` (667), `js_parse_function_decl2` (567),
`js_parse_class` (511), `resolve_scope_var` (480), `js_parse_destructuring_element` (468),
`next_token` (441), `resolve_variables` (358).

## Le compilateur (retrait de l'eval)

Le compilateur occupe environ 13 300 lignes, soit 22 % du fichier : `compiler/tokenizer`,
`compiler/emitter`, `compiler/parser`, `compiler/passes` et la partie compilation de `eval-api`.

### Il est déjà derrière une indirection

- Le contexte a un pointeur `ctx->eval_internal` (ligne 555).
- Seul `JS_AddIntrinsicEval` (ligne 55947) le remplit avec `__JS_EvalInternal` (ligne 37071),
  qui est l'unique entrée vers le compilateur.
- Tout le reste passe par `JS_EvalInternal` (ligne 37187), qui lève une `TypeError`
  « eval is not supported » si le pointeur est NULL. C'est le cas de `JS_Eval`, `JS_EvalThis`,
  `JS_EvalObject`, de l'eval global (`js_global_eval`), du constructeur `Function`
  (`js_function_constructor`) et de l'eval direct dans l'interpréteur (`JS_CallInternal` →
  `JS_EvalObject`).
- `JS_NewContext` appelle `JS_AddIntrinsicEval`. Une build sans compilateur doit partir de
  `JS_NewContextRaw` et n'ajouter que les intrinsèques voulus.

### Couplages à couper pour sortir le compilateur de la build

1. **JSON.parse utilise l'état du parseur.** `JS_ParseJSON3` utilise `JSParseState`,
   `js_parse_init`, `free_token` et `js_parse_error`. Le tokenizer JSON (`json_parse_*`,
   `json_next_token`, lignes 23156-23529) utilise `ident_realloc`, `js_parse_error`,
   `js_parse_error_pos` et `free_token`. Il faut un petit socle commun « tokenizer » partagé par
   le JSON et le compilateur, ou un état propre au JSON.
2. **`free_function_bytecode`** (ligne 36166) est rangé parmi les passes du compilateur, mais il
   libère du bytecode à l'exécution et le GC l'appelle (`free_gc_object`). Il va côté runtime.
3. **`opcode_info` et `short_opcode_info`** (lignes 22035-22068) sont définis dans la zone du
   compilateur, mais le lecteur et l'écrivain de bytecode s'en servent (`bc_byte_swap`,
   `JS_WriteFunctionBytecode`, `JS_ReadFunctionBytecode`). À partager. Le debugger en aura aussi
   besoin pour parcourir le bytecode.
4. **`js_parse_error`** est appelé par `add_export_entry2` dans le module `modules` (ligne 29680).
5. **`eval-api` mélange deux rôles.** `JS_EvalFunction` / `JS_EvalFunctionInternal` exécutent du
   bytecode déjà compilé (les modules s'en servent) et restent côté runtime. `__JS_EvalInternal`
   et ses appelants relèvent du compilateur.
6. **Les types du compilateur** (`JSFunctionDef`, `JSToken`, `JSParseState`, `BlockEnv`,
   `JSVarDef`…, lignes 21668-22057) forment un header privé du compilateur, sauf ce que le JSON
   garde en commun (point 1).

### Conséquence pour le debugger

`Runtime.evaluate` et `Debugger.evaluateOnCallFrame` ont besoin du compilateur. Décision prise
depuis : aucune build ESP ne contient le compilateur, même en debug. Les expressions sont
compilées sur l'hôte et envoyées à l'ESP en bytecode (voir `doc/architecture.md`).

## Points d'intérêt pour le debugger

- **Interpréteur** : `JS_CallInternal`, lignes 17772-20599. Une seule fonction, c'est là que
  vont les hooks.
- **Frames** : `JSStackFrame` (ligne 407), chaînées depuis `rt->current_stack_frame`.
- **Positions** : `find_line_num` (ligne 7435) décode la table `pc2line`, `build_backtrace`
  (ligne 7538) parcourt les frames. La table est produite à la compilation par
  `compute_pc2line_info` (ligne 34450).
- **Mot-clé `debugger`** : il est toujours ignoré par le parseur (ligne 29507, « currently no
  debugger »).
- **Interruption** : `js_poll_interrupts` (ligne 7877) et `JS_SetInterruptHandler` (ligne 2236).

## Version découpée (engine/qjsx)

`engine/qjsx/` contient le même moteur découpé en 55 fichiers sources. `engine/qjs-bellard/`
reste intact. Chaque fichier indique en tête les lignes de `quickjs.c` dont il provient.
Performances, tailles et temps de compilation : [quickjs-split-perf.md](quickjs-split-perf.md).
Le découpage est produit par le générateur de [tools/quickjs-split](../tools/quickjs-split/README.md),
qui fournit aussi le build de test et la comparaison de performances.

### Fichiers

| Dossier | Rôle | Fichiers (lignes) |
|---|---|---|
| `core/` | toujours compilé | atoms (936), bigint-arith (1470), cfunc (366), closures (761), conversions (1089), errors (552), gc (1296), generators (860), interpreter (3117), intrinsics (363), iteration (890), modules (1934), objects (982), operators (1562), print (872), properties (3177), runtime (1458), strings (1196) |
| `bytecode/` | chargement et sauvegarde du bytecode | opcodes (21), reader (1158), writer (956) |
| `builtins/` | objets standard, un fichier par objet | array (1964), arraybuffer (637), atomics (541), base64 (764), bigint (206), dataview (396), date (1255), function (560), global (329), iterator (1095), json (1577), mapset (1643), math (537), number (372), object (1150), promise (1368), proxy (1165), regexp (1766), string (1753), symbol (133), typedarray (2108), weakref (302) |
| `compiler/` | le compilateur, absent d'une build sans eval | emitter (632), eval (134), finalize (582), funcdef (607), parser-class (686), parser-expr (3140), parser-func (831), parser-module (486), parser-stmt (1346), resolve-labels (1828), resolve-vars (1173), tokenizer (1202) |

Les autres fichiers du moteur (`cutils`, `dtoa`, `libregexp`, `libunicode`, `quickjs.h`…)
sont copiés tels quels à la racine de `engine/qjsx/`.

### Headers

- `quickjs-internal.h`, inclus par tous les fichiers. Dans l'ordre :
  - le prélude de `quickjs.c` sans ses prototypes ;
  - les macros et types partagés entre fichiers ;
  - les headers de prototypes ;
  - les fonctions inline partagées.
- `core/core.h`, `bytecode/bytecode.h`, `builtins/builtins.h` : prototypes des fonctions et
  déclarations `extern` des tables partagées entre fichiers. Inclus par `quickjs-internal.h`.
- `compiler/compiler.h` : types, macros et prototypes privés du compilateur. Seuls les
  fichiers de `compiler/` l'incluent. Aucun autre fichier ne dépend du compilateur, sauf
  `JS_NewContext`, qui appelle `JS_AddIntrinsicEval` de l'API publique.

### Règles du découpage

- **Fonctions et tables** : seules celles utilisées dans plusieurs fichiers ont perdu
  `static` (688 fonctions, 48 tables). Les autres restent `static` dans leur fichier.
- **Taille des tables partagées** : elle est écrite dans leur déclaration `extern`, pour que
  `countof()` fonctionne ailleurs. Si une table change de taille sans mise à jour du header,
  la compilation échoue.
- **Macros définies dans une fonction** : quand elles sont utilisées ailleurs (par exemple
  `JS_THROW_VAR_RO` dans `JS_CallInternal`), une copie identique est placée dans un header.
- **Éléments déplacés hors de leur zone de lignes** :
  - `free_function_bytecode` et `free_bytecode_atoms` vont dans `core/gc.c` ;
  - `opcode_info` va dans `bytecode/opcodes.c` ;
  - `JS_EvalFunction` et la répartition de l'eval vont dans `core/interpreter.c` ;
  - `JS_ResolveModule` va dans `core/modules.c` ;
  - `ident_realloc` va dans `core/strings.c` et `get_line_col` dans `core/errors.c` ;
  - `js_get_prototype_from_ctor` va dans `core/objects.c`.

### Modifications du code par rapport à quickjs.c

1. **JSON.parse a son propre état** (`JSONParseState`, `JSONToken`, `JSON_TOK_*`, dans
   `builtins/json.c`), au lieu de réutiliser celui du parseur JavaScript.
2. **Les exports de module sont séparés du parseur** :
   - `add_export_entry2` (`core/modules.c`) ne reçoit plus de `JSParseState` et lève une
     `SyntaxError` ordinaire ;
   - `add_export_entry` (`compiler/parser-module.c`) signale le doublon avec la position
     dans le source, puis l'appelle.

Rien d'autre n'a changé dans le code des fonctions.
