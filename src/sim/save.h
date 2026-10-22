#pragma once
// Saved games. A save file is
//
//   "RSAV"  u32 format  u32 headerBytes  header (JSON, for people and tools)
//   u64 payloadBytes  payload (binary, little-endian)  u64 FNV-1a of all of the above
//
// The payload holds the whole simulation (world bricks, bodies, components,
// debris, water bookkeeping and the air) followed by whatever the game layer
// appends (player, tools, salvage...). Bricks are run-length encoded per field,
// so the 345 MB default world saves in a few megabytes. Files are written to a
// temporary name and renamed into place, so a crash mid-save never leaves a
// half-written file under the real name.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "sim.h"

namespace save {

constexpr uint32_t FORMAT = 1;

struct Writer {
  std::vector<uint8_t> buf;
  void bytes(const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; buf.insert(buf.end(), b, b + n); }
  void u8(uint8_t v) { buf.push_back(v); }
  void u16(uint16_t v) { bytes(&v, 2); }
  void u32(uint32_t v) { bytes(&v, 4); }
  void i32(int32_t v) { bytes(&v, 4); }
  void u64(uint64_t v) { bytes(&v, 8); }
  void f32(float v) { bytes(&v, 4); }
  void f64(double v) { bytes(&v, 8); }
  void v3(const vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
  void iv3(const ivec3& v) { i32(v.x); i32(v.y); i32(v.z); }
  void str(const std::string& s) { u32((uint32_t)s.size()); bytes(s.data(), s.size()); }
  void tag(const char* t) { bytes(t, 4); }
};

// Every read is bounds-checked; the first failure latches `ok = false` and all
// later reads return zeros, so parsers can check once per section.
struct Reader {
  const uint8_t* p = nullptr;
  const uint8_t* end = nullptr;
  bool ok = true;
  std::string err;
  Reader(const uint8_t* b, size_t n) : p(b), end(b + n) {}
  bool fail(const std::string& why) { if (ok) { ok = false; err = why; } return false; }
  bool bytes(void* dst, size_t n) {
    if (!ok || (size_t)(end - p) < n) { memset(dst, 0, n); return fail("unexpected end of save data"); }
    memcpy(dst, p, n); p += n; return true;
  }
  size_t left() const { return (size_t)(end - p); }
  uint8_t u8() { uint8_t v; bytes(&v, 1); return v; }
  uint16_t u16() { uint16_t v; bytes(&v, 2); return v; }
  uint32_t u32() { uint32_t v; bytes(&v, 4); return v; }
  int32_t i32() { int32_t v; bytes(&v, 4); return v; }
  uint64_t u64() { uint64_t v; bytes(&v, 8); return v; }
  float f32() { float v; bytes(&v, 4); return v; }
  double f64() { double v; bytes(&v, 8); return v; }
  vec3 v3() { vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
  ivec3 iv3() { ivec3 v; v.x = i32(); v.y = i32(); v.z = i32(); return v; }
  std::string str(uint32_t maxLen = 4096) {
    uint32_t n = u32();
    if (!ok) return {};
    if (n > maxLen || n > left()) { fail("string too long in save data"); return {}; }
    std::string s((const char*)p, n); p += n; return s;
  }
  bool tag(const char* t) {
    char b[4];
    if (!bytes(b, 4)) return false;
    return memcmp(b, t, 4) == 0 ? true : fail(std::string("save section '") + t + "' is missing or out of order");
  }
  // A count that must fit in the remaining bytes at `minBytesEach` per item.
  uint32_t count(uint32_t maxN, size_t minBytesEach, const char* what) {
    uint32_t n = u32();
    if (ok && (n > maxN || (size_t)n * minBytesEach > left())) { fail(std::string("implausible ") + what + " count in save data"); return 0; }
    return n;
  }
};

// Material ids as stored in a file, mapped to this build's Mat enum by name.
struct MatMap {
  uint8_t to[256];
  bool identity = true;
  uint8_t operator()(uint8_t m) const { return to[m]; }
};

// Simulation state. `readSim` validates the settings, materials and parts
// first and only then re-initialises `s` (setting `replaced`) and fills it in.
// On failure `r.err` says why; if `replaced` is set the live world is gone and
// the caller must rebuild one.
void writeSim(Writer& w, const Sim& s);
bool readSim(Reader& r, Sim& s, MatMap& mats, bool& replaced);

// Pieces the game layer reuses for its own section.
void writeComponent(Writer& w, const Component& c);
Component readComponent(Reader& r);
void writeVolume(Writer& w, const Volume& v);
bool readVolume(Reader& r, Volume& v, const MatMap& mats, uint32_t maxComp);

// Whole-file I/O. `header` is a JSON object; `writeFile` replaces the file
// atomically. `readFile` checks magic, format, sizes and the checksum before
// returning the payload.
bool writeFile(const std::string& path, const std::string& header, const std::vector<uint8_t>& payload, std::string& err);
bool readFile(const std::string& path, std::string& header, std::vector<uint8_t>& payload, std::string& err);
// Just the JSON header, without reading or checking the payload (for listings).
bool readHeader(const std::string& path, std::string& header);

// Hash of everything that defines the simulated state (world and body voxels,
// body transforms and velocities, components, the air). Used to prove a save/load
// round-trip is exact and, later, that two replays agree.
// With `parts`, also lists the running hash after each section (world, each
// body, parts, air) so a mismatch can be located.
uint64_t stateHash(const Sim& s, std::vector<std::pair<std::string, uint64_t>>* parts = nullptr);

}  // namespace save
