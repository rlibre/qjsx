# Proposal: a qjsx port for the STM32H755 (ST NUCLEO-H755ZI-Q)

> **Status: proposal awaiting the user's decisions, written on 2026-11-12.** Nothing is
> coded, nothing is downloaded. Every choice below marked **(decision)** waits for the user;
> section 13 lists them all.
>
> **Sources.** `www.st.com` did not answer from the research environment on 2026-11-12
> (every fetch of a PDF timed out), so the ST documents themselves (UM2408, RM0399, the
> datasheet) could **not** be read. What is stated here comes from ST's own sources on
> GitHub (STM32CubeH7 templates, HAL and CMSIS repositories), the Zephyr board and SoC
> descriptions, the FreeRTOS kernel sources and ST community answers, each cited where it
> is used. Facts that come from memory only are marked *(to check)*: they must be checked
> against UM2408 / RM0399 / the MB1363 schematic before any code is written. Chapter titles
> of the ST documents are given instead of section numbers that could not be verified.

## 1. The board

The user wrote "nucleo-H755ZI-A". No ST board has that name. The board assumed here is
the **NUCLEO-H755ZI-Q** (ST order code NUCLEO-H755ZI-Q, board reference **MB1363**),
described with its sisters in user manual **UM2408** "STM32H7 Nucleo-144 boards (MB1363)".
The "-Q" suffix marks the Nucleo-144 boards with an STLINK-V3 and an SMPS-capable chip.
**(decision 1: confirm the board printed on the box / the silkscreen.)**

| Item | Value | Source |
|---|---|---|
| MCU | STM32H755ZIT6, LQFP144, Cortex-M7 (480 MHz max) + Cortex-M4 (240 MHz max), both with FPU; the M7 has a **double-precision** FPU | [Zephyr board page](https://docs.zephyrproject.org/latest/boards/st/nucleo_h755zi_q/doc/index.html) |
| Flash | 2 MB, two banks of 1 MB, 128 KB sectors, 32-byte programming unit | [Zephyr stm32h745.dtsi](https://github.com/zephyrproject-rtos/zephyr/blob/main/dts/arm/st/h7/stm32h745.dtsi) |
| RAM | ~1 MB in eight blocks (section 3) | Zephyr `stm32h7.dtsi`, `stm32h745.dtsi` |
| Crypto | the "5" of H755 = H745 + CRYP/HASH; every NUCLEO-H745ZI-Q project runs on it | [STM32CubeH7 `Projects/NUCLEO-H755ZI-Q/readme.txt`](https://github.com/STMicroelectronics/STM32CubeH7/tree/master/Projects/NUCLEO-H755ZI-Q) |
| Debugger | on-board **STLINK-V3E**: SWD, virtual COM port, USB mass storage | Zephyr board page |
| Host serial | ST-LINK virtual COM port on **USART3, PD8 (TX) / PD9 (RX)** | Zephyr board page |
| User USB | USB OTG FS, micro-AB connector | Zephyr board page |
| Ethernet | RMII PHY (LAN8742A on the H7 Nucleo-144 boards *(to check)*), RJ45 | Zephyr board page, UM2408 |
| LEDs | LD1 green **PB0** (Zephyr's H745ZI-Q board; Zephyr's H755 page says PA5: UM2408 shows a solder-bridge choice between the two *(to check)*), LD2 yellow **PE1**, LD3 red **PB14** | [Zephyr nucleo_h745zi_q.dtsi](https://github.com/zephyrproject-rtos/zephyr/blob/main/boards/st/nucleo_h745zi_q/nucleo_h745zi_q.dtsi), Zephyr board page |
| Button | B1 USER on **PC13** (B2 is RESET) | Zephyr dtsi |
| Clocks | HSE = **8 MHz from the ST-LINK's MCO, bypass mode**; LSE 32.768 kHz crystal | [ST template `BootCM7_CM4Gated` main.c](https://github.com/STMicroelectronics/STM32CubeH7/tree/master/Projects/NUCLEO-H745ZI-Q/Templates/BootCM7_CM4Gated), Zephyr board page |
| No PSRAM, no SD card, no RGB LED | | |

### 1.1 Power supply: the SMPS trap

The STM32H755 has an internal step-down converter (SMPS) and an LDO for its core voltage.
How they are wired is a property of the **board**, and the firmware must tell the chip the
same thing, in `PWR_CR3`, once after each power-on reset (the register can only be written
once per power-on: a later write is ignored). On the NUCLEO-H755ZI-Q the default wiring is
**SMPS direct supply** (the SMPS feeds VCORE, the LDO is off): UM2408 chapter "Power supply",
table "SMPS / LDO configuration" (configuration "SMPS only", the default)
([ST community](https://community.st.com/t5/stm32cubemx-mcus/default-settings-for-nucleo-h755-not-working-after-power-off/td-p/759842);
Zephyr: `power-supply = "smps-direct"`).

- **What the firmware must do**, first thing after reset, before touching the clocks:
  `HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY)` (SMPSEN = 1, LDOEN = 0, BYPASS = 0), then
  wait for `ACTVOSRDY`. Never `PWR_LDO_SUPPLY`, the default of many generated projects
  (`USE_PWR_LDO_SUPPLY`).
- **What a wrong setting does**: the core voltage does not come up as the firmware expects;
  the chip stops before it can answer the debugger, at every power-on, since the firmware
  writes the wrong value again each time. The ST-LINK then reports "no target found".
- **Recovery** (ST community, reported by several users, *to check against ST's knowledge
  article*): power off; tie **BOOT0 to VDD** (on this board a wire from **CN11 pin 7
  (BOOT0) to CN11 pin 5 (VDD)**); power on: the chip starts the system bootloader instead
  of the firmware, so the wrong value is not written; connect STM32CubeProgrammer
  ("under reset", or "power down" mode as one user reports), **full chip erase**; remove the
  wire; flash a firmware with the right setting.
  ([thread 1](https://community.st.com/t5/stm32-mcus-boards-and-hardware/nucleo-h755zi-q-how-can-i-resolve-st-link-quot-no-device-found/m-p/632372),
  [thread 2](https://community.st.com/t5/stm32cubemx-mcus/default-settings-for-nucleo-h755-not-working-after-power-off/m-p/760162))
- **In the port**: the supply mode is a constant of the board (`board_config.h`), not of the
  chip family: a WeAct STM32H743 (the board of the backlog) has no SMPS at all and must use
  the LDO. A wrong board folder is the most likely way to lock a board.

### 1.2 Clock: 400 MHz, not 480

480 MHz needs voltage scale **VOS0**, and VOS0 is only available when the **LDO** supplies
the core. In SMPS direct mode the highest scale is VOS1, limited to **400 MHz** for the M7
(200 MHz for the M4 and the AXI/AHB buses). This is what ST's own FAQ says
([ST community FAQ](https://community.st.com/t5/stm32-mcus/faq-why-the-direct-smps-supply-option-is-not-shown-in/ta-p/853623))
and what the ST templates for this board use: "SYSCLK = 400000000", PLL from the 8 MHz HSE
in bypass, M = 4, N = 400, P = 2, VOS1, flash latency 4 wait states
([template main.c](https://github.com/STMicroelectronics/STM32CubeH7/tree/master/Projects/NUCLEO-H745ZI-Q/Templates/BootCM7_CM4Gated)).
Running at 480 MHz needs the board rewired for the LDO (solder bridges, UM2408), and users
report cold-boot failures and brown-outs on USB power at 480 MHz. **Recommendation: 400 MHz,
SMPS direct, the board as delivered.**

Proposed tree: M7 400 MHz; AXI / AHB (HPRE / 2) 200 MHz; APB1..4 100 MHz (timers 200 MHz);
flash wait states from RM0399 "Embedded flash memory", table of wait states versus AXI
clock and voltage scale (the ST template uses 4; the table probably allows 2 at 200 MHz in
VOS1 *(to check)*: fewer wait states speed up the code that stays in flash).

### 1.3 Two cores: which one runs qjsx, and the M4

**qjsx runs on the Cortex-M7**, alone. It is the faster core, the one that owns the clock
and power configuration ("Clock configuration is only accessible to M7 core", Zephyr board
page), and it has the TCMs and the double-precision FPU. FreeRTOS runs single-core on it.

The M4 boots or not according to two **option bytes** (non-volatile, in the flash's option
area): **BCM7** and **BCM4** (boot enable of each core), with their boot addresses
**BOOT_CM7_ADD0 = 0x08000000** and **BOOT_CM4_ADD0 = 0x08100000** (start of bank 2) by
default (Zephyr board page). As delivered, both cores boot *(to check: Nucleo boards ship
with BCM4 = BCM7 = 1 and a demo firmware in both banks)*. That matters here: bank 2 is where
this proposal puts the packages (section 4), and an M4 that boots from 0x08100000 would run
whatever is there.

Three ways out **(decision 2)**:

| Option | How | Cost |
|---|---|---|
| **A. Gate the M4 (recommended)** | Set **BCM4 = 0** once with STM32CubeProgrammer (`-ob BCM4=0`). The M4's clock is then gated at reset; the M7 could release it later with `HAL_RCCEx_EnableBootCore(RCC_BOOT_C2)`, which qjsx never does. This is exactly ST's template `BootCM7_CM4Gated` ("BOOT_CM4(BCM4) = 0, BOOT_CM7(BCM7) = 1") | one persistent change of the board's option bytes, by hand, documented; the firmware reads `FLASH->OPTSR_CUR` at boot and prints a warning when BCM4 is still set |
| B. Park the M4 in bank 2 | Keep BCM4 = 1; the firmware image also writes, at 0x08100000, a few bytes of M4 code (vector table + `WFE` loop), as ST's dual-core templates embed the M4 image in the M7 hex | no option byte change, but a whole 128 KB sector of bank 2 is lost (sectors are the erase unit): package slots shrink from 512 KB to 384 KB |
| C. Use the M4 | later only: an I/O coprocessor, a second JS runtime... | out of scope |

### 1.4 Default boot of the board

BOOT0 is pulled low on the board: the M7 starts from BOOT_CM7_ADD0 (0x08000000, bank 1).
With BOOT0 high (CN11 pin 7 to VDD) the system bootloader runs instead: that is the recovery
path of 1.1. The 8 MHz HSE comes from the ST-LINK: if the board is powered without the
ST-LINK USB (external 5 V), check that the ST-LINK still runs (it is powered from the same
5 V on these boards *(to check)*), or fall back to HSI.

## 2. Toolchain, SDK, flashing

### 2.1 Recommendation

| Part | Recommendation | Why |
|---|---|---|
| Compiler | **Arm GNU Toolchain 15.2** already in `C:\tools\pico\arm-gnu-toolchain-15.2.rel1` | it targets every Cortex-M; its newlib has the `thumb/v7e-m+dp/hard` multilib for `-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard`; GCC 15 needs the engine options already in `qjsx.cmake` (`-Wno-incompatible-pointer-types`...). No download |
| RTOS | **FreeRTOS-Kernel V11.3.3** already in `C:\tools\pico\FreeRTOS-Kernel`, single-core, port **`portable/GCC/ARM_CM4F`** if the core is not r0p1, else `ARM_CM7/r0p1` | the kernel's own [ReadMe](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/portable/GCC/ARM_CM7/ReadMe.txt): "If in doubt, use the FreeRTOS port provided specifically for r0p1", recommended `ARM_CM4F` otherwise. The STM32H7's M7 is believed to be r1p1 *(to check: errata sheet of the STM32H745/755, and `SCB->CPUID` read on the board in step 1)*. No download |
| Chip support | **CMSIS device header + startup + LL/HAL drivers from ST's GitHub repositories**, not the whole STM32CubeH7 package | only three small repositories are needed; STM32CubeH7 (v1.13.0 on GitHub) is a large package with git submodules and hundreds of example projects, useful to read (on GitHub) rather than to install |
| Driver style | **LL drivers and registers** for what runs often (GPIO, UART, timers); **HAL** where it saves real work and runs rarely (PWR supply, RCC/PLL, FLASH erase and program, option bytes read) | the HAL's handles and callbacks are heavy; the hot paths are a few register accesses |
| Build | our own CMake project (like `ports/rp2350/CMakeLists.txt`) with a small **toolchain file**, **ninja** from `C:\tools\pico\ninja`, CMake from the PATH | the same shape as the RP2350 port, no vendor build system |
| Linker script | **ours**, written for qjsx (memory regions of section 3, `.itcm` and `.dtcm` sections) | ST's generated scripts put everything in DTCM 128 KB and the heap after `.bss` |
| Flashing | **STM32CubeProgrammer CLI** (`STM32_Programmer_CLI -c port=SWD mode=UR -w qjsx.elf -v -rst`) behind a `tools/stm32.ps1` like `tools/pico.ps1` | it also reads / writes option bytes (BCM4), does the full erase of the recovery, and updates the ST-LINK firmware |
| Alternative flashing | **xPack OpenOCD** (`stm32h7x` driver: flash banks, mass erase, option bytes) | no ST account needed; less convenient for option bytes |
| Quick flashing | drag and drop of `qjsx.bin` on the ST-LINK's USB drive | fine for a firmware update; cannot erase bank 2, set option bytes nor recover a locked chip |

### 2.2 Downloads to approve

Nothing below was downloaded. Sizes are **estimates** (GitHub does not show them before
download) unless stated.

| # | What | Version | Official URL | Size (approx.) | Needed |
|---|---|---|---|---|---|
| 1 | `cmsis-device-h7` (device headers `stm32h755xx.h`, `system_stm32h7xx*.c`, GCC startup files) | **v1.10.7** (tag of 2026-02-16) | https://github.com/STMicroelectronics/cmsis-device-h7 | 5-10 MB zip (each device header is ~2 MB of text) | yes |
| 2 | `stm32h7xx-hal-driver` (HAL + LL) | **v1.11.6** (tag of 2026-02-16) | https://github.com/STMicroelectronics/stm32h7xx-hal-driver | ~5 MB zip | yes |
| 3 | CMSIS Core headers (`core_cm7.h`, `cmsis_gcc.h`...) | ST's fork **v5.9.0**, matching the HAL generation (ST also tags a 6.3.0) | https://github.com/STMicroelectronics/cmsis_core | unknown, possibly tens of MB (it may hold the DSP library): only `Include/` is used. Alternative: `CMSIS/Core` of https://github.com/ARM-software/CMSIS_6 | yes |
| 4 | **STM32CubeProgrammer** (GUI + CLI, includes the ST-LINK USB drivers and the ST-LINK firmware upgrade) | **2.23** (released 2026-07; release note RN0109 rev 34) | https://www.st.com/en/development-tools/stm32cubeprog.html (free **ST account login required**) | ~300 MB installer *(to check)* | yes (recommended), or #5 |
| 5 | xPack OpenOCD (alternative to #4, no account) | **v0.12.0-7** (2025-10) | https://github.com/xpack-dev-tools/openocd-xpack/releases | ~5 MB zip | optional |
| 6 | ST documents (PDF, no login): **UM2408** (the board), **MB1363 schematic**, **RM0399** (reference manual STM32H745/755/747/757), the **STM32H755xI datasheet** (DS12919 *(number to check)*), its **errata sheet**, **AN4838** (MPU), **AN4839** (L1 cache), **AN5361** (dual-core STM32H7 projects) | latest | https://www.st.com/en/evaluation-tools/nucleo-h755zi-q.html (Documentation tab) | a few MB each; RM0399 ~30 MB | yes, to verify this proposal |

Not needed for the first steps: the full STM32CubeH7 package (v1.13.0), lwIP and the
LAN8742 driver (Ethernet, later), a USB stack (later; TinyUSB is already on the disk as a
submodule of the pico-sdk and supports the STM32H7's USB controller), STM32CubeIDE /
STM32CubeMX. Proposed location: `C:\tools\stm32\` (as `C:\tools\pico\`), each archive's
SHA-256 checked as for the RP2350 toolchain.

## 3. Memory map and where things go

### 3.1 The memories of the chip (M7 view)

| Memory | Address | Size | Bus / speed for the M7 | Source |
|---|---|---|---|---|
| ITCM RAM | 0x0000_0000 | 64 KB | tightly coupled, 0 wait state, instructions and data | Zephyr `stm32h7.dtsi`; ST template readme ("ITCM-RAM (64 KB) allowing zero wait state access") |
| Flash bank 1 | 0x0800_0000 | 1 MB | AXI, through the 16 KB I-cache / D-cache | Zephyr `stm32h745.dtsi` |
| Flash bank 2 | 0x0810_0000 | 1 MB | same | same |
| DTCM RAM | 0x2000_0000 | 128 KB | tightly coupled, 0 wait state, data only; **not reachable by DMA1/DMA2** (MDMA only) | Zephyr; ST template readme |
| AXI SRAM (D1) | 0x2400_0000 | **512 KB** | 64-bit AXI at 200 MHz, cached | Zephyr `stm32h745.dtsi` |
| SRAM1, SRAM2, SRAM3 (D2) | 0x3000_0000 | 128 + 128 + 32 KB, contiguous | AHB at 200 MHz, cached; reachable by every DMA, Ethernet, USB | Zephyr |
| SRAM4 (D3) | 0x3800_0000 | 64 KB | AHB, D3 domain (BDMA) | Zephyr |
| Backup SRAM | 0x3880_0000 | 4 KB | kept in standby / VBAT | Zephyr |

Total ~1 MB of RAM, of which the largest contiguous block is the 512 KB AXI SRAM.

### 3.2 Proposed placement

| What | Where | Size | Why |
|---|---|---|---|
| Hot engine code (`JS_CallInternal` ~25 KB + the ~30 functions of `QJSX_SRAM_FUNCTIONS`, ~15 KB) | **ITCM**, copied from flash at boot | ~40 KB of 64 | 0 wait state, never evicted, as on the RP2350 (section 8) |
| Vector table, FreeRTOS's PendSV / SysTick handlers, UART interrupt | ITCM | ~2 KB | same reason |
| Stacks: JS task (48 KB), link reader, timer task, idle, main stack | **DTCM** (a FreeRTOS heap_4 / heap_5 region placed in DTCM, or static task buffers) | ~70 KB | QuickJS keeps the JS values of every frame on the C stack (`alloca` in `JS_CallInternal`): 0-wait-state stack accesses matter |
| `.data`, `.bss` of the firmware | DTCM | ~20-30 KB *(to measure)* | small and hot |
| **JS heap** | **AXI SRAM, all of it**, managed by `memory::RegionHeap` (`board::jsAllocator()` = `memory::jsAllocator(&axiHeap, false)`) | **~512 KB** | the largest block; `RegionHeap` was designed for it ("later the AXI SRAM of an STM32H7", `region_heap.h`) |
| C heap (`malloc` → `memory::allocate`) | **SRAM1-3**, 288 KB, through our `_sbrk` | 288 KB | the agent's frame parser alone takes 2 × 64 KB (`DEFAULT_MAX_MESSAGE`, `common/agent/agent.cpp`); later the DMA buffers of Ethernet / RMT, which cannot live in DTCM |
| SRAM4 | free (later: BDMA buffers, or a second region of the C heap) | 64 KB | |
| Backup SRAM | free (lead: keep the text of the last `fatal()` across the restart) | 4 KB | |

**What 512 KB of JS heap means.** The other boards have 8 MB of PSRAM. Here a program and
its data must fit in about half a megabyte, *including the bytecode of every module*: the
engine copies each module's bytecode from the package into the JS heap when it loads it
(`JS_ReadObject` without `JS_READ_OBJ_ROM_DATA`, new-device.md 4.9). Expect "small
programs": a few thousand lines, a few thousand live objects. Two things to measure in
step 2: the heap taken by an empty runtime and context, and by `tests/fixtures/debuggee`.
Leads if it is too tight: running the bytecode in place from flash
(`JS_READ_OBJ_ROM_DATA`, already listed as "to verify" for the ESP32), and putting SRAM1-3
in the JS heap too (a second `RegionHeap` region, or a C heap moved to SRAM4 + DTCM).
**(decision 3: the split JS heap / C heap.)**

### 3.3 Caches and MPU

The M7 has a 16 KB instruction cache and a 16 KB data cache, off at reset. Both are turned
on at boot (`SCB_EnableICache`, `SCB_EnableDCache`). Coherence questions, with the rules
of AN4839 "Level 1 cache on STM32F7 and STM32H7" and AN4838 "MPU management":

- **Flash and packages.** The packages are read through the D-cache (bank 2 is cacheable by
  the default memory map). After erasing or programming a range, **invalidate the D-cache
  lines of that range** (`SCB_InvalidateDCache_by_Addr`), or the header read just before the
  write stays in the cache. This is the equivalent of the XIP cache flush the pico-sdk does.
- **DMA.** Nothing in the first firmware uses DMA (the UART works by interrupts, the USB
  OTG FS has its own FIFOs). Later: the RMT buffers (clean before the DMA starts) and the
  Ethernet descriptors and buffers (an MPU region marked non-cacheable in SRAM3, as ST's
  examples do).
- **MPU regions** (proposal):
  1. a background region over the 4 GB, no access, so that speculative reads of the M7 never
     reach unmapped external memory areas (ST's recommendation in its templates and AN4838);
  2. **a 32-byte no-access region at address 0**: the ITCM sits at 0x0, so a write through a
     null pointer would silently overwrite the vector table or the interpreter loop. The
     hot code is placed above 0x100 and the first bytes are fenced;
  3. flash, RAMs: normal memory, write-back, as the default map;
  4. later: SRAM3 non-cacheable for Ethernet.
- **D2 SRAM clocks.** SRAM1-3 belong to the D2 domain; their clocks must be enabled by the
  startup code before the C heap uses them (`RCC_AHB2ENR` SRAM1/2/3 enable bits *(to check
  whether they are on at reset on the H755)*), all the more since the M4 (the usual "owner"
  of D2) never runs.

### 3.4 Hot code in ITCM

The mechanism of the RP2350 port carries over unchanged: the engine is a static library
built with `-ffunction-sections`; after the build, `objcopy --rename-section
.text.<function>=.itcm_text.<function>` moves the listed functions into a section that our
linker script places in ITCM with its load address in flash; the startup code copies it
(as it copies `.data`). Same list (`QJSX_SRAM_FUNCTIONS`), same `-fira-region=one` on
`interpreter.c`. ITCM holds 64 KB, the list takes ~40 KB: room for a few more functions
chosen by measurement.

## 4. Flash

### 4.1 Facts

- 2 banks of 1 MB, each **8 sectors of 128 KB**; the erase unit is the sector.
- Programming unit: the **flash word, 256 bits = 32 bytes** with ECC
  (`write-block-size = <32>` in Zephyr's DTS; `FLASH_TYPEPROGRAM_FLASHWORD` in the HAL). A
  flash word is written **once** between two erases (its ECC forbids writing it again).
- Erase time: around a second per sector, Zephyr uses 4 s as the maximum ("maximum erase
  time for a 128K sector"); the datasheet's table "Flash memory programming" gives the
  typical and maximum values *(to check)*.
- **Read while write between banks** (RM0399, chapter "Embedded flash memory"): code keeps
  running from bank 1 while bank 2 is erased or programmed, interrupts enabled. A read of
  the bank being written waits until the operation ends *(to check: stall, not error)*.
- An interrupted programming (power lost) can leave a flash word with a wrong ECC: reading
  it then gives a **double ECC error, i.e. a bus fault**, not just wrong data.

### 4.2 Proposed layout

| Range | Content |
|---|---|
| Bank 1, 0x0800_0000 - 0x080F_FFFF (1 MB) | the firmware (~750 KB expected, as the RP2350's 740 KB; up to 1 MB possible; the firmware checks at boot that it ends before bank 2, as `board_rp2350.cpp` does) |
| Bank 2, sectors 0-3, 0x0810_0000 (512 KB) | package **slot 0** |
| Bank 2, sectors 4-7, 0x0818_0000 (512 KB) | package **slot 1** |

Largest package: 512 KB minus the header. (The ESP32's slots are 1 MB, the RP2350's 4 MB;
with a 512 KB JS heap a larger package could not run anyway.) With option B of 1.3 the
slots are 384 KB.

### 4.3 The storage, adapted from `board_rp2350.cpp`

The same two-slot design (header written last, highest sequence wins, the running slot
never erased), with these differences:

- **No `flash_safe_execute`.** Writes go to bank 2 while the code runs from bank 1 and
  ITCM: no other core to hold, interrupts stay on, the UART keeps receiving.
- **Header**: not a sector of its own (that would waste 128 KB per slot), but the first
  bytes of the slot's first sector; the package starts at offset 256 (any multiple of 32).
  The header (magic, sequence, size, 64-byte build id: 76 bytes) spans three flash words;
  the words holding sequence / size / build id are written first, **the word holding the
  magic last**.
- **Programming by flash words**: the upload goes through a 32-byte buffer (the RP2350's
  256-byte page buffer becomes a 32-byte one); the chunks of 2048 bytes are multiples of 32.
- **Erase on demand.** Erasing a whole 512 KB slot in `beginUpload()` takes 4 sectors x
  ~1-4 s, longer than the host's request timeout (3 s, 3 attempts: `host/js/device.js`).
  Proposed: `beginUpload()` erases the first sector only; `writeUpload()` erases the next
  sector when the first chunk enters it. Each request then waits at most one sector erase.
  The agent already answers a repeated request again without running it twice.
- **ECC errors on the headers**: `findInstalled()` reads the two headers with precise bus
  faults ignored (`SCB->CCR.BFHFNMIGN` with `FAULTMASK` set for the few reads), then checks
  the double-ECC flag of bank 2; a header with an ECC error counts as "no header". Without
  this, a power cut during `finishUpload()` could make the board fault at every boot.
- **D-cache invalidation** of the written range after each erase / program (3.3).
- `openPackage()` returns `0x0810_0000 + slot offset + 256`: the package is read in place
  through the cache; `closePackage()` only forgets the slot (nothing to unmap).
- A package upload while a program runs from the other slot of the same bank: the program
  only reads its package while modules load (the bytecode is then in the JS heap), so the
  stalls of 4.1 should not show; to check.

### 4.4 The M4 and bank 2

With option A (BCM4 = 0), nothing runs from bank 2. With BCM4 = 1 and no parking image, the
M4 would boot from the first bytes of slot 0 (package data or erased flash) and fault,
again at each package install: this is why decision 2 comes before any flashing of
packages.

## 5. Links to the host

| Link | Pros | Cons | Proposal |
|---|---|---|---|
| **ST-LINK virtual COM port** (USART3, PD8 TX / PD9 RX) | the COM port belongs to the ST-LINK: it **survives every reset of the STM32** (RESET button, `qjsx device reset`, `fatal()`), unlike the USB CDC of the RP2350 (known-bugs.md "USB link and chip reset"); same cable as flashing and SWD; the same model as the ESP32's UART0 + CH343 | a UART: speed limited by the ST-LINK's bridge (921600 certainly, a few Mbaud probably, *to measure*: the STLINK-V3 derives its rates from its own clock, not every rate is exact) | **first link** |
| USB OTG FS, user connector (PA11 / PA12, VBUS sense PA9 *(pins to check on MB1363)*) | 12 Mbit/s, a second independent port | the COM port vanishes at each chip reset (same problem as the RP2350); a USB stack to add (TinyUSB, already on the disk, or ST's USB device library); a 48 MHz clock (HSI48 + CRS, or a PLL) | later |
| Ethernet (RMII, LAN8742A *(to check)*), TCP | the agent already supports TCP (with its token); fast; the PC reconnects after a reset; no cable to the PC other than the network | lwIP + the ETH driver + the PHY driver to download; DMA descriptors in D2 SRAM with an MPU region; ~60-100 KB of RAM, which this chip lacks; the agent has one link at a time (new-device.md 4.8) | later, after the JS heap is measured |

**The UART link in detail** (model: `UartLink` of `board_esp32.cpp`):

- USART3 at 921600 baud 8N1 (more after measuring), clocked from APB1 (100 MHz) or a kernel
  clock; RX by interrupt using the 16-byte RX FIFO with a threshold and the idle-line
  interrupt, into a FreeRTOS stream buffer read by the reader task (`agent->receive()`);
  no DMA at first (no cache question).
- TX: one mutex for every write. `UartLink::send` writes a whole frame in one call; the
  newlib `_write` (console, `printf`) takes the same mutex: a frame is never cut by text.
  TX through a ring buffer emptied by the TX-empty interrupt, or polled at first.
- `restart()` waits for the "transmission complete" flag before resetting, so the answer
  to `RESET` leaves the wire (the ESP32 port does not wait; here it costs nothing).
- The host target: `serial:COMx@921600`.

## 6. `platform.h` and `board.h` for this port

### 6.1 `platform.h`

`ports/freertos/platform_freertos.cpp` (`sleepMs`, `yieldIfDue`, `Mutex`, `Signal`) is used
as is. The port writes `platform_stm32.cpp`:

| Function | Implementation |
|---|---|
| `monotonicMs()` | **TIM2** (32-bit) at 1 MHz, extended to 64 bits by counting its overflows (every 71.6 min) in its update interrupt; read as high word, counter, high word again (retry if the overflow count changed). Not the FreeRTOS tick: the ARM_CM4F port has a 32-bit `TickType_t`, and `platform.h` forbids a 32-bit source. *(Alternative to discuss: TIM2 and TIM5 chained, the second counting the overflows of the first in hardware.)* |
| `fatal(message)` | print `qjsx: fatal: <message>` on USART3 by **polling** (the scheduler or the TX interrupt may be what failed), wait for "transmission complete", `NVIC_SystemReset()` |
| `startThread()` | `xTaskCreate` (single core: `options.core` ignored); stack in bytes / 4 (FreeRTOS counts words); defaults 4096 bytes, priority 5, as on the other ports; the same `ThreadStart` record as `platform_rp2350.cpp` |
| `sleepMs`, `yieldIfDue`, `Mutex`, `Signal` | shared (`ports/freertos/`); tick 1 kHz on SysTick, owned by FreeRTOS |
| `atomicIncrement` / `Decrement` | GCC builtins (LDREX / STREX on the M7): nothing to write |

FreeRTOS: `configNUMBER_OF_CORES 1`, `configPRIO_BITS 4` (the STM32H7 has 16 interrupt
priority levels), `configMAX_SYSCALL_INTERRUPT_PRIORITY` = 5 << 4, `configENABLE_FPU`
implicit in ARM_CM4F, `configCHECK_FOR_STACK_OVERFLOW 2`, the malloc-failed hook to
`fatal()` (as `platform_rp2350.cpp`). Task memory: **heap_4 or heap_5 with its region in
DTCM** (3.2), while `malloc` serves SRAM1-3. The HAL's `HAL_GetTick()` is redirected to the
FreeRTOS tick (or TIM2), since FreeRTOS owns SysTick.

### 6.2 `board.h`

| Function | Implementation |
|---|---|
| `identity()` | chip `"STM32H755ZI"` (from `DBGMCU->IDCODE` for the revision letter, *option*), `BOARD_NAME` = `"ST NUCLEO-H755ZI-Q"`, flash size from the flash size register (`FLASHSIZE_BASE`), `psramSize` 0 |
| `platformName()` | `"stm32"` or `"stm32h7"` **(decision 4)**, like `"esp32"` and `"rp2350"` |
| `dataDirectory()` | `nullptr`: no file system (no SD slot on this board) |
| `restart()` | wait for the UART's transmission complete, `NVIC_SystemReset()`; does not return |
| `jsAllocator()` | `memory::jsAllocator(&axiHeap, false)`, `axiHeap` initialised over the AXI SRAM at boot (`board::startMemory()`, as on the RP2350) |
| `jsStackSize()` | 40 KB of the 48 KB JS task stack (DTCM), as on the other ports |
| `packageStorage()` | the `FlashStorage` of 4.3, on bank 2 |
| `openPackage()` / `closePackage()` | the slot's address in the memory-mapped flash / forget the slot |
| `startLinks()` | USART3 link (section 5): pins, clock, interrupts, reader task, `agent->attach(&uartLink)` once (a serial link is always there), `qjsx agent: listening on USART3 (ST-LINK)` |
| `writeConsole()` | `fwrite` to stdout / stderr, then `fflush`: the same `_write` as `printf` |

Entry point (`main.cpp`): supply (SMPS direct) → clocks (400 MHz) → MPU → caches → D2 SRAM
clocks → `board::startMemory()` (the AXI heap) → check of BCM4 (warning) → hardware modules
registered (`device/gpio`...) → JS task (48 KB, DTCM) → `vTaskStartScheduler()`. The ITCM
copy and the `.data` / `.bss` setup are done by the startup code before `main`.
`agent::Identity` = `{ "qjsx stm32h7", CONFIG_VERSION, nullptr, JS_GetBytecodeVersion(),
QJSX_VERSION }`.

## 7. What newlib lacks: the `compat` approach

There is no SDK below us here (the pico-sdk provided the newlib system calls and
`_gettimeofday`; here nobody does). Three groups:

1. **The same as `ports/rp2350/compat/`**: `pthread.h` + mutex / condition on FreeRTOS
   semaphores (the engine's class-id lock, `Atomics.wait`), `dirent.h` and the directory /
   file calls failing with `ENOSYS`, `clock_gettime` (from `monotonicMs()`'s timer instead
   of `time_us_64()`). Proposal **(decision 5)**: move these files to
   `ports/freertos/compat/`, shared by both ports, the clock read through one small
   per-port function; or copy them into `ports/stm32h7/compat/` and leave the RP2350
   untouched.
2. **The newlib system calls** the pico-sdk used to provide (`syscalls.cpp` of the port):
   `_sbrk` (over SRAM1-3, fatal past the end), `_write` (USART3, under the TX mutex), `_read`
   (none: returns 0), `_close`, `_fstat` (character device, so stdout is line-buffered),
   `_isatty`, `_lseek`, `_getpid`, `_kill`, `_exit` (→ `fatal`), `_gettimeofday` (boot time,
   there is no wall clock), plus `_stat`, `_unlink`, `_link` as in the RP2350's compat.
3. **Thread safety of newlib**: `__malloc_lock` / `__malloc_unlock` on a FreeRTOS recursive
   mutex (the pico-sdk had `PICO_USE_MALLOC_MUTEX`); `configUSE_NEWLIB_REENTRANT 1` so that
   `errno` and stdio state are per task *(to decide with the newlib flavour: the full newlib,
   not newlib-nano, is proposed, since QuickJS and our code use `snprintf` with doubles)*.

The engine stays a library of its own (its `list.h` against FreeRTOS's), as on the RP2350.

## 8. Expected speed

Reference points (architecture.md 5.4, known-bugs.md "Speed"): a JS loop toggling a pin
takes about **490 clocks per turn on the ESP32-S3** at 240 MHz (247 kHz on the pin, ~2.0 us
per toggle) and **570 clocks on the RP2350** at 150 MHz (132 kHz, ~3.8 us), once the hot
engine code runs from SRAM.

What the STM32H755's M7 changes:

| Factor | Effect |
|---|---|
| Clock 400 MHz (vs 240 / 150) | x1.7 vs the ESP32-S3, x2.7 vs the RP2350, at equal clocks per turn |
| Cortex-M7: 6-stage, dual-issue, branch prediction, 64-bit AXI and LDRD | fewer clocks per turn than the in-order M33 and LX7; JSValues are 64-bit on a 32-bit core: two-word loads and stores pair well |
| Interpreter in ITCM, stacks in DTCM, both 0 wait state | the RP2350's lesson (the interpreter evicting itself from a 16 KB cache) does not apply; the C stack, heavily used by `JS_CallInternal`, is the fastest memory of the chip |
| JS heap in internal SRAM behind a 16 KB D-cache | much faster than the PSRAM of both other boards (the RP2350's PSRAM shares a 16 KB cache with the flash; the ESP32-S3's is octal PSRAM behind its cache) |
| Double-precision FPU | JS numbers are doubles: the ESP32-S3 (single-precision FPU) and the RP2350 (single-precision FPU, doubles in software or its coprocessor) compute them in software; `Math.sin` and arithmetic on non-integers should gain the most |
| Code left in flash (the rest of the engine, the runtime) | 16 KB I-cache in front of a flash at 200 MHz with wait states: cold paths are slower than on an ESP32 with its larger cache |

Estimate: **250 to 450 clocks per turn**, i.e. **0.6 to 1.1 us per toggle**, a pin at
**450 to 800 kHz** in the toggle loop: **about 2 to 3 times the ESP32-S3, 4 to 6 times the
RP2350**. This is reasoning, not a measure: step 3 runs the same `C:\temp\qjsx-speed`
program and the toggle loop.

## 9. Hardware modules

### 9.1 `hal_gpio.h` on the STM32

- **Pin numbers**: `port * 16 + pin` (PA0 = 0, PB0 = 16, PC13 = 45, ..., PH1 = 113): the
  "GPIO number" of the decision "Pins by their GPIO number only". `pinCount()` = 8 x 16 =
  128 (ports A to H on the LQFP144; `configure()` refuses the numbers the package lacks).
  How a program writes "PB0" **(decision 6)**: the number (16), or names in a table of the
  board (the backlog idea "a table of names per board, LED, BOOT"), or a helper in the
  standard library.
- `configure()`: clock of the port (`RCC_AHB4ENR`), `MODER` (input / output), `OTYPER`
  (**open drain is native**: OTYPER = 1), `PUPDR` (pull up / down / none), `OSPEEDR` (high
  speed for outputs); an output starts at 0 (`BSRR` reset bit written before `MODER`).
- `read()`: `IDR`. `write()`: **`BSRR`** (one atomic write: bit n sets, bit n + 16 resets).
- `toggle()`: the STM32 has no toggle register. One `BSRR` write computed from `ODR`
  (`BSRR = ((ODR & bit) << 16) | (~ODR & bit)`): the write is atomic, the read before it is
  not (an interrupt changing the same pin in between would be lost; no interrupt of ours
  writes pins). `ODR ^= bit` is a read-modify-write of the whole port and is avoided.
- `watch()` / `unwatch()`: **EXTI**. The chip has **16 EXTI lines, one per pin number**,
  each connected to ONE port at a time (`SYSCFG_EXTICR`): PA5 and PB5 cannot be watched
  together. `watch()` fails when the line is taken by another port (a real limit of the
  chip, to write in the module's doc). Interrupt handlers `EXTI0` ... `EXTI4`,
  `EXTI9_5`, `EXTI15_10` acknowledge (`EXTI_C1PR1`) and call
  `gpiowatch::postFromInterrupt(pin)`.
- `ports/freertos/gpio_watch.cpp` is shared, but its `MAX_PINS` is **64**: the STM32
  numbering needs **128** (or the watch could be keyed by EXTI line, 16). A small change of
  shared code, to agree **(decision 7)**.
- Board pins that a program must not take (the program is responsible, per the agreed
  rule): PD8 / PD9 (the host link), PA13 / PA14 (SWD), PH0 (HSE from the ST-LINK), the
  Ethernet RMII pins and the USB pins (once used). On-board: LD1 PB0, LD2 PE1, LD3 PB14, B1
  PC13.

### 9.2 `hal_rmt.h` on the STM32

No RMT peripheral; two ways, both free of CPU work while sending:

| Way | How | Covers |
|---|---|---|
| **Timer + DMA (recommended)** | a general-purpose timer in PWM mode, its prescaler giving `resolutionHz` (timer clock 200 MHz); the DMA rewrites ARR (period) and CCR (high time) at each update ("DMA burst"), one pair per pulse pair; the DMA's transfer-complete interrupt posts "done" to the hal's event task | `sendPulses` (pulses paired as high / low; a low pulse first is a CCR of 0) and `sendBytes` (each bit a pair: bit0 / bit1 timings; the reset is the last low time). One channel per free timer channel with a DMA request (TIM1/2/3/4/5/8...): enough channels |
| SPI MOSI | encode each WS2812 bit as 3 SPI bits at 2.4 MHz | `sendBytes` only, fixed timings: simpler, not general |

The DMA buffers live in the C heap (SRAM1-3, reachable by DMA1/2, not DTCM), are cleaned
from the D-cache before each start (or the region is non-cacheable). Memory per LED: 24
bits x 2 half-words = 96 bytes per WS2812 LED, copied at each `send` (the hal copies, as
agreed). The Nucleo has no WS2812: an external LED or strip is needed for the test (a
3.3 V data line usually works for a few LEDs).

## 10. Proposed layout

```
ports/stm32h7/                      (decision 8: or ports/stm32/)
  CMakeLists.txt                    board chosen by QJSX_BOARD, as ports/rp2350
  cmake/arm-none-eabi.cmake         toolchain file (paths given by tools/stm32.ps1)
  ld/stm32h7_cm7.ld                 our linker script: ITCM / DTCM / AXI / SRAM1-3 / flash banks
  main/
    main.cpp                        supply, clocks, MPU, caches, JS task, scheduler
    system_stm32h7.cpp              supply / PLL / flash latency / MPU / caches (from board_config.h)
    board_stm32h7.cpp, .h           board.h: storage, links, JS heap, identity
    platform_stm32h7.cpp            platform.h: TIM2 clock, fatal, tasks, FreeRTOS hooks
    uart_link.cpp, .h               USART link + console (one TX path)
    flash_stm32h7.cpp, .h           erase / program flash words, ECC-safe reads
    syscalls.cpp                    newlib system calls, malloc lock
    hal_gpio_stm32h7.cpp            hal_gpio.h (step 4)
    hal_rmt_stm32h7.cpp             hal_rmt.h (step 5)
    FreeRTOSConfig.h
    stm32h7xx_hal_conf.h            the HAL modules used
  compat/                           or ports/freertos/compat/ (decision 5)
  boards/nucleo-h755zi-q/
    board.cmake                     MCU define (STM32H755xx, CORE_CM7), memory sizes
    board_config.h                  BOARD_NAME, SUPPLY = direct SMPS, HSE 8 MHz bypass,
                                    400 MHz, host USART3 PD8/PD9, flash map, LEDs, BCM4 policy
tools/stm32.ps1                     configure | build | flash | erase | ob (option bytes)
                                    log C:\temp\stm32-build.log, exit code C:\temp\stm32-status.txt
```

The family folder holds what every STM32H7 shares (the future WeAct STM32H743 of the
backlog: same core and memories, LDO supply, no M4, 2 MB flash, QSPI flash, microSD); the
board folder holds the supply mode, the clock source, the pins and the flash map.

## 11. Plan (the order agreed for the RP2350)

0. **Preparation** (with the user): approve and fetch the downloads of 2.2; read the
   option bytes with STM32CubeProgrammer (BCM4, BCM7, boot addresses, RDP level 0); update
   the ST-LINK firmware; apply decision 2 (BCM4 = 0); note what the board ships with.
1. **Bring-up without qjsx**: startup, supply SMPS direct (the risky line: reviewed twice,
   tested once from a debug session), 400 MHz, MPU, caches, TIM2 clock, USART3 `printf`,
   LD1 blinking from a FreeRTOS task; read `SCB->CPUID` (core revision → FreeRTOS port),
   measure the clock with the DWT cycle counter.
2. **Minimal qjsx firmware**: engine library + common code + agent over the ST-LINK COM
   port, `FlashStorage` on bank 2, JS heap in AXI SRAM, ITCM code from the start. The
   checks of new-device.md section 5: `qjsx device info`, deploy and run of
   `tests/fixtures/nocompiler` compared with `qjsx-target`, `tests/debugger.test.js`
   (72 checks), `proxy.test.js` (30), `cdp.test.js` (53), an upload cut in the middle,
   `qjsx device stop` / `reset` (the COM port stays: check the tool keeps the session).
3. **Memory and speed**: heap used by an empty runtime and by the test programs (decide
   3.2's split), the speed program and the toggle loop against the ESP32-S3 and the
   RP2350, flash wait states, the ITCM list, `-O2` vs `-Os`.
4. **`device/gpio`** (`hal_gpio_stm32h7.cpp`, `gpio_watch` at 128 pins), LEDs and B1;
   toggle loop on a pin measured with the oscilloscope.
5. **`device/rmt`** (timer + DMA) and `stdlib/rgbled` on an external WS2812.
6. Later, to discuss: USB CDC, Ethernet + TCP link, bytecode in place
   (`JS_READ_OBJ_ROM_DATA`), the M4.

## 12. Risks

| Risk | Mitigation |
|---|---|
| Wrong supply configuration locks the board (1.1) | constant of the board folder; first flash from a debug session; recovery procedure written in the port's notes before step 1 |
| M4 booting into package data (1.3, 4.4) | decision 2 before step 2; BCM4 checked at boot |
| 512 KB JS heap too small for useful programs | measure in step 3; leads: bytecode in place, SRAM1-3 in the JS heap |
| Erase time vs the host's 3 s timeout | erase on demand, sector by sector (4.3) |
| ECC double error after a power cut during an upload → fault at every boot | ECC-safe header reads (4.3) |
| Stale D-cache lines after programming | invalidate the written range (3.3) |
| Null pointer writes into ITCM | MPU guard at address 0 (3.3) |
| DMA from DTCM (stacks) silently failing | rule: DMA buffers only in the C heap (SRAM1-3); no DMA at all in step 2 |
| `gpio_watch` limited to 64 pins | decision 7 |
| ST-LINK VCP speed / exact baud rates | measure in step 2; 921600 first |
| Facts of this proposal not checked against the ST PDFs (st.com unreachable) | read UM2408, RM0399, the datasheet and the errata before step 1; every *(to check)* above |
| newlib without an SDK: missing system calls show up as link errors, reentrancy issues as rare bugs | the list of section 7; malloc lock from the start |

## 13. Decisions for the user

1. **The board**: NUCLEO-H755ZI-Q (MB1363) assumed; "nucleo-H755ZI-A" does not exist.
2. **The M4**: A. BCM4 = 0 set once with STM32CubeProgrammer (recommended), B. a parking
   image in bank 2 (slots of 384 KB), C. something else.
3. **RAM split**: JS heap = AXI SRAM 512 KB, C heap = SRAM1-3 288 KB, stacks and data in
   DTCM (proposed); to revisit after the measures of step 3.
4. **`process.platform`**: `"stm32"` or `"stm32h7"`.
5. **compat**: shared `ports/freertos/compat/` (touches the RP2350 port) or a copy in the
   new port.
6. **Pin names**: numbers only (`port * 16 + pin`), or names (`"PB0"`) somewhere.
7. **`gpio_watch`**: raise `MAX_PINS` to 128 (shared code) or key it by EXTI line.
8. **Folder name**: `ports/stm32h7/` (proposed) or `ports/stm32/`; board folder
   `boards/nucleo-h755zi-q/`.
9. **Clock**: 400 MHz with the board as delivered (proposed), or rework the board for the
   LDO and 480 MHz.
10. **Flashing tool**: STM32CubeProgrammer 2.23 (ST account needed) or xPack OpenOCD.
11. **Downloads**: the list of 2.2, and where (`C:\tools\stm32\` proposed).
12. **First link**: ST-LINK COM port (proposed); then USB CDC or Ethernet, in which order.
13. **Driver style**: LL + registers on hot paths, HAL for PWR / RCC / FLASH (proposed), or
    registers only (CMSIS headers alone).

## 14. Sources

- ST, NUCLEO-H755ZI-Q product page and UM2408 (not reachable on 2026-11-12):
  https://www.st.com/en/evaluation-tools/nucleo-h755zi-q.html,
  https://www.st.com/resource/en/user_manual/um2408-stm32h7-nucleo144-boards-mb1363-stmicroelectronics.pdf
- STM32CubeH7 on GitHub (v1.13.0): https://github.com/STMicroelectronics/STM32CubeH7 ;
  `Projects/NUCLEO-H755ZI-Q/readme.txt`; template `Projects/NUCLEO-H745ZI-Q/Templates/BootCM7_CM4Gated`
  (readme, `CM7/Src/main.c`, `STM32CubeIDE/CM7/STM32H745ZITX_FLASH.ld`)
- HAL / LL drivers: https://github.com/STMicroelectronics/stm32h7xx-hal-driver (v1.11.6);
  CMSIS device: https://github.com/STMicroelectronics/cmsis-device-h7 (v1.10.7);
  CMSIS core: https://github.com/STMicroelectronics/cmsis_core
- Zephyr: board page https://docs.zephyrproject.org/latest/boards/st/nucleo_h755zi_q/doc/index.html ;
  `dts/arm/st/h7/stm32h7.dtsi`, `dts/arm/st/h7/stm32h745.dtsi`,
  `boards/st/nucleo_h745zi_q/nucleo_h745zi_q.dtsi` in https://github.com/zephyrproject-rtos/zephyr
- FreeRTOS-Kernel, `portable/GCC/ARM_CM7/ReadMe.txt`: https://github.com/FreeRTOS/FreeRTOS-Kernel
- ST community: SMPS default and LDO lock-up
  https://community.st.com/t5/stm32cubemx-mcus/default-settings-for-nucleo-h755-not-working-after-power-off/td-p/759842 ;
  recovery https://community.st.com/t5/stm32-mcus-boards-and-hardware/nucleo-h755zi-q-how-can-i-resolve-st-link-quot-no-device-found/m-p/632372 ;
  VOS0 and direct SMPS https://community.st.com/t5/stm32-mcus/faq-why-the-direct-smps-supply-option-is-not-shown-in/ta-p/853623
- STM32CubeProgrammer 2.23: https://community.st.com/stm32cubeprogrammer-mcus-30/stm32cubeprogrammer-v2-23-released-166573
- xPack OpenOCD: https://github.com/xpack-dev-tools/openocd-xpack/releases
- In this repository: `doc/new-device.md`, `doc/architecture.md` 5.4 / 5.5,
  `doc/known-bugs.md` "Speed", `ports/rp2350/`, `ports/esp32/main/board_esp32.cpp`,
  `common/board/board.h`, `common/runtime/platform.h`, `common/runtime/region_heap.h`,
  `common/hal/hal_gpio.h`, `common/hal/hal_rmt.h`, `ports/freertos/gpio_watch.*`,
  `host/js/device.js` (request timeout).
