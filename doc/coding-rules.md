# C / C++ coding rules

Rules established on the qjsx project (November 2026), written to be reused as they are in
another C or C++ project. They also serve as instructions for a coding assistant: this file
can be copied into the project and referenced from its `CLAUDE.md`.

**Principle: readability and maintainability come first.** Code is read more often than it is
written. Every rule below serves this goal: seeing at a glance what a function does, what it
modifies, and in what order.

Contents:

1. [Writing the code](#1-writing-the-code)
2. [Layout](#2-layout)
3. [Comments](#3-comments)
4. [Parameters and types](#4-parameters-and-types)
5. [Naming](#5-naming)
6. [Memory and stack](#6-memory-and-stack)
7. [Checks](#7-checks)
8. [Summary](#8-summary)

---

## 1. Writing the code

- **One idea per line.** A complicated computation is split into named steps, with
  intermediate variables whose names say what they hold.

  ```c
  // no
  if (fields->integer(KEY_SIZE, &size) && size > 0 && size < limit && !uploading) { ... }

  // yes
  bool sizeValid = fields->integer(KEY_SIZE, &size) && size > 0 && size < limit;
  bool canStart = sizeValid && !uploading;
  if (canStart) {
      ...
  }
  ```

- **A condition that has a meaning gets a name**: `bool isDot = ...;` then `if (isDot)`. Avoid
  the inline condition that has to be decoded.
- **No trick without a comment.** A bit computation, an unusual conversion or a behavior that
  depends on a detail of the library is explained on the line above.
- **No duplication.** A piece of code written twice becomes a function.
- **No nested ternary operator.** A simple ternary is accepted. As soon as it contains a call
  with side effects, or takes more than one line, write an `if`.

## 2. Layout

### Braces

- **The opening brace is on the line of the signature or of the statement**:

  ```c
  int Loop::run() {
      ...
  }
  ```

- **Braces always, and no code on one line.** The body of an `if`, of a loop or of a function
  is always between braces and on its own lines, even if it contains only one statement.

  ```c
  // no
  if (done) return;
  if (done) { return; }
  int size() const { return _size; }

  // yes
  if (done) {
      return;
  }

  int size() const {
      return _size;
  }
  ```

- **Every `case` of a `switch` has its block between braces**, with the `break` inside, and
  the `case`s are indented one level under the `switch`. A label without statements, which
  "falls through" to the next one, stays bare.

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

### Blank lines: a function reads step by step

The body of a function is split into **groups**, one per step: prepare, convert, open, check,
loop, close… A blank line separates two groups.

- **A check stays attached to the statement whose result it tests.**
- **A blank line follows a closed block** (`}`), `} while (...);` included, unless what follows
  is `}`, `else` or `catch`.
- **A blank line precedes a loop** that follows other statements.
- **Never two blank lines in a row.**

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

The methods of a class that span several lines are also separated by a blank line.

### Alignment in headers

In a class or a struct of a `.h`, the names of the members and of the methods are aligned in
one column. The star or the ampersand of a pointer or of a reference goes right before the
name, hanging to the left of the column. Very long types (templates, for example) do not push
the column: they keep a single space.

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

## 3. Comments

### File header

Every source file of the project starts with the same header: the name of the project
drawn in ASCII, the author, the copyright holder and the license (the holder written
exactly as in `LICENSE`). The comment that describes the file comes after it, separated
by a blank line. In C, C++ and JS:

```c
/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */
```

- A block comment, never `//` lines: in C and C++ a `//` line that ends with `\` goes on
  to the next line (GCC `-Wcomment`, an error with `-Werror`), and the drawing has such
  lines. The drawing never contains `*/`.
- Shell, CMake, Perl, PowerShell, `sdkconfig` and partition CSV files: the same lines
  prefixed with `# ` (after the `#!` line of a script); Lua: `-- `.
- Not in the files that come from elsewhere: the original notice of their authors stays
  (in qjsx: the QuickJS files of `engine/`, except the files written for qjsx). Not in
  JSON (no comments) nor in the documentation.
- The `SPDX-License-Identifier` line lets the license tools read the license.

### A comment on every function

Every function and every method has a `/** ... */` block right above it:
- in the `.h`, above the declaration;
- in the `.cpp`, above the definition, including for `static` functions, the functions of an
  anonymous namespace, constructors and destructors.

This block says **what** the function **does**, in one or two lines. It does not tell how it
does it.

```c
/**
 * HELLO: opens the session.
 */
void Agent::handleHello(int64_t id, const Fields *fields) {
```

In a header, add what the caller must know: who owns the returned memory, which thread may
call the function, what the return value is on error.

**Parameters and return value: only if they are not obvious.** No systematic list of
`@param` / `@return`. A parameter is described when its name and its type are not enough (unit,
range, `nullptr` allowed, who owns it, what an output holds on failure), and the return value
when it is not obvious (meaning of a `false`, of a `-1`, of a `nullptr`).

```c
/**
 * Receives at most RECEIVE_BLOCK bytes into a new block given to the caller in
 * *data (null unless the count is > 0). Returns the byte count, 0 at the end of
 * the stream, < 0 on error.
 */
int receiveBlock(Socket socket, uint8_t **data);
```

### The flow goes in the code, as pseudo-code

The "how" is written **in the body of the function**, in the manner of pseudo-code: a short
comment above each group of statements (see the blank lines). Read in sequence, these comments
tell the algorithm.

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

- **Comment the why and the non-obvious.** A comment does not repeat the code (no
  `// increment i` above `i++`).
- **Document what cannot be seen**: the thread that runs the code, the order imposed between
  two operations and its reason, who frees what, what happens on failure.
- **A comment above every group**, even when it seems obvious: read in sequence, they must
  tell the whole function, with no gap.
- **A small function without steps** only needs its header block.

### Members of structs and classes

Every member (data) of a struct or of a class has a short `//` comment at the end of the
line. It says what it holds, its unit, who owns the memory, which thread or which lock
protects it. The comments of consecutive members are aligned in one column. If the comment
does not fit on the line, it goes on a `//` line above the member.

```c
struct Timer {
    int64_t   deadline;   // ms, monotonic clock
    int64_t   interval;   // ms; 0 for a one-shot timer
    JSValue   callback;   // owned
    Timer    *next;       // next timer by deadline
};
```

### Section comments

To split a long file into parts, use a line of hash signs, more visible than a line of
dashes. It carries the title of the part, or it stands alone as a plain separator. It is
filled up to column 80. A `/** */` block is not used: it is reserved for functions.

```c
// ### Sending ##################################################################

// ##############################################################################
```

## 4. Parameters and types

- **No reference in APIs**, neither `T &` nor `const T &`. At the call, they hide that a
  variable may be modified. Instead:
  - **as input**, a small struct is passed by value (copied: the function cannot modify it),
    a large one as `const T *`;
  - **as output**, pass an explicit pointer `T *out` that the function fills. The `&` seen at
    the call shows that the variable is going to change.

  ```c
  board::Identity identity;
  board::identity(&identity);              // fills identity: the & shows it

  bool startThread(ThreadFunction *run, void *argument, ThreadOptions options);   // copy
  ```

  Possible exception: the internal templates of a generic library may keep their `const T &`
  to avoid copies, as long as the API visible to the user follows the rule.

- **C-style casts**, `(T)value`, rather than `static_cast<T>(value)`, `reinterpret_cast` or
  `const_cast`. Mind the precedence: when a member, an index or a call follows, put the cast
  between parentheses.

  ```c
  ((Runner *)opaque)->run();       // yes: opaque is cast, then run() is called
  (Runner *)opaque->run();         // NO: C++ reads (Runner *)(opaque->run())
  ```

  No useless parentheses: `(char *)malloc(size)`, and not `(char *)(malloc(size))`.

- **Times and counters**: a time in milliseconds and a delay are signed on 64 bits
  (`int64_t`). A deadline minus the current time gives a negative delay for a timer that has
  already expired, and 64 bits do not overflow in practice. Do not use a 32-bit counter that
  wraps to zero, like the ticks of an RTOS.

## 5. Naming

- **Private members**: they start with an underscore (`_state`, `_firstRequest`), and not
  `state_`. Exception: the prefixes pasted by a macro (`jsNewModule_##name`).
- **Types and classes** in `PascalCase`, **functions, methods and variables** in `camelCase`,
  **constants and C enumeration values** in `UPPERCASE_UNDERSCORED` (`KIND_HELLO`,
  `DEFAULT_STACK`), **values of an `enum class`** in `PascalCase` (`StepMode::Over`).
- **Names that say what they hold**, without obscure abbreviations: `received`,
  `packageSize`, `isDebugRequest`, and not `n`, `sz` or `flag`. Short loop counters (`i`, `j`)
  are accepted.
- **Booleans read like a question**: `isOpen`, `hasWork`, `stillCurrent`, `wanted`.

## 6. Memory and stack

- **The stack is small** on an embedded system or under an RTOS: a few KB per task. Do not put
  an array of more than about 256 bytes on the stack. An I/O buffer goes on the heap.
- **I/O threads read directly into the allocated block they pass on** (to a queue, to an
  event loop), shrunk afterwards to the received size with `realloc`. This avoids a buffer on
  the stack and a copy. If the bytes are consumed right away, use a buffer allocated only once
  when the thread starts.
- **All allocations go through a single module** (for example `memory::allocate`,
  `reallocate`, `release`, `copyText`), never through `malloc` / `realloc` / `free` directly.
  Running out of memory is fatal there: message then stop (abort on a PC, restart on a
  target). So these functions never return `nullptr` and the caller does not test the result.
  This single entry point also makes it possible to count or trace the allocations
  (profiling). The heap of a library itself (the one of the JS engine, for example) keeps its
  allocator.
- **Memory ownership is written** in the comment of the function: "malloc'd, freed by the
  caller", "borrowed", "the loop owns it".

## 7. Checks

Part of these rules is mechanical and can be applied or checked by a tool (a script, a
configured formatter, an automatic review):

- move the brace of a function up to the line of its signature;
- split the code written on one line and add the missing braces, `case` included;
- indent the `case`s under their `switch`;
- align the names of the members in headers;
- add a blank line after a closed block and before a loop;
- convert C++ casts into C-style casts;
- list the functions without a `/** */` block.

The rest cannot be automated: grouping the lines by logical steps and writing the pseudo-code
are done by hand, while rereading.

A formatting tool must never touch the code of a third-party library, nor the body of
macros.

**Checking that a formatting pass did not change the code**: compare the two versions
stripped of their comments and of their blank lines (and of the braces, if the pass added
some). They must be identical. For example:

```sh
strip() { perl -0pe 's{/\*.*?\*/}{}gs; s{//[^\n]*}{}g' "$1" | grep -v '^\s*$' | tr -d '{} \t\n'; }
diff <(strip avant.cpp) <(strip apres.cpp)
```

The result must be empty.

## 8. Summary

| Topic | Rule |
|---|---|
| Principle | readability and maintainability first; one idea per line; named steps; no duplication |
| Braces | on the line of the signature; always present; never code on one line; every `case` between braces, indented under its `switch` |
| Blank lines | one group per step; after a closed block; before a loop; never two in a row |
| Headers | names of the members and methods aligned in one column |
| Comments | short `/** */` (the what) on every function; the flow as pseudo-code in the body |
| Parameters | no references; input by value or `const T *`; output through `T *out` |
| Casts | `(T)x`; `((T *)p)->m` when a member follows |
| Naming | private members `_name`; booleans as a question; explicit names |
| Stack | no array of more than ~256 bytes; I/O buffers on the heap |
| Time | signed `int64_t` in milliseconds, never a 32-bit counter |
