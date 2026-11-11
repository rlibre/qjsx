# Règles d'écriture du code C / C++

Règles établies sur le projet espjs (novembre 2026), écrites pour être reprises telles quelles
dans un autre projet en C ou en C++. Elles servent aussi de consignes pour un assistant de
code : on peut copier ce fichier dans le projet et y faire référence depuis son `CLAUDE.md`.

**Principe : la lisibilité et la maintenabilité passent avant tout.** Le code se lit plus
souvent qu'il ne s'écrit. Chaque règle ci-dessous sert ce but : voir d'un coup d'œil ce que
fait une fonction, ce qu'elle modifie, et dans quel ordre.

Sommaire :

1. [Écriture du code](#1-écriture-du-code)
2. [Mise en page](#2-mise-en-page)
3. [Commentaires](#3-commentaires)
4. [Paramètres et types](#4-paramètres-et-types)
5. [Nommage](#5-nommage)
6. [Mémoire et pile](#6-mémoire-et-pile)
7. [Contrôles](#7-contrôles)
8. [Résumé](#8-résumé)

---

## 1. Écriture du code

- **Une idée par ligne.** Un calcul compliqué se découpe en étapes nommées, avec des variables
  intermédiaires dont le nom dit ce qu'elles contiennent.

  ```c
  // non
  if (fields->integer(KEY_SIZE, &size) && size > 0 && size < limit && !uploading) { ... }

  // oui
  bool sizeValid = fields->integer(KEY_SIZE, &size) && size > 0 && size < limit;
  bool canStart = sizeValid && !uploading;
  if (canStart) {
      ...
  }
  ```

- **Une condition qui a un sens reçoit un nom** : `bool isDot = ...;` puis `if (isDot)`. On évite
  la condition en ligne qui oblige à décoder.
- **Pas d'astuce sans commentaire.** Un calcul de bits, une conversion inhabituelle ou un
  comportement qui dépend d'un détail de la bibliothèque s'expliquent sur la ligne d'au-dessus.
- **Pas de duplication.** Un bout de code écrit deux fois devient une fonction.
- **Pas d'opérateur ternaire imbriqué.** Un ternaire simple est accepté. Dès qu'il contient un
  appel avec des effets de bord, ou qu'il fait plus d'une ligne, on écrit un `if`.

## 2. Mise en page

### Accolades

- **L'accolade ouvrante est sur la ligne de la signature ou de l'instruction** :

  ```c
  int Loop::run() {
      ...
  }
  ```

- **Accolades systématiques et pas de code sur une ligne.** Le corps d'un `if`, d'une boucle ou
  d'une fonction est toujours entre accolades et sur ses propres lignes, même s'il ne contient
  qu'une instruction.

  ```c
  // non
  if (done) return;
  if (done) { return; }
  int size() const { return _size; }

  // oui
  if (done) {
      return;
  }

  int size() const {
      return _size;
  }
  ```

- **Chaque `case` d'un `switch` a son bloc entre accolades**, avec le `break` à l'intérieur, et
  les `case` sont indentés d'un niveau sous le `switch`. Une étiquette sans instructions, qui
  « tombe » sur la suivante, reste nue.

  ```c
  switch (kind) {
      case KIND_INFO: {
          handleInfo(id);
          break;
      }
      case KIND_FLOAT16:
      case KIND_FLOAT32: {
          readFloat();
          break;
      }
      default: {
          respondError(id, "unknown request");
          break;
      }
  }
  ```

### Lignes vides : une fonction se lit par étapes

Le corps d'une fonction se découpe en **groupes**, un par étape : préparer, convertir, ouvrir,
vérifier, boucler, fermer… Une ligne vide sépare deux groupes.

- **Une vérification reste collée à l'instruction dont elle teste le résultat.**
- **Une ligne vide suit un bloc fermé** (`}`), y compris `} while (...);`, sauf si la suite est
  `}`, `else` ou `catch`.
- **Une ligne vide précède une boucle** qui suit d'autres instructions.
- **Jamais deux lignes vides de suite.**

```c
bool listDirectory(const char *path, DirectoryVisitor *visit, void *opaque) {
    size_t length = strlen(path);
    char *pattern = (char *)malloc(length + 3);
    snprintf(pattern, length + 3, "%s/*", path);

    wchar_t *wide = widen(pattern);
    free(pattern);
    if (wide == nullptr) {
        return false;
    }

    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(wide, &entry);
    free(wide);

    if (search == INVALID_HANDLE_VALUE) {
        setErrnoFromWin32();
        return false;
    }

    bool ok = true;

    do {
        bool isDot = wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0;
        if (isDot) {
            continue;
        }

        char *name = narrow(entry.cFileName);
        ok = visit(opaque, name);
        free(name);
    } while (ok && FindNextFileW(search, &entry));

    FindClose(search);
    return ok;
}
```

Les méthodes d'une classe sur plusieurs lignes sont aussi séparées par une ligne vide.

### Alignement dans les en-têtes

Dans une classe ou une structure d'un `.h`, les noms des membres et des méthodes sont alignés
sur une colonne. L'étoile ou l'esperluette d'un pointeur ou d'une référence se place juste
avant le nom, en débordant à gauche de la colonne. Les types très longs (par exemple les
templates) ne repoussent pas la colonne : ils gardent une seule espace.

```c
class Mailbox {
public:
    void             retain();
    void             release();
    Message         *take(int64_t timeoutMs);
    bool             empty();

private:
    platform::Signal _signal;
    Message         *_first;
    Message         *_last;
    int              _references;
};
```

## 3. Commentaires

### Un commentaire sur chaque fonction

Chaque fonction et chaque méthode a un bloc `/** ... */` juste au-dessus d'elle :
- dans le `.h`, au-dessus de la déclaration ;
- dans le `.cpp`, au-dessus de la définition, y compris pour les fonctions `static`, les
  fonctions d'un namespace anonyme, les constructeurs et les destructeurs.

Ce bloc dit **ce que fait** la fonction, en une ou deux lignes. Il ne raconte pas comment elle
le fait.

```c
/**
 * HELLO: opens the session.
 */
void Agent::handleHello(int64_t id, const Fields *fields) {
```

Dans un en-tête, on ajoute ce que l'appelant doit savoir : qui possède la mémoire rendue,
quel thread peut appeler la fonction, ce que vaut le retour en cas d'erreur.

**Paramètres et valeur de retour : seulement s'ils ne sont pas évidents.** Pas de liste
systématique de `@param` / `@return`. On décrit un paramètre quand son nom et son type ne
suffisent pas (unité, plage, `nullptr` permis, qui le possède, ce qu'une sortie contient en cas
d'échec), et le retour quand il n'est pas évident (sens d'un `false`, d'un `-1`, d'un
`nullptr`).

```c
/**
 * Receives at most RECEIVE_BLOCK bytes into a new block given to the caller in
 * *data (null unless the count is > 0). Returns the byte count, 0 at the end of
 * the stream, < 0 on error.
 */
int receiveBlock(Socket socket, uint8_t **data);
```

### Le déroulement va dans le code, en pseudo-code

Le « comment » s'écrit **dans le corps de la fonction**, à la manière d'un pseudo-code : un
commentaire court au-dessus de chaque groupe d'instructions (voir les lignes vides). Lus à la
suite, ces commentaires racontent l'algorithme.

```c
void Agent::onMessage(void *opaque, const uint8_t *message, size_t size) {
    Agent *agent = (Agent *)opaque;

    // decode [kind, id, payload]; anything else is not ours: ignored
    cbor::Reader reader(message, size);
    ...

    // same id as the last answered request: the host did not get the answer
    ...

    // then send the remembered answer again, without doing the request again
    ...

    // any other request: answered here, now
    agent->handle(kind, id, &fields);
}
```

- **On commente le pourquoi et le non-évident.** Un commentaire ne répète pas le code (pas de
  `// increment i` au-dessus de `i++`).
- **On documente ce qui ne se voit pas** : le thread qui exécute le code, l'ordre imposé entre
  deux opérations et sa raison, qui libère quoi, ce qui se passe en cas d'échec.
- **Un commentaire au-dessus de chaque groupe**, même quand il paraît évident : lus à la
  suite, ils doivent raconter toute la fonction, sans trou.
- **Une petite fonction sans étapes** n'a besoin que de son bloc d'en-tête.

### Membres des structures et des classes

Chaque membre (donnée) d'une structure ou d'une classe a un commentaire `//` court en fin de
ligne. Il dit ce qu'il contient, son unité, qui possède la mémoire, quel thread ou quel verrou
le protège. Les commentaires de membres consécutifs sont alignés sur une colonne. Si le
commentaire ne tient pas sur la ligne, il passe sur une ligne `//` au-dessus du membre.

```c
struct Timer {
    int64_t   deadline;   // ms, monotonic clock
    int64_t   interval;   // ms; 0 for a one-shot timer
    JSValue   callback;   // owned
    Timer    *next;       // next timer by deadline
};
```

### Commentaires de section

Pour découper un long fichier en parties, on utilise une ligne de dièses, plus visible qu'un
trait. Elle porte le titre de la partie, ou bien elle est seule pour un simple trait. On la
complète jusqu'à la colonne 80. On n'utilise pas un bloc `/** */`, réservé aux fonctions.

```c
// ### Sending ##################################################################

// ##############################################################################
```

## 4. Paramètres et types

- **Pas de référence dans les API**, ni `T &` ni `const T &`. Elles cachent, à l'appel, qu'une
  variable peut être modifiée. À la place :
  - **en entrée**, une petite structure se passe par valeur (copiée : la fonction ne peut pas
    la modifier), une grosse par `const T *` ;
  - **en sortie**, on passe un pointeur explicite `T *out` que la fonction remplit. Le `&` qu'on
    voit à l'appel montre que la variable va changer.

  ```c
  board::Identity identity;
  board::identity(&identity);              // remplit identity : le & le montre

  bool startThread(ThreadFunction *run, void *argument, ThreadOptions options);   // copie
  ```

  Exception possible : les templates internes d'une bibliothèque générique peuvent garder leurs
  `const T &` pour éviter des copies, tant que l'API visible par l'utilisateur respecte la
  règle.

- **Casts à la C**, `(T)value`, plutôt que `static_cast<T>(value)`, `reinterpret_cast` ou
  `const_cast`. Attention à la précédence : quand un membre, un indice ou un appel suit, on
  met le cast entre parenthèses.

  ```c
  ((Runner *)opaque)->run();       // oui : on caste opaque, puis on appelle run()
  (Runner *)opaque->run();         // NON : C++ lit (Runner *)(opaque->run())
  ```

  On ne met pas de parenthèses inutiles : `(char *)malloc(size)`, et non
  `(char *)(malloc(size))`.

- **Temps et compteurs** : un temps en millisecondes et un délai sont signés sur 64 bits
  (`int64_t`). Une échéance moins l'heure courante donne un délai négatif pour un timer déjà
  échu, et 64 bits ne débordent pas en pratique. On n'utilise pas de compteur 32 bits qui
  repart à zéro, comme les ticks d'un RTOS.

## 5. Nommage

- **Membres privés** : ils commencent par un souligné (`_state`, `_firstRequest`), et non
  `state_`. Exception : les préfixes collés par une macro (`jsNewModule_##name`).
- **Types et classes** en `PascalCase`, **fonctions, méthodes et variables** en `camelCase`,
  **constantes et valeurs d'énumération C** en `MAJUSCULES_SOULIGNÉES` (`KIND_HELLO`,
  `DEFAULT_STACK`), **valeurs d'un `enum class`** en `PascalCase` (`StepMode::Over`).
- **Des noms qui disent ce qu'ils contiennent**, sans abréviation obscure : `received`,
  `packageSize`, `isDebugRequest`, et non `n`, `sz` ou `flag`. Les compteurs de boucle courts
  (`i`, `j`) sont acceptés.
- **Les booléens se lisent comme une question** : `isOpen`, `hasWork`, `stillCurrent`,
  `wanted`.

## 6. Mémoire et pile

- **La pile est petite** sur un système embarqué ou sous RTOS : quelques Ko par tâche. On ne met
  pas de tableau de plus de 256 octets environ sur la pile. Un tampon d'entrée-sortie va sur le
  tas.
- **Les threads d'entrée-sortie lisent directement dans le bloc alloué qu'ils transmettent**
  (à une file, à une boucle d'événements), réduit ensuite à la taille reçue avec `realloc`. On
  évite ainsi un tampon sur la pile et une copie. Si les octets sont consommés tout de suite,
  on utilise un tampon alloué une seule fois au démarrage du thread.
- **Toutes les allocations passent par un module unique** (par exemple `memory::allocate`,
  `reallocate`, `release`, `copyText`), jamais par `malloc` / `realloc` / `free` directement.
  Un manque de mémoire y est fatal : message puis arrêt (abort sur PC, redémarrage sur cible).
  Ces fonctions ne rendent donc jamais `nullptr` et l'appelant ne teste pas le résultat. Ce
  point d'entrée unique permet aussi de compter ou de tracer les allocations (profilage). Le
  tas propre à une bibliothèque (celui du moteur JS, par exemple) garde son allocateur.
- **La possession de la mémoire est écrite** dans le commentaire de la fonction : « malloc'd,
  freed by the caller », « borrowed », « the loop owns it ».

## 7. Contrôles

Une partie de ces règles est mécanique et peut être appliquée ou vérifiée par un outil (un
script, un formateur configuré, une revue automatique) :

- remonter l'accolade d'une fonction sur la ligne de sa signature ;
- éclater le code écrit sur une ligne et ajouter les accolades manquantes, `case` compris ;
- indenter les `case` sous leur `switch` ;
- aligner les noms des membres dans les en-têtes ;
- ajouter une ligne vide après un bloc fermé et avant une boucle ;
- convertir les casts C++ en casts à la C ;
- lister les fonctions sans bloc `/** */`.

Le reste ne s'automatise pas : regrouper les lignes par étapes logiques et écrire le
pseudo-code se font à la main, en relisant.

Un outil de mise en forme ne doit jamais toucher au code d'une bibliothèque tierce, ni au
corps des macros.

**Vérifier qu'une passe de mise en forme n'a pas changé le code** : on compare les deux versions
débarrassées de leurs commentaires et de leurs lignes vides (et des accolades, si la passe en a
ajouté). Elles doivent être identiques. Par exemple :

```sh
strip() { perl -0pe 's{/\*.*?\*/}{}gs; s{//[^\n]*}{}g' "$1" | grep -v '^\s*$' | tr -d '{} \t\n'; }
diff <(strip avant.cpp) <(strip apres.cpp)
```

Le résultat doit être vide.

## 8. Résumé

| Sujet | Règle |
|---|---|
| Principe | lisibilité et maintenabilité d'abord ; une idée par ligne ; étapes nommées ; pas de duplication |
| Accolades | sur la ligne de la signature ; toujours présentes ; jamais de code sur une ligne ; chaque `case` entre accolades, indenté sous son `switch` |
| Lignes vides | un groupe par étape ; après un bloc fermé ; avant une boucle ; jamais deux de suite |
| En-têtes | noms des membres et méthodes alignés sur une colonne |
| Commentaires | `/** */` court (le quoi) sur chaque fonction ; le déroulement en pseudo-code dans le corps |
| Paramètres | pas de références ; entrée par valeur ou `const T *` ; sortie par `T *out` |
| Casts | `(T)x` ; `((T *)p)->m` quand un membre suit |
| Nommage | membres privés `_nom` ; booléens en question ; noms explicites |
| Pile | pas de tableau de plus de ~256 octets ; tampons d'entrée-sortie sur le tas |
| Temps | `int64_t` signé en millisecondes, jamais un compteur 32 bits |
