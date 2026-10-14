// CRC-32/ISO-HDLC (zlib) and the WCB "|CRC%08X" command suffix.
// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#ifndef WCB_CRC_H
#define WCB_CRC_H
#include <stddef.h>
#include <stdint.h>

namespace wcb {

// init 0xFFFFFFFF, reflected poly 0xEDB88320, final complement.
uint32_t crc32(const void* data, size_t len);

// Write "<cmd>|CRC%08X" (uppercase hex) into out. False if it does not fit.
bool appendCrc(const char* cmd, char* out, size_t outSize);

enum class CrcCheck { OK, MISSING, MISMATCH };

// Verify the trailing "|CRC" + 8 hex digits (anchored to the END of the
// string) and strip it on success. On MISSING/MISMATCH `cmd` is unchanged.
CrcCheck verifyStripCrc(char* cmd);

}  // namespace wcb
#endif
