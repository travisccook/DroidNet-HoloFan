# CLAUDE.md

Guidance for Claude Code (claude.ai/code) and other coding assistants working
in this repository.

## Project overview

DroidNet-HoloFan is replacement firmware for the ESP32-C2 (ESP8684) in a
Pixel Arc Atom PRO holofan (vendor model F-S12). It makes the fan node 16
("HoloFan") on a WCB ESP-NOW mesh and drives the fan's LED/SD controller (an
HC32F460, "the MCU") over UART1, with clip upload over SPI and OTA updates in
a Wi-Fi maintenance mode. docs/HOW_IT_WORKS.md has the hardware and protocol
facts. The lift that carries the fan is the sibling repository
DroidNet-HoloLifter.

## Build and test

```bash
firmware/holofan/build.sh                 # ESP-IDF build; IDF_PATH defaults to ~/esp/esp-idf
pio test -e native                        # C++ unit tests of lib/ (Unity, host)
pio test -e native -f test_mcu_proto      # one suite
python3 -B -m unittest discover -s firmware/holofan/tests          # fanflash.py (fake esptool)
python3 -B -m unittest discover -s tools/bench -p 'test_*.py'
python3 -B -m unittest discover -s tools/holofan_clip -p 'test_*.py'
python3 -B -m unittest discover -s tools -p 'test_check_private.py'
```

ESP-IDF must be at commit 11edebdbbe (release/v5.4); build.sh warns
otherwise. Set IDF_TOOLS_PATH if the ESP-IDF tools are not in ~/.espressif.
fanflash.py is tested only against tests/fake_esptool.py; never run its
write commands against hardware from an automated session.

## Layout

    firmware/holofan/   ESP-IDF project: main/, components/, partitions.csv,
                        build.sh, fanflash.py and its shell wrappers
    lib/                pure C++ shared with the native tests:
                        HolofanProtocol FanLogic (fan only)
                        WcbCore NodeConfig MaintCore LiftCommands (shared)
    test/               native Unity suites
    tools/              clip generator, bench tools, UART bridge sketch,
                        check_private.py (pre-commit leak scan)
    docs/               HOW_IT_WORKS, SERIAL_COMMANDS, MAINTENANCE_API,
                        WCB_MESH_NOTES, HOLOFAN_IR_CODES, images/

firmware/holofan/components/wcbcore compiles ../../../../lib directly, so
the layout must stay as it is.

lib/WcbCore, lib/NodeConfig, lib/MaintCore and lib/LiftCommands are shared
with DroidNet-HoloLifter: change both copies together and keep them
identical (`diff -r ../DroidNet-HoloLifter/lib/WcbCore lib/WcbCore`). The
same holds for their test suites.

## Safety rules that belong to the code

- Never send the MCU `v` (SD card format) except through the maintenance
  API's explicit POST /api/card/format?confirm=FORMAT-SD-CARD. buildLine()
  refuses any other line starting with `v`; the mesh's raw #HLFX allowlist
  excludes v, u, w and y.
- Never configure GPIO8 or GPIO9 (boot straps).
- Never erase NVS, and never write outside the "hlmesh" namespace: NVS holds
  the chip's RF calibration and the stock firmware's data. The first flash
  never writes the nvs partition.
- The MCU drops or misreads a line sent while it is busy: keep the settle
  wait after every action and the queue limits.
- Public defaults stay as they are: mesh TX power 8 dBm (maintenance 20),
  mesh off until a password is set, no Wi-Fi profiles.
- A new image must reach the mesh within 30 s of booting or it rolls back;
  do not mark an image valid any earlier.

## Command protocol

Commands are `#HL...` lines sent through a WCB as `;wHoloFan,#HLFS\r`. Only
queries reply, as a mesh broadcast starting with `#HL:`; actions are silent.
docs/SERIAL_COMMANDS.md is the reference; keep it in step with
lib/HolofanProtocol and lib/FanLogic, and README.md's list of everyday
and setup commands with it.

## Documentation rules

- No markdown tables. Use text tables inside code blocks so they read well in
  a plain text editor.
- Every fact is either tested on a fan or marked untested.
- Protocol facts only: no stock-firmware addresses, decompilation or vendor
  firmware images.
- Never assume an API or command syntax (ESP-IDF, esptool, espefuse); check
  the documentation for the pinned version.

## Private data

Never write home, droid or credential details into this repository: IP
addresses on a home network, host names, Wi-Fi or mesh passwords, MAC
addresses of real devices, personal paths, backups of a fan's flash.
tools/check_private.py checks for them and runs as the pre-commit hook:

    python3 tools/check_private.py            whole tree
    python3 tools/check_private.py --staged   what is staged
