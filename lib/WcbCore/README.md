# WcbCore

Portable C++17 client for the WCB ESP-NOW mesh, shared by the HoloLifter
(Arduino, lib/WcbArduino) and the HoloFan (ESP-IDF, firmware/holofan). No
Arduino, ESP-IDF or FreeRTOS headers; unit-tested in the PlatformIO `native`
env (`pio test -e native -f "test_wcb_*"`).

Ported from WCB_Client 1.17.2 (MIT, see THIRD_PARTY_NOTICES.md). Wire facts:
docs/WCB_MESH_NOTES.md.

Shared by DroidNet-HoloLifter and DroidNet-HoloFan: change both copies together
and keep them identical.

## Units

    WcbPacket     252 B ETM layout, packet types, node/group MACs, constants
    WcbCrc        CRC-32 and the "|CRC%08X" suffix
    WcbSeqWindow  per-sender anti-replay window
    WcbWdp        our WDP advert; SOLICIT detection
    WcbRterm      204 B remote-terminal line packet
    WcbRing       lock-free SPSC ring: radio task -> poll task
    WcbPeers      online tracking, peer registration, dedupe + BOOT reset
    WcbSendQueue  ETM retries for our sends
    WcbSchedule   BOOT / heartbeat / WDP timers
    WcbPlatform   the interface an adapter implements
    WcbNode       ties it together

## Threading contract

- `WcbNode::ingest()` may run on any task (the ESP-NOW receive callback). It
  only copies the frame into the ring (and runs the optional peek hook).
- Everything else, including every `WcbPlatform` call, runs on ONE owner
  task: Arduino `loop()` on the HoloLifter, the mesh task on the fan.
- So `esp_now_add_peer` and `esp_now_send` never run in the receive callback.
- `setPeekHook()` only works before the first `begin()` (ingest reads it
  without a lock); it returns false afterwards.
- A command that arrives while the app's command queue is full is not ACKed
  and not recorded, so the sender's ETM retry redelivers it.

## Differences from WCB_Client (deliberate)

- ACKs are sent from `poll()`, not from the receive callback; poll at 10 ms or
  faster so ACKs land well inside the sender's 500 ms retry window.
- Every node id is swept for offline (not only floor/learned peers).
- A sender outside 1..20, or claiming our own id, is rejected.
- An ACK frees a pending send only if it comes from that send's target.
- Not ported: broadcast send, fragmentation (>187 chars), Maestro/Kyber/raw
  serial, bulk transfer, stored sequences, neighbour decoding, learned peers
  in NVS, auto-join, delivery statistics, the Wizard management relay.
