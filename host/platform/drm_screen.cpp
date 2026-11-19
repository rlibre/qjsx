/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The display of a Linux board through DRM (2026-11-18, first the
// STM32MP157-DK2: its 480 x 800 DSI panel), for "qjsx agent --screen
// /dev/dri/card0" (screen_host.h). No libdrm: the kernel's ioctls.
//
// 1. the device opened and its master taken (another program holding the
//    display, as Weston, makes it fail: stop it first);
// 2. its first connected connector, a mode of it, an encoder and a CRTC that
//    can drive it. The mode: without a size asked (--screen-size), the
//    preferred one; with one, the smallest mode holding it (that size
//    itself when the display has it), the preferred then the fastest among
//    those of a size;
// 3. a "dumb" buffer of the mode's size, 16 bits a pixel (DRM_FORMAT_RGB565,
//    the pixels of board_screen.h), made a framebuffer, mapped, black, shown;
// 4. sys/draw draws into an image in ordinary memory (cached: the rotations
//    write scattered pixels, slow in the mapping, which is not), of the size
//    asked, else the mode's; each area made visible is copied into the
//    mapping, row by row, the image centered in the mode (black around it
//    when smaller);
// 5. for a mouse (evdev_touch.cpp), the cursor: an arrow in a buffer of its
//    own, 64 x 64 ARGB, shown and moved by the CRTC's cursor plane (the
//    legacy cursor ioctl): the image is never drawn again for it. Made only
//    when a mouse asks for it (a touch panel shows no cursor); a card
//    without a cursor plane refuses it.
#include <stdio.h>

#include "screen_host.h"

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "memory.h"

namespace {

const uint32_t BITS_PER_PIXEL = 16;    // RGB565
const uint32_t COLOR_DEPTH = 16;
const uint32_t CONNECTED = 1;          // drm_mode_get_connector.connection

// The cursor: its size (what every card takes), its pixels (ARGB8888).
const uint32_t CURSOR_SIZE = 64;
const uint32_t CURSOR_BITS_PER_PIXEL = 32;
const uint32_t CURSOR_OUTLINE = 0xff000000;
const uint32_t CURSOR_FILL = 0xffffffff;

// The arrow, its tip at the top left: 'X' the outline, '.' the inside, the
// rest transparent.
const char *const ARROW[] = {
    "X",
    "XX",
    "X.X",
    "X..X",
    "X...X",
    "X....X",
    "X.....X",
    "X......X",
    "X.......X",
    "X........X",
    "X.........X",
    "X......XXXXX",
    "X...X..X",
    "X..XX..X",
    "X.X  X..X",
    "XX   X..X",
    "X     X..X",
    "      X..X",
    "       XX",
};

board::Screen image = {};              // what sys/draw draws into
uint8_t      *mapped = nullptr;        // the buffer the panel shows
uint32_t      pitch = 0;               // bytes of a row of it
int           originX = 0;             // where the image starts in it, pixels (centered)
int           originY = 0;
int           card = -1;              // the DRM device, open while the agent runs
uint32_t      shownCrtc = 0;           // the CRTC showing the buffer (and the cursor)

/**
 * An ioctl, again while interrupted.
 */
int control(int fd, unsigned long request, void *argument) {
    int result = 0;

    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && (errno == EINTR || errno == EAGAIN));

    return result;
}

/**
 * Copies an area of the image to the buffer the panel shows, row by row, at
 * the image's place in it.
 */
void copyArea(board::ScreenArea area) {
    // cut to the image
    int left = area.x < 0 ? 0 : area.x;
    int top = area.y < 0 ? 0 : area.y;
    int right = area.x + area.width > image.width ? image.width : area.x + area.width;
    int bottom = area.y + area.height > image.height ? image.height : area.y + area.height;

    // each row of the area
    size_t bytes = (size_t)(right - left) * sizeof(uint16_t);

    for (int row = top; row < bottom; row++) {
        const uint16_t *from = image.pixels + (size_t)row * (size_t)image.width + (size_t)left;
        uint8_t *to = mapped + (size_t)(originY + row) * pitch + (size_t)(originX + left) * sizeof(uint16_t);
        memcpy(to, from, bytes);
    }
}

/**
 * Whether 'a' is a better mode than 'b': the smaller, then the preferred,
 * then the faster.
 */
bool isBetterMode(const drm_mode_modeinfo *a, const drm_mode_modeinfo *b) {
    // the smaller
    uint32_t areaA = (uint32_t)a->hdisplay * (uint32_t)a->vdisplay;
    uint32_t areaB = (uint32_t)b->hdisplay * (uint32_t)b->vdisplay;
    if (areaA != areaB) {
        return areaA < areaB;
    }

    // the preferred
    bool preferredA = (a->type & DRM_MODE_TYPE_PREFERRED) != 0;
    bool preferredB = (b->type & DRM_MODE_TYPE_PREFERRED) != 0;
    if (preferredA != preferredB) {
        return preferredA;
    }

    // the faster
    return a->vrefresh > b->vrefresh;
}

/**
 * The mode among 'modes' for an image of 'width' x 'height': without a size
 * (0), the preferred one (else the first); with one, the best (isBetterMode)
 * of those holding it. False when none holds it.
 */
bool chooseMode(const drm_mode_modeinfo *modes, uint32_t count, int width, int height, drm_mode_modeinfo *out) {
    // no size: the preferred mode, else the first
    if (width == 0) {
        *out = modes[0];
        for (uint32_t m = 0; m < count; m++) {
            if ((modes[m].type & DRM_MODE_TYPE_PREFERRED) != 0) {
                *out = modes[m];
                break;
            }
        }
        return true;
    }

    // a size: the best of the modes holding it
    const drm_mode_modeinfo *best = nullptr;

    for (uint32_t m = 0; m < count; m++) {
        bool holds = modes[m].hdisplay >= width && modes[m].vdisplay >= height;
        if (holds && (best == nullptr || isBetterMode(&modes[m], best))) {
            best = &modes[m];
        }
    }

    if (best == nullptr) {
        return false;
    }

    *out = *best;
    return true;
}

/**
 * Says that no mode holds 'width' x 'height', and the modes of the display.
 */
void printModes(const drm_mode_modeinfo *modes, uint32_t count, int width, int height) {
    fprintf(stderr, "qjsx agent: --screen-size %dx%d: larger than every mode of the display:", width, height);

    for (uint32_t m = 0; m < count; m++) {
        fprintf(stderr, " %ux%u@%u", modes[m].hdisplay, modes[m].vdisplay, modes[m].vrefresh);
    }

    fprintf(stderr, "\n");
}

/**
 * The connector to use (its id, its mode for an image of 'width' x 'height'
 * (chooseMode; 0: none asked), its encoder) among the card's: the first
 * connected one. False after printing why.
 */
bool findConnector(int fd, const drm_mode_card_res *resources, int width, int height, uint32_t *connectorId, drm_mode_modeinfo *mode,
                   uint32_t *encoderId) {
    const uint32_t *connectors = (const uint32_t *)(uintptr_t)resources->connector_id_ptr;

    for (uint32_t i = 0; i < resources->count_connectors; i++) {
        // its counts
        drm_mode_get_connector connector = {};
        connector.connector_id = connectors[i];
        if (control(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) != 0 || connector.connection != CONNECTED || connector.count_modes == 0) {
            continue;
        }

        // its modes and encoders (not its properties)
        uint32_t modeCount = connector.count_modes;
        uint32_t encoderCount = connector.count_encoders;
        drm_mode_modeinfo *modes = (drm_mode_modeinfo *)memory::allocate(sizeof(drm_mode_modeinfo) * modeCount);
        uint32_t *encoders = (uint32_t *)memory::allocate(sizeof(uint32_t) * (encoderCount > 0 ? encoderCount : 1));
        connector.modes_ptr = (uint64_t)(uintptr_t)modes;
        connector.encoders_ptr = (uint64_t)(uintptr_t)encoders;
        connector.count_props = 0;
        bool read = control(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) == 0 && connector.count_modes > 0;

        // its mode (none holding the size: said, with its modes)
        bool chosen = read && chooseMode(modes, connector.count_modes, width, height, mode);
        if (read && !chosen) {
            printModes(modes, connector.count_modes, width, height);
        }

        // its encoder now, else its first
        if (chosen) {
            *connectorId = connector.connector_id;
            *encoderId = connector.encoder_id != 0 ? connector.encoder_id : (connector.count_encoders > 0 ? encoders[0] : 0);
        }

        memory::release(modes);
        memory::release(encoders);
        if (read && !chosen) {
            return false;
        }

        if (chosen && *encoderId != 0) {
            return true;
        }
    }

    fprintf(stderr, "qjsx agent: --screen: no connected display\n");
    return false;
}

/**
 * The CRTC that drives 'encoderId': its own, else the first it may use.
 */
uint32_t findCrtc(int fd, const drm_mode_card_res *resources, uint32_t encoderId) {
    // the encoder
    drm_mode_get_encoder encoder = {};
    encoder.encoder_id = encoderId;
    if (control(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) != 0) {
        return 0;
    }

    // its CRTC now, else the first of those it may use
    if (encoder.crtc_id != 0) {
        return encoder.crtc_id;
    }

    const uint32_t *crtcs = (const uint32_t *)(uintptr_t)resources->crtc_id_ptr;

    for (uint32_t i = 0; i < resources->count_crtcs; i++) {
        if ((encoder.possible_crtcs & (1u << i)) != 0) {
            return crtcs[i];
        }
    }

    return 0;
}

/**
 * Makes the buffer of 'mode', maps it, shows it on 'crtcId' through
 * 'connectorId'. False after printing why.
 */
bool showBuffer(int fd, uint32_t connectorId, uint32_t crtcId, drm_mode_modeinfo *mode) {
    // the buffer, 16 bits a pixel
    drm_mode_create_dumb buffer = {};
    buffer.width = mode->hdisplay;
    buffer.height = mode->vdisplay;
    buffer.bpp = BITS_PER_PIXEL;
    if (control(fd, DRM_IOCTL_MODE_CREATE_DUMB, &buffer) != 0) {
        fprintf(stderr, "qjsx agent: --screen: cannot make a buffer (%s)\n", strerror(errno));
        return false;
    }

    // a framebuffer of it
    drm_mode_fb_cmd framebuffer = {};
    framebuffer.width = buffer.width;
    framebuffer.height = buffer.height;
    framebuffer.pitch = buffer.pitch;
    framebuffer.bpp = BITS_PER_PIXEL;
    framebuffer.depth = COLOR_DEPTH;
    framebuffer.handle = buffer.handle;
    if (control(fd, DRM_IOCTL_MODE_ADDFB, &framebuffer) != 0) {
        fprintf(stderr, "qjsx agent: --screen: cannot make a framebuffer (%s)\n", strerror(errno));
        return false;
    }

    // mapped, black
    drm_mode_map_dumb map = {};
    map.handle = buffer.handle;
    if (control(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        fprintf(stderr, "qjsx agent: --screen: cannot map the buffer (%s)\n", strerror(errno));
        return false;
    }

    void *memory = mmap(nullptr, (size_t)buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map.offset);
    if (memory == MAP_FAILED) {
        fprintf(stderr, "qjsx agent: --screen: cannot map the buffer (%s)\n", strerror(errno));
        return false;
    }

    mapped = (uint8_t *)memory;
    pitch = buffer.pitch;
    memset(mapped, 0, (size_t)buffer.size);

    // shown
    drm_mode_crtc crtc = {};
    crtc.crtc_id = crtcId;
    crtc.fb_id = framebuffer.fb_id;
    crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&connectorId;
    crtc.count_connectors = 1;
    crtc.mode = *mode;
    crtc.mode_valid = 1;
    if (control(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) != 0) {
        fprintf(stderr, "qjsx agent: --screen: cannot show the buffer (%s)\n", strerror(errno));
        return false;
    }

    return true;
}

/**
 * The arrow drawn into the cursor's buffer ('row' bytes a row), transparent
 * around it.
 */
void drawArrow(uint8_t *pixels, uint32_t row) {
    int rows = (int)(sizeof(ARROW) / sizeof(ARROW[0]));

    for (int y = 0; y < rows; y++) {
        uint32_t *line = (uint32_t *)(pixels + (size_t)y * row);
        int length = (int)strlen(ARROW[y]);

        for (int x = 0; x < length; x++) {
            char mark = ARROW[y][x];
            line[x] = mark == 'X' ? CURSOR_OUTLINE : (mark == '.' ? CURSOR_FILL : 0);
        }
    }
}

/**
 * Moves the cursor: its tip at (x, y) of the image (wherever the image is in
 * the mode). Any thread (an ioctl).
 */
void moveDrmCursor(int x, int y) {
    drm_mode_cursor cursor = {};
    cursor.flags = DRM_MODE_CURSOR_MOVE;
    cursor.crtc_id = shownCrtc;
    cursor.x = originX + x;
    cursor.y = originY + y;
    control(card, DRM_IOCTL_MODE_CURSOR, &cursor);
}

/**
 * Makes the cursor (its buffer, the arrow in it), gives it to the CRTC, puts
 * it at (x, y). False when the card refuses it (no cursor plane) or a step
 * fails.
 */
bool showDrmCursor(int x, int y) {
    // its buffer, 32 bits a pixel
    drm_mode_create_dumb buffer = {};
    buffer.width = CURSOR_SIZE;
    buffer.height = CURSOR_SIZE;
    buffer.bpp = CURSOR_BITS_PER_PIXEL;
    if (control(card, DRM_IOCTL_MODE_CREATE_DUMB, &buffer) != 0) {
        return false;
    }

    // the arrow in it (the mapping only for that)
    drm_mode_map_dumb map = {};
    map.handle = buffer.handle;
    if (control(card, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        return false;
    }

    void *memory = mmap(nullptr, (size_t)buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, card, (off_t)map.offset);
    if (memory == MAP_FAILED) {
        return false;
    }

    memset(memory, 0, (size_t)buffer.size);
    drawArrow((uint8_t *)memory, buffer.pitch);
    munmap(memory, (size_t)buffer.size);

    // given to the CRTC, then put in its place
    drm_mode_cursor cursor = {};
    cursor.flags = DRM_MODE_CURSOR_BO;
    cursor.crtc_id = shownCrtc;
    cursor.handle = buffer.handle;
    cursor.width = CURSOR_SIZE;
    cursor.height = CURSOR_SIZE;
    if (control(card, DRM_IOCTL_MODE_CURSOR, &cursor) != 0) {
        return false;
    }

    moveDrmCursor(x, y);
    return true;
}

const hostscreen::CursorDriver CURSOR = { &showDrmCursor, &moveDrmCursor };

} // namespace

namespace hostscreen {

bool openDrm(const char *path, int width, int height) {
    // the device, and its master: nobody else drives the display
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "qjsx agent: --screen %s: %s\n", path, strerror(errno));
        return false;
    }

    if (control(fd, DRM_IOCTL_SET_MASTER, nullptr) != 0) {
        fprintf(stderr, "qjsx agent: --screen %s: another program drives the display (Weston?): stop it first\n", path);
        close(fd);
        return false;
    }

    // the card's connectors and CRTCs (their counts, then their ids)
    drm_mode_card_res resources = {};
    if (control(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) != 0) {
        fprintf(stderr, "qjsx agent: --screen %s: not a display (%s)\n", path, strerror(errno));
        close(fd);
        return false;
    }

    uint32_t *connectors = (uint32_t *)memory::allocate(sizeof(uint32_t) * (resources.count_connectors + 1));
    uint32_t *crtcs = (uint32_t *)memory::allocate(sizeof(uint32_t) * (resources.count_crtcs + 1));
    resources.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    resources.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    resources.count_fbs = 0;
    resources.count_encoders = 0;
    bool listed = control(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) == 0;

    // the connector, its mode, the CRTC; then the buffer shown
    uint32_t connectorId = 0;
    uint32_t encoderId = 0;
    drm_mode_modeinfo mode = {};
    bool found = listed && findConnector(fd, &resources, width, height, &connectorId, &mode, &encoderId);
    uint32_t crtcId = found ? findCrtc(fd, &resources, encoderId) : 0;
    bool shown = crtcId != 0 && showBuffer(fd, connectorId, crtcId, &mode);
    memory::release(connectors);
    memory::release(crtcs);
    if (!shown) {
        if (found && crtcId == 0) {
            fprintf(stderr, "qjsx agent: --screen %s: no CRTC for the display\n", path);
        }
        close(fd);
        return false;
    }

    // the image sys/draw draws into: the size asked, else the mode's;
    // centered in the mode
    int imageWidth = width > 0 ? width : (int)mode.hdisplay;
    int imageHeight = height > 0 ? height : (int)mode.vdisplay;
    originX = ((int)mode.hdisplay - imageWidth) / 2;
    originY = ((int)mode.vdisplay - imageHeight) / 2;

    size_t bytes = (size_t)imageWidth * (size_t)imageHeight * sizeof(uint16_t);
    uint16_t *pixels = (uint16_t *)memory::allocate(bytes);
    memset(pixels, 0, bytes);
    image = board::Screen{ pixels, imageWidth, imageHeight };

    // the display of the board from now (the device stays open: closing it
    // would drop the master)
    card = fd;
    shownCrtc = crtcId;
    setScreen(image, &copyArea);
    setCursor(&CURSOR);

    bool fills = imageWidth == (int)mode.hdisplay && imageHeight == (int)mode.vdisplay;
    if (fills) {
        printf("qjsx agent: display %d x %d on %s\n", image.width, image.height, path);
    } else {
        setScreenFrame(ScreenFrame{ (int)mode.hdisplay, (int)mode.vdisplay, originX, originY });
        printf("qjsx agent: display %d x %d, centered in the mode %u x %u, on %s\n", image.width, image.height, mode.hdisplay, mode.vdisplay, path);
    }
    return true;
}

} // namespace hostscreen

#else

namespace hostscreen {

bool openDrm(const char *path, int, int) {
    fprintf(stderr, "qjsx agent: --screen %s: only on a Linux board\n", path);
    return false;
}

} // namespace hostscreen

#endif
