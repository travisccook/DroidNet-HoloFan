// Mesh node configuration shared by the HoloLifter and the HoloFan: the
// stored settings, their validation, the #HLZ mesh command family and the
// USB/UART "mesh ..." console commands (docs/SERIAL_COMMANDS.md).
// Pure C++: no Arduino / ESP-IDF headers. Storage lives in each target's
// adapter (Arduino Preferences / IDF nvs), namespace "hlmesh".
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H
#include <stddef.h>
#include <stdint.h>

namespace nodecfg {

struct WifiProfile {
  char ssid[33] = {0};    // 1-32 chars; empty = profile unset
  char pass[64] = {0};    // empty (open network) or 8-63 chars
};

struct NodeConfig {
  uint8_t     id = 0;              // 1..19 (20 is the WCB controller slot)
  char        alias[25] = {0};     // WDP device type = the ;w<alias> name
  bool        replies = true;      // broadcast query replies on the mesh (#HLZP1); false = silent
  uint8_t     floorQ = 3;          // pre-registered WCB peers 1..floorQ
  uint8_t     channel = 1;         // mesh channel 1..11
  uint8_t     oct2 = 0, oct3 = 0;  // mesh MAC octets
  char        password[40] = {0};  // mesh password; empty = mesh off
  bool        checksum = true;     // must match the fleet's ?ETM,CHKSM
  WifiProfile profile[2];          // 1 = home, 2 = DroidNet hotspot
};

void setDefaults(NodeConfig& c, uint8_t id, const char* alias);

bool validId(int id);                 // 1..19
bool validAlias(const char* s);       // 1-24 of [A-Za-z0-9_.-], not starting with a digit
bool validChannel(int ch);            // 1..11
bool validSsid(const char* s);        // 1-32 printable chars
bool validPass(const char* s);        // empty or 8-63 printable chars
bool validPassword(const char* s);    // mesh password: 1-39 printable chars, no ^ , ;

// Lower-cased alias for the mDNS hostname ("HoloFan" -> "holofan").
void hostnameFromAlias(const char* alias, char* out, size_t outSize);

// Decode an even-length hex string into `out` (NUL-terminated). Fails on odd
// length, a non-hex digit, a decoded NUL, or if it does not fit.
bool hexDecode(const char* hex, char* out, size_t outSize);

// ---- #HLZ mesh commands -----------------------------------------------------
enum class ZType { NONE, REPLY, ID, NAME, SSID, PASS, MAINT, QUERY };

struct ZCmd {
  ZType   type = ZType::NONE;
  int     num = 0;          // REPLY 0/1, ID 1-19, MAINT/SSID/PASS profile 1-2
  char    text[64] = {0};   // NAME alias, decoded SSID or passphrase
};

// Parse a normalized #HLZ line. False if it is not a valid #HLZ command.
bool parseZ(const char* line, ZCmd& out);

enum class ZResult {
  SAVED,          // config changed; persist; takes effect now (replies, Wi-Fi profile)
  SAVED_REBOOT,   // config changed; persist; takes effect after reboot (id, alias)
  MAINT,          // caller should try to enter maintenance mode on profile num
  QUERY           // caller should reply with formatNodeReply()
};
// Apply a parsed command to `c`. Never fails (parseZ validated it).
ZResult applyZ(const ZCmd& z, NodeConfig& c);

struct NodeRuntime {
  const char* fw = "";
  const char* slot = "";
  bool     v2 = false;        // ESP-NOW v2 active
  bool     meshUp = false;
  uint32_t rx = 0, rej = 0, crc = 0, dup = 0, retry = 0, gaveUp = 0;
};
// "#HL:NODE id=.. name=.. reply=on|off ch=.. q=.. v2=.. up=.. rx=.. rej=.. crc=.. dup=.. rt=.. gu=.."
// (rt = ETM retries, gu = sends given up; firmware and slot are in #HLI).
// Worst case is 144 chars (reply=off, 24-char alias, max counters), inside one 187-char mesh command.
int formatNodeReply(const NodeConfig& c, const NodeRuntime& rt, char* out, size_t outSize);

// ---- USB / UART console: "mesh ..." -----------------------------------------
// Case is preserved. The last argument of pw / ssid / pass / name takes the
// rest of the line, so Wi-Fi names with spaces work.
enum class UsbType {
  NONE, SHOW, PASSWORD, OCTETS, CHANNEL, FLOOR, CHECKSUM, ID, NAME, REPLY, SSID, PASS, MAINT, RESTART, MESH_OFF, CLEAR
};
struct UsbCmd {
  UsbType type = UsbType::NONE;
  int     a = 0, b = 0;       // OCTETS oct2/oct3; CHANNEL/FLOOR/ID value; CHECKSUM/REPLY 0/1; SSID/PASS/MAINT profile
  char    text[64] = {0};     // PASSWORD, NAME, SSID, PASS text
};
// Parse a raw console line beginning with "mesh". On a bad argument returns
// false and writes a usage hint to err.
bool parseUsb(const char* line, UsbCmd& out, char* err, size_t errSize);
// Apply a parsed console command (SHOW/MAINT/RESTART change nothing here).
// Returns true if `c` changed and must be saved.
bool applyUsb(const UsbCmd& u, NodeConfig& c);
// Multi-line human-readable dump for "mesh" (password shown as ***).
int formatShow(const NodeConfig& c, const NodeRuntime& rt, char* out, size_t outSize);

}  // namespace nodecfg
#endif
