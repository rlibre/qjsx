/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The driver of device/serial for the serial ports of the PC (see
// serial_host.h, hal_serial.h). A read waits 100 ms at most and returns as
// soon as bytes are there.
//
// Windows: the port is opened for overlapped I/O, so that the reads of the
// reader thread and the writes of the JS thread do not wait for each other.
// On a port opened without it, Windows runs the operations one at a time: a
// write waited for the end of the pending read (100 ms when nothing comes),
// and the reader thread, which reads again at once, often got in first
// again, so that one write could take seconds.
#include "serial_host.h"
#include "memory.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

// An open port of the PC (hal_serial.h).
struct hal::serial::Port {
#if defined(_WIN32)
    HANDLE handle;                  // the COM port, opened for overlapped I/O; owned
#else
    int    handle;                  // the tty; owned
#endif
};

namespace {

using hal::serial::Parity;
using hal::serial::Port;
using hal::serial::Settings;

#if defined(_WIN32)

/**
 * The settings of an open COM port: speed, format, flow control, and read
 * timeouts (a read returns as soon as bytes are there, or after 100 ms).
 * False if Windows refused them.
 */
bool configurePort(HANDLE handle, const Settings *settings) {
    // start from the current state of the port
    DCB state;
    memset(&state, 0, sizeof(state));
    state.DCBlength = sizeof(state);
    GetCommState(handle, &state);

    // speed and character format
    state.BaudRate = (DWORD)settings->baudRate;
    state.ByteSize = (BYTE)settings->dataBits;
    state.Parity = settings->parity == Parity::even  ? EVENPARITY
                 : settings->parity == Parity::odd   ? ODDPARITY
                                                     : NOPARITY;
    state.fParity = settings->parity != Parity::none;
    state.StopBits = settings->stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    state.fBinary = TRUE;

    // no software flow control; RTS / CTS only if asked (else RTS keeps what
    // the port had)
    state.fOutX = FALSE;
    state.fInX = FALSE;
    state.fOutxDsrFlow = FALSE;
    state.fOutxCtsFlow = settings->flowControl ? TRUE : FALSE;
    if (settings->flowControl) {
        state.fRtsControl = RTS_CONTROL_HANDSHAKE;
    }

    // apply both
    COMMTIMEOUTS timeouts = { MAXDWORD, MAXDWORD, 100, 0, 0 };
    return SetCommState(handle, &state) && SetCommTimeouts(handle, &timeouts);
}

/**
 * Opens a COM port with 'settings' (see hal_serial.h for the errors).
 */
Port *openPort(const char *name, int, const Settings *settings, int *error) {
    // add the "\\.\" device prefix if missing ("COM10" and above need it)
    char device[300];
    snprintf(device, sizeof(device), strncmp(name, "\\\\.\\", 4) == 0 ? "%s" : "\\\\.\\%s", name);

    // open it for reading and writing, not shared, for overlapped I/O (reads
    // and writes are then independent, see the top of the file)
    DWORD access = GENERIC_READ | GENERIC_WRITE;
    HANDLE handle = CreateFileA(device, access, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD failure = GetLastError();
        *error = failure == ERROR_FILE_NOT_FOUND ? ENOENT : failure == ERROR_ACCESS_DENIED ? EBUSY : EIO;
        return nullptr;
    }

    // set it up, or close it
    if (!configurePort(handle, settings)) {
        CloseHandle(handle);
        *error = EINVAL;
        return nullptr;
    }

    Port *port = (Port *)memory::allocate(sizeof(Port));
    port->handle = handle;
    return port;
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
 * Reads what is available, waiting 100 ms at most (see configurePort).
 */
int readPort(Port *port, uint8_t *buffer, size_t size) {
    OVERLAPPED operation;
    if (!beginOperation(&operation)) {
        return -1;
    }

    // start the read, then wait for bytes or for the timeout
    DWORD received = 0;
    BOOL started = ReadFile(port->handle, buffer, (DWORD)size, nullptr, &operation);
    if (!endOperation(port->handle, started, &operation, &received)) {
        return -1;
    }

    return (int)received;
}

/**
 * Writes everything, looping on partial writes.
 */
bool writePort(Port *port, const uint8_t *data, size_t size) {
    while (size > 0) {
        OVERLAPPED operation;
        if (!beginOperation(&operation)) {
            return false;
        }

        // start the write, then wait until the bytes are taken
        DWORD written = 0;
        BOOL started = WriteFile(port->handle, data, (DWORD)size, nullptr, &operation);
        if (!endOperation(port->handle, started, &operation, &written) || written == 0) {
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
void closePort(Port *port) {
    CloseHandle(port->handle);
    memory::release(port);
}

/**
 * Serial ports declared by the drivers, from the registry (none if the key
 * is missing).
 */
void listPorts(hal::serial::ListHandler *add, void *context) {
    // open HKLM\HARDWARE\DEVICEMAP\SERIALCOMM
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }

    // each value is one port, its data the name; one byte is kept for the
    // terminating zero
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
            add(context, data, -1);
        }
    }

    RegCloseKey(key);
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
 * The termios bits of a character size.
 */
tcflag_t sizeBits(int dataBits) {
    switch (dataBits) {
        case 5: {
            return CS5;
        }
        case 6: {
            return CS6;
        }
        case 7: {
            return CS7;
        }
        default: {
            return CS8;
        }
    }
}

/**
 * Raw mode at 'speed' with the format and flow control of 'settings'; reads
 * return what is there, or nothing after 100 ms (VMIN 0, VTIME 1).
 */
void makeSettings(struct termios *state, speed_t speed, const Settings *settings) {
    // raw, receiver on, modem lines ignored
    cfmakeraw(state);
    state->c_cflag |= CLOCAL | CREAD;

    // character format: size, parity, stop bits
    state->c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
    state->c_cflag |= sizeBits(settings->dataBits);
    if (settings->parity != Parity::none) {
        state->c_cflag |= PARENB;
    }

    if (settings->parity == Parity::odd) {
        state->c_cflag |= PARODD;
    }

    if (settings->stopBits == 2) {
        state->c_cflag |= CSTOPB;
    }

#ifdef CRTSCTS
    // hardware flow control (not POSIX: Linux and macOS have it)
    state->c_cflag &= ~CRTSCTS;
    if (settings->flowControl) {
        state->c_cflag |= CRTSCTS;
    }

#endif

    // speed and read timing
    cfsetispeed(state, speed);
    cfsetospeed(state, speed);
    state->c_cc[VMIN] = 0;
    state->c_cc[VTIME] = 1;   // 100 ms
}

/**
 * Opens a tty with 'settings' (see hal_serial.h for the errors; EINVAL if
 * the system has no constant for the baud rate).
 */
Port *openPort(const char *name, int, const Settings *settings, int *error) {
    // map the baud rate
    speed_t speed = speedOf(settings->baudRate);
    if (speed == 0) {
        *error = EINVAL;
        return nullptr;
    }

#ifndef CRTSCTS
    // no hardware flow control on this system
    if (settings->flowControl) {
        *error = EINVAL;
        return nullptr;
    }

#endif

    // open the device, not as the controlling terminal of the process
    int handle = open(name, O_RDWR | O_NOCTTY);
    if (handle < 0) {
        *error = errno;
        return nullptr;
    }

    // the new settings, from the current ones
    struct termios state;
    bool configured = tcgetattr(handle, &state) == 0;
    if (configured) {
        makeSettings(&state, speed, settings);
        configured = tcsetattr(handle, TCSANOW, &state) == 0;
    }

    if (!configured) {
        *error = errno;
        close(handle);
        return nullptr;
    }

    Port *port = (Port *)memory::allocate(sizeof(Port));
    port->handle = handle;
    return port;
}

/**
 * Reads what is available, waiting 100 ms at most (see makeSettings).
 */
int readPort(Port *port, uint8_t *buffer, size_t size) {
    ssize_t received = read(port->handle, buffer, size);

    // a signal interrupted the wait: nothing read, the port stays open
    if (received < 0 && errno == EINTR) {
        return 0;
    }

    return received < 0 ? -1 : (int)received;
}

/**
 * Writes everything, looping on partial writes and on writes interrupted by
 * a signal.
 */
bool writePort(Port *port, const uint8_t *data, size_t size) {
    while (size > 0) {
        ssize_t written = write(port->handle, data, size);
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
void closePort(Port *port) {
    close(port->handle);
    memory::release(port);
}

/**
 * USB serial adapters: /dev/ttyUSB*, /dev/ttyACM* (Linux), /dev/cu.* (macOS),
 * in directory order.
 */
void listPorts(hal::serial::ListHandler *add, void *context) {
    // scan /dev
    DIR *directory = opendir("/dev");
    if (directory == nullptr) {
        return;
    }

    // keep the entries whose name is a USB serial adapter
    struct dirent *entry;

    while ((entry = readdir(directory)) != nullptr) {
        const char *name = entry->d_name;
        bool isSerial = strncmp(name, "ttyUSB", 6) == 0 || strncmp(name, "ttyACM", 6) == 0
                     || strncmp(name, "cu.", 3) == 0;
        if (isSerial) {
            char path[300];
            snprintf(path, sizeof(path), "/dev/%s", name);
            add(context, path, -1);
        }
    }

    closedir(directory);
}

#endif

const hal::serial::Driver HOST_DRIVER = {
    true,                           // ports named by the system
    &openPort,
    &readPort,
    &writePort,
    &closePort,
    &listPorts,
};

} // namespace

namespace host {

/**
 * The driver of the PC's serial ports.
 */
const hal::serial::Driver *serialDriver() {
    return &HOST_DRIVER;
}

} // namespace host
