<!-- Research notes, 2026-10-05. Summaries of greghulette/WCB (GPL-3.0) and greghulette/WCBClient (MIT) written in our own words with citations; no code copied. Scratch-clone paths below refer to temporary read-only clones and may not exist. -->

# WCB mesh deep review (for HoloLifter + holofan ESP32-C2 as first-class mesh nodes)

Date of review: 2026-10-05. Everything below was read from source; citations are file:line.

Path shorthands:

- `WCB/` = github.com/greghulette/Wireless_Communication_Board-WCB (firmware repo, GPL-3.0; WCB.ino is 8244 lines, **grep it with `grep -a`**, it contains non-UTF8 bytes)
- `CLIENT/` = github.com/greghulette/WCBClient, shallow-cloned to `<scratchpad>/WCBClient` (read-only). `CLIENT/src/WCB_Client.h` and `.cpp`.
- `NAVI/` = github.com/greghulette/NaviCore, cloned to `<scratchpad>/NaviCore` (reference client, MIT).
- `WCMD/` = github.com/greghulette/WcbCmd, cloned to `<scratchpad>/WcbCmd` (MIT).
- `WIKI/` = github.com/greghulette/Wireless_Communication_Board-WCB.wiki (public; cloned to `<scratchpad>/wcbwiki`). The wiki is partly STALE (see H).

---

## A. WCB_Client

### A1. Location, version, license, platforms

- Repo: https://github.com/greghulette/WCBClient (library name **WCB_Client**; header `WCB_Client.h`). Public, MIT (`CLIENT/LICENSE`, "Copyright (c) 2026 Greg Hulette"). Last push 2026-10-05. Version **1.17.2** (`CLIENT/library.properties:2`). Tags exist (1.12.1, 1.13.0, 1.13.2 ...). In Arduino Library Manager as `WCB_Client`. A bench mirror also exists in greghulette/Arduino-Code `libraries/WCB_Client` (CLIENT/CLAUDE.md rule 1 says the WCBClient repo is the only copy that ships).
- Not named "WCBClient" in code: class is `WCB_Client`; the wiki page says `WCBClient`/`<WCBClient.h>` (stale).
- Platform: `architectures=esp32` only. Hard dependencies on **Arduino-ESP32**: `<Arduino.h>`, `<WiFi.h>`, `<Preferences.h>` (NVS for learned peers), `HardwareSerial`, `Serial.printf`, `millis()`, Arduino `String`, `esp_random`, `std::atomic`, FreeRTOS spinlocks (`CLIENT/src/WCB_Client.h:3-6`, `WCB_Client.cpp:1-4`). README requires "esp32 by Espressif v3.x" (README "Requirements"); CI compiles `esp32:esp32@3.3.4` for `esp32s3` (CLIENT/CLAUDE.md "Verifying"). The ESP-NOW receive callback uses the IDF-5 signature `esp_now_recv_info_t*` (`WCB_Client.cpp:2949`) so **Arduino-ESP32 2.x / IDF 4.4 will not compile it**.
- **It does NOT compile for plain ESP-IDF (no Arduino)** without porting. It could compile on ESP32-C2 only if you run arduino-esp32 3.x as an IDF component; it was only ever compiled for classic ESP32 / S3 as far as the repo shows (no C2 mention). No test suite ("No test suite... Real verification is on hardware", CLIENT/CLAUDE.md).
- Companion libs (all MIT, same author): **WcbCmd 0.9.1** (translates `;M/;A/;L/;H/;D` verbs to device wire bytes; shared by WCB firmware and NaviCore; irrelevant for our two devices), **NaviCore** (ESP32-S3 firmware, reference WCB_Client host, MIT). Third-party consumer: thePunderWoman/Amidala (`src/wcb_client_controller.cpp`) shows the "queue from RX callback, handle in loop" pattern.

### A2. Public API (CLIENT/src/WCB_Client.h)

Construction/lifecycle

- `WCB_Client(oct2, oct3, password, wcb_quantity, device_id, cmdCb=nullptr, statusCb=nullptr)` (:~499). Singleton (`_instance`), global scope OK, does not touch hardware.
- `bool begin()` -- WiFi STA (or AP_STA if SoftAP is up), `WiFi.disconnect()`, pins channel, sets MAC, `esp_now_init`, registers rx cb, adds peers 1..quantity (minus self) + broadcast peer + special peer, restores NVS learned peers (`WCB_Client.cpp:51-176`). Returns false on bad id/ESP-NOW failure. Always check it; `update()` is a no-op if not started.
- `void update()` -- call every loop; drives boot announces, heartbeat, offline sweep, fragmented-send job, bulk, WDP cadence, neighbor aging, deferred auto-join, retries (`.cpp:204-375`). Do not block loop().
  Send
- `send(wcb, "cmd", ensured=true)` unicast text; >187 chars (with CRC; 199 without) auto-fragments via MGMT packets (up to 16 x 179 = 2864 chars; non-blocking; one job at a time; unicast only) (`.h` doc ~ :560; `.cpp:376-575`).
- `broadcast("cmd", ensured=true)` (no fragmentation; JSON `{...}` is forced best-effort, `.cpp:392-408`).
- `sendRaw(wcb, port1-5, data, len<=177)` -> target id 97 on the **249 B non-ETM struct** (`.cpp:581-640`); `sendKyber(data,len<=178)` -> target 98 broadcast (`.cpp:650-690`).
- `sendRawPacket(wcb, data, len)` unicast any custom struct (OTA ACKs etc.) (`.cpp:1515`).
- `sendToSpecialPeer`, `enableSpecialPeer(id=20)`, `isSpecialPeerOnline()`.
  Receive/callbacks
- `onCommand(void(*)(uint8_t sender, const char* cmd))` -- runs on the **ESP-NOW/WiFi task**; CRC suffix already verified and stripped; duplicates (same seq) already dropped. `onStatusChange(void(*)(uint8_t id,bool online))`.
- `onRawPacket(void(*)(const uint8_t* mac,const uint8_t* data,int len))` -- any packet whose length != 252 that passed the MAC-namespace gate (and was not an inbound MGMT frag / SEQ reply). Single callback; used for OTA (NaviCore).
- `onNeighbor`, `getNeighbor(n)`, `neighborCount()` (WDP consumer); `setAutoJoin`, `forgetPeer`, `clearLearnedPeers`, `isLearnedPeer`.
- `onBulkBegin/Chunk/Complete` (bulk transfer on loop task; protocol is a JSON-ish `bb/bc/bd` envelope in commands, `.cpp:902-1180`), `requestSequenceNames/requestSequence/saveSequence/deleteSequence` + callbacks (stored-sequence inventory; not needed by us).
  Identity (WDP)
- `setIdentity(type, fw, hwRev=nullptr, caps=nullptr)`, `setTemporary(bool)`, `setPortLabel(port1-5,label)`, `setMaestroIds(ids,count,bauds)`.
  Config/diagnostic
- `setChecksum(bool)` (default true), `setMeshChannel(1-11)` (before `begin()`; after begin it re-pins live when no SoftAP), `isOnline(id)`, `getPeerStats/getAggregateStats/getBroadcastSent/resetStats`, `monitorRaw/monitorSerial` (UART tap helpers).
- `WCBStream` (Pololu Maestro Stream adapter, `CLIENT/src/WCBStream.h`) and `WcbMgmt` (Wizard doorway, `CLIENT/src/WCB_Mgmt.h`) -- not relevant to us.

### A3. What it implements

ETM (seq/ACK/retries), heartbeat, **boot announce x3** (added 1.17.1), WDP advert (encode + decode + SOLICIT answer), CRC32, inbound dedupe, auto-join, fragmentation both directions, delivery stats, bulk transfer, sequence inventory. Specifics in B.

### A4. How a client receives commands, and replies

- Receive: a WCB (or another client) sends a COMMAND whose target = client's id (or 0 broadcast). Client ACKs, checks CRC, de-dups, calls `onCommand(senderID, cmd)` with the bare string. **The only provenance given is the sender's WCB number (1-20).** The WCB's own handler knows no more: a command received over ESP-NOW does not carry its sender into the firmware's handlers (`CLIENT/src/WCB_Client.h` ~:200 comment re `?SEQ,GET`).
- How a droid controller addresses a client: the controller (e.g. SHADOW) writes a line on a WCB serial port such as `;w<id|alias>,<payload>` (command char `;` default, `?` local, delimiter `^`; all changeable in WCB: WCB.ino:134-157). `processWCBMessage` (WCB.ino:6488-6628) parses `;w2,...`, `;w13,...` (multi-digit requires comma) or **alias form `;wHoloFan,...`** and unicasts via ETM to that peer. The payload (without `;w<target>,`) is what `onCommand` receives. Inference (not read end-to-end): the WCB strips the trailing `\r` of the serial line before processing, so the client sees `#HLU` not `#HLU\r`; confirm on hardware.
- Replies from a client: the client builds a _WCB command string_ and `send()`s it to a WCB:
  - `send(senderWcb, ";S<port>,<text>")` -> that WCB writes `<text>` + `'\r'` to its serial port `<port>` (0-5; 0 = USB) (`processSerialMessage` WCB.ino:6444-6486; `writeSerialString` appends `\r`, WCB.ino:1998-2001). This is how `#HL:OK` would reach SHADOW's serial line: `wcb.send(1, ";S2,#HL:OK")` (WCB 1, port 2 hypothetical).
  - `sendRaw(wcb, port, bytes, n)` writes exact bytes (no auto `\r`) to a WCB port (target 97, WCB.ino:228 `RAW_SERIAL_MAX_CHUNK 177`).
  - Anything starting `;w<other>,...` is routed onward by the receiving WCB (client -> WCB -> another node). Messages received from the mesh are marked `lastReceivedViaESPNOW` (one-hop loop prevention for broadcast-able tokens, WCB.ino:4319).
  - JSON-style telemetry convention: payloads starting with `{` are treated by WCBs as controller/telemetry JSON (NaviCore uses `wcb->send(sender, "{...ACK...}")`, NAVI/NaviCore.ino:2335). A WCB relays `{`-prefixed ETM payloads to USB only if a host subscribed (WCB.ino:4350-4380). **Do not start any holofan reply text with `{`**.
- Because a client doesn't know which serial port the controller sits on, the reply port must be configured on the client (NVS) or carried in the command (design choice for us).
- Gotcha: commands relayed by the Wizard's management path may carry a leading `0x01` (SOH) wizard-origin marker (WCB.ino:4317, 2931); WCB_Client does not strip it. Normal `;w` traffic does not carry it.

### A5. Example sketches (CLIENT/examples/)

BasicUsage (send/receive/status), SpecialPeer (id 20, NaviCore), NeighborDiscovery (setIdentity + onNeighbor), MaestroUnicast/Broadcast/Combined, DeliveryStats, SequenceInventory, MgmtRelay (temporary peer + WebSocket), AllFeatures (everything). Minimal client:

```cpp
#include <WCB_Client.h>
void onCmd(uint8_t sender, const char* cmd){ /* RX/WiFi task: enqueue only */ }
WCB_Client wcb(0xD3, 0x3B, "password", /*quantity*/3, /*id*/15, onCmd);
void setup(){ wcb.setMeshChannel(1); if(!wcb.begin()) for(;;); wcb.setIdentity("HoloLifter","1.0.0","revA","lift.motor"); }
void loop(){ wcb.update(); }
```

---

## B. Exact wire formats a client needs

All multi-byte integers little-endian (Xtensa/RISC-V native), all structs `__attribute__((packed))`. ESP-NOW broadcast MAC = `FF:oct2:oct3:FF:FF:FF`; unicast MAC of node N = `02:oct2:oct3:00:00:N`, N = 1..20 (`CLIENT/.cpp:1613-1631`; WCB.ino:8014-8030). Sizes below are what WCB dispatches on (WCB.ino:4020-4098); **receivers route by total length first**.

### B1. Main ETM struct, 252 B (`espnow_struct_message_etm`, WCB.ino:~205-212; `CLIENT/.h` `wcb_packet_etm_t`)

```
off  len  field
0    40   structPassword   NUL-terminated ASCII, compared with strncmp(...,39)
40   4    structSenderID   ASCII decimal e.g. "15\0"
44   4    structTargetID   ASCII decimal; "0"=broadcast; 97 raw serial; 98 Kyber; else unicast id
48   1    structCommandIncluded  0/1
49   200  structCommand    NUL-padded; text command (+ "|CRC%08X") or raw WDP TLV bytes
249  1    structPacketType
250  2    structSequenceNumber (uint16 LE)
```

Non-ETM 249 B variant (`espnow_struct_message`, no last 3 bytes): used ONLY for raw serial (97) and Kyber (98) and when ETM is off fleet-wide (`CLIENT/.cpp:581-690`; WCB.ino:4128-4170). A 252 B packet with target 97/98 is dropped.
**252 > 250 = ESP-NOW v1 limit.** The WCB packets only work because both ends are ESP-NOW v2 (IDF >= 5.4; ESP_NOW_MAX_DATA_LEN_V2). From Espressif's ESP32-C2 v5.4 docs: v1 max 250, v2 max 1490 (not 1470); "v2.0 devices can receive from both v1 and v2; v1 devices can only receive from v1"; a v2 packet larger than 250 to a v1 device is truncated or discarded. https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32c2/api-reference/network/esp_now.html . **The fan must be on IDF >= 5.4 (user plan: 5.4, OK) and the HoloLifter on Arduino-ESP32 3.x (IDF >= 5.1.. ; 3.3.4 = IDF 5.5)**; check `esp_now_get_version()`. The WCB source never mentions v2 explicitly; it simply relies on the default of IDF 5.5.

### B2. Packet types (WCB.ino:230-260; CLIENT/.h:49-53,222-225)

```
0 COMMAND   text command (target = recipient or 0)
1 ACK       unicast reply to a COMMAND; echoes seq; commandIncluded=0
2 HEARTBEAT broadcast, commandIncluded=0, seq=0
3 MGMT_FRAG / 5 MGMT_FRAG_UNICAST   (226 B struct, below)  [type 5 reused as CONFIG_REQ, 43 B]
4 MGMT_ACK, 6 CONFIG_FRAG, 7 STATS_REQ (also REMOTE_TERM=7 in RemoteTerm), 8 ETM_REQ, 9 STATS_FRAG, 10 ETM_FRAG
11 ETM_BOOT broadcast "I (re)booted", same 252 B layout as heartbeat
12 WDP      advert, same 252 B envelope, TLVs in structCommand
13-16 SEQ_REQ / SEQ_FRAG / SEQVAL_REQ / SEQVAL_FRAG
18/19 config-in-parts (WCB_Mgmt only); 20-24 OTA (below)
```

Other struct sizes in use on the mesh (must not collide with a custom struct): **43, 55, 59, 204, 226, 230, 243, 249, 252** (WCB.ino:4025-4060, WCB_RemoteTerm.h:40-61, WCB_OTA doc §4). Our private packets must use some other length (or ride in commands).

### B3. Heartbeat / boot announce / ACK contents

- HEARTBEAT: zeroed 252 B; password; sender=our id; target "0"; commandIncluded 0; type 2; seq 0. Broadcast to group MAC. WCB: every 9-11 s random (WCB.ino:1067-1085, `etmHeartbeatSec`=10); client: fixed 10 s (`CLIENT/.cpp:1835-1850`, `_heartbeatIntervalSec`).
- BOOT: same but type 11. WCB sends 3, ~1.2 s apart (WCB.ino:8115-8124); client sends 3 x 1.2 s from `update()` (1.17.1; `.cpp:1819-1833`, :210-214). Purpose: receivers clear that sender's duplicate-seq window (WCB.ino:4210-4220; client `.cpp` ETM_BOOT case) because a rebooted node restarts seq at 1.
- ACK: sender=us, target=original sender (ASCII), type 1, commandIncluded 0, **seq = echoed seq**, unicast to the sender's derived MAC (`.cpp:2183-2199`). Must be sent for every COMMAND addressed to us (or broadcast), **including duplicates and including packets whose CRC will be rejected** (WCB: etmSendAck fires before CRC check, WCB.ino:4268-4275; client: `.cpp` COMMAND case "Always ACK"). Exception: do NOT ACK a target-0 command whose text starts with `{` (WCB.ino:4256-4265). Peer must exist in ESP-NOW peer table before unicast (WCB re-adds on demand, WCB.ino:1470-1478; client flags above-floor senders and registers them transiently from update(), `.cpp:290-315`).

### B4. COMMAND, sequence, ETM retries

- Seq: uint16, post-increment from 0 at boot, **never 0 on the wire** (WCB firmware reads 0 as an "empty slot" in its dedupe ring, so a seq-0 command would be ACKed then discarded; `CLIENT/.cpp:2207-2225`). Wraps skipping 0. Heartbeat/ACK/boot/WDP use seq 0 / echo.
- Retransmits reuse the same seq. ETM: initial send (unicast or one broadcast), then every **500 ms** (`etmTimeoutMs`) a per-board unicast retry, **3 retries max**, give up when the board is offline (`CLIENT/.h:130-131`, `.cpp:240-300`; WCB.ino:1345-1440). Pending table size 10 both sides (WCB.ino:539, `.h:84`).
- Dedupe: receiver keeps recent seqs per sender: WCB last **8** (`ETM_SEQ_HISTORY`, WCB.ino:589), client high-water + 32-bit mask (`.cpp:879-899`). Seq 0 never deduped. Cleared by sender's BOOT packet; client clears once per 4 s hold (`WCB_BOOT_RESET_HOLD_MS`).
- ETM ON/OFF and CHKSM ON/OFF must match fleet-wide (below, H).

### B5. CRC rules

- Algorithm: CRC-32/ISO-HDLC (zlib): init 0xFFFFFFFF, reflected poly 0xEDB88320, final XOR (`CLIENT/.cpp:2930-2944`). Computed over the command bytes only (no password etc.).
- Suffix: `"|CRC"` + 8 **uppercase** hex digits appended to the command text inside structCommand: `<cmd>|CRC1A2B3C4D`. Receiver verifies the **last 12 chars** (client) / `lastIndexOf("|CRC")` (WCB, WCB.ino:4293-4311), rejects on missing/mismatch, then strips.
- Length: structCommand 200 incl NUL -> 199 chars; with CRC suffix **187 chars max** (`ETM_MAX_CMD_WITH_CRC`, WCB.ino:223; WCB refuses to send longer: WCB.ino:2571-2578). Longer unicast -> MGMT fragments.
- CRC is **not** applied to: heartbeat, ACK, boot, WDP (TLV bytes), raw (97/98), MGMT frags, OTA structs.

### B6. MGMT fragment struct, 226 B (`espnow_struct_mgmt`, WCB.ino:~842-851; CLIENT/.h:~190-203)

```
0 40 password | 40 1 packetType (3 or 5) | 41 1 targetWCB | 42 2 sessionId | 44 1 chunkIdx | 45 1 totalChunks | 46 180 payload
```

Chunk 179 chars + NUL, max 16 chunks (2864 chars); no per-chunk ACK; sent to the **group broadcast MAC**, target in-struct; sessionId never 0xFFFF; client sends 3 passes if ensured, 2 otherwise, 10 ms apart; receiver reassembles per session id, 15 s window, executes whole string (`CLIENT/.cpp:453-575`; WCB.ino:2900-3090). The client library also _reassembles inbound_ 226-byte frags addressed to it and delivers them to `onCommand` (`.cpp:2576`).

### B7. WDP advert (type 12) -- WDP_DESIGN.md:62-119; CLIENT/.cpp:1861-2005 (encoder), :2045-2180 (decoder); WCB_WDP.cpp:67-97, 495-640

structCommand layout (200 B): `'W'(0x57) | 0x01 | TLV* | 0x00 end`; each TLV `[type][len][value]`; unknown types skipped by length; envelope sender=our id, target "0", commandIncluded 1, seq 0, **no CRC**. Packet sent to group broadcast MAC.

```
0x00 END
0x01 ALIAS       string <=24            WCB only (board alias)
0x03 FWVER       string <=27            WCB + client
0x04 HWVER       u8                     WCB (1,21,23,24,31,32)
0x05 CAPFLAGS    u16 LE                 WCB bitmap: 0001 HCR,0002 MP3,0004 WLED,0008 Kyber-local,0010 Maestro-remote,0020 PWM,0040 Controller-link,0080 Maestro-host,0100 DFPlayer
0x06 MAESTRO     bytes (<=9 ids)        WCB/NaviCore
0x09 PORTLABEL   [port 1-5][label<=24]  one TLV per labelled port
0x0A CTRLID      u8                     WCB when linked to a controller
0x0B DEVTYPE     string <=24            **marks sender as a CLIENT; becomes its display name AND the ;w<alias> name**
0x0C HWREV       string <=15            client
0x0D CAPTAGS     string <=48            client, space-separated tags (e.g. "lift.motor")
0x0E MAESTRO_CFG [id][baudCode]*        WCB/NaviCore (baud table, CLIENT .cpp:1890-1893)
0x0F WLED_CFG    [id][baudCode]*        WCB
0x10 PWMTARGET   [wcb][port]*           WCB
0x11 SOLICIT     len 0                  "advertise now" (?WDP,POLL). MUST be checked BEFORE decode: otherwise it blanks the sender's record (WDP_DESIGN.md:79-87, revision log 2026-08-19). Receivers answer with a jittered advert (jitter (id%16)*40 ms client / board-number based WCB)
0x12 FLAGS       u8 bit0 = TEMPORARY    "adopt me live but never persist"
0x13 SEQHASH     u32 LE                 WCB FNV-1a of stored sequences
0x40-0xFE reserved
```

Minimal client advert (what `setIdentity` emits, `.cpp:1966-2005`): `57 01 | 0B len "HoloFan" | 03 len "1.0.0" | [0C hwRev] | [0D caps] | [flags 12 01 01 if temporary] | 00`. DEVTYPE is mandatory (empty type = no advert). Order: identity TLVs first, port labels last (trailing TLVs are dropped if the 200 B fills).
Cadence: client = 3 boot-burst adverts 1.3 s apart (first at +300 ms after `setIdentity`) then every **60 s** (+(id%16)*500 ms stagger); a TEMPORARY device every 15 s; on label/Maestro change a 2-advert burst (`.cpp:1912-2043`). WCB = 3 x boot burst (~1.6 s after boot), 60 s backstop, plus on-change (hash check twice a second, WCB_WDP.cpp:355-384). Neighbor staleness TTL 180 s (WDP_TTL_MS WCB_WDP.cpp:53; client `WCB_WDP_NEIGHBOR_TTL_MS` 180000, temporary 50 s `.h:109-113`).
WDP requires ETM on (WCB_WDP.cpp:356 `if (!wdpEnabled || !etmEnabled) return`).
Sanitising: receivers scrub `,` `]` and control chars in strings to `_` (WDP_DESIGN.md:118) -- **no commas/brackets in DEVTYPE**.

### B8. What a client MUST do and when (derived)

1. Before ESP-NOW init: STA mode, PS off, channel = mesh channel, **STA MAC = 02:oct2:oct3:00:00:id** (CLIENT/.cpp:101-157).
2. Receive gate (in order; all three are used by WCB and client): (a) source MAC octets 0-4 == `02,oct2,oct3,00,00` (WCB.ino:4019-4021 + client `.cpp:2655-2670`); (b) size routing (252 -> ETM, else raw hooks); (c) password match; (d) claimed sender id (1-20) == src MAC last octet (spoof guard; WCB.ino:4189-4200); (e) target is 0 or us.
3. COMMAND -> ACK immediately, verify/strip CRC, dedupe by (sender,seq), deliver.
4. Heartbeat every ~10 s, 3 boot announces on every join, WDP advert burst on join + every 60 s, answer SOLICIT with an advert within ~0.6 s.
5. Track peers' heartbeats if it wants `isOnline` (not required for being reachable).
6. To be _reachable inbound_ WCBs must have the client in their peer set: either id <= `?WCBQ` (floor) or they auto-join after hearing **2 WDP adverts** (WCB_WDP.cpp:675-690; client `.cpp:2160-2170`). Learned peers persist in WCB NVS.
7. To be _online_ in WCB's eyes the client must heartbeat: WCB marks every valid ETM packet from an active peer online (WCB.ino:4204-4222), flips offline after silence (C).

---

## C. Addressing / IDs

- Node id 1-20 (`MAX_WCB_COUNT 20`, WCB.ino:119-120; ESP-NOW peer table ~20, wiki ESP-NOW-Communication.md:665). **WCBs and all clients share the same 1..20 number space and each id = one derived MAC**: duplicate id = identical MAC = chaos (`CLIENT/README` "WiFi MAC is overwritten"). There is no central assigner; ids are chosen manually. Total mesh capacity is therefore 20 nodes incl. WCBs, NaviCore/controller, HoloLifter, fan, relay. The group broadcast peer also consumes one ESP-NOW peer slot on each node.
- Floor vs learned: `?WCBQ` = number of pre-registered peers 1..N ("floor, not ceiling", WDP_DESIGN.md:182). A client with id > WCBQ and not 20 is allowed (begin() only warns, `.cpp:44-48`) but WCBs can reach it only after auto-join (2 adverts). Id **20** = "special/controller" slot: requires `?SPECIAL,ON` / `?CONTROLLER,ON,<id>` (WCB.ino:5394-5440), is never auto-joined, and WCBs track it separately. **WCBs auto-adopt as controller any client whose DEVTYPE equals "NaviCore", "Sabé" or "Sabe"** (WCB_WDP.cpp:405-412, 625-631) -- never use those names.
- Recommendation: HoloLifter = 15, fan = 16 (or any free 1-19 above real WCB count), WCBQ left as is, auto-join ON (default). Avoid 20 unless intentionally a controller.
- Names: the WDP `DEVTYPE` is the alias used by `;w<name>,cmd` on the WCB (`wdpResolveAlias`, WCB_WDP.cpp:770-782; processWCBMessage alias branch WCB.ino:6540-6572). Rules: case-insensitive exact match; must not start with a digit; ambiguous (2 nodes with same name, or a WCB alias equal to it) -> WCB refuses ("ambiguous, use the number"); <=24 chars; avoid spaces/commas (the token ends at the first comma). Suggested: `HoloLifter`, `HoloFan` (the shared vocabulary in `WCB/Wizard/device-labels.js` is advisory; custom names accepted as-is: WDP_DEVICE_ANNOUNCE.md "Device names").
- A client can be addressed three ways from a controller: `;w16,payload`, `;wHoloFan,payload`, or broadcast `;w0`/plain broadcast.
- WCB-to-client commands arrive with `senderID` = WCB number that executed the `;w`. The original droid-controller serial port and identity are lost. Replies: see A4. The WCB also keeps a per-port "WDP-DA" serial announce model (`@WDP1 {json}` lines every 25-30 s, WDP_DEVICE_ANNOUNCE.md) for devices wired to a WCB serial port -- an alternative way to appear in the mesh view, but only as a port label, not an addressable node.
- Alias mapping to fleet size: 20 slots; learned peers are permanent in NVS (`?WDP,FORGET,<id>`, `?WDP,CLEAR`).

---

## D. Channel / mode rules; leaving and rejoining

- One radio, one channel. Default **channel 1** (`WCB_MESH_CHANNEL_DEFAULT`, WCB_Storage.h:25), range 1-11 only (12/13 rejected, WCB_Storage.cpp:466-468; client `setMeshChannel` `.cpp:1536-1541`). Peers are registered with `channel=0` ("current channel"). Changing it: `?WCBCH,<n>` is persisted and applied **on reboot only** (WCB_Storage.cpp:473-490) and must be done on every node; a node with the wrong channel is silently deaf and cannot discover the right one (`CLIENT/.cpp:1555-1560`).
- WCB runs `WiFi.mode(WIFI_STA)`, `esp_wifi_set_ps(WIFI_PS_NONE)` (needed: modem sleep drops MAC ACKs and makes a board look offline, WCB.ino:7985-8010), `esp_wifi_set_channel(meshChannel, WIFI_SECOND_CHAN_NONE)`, `esp_wifi_set_mac`, **no encryption** (`encrypt=false`, no PMK/LMK), default TX power.
- WCB_Client README "Limitations & Coexistence": `begin()` does `WiFi.disconnect()` and drops any STA association; associating with an external router moves the radio to the AP's channel => mesh blackout unless the AP is on the same channel; SoftAP before begin() is supported (WIFI_AP_STA, AP owns channel; NaviCore does this, NAVI/NaviCore.ino:4884-4999); only one ESP-NOW stack/instance; MAC override breaks MAC-based identity; set `WIFI_PS_NONE`.
- **Fan leaves mesh to join a WiFi network, then returns (no support in WCB_Client; our code):** the library never does this. What the WCB side does while the fan is gone:
  - ETM offline after `(etmHeartbeatSec+1) * etmMissedHeartbeats` = 11 x 5 = **55 s** with defaults (WCB.ino:1110, 282); client library uses 10 x 5 = 50 s (`.h` `_missedBeforeOffline`). TestConfigs set `?ETM,MISS,3` (=33 s) so real fleets differ (TestConfigs/WCB1_01_base.txt:64).
  - `membership never expires`: the fan stays a learned peer in each WCB's NVS; only `boardTable[].online` flips (WDP_DESIGN.md:167-172). WDP neighbor info goes stale at 180 s but the record stays.
  - Unicast ETM to an offline peer: WCB does not expect an ACK (`if (!boardTable[b].online) continue;`, WCB.ino:1262) => **single best-effort frame, no retries**; in-flight ensured commands to a board that goes offline are cancelled (WCB.ino:1355-1362). So commands sent to the fan during upload are lost silently. Design implication: either have the controller see it is busy (fan can broadcast nothing...) or gate on `wdpResolve`/online state in the controller.
  - Learned (client) peers that never ACK are excluded from broadcast completion ("wcbPeerReciprocated", WCB.ino:1280-1283), so a silent fan never stalls broadcasts.
  - Return: any valid ETM packet from an active peer re-marks it online and prints `came ONLINE` (WCB.ino:4204-4222); a BOOT packet forces the "came online" edge and **clears that sender's dedupe window**. Seq counter: if the fan did not reboot its seq continues (WCB window 8, fine); if it rebooted, seq restarts at 1 and the 3 BOOT announces clear the window (WCB.ino:4217-4220). The client library documents the failure if announces are lost: commands ACKed-then-dropped as duplicates (README 1.17.1).
  - Recommended return sequence for the C2 firmware: disconnect STA (`esp_wifi_disconnect`), set channel to mesh channel (do not call while connecting; IDF doc), make sure STA MAC is still the derived one (`esp_wifi_set_mac` only works when the interface is stopped, and the bit-0 rule: 0x02 ok; IDF doc https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32c2/api-reference/network/esp_wifi.html -- persistence of the MAC across stop/start is not documented, so re-apply it), re-init ESP-NOW (`esp_now_deinit/init`), re-register WCB peers + broadcast peer (they are keyed on MAC, hence safe to re-add), then 3 x BOOT (1.2 s apart), heartbeat immediately, 3 x WDP burst. Optionally advertise TEMPORARY flag? NO - temporary peers are evicted after ~50 s of silence by WCBs (WDP_DESIGN.md:173-181), which would be desirable only if the fan is often away; the downside is no persistence. Recommend permanent (default).
  - While associated to an external AP the fan is unreachable on the mesh AND will not hear WCB traffic; WCBs mark it offline. A "leaving" notice: no protocol exists (no goodbye packet). You can emit a private ordinary command (`;w<ctrl>,#HL:UPLOAD`) to inform the controller before leaving.
  - If the mesh channel differs from the home router channel, a channel switch occurs on association; you must switch back. ESP32-C2 specifics: ESP-NOW is supported on C2 and "sleep supported only in station mode" (IDF docs); C2 has one radio, same one-channel limit.

---

## E. OTA (WCB/docs/WCB_OTA_TECHNICAL.md)

- Mechanism: standard IDF `esp_ota_ops` to the **inactive** slot; verify (magic + SHA-256 via `esp_ota_end`) **before** `esp_ota_set_boot_partition`; reboot. Failure/timeout/power loss -> `esp_ota_abort`, boot pointer unchanged (doc :32-90). Session timeout 30 s idle.
- Transports (same core `otaBegin/otaWrite/otaEnd/otaAbortSession`): (1) USB serial `?OTALOCAL,BEGIN|DATA|END|ABORT|BAUD` base64, <=1024 B chunks, ACK-paced (`[OTA:ACK,<cursor>]`); (2) **ESP-NOW relay**: browser -> USB -> relay WCB -> unicast to the target, via `?OTA,*`. Packet types OTA_BEGIN=20, DATA=21, ACK=22, END=23, ABORT=24 (doc :140). Two packed structs, locked sizes: **55 B control** `{pw[40], type, targetWCB, sourceWCB, chipFamily, status, u16 sessionId, u32 imageSize, u32 ackedOffset}` and **243 B data** `{pw[40], type, targetWCB, sourceWCB, u16 sessionId, u16 dataLen, u32 fragOffset, u8 data[192]}` (doc :142-167). Strict in-order cursor; target always ACKs its current contiguous cursor; sender rewinds on stall; `OTA_ST_ERR` once a session is torn down.
- Rule that matters: **never run flash ops in the ESP-NOW rx callback**; enqueue (FreeRTOS queue of 12 x 244 B) and drain in loop (doc §5). Chip-family guard byte: 0 = ESP32, 1 = S3, 0xFF unsupported (doc :61) -- C2 has no code; the Wizard (GPL) would need a new value if you want to flash the fan through the WCB relay.
- Partition layout (doc §1): `nvs 0x9000/0x5000`, `otadata 0xE000/0x2000`, `ota_0 0x10000/0x1E0000`, `ota_1 0x1F0000/~1.9MB` (WCB `min_spiffs` scheme; NaviCore adds `spiffs 0x3D0000/0x20000`, `coredump 0x3F0000/0x10000`, `clips`, 16 MB flash, NAVI/partitions.csv). Needs a 2-OTA-slot table. For a 2 MB C2 module this means ~1 MB slots.
- Rollback: WCB build does **not** enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (Arduino default). If you do enable it (recommended for the fan), the new image must call `esp_ota_mark_app_valid_cancel_rollback()` early or the bootloader rolls back on next reset (doc :271; IDF doc: states PENDING_VERIFY/VALID/INVALID/ABORTED, `esp_ota_mark_app_invalid_rollback_and_reboot()`; https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32c2/api-reference/system/ota.html).
- otadata coexistence (doc §6): every flash path must set the boot slot (IDE writes boot_app0 to 0xE000; esptool app-only flashes must erase 0xE000).
- Reusable for the fan: the lifecycle (begin/write/end/abort), in-order-cursor reliability model, defer-out-of-callback pattern, brick guards (chip-family, image size <= slot, ACK-the-cursor), CRC-over-offset trick (`<offset>:<crc32>` suffix) for serial hop. Since the fan already joins WiFi for video upload, the simplest fan OTA is HTTPS/HTTP (`esp_https_ota`) from the same WiFi session; mesh OTA is optional. HoloLifter on Arduino: `Update.h`/`esp_ota_ops` same.

---

## F. Security / limits

- **ESP-NOW is unencrypted** (`peer.encrypt=false`, no PMK; WCB.ino:8059, CLIENT `.cpp:1655`). The "password" is a 39-char (`char[40]`) **plaintext** field inside every packet (`?EPASS` rejects >= 40 chars, WCB.ino:5290-5296; stored in NVS plaintext `espnow_config/password`). It is compared with plain strcmp/strncmp. Anyone in radio range can sniff it from any heartbeat. Default is `"change_me_or_risk_takeover"` (WCB.ino:149); the code comment says "lower case characters only" but it is not enforced.
- Gates (WDP_DESIGN.md:239-256): MAC-group octets, password, sender-id-range/not-self, sender-MAC-binding (id == MAC[5]); auto-join needs 2 adverts; a learned peer carries membership only (MAC derived locally). Not an authentication system: a rogue with the password can impersonate any id that matches its own MAC (it can set any MAC), send any `?` config command (including `?EPASS`, `?WCBCH`, OTA relay) to any WCB. Treat the mesh as a hobby-grade trust domain. WDP adverts are cleartext by design.
- Limits to design around: command <= **187 chars with CRC** (199 without); unicast >187 auto-fragments up to 2864 chars (client->WCB direction in library; WCB refuses to _send_ >187 over plain ETM, WCB.ino:2571; Wizard relay fragments); raw serial <= 177 B; Kyber <= 178 B; payload cannot contain the WCB chain delimiter `^` (splitter runs before routing, WCB.ino ~6585 comment) nor start with `;`/`?`/`{`/`@` unless intended; WCB scrubs `,` in names.
- **Sending WiFi credentials over the mesh**: SSID <= 32 B and WPA2 passphrase <= 63 B fit in 187 chars, so size is not the issue; secrecy is: the credentials would be broadcast/unicast in clear (ESP-NOW unencrypted; WCB also logs received commands when debug is on, and ETM statistics/command text appear in `?DEBUG` output). Avoid `^`, `;`, `?`, `{` in the payload (use hex/base64 of the fields), and note the serial line `;wHoloFan,...` passes through the controller's serial line and a WCB console. Recommendations: (1) provision WiFi creds on the fan itself (USB/NVS/its own AP page) and send only `join profile` / `upload now` commands over the mesh; (2) if creds must travel, wrap them in app-layer authenticated encryption (AES-GCM, key not equal to the mesh password; ESP32-C2 has hardware AES) and base64 them; (3) enable ESP-NOW encryption (PMK/LMK) is not possible with WCB peers (they use encrypt=false, so a fan unicasting encrypted to a WCB would not be received).

---

## G. Licensing

- WCB firmware repo (`WCB/`): **GPL-3.0** (WCB/LICENSE; GitHub API spdx GPL-3.0). That covers Code/WCB/_, docs/_.md, Wizard/*, tests/wdp_wire_test.cpp, TestConfigs. Copying firmware code, the Wizard, doc prose or the test file into our project would make our combined work GPLv3-obligated. Do not copy from `WCB/`.
- **WCB_Client: MIT**, WcbCmd: MIT, NaviCore: MIT (all "Copyright (c) 2026 Greg Hulette"). MIT permits porting/modifying/relicensing-under-MIT as long as the copyright + permission notice are kept in copies or substantial portions. Port from `CLIENT/src` (MIT), not from `WCB.ino`.
- Wire protocol facts (field layouts, constants, algorithm = standard CRC-32) are interoperability facts, not copyrightable expression; implementing them independently (clean-room from this document / the MIT client) is fine. Quote WDP_DESIGN.md prose only briefly (GPL-licensed docs); re-describe in our own words. The WDP wire test (GPL) can be re-created: derive vectors from the MIT client or from capturing our own packets.
- I am not a lawyer; verify the NOTICE requirement in a port: put "Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT" in the file header and licence text in `THIRD_PARTY`.

---

## H. Gotchas, compat rules, real configurations

Compat rules (all fleet-wide):

- **ETM on/off must match on every board**; ETM-sized packets are dropped by an ETM-off board and vice versa (except raw/PWM/Maestro/`{` JSON which are let through), WCB.ino:4101-4140. **WDP needs ETM** (WCB_WDP.cpp:356).
- **CHKSM (`?ETM,CHKSM`) must match**, otherwise packets are ACKed then silently discarded (the sender thinks delivery succeeded) (WCB.ino:4268-4311, CLIENT README "Checksum Setting"). `setChecksum(false)` only if all WCBs are OFF.
- Same MAC octets (`?MAC,2,xx`, `?MAC,3,xx`), same password, same channel; WCB firmware >= 6.0 required by client README; WDP/auto-join features need >= 6.x with WDP; MGMT_FRAG_UNICAST semantics need 6.1.1+ (`CLIENT/.h:181-188`); current WCB 6.2.1_021242RSEP2026 (WCB.ino:180).
- Timing constants are "fleet-wide agreements" (CLIENT/CLAUDE.md rule 4): heartbeat 10 s, offline = 5 missed, retry 500 ms x3, pending 10.
- Callbacks run on the WiFi task; no flash/NVS/blocking/printf-heavy work in them (CLIENT/CLAUDE.md rule 2; WCB CLAUDE.md rule 11: a multi-KB Serial.printf in the callback fired the WiFi watchdog). Also **`esp_now_add_peer` + NVS writes must not run in the rx callback** (boot-loop hazard, `.cpp:2102-2112`): flag and do it in the loop task.
- WCB `ESP.restart()` / `?`-commands from clients: a client can run any `?` command on a WCB; a config push is a stream and restarts are deferred.
- Command semantics: WCB `;` prefix commands and `?` config commands share the namespace the client's payload may be parsed in only when a WCB is the _receiver_. When the receiver is our client, the payload is opaque text.
- WCB docs/wiki disagreements to correct: wiki and CLIENT README say `?WCBM` / `?WCBP` query the octets/password; **those commands do not exist in the firmware** (`grep` of WCB.ino/WCB_Help finds none). Real ones: `?MAC,2,<hex>`, `?MAC,3,<hex>`, `?EPASS,<pw>`, `?WCB,<n>`, `?WCBQ,<n>`, `?WCBCH,<n>`, `?config`, `?WDP,LIST|DUMP|POLL|FORGET,<id>|CLEAR`, `?CONTROLLER,ON,<id>` (WCB.ino:5259-5440; WCB_Help.cpp). Wiki says "Max packet 250 bytes" (Network-Design.md:204), but ETM frames are 252 (needs v2). Wiki library name `WCBClient` vs real `WCB_Client`. `?ETM,MISS` default is 5 in code (WCB.ino:282) but TestConfigs use 3.
- Client-side bug classes recorded in changelog worth reusing as test cases: seq 0 ghost-drop, duplicate window after reboot (boot announce), solicit blanking neighbor rows, id-spoof binding, ACK before CRC, `{` best-effort broadcasts, channel mismatch silent.
- Test configs (WCB/TestConfigs): three-board lab: `?WCB,n ?WCBQ,3 ?MAC,2,D3 ?MAC,3,3B ?EPASS,<password> ?ETM,ON ?ETM,HB,10 ?ETM,MISS,3 ?ETM,TIMEOUT,500 ?ETM,CHKSM,ON` (TestConfigs/WCB1_01_base.txt:64); bench expectations in TestPlan.md/steps.md; they contain no client examples. Useful as realistic values for our harness (oct2=0xD3, oct3=0x3B; example in the CLIENT docs uses 5C/57).
- The WCB-hosted automated test (`WCB/tests/wdp_wire_test.cpp`) re-expresses TLV encode/decode, election, auto-join vetting, sender-MAC binding, SOLICIT; GPL, host-side `g++ -std=c++11` -- useful as a checklist of invariants, not as code to copy.
- Bring-up checklist for a new client: (1) correct oct2/oct3/password/channel; (2) check `?WDP,LIST` on a WCB shows our node after ~5 s; (3) `;w<id>,test` from a WCB and see callback; (4) check `?STATS` / `?ETM,CHAR`; (5) power-cycle the node and confirm no ghost drops; (6) unplug for >1 min and confirm `went OFFLINE` / `came ONLINE (boot)` lines.

---

## I. Recommendation

1. **HoloLifter (ESP32-WROOM, Arduino): use WCB_Client as-is, pinned to a tag/commit (vendor the MIT lib into `lib/` rather than tracking master; the author pushes breaking-ish behaviour changes frequently, 1.13 -> 1.17 in weeks).** Preconditions:
   - Toolchain: HoloLifter's `platformio.ini` uses stock `platform = espressif32` (Arduino-ESP32 2.x/IDF 4.4). WCB_Client needs Arduino-ESP32 3.x (IDF 5.x signature, ESP-NOW v2 for 252 B frames). PlatformIO has no official Arduino 3.x support; use the community **pioarduino** platform (`platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip`; its releases track Arduino 3.3.x/IDF 5.5, https://github.com/pioarduino/platform-espressif32 ). This is a build-system migration for the HoloLifter that must be tested with SCMD/ESP32Encoder/IRremote/ArduinoJson first.
   - Radio conflict: HoloLifter's current Phase-6 design joins the holofan's WiFi AP in STA mode (`lib/HolofanWiFi`); `WCB_Client::begin()` disconnects STA and the mesh needs a fixed channel -- the two cannot coexist. The new architecture (fan is a mesh node) removes the need; delete the STA join. Keep `lib/HolofanProtocol` as the codec (commands now ride mesh text commands instead of HTTP).
   - Use `setIdentity("HoloLifter", fw, rev, "lift.motor")`, id 15, non-20; `onCommand` -> enqueue into a ring/queue and parse `#HL...` in loop (keeps ESP32Encoder/limit-switch loop responsive and flash/NVS out of the callback). Replies via `wcb.send(senderId, ";S<port>,#HL:...")` as in A4, port from NVS. Do not use `Serial`/GPIO1/3 prints in callbacks. Safety: motor stop must not depend on mesh liveness (emergency stop locally).
   - Cost: adds ~Arduino WiFi/ESP-NOW footprint (library has no external deps; RAM small: peer stats 400 B, neighbors 20 x ~300 B).
2. **Fan (ESP32-C2, ESP-IDF 5.4): do NOT try to use WCB_Client as-is.** It is Arduino-bound (WiFi.h/Preferences/Serial/String/millis), 2952 lines of which only ~600 are needed. **Port the core into a portable, dependency-free C/C++ protocol library** (pure functions: struct pack/unpack, CRC32, WDP TLV encode/decode, dedupe window, ACK/heartbeat/boot builders, ETM retry state machine driven by injected `millis` + `send(mac,buf,len)` interface), plus a thin ESP-IDF adapter (esp_wifi/esp_now/NVS/FreeRTOS). Benefits: unit-testable on the host in the HoloLifter's existing PlatformIO `native` Unity env (also usable by HoloLifter later), no GPL contamination (derive from MIT client; keep notice), and one codebase for both nodes if the HoloLifter later wants to drop the Arduino dependency. Port scope (minimum viable "first-class" client): MAC derive + set; peer add; receive gate (MAC, size, password, id==MAC[5], target); COMMAND ACK/CRC/dedupe/deliver; heartbeat 10 s; 3 boot announces; WDP advert 3 burst + 60 s + SOLICIT reply; send() with ETM retries (needed for replies, optional); inbound MGMT frag reassembly (only if you expect >187 char commands, e.g. WiFi profile; otherwise skip); leave/join state machine for upload mode (D). Skip: Maestro/Kyber/bulk/sequences/stats/neighbor decoding/auto-join (the fan only needs to be addressed; it need not learn others, but keep the WCB list it must reach = the WCB that fronts the controller).
   - Make `ESP-NOW v2` explicit: assert `esp_now_get_version()==2` at boot; log if a 252 B send returns `ESP_ERR_ESPNOW_ARG`.
   - Reachability: choose id <= `?WCBQ` or accept that the WCBs learn it from the first 2 adverts (3-advert boot burst makes that automatic within ~3 s).
   - Reply route: the fan must know which WCB to answer (use `senderID` from the command callback) and which serial port (configured).
3. **Do not fork WCB firmware code.** If a feature needs WCB cooperation (e.g. an "away" state, name resolution), prefer conventions in our payload and keep the mesh protocol vanilla; wire format changes would have to land in both GPL firmware and Greg's client (CLIENT/CLAUDE.md: "New TLVs must be added on both sides").
4. Open questions/need hardware verification: (a) exact payload seen by `onCommand` for `;wHoloFan,#HLU\r` (trailing `\r`, SOH, spaces); (b) whether `esp_wifi_set_mac` ordering on C2 (set after `esp_wifi_set_mode` and before `esp_wifi_start`) holds and persists across stop/start; (c) C2 ESP-NOW peer-table size and v2 packet 252 B TX/RX interop with S3/ESP32 WCBs; (d) WCB offline-marking times with the fleet's actual `?ETM,MISS`; (e) alias ambiguity if two clients share a name; (f) whether a long (>187 B) WCB->client command is ever generated (WCB refuses >187 under CHKSM).
