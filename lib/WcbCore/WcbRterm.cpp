#include "WcbRterm.h"
#include "WcbPacket.h"
#include <string.h>

namespace wcb {

size_t buildRterm(RtermPacket& p, const char* password, uint8_t source, uint8_t dest,
                  const char* text, size_t len) {
  memset(&p, 0, sizeof(p));
  strncpy(p.password, password, sizeof(p.password) - 1);
  p.packetType = PKT_RTERM;
  p.sourceId = source;
  p.destId = dest;
  size_t n = len > RTERM_TEXT_MAX ? RTERM_TEXT_MAX : len;
  memcpy(p.text, text, n);
  p.textLen = (uint8_t)n;
  return n;
}

}  // namespace wcb
