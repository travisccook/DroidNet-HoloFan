// Maintenance ("upload") mode state machine, shared by the HoloLifter and the
// HoloFan (docs/MAINTENANCE_API.md). Pure C++ driven by an injected clock; the
// adapter performs the returned actions (leave mesh, join Wi-Fi, start the
// HTTP server, return to the mesh).
//
//   MESH --request()--> JOINING --IP within 20 s--> SERVING --done()--> MESH
//                          |                           |
//                          +--no IP in 20 s--> MESH    +--link down 20 s--> MESH
// (link down is ignored while held: poll(..., hold = true)).
//
// A power cycle always lands in MESH (nothing is persisted). No idle timeout.
#ifndef MAINT_FSM_H
#define MAINT_FSM_H
#include <stdint.h>

namespace maint {

constexpr uint32_t JOIN_TIMEOUT_MS = 20000;   // join or re-join window
constexpr uint32_t LINK_LOST_MS    = 20000;   // serving but disconnected this long -> give up

enum class State { MESH, JOINING, SERVING };
enum class Action { NONE, LEAVE_MESH_AND_JOIN, START_SERVER, RETURN_TO_MESH };
enum class Refusal { NONE, BUSY, NO_PROFILE, NOT_SAFE };

class MaintFsm {
 public:
  // Ask to enter maintenance on `profile` (1|2). `profileSet` = it has an
  // SSID; `safe` = the node allows it now (e.g. the lift is not moving).
  Refusal request(uint8_t profile, bool profileSet, bool safe, uint32_t now);
  // POST /api/done. Safe to call from another task only through the
  // adapter's own flag; MaintFsm itself is single-task.
  void done() { doneRequested_ = true; }
  // Call every loop. `gotIp` = the Wi-Fi station currently has an IP.
  // `hold` (HoloFan: an upload session is running): a
  // lost link never returns to the mesh; the LINK_LOST_MS count starts
  // when the hold ends.
  Action poll(uint32_t now, bool gotIp, bool hold = false);
  State state() const { return state_; }
  uint8_t profile() const { return profile_; }

 private:
  State    state_ = State::MESH;
  uint8_t  profile_ = 0;
  bool     leavePending_ = false;
  bool     doneRequested_ = false;
  uint32_t since_ = 0;        // JOINING: entry time; SERVING: time the link went down
  bool     linkDown_ = false;
};

}  // namespace maint
#endif
