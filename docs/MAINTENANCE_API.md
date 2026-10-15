# Maintenance mode HTTP API (contract for DroidNet and other uploaders)

Shared by DroidNet-HoloLifter and DroidNet-HoloFan; the fan-only parts (clips,
uploads) apply to the fan. Both nodes implement it. This is the only way to
update either board after its one wired flash.

## Entering maintenance mode

1. Store a Wi-Fi profile on the node (once): over the mesh with
   #HLZKS<p>=<ssid> and #HLZKP<p>=<passphrase> (p = 1 home, 2 DroidNet; or
   in hex, docs/SERIAL_COMMANDS.md),
   or on the HoloLifter's USB console with "mesh ssid" / "mesh pass".
2. Send #HLZX<p> to the node (e.g. ;wHoloLifter,#HLZX2\r on a WCB), or
   "mesh maint <p>" on the HoloLifter's USB console.
3. The node leaves the mesh, joins the network and, once it has an IP
   address, announces itself over mDNS as <alias lower-cased, with _ and . turned into ->.local
   (hololifter.local, holofan.local) with an _http._tcp service on port 80.

HoloFan only, without the mesh: power-cycle the fan three times in a row,
each time on for 2-10 s (e.g. #HLF0 / #HLF1 on the HoloLifter's relay). The
third boot enters maintenance on profile 1 (profile 2 if 1 is unset). Only
power-on resets count, so brown-outs and software reboots never trigger it.
The fan's UART0 console also takes "mesh maint <p>".

HoloFan 0.9.12: the fan plays at volume 1 while it is in maintenance, where
Wi-Fi transmits at full power. The volume it had comes back 5 s after the
fan is on the mesh again, and 8 s after the boot when maintenance ended in
the reboot of an OTA. A power cycle in maintenance leaves the fan at its
power-up volume, 3.

Refusals (the node stays on the mesh): profile has no SSID; already in
maintenance; HoloLifter only: the lift is moving.

While in maintenance the HoloLifter's motor driver is disabled; its limit
switches, e-stop and USB console keep running. The node is off the mesh:
commands sent to it are lost, and WCBs mark it offline after about 55 s.

Commands sent to the node after #HLZX in the same burst are not run, and are
dropped when the node leaves the mesh. On return to the mesh the HoloLifter
re-enables its motor driver; if a move was commanded while it was away (for
example a USB 'up'), the lift is stopped first, which, like any stop, also
cuts the holofan relay. Wi-Fi auto-reconnect is switched off on return so
the radio stays on the mesh channel.

## Leaving maintenance mode

The node returns to the mesh (3 boot announces, heartbeat, WDP adverts) on:

- POST /api/done;
- no IP address within 20 s of starting to join;
- the Wi-Fi link down for 20 s while serving;
- a power cycle.

There is no idle timeout.

## Endpoints (port 80, no authentication)

    GET  /api/info
         200 {"node":"HoloLifter","fw":"2.0.0","slot":"app0","mode":"maint","spi":false}
             (the HoloFan reports "spi":true: it has the clip routes, plus
              "heap" free heap bytes, "hmin" the lowest free heap since boot
              (0.8.5) and "blk" largest free DMA block)
             HoloFan "last": the last clip operation (PUT, DEL, FMT), for
             bench diagnostics. After a PUT:
               PUT <name> <err> f=.. b=.. crc=.. wmax=.. igmax=.. fl=../../.. win=.. low=..
                   it=.. us=.. first=.. gap=.. worst=.. hiw=.. spi=..k/..
                   rx=..
             where
               f       SPI frames (1440 bytes) sent to the LED/SD controller
               b       body bytes received, then '-' complete, 'e' read
                       error, 's' stalled out, 'f' flash error
               crc     CRC32 (zlib) of every byte sent over SPI, the zero
                       padding included: equals zlib.crc32 of the clip padded
                       with zeros to a multiple of 28800 (since 0.7.2)
               wmax    longest 20-frame window, ms: its first frame to its
                       last (since 0.8.1)
               igmax   longest gap between windows, ms: the last frame
                       of one to the first of the next (since 0.8.1)
               fl      staging flash: total erase ms / total write ms /
                       longest single flash call ms (since 0.7.2)
               win     20-frame windows acknowledged on GPIO18
               low, it, us   the last window's GPIO18 wait: LOW samples (100
                       = ok), samples taken, microseconds
               first   ms from the start of the upload to the first frame
               gap     longest pause between two frames inside the window
                       that was open at the end (0 after a full upload)
               worst   longest pause between two frames, ms
               hiw     frames that first waited for GPIO18 HIGH (the
                       controller still busy with an earlier window; since
                       0.7.2)
               spi     the SPI clock as the driver set it, in kHz, and its
                       source, pll or xtal (e.g. spi=40000k/pll; since 0.7.2;
                       see PUT ?mhz=&clk=)
               rx      the controller's last bytes, made printable
             HoloFan "wifi" (since 0.8.0):
               {"rssi":<dBm>,"ch":<channel>,"disc":<n>,"reason":<code>}
               rssi, ch  the access point as the fan hears it now (null
                         while not associated)
               disc      Wi-Fi disconnects since maintenance started
               reason    the last disconnect's reason code (esp_wifi
                         wifi_err_reason_t)
             After a resumable upload "last" starts with UPL, has the same
             b= crc= wmax= igmax= fl= fields (wmax/igmax: since 0.8.1;
             stg=<B/s> right after igmax: since 0.8.2, the staging rate
             before the first frame went out, i.e. bytes staged by the
             first pop over the time from session open to it) and adds:
               req=.. recon=.. dup=.. gap=.. stall=.. quiet=.. ringmin=..
               rssi=<min>/<max> disc=.. stack=..
             (see "Resumable upload" below), and the body end codes gain
             'd' (the buffer ran dry) and 'a' (aborted).

    POST /api/ota
         body: the raw application image (the .bin PlatformIO / idf.py builds,
               NOT a merged image with bootloader); Content-Length required;
               Content-Type must not be multipart (use application/octet-stream)
         200 {"ok":true}            image verified and staged; the node reboots
         400 {"ok":false,"err":"length"}
         500 {"ok":false,"err":"<reason>"}   nothing changed, still serving

    POST /api/done
         200 {"ok":true}            the node returns to the mesh

    HoloFan only:
    GET    /api/clips
           200 [{"name":"02-Name.bin"}, ...]      every name on the SD card
           503 {"ok":false,"err":"nomcu"|"busy"}
    PUT    /api/clips/<name>[?mhz=<10|20|40>&clk=<pll|xtal>]
           body: the clip bytes (Content-Length required; the fan pads to
                 a multiple of 28800 itself); name NN-<chars>.bin, may be
                 percent-encoded in the URL; no '/', '\' or ':'
           query (optional, since 0.7.2; for single-variable bench tests):
                 the SPI clock of this upload only
                   mhz   10, 20 or 40 (MHz); default 40
                   clk   pll (PLL_F40M, the IDF default) or xtal (the
                         40 MHz crystal, as the stock firmware);
                         default pll
                 Either, both (any order) or none; the next PUT without
                 them is back to 40 MHz from pll (the clock of every
                 upload before 0.7.2). Values exactly as above (lower
                 case, no percent-encoding); anything else, an unknown or
                 repeated key, or an empty pair is a 400 "query" and
                 nothing reaches the LED/SD controller. "last" shows the
                 clock the driver actually set (spi=).
           The fan first buffers up to ~900 KB of the body in flash (its
           spare firmware slot) before it starts writing to the SD card, so
           expect no reply for the whole transfer; Wi-Fi stalls of up to
           about a minute are survived; a dropped connection loses the upload.
           From the dome use the resumable upload below.
           200 {"ok":true,"size":<bytes>}
           400 {"ok":false,"err":"name"|"length"|"query"}
           409 {"ok":false,"err":"exists"}        delete it first
           409 {"ok":false,"err":"upload"}     a resumable upload is running
           500 {"ok":false,"err":"cf"|"rf"|"sync"|"spi"|"recv"|"noverdict"|"nomem"|"flash","partial":<bool>}
               partial true: a half-written file may remain; DELETE it
           503 as GET, or {"ok":false,"err":"verify"}: a fresh firmware
               image is still proving itself (about 30 s after an OTA)
           Every PUT error closes the connection after the reply.
           An upload erases the fan's previous firmware image (kept in the
           spare slot after an OTA); the running image is unaffected.
    POST   /api/card/format?confirm=FORMAT-SD-CARD
           erases the SD card (every clip). 200 {"ok":true,"clips":[]};
           400 {"ok":false,"err":"confirm"} without the exact token
    DELETE /api/clips/<name>
           takes about 5-9 s: the fan lists the card, sends the delete,
           waits 3 s (the MCU never confirms it) and lists again; up to
           about 20 s when the MCU is busy, because an empty or cut-off
           listing is asked for again, up to 3 times (HoloFan 0.9.15).
           Give it a 30 s timeout, and after a timeout list the clips
           (GET /api/clips) before trying again.
           200 {"ok":true}             gone from a full listing
           400 {"ok":false,"err":"name"}
           404 {"ok":false,"err":"notfound"}
           500 {"ok":false,"err":"still","partial":true}
                                       still listed, or no full listing came
                                       back to confirm it (0.9.15)
           503 as GET

    The clip routes do not check the mode: they work whenever the fan's
    HTTP server runs (today: in maintenance mode, the only time the fan has
    an IP address). One request is served at a time; an upload from the
    dome runs at about 15-25 KB/s, so a 2 MB clip takes a couple of minutes.

## Resumable upload (HoloFan 0.8.0)

For links that drop. The fan buffers up to 921,600 bytes of the clip in
flash and keeps feeding its LED/SD controller from that buffer while the
client reconnects. One session at a time.

    The controller's timing rule (bench 2026-10-07): it takes the clip in
    whole 20-frame windows and aborts the transfer when SPI data stops for
    more than about 3-5 s anywhere. So windows must arrive whole and at
    most about 3 s apart. Wi-Fi from the dome stalls for seconds to
    minutes, so the fan holds a reserve and spends it at the controller's
    pace.

    Pacing (0.8.2): the default pace is reserve. Before sending starts the
    fan buffers the ring minus one window (620 frames, about 890 KB; knob
    pre) and then creates the file. While the body is still arriving it
    sends a window (20 frames back to back) only when 20 frames are
    buffered AND (the ring is full OR keep ms (default 3000) have passed
    since the previous window's last frame): windows every 3 s, never a
    partial one, no trickle. With a full ring the reserve covers about 31
    windows x 3 s = about 90 s of Wi-Fi outage. Once the body is
    complete everything goes out at once. The session fails fast
    ("recv", body end 'd') when no frame has gone out for dry ms (default
    4000 for reserve), because the controller would have aborted by then.
    Link requirement: for a clip larger than the ring the client must
    average at least about 9 KB/s (a window every 3 s is 9.6 KB/s;
    the reserve covers the difference for a while); below that the reserve
    drains and the session ends 'd'. Before the first window the
    10-minute idle rule applies instead.
    Clients: resolve the fan's name ONCE (IPv4) and reuse the address.
    On macOS getaddrinfo("holofan.local") takes about 5 s (it waits for an
    AAAA answer the fan's mDNS never gives); paying that per connection
    cut uploads to about 2.6 KB/s (bench 2026-10-07). After any stall,
    reconnect at once (about 5 s socket timeout): a fresh connection
    recovers faster than TCP back-off on the old one, and the fan gives up
    a silent chunk after 3 s.
    Connections (0.8.5): the fan serves at most 3 HTTP connections (2 more
    may wait; a new one closes the least recently used), each with a
    4,288-byte TCP receive window, and has 6 static + 10 dynamic Wi-Fi RX
    buffers (0.8.6). That keeps the heap above ~5 KB during a session:
    before 0.8.5 queued connections used it up and the fan stopped
    receiving (still associated) until the session ended. Use one
    keep-alive connection for the PUTs and at most one more for progress
    GETs. From the dome a connection carries about 15-35 KB/s.
    Window gate (0.8.7): while a window waits for the controller's ack (a
    ~50 us LOW on GPIO18) the fan does not write chunk bytes to flash: a
    flash erase stops the chip for ~40 ms and the ack was missed (session
    failed "sync"). A PUT therefore waits while each window is sent and
    acknowledged: normally tens of ms, up to a few seconds when the
    controller is slow to ack (its own SD write); clients see nothing else.
    pace=burst (0.8.1: windows as soon as 20 frames are buffered, igap
    apart, no reserve; dry default 30000) remains for experiments.
    pace=trickle (0.8.0: one frame per second) is refused since 0.9.1 (400
    "query"): the controller rejects windows spread over seconds, and a
    trickle waited inside a window while the window gate kept the chunk
    handler, and with it the whole HTTP server, blocked. igap and (for
    reserve) keep must be below dry (else 400 "query"). 0.8.2 also turns Wi-Fi A-MPDU RX off, so the access point
    sends single frames (the rotor's antenna loses long aggregates).

    POST   /api/upload?name=<NN-x.bin>&size=<bytes>[&mhz=<10|20|40>&clk=<pll|xtal>]
                                    [&pace=<reserve|burst>&keep=<ms>&igap=<ms>&pre=<frames>&dry=<ms>]
                                    [&dark=<0|1>]
           Opens a session. name and size as for PUT /api/clips/<name>
           (name percent-encoded). A POST with the same name and size while
           that session runs returns it again (safe to retry).
           Timing knobs (since 0.8.1; each optional, at most once; a bad
           or repeated value is 400 "query"):
             pace  reserve (default since 0.8.2): whole windows, keep ms
                   apart while the body arrives (see Pacing). burst: send
                   each 20-frame window back to back once a whole window
                   is buffered. (trickle: refused since 0.9.1.)
             keep  1000..10000 ms (default 3000; reserve only; must be
                   below dry): least time between one window's last frame
                   and the next window's first unless the ring is full.
             igap  0..60000 ms (default 0; must be below dry): least time between one window's
                   last frame and the next window's first (burst only).
             pre   20..617 frames (default 0 = 620, the ring minus one
                   window): buffered before the file is created and
                   sending starts.
             dry   1000..600000 ms (default 4000 for reserve, 30000 for
                   burst): the session fails
                   ("recv", body end 'd') when no frame has gone to the
                   controller for this long while the body is incomplete.
             dark  0 (default) or 1 (0.9.8): before pausing for the upload,
                   play 99-Black.bin if the card has it (dark and silent),
                   else set brightness 1. Note: a 99-Black.bin on the card
                   also plays (2 s of black) in loop all and with next/prev.
           After every clip lease that touched the controller (upload,
           delete, format; not a list) the fan puts back what the
           controller changed: volume (at most 2), loop mode (if known),
           brightness, then the clip: in loop all a playing fan is left
           alone; otherwise the clip that was playing starts again (or is
           shown paused) if a fresh listing still has it. A mesh command
           after the lease wins over the restore of what it sets; the
           restore is dropped after 60 s. RTERM "[mcu] after a clip lease:
           <what changed>; restored ..." (in maintenance: "[mcu] while off
           the mesh: ..." at the first command back on the mesh).
           200 {"ok":true,"id":<n>,"staged":<bytes>,"size":<bytes>}
           400 {"ok":false,"err":"name"|"length"|"query"}
           409 {"ok":false,"err":"exists"}           delete it first
           409 {"ok":false,"err":"busy","name":"..","state":".."}
           500 {"ok":false,"err":"nomem"|"flash"}
           503 {"ok":false,"err":"nomcu"|"busy"|"verify"}
    PUT    /api/upload?id=<n>&off=<bytes>
           body: the clip's bytes from offset off (Content-Length
           required; any length; 32 KB recommended). off may be below
           staged (the overlap is dropped) but not above it.
           200 {"ok":true,"staged":<bytes>}
           400 {"ok":false,"err":"length"|"query"}   past the size, bad query
           404 {"ok":false,"err":"id"}               no such session
           408 {"ok":false,"err":"recv","staged":<bytes>}   nothing for 3 s,
               or no ring space for 3 s (the ring can fill while the
               controller creates the file): resume from staged
           409 {"ok":false,"err":"offset","staged":<bytes>}
           409 {"ok":false,"err":"state","state":"..","staged":<bytes>}
           Every reply but 200 closes the connection. Bytes received before
           a connection dies stay staged.
    GET    /api/upload
           200 {"state":"none"|"prefill"|"sending"|"ok"|"failed","id":..,
                "name":"..","size":..,"staged":..,"ring":<frames buffered>,
                "sent":<body frames sent>,"err":"..","partial":<bool>,
                "link":{"req":..,"recon":..,"dup":..,"gap":..,"stall":..,
                        "quiet":<ms>,"ringmin":<frames>,"stg":<B/s>,
                        "rssi":[min,max],
                        "disc":..,"stack":<bytes>,
                        "heap":<bytes>,"hmin":<bytes>},
                "pace":{"mode":"reserve"|"burst","igap":<ms>,
                        "keep":<ms>,"pre":<frames>,"dry":<ms>}}
           prefill: buffering, nothing on the SD card yet. sending: the
           controller has the file. A finished session stays readable
           until the next POST or a reboot ("none").
    DELETE /api/upload[?id=<n>]
           Abort. 200 {"ok":true,"state":".."}; the session then ends as
           failed/abort and the partial file is deleted.
           404 {"ok":false,"err":"id"}

    How a session fails: no bytes for 10 minutes before sending starts
    (recv, b=..s); once sending, no frame sent for `dry` ms (default 4000 for reserve, 30000 otherwise)
    while the clip is incomplete (recv, b=..d); DELETE (abort); a flash error (flash); the
    controller's errors as for PUT (cf, rf, sync, spi, noverdict); the MCU
    link busy for 30 s (busy).

    While a session is in prefill or sending: PUT and DELETE
    /api/clips/<name>, POST /api/card/format, POST /api/ota and POST
    /api/done answer 409 {"ok":false,"err":"upload"}; GET /api/clips
    answers 503 busy. Maintenance mode does not end on link loss while a
    session runs (it rejoins).

    Client algorithm (tools/bench/clip_put.py; DroidNet should do the same):
    1. POST until it answers (it is idempotent). exists or busy: stop.
    2. PUT from staged on a keep-alive connection with a ~5 s socket
       timeout. On any error, timeout, 408 or 409 offset: drop the
       connection, wait 1 s, GET /api/upload until it answers and continue
       from its staged. 404 id, or state none / another id: the fan
       rebooted; a partial file may remain (DELETE
       /api/clips/<name>?force=1), start again.
    3. When staged == size, poll GET every 2 s until ok or failed.
    The fan advertises a 536-byte TCP MSS (0.8.0), so no client-side
    segment tuning is needed.

Example with curl:

    curl http://hololifter.local/api/info
    curl -X POST -H 'Content-Type: application/octet-stream' \
         --data-binary @.pio/build/esp32thing_plus/firmware.bin \
         http://hololifter.local/api/ota
    curl -X POST http://hololifter.local/api/done

    curl http://holofan.local/api/clips
    curl -X PUT -H 'Content-Type: application/octet-stream' \
         --data-binary @99-Test.bin http://holofan.local/api/clips/99-Test.bin
    curl -X PUT -H 'Content-Type: application/octet-stream' \
         --data-binary @99-Test.bin \
         'http://holofan.local/api/clips/99-Test.bin?mhz=10&clk=xtal'
    curl -X DELETE http://holofan.local/api/clips/99-Test.bin

    python3 tools/bench/clip_put.py 03-Rings.bin http://holofan.local 03-Rings.bin
    python3 tools/bench/clip_put.py --abort http://holofan.local

## After an OTA: automatic rollback

The new image boots in the bootloader's PENDING_VERIFY state. It keeps
itself only if, within 30 s, it reaches the mesh. (The fan does not require
its LED/SD controller to answer: a silent controller must not block the
update that fixes it.) Otherwise it rolls back to the previous image on its own.
A node with no mesh password configured is not rolled back for that reason.

## Security

No authentication: anyone on the home or DroidNet network can upload firmware
or delete clips while a node is in maintenance mode. Mitigation: maintenance
mode is entered only on command and ends on done, link loss or power cycle.
