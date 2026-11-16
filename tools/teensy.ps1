#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Builds and flashes the qjsx i.MX RT1062 firmware (ports/imxrt1062), for one
# board, with what is installed in C:\tools: the NXP MCUXpresso SDK
# (C:\tools\nxp), the Arm GNU Toolchain, FreeRTOS-Kernel and ninja (installed
# for the RP2350 in C:\tools\pico), teensy_loader_cli (C:\tools\teensy);
# CMake from the PATH.
#   [board=NAME] configure [-DOPTION=value ...]
#                            CMake (first time, or after adding / removing a
#                            source); options stay in the CMake cache
#   [board=NAME] build       configure if needed, then ninja (default)
#   [board=NAME] flash       a board running qjsx is asked for its loader (its
#                            USB serial port set to 134 bauds, PJRC's
#                            convention), then teensy_loader_cli writes
#                            qjsx.hex and restarts it; without qjsx running,
#                            press the board's button
#   [board=NAME] ninja ...   ninja itself (e.g. "ninja -k 0": every error in one pass)
#   [board=NAME] loader ...  teensy_loader_cli itself
# NAME is a folder of ports/imxrt1062/boards (default teensy40); each board
# builds in ports/imxrt1062/build-NAME. The output also goes to
# C:\temp\teensy-build.log and the exit code to C:\temp\teensy-status.txt
# (written last).
$sdk = 'C:\tools\nxp\mcuxsdk-26.06.00-LTS\mcuxsdk'
$freertos = 'C:\tools\pico\FreeRTOS-Kernel'
$toolchain = 'C:\tools\pico\arm-gnu-toolchain-15.2.rel1'
$ninja = 'C:\tools\pico\ninja\ninja.exe'
$loader = 'C:\tools\teensy\teensy_loader_cli-2.3\teensy_loader_cli.exe'

# PJRC's USB ids of the Teensy serial port (the qjsx firmware uses them)
$serialIds = 'USB\VID_16C0&PID_0483*'
$loaderBaudRate = 134

Set-Location 'C:\Sandbox\espjs\ports\imxrt1062'
Remove-Item 'C:\temp\teensy-status.txt' -ErrorAction SilentlyContinue
$env:PATH = "$toolchain\bin;C:\tools\pico\ninja;$env:PATH"

# the board, then the command and its arguments
$board = 'teensy40'
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

# CMake with the paths of what is installed, '/' in the paths (CMake prefers
# them), then the options given after the command
function Configure {
    cmake -G Ninja -S . -B $buildDir `
        "-DCMAKE_MAKE_PROGRAM=$($ninja -replace '\\', '/')" `
        "-DCMAKE_TOOLCHAIN_FILE=toolchain.cmake" `
        "-DARM_TOOLCHAIN_PATH=$($toolchain -replace '\\', '/')" `
        "-DMCUXSDK_PATH=$($sdk -replace '\\', '/')" `
        "-DFREERTOS_KERNEL_PATH=$($freertos -replace '\\', '/')" `
        "-DQJSX_BOARD=$board" `
        -DCMAKE_BUILD_TYPE=Release `
        @rest 2>&1
}

# The COM port of a board running qjsx (its USB serial port), or $null.
function Find-SerialPort {
    $device = Get-CimInstance Win32_PnPEntity | Where-Object { $_.DeviceID -like $serialIds } | Select-Object -First 1
    if ($null -eq $device -or $device.Name -notmatch '\((COM\d+)\)') {
        return $null
    }
    return $Matches[1]
}

# Asks a board running qjsx for its loader: its port opened at 134 bauds.
function Request-Loader {
    $port = Find-SerialPort
    if ($null -eq $port) {
        "teensy.ps1: no board running qjsx found: press the board's button for its loader"
        return
    }

    "teensy.ps1: $port set to $loaderBaudRate bauds: the board goes to its loader"
    try {
        $serial = New-Object System.IO.Ports.SerialPort $port, $loaderBaudRate
        $serial.Open()
        $serial.Close()
    } catch {
        "teensy.ps1: $port did not take $loaderBaudRate bauds ($($_.Exception.Message)): press the board's button"
    }
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
        Request-Loader
        & $loader --mcu=TEENSY40 -w -v "$buildDir\qjsx.hex" 2>&1
    }
    'ninja' {
        & $ninja -C $buildDir @rest 2>&1
    }
    'loader' {
        & $loader @rest 2>&1
    }
    default {
        "teensy.ps1: unknown command '$command' (configure, build, flash, ninja, loader)"
        $global:LASTEXITCODE = 2
    }
}
$status = $LASTEXITCODE
$output | Tee-Object -FilePath 'C:\temp\teensy-build.log'

Set-Content -Path 'C:\temp\teensy-status.txt' -Value $status
