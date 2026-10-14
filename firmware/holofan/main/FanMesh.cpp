#include "FanMesh.h"
#include <atomic>
#include <stdio.h>
#include <string.h>
#include "ConfigStoreIdf.h"
#include "Console.h"
#include "FanController.h"
#include "FanMcu.h"
#include "FanVersion.h"
#include "LiftCommands.h"
#include "LoudTest.h"
#include "MaintFsm.h"
#include "MaintIdf.h"
#include "McuProto.h"
#include "MeshRadioIdf.h"
#include "NodeConfig.h"
#include "PowerCycleTrigger.h"
#include "SagLog.h"
#include "SagMeter.h"
#include "MaintVolume.h"
#include "TxPower.h"
#include "ValidityGate.h"
#include "WcbNode.h"
#include "WcbPacket.h"
#include "command.h"
#include "esp_attr.h"
#include "esp_now.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static constexpr uint8_t DEFAULT_ID = 16;
static constexpr const char* DEFAULT_ALIAS = "HoloFan";
static constexpr uint8_t LOCAL_SENDER = 0;          // console "hl" commands: print, never send
static constexpr int CMDS_PER_LOOP = 4;
static constexpr uint32_t LOOP_MS = 5;              // WcbNode::poll() at <= 10 ms (README)
static constexpr uint8_t TERM_LINES_PER_SEC = 20;

static nodecfg::NodeConfig s_cfg;
static wcb::WcbNode        s_node;
static MeshRadioIdf        s_radio;
static bool                s_wifiOk = false;
static bool                s_radioUp = false;
static bool                s_maintOk = false;        // STA netif + handlers ready
static maint::MaintFsm     s_maint;
static maint::ValidityGate s_gate;
static maint::PowerCycleTrigger s_cycles;
static uint8_t             s_storedCycles = 0;       // read once at boot
static QueueHandle_t       s_lines = nullptr;        // console lines from the UART0 reader
static uint8_t             s_termTarget = 0;         // WCB that last sent us a command
static char                s_slot[17] = "?";
static uint32_t            s_dropped = 0;            // actions dropped: MCU queue full
static holofan::Catalog    s_catalog;                // mesh-task scratch for #HLFD
static fanlogic::LoudTest  s_loud;                   // #HLFY bench test (0.9.4)
static std::atomic<uint8_t> s_txDbm{0};              // #HLFO: saved mesh TX power, 0 = the default (0.9.10)
// A #HLFY read-back may take a whole setVolumeChecked plus an MCU-task pass.
static_assert(fanlogic::LOUD_RESULT_MS >= 2 * (fanlogic::ACT_SETTLE_MAX_MS +
                                               fanlogic::VOLUME_READS * (fanlogic::SETTING_TIMEOUT_MS +
                                                                         fanlogic::QUERY_SETTLE_MAX_MS)) + 5000,
              "LOUD_RESULT_MS must cover setVolumeChecked");
// The running #HLFY test, kept through a software reset (the brownout
// handler resets in software; the C2 has no RTC memory, .noinit survives).
static __NOINIT_ATTR fanlogic::LoudRecord s_rec;
static char                s_recLine[CON_LINE_MAX + 1];   // a reset report, sent at the first mesh command
// 0.9.12: the volume to put back after maintenance mode (volume 1 meanwhile),
// kept like s_rec through the reboot that ends an OTA.
static __NOINIT_ATTR fanlogic::MaintVolumeRecord s_mv;
static bool                s_mvDue = false;               // put it back at s_mvAt
static uint32_t            s_mvAt = 0;
constexpr uint32_t MAINT_VOLUME_WAIT_MS = 2000;   // for volume 1 to read back before the Wi-Fi starts
constexpr uint32_t MAINT_VOLUME_BACK_MS = 5000;   // after the return: not under the boot announces
constexpr uint32_t MAINT_VOLUME_BOOT_MS = 8000;   // after a reboot: the MCU handshake and the announces first
static bool                s_recPending = false;

static uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

static nodecfg::NodeRuntime runtime() {
  nodecfg::NodeRuntime rt;
  wcb::WcbCounters c = s_node.counters();
  rt.fw = HOLOFAN_VERSION;
  rt.slot = s_slot;
  rt.v2 = s_radio.v2();
  rt.meshUp = s_node.started();
  rt.rx = c.rxOk;
  rt.rej = c.rejMac + c.rejSize + c.rejPassword + c.rejSpoof + c.rejTarget;
  rt.crc = c.crcFail;
  rt.dup = c.dupDrop;
  rt.retry = c.retries;
  rt.gaveUp = c.gaveUp;
  return rt;
}

// Query replies are a mesh broadcast; console "hl" prints instead.
static void reply(uint8_t sender, const char* text) {
  if (sender == LOCAL_SENDER) { printf("%s\n", text); return; }   // con() would cut it at CON_LINE_MAX
  if (!s_cfg.replies || !s_node.started()) return;
  s_node.broadcast(text);
}

// Wi-Fi must be started. The radio keeps to steps (2, 5, 7, 8.5, 11, 13, 14,
// 15, 16.5, 18, 20 dBm) and takes the one at or under what is asked.
static void setTxPower(int quarterDbm) {
  esp_err_t err = esp_wifi_set_max_tx_power((int8_t)quarterDbm);
  if (err != ESP_OK) con("[mesh] TX power %d/4 dBm not set: %s", quarterDbm, esp_err_to_name(err));
}

// On the mesh the fan transmits at the saved #HLFO power (0.9.10): at
// volume 3 its own 20 dBm bursts dip the supply and start ~10 s silences.
static bool radioStart() {
  s_radioUp = s_wifiOk && s_radio.start(s_cfg, s_node);
  if (s_radioUp) setTxPower(fanlogic::txPowerQuarterDbm(s_txDbm.load()));
  return s_radioUp;
}

int fanMeshTxDbm() { return fanlogic::txPowerDbm(s_txDbm.load()); }

// #HLFY<r><s> (bench): volume 3 past the cap for s seconds with the radio
// on (r=1) or off (r=0), then the old volume (LoudTest.h). Each volume is
// read back before the next step. While the test holds the radio down,
// RTERM lines wait in the console queue (16 lines) and go out after rejoin.
static uint32_t s_loudReq = 0;                       // the checked-volume request awaited
static int      s_loudHoldEnd = -1;                  // cached volume when the hold ended (-1: not yet)
static bool     s_loudNoBod = false;                 // a sag instrument had the brownout reset off during the test
static uint32_t s_loudUnicast = 0;                   // r=2: unicast frames received (each ACKed on air) at RADIO_OFF

static const char* radioName(int mode) { return mode == 1 ? "on" : mode == 2 ? "RECEIVE-ONLY" : "OFF"; }

static void startLoudTest(int param) {
  holofan::Status st;
  fanMcuSnapshot(&st, nullptr);
  if (st.state != holofan::FanState::READY) { con("[test] refused: MCU not ready"); return; }
  if (s_maint.state() != maint::State::MESH || !s_radioUp) { con("[test] refused: not on the mesh"); return; }
  if (sagProbeRunning()) { con("[test] refused: #HLFJ is sending (TX power changed)"); return; }
  if (!s_loud.start(param, st.volume, nowMs(), fanMcuVolumeCap())) { con("[test] refused: a test is running"); return; }
  s_loudHoldEnd = -1;
  s_loudNoBod = sagRunning();
  uint32_t seq = fanlogic::loudRecordValid(s_rec) ? s_rec.seq + 1 : 1;
  s_rec = {};
  s_rec.seq = seq;
  s_rec.param = (uint32_t)param;
  s_rec.phase = (uint8_t)fanlogic::LoudPhase::PRE;
  fanlogic::loudRecordSeal(s_rec);
  con("[test] %lu: volume 3 for %d s with the radio %s, then volume %d", (unsigned long)seq, s_loud.seconds(),
      radioName(s_loud.mode()), s_loud.restoreVolume());
}

// Every loop while a test runs: where it is, for the next boot if this one ends in a reset.
static void recordLoudTest() {
  if (!s_loud.active() && s_loud.phase() != fanlogic::LoudPhase::DONE) return;
  uint8_t ph = (uint8_t)s_loud.phase();
  if (ph == s_rec.phase && !s_loud.active()) return;   // DONE already recorded
  uint32_t now = nowMs();
  s_rec.phase = ph;
  s_rec.testMs = s_loud.testMs(now);
  s_rec.holdMs = s_loud.holdMs(now);
  fanlogic::loudRecordSeal(s_rec);
}

static void loudLoop() {
  using A = fanlogic::LoudTest::Action;
  if (s_loudReq) {
    int got = fanMcuVolumeResult(s_loudReq);
    if (got >= 0) { s_loudReq = 0; s_loud.volumeResult(got, nowMs()); }
  }
  if (s_loud.active() && sagRunning()) s_loudNoBod = true;
  switch (s_loud.poll(nowMs())) {
    case A::VOLUME_UP:
      s_loudReq = fanMcuVolumeChecked(fanlogic::LOUD_VOLUME);
      if (!s_loudReq) {
        con("[test] MCU task not running: test not started");
        s_loud.cancel();
        s_rec.phase = (uint8_t)fanlogic::LoudPhase::DONE;   // nothing changed: nothing to report after a reset
        fanlogic::loudRecordSeal(s_rec);
      }
      break;
    case A::RADIO_OFF:
      s_rec.phase = (uint8_t)fanlogic::LoudPhase::QUIET;   // sealed before the radio goes (a reset in it reads QUIET)
      fanlogic::loudRecordSeal(s_rec);
      s_loudUnicast = MeshRadioIdf::unicastRx();
      s_node.suspend();                              // r=2: the node sends nothing; the radio still receives
      if (s_loud.mode() == 0) {                      // r=0: ESP-NOW and Wi-Fi stopped
        s_radioUp = false;
        if (!s_radio.powerOff()) con("[test] esp_wifi_stop failed: the radio is still on");
      }
      break;
    case A::VOLUME_RESTORE:
      if (s_loudHoldEnd < 0) {                       // a #HLFV, a mute or a reconnect cap during the hold shows here
        holofan::Status st;
        fanMcuSnapshot(&st, nullptr);
        s_loudHoldEnd = st.volume;
      }
      s_loudReq = fanMcuVolumeChecked(s_loud.restoreVolume());
      if (!s_loudReq) s_loud.retry(nowMs());
      break;
    case A::RADIO_ON:
      s_rec.phase = (uint8_t)fanlogic::LoudPhase::REJOIN;  // sealed before esp_wifi_start / PHY calibration
      fanlogic::loudRecordSeal(s_rec);
      if (s_loud.mode() == 2) {
        unsigned acked = (unsigned)(MeshRadioIdf::unicastRx() - s_loudUnicast);
        s_node.rejoin();
        if (acked) con("[test] receive-only hold: %u unicast frames reached the fan (each ACKed on air)", acked);
        break;
      }
      if (s_cfg.password[0] && radioStart()) { s_node.rejoin(); break; }
      con("[test] the radio did not come back: restarting");
      vTaskDelay(pdMS_TO_TICKS(100));
      fanMcuHoldForRestart();
      esp_restart();
      break;
    case A::REPORT:
      s_loudReq = 0;
      if (s_loudNoBod) con("[test] NOTE: #HLFE/#HLFJ had the brownout reset off: \"no reset\" is no verdict; read the [sag] L7 count");
      if (s_loud.confirmed() && s_loudHoldEnd != fanlogic::LOUD_VOLUME)
        con("[test] HOLD INTERRUPTED: volume %d at the end of the hold (not 3)", s_loudHoldEnd);
      if (!s_loud.confirmed())
        con("[test] STOPPED: the MCU reported volume %d, not 3; volume %d again (read back %d)", s_loud.upReadBack(),
            s_loud.restoreVolume(), s_loud.restoreReadBack());
      else if (s_loud.restoreReadBack() != s_loud.restoreVolume())
        con("[test] volume 3 for %d s with the radio %s: no reset; RESTORE FAILED: MCU reports volume %d", s_loud.seconds(),
            radioName(s_loud.mode()), s_loud.restoreReadBack());
      else
        con("[test] volume 3 for %d s with the radio %s: no reset; volume %d again (read back)", s_loud.seconds(),
            radioName(s_loud.mode()), s_loud.restoreVolume());
      break;
    case A::NONE:
      break;
  }
}

// #HLFJ probe frame: 250 zero bytes to the WCB group MAC (one burst, no ACK,
// no retry; no WCB accepts it: wrong size and magic). Runs on the probe task.
static bool probeSend(const uint8_t* data, size_t len) {
  uint8_t mac[6];
  wcb::groupMac(s_cfg.oct2, s_cfg.oct3, mac);
  return esp_now_send(mac, data, len) == ESP_OK;
}

static void runFan(const holofan::Cmd& c, uint8_t sender) {
  char out[holofan::REPLY_MAX + 1];
  switch (c.type) {
    case holofan::CmdType::STATUS: {
      holofan::Status s;
      fanMcuSnapshot(&s, nullptr);
      if (holofan::formatStatus(s, out, sizeof(out)) > 0) reply(sender, out);
      return;
    }
    case holofan::CmdType::DIAG:
      fanMcuDiag(out, sizeof(out));
      reply(sender, out);
      return;
    case holofan::CmdType::CATALOG_DUMP:
      fanMcuSnapshot(nullptr, &s_catalog);
      holofan::formatCatalog(s_catalog, out, sizeof(out));
      reply(sender, out);
      return;
    case holofan::CmdType::POWER_ON:
    case holofan::CmdType::POWER_OFF:
      return;                                        // the relay is the HoloLifter's (#HLF1/#HLF0 to node 15)
    case holofan::CmdType::LOUD_TEST:
      startLoudTest(c.param);
      return;
    case holofan::CmdType::SAG_METER:
      if (s_maint.state() != maint::State::MESH) { con("[sag] refused: not on the mesh"); return; }
      if (!sagStart(c.param)) { con("[sag] refused: an instrument is running"); return; }
      con("[sag] levels %d-%d for %d s: NO brownout reset until it ends", c.param / 10000, c.param / 1000 % 10,
          c.param % 1000);
      return;
    case holofan::CmdType::BLUETOOTH:
      if (sagRunning()) { con("[fan] refused: no NVS writes while #HLFE/#HLFJ has the brownout reset off"); return; }
      if (!fanMcuPost(c)) {
        con("[fan] busy: Bluetooth %s not applied and not saved; send it again", c.param ? "on" : "off");
        return;
      }
      configstore::saveBluetooth(c.param != 0);
      con("[fan] Bluetooth %s (saved; off is the default since 0.9.9)", c.param ? "ON" : "off");
      return;
    case holofan::CmdType::VOLUME_CAP:
      if (sagRunning()) { con("[fan] refused: no NVS writes while #HLFE/#HLFJ has the brownout reset off"); return; }
      if (s_loud.active()) { con("[fan] refused: #HLFY test running"); return; }
      if (!fanMcuPost(c)) {
        con("[fan] busy: volume cap %d not applied and not saved; send it again", c.param);
        return;
      }
      configstore::saveVolumeCap((uint8_t)c.param);
      if (c.param == 3) con("[fan] volume cap 3 saved: #HLFV3 sets 3 and the MCU's own volume 3 stays (#HLFH2 puts the cap back)");
      else con("[fan] volume cap 2 saved (the default): a volume 3 is lowered to 2");
      return;
    case holofan::CmdType::TX_POWER: {
      if (sagRunning()) { con("[mesh] refused: no NVS writes while #HLFE/#HLFJ has the brownout reset off"); return; }
      if (s_loud.active()) { con("[mesh] refused: #HLFY test running"); return; }
      s_txDbm.store((uint8_t)c.param);
      configstore::saveTxPower((uint8_t)c.param);
      int8_t q = 0;
      if (s_maint.state() != maint::State::MESH || !s_radioUp) {
        con("[mesh] TX power %d dBm%s saved; used from the next mesh start", fanMeshTxDbm(),
            c.param ? "" : " (the default)");
        return;
      }
      setTxPower(fanlogic::txPowerQuarterDbm(c.param));
      if (esp_wifi_get_max_tx_power(&q) != ESP_OK) q = 0;
      con("[mesh] TX power %d dBm%s saved (maintenance always uses %d); the radio is at %d.%02d dBm", fanMeshTxDbm(),
          c.param ? "" : " (the default)", fanlogic::TX_DBM_MAX, q / 4, (q % 4) * 25);
      return;
    }
    case holofan::CmdType::HEARTBEAT:
      s_node.setHeartbeatMs((uint32_t)c.param * 1000u);
      con("[mesh] heartbeat every %lu s (bench, 2-25 s: 10 s again at reboot or #HLFK00)",
          (unsigned long)(s_node.heartbeatMs() / 1000));
      return;
    case holofan::CmdType::TX_PROBE:
      if (s_maint.state() != maint::State::MESH || !s_radioUp || (s_loud.active() && s_loud.radioOff())) {
        con("[probe] refused: the radio is not on the mesh");
        return;
      }
      if (!sagStartProbe(c.param, probeSend)) { con("[probe] refused: an instrument is running"); return; }
      con("[probe] levels %d-%d at %d dBm, %d frames each (~%d s): NO brownout reset until it ends", c.param / 10000,
          c.param / 1000 % 10, c.param % 1000, fanlogic::PROBE_FRAMES,
          (c.param / 1000 % 10 - c.param / 10000 + 1) * fanlogic::PROBE_FRAMES / 10);
      return;
    default:
      if (!fanMcuPost(c)) con("[fan] busy: command dropped (%u total)", (unsigned)++s_dropped);
      return;
  }
}

static void enterMaint(uint8_t profile) {
  if (!s_maintOk) { con("[maint] refused: Wi-Fi station not available"); return; }
  if (s_loud.active()) { con("[maint] refused: #HLFY test running"); return; }
  if (sagRunning()) { con("[maint] refused: #HLFE sag meter running (no brownout reset)"); return; }
  bool set = profile >= 1 && profile <= 2 && s_cfg.profile[profile - 1].ssid[0];
  maint::Refusal r = s_maint.request(profile, set, true, nowMs());
  if (r != maint::Refusal::NONE) {
    con("[maint] refused: %s", r == maint::Refusal::BUSY ? "already in maintenance" : "profile not set");
    return;
  }
  holofan::Status st;
  fanMcuSnapshot(&st, nullptr);
  s_mvDue = false;                                  // a volume not yet put back stays in s_mv
  if (fanlogic::maintVolumeEnter(s_mv, st.state == holofan::FanState::READY ? st.volume : 0) &&
      !fanMcuPost(holofan::Cmd(holofan::CmdType::VOLUME, fanlogic::MAINT_VOLUME)))
    con("[maint] MCU busy: the volume was not lowered");
}

// Volume 1 must be in place before the Wi-Fi's full-power bursts start.
static void waitMaintVolume() {
  for (uint32_t t = 0; t < MAINT_VOLUME_WAIT_MS; t += 50) {
    holofan::Status st;
    fanMcuSnapshot(&st, nullptr);
    if (st.state != holofan::FanState::READY || st.volume == fanlogic::MAINT_VOLUME) return;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Back on the mesh (or booted after an OTA): put the volume back once the
// burst of announces is over, so the step up does not land under them.
static void maintVolumeLoop() {
  if (!s_mvDue || (int32_t)(nowMs() - s_mvAt) < 0) return;
  if (s_maint.state() != maint::State::MESH) { s_mvDue = false; return; }   // in maintenance again: its return arms this
  holofan::Status st;
  fanMcuSnapshot(&st, nullptr);
  if (st.state != holofan::FanState::READY) { s_mvAt = nowMs() + 1000; return; }
  fanlogic::MaintVolumeRecord keep = s_mv;
  int v = fanlogic::maintVolumeLeave(s_mv);
  if (v != 0 && !fanMcuPost(v == fanlogic::MAINT_RESTORE_CAP ? holofan::Cmd(holofan::CmdType::UNMUTE, 0)
                                                             : holofan::Cmd(holofan::CmdType::VOLUME, v))) {
    s_mv = keep;                                    // the MCU task's queue is full: again shortly
    s_mvAt = nowMs() + 500;
    return;
  }
  s_mvDue = false;
  if (v > 0) con("[maint] volume back to %d", v);
  else if (v == fanlogic::MAINT_RESTORE_CAP) con("[maint] volume back to the cap");
}

// Maintenance mode (docs/MAINTENANCE_API.md): leave the mesh, join profile p, serve the
// HTTP API, return on done / no IP in 20 s / link down 20 s.
static void maintLoop() {
  // An upload session holds maintenance through link loss.
  switch (s_maint.poll(nowMs(), maintidf::hasIp(), maintidf::uploadActive())) {
    case maint::Action::LEAVE_MESH_AND_JOIN:
      con("[maint] leaving mesh, joining profile %u (%s)", s_maint.profile(), s_cfg.profile[s_maint.profile() - 1].ssid);
      s_node.suspend();
      if (s_radioUp) { s_radio.stop(); s_radioUp = false; }
      waitMaintVolume();
      {
        char host[25];
        nodecfg::hostnameFromAlias(s_cfg.alias, host, sizeof(host));
        maintidf::join(s_cfg.profile[s_maint.profile() - 1], host);
        setTxPower(fanlogic::txPowerQuarterDbm(0));     // uploads and OTA: all the range the dome's Wi-Fi link has
      }
      break;
    case maint::Action::START_SERVER: {
      if (!maintidf::startServer(s_cfg.alias, HOLOFAN_VERSION)) {
        con("[maint] HTTP server failed to start; returning to mesh");
        s_maint.done();
        break;
      }
      char ip[16], host[25];
      maintidf::ipString(ip, sizeof(ip));
      nodecfg::hostnameFromAlias(s_cfg.alias, host, sizeof(host));
      con("[maint] serving http://%s/ (mDNS %s.local)", ip, host);
      break;
    }
    case maint::Action::RETURN_TO_MESH:
      con("[maint] returning to mesh");
      maintidf::stop();
      if (s_cfg.password[0] && radioStart()) s_node.rejoin();
      if (fanlogic::maintVolumeValid(s_mv)) { s_mvDue = true; s_mvAt = nowMs() + MAINT_VOLUME_BACK_MS; }
      break;
    case maint::Action::NONE:
      break;
  }
  if (maintidf::takeDone()) s_maint.done();
  if (maintidf::takeReboot()) {
    con("[maint] OTA image staged; rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));                 // let the HTTP 200 leave
    fanMcuHoldForRestart();
    esp_restart();
  }
}

static void runZ(const nodecfg::ZCmd& z, uint8_t sender) {
  bool writes = z.type != nodecfg::ZType::QUERY && z.type != nodecfg::ZType::MAINT;
  if (writes && sagRunning()) { con("[mesh] refused: no NVS writes while #HLFE/#HLFJ has the brownout reset off"); return; }
  switch (nodecfg::applyZ(z, s_cfg)) {
    case nodecfg::ZResult::SAVED:
      if (!configstore::save(s_cfg)) con("[mesh] ERROR save failed");
      break;
    case nodecfg::ZResult::SAVED_REBOOT:
      if (configstore::save(s_cfg)) con("[mesh] saved; applies after reboot");
      else con("[mesh] ERROR save failed");
      break;
    case nodecfg::ZResult::MAINT:
      enterMaint((uint8_t)z.num);
      break;
    case nodecfg::ZResult::QUERY: {
      char out[200];
      if (nodecfg::formatNodeReply(s_cfg, runtime(), out, sizeof(out)) > 0) reply(sender, out);
      break;
    }
  }
}

// One mesh (or console "hl") line. Unknown commands are silent on the mesh
// (actions never reply) and reported on the console.
// #HLFX<cmd> [args]: one raw line to the MCU (debug). Over the mesh only
// read/playback commands pass (rawMeshLineAllowed: never v, u, w, y, and no
// volume above VOLUME_MAX); the reply is printed with con(), so it reaches
// the sender as an RTERM line.
static bool runRawFromMesh(const char* rest) {
  while (*rest == ' ') rest++;                       // "#HLFX m 1" and "#HLFXm 1" both work
  if (!fanlogic::rawMeshLineAllowed(rest)) { con("[mcu] refused over the mesh: %s", rest); return true; }
  if (!fanMcuRaw(rest)) con("[mcu] busy");
  return true;
}

// Returns whether the line was one of ours (#HLF..., #HLZ...): only those
// make the sender the RTERM target (0.9.7: mesh broadcasts such as the
// HoloLifter's "#HL:STATUS" replies reach popCommand too).
static bool dispatch(uint8_t sender, char* raw) {
  char* line = lift::normalizeLine(raw);
  if (strncmp(line, "#HLFX", 5) == 0) { runRawFromMesh(line + 5); return true; }
  holofan::Cmd fc;
  nodecfg::ZCmd zc;
  if (holofan::parseCommand(line, fc)) { runFan(fc, sender); return true; }
  if (nodecfg::parseZ(line, zc)) { runZ(zc, sender); return true; }
  if (sender == LOCAL_SENDER) con("#HL:ERROR badcmd (%s)", line);
  return false;
}

static void printHelp() {
  con("HoloFan %s console (UART0 115200):", HOLOFAN_VERSION);
  con("  hl <#HL...>                  run a mesh command locally (e.g. hl #HLFS)");
  con("  mesh                         show mesh config and counters");
  con("  mesh pw <pw> | oct <hh> <hh> | ch <1-11> | q <0-20> | crc on|off");
  con("  mesh id <1-19> | name <alias> | reply on|off");
  con("  mesh ssid <1|2> <name> | pass <1|2> [pass] | clear <1|2> | off | restart");
  con("  mesh maint <1|2>             maintenance mode (Wi-Fi + HTTP OTA) on profile 1|2");
  con("  radio                        ESP-NOW receive diagnostics");
  con("  mcu <cmd> [args]             send one line to the LED/SD MCU, print its raw reply (never v)");
}

static void handleConsole(char* line) {
  if (strncmp(line, "hl ", 3) == 0) { dispatch(LOCAL_SENDER, line + 3); return; }
  if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) { printHelp(); return; }
  if (strcmp(line, "radio") == 0) {
    char out[256];
    s_radio.formatDiag(out, sizeof(out));
    printf("%s\n", out);                            // longer than one RTERM line
    return;
  }
  if (strncmp(line, "mcu ", 4) == 0) { if (!fanMcuRaw(line + 4)) con("[mcu] busy"); return; }
  if (strncmp(line, "mesh", 4) != 0 || (line[4] != '\0' && line[4] != ' ')) {
    con("unknown command (help)");
    return;
  }
  nodecfg::UsbCmd u;
  char err[96];
  if (!nodecfg::parseUsb(line, u, err, sizeof(err))) { con("ERROR %s", err); return; }
  switch (u.type) {
    case nodecfg::UsbType::SHOW: {
      static char out[640];                         // mesh task only
      if (nodecfg::formatShow(s_cfg, runtime(), out, sizeof(out)) > 0) printf("%s\n", out);
      return;
    }
    case nodecfg::UsbType::MAINT:
      enterMaint((uint8_t)u.a);
      return;
    case nodecfg::UsbType::RESTART:
      con("restarting...");
      vTaskDelay(pdMS_TO_TICKS(100));
      fanMcuHoldForRestart();
      esp_restart();
      return;
    default:
      if (nodecfg::applyUsb(u, s_cfg) && configstore::save(s_cfg)) con("OK saved (mesh restart to apply mesh settings)");
      else con("ERROR save failed");
      return;
  }
}

// Mirror queued console lines to the last sender, at most 20 a second.
bool fanMeshCanSend() { return s_termTarget != 0 && (s_node.started() || s_loud.radioDown()); }

static void pumpTerm() {
  static uint32_t windowMs = 0;
  static uint8_t sent = 0;
  char line[CON_LINE_MAX + 1];
  if (s_loud.radioDown()) return;                  // #HLFY radio-off hold: keep the lines for after rejoin
  while (conTakeMirror(line, sizeof(line))) {
    if (s_termTarget == 0 || !s_node.started()) continue;
    uint32_t now = nowMs();
    if (now - windowMs >= 1000) { windowMs = now; sent = 0; }
    if (sent >= TERM_LINES_PER_SEC) continue;
    sent++;
    s_node.sendTerm(s_termTarget, line);
  }
}

static bool meshBegin() {
  if (!s_cfg.password[0]) {
    con("[mesh] OFF: no mesh password set (UART0: mesh pw <password>, then mesh restart)");
    return false;
  }
  if (!s_wifiOk) return false;
  wcb::WcbConfig wc;
  wc.oct2 = s_cfg.oct2;
  wc.oct3 = s_cfg.oct3;
  wc.id = s_cfg.id;
  wc.floorQ = s_cfg.floorQ;
  strncpy(wc.password, s_cfg.password, sizeof(wc.password) - 1);
  wc.checksum = s_cfg.checksum;
  strncpy(wc.identity.devType, s_cfg.alias, sizeof(wc.identity.devType) - 1);
  strncpy(wc.identity.fw, HOLOFAN_VERSION, sizeof(wc.identity.fw) - 1);
  strncpy(wc.identity.hwRev, HOLOFAN_HWREV, sizeof(wc.identity.hwRev) - 1);
  strncpy(wc.identity.caps, HOLOFAN_CAPS, sizeof(wc.identity.caps) - 1);
  if (!radioStart() || !s_node.begin(wc, s_radio)) {
    con("[mesh] start FAILED (see messages above)");
    return false;
  }
  con("[mesh] node %u \"%s\" on channel %u, replies %s", s_cfg.id, s_cfg.alias, s_cfg.channel,
      s_cfg.replies ? "broadcast" : "off");
  return true;
}

// OTA validity: a new image in PENDING_VERIFY keeps itself only
// if, within 30 s, the mesh is up (when configured) and the MCU answered.
static void gateLoop() {
  if (s_node.started()) s_gate.meshUp();
  if (fanMcuAnswered()) s_gate.mcuOk();
  switch (s_gate.poll(nowMs())) {
    case maint::Verdict::VALID:
      esp_ota_mark_app_valid_cancel_rollback();
      con("[ota] new image marked valid");
      break;
    case maint::Verdict::INVALID:
      con("[ota] mesh not up within 30 s: rolling back");
      vTaskDelay(pdMS_TO_TICKS(100));
      fanMcuHoldForRestart();
      esp_ota_mark_app_invalid_rollback_and_reboot();
      break;
    case maint::Verdict::NONE:
      break;
  }
}

// Three short power cycles (each 2-10 s on) enter maintenance on profile 1
// (or 2 if 1 is unset) without the mesh: the way in when the mesh link is down.
static void cyclesLoop() {
  switch (s_cycles.poll(nowMs(), s_storedCycles)) {
    case maint::PowerCycleTrigger::Action::COUNT:
      configstore::savePowerCycles(s_cycles.newCount());
      con("[maint] power cycle %u of %u (cycle again within 10 s for maintenance)", s_cycles.newCount(), maint::PC_CYCLES);
      break;
    case maint::PowerCycleTrigger::Action::ENTER_MAINT:
      configstore::savePowerCycles(0);
      con("[maint] %u power cycles: entering maintenance mode", maint::PC_CYCLES);
      enterMaint(s_cfg.profile[0].ssid[0] ? 1 : 2);
      break;
    case maint::PowerCycleTrigger::Action::CLEAR:
      configstore::savePowerCycles(0);
      break;
    case maint::PowerCycleTrigger::Action::NONE:
      break;
  }
}

static void meshTask(void*) {
  s_cycles.begin(esp_reset_reason() == ESP_RST_POWERON);
  s_storedCycles = configstore::loadPowerCycles();
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  bool pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  // The MCU is NOT required: in the droid it can be silent for reasons
  // outside this image (seen 2026-10-06), and requiring it would roll back
  // every update, including the one that fixes it. The mesh (which carries
  // the way back into maintenance mode) is what must work.
  s_gate.begin(pending, s_cfg.password[0] != '\0', false, nowMs());
  if (pending) con("[ota] new image: must reach the mesh within 30 s or it rolls back");
  meshBegin();
  uint8_t sender;
  char line[wcb::COMMAND_FIELD + 1];
  char typed[CON_LINE_MAX + 1];
  for (;;) {
    gateLoop();
    cyclesLoop();
    s_node.poll();
    for (int i = 0; i < CMDS_PER_LOOP && s_maint.state() == maint::State::MESH && s_node.popCommand(sender, line, sizeof(line)); i++) {
      if (!dispatch(sender, line)) continue;      // not ours: neither the RTERM target nor the reset report
      s_termTarget = sender;                       // RTERM goes to whoever talked to us last (lines go out in pumpTerm)
      char boot[CON_LINE_MAX + 1];
      if (fanMcuTakeBootLine(boot, sizeof(boot))) con("%s", boot);   // what the MCU said at connect (0.9.8)
      if (fanMcuTakeLatchedUnprompted(boot, sizeof(boot))) con("[mcu] while off the mesh: %s", boot);
      if (s_recPending) {                          // the last boot ended in a reset during #HLFY
        s_recPending = false;
        con("%s", s_recLine);
        s_rec.reported = 1;
        fanlogic::loudRecordSeal(s_rec);
      }
    }
    if (xQueueReceive(s_lines, typed, 0) == pdTRUE) handleConsole(typed);
    loudLoop();
    recordLoudTest();
    pumpTerm();
    maintLoop();
    maintVolumeLoop();
    vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
  }
}

void fanMeshStart() {
  if (fanlogic::loudRecordValid(s_rec) && !s_rec.reported && s_rec.phase != (uint8_t)fanlogic::LoudPhase::DONE &&
      s_rec.phase != (uint8_t)fanlogic::LoudPhase::IDLE &&
      fanlogic::loudRecordFormat(s_rec, (int)esp_reset_reason(), s_recLine, sizeof(s_recLine)) > 0) {
    s_recPending = true;
    printf("%s\n", s_recLine);
  }
  // A power-on: .noinit holds whatever was there, and the MCU has started
  // afresh anyway. Any other reset (the reboot of an OTA, a brownout in
  // maintenance): the MCU still plays at the maintenance volume.
  if (esp_reset_reason() == ESP_RST_POWERON) fanlogic::maintVolumeClear(s_mv);
  else if (fanlogic::maintVolumeValid(s_mv)) { s_mvDue = true; s_mvAt = nowMs() + MAINT_VOLUME_BOOT_MS; }
  const esp_partition_t* run = esp_ota_get_running_partition();
  if (run) strncpy(s_slot, run->label, sizeof(s_slot) - 1);
  configstore::load(s_cfg, DEFAULT_ID, DEFAULT_ALIAS);
  s_txDbm.store(configstore::loadTxPower());
  s_lines = xQueueCreate(4, CON_LINE_MAX + 1);
  // The saved Bluetooth choice (#HLFA, off when never saved), applied once by
  // the MCU task after its handshake: no off/on blip at boot when it is on.
  fanMcuPost(holofan::Cmd(holofan::CmdType::BLUETOOTH, configstore::loadBluetooth() ? 1 : 0));
  s_wifiOk = MeshRadioIdf::initWifi();
  s_maintOk = s_wifiOk && maintidf::init();
  xTaskCreate(meshTask, "mesh", 6144, nullptr, 6, nullptr);
}

void fanMeshConsoleLine(const char* line) {
  char buf[CON_LINE_MAX + 1];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  if (s_lines && xQueueSend(s_lines, buf, 0) != pdTRUE) printf("console busy: line dropped\n");
}
