# Tools

Helpers for making clips, uploading them, and testing the HoloFan on a bench.
None of them is needed to build or flash the firmware. To make and load
clips day to day, use DroidNet Signal Booster (README.md, "Clips"); these
tools are for development and bench tests.

## How the bench tools reach the mesh

The mesh tools do not talk to the fan directly. They send WCB commands through
a DroidNet Pi: a Raspberry Pi with a WCB on its USB port (/dev/ttyUSB0) that
serves POST /api/wcb/command (port 80) and a read-only serial WebSocket
(port 8080). You name that Pi yourself; nothing defaults to a particular
machine:

    hfbench.py      --host <name.local> or $HFBENCH_HOST (or --host-ip A.B.C.D)
    ota_fan.sh      WCB_HOST=<name>          (without .local)
    meshlog.py, send.py, probe.py, soak.py, wcbwatch.py
                    HOST as the first argument (without .local; they add it)

hfbench.py refuses WCB setters (`?X,<value>`): only the read-only `?WDP,DUMP`
and `?BACKUP` pass. Changing a WCB's settings is left to you, by hand.

## Clips (tools/holofan_clip)

    clipgen.py          make test clips for any geometry (64x520x6 for the
                        Atom PRO), a stride probe, and inspect existing clips
                          clipgen.py clip --geom 64x520x6 --seconds 3 --video rings --audio tone:440 out.bin
                          clipgen.py inspect --geom 64x520x6 clip.bin
                          clipgen.py crc file.bin
    make_test_clip.py   the smallest playable clip from a header file
                          make_test_clip.py <ee37-header.bin> <out.bin> [frames]
    pipeline_sim.cpp    offline simulation of the upload path (lib/FanLogic)
                          c++ -std=c++17 -O2 -I lib/FanLogic -I lib/HolofanProtocol \
                            tools/holofan_clip/pipeline_sim.cpp lib/FanLogic/*.cpp \
                            lib/HolofanProtocol/*.cpp -o sim
                          ./sim <clip.bin> [mcu store ms per window]

## Upload and update (tools/bench)

    clip_put.py         upload a clip with the resumable session API (fan in
                        maintenance mode)
                          clip_put.py FILE http://holofan.local NAME
                          clip_put.py --abort http://holofan.local
    ota_fan.sh          put the fan in maintenance mode through a WCB, then
                        POST a new app image, retrying
                          WCB_HOST=<pi> [FAN=<fan-ip>] tools/bench/ota_fan.sh [image]

## Bench measurements (tools/bench)

    meshlog.py          log every line a WCB prints, with timestamps
                          meshlog.py HOST LOGFILE
    hfbench.py          send / run-until / query / mark / summary over that log
                          hfbench.py send '#HLFS' --host <pi.local>
                          hfbench.py summary LOGFILE
    send.py             fire-and-forget WCB commands
                          send.py HOST cmd [cmd ...]
    wcbwatch.py         send commands and watch the WCB's lines
                          wcbwatch.py HOST SECONDS_PER_CMD cmd [cmd ...]
    probe.py            count answers to a broadcast query and a unicast probe
                          probe.py HOST ROUNDS GAP_S LOGFILE LABEL
    soak.py             long liveness test (RTERM answers, longest silence)
                          soak.py HOST SECONDS GAP LOGFILE LABEL
    silences.py         find silences in a phone recording of the fan
                          silences.py RECORDING [--start HH:MM:SS]
    av_offset.py        audio/video offset from a phone video of the sync clip
                          av_offset.py VIDEO

meshlog.py and wcbwatch.py need the `websockets` package. silences.py needs
ffmpeg (or afconvert on macOS); av_offset.py needs ffmpeg, numpy and scipy.

## UART bridge (tools/holofan_uart_bridge)

holofan_uart_bridge.ino turns a SparkFun RedBoard Artemis into a 3.3 V
USB-serial passthrough for the fan's header. Any 3.3 V USB-serial adapter
does the same job (README.md, "Connecting the fan").

## Tests

    python3 -B -m unittest discover -s tools/bench -p 'test_*.py'
    python3 -B -m unittest discover -s tools/holofan_clip -p 'test_*.py'
    python3 -B -m unittest discover -s tools -p 'test_check_private.py'

The test_av_offset and test_silences suites need the same programs. The
SW130 tests in test_clipgen.py run only if a vendor clip is at
~/Downloads/SW130.BIN and are skipped otherwise.
