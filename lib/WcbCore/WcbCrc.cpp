// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#include "WcbCrc.h"
#include "WcbPacket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace wcb {

uint32_t crc32(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

bool appendCrc(const char* cmd, char* out, size_t outSize) {
  size_t len = strlen(cmd);
  if (len + CRC_SUFFIX_LEN + 1 > outSize) return false;
  snprintf(out, outSize, "%s|CRC%08X", cmd, (unsigned)crc32(cmd, len));
  return true;
}

static bool isHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

CrcCheck verifyStripCrc(char* cmd) {
  size_t len = strlen(cmd);
  if (len < CRC_SUFFIX_LEN) return CrcCheck::MISSING;
  char* tag = cmd + len - CRC_SUFFIX_LEN;
  if (strncmp(tag, "|CRC", 4) != 0) return CrcCheck::MISSING;
  for (int i = 4; i < 12; i++)
    if (!isHex(tag[i])) return CrcCheck::MISSING;
  uint32_t rx = (uint32_t)strtoul(tag + 4, nullptr, 16);
  if (rx != crc32(cmd, (size_t)(tag - cmd))) return CrcCheck::MISMATCH;
  *tag = '\0';
  return CrcCheck::OK;
}

}  // namespace wcb
