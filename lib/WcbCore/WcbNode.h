// A WCB mesh client node: joins the ESP-NOW mesh as node `id`, ACKs and
// delivers commands addressed to it, sends replies with ETM retries, and
// keeps up its heartbeat, BOOT announces, WDP adverts and RTERM lines.
//
// Threading contract:
//   ingest()          - any task (the radio receive callback). Copies only.
//   everything else   - one owner task (Arduino loop() / the fan's mesh task).
// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#ifndef WCB_NODE_H
#define WCB_NODE_H
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include "WcbPacket.h"
#include "WcbPeers.h"
#include "WcbPlatform.h"
#include "WcbRing.h"
#include "WcbSchedule.h"
#include "WcbSendQueue.h"
#include "WcbWdp.h"

namespace wcb {

struct WcbConfig {
  uint8_t  oct2 = 0;
  uint8_t  oct3 = 0;
  uint8_t  id = 0;            // 1..20
  uint8_t  floorQ = 3;        // pre-register peers 1..floorQ (0..20)
  char     password[PASSWORD_FIELD] = {0};
  bool     checksum = true;   // must match the fleet's ?ETM,CHKSM
  Identity identity;          // empty devType = no WDP advert
};

struct WcbCounters {
  uint32_t rxOk = 0;
  uint32_t rejMac = 0;        // source MAC not in this mesh
  uint32_t rejSize = 0;       // not a 252-byte ETM frame
  uint32_t rejPassword = 0;
  uint32_t rejSpoof = 0;      // sender id out of range, != MAC[5], or our own id
  uint32_t rejTarget = 0;     // addressed to another node
  uint32_t crcFail = 0;       // missing or wrong |CRC (ACKed anyway)
  uint32_t dupDrop = 0;       // retransmit of a command already delivered
  uint32_t ringFull = 0;      // receive ring overflow (frame lost before poll)
  uint32_t cmdQueueFull = 0;  // queue full: command not ACKed, left for the sender's retry
  uint32_t txFail = 0;        // platform send() returned false
  uint32_t retries = 0;
  uint32_t gaveUp = 0;
  uint32_t sendQueueFull = 0; // all PENDING_MAX slots held ensured sends
};

// Called from ingest() (radio task, small stack) for a COMMAND that passes
// the gate and the CRC check, before it is queued. Must be short, must not
// block, and should keep its own stack use small.
typedef void (*PeekFn)(void* ctx, uint8_t sender, const char* cmd);

class WcbNode {
 public:
  static constexpr size_t RING_N = 8;    // received frames buffered between polls
  static constexpr size_t CMD_QUEUE_N = 8;

  // Validates the config (id 1..20, floorQ <= 20, non-empty password),
  // registers peers 1..floorQ (minus self) and the group MAC, and arms the
  // BOOT / heartbeat / WDP schedule. False on a bad config.
  bool begin(const WcbConfig& cfg, WcbPlatform& platform);
  void ingest(const uint8_t mac[6], const uint8_t* data, size_t len);
  // Install the peek hook. Only before the first begin(): ingest() reads it
  // without locking. Returns false (and changes nothing) once begun.
  bool setPeekHook(PeekFn fn, void* ctx);
  // Process received frames, run timers and retries. Call often (<= 10 ms).
  void poll();
  // Next delivered command (CRC stripped). False if none.
  bool popCommand(uint8_t& sender, char* buf, size_t size);
  // Unicast a text command (e.g. ";S2,#HL:POS 4520") to node `target`.
  // False if not started, bad target, too long (187 with checksum, else 199),
  // or the send could not be tracked (sent once, untracked).
  bool send(uint8_t target, const char* text, bool ensured = true);
  // Broadcast a text command to every node (target 0, group MAC), best
  // effort: never tracked or retried. WCBs write such text to their
  // broadcast-out serial ports (?BCAST,OUT). Same length limits as send().
  bool broadcast(const char* text);
  // Send one console line as RTERM packet(s) of up to 160 chars.
  bool sendTerm(uint8_t target, const char* line);
  bool isOnline(uint8_t id) const { return peers_.isOnline(id); }
  // Stop all mesh activity (the adapter is about to leave the channel).
  void suspend();
  // Back on the mesh after suspend(): the adapter has re-initialised
  // ESP-NOW. Re-registers peers, forgets every sender's dedupe window and
  // pending send, and restarts the BOOT / heartbeat / WDP schedule.
  void rejoin();
  bool started() const { return started_.load(std::memory_order_acquire); }
  // Bench: heartbeat period (Schedule::setHeartbeatMs; 0 = default). Same task as poll().
  void setHeartbeatMs(uint32_t ms) { sched_.setHeartbeatMs(ms); }
  uint32_t heartbeatMs() const { return sched_.heartbeatMs(); }
  uint8_t id() const { return cfg_.id; }
  WcbCounters counters() const;

 private:
  enum class Gate { OK, MAC, PASSWORD, SPOOF, TARGET };
  struct CmdItem { uint8_t sender; char text[COMMAND_FIELD]; };

  Gate gate(const uint8_t mac[6], const EtmPacket& p, uint8_t& sender) const;
  bool extractCommand(const EtmPacket& p, char out[COMMAND_FIELD + 1]) const;
  void handle(const typename PacketRing<RING_N>::Slot& s, uint32_t now);
  void handleCommand(const EtmPacket& p, uint8_t sender);
  void registerPeers();
  void ensurePeer(uint8_t id);
  void sendAck(uint8_t target, uint16_t seq);
  void sendGroup(uint8_t type);
  void sendAdvert();
  void buildCommand(EtmPacket& p, uint8_t target, uint16_t seq, const char* text) const;
  bool transmitCommand(uint8_t target, uint16_t seq, const char* text);
  bool fitsCommand(const char* text) const;
  bool sendFrame(const uint8_t mac[6], const void* frame, size_t len);
  uint16_t nextSeq();
  bool pushCmd(uint8_t sender, const char* text);
  static void retransmitThunk(void* ctx, uint8_t target, uint16_t seq, const char* text);

  WcbConfig    cfg_;
  WcbPlatform* plat_ = nullptr;
  uint8_t      groupMac_[6] = {0};
  std::atomic<bool> started_{false};
  bool         begun_ = false;
  PeekFn       peek_ = nullptr;
  void*        peekCtx_ = nullptr;
  PacketRing<RING_N> ring_;
  std::atomic<uint32_t> ringFull_{0};   // written by ingest() only
  PeerTable    peers_;
  SendQueue    sendq_;
  Schedule     sched_;
  uint16_t     seq_ = 0;
  CmdItem      cmdq_[CMD_QUEUE_N];
  size_t       cmdHead_ = 0;
  size_t       cmdCount_ = 0;
  WcbCounters  counters_;
};

}  // namespace wcb
#endif
