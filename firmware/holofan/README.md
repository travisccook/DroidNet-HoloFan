# HoloFan firmware (ESP32-C2)

Replacement firmware for the ESP32-C2 (ESP8684) in a Pixel Arc Atom PRO
holofan. It joins a WCB ESP-NOW mesh as node 16 "HoloFan" and drives the
fan's LED/SD controller ("the MCU") over UART1. Commands:
docs/SERIAL_COMMANDS.md. How the fan works: docs/HOW_IT_WORKS.md. The
top-level README.md is the place to start; this page is for developers.

The fan runs from ota_0 / ota_1 under ESP-IDF's rollback bootloader. After
the one wired first flash, updates arrive over Wi-Fi in maintenance mode
(`;wHoloFan,#HLZX1` on a WCB, or three power cycles of 2-10 s each), and a
new image must reach the mesh within 30 s or it rolls back by itself (since
0.9.19: receive a frame from another node, so keep a WCB on and in range).

## Layout

    main/                 app_main, mesh task (FanMesh), MCU task (FanMcu),
                          console, sag meter
    components/wcbcore    compiles ../../lib/{WcbCore,NodeConfig,MaintCore,
                          HolofanProtocol,FanLogic,LiftCommands}
    components/wcb_idf    ESP-NOW radio adapter (MeshRadioIdf), NVS settings
                          (ConfigStoreIdf)
    components/mcu_link   UART1 + GPIO link (McuUart) and SPI upload (McuSpi)
    components/maint_idf  maintenance mode: Wi-Fi, mDNS, HTTP API, OTA, uploads
    partitions.csv        nvs, ota_0, ota_1, otadata
    fanflash.py           check / backup / first / restore / app over a serial
                          adapter; backup_stock.sh, flash_first.sh,
                          restore_stock.sh and flash.sh call it
    tests/                fanflash.py's tests, against a fake esptool

The logic (MCU line protocol, FanController, clip upload, maintenance state
machine) is pure C++ in lib/ and runs on the computer:

    pio test -e native                                   from the repo root
    python3 -B -m unittest discover -s firmware/holofan/tests

## Build

    ./build.sh

Needs ESP-IDF at commit 11edebdbbe (release/v5.4, the build the stock fan
firmware used; README.md, "Building"). IDF_PATH defaults to ~/esp/esp-idf;
set IDF_TOOLS_PATH if your ESP-IDF tools are not in ~/.espressif. build.sh
warns when ESP-IDF is at another commit, checks that the image fits an OTA
slot (960 KiB) and that the shared mesh core makes no libatomic calls (the
ESP32-C2 has no atomic instructions); it fails if that check cannot run.

sdkconfig is generated from sdkconfig.defaults only when it does not exist.
build.sh warns when sdkconfig.defaults is newer than sdkconfig: delete
sdkconfig and build again to apply the changed defaults.

## First wired flash

README.md, "Connecting the fan" and "First flash", has the full procedure.
In short, with the fan in ROM download mode (GPIO9 held to GND at power-up)
and ESP-IDF's environment loaded (`. $IDF_PATH/export.sh`):

    python fanflash.py check  <port>                 read-only gate
    ./backup_stock.sh         <port> <backup-dir>    read all 2 MB twice
    ./flash_first.sh          <port> <backup-dir>    bootloader, table, otadata, app
    ./restore_stock.sh        <port> <backup-dir>    back to stock

flash_first.sh and restore_stock.sh refuse unless <backup-dir> holds a
verified backup of the chip on <port>, and so does flash.sh <port>
<backup-dir>. No command erases the flash. backup_stock.sh only reads.
flash_first.sh and flash.sh never erase or write the nvs partition
(0x9000-0xEFFF); fanflash.py refuses an image too large for its region.
restore_stock.sh writes the chip's own 2 MB backup back, NVS included: that
is what restoring stock means. flash.sh (app only, into ota_0) is for
recovery: after an OTA the fan may be booting ota_1, so prefer an OTA.
A backup directory that holds a chip's stock-<mac>.bin without its .json,
or the .json without the .bin, is refused by backup_stock.sh and by the
three flashing commands; the message says what to move. (These refusals are tested against the fake esptool
only.)

## Update over Wi-Fi

    curl -X POST -H 'Content-Type: application/octet-stream' \
         --data-binary @build/holofan.bin http://holofan.local/api/ota

## Clips over Wi-Fi (maintenance mode)

Users load clips with DroidNet Signal Booster (README.md, "Clips"); these
calls are for development:

    curl http://holofan.local/api/clips
    python3 tools/bench/clip_put.py 03-Rings.bin http://holofan.local 03-Rings.bin
    curl -X DELETE http://holofan.local/api/clips/03-Rings.bin

Clip names are NN-<chars>.bin; the fan pads a clip to a multiple of 28,800
bytes itself. A name that already exists is refused (409): delete it first.
Clips for this fan are 64x520x6 (tools/holofan_clip/clipgen.py).

## Console (UART0 = the programming header, 115200)

`help` lists the commands (docs/SERIAL_COMMANDS.md, "Fan console"). The mesh
is off until `mesh pw` sets a password; `mesh oct` and `mesh ch` take the
MAC octets and channel from your WCB mesh's configuration. Settings live in
NVS namespace "hlmesh"; the firmware writes only that namespace and never
erases NVS.

## 0.9.19: not yet tried on a fan

0.9.19 (code-review fixes) has passed the native and Python tests only. It
has not been built with ESP-IDF or run on a fan. The EXPECT column is what
the code is meant to do, not a result. Start with ./build.sh.

    STEP  WHAT                                      EXPECT
    ----  ----------------------------------------  ----------------------------------------
    F1    OTA with a WCB on and in range            "[ota] new image marked valid" once a
                                                    mesh frame has arrived (a WCB's
                                                    heartbeat: about every 10 s). From the
                                                    dome: check that one arrives in 30 s,
                                                    or a good image rolls back
    F2    OTA with every WCB off                    "[ota] ... rolling back" after 30 s;
                                                    #HLI shows the old version
    F3    maintenance and back, several times       on the mesh again each time; "[mesh]
                                                    radio is down; restarting it" means
                                                    the 5 s retry had to step in
    F4    upload a clip (PUT, and a session)        ends ok, not "noverdict"; GET
                                                    /api/trace shows the RT
    F5    DELETE /api/upload during "sending"       failed/abort; the name is not in GET
                                                    /api/clips afterwards
    F6    a card whose listing is over 1 KB:        the command after each is still
          #HLFC, #HLFT<NN>, console "mcu s"         answered right (no dropped or misread
                                                    line); #HLFS err=none
    F7    #HLFV3x, #HLFV02, #HLFPx, #HLFW10         ignored; #HLFV2, #HLFG1, #HLFT3 work
    F8    POST /api/ota with a file over 960 KiB    400 "length", connection closed at once
    F9    POST /api/upload ...&pre=575              400 "query"; pre=574 opens a session
    F10   during #HLFE: console "mesh name X"       "[mesh] refused: ..."; the same line
                                                    is saved once #HLFE has ended
    F11   console "mcu" with a line over 95         "[mcu] refused ..."; nothing sent to
          characters, or with four arguments        the MCU
    F12   three power cycles of 2-10 s              maintenance mode, as before. Two
                                                    cycles, console "mesh restart", 10 s,
                                                    one more cycle: no maintenance mode

## Safety rules in the code

- `v` (SD card format) goes to the MCU only through the maintenance API's
  explicit POST /api/card/format?confirm=FORMAT-SD-CARD. buildLine() refuses
  any other command starting with `v`, and the mesh's raw #HLFX allowlist
  excludes v, u, w and y.
- Arguments with NUL, CR, LF, space, control or non-ASCII bytes are refused,
  so a name can never split the `cmd args\n` framing.
- GPIO8 and GPIO9 (boot straps) are never configured.
- The mesh TX power defaults to 8 dBm (#HLFO; docs/HOW_IT_WORKS.md,
  section 7); maintenance mode always uses 20 dBm.
