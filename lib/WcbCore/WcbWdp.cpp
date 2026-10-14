// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#include "WcbWdp.h"
#include <string.h>

namespace wcb {

static int putTlv(uint8_t* buf, int o, int max, uint8_t type, const char* s) {
  int len = (int)strlen(s);
  if (len > 255) len = 255;
  if (o + 2 + len > max) return o;  // no room: skip this TLV
  buf[o++] = type;
  buf[o++] = (uint8_t)len;
  memcpy(buf + o, s, (size_t)len);
  return o + len;
}

int buildAdvert(const Identity& id, uint8_t* out, int max) {
  if (!id.devType[0] || max < 3) return 0;
  int o = 0;
  out[o++] = WDP_MAGIC;
  out[o++] = WDP_PROTO;
  o = putTlv(out, o, max, TLV_DEVTYPE, id.devType);
  if (id.fw[0])    o = putTlv(out, o, max, TLV_FWVER, id.fw);
  if (id.hwRev[0]) o = putTlv(out, o, max, TLV_HWREV, id.hwRev);
  if (id.caps[0])  o = putTlv(out, o, max, TLV_CAPTAGS, id.caps);
  if (o < max) out[o++] = TLV_END;
  return o;
}

bool isSolicit(const uint8_t* payload, int len) {
  if (len < 2 || payload[0] != WDP_MAGIC || payload[1] != WDP_PROTO) return false;
  int s = 2;
  while (s + 2 <= len) {
    uint8_t type = payload[s];
    if (type == TLV_END) break;
    int l = payload[s + 1];
    if (s + 2 + l > len) break;
    if (type == TLV_SOLICIT) return true;
    s += 2 + l;
  }
  return false;
}

}  // namespace wcb
