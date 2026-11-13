#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Loads the ESP-IDF v6.1 environment (the user's IDF PowerShell profile), then
# runs, in the qjsx ESP32 project (ports/esp32), for one board:
#   [board=NAME] ninja ...     ninja itself (e.g. "ninja -k 0": every error in one pass)
#   [board=NAME] esptool ...   esptool (e.g. "esptool -p COM11 flash-id": flash and PSRAM)
#   [board=NAME] ...           idf.py with the given arguments ("build", "-p COM11 flash")
# NAME is a folder of ports/esp32/boards (default yd-esp32-s3-n16r8); each board
# builds in ports/esp32/build-NAME. The output also goes to
# C:\temp\idf-build.log (UTF-16) and the exit code to C:\temp\idf-status.txt
# (written last).

# Python of ESP-IDF in UTF-8 (this process only): otherwise the profile warns
# "Your environment is not configured to handle Unicode characters"
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
Set-Location 'C:\Sandbox\espjs\ports\esp32'
Remove-Item 'C:\temp\idf-status.txt' -ErrorAction SilentlyContinue

# the board, then the tool and its arguments
$board = 'yd-esp32-s3-n16r8'
$words = @($args)
if ($words.Count -gt 0 -and $words[0] -like 'board=*') {
    $board = $words[0].Substring(6)
    $words = if ($words.Count -gt 1) { $words[1..($words.Count - 1)] } else { @() }
}

$buildDir = "build-$board"
$tool = if ($words.Count -gt 0) { $words[0] } else { '' }
$rest = if ($words.Count -gt 1) { $words[1..($words.Count - 1)] } else { @() }

# run it, the output also in the log
if ($tool -eq 'ninja') {
    ninja -C $buildDir @rest 2>&1 | Tee-Object -FilePath 'C:\temp\idf-build.log'
} elseif ($tool -eq 'esptool') {
    python -m esptool @rest 2>&1 | Tee-Object -FilePath 'C:\temp\idf-build.log'
} else {
    idf.py -B $buildDir -D "QJSX_BOARD=$board" @words 2>&1 | Tee-Object -FilePath 'C:\temp\idf-build.log'
}

Set-Content -Path 'C:\temp\idf-status.txt' -Value $LASTEXITCODE
