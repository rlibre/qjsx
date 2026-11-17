#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# What the firmware of the WT32-SC01 adds for this board (read by
# ports/esp32/main/CMakeLists.txt; optional in a board folder).

# its 3.5" ST7796 display: the module sys/draw (common/draw), for
# stdlib/display
set(QJSX_BOARD_DRAW ON)
