#include "MaintFsm.h"

namespace maint {

Refusal MaintFsm::request(uint8_t profile, bool profileSet, bool safe, uint32_t now) {
  if (state_ != State::MESH) return Refusal::BUSY;
  if (profile < 1 || profile > 2 || !profileSet) return Refusal::NO_PROFILE;
  if (!safe) return Refusal::NOT_SAFE;
  state_ = State::JOINING;
  profile_ = profile;
  leavePending_ = true;
  doneRequested_ = false;
  since_ = now;
  return Refusal::NONE;
}

Action MaintFsm::poll(uint32_t now, bool gotIp, bool hold) {
  switch (state_) {
    case State::MESH:
      doneRequested_ = false;
      return Action::NONE;
    case State::JOINING:
      if (leavePending_) { leavePending_ = false; since_ = now; return Action::LEAVE_MESH_AND_JOIN; }
      if (gotIp) { state_ = State::SERVING; linkDown_ = false; return Action::START_SERVER; }
      if ((uint32_t)(now - since_) >= JOIN_TIMEOUT_MS) { state_ = State::MESH; return Action::RETURN_TO_MESH; }
      return Action::NONE;
    case State::SERVING:
      if (doneRequested_) { doneRequested_ = false; state_ = State::MESH; return Action::RETURN_TO_MESH; }
      if (gotIp || hold) { linkDown_ = false; return Action::NONE; }   // held: the count restarts later
      if (!linkDown_) { linkDown_ = true; since_ = now; return Action::NONE; }
      if ((uint32_t)(now - since_) >= LINK_LOST_MS) { state_ = State::MESH; return Action::RETURN_TO_MESH; }
      return Action::NONE;
  }
  return Action::NONE;
}

}  // namespace maint
