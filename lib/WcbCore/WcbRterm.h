// WCB remote terminal (RTERM) line packet: 204 bytes, packet type 7. A WCB
// prints a received one as "[TERM:<sourceId>]<text>" on its USB console.
// Layout is a wire fact shared by the WCB firmware and NaviCore (MIT).
#ifndef WCB_RTERM_H
#define WCB_RTERM_H
#include <stddef.h>
#include <stdint.h>

namespace wcb {

constexpr size_t RTERM_TEXT_MAX = 160;

#pragma pack(push, 1)
struct RtermPacket {
  char    password[40];
  uint8_t packetType;   // PKT_RTERM
  uint8_t sourceId;     // this node
  uint8_t destId;       // the WCB that should print it
  uint8_t textLen;      // <= RTERM_TEXT_MAX
  char    text[160];
};
#pragma pack(pop)
static_assert(sizeof(RtermPacket) == 204, "RTERM packet must be 204 bytes");

// Fill `p` with up to RTERM_TEXT_MAX chars of `text`. Returns chars packed.
size_t buildRterm(RtermPacket& p, const char* password, uint8_t source, uint8_t dest,
                  const char* text, size_t len);

}  // namespace wcb
#endif
