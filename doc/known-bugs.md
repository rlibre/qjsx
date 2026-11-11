# Défauts connus (non corrigés)

Relevés le 2026-11-11 par les agents qui ont documenté le code, puis pendant les corrections,
classés par gravité. Il n'y a plus de défaut « grave » ouvert. Les défauts corrigés sont dans
`history.md`, avec ce qui a été fait. Les « mineurs » attendent l'accord de l'utilisateur.

## Moyens

- **Pas-à-pas over / out** (travail du debugger à part) : ils comparent la profondeur de pile.
  Après un `await`, ou dans un autre callback, ils peuvent s'arrêter dans du code sans
  rapport. Conception convenue : frames comparés par adresse de pile C, ceux des générateurs
  et des fonctions async par leur objet d'état.
- **Corrections non compilées sur POSIX / macOS** : les branches POSIX / macOS des corrections
  des « moyens » n'ont été que relues, pas compilées (`pthread_condattr_setclock`, `poll`,
  `fseeko`, `MSG_NOSIGNAL`...).

### Relevés pendant les corrections (2026-11-11)

- **Agent** : les dernières réponses ne sont oubliées que par `attach()`. Sur une liaison
  série, un nouvel hôte envoie HELLO sans `attach()` : une requête dont le numéro est celui de
  la dernière réponse de l'hôte précédent (HELLO numéro 1) reçoit cette ancienne réponse.
  Piste : ne pas traiter HELLO comme une répétition et oublier les deux réponses dans
  `handleHello`.
- **Debugger** : une seule dernière réponse gardée ; avec des requêtes en rafale, la
  répétition d'une requête dont la réponse a été remplacée est exécutée de nouveau.
- **`objectClassName`** : `ownDataProperty` sur le prototype et sur `constructor` déclenche
  le piège `getOwnPropertyDescriptor` d'un Proxy (du code du script tourne, sans pause
  possible désormais).
- **BigInt** : `strtoll` accepte des espaces en tête et un `+`.
- **`net`** :
  - MESSAGE_ACCEPTED / MESSAGE_CONNECTED portent une socket : si le message est jeté (source
    retirée, mailbox fermée à l'arrêt), la socket fuit ;
  - un `PendingConnect` (qui tient des valeurs JS) n'est jamais libéré si le programme se
    termine par `exit()` pendant une connexion.
- **Agent de bureau, `readerThread`** : il compte sur `shutdown()` pour réveiller un `recv()`
  bloqué quand un nouvel hôte remplace l'ancien ; sinon l'ancien thread attend que son hôte
  se déconnecte.
- **`Signal`** est récursif sous Windows (CRITICAL_SECTION), pas sous POSIX.
- **`readAll`** rend EIO quand la mesure de la taille échoue, au lieu de l'errno du `seek`.
- **`fseeko` / `ftello`** utilisés sur tout ce qui n'est pas Windows : à vérifier dans la
  newlib d'ESP-IDF.
- **jsbind** :
  - `makeError` (utilisé par `Resolver::reject`) lève puis reprend l'exception : une exception
    déjà en attente est perdue ;
  - `ClassId<T>::id` et `JS_NewClassID` sont globaux : pas sûrs si deux threads enregistrent
    des classes en même temps ;
  - `Convert<Range<T,Lo,Hi>>` compare avec `(T)Lo` : un `Lo` négatif avec un `T` non signé
    déborde.

## Mineurs et écarts avec Node ou le web

- **`TextDecoder`** : il ne retire pas le BOM, ne gère pas `fatal` ni `ignoreBOM`, et refuse un
  nom d'encodage entouré d'espaces.
- **`path`** :
  - `relative` est sensible à la casse sous Windows ;
  - `resolve("C:foo")` (relatif à un lecteur) est mal résolu.
- **`process.cwd()`** rend des `/` sous Windows : écart voulu (décision du 2026-11-11, chemins
  en `/`), noté ici pour mémoire.
- **`manifestMain`** (ex-`readManifestMain`) : si `JS_ToCString` échoue, le message affiché est trompeur.
- **POSIX `serial`** :
  - `cfmakeraw` ne remet pas `CSTOPB` ni `CRTSCTS` à zéro ;
  - `tcgetattr` n'est pas vérifié ;
  - un `EINTR` ferme le port.
- **Windows `listPorts`** : `ERROR_MORE_DATA` arrête la liste.
- **`Reader::integer`** (CBOR) : il rend négatives les valeurs au-dessus d'INT64_MAX.
- **`Fields`** : il ignore en silence les paires au-delà de 16.
- **`encoded()` sur un contenu vide** : `memcpy(dst, nullptr, 0)`, formellement indéfini.
