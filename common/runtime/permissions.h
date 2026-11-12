/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What a program may reach of the machine, on Deno's model: files (read,
// write: folders), network (hosts), environment variables, serial ports.
// Each native module checks its operations here (sys/fs, sys/net,
// device/serial, process.env); nothing is allowed unless granted.
//
// Who sets them (doc/architecture.md 6.7):
//   - a program of the PC (an executable, "qjsx run"): the "desktop" section
//     of its manifest and the command line (host/permissions_host.cpp);
//   - a device program (a board, the desktop agent, a twin): no limit (user,
//     2026-11-12), only its working folder, the board's data folder
//     (agent::Runner);
//   - the tool's own commands: no Permissions at all, everything allowed.
//
// A run gives its Permissions in RunHooks; rt::runProgram makes them those
// of its thread for the whole run (several runtimes may run at the same time
// on different threads: the twin's page and the device program).
#pragma once

#include <stddef.h>

namespace rt {

enum class Access {
    read,   // read a file, list a folder, stat, exists
    write,  // create, write, rename, delete
};

// One kind of permission: nothing, a list of items, or everything.
struct PermissionList {
    char **items;      // owned strings (real folders, hosts, names, ports)
    int    count;      // entries of items
    bool   everything; // granted without limit
};

class Permissions {
public:
    /**
     * Nothing allowed, the system's current folder, a file system.
     */
    Permissions();

    /**
     * Frees the lists.
     */
    ~Permissions();
    Permissions(const Permissions &) = delete;
    Permissions &operator=(const Permissions &) = delete;

    // ### Granting (before the run) ##########################################

    /**
     * The program's current folder (process.cwd()), against which fs resolves
     * relative paths ('folder' copied, absolute). nullptr: the system's.
     */
    void        setWorkingFolder(const char *folder);

    /**
     * A device without file system: fs fails with ENOSYS, process.cwd() is "/".
     */
    void        setNoFileSystem();

    /**
     * Grants every kind without limit (-A).
     */
    void        allowEverything();

    /**
     * Grants reading (or writing) under 'folder' (real path taken now, so a
     * link in it is followed once, here). False with errno set if the path
     * cannot be resolved.
     */
    bool        allowFiles(Access access, const char *folder);

    /**
     * Grants reading (or writing) everywhere.
     */
    void        allowFilesEverywhere(Access access);

    /**
     * Grants the network: "host" (every port) or "host:port"; nullptr: every
     * host.
     */
    void        allowNet(const char *hostPort);

    /**
     * Grants an environment variable; nullptr: every variable.
     */
    void        allowEnv(const char *name);

    /**
     * Grants a serial port ("COM3", "/dev/ttyUSB0"); nullptr: every port.
     */
    void        allowSerial(const char *port);

    /**
     * The end of the message of a refusal: how to grant it ("allow it in ...").
     * Copied.
     */
    void        setHint(const char *hint);

    // ### Checking (JS thread of the run) ####################################

    /**
     * The program's current folder; nullptr: the system's.
     */
    const char *workingFolder() const;

    /**
     * False on a device without file system.
     */
    bool        hasFileSystem() const;

    /**
     * True if 'path' (absolute or relative to the system's current folder)
     * may be accessed: its real path (links followed, see files::realPath)
     * lies under a granted folder. False with errno EACCES otherwise.
     */
    bool        mayAccess(const char *path, Access access) const;

    /**
     * True if the program may connect to (or listen on) host:port.
     */
    bool        mayUseNet(const char *host, int port) const;

    /**
     * True if the program may read the environment variable 'name'.
     */
    bool        mayUseEnv(const char *name) const;

    /**
     * True if the program may open the serial port 'port'; nullptr: may it
     * list the ports (some port granted).
     */
    bool        mayUseSerial(const char *port) const;

    /**
     * The end of a refusal message ("" if none was set).
     */
    const char *hint() const;

private:
    char          *_workingFolder;  // owned; nullptr: the system's current folder
    bool           _fileSystem;     // false: no file system (ENOSYS)
    PermissionList _read;           // real folders that may be read
    PermissionList _write;          // real folders that may be written
    PermissionList _net;            // "host" or "host:port"
    PermissionList _env;            // variable names
    PermissionList _serial;         // port names
    char          *_hint;           // owned; how to grant a refused access
};

/**
 * Makes 'permissions' those of the calling thread (borrowed; nullptr: the
 * tool, everything allowed). rt::runProgram calls it.
 */
void setPermissions(const Permissions *permissions);

/**
 * The permissions of the calling thread's run; nullptr: everything allowed.
 */
const Permissions *permissions();

/**
 * Name of an access, for messages: "read" or "write".
 */
const char *accessName(Access access);

} // namespace rt
