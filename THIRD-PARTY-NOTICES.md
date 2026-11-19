# Third-party notices

qjsx itself is under the MIT license (see `LICENSE`). What a release delivers
also contains the work of others, listed here with its license and its
copyright notice. The full text of the licenses that are not written out
below is in the `licenses/` folder.

Where each one is:

| Component | In the tool `qjsx` | In the firmwares | In `stdlib.qjslib` | License |
| --- | --- | --- | --- | --- |
| QuickJS | yes | yes | | MIT |
| Inter font | | | yes | SIL Open Font License 1.1 |
| x4js | yes (page of the digital twin) | | | MIT |
| normalize.css | yes (page of the digital twin) | | | MIT |
| Font Awesome Free icons | yes (page of the digital twin) | | | CC BY 4.0 |
| LittleFS | | yes | | BSD-3-Clause |
| esp_littlefs | | yes | | MIT |
| TinyUSB | | yes (boards with a USB device port) | | MIT, parts BSD-3-Clause |
| esp_tinyusb | | yes (boards with a USB device port) | | Apache-2.0 |
| ILI9881C panel tables | | yes (M5Stack Tab5, Waveshare ESP32-P4) | | Apache-2.0 |
| ESP-IDF and what it brings | | yes | | Apache-2.0 and others |
| MinGW-w64 runtime, GCC runtime | yes (Windows) | | | see below |
| GNU C Library (glibc) | yes (Linux Arm) | | | LGPL-2.1-or-later |

## QuickJS

The JavaScript engine, by Fabrice Bellard and Charlie Gordon
(https://bellard.org/quickjs/), modified for qjsx (`doc/engine-changes.md` in
the sources).

    QuickJS Javascript Engine

    Copyright (c) 2017-2021 Fabrice Bellard
    Copyright (c) 2017-2021 Charlie Gordon

License: MIT (text below).

The engine includes tables derived from the Unicode Character Database,
Copyright (c) 1991-2026 Unicode, Inc., under the Unicode License v3
(https://www.unicode.org/license.txt).

## Inter font

The fonts of the standard library (`stdlib/fonts/inter16`, `inter24`,
`inter40`) are made from Inter (https://github.com/rsms/inter).

    Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)

License: SIL Open Font License, Version 1.1: `licenses/OFL-1.1-Inter.txt`.

## x4js

The user interface library of the page of the digital twin
(https://www.npmjs.com/package/x4js).

    Copyright (c) 2026 R-libre ingenierie

License: MIT (text below).

## normalize.css

Style rules of x4js come from normalize.css v8.0.1
(https://github.com/necolas/normalize.css).

    Copyright (c) Nicolas Gallagher and Jonathan Neal

License: MIT (text below).

## Font Awesome Free icons

Icons of the page of the digital twin come from Font Awesome Free 6.5.1 by
@fontawesome (https://fontawesome.com), under the Creative Commons
Attribution 4.0 International license (https://fontawesome.com/license/free,
https://creativecommons.org/licenses/by/4.0/).

## LittleFS

The file system of the `/data` partition of a board
(https://github.com/littlefs-project/littlefs).

    Copyright (c) 2022, The littlefs authors.
    Copyright (c) 2017, Arm Limited. All rights reserved.

    Redistribution and use in source and binary forms, with or without modification,
    are permitted provided that the following conditions are met:

    -  Redistributions of source code must retain the above copyright notice, this
       list of conditions and the following disclaimer.
    -  Redistributions in binary form must reproduce the above copyright notice, this
       list of conditions and the following disclaimer in the documentation and/or
       other materials provided with the distribution.
    -  Neither the name of ARM nor the names of its contributors may be used to
       endorse or promote products derived from this software without specific prior
       written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
    ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
    ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## esp_littlefs

LittleFS for ESP-IDF, version 1.23.1
(https://github.com/joltwallet/esp_littlefs).

    Copyright 2020 Brian Pugh

License: MIT (text below).

## TinyUSB

The USB device stack of the boards that have a USB device port
(https://github.com/hathach/tinyusb), as packaged by Espressif
(`espressif/tinyusb`).

    Copyright (c) 2012-2026, hathach (tinyusb.org)

License: MIT (text below).

Its driver of the Synopsys DWC2 controller holds register definitions

    Copyright (c) 2019 STMicroelectronics. All rights reserved.

under the BSD-3-Clause license (https://opensource.org/licenses/BSD-3-Clause;
the same conditions as LittleFS's above, with the name of STMicroelectronics).

## esp_tinyusb

Espressif's layer over TinyUSB, version 2.4.0
(https://github.com/espressif/esp-usb).

    Copyright Espressif Systems (Shanghai) CO LTD

License: Apache License 2.0: `licenses/Apache-2.0.txt`.

## ILI9881C panel tables

The start-up commands of the display panels of the M5Stack Tab5 and of the
Waveshare 7-DSI-TOUCH-A (`common/drivers/ili9881c.cpp` in the sources) are
the values of the display drivers published by M5Stack and by Waveshare,
which derive from Espressif's `esp_lcd_ili9881c` component. The values were
rewritten in the form of qjsx; no other code was taken.

    SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
    SPDX-License-Identifier: Apache-2.0

License: Apache License 2.0: `licenses/Apache-2.0.txt`.

## ESP-IDF

The firmwares of the ESP32 boards are built with ESP-IDF v6.1
(https://github.com/espressif/esp-idf), Copyright Espressif Systems
(Shanghai) CO LTD, under the Apache License 2.0 (`licenses/Apache-2.0.txt`).
ESP-IDF brings components of other authors under their own licenses, among
them FreeRTOS (MIT), the C library, Mbed TLS and the ROM and radio libraries
of Espressif. Their list, with their copyright notices, is the file
`licenses/ESP-IDF-COPYRIGHT.rst` of this archive, copied from the ESP-IDF
version that built the firmwares.

## MinGW-w64 and GCC runtime (Windows executable)

`qjsx.exe` is built with GCC for MinGW-w64 and linked statically with:

- the MinGW-w64 runtime and winpthreads (https://www.mingw-w64.org),
  Copyright (c) the mingw-w64 project and its contributors, under permissive
  licenses (Zope Public License 2.1, MIT, BSD and public domain parts; file
  `COPYING.MinGW-w64-runtime.txt` of the MinGW-w64 sources). Its formatted
  output code derives from David M. Gay's gdtoa, Copyright (C) 1997-2001
  Lucent Technologies, whose license allows use, copy, modification and
  distribution provided its notice is kept;
- libgcc and libstdc++ (https://gcc.gnu.org), Copyright (C) Free Software
  Foundation, Inc., under the GPL version 3 with the GCC Runtime Library
  Exception 3.1, which allows their distribution inside a program under any
  license.

## GNU C Library (Linux Arm executable)

The `qjsx` executable for Linux Arm is linked statically with the GNU C
Library (glibc) of the Arm GNU Toolchain, Copyright (C) Free Software
Foundation, Inc., under the GNU Lesser General Public License, version 2.1
or later (https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html), and with
libgcc and libstdc++ (GCC Runtime Library Exception, as above).

The sources of that glibc are published with the toolchain:
https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads (source
tarball of the release used). To link the executable again with another
version of the library, build qjsx from its sources
(https://github.com/rlibre/qjsx): `premake5 gmake --cc=gcc --shell=posix
--cross=linux-arm`, then `make -C build-linux-arm config=release qjsx`.

## The MIT license

The components above marked "MIT" are under these terms, each with its own
copyright notice:

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
