// CRC-32/ISO-HDLC as zlib computes it (reflected poly 0xEDB88320, init and
// final complement), incremental like zlib.crc32(data, crc): start with 0,
// feed the previous result back in. The clip upload reports the CRC of
// every byte it handed to the SPI driver, so the bench can compare it with
// zlib.crc32() over the padded clip file.
// Header-only (the table is built at compile time).
#ifndef FAN_CRC32_H
#define FAN_CRC32_H
#include <stddef.h>
#include <stdint.h>

namespace fanlogic {

namespace detail {
struct Crc32Table {
  uint32_t v[256];
  constexpr Crc32Table() : v() {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
      v[i] = c;
    }
  }
};
inline constexpr Crc32Table CRC32_TABLE{};
}  // namespace detail

inline uint32_t crc32Update(uint32_t crc, const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  crc = ~crc;
  while (len--) crc = detail::CRC32_TABLE.v[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

}  // namespace fanlogic
#endif
