# Known defects (not fixed)

Recorded on 2026-11-11 by the agents that documented the code, then during the fixes, sorted
by severity. There is no open "serious" defect any more. The fixed defects are in
`history.md`, with what was done. The "minor" ones await the user's agreement.

## Medium

- **Fixes not compiled on POSIX / macOS**: the POSIX / macOS branches of the fixes of the
  "medium" defects were only reread, not compiled (`pthread_condattr_setclock`, `poll`,
  `fseeko`, `MSG_NOSIGNAL`...).

### Recorded during the fixes (2026-11-11)

- **Agent**: the last answers are only forgotten by `attach()`. On a serial link, a new host
  sends HELLO without `attach()`: a request whose number is the one of the last answer of the
  previous host (HELLO number 1) receives this old answer.
  Lead: do not treat HELLO as a repetition and forget both answers in `handleHello`.
- **Debugger**: only one last answer is kept; with a burst of requests, the repetition of a
  request whose answer was replaced is executed again.
- **`objectClassName`**: `ownDataProperty` on the prototype and on `constructor` triggers
  the `getOwnPropertyDescriptor` trap of a Proxy (script code runs, with no pause possible
  from now on).
- **BigInt**: `strtoll` accepts leading spaces and a `+`.
- **`net`**:
  - MESSAGE_ACCEPTED / MESSAGE_CONNECTED carry a socket: if the message is dropped (source
    removed, mailbox closed at shutdown), the socket leaks;
  - a `PendingConnect` (which holds JS values) is never freed if the program ends with
    `exit()` during a connection.
- **Desktop agent, `readerThread`**: it relies on `shutdown()` to wake up a blocked `recv()`
  when a new host replaces the old one; otherwise the old thread waits for its host to
  disconnect.
- **`Signal`** is recursive on Windows (CRITICAL_SECTION), not on POSIX.
- **`readAll`** returns EIO when measuring the size fails, instead of the errno of the `seek`.
- **`fseeko` / `ftello`** used on everything that is not Windows: to be checked in the
  newlib of ESP-IDF.
- **jsbind**:
  - `makeError` (used by `Resolver::reject`) throws then takes the exception back: an
    exception already pending is lost;
  - `ClassId<T>::id` and `JS_NewClassID` are global: not safe if two threads register
    classes at the same time;
  - `Convert<Range<T,Lo,Hi>>` compares with `(T)Lo`: a negative `Lo` with an unsigned `T`
    overflows.

### Recorded while checking new-device.md (2026-11-11)

- **ESP32, answer to `RESET`**: the agent hands the answer to `UartLink::send`, then
  `board::restart()` calls `esp_restart()` at once. `uart_write_bytes` only copies the frame
  into the transmit buffer of the driver: the chip may restart before it leaves, and the host
  then waits for an answer that never comes. Lead: wait for the end of the transmission
  (`uart_wait_tx_done`) before `esp_restart()`.

## Minor and differences with Node or the web

- **Desktop `startLinks()`** (`host/platform/board_host.cpp`): when the accept thread cannot
  start, it returns false without printing why, against the contract of `board.h`.
- **ESP32 `writeConsole()`** (`ports/esp32/main/board_esp32.cpp`): compares `stream` with the
  literal `2` instead of `rt::OUTPUT_STDERR`, as the desktop board does.
- **Agent upload hash** (`common/agent/agent.cpp`): allocated with `new` / `delete`, outside
  `memory::allocate` / `release` (the allocation rule only names malloc / realloc / free: to
  decide).

- **`TextDecoder`**: it does not remove the BOM, does not handle `fatal` nor `ignoreBOM`, and
  rejects an encoding name surrounded by spaces.
- **`path`**:
  - `relative` is case-sensitive on Windows;
  - `resolve("C:foo")` (relative to a drive) is resolved wrongly.
- **`process.cwd()`** returns `/` on Windows: intended difference (decision of 2026-11-11,
  paths with `/`), noted here for the record.
- **`manifestMain`** (formerly `readManifestMain`): if `JS_ToCString` fails, the message displayed is misleading.
- **POSIX `serial`**:
  - `cfmakeraw` does not reset `CSTOPB` nor `CRTSCTS` to zero;
  - `tcgetattr` is not checked;
  - an `EINTR` closes the port.
- **Windows `listPorts`**: `ERROR_MORE_DATA` stops the list.
- **`Reader::integer`** (CBOR): it returns the values above INT64_MAX as negative.
- **`Fields`**: it silently ignores the pairs beyond 16.
- **`encoded()` on an empty content**: `memcpy(dst, nullptr, 0)`, formally undefined.
