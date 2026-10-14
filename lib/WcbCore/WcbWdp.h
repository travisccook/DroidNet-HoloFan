// WDP (WCB Discovery Protocol) device-identity advert, client side:
// building our advert and recognising a SOLICIT ("advertise now").
// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#ifndef WCB_WDP_H
#define WCB_WDP_H
#include <stdint.h>

namespace wcb {

constexpr uint8_t WDP_MAGIC   = 'W';
constexpr uint8_t WDP_PROTO   = 0x01;
constexpr uint8_t TLV_END     = 0x00;
constexpr uint8_t TLV_FWVER   = 0x03;
constexpr uint8_t TLV_DEVTYPE = 0x0B;
constexpr uint8_t TLV_HWREV   = 0x0C;
constexpr uint8_t TLV_CAPTAGS = 0x0D;
constexpr uint8_t TLV_SOLICIT = 0x11;

struct Identity {
  char devType[25] = {0};  // also the ;w<alias> name; no ',' ']' or leading digit
  char fw[28]      = {0};
  char hwRev[16]   = {0};
  char caps[49]    = {0};  // space-separated tags, e.g. "lift.motor"
};

// Encode 'W' 0x01 DEVTYPE [FWVER] [HWREV] [CAPTAGS] END into out (at most
// `max` bytes). Returns the byte count, or 0 if devType is empty (no advert).
int buildAdvert(const Identity& id, uint8_t* out, int max);

// True if the WDP payload carries a SOLICIT TLV. Must be checked before any
// advert decoding (a SOLICIT carries no facts).
bool isSolicit(const uint8_t* payload, int len);

}  // namespace wcb
#endif
