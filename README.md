# DroidNet-HoloFan

Replacement firmware for the ESP32-C2 inside a Pixel Arc Atom PRO holofan
(made by Missyou, sold for the "5D HoLo" app, model F-S12). It turns the fan
into a node (id 16, alias "HoloFan") on Greg Hulette's WCB ESP-NOW mesh, so a
droid controller can drive it like any other part of the droid: play, pause,
next, previous, clip by number, volume, brightness, loop, Bluetooth off. In
maintenance mode the fan joins your Wi-Fi for clip uploads and over-the-air
updates, and a bad update rolls itself back.

The fan's LED and SD-card controller keeps its own firmware; only the radio
chip's software is replaced, and your backup of the stock firmware puts it
back.

Its companion is
[DroidNet-HoloLifter](https://github.com/travisccook/DroidNet-HoloLifter), a
motorized lift that raises the fan out of an astromech dome and switches its
power.

---

## Standing on shoulders

None of this would exist without people who took these fans apart first and
wrote down what they found.

- **David Smith (daveismith): beancell-holo**
  <https://github.com/daveismith/beancell-holo>. His documentation of the 5D
  fans' access point and their HTTP and WebSocket control protocol
  (docs/display-protocol.md in his repo), and his idea of switching a fan's
  power with a relay from another ESP32, were the starting point of this
  project.
- **Greg Hulette: the Wireless Communication Board (WCB)**
  <https://github.com/greghulette/Wireless_Communication_Board-WCB> (GPL-3.0)
  and **WCB_Client** <https://github.com/greghulette/WCBClient> (MIT). The mesh
  the fan joins is Greg's: the boards, the ESP-NOW protocol, acknowledgements,
  retries and device discovery. lib/WcbCore is a port of parts of WCB_Client
  (THIRD_PARTY_NOTICES.md). Thank you, Greg.
- **efriman3: HoloSet** <https://github.com/efriman3/holoset> (MIT), a
  setlist controller for these fans over their own Wi-Fi API, built on a
  Missyou F1 with the same stock firmware version as ours.
- **Oblokos: Holo Fan Toolkit** <https://github.com/Oblokos/holo-fan-toolkit>
  and **ddefino2: Halosmith** <https://github.com/ddefino2/halosmith>,
  clip encoders for other fans of this family. Their work showed how the .bin
  clips are built: polar lines, bit planes, unsigned 8-bit audio at 32 kHz.
- **Zachary McMurrin (zackmcmurrin-dev): holo-fan-linux**
  <https://github.com/zackmcmurrin-dev/holo-fan-linux> (MIT). Our clip
  generator uses the 812-byte clip header he published
  (THIRD_PARTY_NOTICES.md).
- **Espressif Systems**: ESP-IDF, esptool and espefuse, which build, flash and
  check this firmware.

---

## Use at your own risk

**Read this before you open your fan.**

- This is a hobby project, shared as is. Under the GPL-3.0 (sections 15 and 16) it comes with no warranty of any kind. If it bricks your fan, that is on
  the person who flashed it, not on the authors.
- Opening the fan voids its warranty.
- A bad flash can leave the fan dead. Your own backup of its stock firmware,
  made with the steps below, is the only way back. Nobody else has it.
- The fan's chips run on 3.3 V. A 5 V adapter, or 5 V on any hole, destroys
  them.
- Never work on the fan while it spins or while its USB-C cable is plugged
  in. The rotor is fast and sharp-edged.
- At volume 3 the fan's own radio can brown out its ESP32-C2 (the firmware
  caps the volume at 2 unless you raise it; docs/HOW_IT_WORKS.md, section 7).
- You need your own WCB mesh. Without one the fan does nothing useful.

Not affiliated with Pixel Arc, Missyou, the makers of the 5D HoLo app,
Espressif, the WCB project or Lucasfilm. Product names belong to their
owners.

---

## What has been tested

One fan: an Atom PRO (model F-S12, stock firmware 1.1.0), in one droid,
since October 2026, updated over Wi-Fi from firmware 0.6 to 0.9.12. Its first
flash went through a SparkFun RedBoard Artemis used as a USB-serial
passthrough (tools/holofan_uart_bridge).

Not tested: any other model, any other USB-serial adapter, a fan without a
WCB mesh. If you try one of these, you are the first.

---

## You will need

- WCB boards on a firmware with ESP-NOW v2 (252-byte packets), and your
  mesh's password, MAC octets 2 and 3, and Wi-Fi channel.
- A USB-serial adapter with 3.3 V logic (check: not 5 V).
- Five male header pins or jumper-wire pins, and some tape or a clip to hold
  them.
- A 330 ohm resistor.
- ESP-IDF at commit 11edebdbbe ("Building", below) and Python 3.
- A small Phillips screwdriver to open the fan.

---

## Is my fan compatible?

Three checks, from easiest to definitive. Only the last one is a hard gate.

### 1. Without opening it

Power the fan normally. A stock fan broadcasts a Wi-Fi network named `5D_`
plus four hex digits (WPA3, password `12345678`). Join it from a computer,
then:

    curl http://192.168.4.1/ctrl/session
    curl http://192.168.4.1/info
    curl "http://192.168.4.1/ctrl/session?action=quit"

The first should answer `{"code":0,"desc":"Session success"}`. A compatible
fan answers the second with `{"model":"F-S12","sw":"1.1.0"}` (the sw version
may differ). The third closes the session.

- `F-S12`: Atom PRO, the tested model.
- `F-S12+`: Atom PRO+. Very likely the same board, but untested, and its clips
  use a different geometry.
- Anything else: untested. It may have a different board.

### 2. Inside

Unplug the fan and open the housing (the rotor can stay on the motor). On the
controller board at the tail of the rotor arm, look for:

- the silkscreen `F_S12+_V1.1`;
- an Espressif `ESP8684` chip next to a `40.000 MHz` crystal;
- a second chip marked `F460JEUA`;
- a microSD slot marked `TF1`;
- a row of five unlabelled holes with a small bracket at one end (pin 1).

![The controller board: ESP8684, crystal and the five-hole header (bottom left)](docs/images/header-and-esp8684.jpg)

Count the LEDs on the arm: 64 is an Atom PRO (tested); 85 to 90 is a PRO+
(untested); 48 is an Atom SE (untested).

### 3. The hard gate

Wired up as in the next section, in download mode:

    python firmware/holofan/fanflash.py check <port>

It must report an ESP32-C2 with 2 MB of flash, and an eFuse gate of no flash
encryption, no secure boot and download mode enabled, ending in `OK`. If it
prints `REFUSED`, stop: this fan cannot take this firmware, and nothing has
been written to it.

---

## Connecting the fan

You only do this once: after the first flash every update goes over Wi-Fi.

### The header

The five holes are at the tail of the rotor arm, next to the ESP8684.
Unplug the fan's USB-C, open the housing and turn the rotor by hand until the
tail faces you.

![The tail of the rotor arm: the ESP8684 and, bottom right, the five holes](docs/images/rotor-tail.jpg)

Counting from the bracket end (pin 1):

    hole 1  GND
    hole 2  GPIO9   to GND through 330 ohm at power-up = download mode
    hole 3  fan RX  <- adapter TX
    hole 4  fan TX  -> adapter RX
    hole 5  3V3     power for the board (and its LEDs)

There is no reset pin: to reset the chip, remove and reapply 3.3 V. When you
do, disconnect the adapter's TX from hole 3 as well (or unplug the adapter):
a TX line left high can keep the chip partly powered through its RX pin, so
it does not reset cleanly (advice, untested).

### Making contact

No soldering is needed. Push header pins or jumper-wire pins into the holes
and hold them in place with tape or a small clip, so that they stay put and
do not touch each other.

### Wiring

Set the adapter to 3.3 V logic. Then:

    adapter GND  -> hole 1
    adapter TX   -> hole 3
    adapter RX   -> hole 4
    3.3 V        -> hole 5
    hole 2       -> 330 ohm -> GND, only while you power up for download mode

The fan's LEDs run from this 3.3 V rail, so it must handle their inrush when
power is applied. Our 3.3 V source dropped out for about 5 s at every
power-up and then recovered. If your adapter's 3.3 V pin is a small
regulator inside its USB chip, feed hole 5 from a separate 3.3 V supply and
connect that supply's ground to the adapter's ground (advice, untested). The
motor does not run from this rail.

### Step 1: listen

Connect only GND (hole 1) and the adapter's RX (hole 4). Open a serial
terminal at 115200 baud and apply 3.3 V to hole 5. You should see the ROM's
boot log, starting with `ESP-ROM:esp8684`. Nothing at all: check GND, or swap
holes 3 and 4.

### Step 2: download mode

Connect the adapter's TX to hole 3 as well. Remove power, connect the 330 ohm
resistor from hole 2 to GND, and apply 3.3 V again. The boot log now says
`waiting for download`. Close the terminal so the port is free.

### Step 3: check

Load ESP-IDF's environment (installed as in "Building", below), then run the
gate:

    . ~/esp/esp-idf/export.sh
    python firmware/holofan/fanflash.py check <port>

### Step 4: back up the stock firmware

    firmware/holofan/backup_stock.sh <port> ~/fan-backup

This reads the whole 2 MB flash twice and keeps it only if both reads match,
as `stock-<chip MAC>.bin` with a `.json` description next to it. It writes
nothing to the fan and never overwrites an existing backup. Copy the
directory somewhere safe: it is the only way back to stock.

### Step 5: build and flash

    firmware/holofan/build.sh
    firmware/holofan/flash_first.sh <port> ~/fan-backup

flash_first.sh refuses unless ~/fan-backup holds a verified backup of this
very chip. It writes ESP-IDF's rollback bootloader, the partition table,
the OTA data and the app, and never touches the chip's NVS (its RF
calibration and the stock firmware's data stay).

### Step 6: first boot and mesh setup

Remove the resistor from hole 2, then remove and reapply 3.3 V. Open the
serial terminal again (115200, same wires): this is the fan's console. Type
`help`, then enter your mesh's values:

    mesh pw <your mesh password>
    mesh oct <hh> <hh>
    mesh ch <channel>
    mesh restart

For updates over Wi-Fi later, store a Wi-Fi profile too:

    mesh ssid 1 <network name>
    mesh pass 1 <passphrase>

The mesh is off until a password is set, so a fresh fan stays silent.

### Step 7: close it up

Disconnect the wires, close the housing and power the fan normally. Within a
few seconds of booting it appears on your WCBs as HoloFan.

---

## Day to day

Send commands through any WCB as `;wHoloFan,<command>` (or `;w16,...`):

    #HLFG<n>          play catalog item n          #HLFP        pause
    #HLFT<NN>         play the clip named NN-...   #HLFN/#HLFR  next / previous
    #HLFV<1-3>        volume (capped at 2)         #HLFB<1-3>   brightness
    #HLFM / #HLFU     mute / unmute                #HLFL<O|A>   loop one / all
    #HLFA<0|1>        Bluetooth off / on           #HLFC        re-read the clips
    #HLFS / #HLFD     status / clip list (reply)   #HLZ...      node settings

Actions never reply; queries reply as a mesh broadcast. Send at most about
three actions a second: the fan's controller needs time between them. The
full reference, with the bench commands, is
[docs/SERIAL_COMMANDS.md](docs/SERIAL_COMMANDS.md).

The fan's power is not a fan command: switch it with a relay (the HoloLifter
does this with `#HLF1` / `#HLF0`) or your own switch.

**Clips** for the Atom PRO are 64 LEDs x 520 lines x 6 bits.
tools/holofan_clip/clipgen.py makes them; other geometries play as snow.

**Maintenance mode** (uploads and updates): send `#HLZX1` (Wi-Fi profile 1),
or power-cycle the fan three times in a row, each time on for 2 to 10 s. The
fan leaves the mesh, joins your Wi-Fi and answers at http://holofan.local:

    python3 tools/bench/clip_put.py 03-Rings.bin http://holofan.local 03-Rings.bin
    curl -X POST -H 'Content-Type: application/octet-stream' \
         --data-binary @firmware/holofan/build/holofan.bin http://holofan.local/api/ota

A new image must reach the mesh within 30 s of booting, or the fan goes back
to the previous one by itself. Maintenance mode has no password: anyone on
that network can upload while it is on. Details:
[docs/MAINTENANCE_API.md](docs/MAINTENANCE_API.md).

---

## Going back to stock

Wire the fan up and put it in download mode (steps 1 and 2), then:

    firmware/holofan/restore_stock.sh <port> ~/fan-backup

It writes your backup of this chip back over the whole flash.

---

## Building

Install ESP-IDF at the commit the stock firmware was built with (ESP-IDF's
own instructions, pinned to one commit):

    mkdir -p ~/esp && cd ~/esp
    git clone --recursive https://github.com/espressif/esp-idf.git
    cd esp-idf
    git checkout 11edebdbbe
    git submodule update --init --recursive
    ./install.sh esp32c2

Then, from this repository:

    firmware/holofan/build.sh

build.sh uses ESP-IDF from ~/esp/esp-idf (or $IDF_PATH), warns if it is at
another commit, and checks that the image fits an OTA slot.

Tests, none of which need hardware:

    pio test -e native
    python3 -B -m unittest discover -s firmware/holofan/tests
    python3 -B -m unittest discover -s tools/bench -p 'test_*.py'
    python3 -B -m unittest discover -s tools/holofan_clip -p 'test_*.py'

---

## How it works

[docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) describes the two chips, the
header, the UART protocol between them, the SPI clip upload, the clip format,
the flash layout and the volume findings. tools/README.md lists the clip and
bench tools.

The mesh libraries lib/WcbCore, lib/NodeConfig, lib/MaintCore and
lib/LiftCommands are shared with DroidNet-HoloLifter and kept identical in
both repositories.

---

## Licence

GPL-3.0, see [LICENSE](LICENSE). Third-party notices (WCB_Client,
holo-fan-linux, ESP-IDF) are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
