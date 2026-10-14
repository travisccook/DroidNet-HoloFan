#include "UploadSession.h"
#include <string.h>

namespace fanlogic {

const char* queryErrName(QueryErr e) {
  switch (e) {
    case QueryErr::OK: return "ok";
    case QueryErr::NAME: return "name";
    case QueryErr::LENGTH: return "length";
    case QueryErr::QUERY: return "query";
  }
  return "?";
}

// Decimal digits only (no sign, at most 10 digits), value <= max.
static bool parseDec(const char* p, const char* end, uint64_t max, uint64_t& v) {
  if (p >= end || end - p > 10) return false;
  v = 0;
  for (; p < end; p++) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (uint64_t)(*p - '0');
  }
  return v <= max;
}

QueryErr parseUploadQuery(UploadReq kind, const char* query, UploadQuery& out) {
  UploadQuery q;
  if (!query) query = "";
  const char* stop = query + strcspn(query, "#");
  bool haveName = false, haveSize = false, haveOff = false, haveMhz = false, haveClk = false,
       havePace = false, haveIgap = false, haveKeep = false, havePre = false, haveDry = false, haveDark = false;
  for (const char* p = query; query != stop && p <= stop;) {
    const char* end = p + strcspn(p, "&#");
    if (end > stop) end = stop;
    const char* eq = (const char*)memchr(p, '=', (size_t)(end - p));
    if (!eq) return QueryErr::QUERY;                       // also an empty pair
    const size_t klen = (size_t)(eq - p);
    const char* v = eq + 1;
    auto key = [&](const char* k) { return strlen(k) == klen && memcmp(p, k, klen) == 0; };
    uint64_t n = 0;
    if (kind == UploadReq::OPEN && key("name") && !haveName) {
      char raw[3 * holofan::CLIP_NAME_MAX + 1];
      const size_t vlen = (size_t)(end - v);
      if (vlen >= sizeof(raw)) return QueryErr::NAME;
      memcpy(raw, v, vlen);
      raw[vlen] = '\0';
      // Reject if the raw value contains '?' (would be silently truncated by decodeClipPath)
      if (memchr(raw, '?', vlen)) return QueryErr::NAME;
      if (!decodeClipPath(raw, q.name, sizeof(q.name)) || !isUploadName(q.name)) return QueryErr::NAME;
      haveName = true;
    } else if (kind == UploadReq::OPEN && key("size") && !haveSize) {
      if (!parseDec(v, end, CLIP_MAX_BYTES, n) || n == 0) return QueryErr::LENGTH;
      q.size = (size_t)n;
      haveSize = true;
    } else if (kind == UploadReq::OPEN && (key("mhz") || key("clk"))) {
      if (!parseSpiPair(p, end, q.spi, haveMhz, haveClk)) return QueryErr::QUERY;
    } else if (kind == UploadReq::OPEN && key("pace") && !havePace) {
      if (end - v == 7 && memcmp(v, "reserve", 7) == 0) q.pace.pace = Pace::RESERVE;
      else if (end - v == 5 && memcmp(v, "burst", 5) == 0) q.pace.pace = Pace::BURST;
      else return QueryErr::QUERY;     // trickle too: it waits inside a window (single PUT only)
      havePace = true;
    } else if (kind == UploadReq::OPEN && key("igap") && !haveIgap) {
      if (!parseDec(v, end, 60000, n)) return QueryErr::QUERY;
      q.pace.igapMs = (uint32_t)n;
      haveIgap = true;
    } else if (kind == UploadReq::OPEN && key("keep") && !haveKeep) {
      if (!parseDec(v, end, 10000, n) || n < 1000) return QueryErr::QUERY;
      q.pace.keepMs = (uint32_t)n;
      haveKeep = true;
    } else if (kind == UploadReq::OPEN && key("pre") && !havePre) {
      if (!parseDec(v, end, SESSION_PRE_MAX, n) || n < 20) return QueryErr::QUERY;
      q.pre = (size_t)n;
      havePre = true;
    } else if (kind == UploadReq::OPEN && key("dry") && !haveDry) {
      if (!parseDec(v, end, 600000, n) || n < 1000) return QueryErr::QUERY;
      q.dryMs = (uint32_t)n;
      haveDry = true;
    } else if (kind == UploadReq::OPEN && key("dark") && !haveDark) {
      if (end - v != 1 || (*v != '0' && *v != '1')) return QueryErr::QUERY;
      q.pace.dark = *v == '1';
      haveDark = true;
    } else if (kind != UploadReq::OPEN && key("id") && !q.hasId) {
      if (!parseDec(v, end, 0x7fffffff, n) || n == 0) return QueryErr::QUERY;
      q.id = (uint32_t)n;
      q.hasId = true;
    } else if (kind == UploadReq::CHUNK && key("off") && !haveOff) {
      if (!parseDec(v, end, CLIP_MAX_BYTES, n)) return QueryErr::LENGTH;
      q.off = (size_t)n;
      haveOff = true;
    } else {
      return QueryErr::QUERY;
    }
    p = end + 1;
  }
  if (kind == UploadReq::OPEN && !haveName) return QueryErr::NAME;
  if (kind == UploadReq::OPEN && !haveSize) return QueryErr::LENGTH;
  if (kind == UploadReq::CHUNK && !q.hasId) return QueryErr::QUERY;
  if (kind == UploadReq::CHUNK && !haveOff) return QueryErr::LENGTH;
  if (kind == UploadReq::OPEN && !haveDry && q.pace.pace != Pace::RESERVE) q.dryMs = SESSION_DRY_MS;
  if (kind == UploadReq::OPEN && q.pace.igapMs >= q.dryMs) return QueryErr::QUERY;   // an igap wait would trip the dry rule
  if (kind == UploadReq::OPEN && q.pace.pace == Pace::RESERVE && q.pace.keepMs >= q.dryMs) return QueryErr::QUERY;   // so would a keep wait
  out = q;
  return QueryErr::OK;
}

OpenDecision openDecision(bool active, const char* activeName, size_t activeSize, const char* name, size_t size) {
  if (!active) return OpenDecision::NEW;
  return strcmp(activeName, name) == 0 && activeSize == size ? OpenDecision::SAME : OpenDecision::BUSY;
}

bool jsonString(const char* s, char* out, size_t size) {
  size_t o = 0;
  for (const char* p = s; *p; p++) {
    unsigned char c = (unsigned char)*p;
    bool esc = c == '"' || c == '\\';
    if (o + (esc ? 2 : 1) + 1 > size) {
      if (size) out[0] = '\0';
      return false;
    }
    if (esc) out[o++] = '\\';
    out[o++] = c < 0x20 ? '.' : (char)c;
  }
  if (size == 0) return false;
  out[o] = '\0';
  return true;
}

UploadResult runSession(McuLease& lease, FrameSource& src, Clock& clock, const char* name, size_t size,
                        const SpiConfig& cfg, const PaceConfig& pace, uint8_t* frame, char* scratch,
                        size_t scratchSize, char* diag, size_t diagSize) {
  if (diagSize) diag[0] = '\0';
  auto bodyResult = [&](ClipErr e) {
    UploadResult r = {e, false};
    r.bodyBytes = src.bodyBytes();
    r.bodyEnd = src.bodyEnd();
    return r;
  };
  while (src.state() == BodyState::ARRIVING && !src.prefilled()) src.wait(SESSION_WAIT_MS);
  if (src.state() == BodyState::FAILED) return bodyResult(src.failErr());
  const uint32_t t0 = clock.nowMs();
  McuLink* link = nullptr;
  while (!(link = lease.lease(LEASE_TRY_MS))) {
    if ((uint32_t)(clock.nowMs() - t0) >= LEASE_RETRY_MS) return bodyResult(ClipErr::BUSY);
    src.wait(SESSION_WAIT_MS);
    if (src.state() == BodyState::FAILED) return bodyResult(src.failErr());
  }
  ClipSpi* spi = lease.spi(cfg);
  if (!spi) {
    lease.diag(diag, diagSize);
    lease.release(false);
    return bodyResult(ClipErr::SPI);
  }
  UploadResult r = uploadClip(*link, *spi, src, clock, name, size, frame, scratch, scratchSize, pace);
  lease.diag(diag, diagSize);
  lease.release(r.err != ClipErr::EXISTS);
  return r;
}

}  // namespace fanlogic
