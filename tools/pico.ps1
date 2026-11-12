#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Builds and flashes the qjsx RP2350 firmware (ports/rp2350) with the toolchain
# installed in C:\tools\pico (Arm GNU Toolchain, pico-sdk, FreeRTOS-Kernel,
# picotool, ninja; CMake from the PATH), for one board:
#   [board=NAME] configure [-DOPTION=value ...]
#                            CMake (first time, or after adding / removing a
#                            source), with options of ports/rp2350/CMakeLists.txt
#                            (e.g. -DQJSX_JS_HEAP_IN_SRAM=ON); options stay in
#                            the CMake cache until changed again
#   [board=NAME] build       configure if needed, then ninja (default)
#   [board=NAME] flash       picotool: reboots a board running qjsx into BOOTSEL
#                            (or takes one already in BOOTSEL), loads the
#                            firmware, then runs it
#   [board=NAME] ninja ...   ninja itself (e.g. "ninja -k 0": every error in one pass)
#   [board=NAME] picotool ...  picotool itself (e.g. "picotool info -a")
# NAME is a folder of ports/rp2350/boards (default olimex-pico2-xxl); each board
# builds in ports/rp2350/build-NAME. The output also goes to
# C:\temp\pico-build.log and the exit code to C:\temp\pico-status.txt (written
# last).
$tools = 'C:\tools\pico'
$sdk = "$tools\pico-sdk"
$freertos = "$tools\FreeRTOS-Kernel"
$toolchain = "$tools\arm-gnu-toolchain-15.2.rel1"
$ninja = "$tools\ninja\ninja.exe"
$picotool = "$tools\picotool\picotool\picotool.exe"
$picotoolDir = "$tools\picotool\picotool"

Set-Location 'C:\Sandbox\espjs\ports\rp2350'
Remove-Item 'C:\temp\pico-status.txt' -ErrorAction SilentlyContinue
$env:PATH = "$toolchain\bin;$tools\ninja;$env:PATH"

# the board, then the command and its arguments
$board = 'olimex-pico2-xxl'
$words = @($args)
if ($words.Count -gt 0 -and $words[0] -like 'board=*') {
    $board = $words[0].Substring(6)
    $words = @($words | Select-Object -Skip 1)
}

# @(...) keeps a single argument an array (an "if" expression would unroll it
# into a string, and @rest would then splat its letters)
$buildDir = "build-$board"
$command = if ($words.Count -gt 0) { $words[0] } else { 'build' }
$rest = @($words | Select-Object -Skip 1)

# CMake with the paths of the toolchain, '/' in the paths (CMake prefers them),
# then the options given after the command
function Configure {
    cmake -G Ninja -S . -B $buildDir `
        "-DCMAKE_MAKE_PROGRAM=$($ninja -replace '\\', '/')" `
        "-DQJSX_BOARD=$board" `
        "-DPICO_SDK_PATH=$($sdk -replace '\\', '/')" `
        "-DFREERTOS_KERNEL_PATH=$($freertos -replace '\\', '/')" `
        "-DPICO_TOOLCHAIN_PATH=$($toolchain -replace '\\', '/')" `
        "-Dpicotool_DIR=$($picotoolDir -replace '\\', '/')" `
        -DCMAKE_BUILD_TYPE=Release `
        @rest 2>&1
}

# run the command, the output also in the log
$output = switch ($command) {
    'configure' {
        Configure
    }
    'build' {
        if (-not (Test-Path "$buildDir\build.ninja")) {
            Configure
        }
        & $ninja -C $buildDir @rest 2>&1
    }
    'flash' {
        & $picotool load -f -x "$buildDir\qjsx.uf2" 2>&1
    }
    'ninja' {
        & $ninja -C $buildDir @rest 2>&1
    }
    'picotool' {
        & $picotool @rest 2>&1
    }
    default {
        "pico.ps1: unknown command '$command' (configure, build, flash, ninja, picotool)"
        $global:LASTEXITCODE = 2
    }
}
$status = $LASTEXITCODE
$output | Tee-Object -FilePath 'C:\temp\pico-build.log'

Set-Content -Path 'C:\temp\pico-status.txt' -Value $status
