# HoloFan command reference (#HL over the WCB mesh)

The HoloFan and the HoloLifter are both nodes on the droid's WCB ESP-NOW mesh,
and a droid controller reaches them through any WCB. This page covers the
HoloFan (node 16), the #HLZ node commands both nodes share, and the fan's
UART console; the lift's own commands are documented in DroidNet-HoloLifter.

## Overview

    Prefix          #HL (HashTag HoloLifter)
    Transport       WCB ESP-NOW mesh (ETM, CRC on), via any WCB serial port
    Terminator      \r on the controller's serial line (the WCB strips it)
    Encoding        ASCII, case-sensitive (#HLU, not #hlu)
    Nodes           HoloLifter  default id 15, alias "HoloLifter"
                    HoloFan     default id 16, alias "HoloFan"

## Addressing a node from a droid controller

Write a WCB command on any WCB serial port:

    ;wHoloLifter,#HLG5000\r     by alias (the node's WDP device name)
    ;w15,#HLG5000\r             by node id

The WCB unicasts the text after the comma to that node with ETM (ACK and
retry). Both forms work once the WCB has heard the node's WDP advert (a few
seconds after the node boots).

## Replies: only queries answer

Action commands (move, stop, set speed, relay, play, ...) never reply, not
even on error. Query replies are BROADCAST on the mesh as plain text, so any
reader can see them: every WCB writes the line to each of its serial ports
that has broadcast output on (?BCAST,OUT,S<n>,ON on the WCB; S0 is its USB,
which is how DroidNet reads them). Ports owned by an HCR, Maestro, MP3 or
DFPlayer are skipped by the WCB.

    controller -> WCB1:            ;wHoloLifter,#HLQ\r
    HoloLifter -> all WCBs:        #HL:STATUS state=IDLE pos=0 ...
    each WCB -> its BCAST-out ports: #HL:STATUS state=IDLE pos=0 ...\r

Replies are on by default; #HLZP0 silences them, #HLZP1 turns them back on.
A reply is sent once (best effort, never retried); if it is lost, ask again.
Each WCB still ACKs the broadcast frame, which the node simply ignores.
Replies never start with { and never contain ^ (both are special to WCBs).

## HoloLifter commands (node 15)

The lift's commands, including the fan relay #HLF1 / #HLF0, are documented in
DroidNet-HoloLifter: https://github.com/travisccook/DroidNet-HoloLifter/blob/main/docs/SERIAL_COMMANDS.md

## HoloFan commands (node 16)

Provided by the HoloFan's own firmware (firmware/holofan). The fan
answers only while it is powered (HoloLifter relay on) and on the mesh.

Playback (silent):

    #HLFG<n>        play catalog item n (1-based, order of #HLFD)
    #HLFT<NN>       play by slot (HoloFan 0.9.3): the first clip on the card
                    whose name starts with NN- or NN_ (NN 0-99, one or two
                    digits, e.g. #HLFT03 or #HLFT3). The fan reads the MCU's
                    full card listing each time (not the 32-entry catalog).
                    No such clip: nothing plays, #HLFS err=noslot
    #HLFP           pause
    #HLFN / #HLFR   next / previous
    #HLFB<1-3>      brightness
    #HLFV<1-3>      volume, capped at 2 since HoloFan 0.9.0 (#HLFV3 sets 2)
                    unless the cap is raised with #HLFH3 (0.9.11, below).
                    At volume 3 the ESP can brown out and the audio can go
                    silent for ~10 s (docs/HOW_IT_WORKS.md, section 7). The
                    MCU comes up at volume 3 after every power cycle. A 3
                    is lowered to 2 when the
                    fan connects to the MCU, read back, and sent again if
                    the MCU dropped it (0.9.1); since 0.9.8 the fan also
                    reads the settings every 30 s and right after any clip
                    upload/delete/format, and lowers a 3 set behind its back
                    (MCU re-init, IR remote, phone app) at once, except
                    during a #HLFY test. The fan's local UART0 console
                    (mcu m 3) is not capped
    #HLFM / #HLFU   mute / unmute. The MCU's lowest volume is 1 (there is no
                    0), so mute sets volume 1; unmute restores the volume from
                    before the mute. Not muted: unmute does nothing, except at
                    volume 1 it sets volume 2
    #HLFL<O|A>      loop one / all
    #HLFW<1|0>      panel switcher on / off
    #HLFC           re-read the clip list from the SD card
    #HLFA<0|1>      Bluetooth off / on (0.9.9; saved; OFF is the default). The
                    fan's LED/SD controller drives a Bluetooth audio chip
                    that pairs with nearby speakers by itself (vendor: "The
                    Atom automatically pairs with the nearest available
                    Bluetooth device"); a droid on the mesh needs none of it. The fan
                    switches it to the saved choice at every connect, every
                    30 s settings read and after clip uploads, reads it back,
                    and reports a change behind its back ("[mcu] UNPROMPTED:
                    ble off->on (switched off)") or a controller that will
                    not switch (once). Raw #HLFX c / d are undone within 30 s:
                    use #HLFA. CAUTION: the speaker is in the stationary
                    body, so Bluetooth may be how the audio reaches it; if
                    the fan is silent with Bluetooth off, send #HLFA1

Queries (reply, from the fan's cached state; never waits on the MCU):

    #HLFS -> #HL:FAN st=<READY|NOMCU> idx=.. name=.. play=.. prog=.. total=.. br=.. vol=.. loop=.. sw=.. err=..
             st     READY, or NOMCU when the LED/SD MCU does not answer (playback
                    commands are then dropped; the fan keeps retrying every 10 s)
             idx    catalog index of the clip playing (0 = not in the catalog)
             name   clip name (first 24 chars), - if none
             play   ing (playing), idle, or ? (unknown)
             prog / total   playback progress as the MCU reports it
             br / vol       1-3 (0 = not known yet)
             loop   all | one
             sw     on | off | ? (the MCU never reports it; known once set)
             err    result of the last action: none, nomcu, badindex,
                    noslot, badlevel, badname, txfail, timeout, nouart (UART1
                    to the MCU never came up)
    #HLFD -> #HL:FANDCIM n=<count> 1=<name> 2=<name> ...
             n is the full count; names that do not fit one mesh command
             (187 chars) are left off the end. Up to 32 clips are kept.

Diagnostics (bench):

    #HLFQ -> #HL:FANDIAG st=.. rxb=.. tx=.. tries=.. ans=.. rx5=.. sync18=.. rst=.. chg=.. v0=.. cap=.. txp=.. last=..
             MCU link counters: rxb bytes from the MCU (0 = it never
             answered), tx lines sent, tries get_setting attempts, rst the
             ESP's last reset reason (1 power-on, 3 software, 9 brownout),
             chg the changes nobody commanded (0.9.8, see below), v0 the
             volume the MCU reported at the first connect before the cap
             (0.9.8; 0 = not read), txp the mesh TX power in dBm that #HLFO
             saved (0.9.10; 20 = the default), last the last bytes received
    Unsolicited RTERM lines (0.9.8), sent to the WCB that last sent an
             #HLF/#HLZ command:
             "[fan] MCU at connect: ble on, loop all, br 2, vol 3 (capped
             to 2), angle 164; playing <clip> <progress>/<total>" once
             after each ESP boot, at the first command (an OTA or ESP
             reset does not power-cycle the MCU; a relay cycle does)
             "[mcu] UNPROMPTED: vol 2->3 (capped)", "loop one->all",
             "ble on->off", "br ..", "angle ..", "clip A->B" (loop one),
             "after NOMCU: ..": the MCU changed without a command of ours
             (seen by the 30 s settings read or the play poll); not counted:
             changes right after a clip lease, reported as "[mcu] after a
             clip lease: ...; restored ..." (docs/MAINTENANCE_API.md, dark).
             Raised while off the mesh (maintenance): "[mcu] while off the
             mesh: UNPROMPTED: ..." / "... after a clip lease: ..." at the
             first command back. A raw #HLFX line that can change the MCU
             makes the next read a new baseline (no report)
             "[mcu] mculog:...": what the MCU prints by itself (0.9.2)
    #HLFX<cmd> [args]   one raw line to the MCU; its reply comes back as an
             RTERM line ([TERM:16][mcu] ...). Over the mesh only read and
             playback commands pass (never v, u, w, y; m only 1-2)
    #HLFY<r><s>     bench test (HoloFan 0.9.4): volume 3 past the cap for s
             seconds (10-300) with the ESP's radio on (r=1), fully off
             (r=0: ESP-NOW and Wi-Fi stopped, no receive or transmit
             current) or receiving only (r=2, 0.9.6: the node sends
             nothing, the radio still receives; unicast frames sent to
             the fan meanwhile are still ACKed on air and counted). With
             r=0/2 the fan leaves the mesh before the volume step (0.9.6)
             and is off it for s seconds; then the old volume and the
             radio come back. Each volume is read back from the MCU
             (0.9.5: up to 3 get_setting tries; 0.9.7: after the MCU has
             settled): the hold starts only once the MCU reports 3 (else
             "[test] STOPPED"), and the radio comes back once it reports
             the old volume (at most ~10 s later, whatever it reports).
             RTERM lines (to the WCB that sent #HLFY; held while the
             radio is off, sent after it is back): "[test] <n>: volume 3
             for ...", "[mcu] volume 3 requested, MCU reports 3" and a
             timed "[mcu] vt: ..." trace of that exchange, the same for
             the restore, then "[test] ... no reset; volume 2 again (read
             back)". "RESTORE FAILED", "HOLD INTERRUPTED" or "NOTE:
             #HLFE/#HLFJ had the brownout reset off" lines need a look.
             No closing line: the ESP reset. Since 0.9.6 the test's phase
             and time are kept in RAM that survives a software reset (the
             brownout handler is one), and the first #HLF/#HLZ command
             after the reboot gets "[test] RESET during #HLFY<param>
             (test n): phase <PRE|QUIET|STEP|HOLD|RESTORE|REJOIN>, x s
             into the hold, y s into the test; rst=<reason>". Refused
             when the MCU is not READY, off the mesh, or while a test
             runs; maintenance is refused while a test runs. Never in a
             show: volume 3 can brown the fan out and cut its audio
    #HLFE<lo><hi><s>  sag meter (0.9.6): the ESP's brownout comparator
             watches its own supply for s seconds (10-300), its threshold
             stepping through levels lo..hi 25 ms at a time (0.9.7: in a
             pseudo-random order): 3 = 3.10 V, 4 = 2.92, 5 = 2.76,
             6 = 2.64, 7 = 2.51 V (Espressif's estimates; chip-to-chip
             spread unknown; level 2 refused since 0.9.7). A 1 ms poll
             reads the comparator's latched event. Report at the end:
             "[sag] <s> s, ms crossed/watched: L3(3.10V)=a/b ...; det n"
             and timeline lines "[sag] s 0-99: --4-5--..." (one character
             per second: the deepest level crossed in it, '-' none).
             While it runs there is NO brownout reset (flash close and RF
             power-down are off too); a restart puts 2.51 V back first
             (0.9.7: shutdown handler and panic-handler wrapper). Level
             7 counts would-be brownouts. #HLZ saves and maintenance are
             refused meanwhile. Run it longer than any #HLFY r=0/2 it
             covers, so its report is not held behind the radio
    #HLFO<dd>       mesh TX power (0.9.10; saved): dd dBm, two digits, 02-20;
             00 = the default, 20 dBm. Used whenever the fan is on the
             mesh, from the next transmission. Reply: "[mesh] TX power <dd>
             dBm saved (maintenance always uses 20); the radio is at <x>
             dBm", x read back from the radio (it gave back the 8.00 it was
             set to on 2026-10-13, although Espressif documents steps of 2,
             5, 7, 8.5, 11, 13, 14, 15, 16.5, 18 and 20 dBm, the one at or
             under dd). Maintenance mode (Wi-Fi: uploads, OTA) always transmits at
             20 dBm, and since 0.9.12 the fan plays at volume 1 meanwhile
             (docs/MAINTENANCE_API.md). Why: at volume 3 the fan's own 20 dBm bursts dip its
             supply and start ~10 s audio silences (bench 2026-10-13); the
             dips shrink with the power and were gone at 2 dBm (#HLFJ).
             A power too low for the WCBs to hear the fan (missing replies,
             OFFLINE lines) does not lock it out: it still hears them, so
             #HLFO20 or #HLFO00 puts it back. Refused during #HLFE/#HLFJ
             and #HLFY
    #HLFH<2|3>      volume cap (0.9.11; saved): the highest volume the fan
             sets or leaves alone. 2 = the default (as 0.9.0-0.9.10). 3: the
             MCU's own volume 3 is no longer lowered at boot, at the 30 s
             settings read or after a clip upload, and #HLFV3 sets 3; the
             command itself never raises the volume. #HLFH2 puts the cap
             back and lowers a 3 at once, with no firmware update. cap= in
             #HLFQ shows it. The MCU powers up at volume 3, so with the cap
             at 3 the fan is at volume 3 after every power cycle. Raw
             "#HLFXm 3" stays refused; #HLFY then restores the volume it
             found, up to the cap. Replies: "[fan] volume cap 3 saved: ..."
             / "[fan] volume cap 2 saved (the default): ...". Refused during
             #HLFE/#HLFJ and #HLFY. Why 3 is allowed at all: with the mesh TX power at 8 dBm (#HLFO08) volume 3
             ran clean on 04-Beeps and a real clip; the bench clip 03-Rings
             still reset the ESP at volume 3, at 8 and at 2 dBm, when the
             fan replied at a volume step or clip start
             (docs/HOW_IT_WORKS.md, section 7). A brownout reset
             costs the mesh link for a few seconds; the MCU plays on
    #HLFK<ss>       bench (0.9.8): mesh heartbeat every ss seconds (02-25;
             00 = the normal 10 s; back to 10 s at the next boot). WCBs
             mark a node OFFLINE after ~55 s without one (configurable per
             WCB; 33 s seen), so one lost beat must still fit. To tell
             whether something follows the fan's own transmissions
    #HLFJ<lo><hi><dBm>  TX dip probe (0.9.7): at TX power dBm (02-20, two
             digits; restored afterwards), for each level lo..hi, 20
             probe frames (250 zero bytes to the WCB group MAC: one burst
             each, no ACK, rejected by every WCB), each with an equal idle
             window before it. Report "[probe] <dBm> dBm, 20 frames/level,
             crossed (idle): L3(3.10V)=x (y) ...": how many bursts dipped
             the ESP's supply under each level, and the background. About
             2 s per level; no brownout reset meanwhile (as #HLFE).
             Refused while another instrument runs or the radio is off

Pacing (0.9.7): the fan lets the LED/SD MCU settle after every action
(until it has been quiet 300 ms, at most 3 s): the MCU drops or misreads a
line that arrives while it is busy (a get_setting 4 ms after "m" ran as
"e", previous clip). One action therefore takes 0.3-1 s (#HLFT about
1 s); up to 8 wait in a queue and more are dropped. Send at most about 3
actions a second and one #HLFT a second.

Query replies are mesh broadcasts: no ACK and no retry. From inside a droid's
dome many of them never reach a WCB in the body (about 85% were lost in one
test), and
the fan's BOOT announces are broadcasts too, so a missing reply or a missing
"came ONLINE" line does not mean the fan is down. RTERM lines are unicast and
arrive: #HLFXget_setting is a reliable liveness check.

Fan power is NOT a fan command: the HoloLifter's relay (#HLF1 / #HLF0 to
node 15) is the only way to power the fan. The fan ignores #HLF1 / #HLF0.

The fan's console is UART0 on its programming header (115200): the same
`mesh ...` commands as the HoloLifter's USB console, plus `hl <#HL...>` and
`help` (firmware/holofan/README.md). Maintenance mode (#HLZX):
docs/MAINTENANCE_API.md.

## Node commands: #HLZ (both nodes)

    #HLZP<0|1>      query replies off / on (broadcast; default on)    saved, live
    #HLZI<1-19>     node id                                           saved, after reboot
    #HLZN<alias>    alias / WDP name: 1-24 of A-Z a-z 0-9 _ . -,
                    not starting with a digit                          saved, after reboot
    #HLZKS<p><hex>  Wi-Fi profile p (1 = home, 2 = DroidNet) SSID, hex  saved
    #HLZKP<p><hex>  profile p passphrase, hex (empty = open network)    saved
    #HLZX<p>        enter maintenance mode on profile p (see docs/MAINTENANCE_API.md)
    #HLZQ           query (reply):
                    #HL:NODE id=.. name=.. reply=on|off ch=.. q=.. v2=<0|1> up=<0|1>
                             rx=.. rej=.. crc=.. dup=.. rt=.. gu=..
                    (rx frames accepted, rej gate rejects, crc checksum
                     failures, dup duplicates dropped, rt retries, gu sends
                     given up)

Hex example: SSID "My Home" is 4D7920486F6D65, so #HLZKS14D7920486F6D65.

Security: WCB meshes are unencrypted, so these can be sniffed by anyone in
radio range. The mesh password, MAC octets, channel and peer floor can only be
changed over USB (HoloLifter) or the fan's UART header, so a mesh command can
never cut a node off the mesh.

## Fan console (UART0, 115200 baud)

The fan's console is on its programming header (README.md, "Connecting the
fan"): holes 3 and 4 at 115200 baud. `help` prints:

    HoloFan <version> console (UART0 115200):
      hl <#HL...>                  run a mesh command locally (e.g. hl #HLFS)
      mesh                         show mesh config and counters
      mesh pw <pw> | oct <hh> <hh> | ch <1-11> | q <0-20> | crc on|off
      mesh id <1-19> | name <alias> | reply on|off
      mesh ssid <1|2> <name> | pass <1|2> [pass] | clear <1|2> | off | restart
      mesh maint <1|2>             maintenance mode (Wi-Fi + HTTP OTA) on profile 1|2
      radio                        ESP-NOW receive diagnostics
      mcu <cmd> [args]             send one line to the LED/SD MCU, print its raw reply (never v)

The `mesh ...` settings are the same as on the HoloLifter's USB console; the
mesh password, MAC octets, channel and peer floor can only be set here, never
over the mesh.

Bench and diagnostics only: #HLFY, #HLFE, #HLFJ, #HLFK and #HLFH3. They exist
to measure the fan; #HLFY and #HLFH3 run it at volume 3, which can brown it
out.

## Design rationale

Why #HL: no overlap with the ~316 commands of the droid ecosystem (MarcDuino,
WCB, JawaLite, FlthyHPs, HCR Vocalizer, motor controllers). Prefixes deliberately avoided: @ (MarcDuino),
? (WCB config), ; (WCB routing), <> (Vocalizer), DT and S (FlthyHPs), #LE
(JawaLite), and bare single letters.
