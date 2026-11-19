/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The touch panel of a Linux board through its input device (2026-11-18,
// first the STM32MP157-DK2: its FT6236, read by the kernel's driver), for
// "qjsx agent --touch /dev/input/eventN" (screen_host.h), read on a thread
// of its own. The device says what it is:
//
// - a multi-touch panel (protocol B, slots): the thread keeps the slots
//   (tracking id, x, y) as the events change them; at each SYN_REPORT the
//   fingers are copied into the report. The range of the coordinates is the
//   device's (EVIOCGABS), from 0. When the image is smaller than the display
//   (--screen-size, centered: screenFrame), the panel still covers the
//   display: its points are scaled to the display, then given in the
//   image's pixels (range: the image's size), a finger outside the image
//   left out;
// - a mouse (relative moves, a left button; 2026-11-18, a board on HDMI
//   without its touch panel): played as a panel of ONE finger. The moves
//   add up to a place kept on the display (the range is the display's size,
//   at rotation 0); the left button held is the finger there, released no
//   finger. The cursor of the display follows the place; moves without the
//   button tell the program nothing.
//
// Each report is marked new and the watcher told (it posts to the program's
// loop; the program then reads the report with board::touchRead).
#include <stdio.h>

#include "screen_host.h"

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>

#include "memory.h"
#include "platform.h"

namespace {

const int MOST_POINTS = 10;            // fingers kept, at most
const int EVENTS_PER_READ = 64;        // input events read at once (on the heap)
const int MOUSE_FINGER_ID = 0;         // the id of the mouse's finger

// A slot of the protocol: a finger while its tracking id is not -1.
struct Slot {
    int id;                            // tracking id; -1: no finger
    int x;
    int y;
};

// The mouse played as a finger (reader thread only).
struct Mouse {
    int  x;                            // its place on the display, pixels at rotation 0
    int  y;
    bool pressed;                      // the left button held
    bool buttonChanged;                // pressed or released since the last SYN_REPORT
};

int             device = -1;           // the input device
bool            isMouse = false;       // a mouse, not a multi-touch panel
board::TouchInfo info = {};            // its points and ranges
int             minimumX = 0;          // the smallest coordinates it gives
int             minimumY = 0;
int             deviceRangeX = 0;      // how many coordinates it gives (a panel)
int             deviceRangeY = 0;
bool            framed = false;        // the image is smaller than the display: points converted
hostscreen::ScreenFrame frame = {};    // where the image lies on the display (framed)

platform::Mutex touchLock;             // what follows
Slot            slots[MOST_POINTS];    // the reader's state (reader thread only, read under the lock)
Mouse           mouse = {};            // the mouse's state (reader thread only)
board::TouchPoint report[MOST_POINTS]; // the last report
int             reportCount = 0;
bool            reportNew = false;     // not read since it came
board::TouchChanged *changedHandler = nullptr;   // the watcher; nullptr: none
void           *changedContext = nullptr;

/**
 * The report made new, the watcher told; called under the lock
 * (touchWatch(nullptr) waits for a call in progress).
 */
void tellWatcher() {
    reportNew = true;
    if (changedHandler != nullptr) {
        changedHandler(changedContext);
    }
}

/**
 * The finger of a slot as a point of the report: the device's coordinates
 * from 0; when framed, scaled to the display then in the image's pixels.
 * False when it lies outside the image.
 */
bool pointOf(const Slot *slot, board::TouchPoint *point) {
    // from 0
    int x = slot->x - minimumX;
    int y = slot->y - minimumY;
    if (!framed) {
        *point = board::TouchPoint{ slot->id, x, y };
        return true;
    }

    // on the display, then on the image
    x = x * frame.displayWidth / deviceRangeX - frame.x;
    y = y * frame.displayHeight / deviceRangeY - frame.y;
    bool inside = x >= 0 && x < info.rangeX && y >= 0 && y < info.rangeY;
    if (!inside) {
        return false;
    }

    *point = board::TouchPoint{ slot->id, x, y };
    return true;
}

/**
 * The fingers of the slots, as the report.
 */
void publishSlots() {
    touchLock.lock();
    reportCount = 0;

    for (const Slot &slot : slots) {
        board::TouchPoint point = {};
        if (slot.id >= 0 && pointOf(&slot, &point)) {
            report[reportCount++] = point;
        }
    }

    tellWatcher();
    touchLock.unlock();
}

/**
 * The mouse's finger as the report: there while the button is held, none
 * once released.
 */
void publishMouse() {
    touchLock.lock();
    reportCount = 0;

    if (mouse.pressed) {
        report[reportCount++] = board::TouchPoint{ MOUSE_FINGER_ID, mouse.x, mouse.y };
    }

    tellWatcher();
    touchLock.unlock();
}

/**
 * 'value' kept from 0 to 'range' - 1.
 */
int clampTo(int value, int range) {
    if (value < 0) {
        return 0;
    }

    return value >= range ? range - 1 : value;
}

/**
 * An event of a multi-touch panel: a slot chosen, its finger, its place; a
 * report at the end of a group.
 */
void panelEvent(const input_event *event, int *current) {
    if (event->type == EV_SYN && event->code == SYN_REPORT) {
        publishSlots();
        return;
    }

    if (event->type != EV_ABS) {
        return;
    }

    switch (event->code) {
        case ABS_MT_SLOT: {
            *current = event->value >= 0 && event->value < MOST_POINTS ? event->value : *current;
            break;
        }
        case ABS_MT_TRACKING_ID: {
            slots[*current].id = event->value;
            break;
        }
        case ABS_MT_POSITION_X: {
            slots[*current].x = event->value;
            break;
        }
        case ABS_MT_POSITION_Y: {
            slots[*current].y = event->value;
            break;
        }
        default: {
            break;
        }
    }
}

/**
 * An event of a mouse: a move added to its place (kept on the display), the
 * left button; at the end of a group, the cursor moved and, while the button
 * is held or when it changed, a report.
 */
void mouseEvent(const input_event *event) {
    // the end of a group: the cursor, the finger
    if (event->type == EV_SYN && event->code == SYN_REPORT) {
        hostscreen::moveCursor(mouse.x, mouse.y);

        if (mouse.pressed || mouse.buttonChanged) {
            publishMouse();
        }

        mouse.buttonChanged = false;
        return;
    }

    // a move
    if (event->type == EV_REL && event->code == REL_X) {
        mouse.x = clampTo(mouse.x + event->value, info.rangeX);
        return;
    }

    if (event->type == EV_REL && event->code == REL_Y) {
        mouse.y = clampTo(mouse.y + event->value, info.rangeY);
        return;
    }

    // the left button (1 pressed, 0 released, 2 repeated: ignored)
    bool isLeftButton = event->type == EV_KEY && event->code == BTN_LEFT && event->value != 2;
    if (isLeftButton && mouse.pressed != (event->value == 1)) {
        mouse.pressed = event->value == 1;
        mouse.buttonChanged = true;
    }
}

/**
 * The reader thread: the events of the device, for ever (until it fails).
 */
void readEvents(void *) {
    input_event *events = (input_event *)memory::allocate(sizeof(input_event) * EVENTS_PER_READ);
    int current = 0;                   // the slot the next events of a panel change

    for (;;) {
        // a block of events
        ssize_t bytes = read(device, events, sizeof(input_event) * EVENTS_PER_READ);
        if (bytes < 0 && errno == EINTR) {
            continue;
        }

        if (bytes <= 0) {
            fprintf(stderr, "qjsx agent: --touch: the input device failed (%s)\n", strerror(errno));
            break;
        }

        // each one, as the device's kind reads it
        int count = (int)(bytes / (ssize_t)sizeof(input_event));

        for (int i = 0; i < count; i++) {
            if (isMouse) {
                mouseEvent(&events[i]);
            } else {
                panelEvent(&events[i], &current);
            }
        }
    }

    memory::release(events);
}

/**
 * The range of an axis of the device: its smallest value and how many.
 */
bool axisOf(int code, int *minimum, int *range) {
    input_absinfo axis = {};
    if (ioctl(device, EVIOCGABS(code), &axis) != 0) {
        return false;
    }

    *minimum = axis.minimum;
    *range = axis.maximum - axis.minimum + 1;
    return true;
}

/**
 * Whether the device gives the event 'code' of the kind 'type' (EV_REL,
 * EV_KEY...): its bit in the kind's table (EVIOCGBIT).
 */
bool givesEvent(int type, int code) {
    unsigned char bits[KEY_MAX / 8 + 1] = {};   // the largest table, 96 bytes
    if (ioctl(device, EVIOCGBIT(type, sizeof(bits)), bits) < 0) {
        return false;
    }

    return (bits[code / 8] & (1 << (code % 8))) != 0;
}

/**
 * A multi-touch panel: its ranges and its slots, none touched; the image's
 * size as its range when the image is smaller than the display. False when
 * the device is not one.
 */
bool startPanel() {
    // its axes and slots
    int slotMinimum = 0;
    int slotCount = 0;
    bool isPanel = axisOf(ABS_MT_POSITION_X, &minimumX, &deviceRangeX) && axisOf(ABS_MT_POSITION_Y, &minimumY, &deviceRangeY)
                   && axisOf(ABS_MT_SLOT, &slotMinimum, &slotCount);
    if (!isPanel) {
        return false;
    }

    // its range: its own, or the image's when smaller than the display
    const board::Screen *screen = board::screen();
    frame = hostscreen::screenFrame();
    framed = screen != nullptr && (frame.displayWidth != screen->width || frame.displayHeight != screen->height);
    info.rangeX = framed ? screen->width : deviceRangeX;
    info.rangeY = framed ? screen->height : deviceRangeY;

    // its fingers, none on it
    info.maxPoints = slotCount < MOST_POINTS ? slotCount : MOST_POINTS;
    for (Slot &slot : slots) {
        slot = Slot{ -1, 0, 0 };
    }

    return true;
}

/**
 * Whether the device is a mouse: moves on both axes and a left button.
 */
bool isMouseDevice() {
    return givesEvent(EV_REL, REL_X) && givesEvent(EV_REL, REL_Y) && givesEvent(EV_KEY, BTN_LEFT);
}

/**
 * A mouse: one finger over the whole display, the place at its center, the
 * cursor shown there. False without a display (said); true without a cursor
 * too (said).
 */
bool startMouse(const char *path) {
    // the display it moves over (opened before: --screen)
    const board::Screen *screen = board::screen();
    if (screen == nullptr) {
        fprintf(stderr, "qjsx agent: --touch %s: a mouse needs the display (--screen)\n", path);
        return false;
    }

    isMouse = true;
    info.maxPoints = 1;
    info.rangeX = screen->width;
    info.rangeY = screen->height;
    mouse = Mouse{ screen->width / 2, screen->height / 2, false, false };

    // its cursor
    if (!hostscreen::showCursor(mouse.x, mouse.y)) {
        printf("qjsx agent: --touch %s: the display has no cursor, the mouse moves unseen\n", path);
    }

    return true;
}

bool driverInfo(board::TouchInfo *out) {
    *out = info;
    return true;
}

void driverWatch(board::TouchChanged *changed, void *context) {
    touchLock.lock();
    changedHandler = changed;
    changedContext = context;
    touchLock.unlock();
}

int driverRead(board::TouchPoint *points, int most) {
    // nothing new since the last read
    touchLock.lock();
    if (!reportNew) {
        touchLock.unlock();
        return -1;
    }

    // the report, at most 'most' fingers
    int count = reportCount < most ? reportCount : most;
    for (int i = 0; i < count; i++) {
        points[i] = report[i];
    }

    reportNew = false;
    touchLock.unlock();
    return count;
}

const hostscreen::TouchDriver DRIVER = { &driverInfo, &driverWatch, &driverRead };

} // namespace

namespace hostscreen {

bool openTouch(const char *path) {
    // the device
    device = open(path, O_RDONLY | O_CLOEXEC);
    if (device < 0) {
        fprintf(stderr, "qjsx agent: --touch %s: %s\n", path, strerror(errno));
        return false;
    }

    // a multi-touch panel, else a mouse
    bool isPanel = startPanel();
    bool isPointer = !isPanel && isMouseDevice();
    if (!isPanel && !isPointer) {
        fprintf(stderr, "qjsx agent: --touch %s: neither a multi-touch panel nor a mouse\n", path);
    }

    bool started = isPanel || (isPointer && startMouse(path));
    if (!started) {
        close(device);
        device = -1;
        return false;
    }

    // the reader, then the touch panel of the board from now
    platform::ThreadOptions options = platform::defaultThreadOptions("touch");
    if (!platform::startThread(&readEvents, nullptr, options)) {
        fprintf(stderr, "qjsx agent: --touch: cannot start its thread\n");
        return false;
    }

    setTouch(&DRIVER);
    const char *kind = isMouse ? "mouse" : "touch panel";
    printf("qjsx agent: %s %d x %d, %d points, on %s\n", kind, info.rangeX, info.rangeY, info.maxPoints, path);
    return true;
}

} // namespace hostscreen

#else

namespace hostscreen {

bool openTouch(const char *path) {
    fprintf(stderr, "qjsx agent: --touch %s: only on a Linux board\n", path);
    return false;
}

} // namespace hostscreen

#endif
