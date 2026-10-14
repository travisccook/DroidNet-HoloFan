// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#include "WcbPacket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace wcb {

void nodeMac(uint8_t oct2, uint8_t oct3, uint8_t id, uint8_t out[6]) {
  out[0] = 0x02; out[1] = oct2; out[2] = oct3; out[3] = 0x00; out[4] = 0x00; out[5] = id;
}

void groupMac(uint8_t oct2, uint8_t oct3, uint8_t out[6]) {
  out[0] = 0xFF; out[1] = oct2; out[2] = oct3; out[3] = 0xFF; out[4] = 0xFF; out[5] = 0xFF;
}

bool isMeshMac(const uint8_t mac[6], uint8_t oct2, uint8_t oct3) {
  return mac[0] == 0x02 && mac[1] == oct2 && mac[2] == oct3 && mac[3] == 0x00 && mac[4] == 0x00;
}

void initEtm(EtmPacket& p, const char* password, uint8_t sender, uint8_t target,
             uint8_t type, uint16_t seq) {
  memset(&p, 0, sizeof(p));
  strncpy(p.password, password, sizeof(p.password) - 1);
  snprintf(p.senderId, sizeof(p.senderId), "%u", (unsigned)sender);
  snprintf(p.targetId, sizeof(p.targetId), "%u", (unsigned)target);
  p.packetType = type;
  p.seq = seq;
}

int parseId(const char field[4]) {
  char buf[5];
  memcpy(buf, field, 4);
  buf[4] = '\0';
  return atoi(buf);
}

}  // namespace wcb
