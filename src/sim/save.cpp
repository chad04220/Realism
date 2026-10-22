#include "save.h"
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include "../core/jobs.h"

namespace fs = std::filesystem;

namespace save {

struct Access {
  static float* clocks(Sim& s) { return &s.thermalAcc_; }
  static const float* clocks(const Sim& s) { return &s.thermalAcc_; }
  static AirSim& air(Sim& s) { return s.air; }
  static std::vector<float>* airFields(AirSim& a, int i) {
    std::vector<float>* f[] = {&a.u, &a.v, &a.w};
    return f[i];
  }
  static const std::vector<float>* airFields(const AirSim& a, int i) {
    const std::vector<float>* f[] = {&a.u, &a.v, &a.w};
    return f[i];
  }
  static float& airTime(AirSim& a) { return a.time_; }
  static float airTime(const AirSim& a) { return a.time_; }
};

namespace {

constexpr int N_CLOCKS = 4;  // thermalAcc_, fluidAcc_, electricAcc_, airAcc_ (declared in that order)

// ------------------------------------------------------------------ helpers

uint64_t fnv(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
  return h;
}
constexpr uint64_t FNV0 = 0xcbf29ce484222325ull;

bool finite3(const vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Run-length encoding of one brick field: (u16 run, value) pairs covering BS3 cells.
template <class T>
void writeRuns(Writer& w, const T* a) {
  int i = 0;
  while (i < BS3) {
    int j = i + 1;
    while (j < BS3 && memcmp(&a[j], &a[i], sizeof(T)) == 0) j++;
    w.u16((uint16_t)(j - i));
    w.bytes(&a[i], sizeof(T));
    i = j;
  }
}
template <class T>
bool readRuns(Reader& r, T* a) {
  int i = 0;
  while (i < BS3 && r.ok) {
    int n = r.u16();
    T v;
    r.bytes(&v, sizeof(T));
    if (n <= 0 || i + n > BS3) return r.fail("corrupt voxel run in save data");
    for (int k = 0; k < n; k++) a[i + k] = v;
    i += n;
  }
  return r.ok;
}

// Float arrays of any length (the air), same scheme with u32 runs.
void writeFloatRuns(Writer& w, const std::vector<float>& a) {
  w.u32((uint32_t)a.size());
  size_t i = 0;
  while (i < a.size()) {
    size_t j = i + 1;
    while (j < a.size() && memcmp(&a[j], &a[i], 4) == 0) j++;
    w.u32((uint32_t)(j - i));
    w.f32(a[i]);
    i = j;
  }
}
bool readFloatRuns(Reader& r, std::vector<float>& a) {
  uint32_t n = r.u32();
  if (!r.ok) return false;
  if (n != a.size()) return r.fail("air grid size in save does not match the world");
  size_t i = 0;
  while (i < n && r.ok) {
    uint32_t run = r.u32();
    float v = r.f32();
    if (run == 0 || i + run > n || !std::isfinite(v)) return r.fail("corrupt air data in save");
    std::fill(a.begin() + i, a.begin() + i + run, v);
    i += run;
  }
  return r.ok;
}

// Joint names are referenced by pointer from Component. Known joints map back
// to their constants; anything else is interned at a stable address.
Joint internJoint(const std::string& name, float energy) {
  static const Joint known[] = {joints::welded, joints::bolted, joints::pressfit, joints::mortar, joints::nailed,
                                joints::screwed, joints::glued, joints::clipped, joints::grown, joints::cast,
                                joints::terrain, joints::resting};
  for (const Joint& j : known)
    if (name == j.name) return Joint{j.name, energy};
  static std::deque<std::string> names;
  for (const std::string& n : names)
    if (n == name) return Joint{n.c_str(), energy};
  names.push_back(name);
  return Joint{names.back().c_str(), energy};
}

void writeMatTable(Writer& w) {
  w.tag("MATS");
  w.u32(MAT_COUNT);
  for (int m = 0; m < MAT_COUNT; m++) w.str(MATS[m].name);
}

bool readMatTable(Reader& r, MatMap& mm) {
  if (!r.tag("MATS")) return false;
  uint32_t n = r.count(256, 5, "material");
  for (int i = 0; i < 256; i++) mm.to[i] = AIR;
  mm.identity = n == MAT_COUNT;
  for (uint32_t i = 0; i < n && r.ok; i++) {
    std::string name = r.str(128);
    int found = -1;
    for (int m = 0; m < MAT_COUNT; m++) if (name == MATS[m].name) { found = m; break; }
    if (found < 0) return r.fail("save uses a material this version doesn't know: " + name);
    mm.to[i] = (uint8_t)found;
    if (found != (int)i) mm.identity = false;
  }
  return r.ok;
}

void remapCellMats(uint8_t* mat, uint8_t* aux, int n, const MatMap& mm) {
  if (mm.identity) return;
  for (int i = 0; i < n; i++) {
    mat[i] = mm(mat[i]);
    if (mat[i] == MOLTEN) aux[i] = mm(aux[i]);  // molten remembers what it was
  }
}

void writeMat3(Writer& w, const mat3& m) { for (float f : m.m) w.f32(f); }
mat3 readMat3(Reader& r) { mat3 m; for (float& f : m.m) f = r.f32(); return m; }

// Bricks with thermal activity. Derivable from temperatures only approximately
// (a brick that cooled back to ambient may still be on the list), so it is
// stored as is.
void writeHot(Writer& w, const HotSet& h) {
  w.u32((uint32_t)h.list.size());
  for (int b : h.list) w.i32(b);
}
void readHot(Reader& r, HotSet& h, size_t bricks) {
  h.reset(bricks);
  uint32_t n = r.count((uint32_t)bricks, 4, "active brick");
  for (uint32_t i = 0; i < n && r.ok; i++) h.add(r.i32());
}

void writeDebris(Writer& w, const Debris& d) {
  w.v3(d.p); w.v3(d.v); w.u8(d.mat); w.f32(d.temp); w.f32(d.life); w.u32(d.comp); w.u8(d.bounces); w.u8(d.aux); w.f32(d.fill);
}

}  // namespace

// --------------------------------------------------------------- components

void writeComponent(Writer& w, const Component& c) {
  w.str(c.name); w.str(c.object); w.u8(c.type);
  w.str(c.joint.name ? c.joint.name : ""); w.f32(c.joint.energy);
  w.v3(c.axis); w.u8(c.reversed); w.f32(c.capacity); w.f32(c.charge); w.f32(c.fuel); w.u8(c.lit); w.f32(c.fuse);
  w.u8(c.powered); w.u8(c.shorted); w.f32(c.powerFactor); w.f32(c.drawW);
  w.f32(c.spin); w.f32(c.spinRate); w.f32(c.driveSpeed); w.f32(c.driveForce);
}

Component readComponent(Reader& r) {
  Component c;
  c.name = r.str(256); c.object = r.str(256);
  uint8_t t = r.u8();
  if (t >= C_TYPE_COUNT) { r.fail("unknown part type in save"); return c; }
  c.type = (CompType)t;
  std::string jn = r.str(64);
  float je = r.f32();
  c.joint = internJoint(jn, je);
  c.axis = r.v3(); c.reversed = r.u8() != 0; c.capacity = r.f32(); c.charge = r.f32(); c.fuel = r.f32(); c.lit = r.u8() != 0; c.fuse = r.f32();
  c.powered = r.u8() != 0; c.shorted = r.u8() != 0; c.powerFactor = r.f32(); c.drawW = r.f32();
  c.spin = r.f32(); c.spinRate = r.f32(); c.driveSpeed = r.f32(); c.driveForce = r.f32();
  if (r.ok && (!finite3(c.axis) || !std::isfinite(c.capacity) || !std::isfinite(c.charge) || !std::isfinite(c.fuel) ||
               !std::isfinite(c.fuse) || !std::isfinite(c.spin) || !std::isfinite(c.spinRate)))
    r.fail("part '" + c.name + "' has non-finite values");
  return c;
}

// ------------------------------------------------------------------ volumes

void writeVolume(Writer& w, const Volume& v) {
  w.tag("VOL ");
  w.i32(v.nx); w.i32(v.ny); w.i32(v.nz); w.u8(v.solidBelow);
  std::vector<uint32_t> live;
  for (size_t i = 0; i < v.bricks.size(); i++) if (v.bricks[i] && v.bricks[i]->count) live.push_back((uint32_t)i);
  w.u32((uint32_t)live.size());
  auto encode = [&](Writer& out, uint32_t i) {
    const Brick* b = v.bricks[i].get();
    out.u32(i);
    writeRuns(out, b->mat); writeRuns(out, b->flags); writeRuns(out, b->hp); writeRuns(out, b->aux);
    writeRuns(out, b->comp); writeRuns(out, b->temp); writeRuns(out, b->fuel);
  };
  if (live.size() < 256) {  // bodies, templates: not worth the threads
    for (uint32_t i : live) encode(w, i);
    return;
  }
  // The world: thousands of bricks, encoded in parallel chunks and joined in
  // order, so the file is identical to a serial encode.
  const int chunk = 64, nChunks = (int)((live.size() + chunk - 1) / chunk);
  std::vector<Writer> parts(nChunks);
  Jobs::get().parallelFor(nChunks, 1, [&](int c0, int c1, int) {
    for (int c = c0; c < c1; c++)
      for (size_t k = (size_t)c * chunk; k < std::min(live.size(), (size_t)(c + 1) * chunk); k++) encode(parts[c], live[k]);
  });
  size_t total = 0;
  for (auto& p : parts) total += p.buf.size();
  w.buf.reserve(w.buf.size() + total);
  for (auto& p : parts) w.buf.insert(w.buf.end(), p.buf.begin(), p.buf.end());
}

bool readVolume(Reader& r, Volume& v, const MatMap& mats, uint32_t maxComp) {
  if (!r.tag("VOL ")) return false;
  int nx = r.i32(), ny = r.i32(), nz = r.i32();
  bool solidBelow = r.u8() != 0;
  if (!r.ok) return false;
  if (nx < 0 || ny < 0 || nz < 0 || nx > 8192 || ny > 8192 || nz > 8192 || (double)nx * ny * nz > 1.2e9)
    return r.fail("implausible volume size in save");
  if (v.nx != nx || v.ny != ny || v.nz != nz) v.resize(nx, ny, nz);
  else { for (auto& b : v.bricks) b.reset(); }
  v.solidBelow = solidBelow;
  uint32_t n = r.count((uint32_t)v.bricks.size(), 7 * 3, "brick");
  for (uint32_t k = 0; k < n && r.ok; k++) {
    uint32_t i = r.u32();
    if (i >= v.bricks.size() || v.bricks[i]) return r.fail("bad brick index in save");
    auto b = std::make_unique<Brick>();
    if (!readRuns(r, b->mat) || !readRuns(r, b->flags) || !readRuns(r, b->hp) || !readRuns(r, b->aux) ||
        !readRuns(r, b->comp) || !readRuns(r, b->temp) || !readRuns(r, b->fuel))
      return false;
    remapCellMats(b->mat, b->aux, BS3, mats);
    int count = 0;
    for (int l = 0; l < BS3; l++) {
      if (!b->mat[l]) continue;
      count++;
      if (b->comp[l] >= maxComp) return r.fail("voxel refers to a part that isn't in the save");
      if (!std::isfinite(b->temp[l]) || !std::isfinite(b->fuel[l])) return r.fail("voxel has non-finite temperature or fill");
      if (b->hp[l] == 0) b->hp[l] = 1;  // hp 0 is never valid
    }
    b->count = count;
    if (count) v.bricks[i] = std::move(b);
  }
  if (!r.ok) return false;
  v.markAllDirty();
  for (size_t i = 0; i < v.bricks.size(); i++) if (v.bricks[i]) { v.shapeFlag[i] = 1; v.shapeList.push_back((int)i); }
  v.version++;
  return true;
}

// --------------------------------------------------------------- simulation

void writeSim(Writer& w, const Sim& s) {
  w.tag("SIM ");
  w.f32(s.cfg.voxelSize); w.v3(s.cfg.extent); w.f32(s.cfg.thermalWarp);
  writeMatTable(w);

  w.tag("COMP");
  const auto& cs = comps();
  w.u32((uint32_t)cs.size());
  for (size_t i = 1; i < cs.size(); i++) writeComponent(w, cs[i]);

  w.tag("STAT");
  w.f32(s.time); w.f32(s.timeScale); w.v3(s.gravity); w.v3(s.sunDir); w.v3(s.wind); w.i32(s.nextBodyId);
  const float* clk = Access::clocks(s);
  for (int i = 0; i < N_CLOCKS; i++) w.f32(clk[i]);

  writeVolume(w, s.world);

  // Placement ghosts and the tools in the player's hands are presentation;
  // the game rebuilds them after a load.
  w.tag("BODY");
  uint32_t nb = 0;
  for (auto& bp : s.bodies) if (!bp->ghost && !bp->viewmodel) nb++;
  w.u32(nb);
  for (auto& bp : s.bodies) {
    const Body& b = *bp;
    if (b.ghost || b.viewmodel) continue;
    w.i32(b.id);
    uint8_t f = (b.frozen ? 1 : 0) | (b.ghost ? 2 : 0) | (b.viewmodel ? 4 : 0) | (b.hidden ? 8 : 0) | (b.held ? 16 : 0) |
                (b.asleep ? 32 : 0) | (b.splitCheck ? 64 : 0);
    w.u8(f);
    w.v3(b.com); w.v3(b.pos); w.f32(b.rot.x); w.f32(b.rot.y); w.f32(b.rot.z); w.f32(b.rot.w);
    w.v3(b.vel); w.v3(b.angVel); w.f32(b.mass);
    w.f32(b.sleepTimer); w.f32(b.age); w.f32(b.restTime); w.i32(b.lastHit); w.f32(b.submerged);
    w.v3(b.force); w.v3(b.torque);
    writeVolume(w, b.vol);
    // Derived data is saved rather than recomputed: a body whose voxels just
    // changed keeps its old mass properties until the next step rebuilds them,
    // and a load must reproduce that exactly.
    w.u8(b.shapeDirty);
    w.f32(b.invMass); writeMat3(w, b.invIlocal); writeMat3(w, b.invIworld); w.f32(b.radius); w.u64(b.voxels);
    w.u32((uint32_t)b.samples.size());
    for (auto& sp : b.samples) { w.v3(sp.local); w.f32(sp.r); w.iv3(sp.cell); w.u32(sp.comp); w.u8(sp.mat); }
    w.u32((uint32_t)b.rot_parts.size());
    for (auto& rp : b.rot_parts) { w.u32(rp.comp); w.v3(rp.pivot); w.v3(rp.axis); w.f32(rp.radius); }
    w.u32((uint32_t)b.compIds.size());
    for (uint32_t c : b.compIds) w.u32(c);
    writeHot(w, b.heat);
  }

  w.tag("DEBR");
  w.u32((uint32_t)s.debris.size());
  for (auto& d : s.debris) writeDebris(w, d);

  w.tag("HOT ");
  writeHot(w, s.worldHot);
  w.u32((uint32_t)s.worldSpecials.size());
  for (int64_t k : s.worldSpecials) w.u64((uint64_t)k);

  w.tag("FLUI");
  w.u32((uint32_t)s.fluidActive.size());
  for (int64_t k : s.fluidActive) w.u64((uint64_t)k);
  w.u32((uint32_t)s.supportSeeds.size());
  for (int64_t k : s.supportSeeds) w.u64((uint64_t)k);

  w.tag("AIR ");
  w.u8(s.airReady);
  if (s.airReady) {
    const AirSim& a = s.air;
    w.i32(a.nx); w.i32(a.ny); w.i32(a.nz);
    w.v3(a.wind); w.f32(a.gust); w.f32(Access::airTime(a));
    for (int i = 0; i < 3; i++) writeFloatRuns(w, *Access::airFields(a, i));
    writeFloatRuns(w, a.T); writeFloatRuns(w, a.smk); writeFloatRuns(w, a.stm);
  }
}

bool readSim(Reader& r, Sim& s, MatMap& mats, bool& replaced) {
  replaced = false;
  if (!r.tag("SIM ")) return false;
  SimSettings cfg = s.cfg;
  cfg.voxelSize = r.f32(); cfg.extent = r.v3(); cfg.thermalWarp = r.f32();
  if (!r.ok) return false;
  if (!(cfg.voxelSize >= 0.0199f && cfg.voxelSize <= 0.0251f)) return r.fail("save uses an unsupported voxel size");
  if (!finite3(cfg.extent) || cfg.extent.x < 1 || cfg.extent.y < 1 || cfg.extent.z < 1 || cfg.extent.x > 64 ||
      cfg.extent.y > 32 || cfg.extent.z > 64)
    return r.fail("save has an implausible world size");
  double cells = (double)(cfg.extent.x / cfg.voxelSize) * (cfg.extent.y / cfg.voxelSize) * (cfg.extent.z / cfg.voxelSize);
  if (cells > 400e6) return r.fail("save's world is too large to load");
  if (!std::isfinite(cfg.thermalWarp) || cfg.thermalWarp < 1 || cfg.thermalWarp > 60) cfg.thermalWarp = 20;
  if (!readMatTable(r, mats)) return false;

  if (!r.tag("COMP")) return false;
  uint32_t nc = r.count(16u << 20, 40, "part");
  if (!r.ok) return false;
  std::vector<Component> parts;
  parts.reserve(nc ? nc - 1 : 0);
  for (uint32_t i = 1; i < nc && r.ok; i++) parts.push_back(readComponent(r));
  if (!r.ok) return false;

  // From here on the live simulation is replaced.
  replaced = true;
  s.init(cfg);
  auto& cs = comps();
  cs.clear();
  cs.emplace_back();  // id 0 = no component
  for (auto& c : parts) cs.push_back(std::move(c));
  const uint32_t maxComp = (uint32_t)cs.size();

  if (!r.tag("STAT")) return false;
  s.time = r.f32(); s.timeScale = r.f32(); s.gravity = r.v3(); s.sunDir = r.v3(); s.wind = r.v3();
  int nextId = r.i32();
  float* clk = Access::clocks(s);
  for (int i = 0; i < N_CLOCKS; i++) clk[i] = r.f32();
  if (r.ok && (!std::isfinite(s.time) || !finite3(s.gravity) || !finite3(s.wind) || !finite3(s.sunDir)))
    return r.fail("save has non-finite world settings");
  if (!(s.timeScale > 0 && s.timeScale <= 1)) s.timeScale = 1;

  if (!readVolume(r, s.world, mats, maxComp)) return false;
  if (s.world.nx != (int)s.world.bx * BS || s.world.solidBelow == false) return r.fail("save's world grid is malformed");
  // Saves from 2.1 and earlier stored water as fractional fills (fuel > 0).
  // Water is whole voxels now: round them, carrying the remainder from cell
  // to cell so the total stays within one voxel of what was saved.
  float waterCarry = 0;
  std::vector<ivec3> drained;
  s.world.forEach([&](int x, int y, int z, Brick* br, int l) {
    if (br->mat[l] != WATER || br->fuel[l] <= 0) return;
    waterCarry += br->fuel[l];
    br->fuel[l] = 0;
    if (waterCarry >= 0.5f) waterCarry -= 1;
    else drained.push_back(ivec3(x, y, z));
  });
  for (auto& c : drained) s.world.set(c.x, c.y, c.z, AIR);

  if (!r.tag("BODY")) return false;
  uint32_t nb = r.count(1u << 20, 100, "body");
  int maxId = 0;
  for (uint32_t i = 0; i < nb && r.ok; i++) {
    int id = r.i32();
    uint8_t f = r.u8();
    vec3 com = r.v3(), pos = r.v3();
    quat q; q.x = r.f32(); q.y = r.f32(); q.z = r.f32(); q.w = r.f32();
    vec3 vel = r.v3(), angVel = r.v3();
    float mass = r.f32();
    float sleepTimer = r.f32(), age = r.f32(), restTime = r.f32();
    int lastHit = r.i32();
    float submerged = r.f32();
    vec3 force = r.v3(), torque = r.v3();
    if (!r.ok) return false;
    if (id <= 0 || s.bodyById(id)) return r.fail("duplicate or invalid body id in save");
    if (!finite3(com) || !finite3(pos) || !finite3(vel) || !finite3(angVel) || !std::isfinite(mass) || mass < 0 ||
        !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
      return r.fail("a body in the save has non-finite motion");
    Body* b = s.newBody();
    b->id = id;
    maxId = std::max(maxId, id);
    b->frozen = f & 1; b->ghost = f & 2; b->viewmodel = f & 4; b->hidden = f & 8; b->held = f & 16;
    b->asleep = f & 32;
    b->com = com; b->pos = pos;
    float qn = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    // Keep the stored bits unless the rotation is actually malformed.
    b->rot = std::fabs(qn - 1) < 1e-3f ? q : qn > 1e-6f ? quat(q.x / qn, q.y / qn, q.z / qn, q.w / qn) : quat();
    b->vel = vel; b->angVel = angVel;
    b->sleepTimer = sleepTimer; b->age = age; b->restTime = restTime; b->lastHit = lastHit; b->submerged = submerged;
    b->force = force; b->torque = torque;
    if (!readVolume(r, b->vol, mats, maxComp)) return false;
    b->mass = mass;
    b->splitCheck = f & 64;
    b->shapeDirty = r.u8() != 0;
    b->invMass = r.f32(); b->invIlocal = readMat3(r); b->invIworld = readMat3(r); b->radius = r.f32();
    b->voxels = (size_t)r.u64();
    uint32_t nsmp = r.count(1u << 22, 33, "collision sample");
    b->samples.resize(nsmp);
    for (auto& sp : b->samples) {
      sp.local = r.v3(); sp.r = r.f32(); sp.cell = r.iv3(); sp.comp = r.u32(); sp.mat = mats(r.u8());
      if (sp.comp >= maxComp) sp.comp = 0;
    }
    uint32_t nrp = r.count(1u << 16, 32, "rotating part");
    b->rot_parts.resize(nrp);
    for (auto& rp : b->rot_parts) {
      rp.comp = r.u32(); rp.pivot = r.v3(); rp.axis = r.v3(); rp.radius = r.f32();
      if (rp.comp >= maxComp) rp.comp = 0;
    }
    uint32_t nci = r.count(1u << 22, 4, "body part");
    b->compIds.resize(nci);
    for (uint32_t& c : b->compIds) { c = r.u32(); if (c >= maxComp) c = 0; }
    readHot(r, b->heat, b->vol.bricks.size());
    if (!r.ok) return false;
    if (!std::isfinite(b->invMass) || !std::isfinite(b->radius) || b->voxels > (size_t)b->vol.nx * b->vol.ny * b->vol.nz)
      return r.fail("a body in the save has corrupt mass properties");
  }
  if (!r.ok) return false;
  s.nextBodyId = std::max(nextId, maxId + 1);

  if (!r.tag("DEBR")) return false;
  uint32_t nd = r.count(1u << 20, 40, "debris");
  s.debris.reserve(nd);
  for (uint32_t i = 0; i < nd && r.ok; i++) {
    Debris d;
    d.p = r.v3(); d.v = r.v3(); d.mat = mats(r.u8()); d.temp = r.f32(); d.life = r.f32(); d.comp = r.u32();
    d.bounces = r.u8(); d.aux = r.u8(); d.fill = r.f32();
    if (d.mat == MOLTEN) d.aux = mats(d.aux);
    if (!r.ok) break;
    if (!finite3(d.p) || !finite3(d.v) || !std::isfinite(d.temp) || !std::isfinite(d.fill)) return r.fail("debris has non-finite values");
    if (d.comp >= maxComp) d.comp = 0;
    if (d.mat == WATER) {  // 2.1 droplets could carry part of a voxel
      waterCarry += d.fill;
      d.fill = 1;
      if (waterCarry < 0.5f) continue;
      waterCarry -= 1;
    }
    s.debris.push_back(d);
  }

  if (!r.tag("HOT ")) return false;
  readHot(r, s.worldHot, s.world.bricks.size());
  uint32_t nsp = r.count(1u << 26, 8, "electrical cell");
  for (uint32_t i = 0; i < nsp && r.ok; i++) s.worldSpecials.insert((int64_t)r.u64());

  if (!r.tag("FLUI")) return false;
  uint32_t nf = r.count(1u << 26, 8, "fluid cell");
  for (uint32_t i = 0; i < nf && r.ok; i++) s.fluidActive.insert((int64_t)r.u64());
  uint32_t ns = r.count(1u << 26, 8, "support seed");
  for (uint32_t i = 0; i < ns && r.ok; i++) s.supportSeeds.push_back((int64_t)r.u64());

  if (!r.tag("AIR ")) return false;
  bool air = r.u8() != 0;
  if (r.ok && air) {
    int nx = r.i32(), ny = r.i32(), nz = r.i32();
    vec3 wind = r.v3();
    float gust = r.f32(), t = r.f32();
    if (!r.ok) return false;
    s.initAir();
    AirSim& a = Access::air(s);
    if (a.nx != nx || a.ny != ny || a.nz != nz) return r.fail("air grid in save doesn't match the world");
    a.wind = finite3(wind) ? wind : a.wind;
    a.gust = std::isfinite(gust) ? gust : 0;
    Access::airTime(a) = std::isfinite(t) ? t : 0;
    for (int i = 0; i < 3; i++) if (!readFloatRuns(r, *Access::airFields(a, i))) return false;
    if (!readFloatRuns(r, a.T) || !readFloatRuns(r, a.smk) || !readFloatRuns(r, a.stm)) return false;
    a.texDirty = true;
  }
  // Local structural checks start from the last full solve's loads: work them
  // out again for the loaded world without changing anything in it.
  if (r.ok) s.checkAllStructures(false);
  return r.ok;
}

// ---------------------------------------------------------------- the file

bool writeFile(const std::string& path, const std::string& header, const std::vector<uint8_t>& payload, std::string& err) {
  Writer w;
  w.bytes("RSAV", 4);
  w.u32(FORMAT);
  w.u32((uint32_t)header.size());
  w.bytes(header.data(), header.size());
  w.u64(payload.size());
  w.buf.insert(w.buf.end(), payload.begin(), payload.end());
  w.u64(fnv(FNV0, w.buf.data(), w.buf.size()));

  std::error_code ec;
  fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::string tmp = path + ".tmp";
  {
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { err = "couldn't write " + tmp; return false; }
    bool ok = fwrite(w.buf.data(), 1, w.buf.size(), f) == w.buf.size() && fflush(f) == 0;
    // On disk, not just in the OS cache, before it replaces the old save: a
    // power cut must leave either the old file or the complete new one.
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = fclose(f) == 0 && ok;
    if (!ok) { err = "disk full or write error saving " + path; fs::remove(tmp, ec); return false; }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    // Some platforms refuse to rename over an existing file.
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) { err = "couldn't replace " + path + ": " + ec.message(); return false; }
  }
  return true;
}

bool readFile(const std::string& path, std::string& header, std::vector<uint8_t>& payload, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { err = "save not found"; return false; }
  f.seekg(0, std::ios::end);
  std::streamoff size = f.tellg();
  f.seekg(0, std::ios::beg);
  if (size < 4 + 4 + 4 + 8 + 8) { err = "save file is empty or truncated"; return false; }
  if (size > (std::streamoff)4096 * 1024 * 1024) { err = "save file is implausibly large"; return false; }
  std::vector<uint8_t> all((size_t)size);
  if (!f.read((char*)all.data(), size)) { err = "couldn't read the save file"; return false; }
  if (memcmp(all.data(), "RSAV", 4) != 0) { err = "not a Realism save"; return false; }
  Reader r(all.data() + 4, all.size() - 4);
  uint32_t format = r.u32();
  if (format == 0 || format > FORMAT) {
    err = "save was made by a newer version of Realism (format " + std::to_string(format) + ")";
    return false;
  }
  uint32_t hn = r.u32();
  if (hn > r.left()) { err = "save file is truncated"; return false; }
  header.assign((const char*)r.p, hn);
  r.p += hn;
  uint64_t pn = r.u64();
  if (!r.ok || pn + 8 != r.left()) { err = "save file is truncated or has trailing data"; return false; }
  uint64_t stored;
  memcpy(&stored, all.data() + all.size() - 8, 8);
  if (fnv(FNV0, all.data(), all.size() - 8) != stored) { err = "save file is corrupt (checksum mismatch)"; return false; }
  payload.assign(r.p, r.p + pn);
  return true;
}

bool readHeader(const std::string& path, std::string& header) {
  std::ifstream f(path, std::ios::binary);
  char head[12];
  if (!f.read(head, 12) || memcmp(head, "RSAV", 4) != 0) return false;
  uint32_t n;
  memcpy(&n, head + 8, 4);
  if (n > 65536) return false;
  header.resize(n);
  return n == 0 || (bool)f.read(header.data(), n);
}

// ----------------------------------------------------------------- hashing

uint64_t stateHash(const Sim& s, std::vector<std::pair<std::string, uint64_t>>* parts) {
  uint64_t h = FNV0;
  auto mark = [&](const std::string& what) { if (parts) parts->push_back({what, h}); };
  auto vol = [&](const Volume& v) {
    h = fnv(h, &v.nx, sizeof(int) * 3);
    v.forEach([&](int x, int y, int z, Brick* b, int l) {
      int xyz[3] = {x, y, z};
      h = fnv(h, xyz, sizeof xyz);
      h = fnv(h, &b->mat[l], 1); h = fnv(h, &b->flags[l], 1); h = fnv(h, &b->hp[l], 1); h = fnv(h, &b->aux[l], 1);
      h = fnv(h, &b->comp[l], 4); h = fnv(h, &b->temp[l], 4); h = fnv(h, &b->fuel[l], 4);
    });
  };
  vol(s.world);
  mark("world");
  for (auto& bp : s.bodies) {
    const Body& b = *bp;
    if (b.ghost || b.viewmodel) continue;
    h = fnv(h, &b.id, sizeof b.id);
    h = fnv(h, &b.pos, sizeof b.pos); h = fnv(h, &b.rot, sizeof b.rot);
    h = fnv(h, &b.vel, sizeof b.vel); h = fnv(h, &b.angVel, sizeof b.angVel);
    h = fnv(h, &b.com, sizeof b.com); h = fnv(h, &b.mass, sizeof b.mass); h = fnv(h, &b.invMass, sizeof b.invMass);
    h = fnv(h, &b.asleep, 1); h = fnv(h, &b.shapeDirty, 1);
    mark("body " + std::to_string(b.id) + " motion");
    vol(b.vol);
    mark("body " + std::to_string(b.id) + " voxels");
  }
  const auto& cs = comps();
  for (size_t i = 1; i < cs.size(); i++) {
    const Component& c = cs[i];
    h = fnv(h, c.name.data(), c.name.size());
    h = fnv(h, &c.type, 1);
    h = fnv(h, &c.charge, 4); h = fnv(h, &c.fuel, 4); h = fnv(h, &c.fuse, 4);
    uint8_t bits = (c.lit ? 1 : 0) | (c.powered ? 2 : 0) | (c.reversed ? 4 : 0);
    h = fnv(h, &bits, 1);
  }
  mark("parts");
  h = fnv(h, &s.time, sizeof s.time);
  if (s.airReady) {
    auto field = [&](const std::vector<float>& f) { h = fnv(h, f.data(), f.size() * sizeof(float)); };
    for (int i = 0; i < 3; i++) field(*Access::airFields(s.air, i));
    field(s.air.T); field(s.air.smk); field(s.air.stm);
  }
  mark("air");
  return h;
}

}  // namespace save
