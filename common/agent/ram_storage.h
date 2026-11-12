/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The package "loaded" in RAM (UPLOAD_BEGIN with VOLATILE): a second place
// next to the package installed in flash by the board. Nothing is written to
// the flash; the package is lost at the next restart of the chip (or of the
// desktop agent's process), and RUN starts it rather than the installed one
// while it is there. Its memory comes from the C heap, exactly the size of
// the package (memory::tryAllocate): a package too large is refused.
//
// Threads: uploads on the transport thread, acquire() / release() on the JS
// thread (the runner), clear() on either; a lock protects the packages. A
// package being run stays alive after a new upload replaced it, until the
// runner releases it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent.h"
#include "platform.h"

namespace agent {

struct RamPackage;   // ram_storage.cpp

class RamStorage : public Storage {
public:
    /**
     * Frees the packages (none may be acquired any more).
     */
                   ~RamStorage();

    /**
     * Allocates the room of the coming package. False if the RAM cannot hold
     * it (the current package stays).
     */
    bool           beginUpload(size_t size) override;

    /**
     * Copies a chunk into the coming package.
     */
    bool           writeUpload(size_t offset, const uint8_t *data, size_t size) override;

    /**
     * The coming package becomes the current one; the previous one is freed
     * (later, if the runner still runs it).
     */
    bool           finishUpload(const char *buildId) override;

    /**
     * Frees the coming package; the current one stays.
     */
    void           abortUpload() override;

    /**
     * Build id and size of the current package. False if none.
     */
    bool           installed(char *buildId, size_t buildIdSize, size_t *packageSize) override;

    /**
     * The current package for a run (JS thread): its bytes, size and build
     * id; kept alive until release(). nullptr if none.
     */
    const uint8_t *acquire(size_t *size, char *buildId, size_t buildIdSize);

    /**
     * Gives back what acquire() returned.
     */
    void           release(const uint8_t *package);

    /**
     * Drops the current package (a package installed in flash replaces it, a
     * desktop "reset" loses it); freed now or when its run releases it.
     */
    void           clear();

private:
    void           unreference(RamPackage *package);

    platform::Mutex _mutex;                // protects the fields below
    RamPackage     *_current = nullptr;    // the package RUN starts; owned (one reference)
    RamPackage     *_upload = nullptr;     // the package being received; owned
    RamPackage     *_running = nullptr;    // the package acquired by the runner (one reference)
};

} // namespace agent
