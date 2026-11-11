/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/serial", the PC side: the serial ports of the machine (on a
// board, the same module will drive the UARTs).
//
//   import { list, Serial } from "device/serial";
//   list()                                   ["COM3", ...] or ["/dev/ttyUSB0", ...]
//   const port = new Serial("COM3", { baudRate: 115200 });
//   port.onData(bytes => ...);
//   port.onClose(() => ...);
//   port.write("text" or bytes);
//   port.close();
//
// 8 data bits, no parity, 1 stop bit. A reader thread polls the port every
// 100 ms at most and posts the received bytes to the event loop; write() runs
// on the JS thread.
// Windows: the port is opened for overlapped I/O, so that the reads of the
// reader thread and the writes of the JS thread do not wait for each other.
// On a port opened without it, Windows runs the operations one at a time: a
// write waited for the end of the pending read (100 ms when nothing comes),
// and the reader thread, which reads again at once, often got in first
// again, so that one write could take seconds.
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "jsbind.h"
#include "memory.h"
#include "loop.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#endif

namespace {

enum MessageKind {
    MESSAGE_DATA = 1,
    MESSAGE_CLOSED,
};

#if defined(_WIN32)
typedef HANDLE PortHandle;
const PortHandle invalidPort = INVALID_HANDLE_VALUE;
#else
typedef int PortHandle;
const PortHandle invalidPort = -1;
#endif

// Shared by the JS thread and the reader thread. Allocated by
// SerialPort::start(), always freed by the reader thread when it ends.
struct PortState {
    PortHandle handle;      // owned: closed by the last owner (releasePortState)
    int closing;            // atomic; set by close(): the reader thread stops
    int references;         // atomic; 2 owners: the SerialPort and the reader thread
    rt::Mailbox *mailbox;   // retained until the state is freed
    rt::SourceId source;    // message source of the SerialPort
};

/**
 * Closes a port (defined below, one version per system).
 */
void closePort(PortHandle handle);

/**
 * One owner of the state lets it go; the last one closes the port and frees
 * the state. The JS side and the reader thread can thus stop in any order
 * (close(), a read error, the end of the runtime) without one using a port
 * or a state the other has freed.
 */
void releasePortState(PortState *state) {
    if (platform::atomicDecrement(&state->references) > 0) {
        return;
    }

    closePort(state->handle);
    state->mailbox->release();
    memory::release(state);
}

#if defined(_WIN32)

/**
 * Opens a COM port at 'baudRate', 8N1. Returns invalidPort and sets *error
 * (ENOENT: no such port, EBUSY: already open, EINVAL: settings refused, EIO: other).
 */
PortHandle openPort(const char *path, int baudRate, int *error) {
    // add the "\\.\" device prefix if missing ("COM10" and above need it)
    char device[64];
    snprintf(device, sizeof(device), strncmp(path, "\\\\.\\", 4) == 0 ? "%s" : "\\\\.\\%s", path);

    // open it for reading and writing, not shared, for overlapped I/O (reads
    // and writes are then independent, see the top of the file)
    DWORD access = GENERIC_READ | GENERIC_WRITE;
    HANDLE handle = CreateFileA(device, access, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : GetLastError() == ERROR_ACCESS_DENIED ? EBUSY : EIO;
        return invalidPort;
    }

    // 'baudRate', 8 data bits, no parity, 1 stop bit, no flow control
    DCB settings;
    memset(&settings, 0, sizeof(settings));
    settings.DCBlength = sizeof(settings);
    GetCommState(handle, &settings);
    settings.BaudRate = (DWORD)baudRate;
    settings.ByteSize = 8;
    settings.Parity = NOPARITY;
    settings.StopBits = ONESTOPBIT;
    settings.fBinary = TRUE;
    settings.fParity = FALSE;
    settings.fOutxCtsFlow = FALSE;
    settings.fOutxDsrFlow = FALSE;
    settings.fOutX = FALSE;
    settings.fInX = FALSE;

    // read timeouts: a read returns as soon as bytes are there, or after
    // 100 ms; apply both, or close the port
    COMMTIMEOUTS timeouts = { MAXDWORD, MAXDWORD, 100, 0, 0 };
    if (!SetCommState(handle, &settings) || !SetCommTimeouts(handle, &timeouts)) {
        CloseHandle(handle);
        *error = EINVAL;
        return invalidPort;
    }

    return handle;
}

/**
 * Prepares an overlapped operation: Windows signals its end through an event
 * of its own. False if the event cannot be created. The caller ends with
 * endOperation().
 */
bool beginOperation(OVERLAPPED *operation) {
    memset(operation, 0, sizeof(*operation));
    operation->hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);   // manual reset, not signaled
    return operation->hEvent != nullptr;
}

/**
 * Waits for the end of the read or write that ReadFile / WriteFile started
 * on 'handle' ('started' is what the call returned), gives the bytes
 * transferred and frees the event of the operation. False on error.
 */
bool endOperation(HANDLE handle, BOOL started, OVERLAPPED *operation, DWORD *transferred) {
    // the call either ended at once, or goes on in the background
    // (ERROR_IO_PENDING); anything else is an error
    bool running = started || GetLastError() == ERROR_IO_PENDING;

    // wait for the end and read its result
    bool done = running && GetOverlappedResult(handle, operation, transferred, TRUE);
    CloseHandle(operation->hEvent);
    return done;
}

/**
 * Reads what is available, waiting 100 ms at most (see openPort). Returns
 * the byte count (0 on timeout) or -1 on error.
 */
int readPort(PortHandle handle, uint8_t *buffer, size_t size) {
    OVERLAPPED operation;
    if (!beginOperation(&operation)) {
        return -1;
    }

    // start the read, then wait for bytes or for the timeout
    DWORD received = 0;
    BOOL started = ReadFile(handle, buffer, (DWORD)size, nullptr, &operation);
    if (!endOperation(handle, started, &operation, &received)) {
        return -1;
    }

    return (int)received;
}

/**
 * Writes everything, looping on partial writes. False on error.
 */
bool writePort(PortHandle handle, const uint8_t *data, size_t size) {
    while (size > 0) {
        OVERLAPPED operation;
        if (!beginOperation(&operation)) {
            return false;
        }

        // start the write, then wait until the bytes are taken
        DWORD written = 0;
        BOOL started = WriteFile(handle, data, (DWORD)size, nullptr, &operation);
        if (!endOperation(handle, started, &operation, &written) || written == 0) {
            return false;
        }

        data += written;
        size -= written;
    }

    return true;
}

/**
 * Closes the port.
 */
void closePort(PortHandle handle) {
    CloseHandle(handle);
}

/**
 * Serial ports declared by the drivers, from the registry. Returns a JS array
 * of names ("COM3"), empty if the key is missing.
 */
JSValue listPorts(JSContext *ctx) {
    // open HKLM\HARDWARE\DEVICEMAP\SERIALCOMM
    JSValue names = JS_NewArray(ctx);
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return names;
    }

    // each value is one port, its data the name; one byte is kept for the
    // terminating zero
    uint32_t count = 0;

    for (DWORD index = 0;; index++) {
        char valueName[256];
        DWORD valueNameSize = sizeof(valueName);
        char data[64];
        DWORD dataSize = sizeof(data) - 1;
        DWORD type = 0;
        LONG result = RegEnumValueA(key, index, valueName, &valueNameSize, nullptr, &type,
                                    (BYTE *)data, &dataSize);

        // a name or a port name too long for the buffers: not a port we
        // can open, skip it and go on with the next value
        if (result == ERROR_MORE_DATA) {
            continue;
        }

        // the end of the values (ERROR_NO_MORE_ITEMS), or a real error
        if (result != ERROR_SUCCESS) {
            break;
        }

        if (type == REG_SZ) {
            data[dataSize] = 0;
            JS_SetPropertyUint32(ctx, names, count++, JS_NewString(ctx, data));
        }
    }

    RegCloseKey(key);
    return names;
}

#else

/**
 * The termios constant of a baud rate, 0 if the system has none for it.
 */
speed_t speedOf(int baudRate) {
    switch (baudRate) {
        case 9600: {
            return B9600;
        }
        case 19200: {
            return B19200;
        }
        case 38400: {
            return B38400;
        }
        case 57600: {
            return B57600;
        }
        case 115200: {
            return B115200;
        }
        case 230400: {
            return B230400;
        }

    #ifdef B460800
        case 460800: {
            return B460800;
        }

    #endif
    #ifdef B921600
        case 921600: {
            return B921600;
        }

    #endif
        default: {
            return 0;
        }
    }
}

/**
 * Opens a tty at 'baudRate', raw. Returns invalidPort and sets *error (EINVAL if
 * the system has no constant for the baud rate).
 */
PortHandle openPort(const char *path, int baudRate, int *error) {
    // map the baud rate
    speed_t speed = speedOf(baudRate);
    if (speed == 0) {
        *error = EINVAL;
        return invalidPort;
    }

    // open the device, not as the controlling terminal of the process
    int handle = open(path, O_RDWR | O_NOCTTY);
    if (handle < 0) {
        *error = errno;
        return invalidPort;
    }

    // the current settings of the port, the base of the new ones
    struct termios settings;
    if (tcgetattr(handle, &settings) != 0) {
        *error = errno;
        close(handle);
        return invalidPort;
    }

    // raw mode at that speed, 8N1 (cfmakeraw leaves the stop bits and the
    // hardware flow control as they were: one stop bit, no RTS/CTS), receiver
    // on, modem lines ignored; reads return what is there, or nothing after
    // 100 ms (VMIN 0, VTIME 1)
    cfmakeraw(&settings);
    settings.c_cflag &= ~CSTOPB;
#ifdef CRTSCTS
    // not POSIX: Linux and macOS have it
    settings.c_cflag &= ~CRTSCTS;
#endif
    cfsetispeed(&settings, speed);
    cfsetospeed(&settings, speed);
    settings.c_cflag |= CLOCAL | CREAD;
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 1;   // 100 ms
    if (tcsetattr(handle, TCSANOW, &settings) != 0) {
        *error = errno;
        close(handle);
        return invalidPort;
    }

    return handle;
}

/**
 * Reads what is available, waiting 100 ms at most (see openPort). Returns
 * the byte count (0 on timeout) or -1 on error.
 */
int readPort(PortHandle handle, uint8_t *buffer, size_t size) {
    ssize_t received = read(handle, buffer, size);

    // a signal interrupted the wait: nothing read, the port stays open
    if (received < 0 && errno == EINTR) {
        return 0;
    }

    return received < 0 ? -1 : (int)received;
}

/**
 * Writes everything, looping on partial writes and on writes interrupted by
 * a signal. False on error.
 */
bool writePort(PortHandle handle, const uint8_t *data, size_t size) {
    while (size > 0) {
        ssize_t written = write(handle, data, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written <= 0) {
            return false;
        }

        data += written;
        size -= written;
    }

    return true;
}

/**
 * Closes the port.
 */
void closePort(PortHandle handle) {
    close(handle);
}

/**
 * USB serial adapters: /dev/ttyUSB*, /dev/ttyACM* (Linux), /dev/cu.* (macOS).
 * Returns a JS array of their paths, in directory order.
 */
JSValue listPorts(JSContext *ctx) {
    // scan /dev
    JSValue names = JS_NewArray(ctx);
    DIR *directory = opendir("/dev");
    if (directory == nullptr) {
        return names;
    }

    // keep the entries whose name is a USB serial adapter
    uint32_t count = 0;
    struct dirent *entry;

    while ((entry = readdir(directory)) != nullptr) {
        const char *name = entry->d_name;
        bool isSerial = strncmp(name, "ttyUSB", 6) == 0 || strncmp(name, "ttyACM", 6) == 0
                     || strncmp(name, "cu.", 3) == 0;
        if (isSerial) {
            char path[300];
            snprintf(path, sizeof(path), "/dev/%s", name);
            JS_SetPropertyUint32(ctx, names, count++, JS_NewString(ctx, path));
        }
    }

    closedir(directory);
    return names;
}

#endif

/**
 * Reader thread of a port: posts the received bytes, then MESSAGE_CLOSED when
 * the port is closed. Owns 'argument', the PortState.
 */
void readLoop(void *argument) {
    PortState *state = (PortState *)argument;
    const size_t blockSize = 1024;
    uint8_t *block = nullptr;

    for (;;) {
        // stop when the JS thread sets 'closing' (seen within 100 ms, the
        // read timeout)
        if (__atomic_load_n(&state->closing, __ATOMIC_ACQUIRE)) {
            break;
        }

        // the bytes are read straight into the block posted to the loop (no
        // buffer on the stack, no copy); the block is reused while reads time
        // out empty
        if (block == nullptr) {
            block = (uint8_t *)memory::allocate(blockSize);
        }

        // read what is there; a read error closes the port
        int received = readPort(state->handle, block, blockSize);
        if (received < 0) {
            break;
        }

        // bytes: post the block shrunk to their size
        if (received > 0) {
            uint8_t *data = (uint8_t *)memory::reallocate(block, (size_t)received);
            state->mailbox->post(state->source, MESSAGE_DATA, data, received, 0);
            block = nullptr;   // now owned by the loop
        }
    }

    memory::release(block);

    // tell the JS thread, then let the state go (the last owner closes the
    // port: the JS side may still be writing)
    state->mailbox->post(state->source, MESSAGE_CLOSED, nullptr, 0, 0);
    releasePortState(state);
}

// Default speed of a port, bits per second.
const int DEFAULT_BAUD_RATE = 115200;

// Options of 'new Serial(path, options)'.
struct OpenOptions {
    js::Range<int, 300, 4000000> baudRate = DEFAULT_BAUD_RATE;   // bits per second
};
JS_STRUCT(OpenOptions, baudRate);

/**
 * An open serial port (JS class Serial), made by 'new Serial(path,
 * { baudRate })'. Life cycle: init() opens the port, registers the message source, starts the reader
 * thread and keeps the object alive; close() only asks the reader thread to
 * stop (the thread owns the PortState and closes the handle); its
 * MESSAGE_CLOSED removes the source, drops the keep-alive and calls onClose.
 */
class Serial {
    JS_CLASS(Serial);

public:
    /**
     * Finalizer (no JS here): asks the reader thread to stop if it still
     * runs, and removes the source.
     */
    ~Serial() {
        requestClose();
        if (_loop != nullptr) {
            _loop->removeSource(_source);
        }
    }

    /**
     * JS constructor: new Serial(path, { baudRate }) opens the port (115200
     * bauds by default). Throws if the port cannot be opened.
     */
    void init(js::Str path, js::Optional<OpenOptions> options) {
        int baudRate = options.has() ? (int)(options.value().baudRate) : DEFAULT_BAUD_RATE;
        start(path.data(), baudRate);
    }

    /**
     * Opens the port and starts reading (JS thread). Returns false with a
     * pending error.
     */
    bool start(const char *path, int baudRate) {
        // open and configure the port (fails with a system error)
        int error = 0;
        PortHandle handle = openPort(path, baudRate, &error);
        if (handle == invalidPort) {
            js::failSystem(error, path);
            return false;
        }

        // register the message source and allocate the PortState shared with
        // the reader thread (the mailbox is retained for the thread)
        snprintf(_path, sizeof(_path), "%s", path);
        _loop = rt::Loop::current();
        _source = _loop->addSource(&onMessage, this);
        _state = (PortState *)memory::allocate(sizeof(PortState));
        *_state = PortState{ handle, 0, 2, _loop->mailbox(), _source };
        _state->mailbox->retain();

        // open: the object stays alive until the close message
        _open = true;
        js::keepAlive(this, true);

        // start the reader thread, which owns the PortState from then on; if
        // it cannot start, close the port, undo the above (except the source,
        // removed by the destructor) and fail
        if (!platform::startThread(&readLoop, _state, platform::defaultThreadOptions("serial-read"))) {
            closePort(handle);
            _state->mailbox->release();
            memory::release(_state);
            _state = nullptr;
            _open = false;
            js::keepAlive(this, false);
            js::fail("serial: cannot start a thread");
            return false;
        }

        return true;
    }

    /**
     * JS: callback(bytes) for each block received (a new Uint8Array).
     */
    JS_EXPORT(onData) void onData(js::Function callback) {
        _dataCallback = callback;
    }

    /**
     * JS: callback() once the port is closed (by close() or after an error).
     */
    JS_EXPORT(onClose) void onClose(js::Function callback) {
        _closeCallback = callback;
    }

    /**
     * JS: writes a string (UTF-8) or bytes, synchronously on the JS thread.
     * Throws TypeError for a wrong type, a system error (EIO) if the write failed.
     */
    JS_EXPORT(write) void write(js::Value data) {
        // the port must be open, and still owned by this side (close() lets
        // the state go at once)
        if (!_open || _state == nullptr) {
            return (void)js::fail("serial: port closed");
        }

        // get the bytes: a C string of the JS string, or the byte view in place
        JSContext *ctx = js::context();
        const char *text = nullptr;
        JSValue buffer = JS_UNDEFINED;
        uint8_t *bytes = nullptr;
        size_t size = 0;
        bool ok = false;
        if (JS_IsString(data.get())) {
            text = JS_ToCStringLen(ctx, &size, data.get());
            bytes = (uint8_t *)((char *)text);
            ok = text != nullptr;
        } else {
            ok = js::detail::bytesOf(ctx, data.get(), &bytes, &size, &buffer);
        }

        // write them, then release what was taken; a wrong type is reported
        // after the release
        bool written = ok && writePort(_state->handle, bytes, size);
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, buffer);
        if (!ok) {
            return (void)js::fail<js::TypeError>("serial: write: data must be a string or bytes");
        }

        // the write failed
        if (!written) {
            return (void)js::failSystem(EIO, _path);
        }
    }

    /**
     * JS: asks the reader thread to stop; it closes the port within 100 ms.
     * The close callback is called when the reader thread has stopped.
     */
    JS_EXPORT(close) void close() {
        requestClose();
    }

    /**
     * JS: the path given to the constructor.
     */
    JS_EXPORT(path) const char *path() const {
        return _path;
    }

    /**
     * JS: false once the close message has been handled.
     */
    JS_EXPORT(isOpen) bool isOpen() const {
        return _open;
    }

private:
    /**
     * Sets the 'closing' flag read by the reader thread and lets this side's
     * share of the PortState go (the thread, last owner, closes the port).
     * Does nothing if already done.
     */
    void requestClose() {
        if (_state != nullptr) {
            __atomic_store_n(&_state->closing, 1, __ATOMIC_RELEASE);
            releasePortState(_state);   // the reader thread closes the port when it stops
            _state = nullptr;
        }
    }

    /**
     * Messages of the reader thread, on the JS thread: data, then the close.
     */
    static void onMessage(void *opaque, const rt::Message *message) {
        // MESSAGE_DATA: onData with a copy of the block (the loop frees the
        // block after this call). Called through a local copy of the
        // callback: the callback may replace itself (port.onData(...)), which
        // would free the function while it runs
        Serial *port = (Serial *)opaque;
        if (message->kind == MESSAGE_DATA) {
            js::Function callback = port->_dataCallback;
            callback(js::Buffer::copy(message->data, message->size));
            return;
        }

        // MESSAGE_CLOSED: mark the port closed, forget the PortState (already
        // freed by the thread), remove the source
        // (after a read error the JS side still owns the state: let it go now)
        port->_open = false;
        if (port->_state != nullptr) {
            releasePortState(port->_state);
            port->_state = nullptr;
        }

        port->_loop->removeSource(port->_source);

        // drop the keep-alive (the object may be collected after this), then
        // call onClose through a local copy of the callback, which stays valid
        // even if the object goes away
        js::Function callback = port->_closeCallback;
        js::keepAlive(port, false);
        callback();
    }

    char _path[256] = "";               // port name as given to the constructor
    rt::Loop *_loop = nullptr;          // loop of the JS thread; nullptr before start()
    rt::SourceId _source = { -1, 0 };   // message source of the reader thread
    PortState *_state = nullptr;        // freed by the reader thread; nullptr once closed
    bool _open = false;                 // true from start() until the close message
    js::Function _dataCallback;         // onData callback; empty: none
    js::Function _closeCallback;        // onClose callback; empty: none
};

/**
 * JS list(): the names of the serial ports of the machine (named serialList
 * in C++, like the other module functions).
 */
js::Value serialList() {
    JSContext *ctx = js::context();
    return js::Value(ctx, listPorts(ctx));
}

} // namespace

/**
 * Module "device/serial": list() and the class Serial.
 */
JS_MODULE(serial, module) {
    JS_FUNCTION_AS(module, serialList, "list");
    JS_ADD_CLASS(module, Serial);
}
