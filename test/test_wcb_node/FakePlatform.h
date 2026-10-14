// Test double for wcb::WcbPlatform: a settable clock and a log of every
// frame sent and every peer added.
#ifndef FAKE_PLATFORM_H
#define FAKE_PLATFORM_H
#include <string.h>
#include <vector>
#include "WcbPacket.h"
#include "WcbPlatform.h"
#include "WcbRterm.h"

struct Frame {
  uint8_t mac[6];
  std::vector<uint8_t> data;
  const wcb::EtmPacket& etm() const { return *reinterpret_cast<const wcb::EtmPacket*>(data.data()); }
  const wcb::RtermPacket& rterm() const { return *reinterpret_cast<const wcb::RtermPacket*>(data.data()); }
};

class FakePlatform : public wcb::WcbPlatform {
 public:
  uint32_t now = 0;
  bool sendOk = true;
  std::vector<Frame> sent;
  std::vector<std::vector<uint8_t>> peers;

  uint32_t nowMs() override { return now; }
  bool send(const uint8_t mac[6], const uint8_t* data, size_t len) override {
    Frame f;
    memcpy(f.mac, mac, 6);
    f.data.assign(data, data + len);
    sent.push_back(f);
    return sendOk;
  }
  bool addPeer(const uint8_t mac[6]) override {
    peers.push_back(std::vector<uint8_t>(mac, mac + 6));
    return true;
  }
  // Frames of one packet type (ETM frames only).
  std::vector<Frame> ofType(uint8_t type) const {
    std::vector<Frame> out;
    for (const auto& f : sent)
      if (f.data.size() == sizeof(wcb::EtmPacket) && f.etm().packetType == type) out.push_back(f);
    return out;
  }
  bool hasPeer(const uint8_t mac[6]) const {
    for (const auto& p : peers)
      if (memcmp(p.data(), mac, 6) == 0) return true;
    return false;
  }
};
#endif
