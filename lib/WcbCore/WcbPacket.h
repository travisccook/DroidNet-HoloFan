// WCB mesh wire format: ETM packet layout, packet types, node MAC scheme.
// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License;
// see THIRD_PARTY_NOTICES.md. Field facts: docs/WCB_MESH_NOTES.md section B.
// Pure C++: no Arduino / ESP-IDF headers (builds in the native test env).
#ifndef WCB_PACKET_H
#define WCB_PACKET_H
#include <stddef.h>
#include <stdint.h>

namespace wcb {

constexpr uint8_t PKT_COMMAND   = 0;
constexpr uint8_t PKT_ACK       = 1;
constexpr uint8_t PKT_HEARTBEAT = 2;
constexpr uint8_t PKT_RTERM     = 7;
constexpr uint8_t PKT_BOOT      = 11;
constexpr uint8_t PKT_WDP       = 12;

constexpr uint8_t MAX_ID           = 20;  // node ids are 1..20 (WCBs and clients share them)
constexpr uint8_t TARGET_BROADCAST = 0;

constexpr size_t PASSWORD_FIELD   = 40;                               // 39 chars + NUL
constexpr size_t COMMAND_FIELD    = 200;                              // 199 chars + NUL
constexpr size_t CRC_SUFFIX_LEN   = 12;                               // "|CRC" + 8 hex digits
constexpr size_t MAX_CMD_NO_CRC   = COMMAND_FIELD - 1;                // 199
constexpr size_t MAX_CMD_WITH_CRC = MAX_CMD_NO_CRC - CRC_SUFFIX_LEN;  // 187

// Fleet-wide timing agreements (WCB firmware and WCB_Client defaults).
constexpr uint32_t HEARTBEAT_MS       = 10000;
constexpr uint32_t OFFLINE_MS         = 50000;  // 5 missed heartbeats
constexpr uint32_t RETRY_MS           = 500;
constexpr uint8_t  MAX_RETRIES        = 3;
constexpr uint32_t BEST_EFFORT_FREE_MS = 1000;
constexpr size_t   PENDING_MAX        = 10;
constexpr uint8_t  BOOT_ANNOUNCES     = 3;
constexpr uint32_t BOOT_GAP_MS        = 1200;
constexpr uint32_t BOOT_RESET_HOLD_MS = 4000;
constexpr uint8_t  WDP_BURST          = 3;
constexpr uint32_t WDP_FIRST_MS       = 300;
constexpr uint32_t WDP_GAP_MS         = 1300;
constexpr uint32_t WDP_PERIOD_MS      = 60000;

// The 252-byte ETM packet. Multi-byte fields are little-endian on every
// target we build for (ESP32 Xtensa, ESP32-C2 RISC-V, x86/arm64 hosts).
#pragma pack(push, 1)
struct EtmPacket {
  char     password[40];    // NUL-terminated, compared over 39 chars
  char     senderId[4];     // ASCII decimal, e.g. "15"
  char     targetId[4];     // ASCII decimal, "0" = broadcast
  uint8_t  commandIncluded; // 0/1
  char     command[200];    // text (+ "|CRC%08X") or WDP TLV bytes
  uint8_t  packetType;      // PKT_*
  uint16_t seq;             // never 0 on a COMMAND
};
#pragma pack(pop)
static_assert(sizeof(EtmPacket) == 252, "ETM packet must be 252 bytes");

// Unicast MAC of node `id`: 02:oct2:oct3:00:00:id.
void nodeMac(uint8_t oct2, uint8_t oct3, uint8_t id, uint8_t out[6]);
// Group broadcast MAC: FF:oct2:oct3:FF:FF:FF.
void groupMac(uint8_t oct2, uint8_t oct3, uint8_t out[6]);
// True if `mac` is a node MAC of this mesh (first five octets match).
bool isMeshMac(const uint8_t mac[6], uint8_t oct2, uint8_t oct3);
// Zero `p` and fill the envelope. commandIncluded is left 0.
void initEtm(EtmPacket& p, const char* password, uint8_t sender, uint8_t target,
             uint8_t type, uint16_t seq);
// atoi() of a 4-byte ASCII id field that may lack a NUL.
int parseId(const char field[4]);

}  // namespace wcb
#endif
