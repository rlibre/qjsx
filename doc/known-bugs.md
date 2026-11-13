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

### To verify: a debugger test failed once after `deploy --reset` (2026-11-12)

On the ESP32-S3, `tests/debugger.test.js` passed 71/72 when run right after a
`qjsx deploy --reset` (the check that failed was not kept), then 72/72 in the next three
runs. Possibly the program started at boot by the reset still running when the test
began; not investigated.

### RP2350 port (2026-11-12)

Flashed and tried on the Olimex PICO2-XXL on 2026-11-12: the USB CDC link, deploy (flash erase
and program through `flash_safe_execute`), run in place, the PSRAM (a 4 MB buffer written and
read back) and the debugger tests (72 / 30 / 53) work. Open:

- **Speed** (measured 2026-11-12, program `C:\temp\qjsx-speed`, best of 3, in ms): JS heap in
  SRAM or PSRAM (CMake option `QJSX_JS_HEAP_IN_SRAM`), the engine all in flash; then, JS
  heap in PSRAM, the interpreter loop (`JS_CallInternal`, 25 KB) in SRAM, then 30 more hot
  functions of the engine (`QJSX_SRAM_FUNCTIONS` in `ports/rp2350/CMakeLists.txt`):

  | Test | heap SRAM | heap PSRAM | + interpreter in SRAM | + 30 functions | + `-fira-region=one` |
  |---|---|---|---|---|---|
  | loop of 200,000 turns (integers) | 588 | 626 | 538 | 539 | 484 |
  | 100,000 function calls | 510 | 1133 | 467 | 467 | 441 |
  | 200,000 property reads | 740 | 781 | 673 | 673 | 600 |
  | 20,000 small objects | 524 | 912 | 805 | 496 | 489 |
  | 5,000 string concatenations | 297 | 369 | 155 | 100 | 97 |
  | 100,000 Uint8Array writes | 337 | 360 | 323 | 323 | 291 |
  | 50,000 Math.sin | 667 | 1207 | 427 | 427 | 405 |

  `-fira-region=one` (interpreter.c only): GCC otherwise keeps the bytecode pointer `pc`
  of `JS_CallInternal` in the stack frame, loaded and stored by every opcode (upstream
  QuickJS compiles the same way: not caused by the debugger changes).

  Profile on the PC (2026-11-12, `bash tools/profile/profile.sh`: a sampling thread
  suspends the JS thread 10,000 times a second, module `profile_test` of qjsx-tests;
  inlined functions make the attribution approximate):
  - **`let` costs 30 %** in a simple loop (363 ms against 251 ms with `var`, the same
    loop): the compiler emits `get_loc_check` / `put_loc_check` (temporal dead zone
    check) even where the variable is surely initialized. Lead: the host compiler emits
    the plain opcodes when it can prove the initialization; the devices would gain it
    with no change of the interpreter or of the bytecode format;
  - calls, ns per call on the PC: JS function 15, plain C function 15, jsbind function 14,
    jsbind method 32 (the method looked up on the prototype, `this` checked, and on
    Windows the emulated `thread_local` of jsbind: `__emutls_get_address`, cheap on the
    devices), jsbind method with two integers 45;
  - object literals: about 18 % in defining their properties one by one (shape
    transitions), 12 % in malloc / free;
  - property reads: `find_own_property` 4 % only, the rest in the interpreter loop.

  Profile of the heavier workload (2026-11-12, `bash tools/profile/profile.sh Release 14
  tools/profile/workload-profiled.js`, share of the samples of each part):
  - the interpreter loop: 28 to 43 % (all but JSON);
  - property lookups (`find_own_property` + `JS_GetPropertyInternal`): 14 % (expressions),
    12 % (sorting and paths), 16 % (ray tracer), 8 % (strings): the largest single cost
    after the loop, in the code written with classes and objects;
  - allocation (`malloc` / `free` of the C library): 5 to 8 % where objects are created
    (on the devices, the JS heap is a `RegionHeap`, not this malloc);
  - string comparisons and atoms (`js_strict_eq2`, `__JS_NewAtom`): 12 % of the
    expression parser (`token.kind === "operator"`, `OPERATIONS[node.operator]`);
  - JSON: spread over its own functions, nothing dominant;
  - `js_debug_frame_has_stops` (the debugger's choice of dispatch table at each call):
    1 % of the call-heavy parts.

  Experiment of a property cache (2026-11-12, on the PC, engine built with
  `-DQJSX_PROPERTY_CACHE_SIZE=X`): a table of the last lookups of `OP_get_field`, by (shape,
  name), for properties of the object or of its prototype, validated at each use (shape
  number renewed on any change in place). 92.8 million lookups in the workload:

  | Entries | RAM (32 bits) | lookups found | workload total (3 runs, ms) |
  |---|---|---|---|
  | none | — | — | 1030, 1017, 1011 |
  | 16 | ~0.3 KB | 66 % | 1037, 1026, 1023 |
  | 64 | ~1.3 KB | 76 % | 1031, 1023, 1039 |
  | 256 | ~5 KB | 82.6 % | 1040, 1026, 1025 |
  | 1024 | ~20 KB | 82.6 % | 1026, 1009, 1010 |

  No gain beyond the noise, whatever the size: QuickJS's own lookup of a property of the
  object or of its prototype (a hash table per shape, one or two probes) costs about what
  checking the cache costs. The 17 % never found are lookups the cache does not keep
  (deeper prototypes, getters, `length` of arrays). On a board (PSRAM behind a 16 KB
  cache) the balance might differ: not measured. The code was removed from the engine
  (user, 2026-11-12): five engine files for no gain.

  The interpreter loop is larger than the 16 KB XIP cache, which the PSRAM shares: from the
  flash, it evicted itself on every turn. With the hot code in SRAM, the PSRAM heap is now
  faster than the SRAM heap with the code in flash. What is left (about 2.7 us, some 400
  clocks at 150 MHz per turn of a simple loop) is the interpreter itself on a 150 MHz
  in-order core, with 64-bit values on a 32-bit core. Leads, not tried: a faster system
  clock (the pico-sdk has no 200 MHz preset for the RP2350), the interpreter itself. Not
  measured against the ESP32-S3 with the same program yet.

- **USB link and chip reset**: the USB port goes away when the chip restarts (`qjsx device
  reset`, RESET button, fatal error). `qjsx deploy --reset` waits for the device and opens
  it again (`Device.reopen`, 2026-11-12); `qjsx device reset`, `qjsx monitor` and a debug
  session do not: they lose the device. With `deploy --reset --monitor`, the output of the
  program before the session is opened again goes to the device's console, not to the
  tool (on a native USB link, it is lost).
- **Flash erase**: one 4 KB sector per `flash_safe_execute` call, 45 ms or so each with the
  interrupts masked on one core and the other core held: a large package takes a few
  seconds to erase, the JS task is stopped meanwhile.

### To verify: `device/gpio` `waitFor` (2026-11-11)

- On the YD-ESP32-S3 board, `waitFor(0, { timeout: 15000 })` on the BOOT button (GPIO0) timed
  out in every run of the trial program (`C:\temp\qjsx-button`), while `onChange` saw every
  press and the timeout itself works (3 s wait rejected after 3002 ms). Not proven: the
  presses may have fallen outside the 15 s window. Differences with `onChange`: the wait
  watches the pin without debounce, and it is the first watch of the pin since boot. Next
  step: three `waitFor(0)` without timeout, printing each press.

### Found while writing the typings (2026-11-13)

Read in the code by the agents that wrote the `.d.ts` files; the typings document the
behaviour as it is. Nothing fixed.

- **Error codes are uneven**: `Adc` and `Pwm` throw with a `code` (EINVAL, EBUSY);
  the constructors of `Gpio`, `Rmt`, `I2c` throw plain `Error`s (a bus already open is
  not EBUSY, nor "no free RMT channel"); "usb: busy", "usb: the device is not started",
  "Serial: port closed", the rejections of `gpio.waitFor` (timeout, close) and of a
  close while an RMT / I2C operation is pending carry no `code`.
- **Errors thrown instead of a rejected promise**: "closed", "already pending", "a send
  is already in progress", "too many transfers pending" are thrown at once by methods
  that return a promise. `bus.probe()` rejects (ETIMEDOUT, EIO), unlike the comment of
  i2c_module.cpp ("never throws").
- **Writable fields**: `Adc.bits`, `Pwm.duty`, `Pwm.frequency`, `Pwm.bits` are not
  `const`: JS may write them, and a written `pwm.frequency` changes what
  `setPulseWidth()` computes.
- **`rmt.send`**: a fractional tick count or level (1.5) is truncated, not refused.
- **No ENOSYS in jsbind's errno names**: on a board without file system, `sys/fs` errors
  carry `code: "EUNKNOWN"`, not "ENOSYS" as CLAUDE.md says.
- **Node differences**: `existsSync` throws EACCES without read permission (Node never
  throws); `process` and `console` properties are not enumerable; `TextDecoder` ignores
  `fatal` / `ignoreBOM`; `net.connect` throws EACCES at once (not a rejection), an unknown
  host is ENOENT (Node: ENOTFOUND); `path.basename(p, ext)` does not check `ext`;
  `path.resolve` on Windows keeps "/a" without drive and joins "C:a" as a name;
  `path.relative` compares case on Windows.
- **Every jsbind class can be built from JS** (`new Stats()`, `new Connection()`...):
  empty, useless objects.
- **`isOpen()` stays true after `close()`** (net Connection, Serial) until the close
  message is handled, while `write()` already throws.
- **Callbacks cannot be removed**: `onData(null)` / `onClose(undefined)` throw a
  TypeError; a callback can only be replaced.
- **`stdlib/rgbled`**: a `type` such as "toString" finds a key of the prototype of the
  table of models (no RangeError); `this.model` is shared by every strip of a type.
- **`stdlib/usb`**: a `UsbSerial` whose `start()` fails stays the existing Usb until
  collected, so a new try throws "usb: busy"; `Usb.close()` of an object never started
  does not stop a device left started by a previous program.
- **Docs**: the function numbers of the compiler follow the order in which functions are
  finished (children first), not "creation order" (CLAUDE.md); the header of
  `common/runtime/permissions.h` says a device program has no limit, but the desktop
  agent and the twin confine files (`confineFiles`).

## Minor and differences with Node or the web

The local fixes of this section were made on 2026-11-11 (`history.md`); the ones left
need a decision of the user.

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
