---
title: Small tasks
description: Writing what a board does all the time (blinking, animating, answering a link) as plain code that waits.
---

A board often does several things at once: a LED that breathes, a strip that animates, a
link that answers commands, a sensor read every second. `stdlib/tasks` lets each of them be
written as a loop that waits, instead of a function called by a timer that must remember
where it was.

Everything still runs on the one JS thread. A task is an async function: while it waits
(`await`), the other tasks and the callbacks run; between two waits, nothing interrupts it.

## A task

```js
import { task } from "stdlib/tasks";
import { RgbLed } from "stdlib/rgbled";

const led = new RgbLed(48);

const breath = task("breath", async self => {
    let phase = 0;
    const tick = self.every(10);
    while (!self.stopped) {
        const level = Math.max(0, Math.sin(phase));
        led.set(0, 0, 40 * level, 20 * level);
        await led.show();
        phase += Math.PI / 50;
        await tick.next();
    }
    led.fill(0, 0, 0);
    await led.show();
});
```

- `task(name, body)` starts the body at once; `self` is the task. The name is printed with
  its errors.
- `self.every(10)` makes a ticker: `tick.next()` waits for the next step, at fixed times
  counted from its creation (the time the loop takes does not shift them). A loop that is
  late does not run several times in a row to catch up: the steps passed are skipped.
- `self.sleep(ms)` waits a time.

## Stopping a task

```js
breath.stop();          // asks
await breath.join();    // waits for the end
```

`stop()` sets `self.stopped` and ends at once the waits the task made with this module
(`self.sleep`, `tick.next`, its mailboxes). The task leaves its loop and ends its own way:
the code after the loop is where it cleans up. A task that never looks at `self.stopped`
does not stop: 5 s after `stop()` it is reported on the console.

`join()` gives the value the body returned. When the body throws, the task prints
`<name>: ended by an error: <error>`, and `join()` is rejected with it.

## Errors in a loop

An error that leaves the body ends the task: its loop does not run again. A loop that
must survive a failed step (a watch, a refresh) catches the errors of that step itself:

```js
task("watch", async self => {
    const tick = self.every(100);
    while (!self.stopped) {
        try {
            await poll();
        } catch (error) {
            console.error("watch:", error);    // printed, the watch goes on
        }
        await tick.next();
    }
});
```

## Values from a callback: a mailbox

A callback (a line received, a button pressed) can come at any time, even while the task
is in the middle of a command. A mailbox keeps the values and gives them to the task one by
one, in order:

```js
task("commands", async self => {
    const inbox = self.mailbox(30);
    link.onLine(line => {
        if (!inbox.send(line)) {
            console.warn(`commands: "${line}" dropped`);
        }
    });

    for await (const line of inbox) {
        await execute(line);       // one command at a time
    }
});
```

`send()` never waits: when the mailbox is full, it refuses the value and returns `false`.
Once the task is stopped, `for await` ends and the values still waiting are dropped.

## Sharing a resource: a mutex

Several tasks that use the same bus must not mix their transfers: a command of one task
may need two transfers in a row that nobody else comes between. A `Mutex` runs the jobs
given to `withLock` one at a time, in the order of the calls:

```js
import { Mutex } from "stdlib/tasks";

const bus = new Mutex();

const count = await bus.withLock(async () => {
    await reader.selectAntenna(1);
    return await reader.tagCount();
});
```

The promise of `withLock` gives the value or the error of the job; an error does not stop
the jobs after it. A job must not call `withLock` on its own mutex: it would wait for
itself.
