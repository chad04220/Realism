#include "workshop.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include "assemblies.h"
#include "game.h"

using namespace ui;
namespace fs = std::filesystem;

namespace {
const uint8_t WS_MATS[] = {STEEL, ALU, IRON, COPPER, BRASS, SHAFT, OAK, PINE, PLASTIC, RUBBER, CARBON, GLASS,
                           BRICK, CONCRETE, STONE, CANVAS, CHARCOAL, NICHROME, CHARGE, PAINT_RED, PAINT_BLUE, PAINT_WHITE, SAND, WATER};
const int WS_MATS_N = sizeof(WS_MATS);

Joint jointFor(uint8_t m) {
  if (MATS[m].metal > 0.5f) return joints::welded;
  if (m == OAK || m == PINE) return joints::nailed;
  if (m == BRICK || m == CONCRETE || m == STONE) return joints::mortar;
  return joints::glued;
}

std::string bpDir() {
  std::string d = plat::userDataDir() + "/blueprints";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

// Mirror a template across its local X.
std::shared_ptr<Template> mirroredX(const Template& t) {
  auto m = std::make_shared<Template>();
  m->name = t.name; m->category = t.category; m->proto = t.proto;
  m->proto.axis.x = -m->proto.axis.x;
  m->vol.resize(t.vol.nx, t.vol.ny, t.vol.nz);
  t.vol.forEach([&](int x, int y, int z, Brick* br, int l) {
    Cell c; c.mat = br->mat[l]; c.comp = br->comp[l];
    m->vol.put(t.vol.nx - 1 - x, y, z, c);
  });
  return m;
}
}  // namespace

Workshop::Workshop(Game& g) : g_(g) {}

Body* Workshop::jig() { return jigId_ ? g_.sim.bodyById(jigId_) : nullptr; }

void Workshop::createJig() {
  Body* b = g_.sim.newBody();
  b->vol.resize(g_.scene.jigDims.x, g_.scene.jigDims.y, g_.scene.jigDims.z);
  b->frozen = true;
  b->asleep = true;
  b->com = vec3(0);
  b->pos = g_.scene.jigOrigin;
  b->mass = 0;
  b->heat.reset(b->vol.bricks.size());
  jigId_ = b->id;
  adoptJig(jigId_);
}

void Workshop::adoptJig(int jigId) {
  Body* j = jigId ? g_.sim.bodyById(jigId) : nullptr;
  if (!j || !j->frozen) { jigId_ = 0; createJig(); return; }
  jigId_ = jigId;
  Body* gh = g_.sim.newBody();
  gh->vol.resize(4, 4, 4);
  gh->frozen = gh->ghost = gh->asleep = true;
  gh->pos = g_.scene.jigOrigin;
  ghostId_ = gh->id;
  undo_.clear();
  redo_.clear();
  vec3 c = g_.scene.jigOrigin + vec3(g_.scene.jigDims.x, 0, g_.scene.jigDims.z) * (g_.sim.vs * 0.5f);
  target_ = tTarget_ = c + vec3(0, 0.3f, 0);
}

void Workshop::enter() {
  if (!jig()) createJig();
  vec3 c = g_.scene.jigOrigin + vec3(g_.scene.jigDims.x, 0, g_.scene.jigDims.z) * (g_.sim.vs * 0.5f);
  tTarget_ = c + vec3(0, 0.3f, 0);
  // Start the orbit from where the player is looking.
  target_ = g_.cam.pos + g_.cam.forward * 1.5f;
  dist_ = length(g_.cam.pos - target_);
  yaw_ = g_.cam.yaw;
  pitch_ = g_.cam.pitch;
  tYaw_ = 0.55f;
  tPitch_ = -0.5f;
  tDist_ = 2.8f;
  status_ = "Build anything. Parts snap to the grid; circuits work while it's in the jig.";
  statusT_ = 6;
  refreshAnalysis();
}

void Workshop::leave() {
  if (Body* gh = g_.sim.bodyById(ghostId_)) { gh->vol.resize(4, 4, 4); gh->vol.markAllDirty(); gh->vol.version++; }
  typingName_ = false;
}

std::shared_ptr<Template> Workshop::currentTemplate() {
  std::shared_ptr<Template> base;
  if (tab_ == TAB_PARTS) {
    auto& cat = partCatalogue();
    if (part_ >= 0 && part_ < (int)cat.size()) base = cat[part_].tpl;
  } else if (tab_ == TAB_SALVAGE) {
    if (salv_ >= 0 && salv_ < (int)g_.salvage.size()) base = g_.salvage[salv_];
  }
  if (!base) return nullptr;
  return rotated(*base, rotYaw_, rotPitch_);
}

bool Workshop::cursorCell(const vec3& ro, const vec3& rd, ivec3& cell, ivec3& hitCell, bool& onFloor) {
  Body* j = jig();
  if (!j) return false;
  const float vs = g_.sim.vs;
  vec3 o = (ro - g_.scene.jigOrigin) / vs;
  const Volume& v = j->vol;
  // DDA through the jig volume.
  vec3 lo(0), hi((float)v.nx, (float)v.ny, (float)v.nz);
  float t0 = 0, t1 = 1e9f;
  int entry = -1;
  for (int a = 0; a < 3; a++) {
    if (std::fabs(rd[a]) < 1e-9f) { if (o[a] < lo[a] || o[a] > hi[a]) { t1 = -1; break; } continue; }
    float ta = (lo[a] - o[a]) / rd[a], tb = (hi[a] - o[a]) / rd[a];
    if (ta > tb) std::swap(ta, tb);
    if (ta > t0) { t0 = ta; entry = a; }
    t1 = std::min(t1, tb);
  }
  if (t1 >= t0) {
    vec3 p = o + rd * (t0 + 1e-4f);
    ivec3 c = floorI(p);
    c = ivec3(std::clamp(c.x, 0, v.nx - 1), std::clamp(c.y, 0, v.ny - 1), std::clamp(c.z, 0, v.nz - 1));
    ivec3 face(0, 0, 0);
    if (entry >= 0) face[entry] = rd[entry] > 0 ? -1 : 1;
    int sx = rd.x > 0 ? 1 : -1, sy = rd.y > 0 ? 1 : -1, sz = rd.z > 0 ? 1 : -1;
    float tdx = std::fabs(1 / rd.x), tdy = std::fabs(1 / rd.y), tdz = std::fabs(1 / rd.z);
    float tmx = (sx > 0 ? c.x + 1 - o.x : o.x - c.x) * tdx, tmy = (sy > 0 ? c.y + 1 - o.y : o.y - c.y) * tdy, tmz = (sz > 0 ? c.z + 1 - o.z : o.z - c.z) * tdz;
    for (int i = 0; i < 1024; i++) {
      if (!v.inb(c.x, c.y, c.z)) break;
      if (v.matI(c) != AIR) {
        hitCell = c;
        cell = c + face;
        onFloor = false;
        return true;
      }
      if (tmx < tmy && tmx < tmz) { c.x += sx; tmx += tdx; face = {-sx, 0, 0}; }
      else if (tmy < tmz) { c.y += sy; tmy += tdy; face = {0, -sy, 0}; }
      else { c.z += sz; tmz += tdz; face = {0, 0, -sz}; }
    }
  }
  // Fall back to the jig floor.
  if (rd.y < -1e-4f && o.y > 0) {
    float t = -o.y / rd.y;
    vec3 p = o + rd * t;
    if (p.x >= 0 && p.z >= 0 && p.x < v.nx && p.z < v.nz) {
      cell = ivec3((int)p.x, 0, (int)p.z);
      hitCell = ivec3(cell.x, -1, cell.z);
      onFloor = true;
      return true;
    }
  }
  return false;
}

void Workshop::applyEdit(std::vector<Edit>& edits, const ivec3& p, const Cell& c) {
  Body* j = jig();
  if (!j || !j->vol.inb(p.x, p.y, p.z)) return;
  Cell before = j->vol.get(p.x, p.y, p.z);
  if (before.mat == c.mat && before.comp == c.comp) return;
  edits.push_back({p, before, c});
  j->vol.put(p.x, p.y, p.z, c);
}

void Workshop::afterEdit() {
  Body* j = jig();
  if (!j) return;
  g_.sim.rebuildBody(*j);
  j->pos = g_.scene.jigOrigin + j->com * g_.sim.vs;
  j->rot = quat();
  j->vel = vec3(0);
  j->angVel = vec3(0);
  if (j->heat.on.size() != j->vol.bricks.size()) j->heat.reset(j->vol.bricks.size());
  anTimer_ = 0;
}

void Workshop::commit(std::vector<Edit>&& edits) {
  if (edits.empty()) return;
  undo_.push_back({std::move(edits)});
  if (undo_.size() > 200) undo_.erase(undo_.begin());
  redo_.clear();
  afterEdit();
}

void Workshop::undo() {
  if (undo_.empty() || !jig()) return;
  Snapshot s = std::move(undo_.back());
  undo_.pop_back();
  for (auto it = s.edits.rbegin(); it != s.edits.rend(); ++it) jig()->vol.put(it->p.x, it->p.y, it->p.z, it->before);
  redo_.push_back(std::move(s));
  afterEdit();
  g_.audio.playUi(SFX_UI, 0.5f, 0.8f);
}

void Workshop::redo() {
  if (redo_.empty() || !jig()) return;
  Snapshot s = std::move(redo_.back());
  redo_.pop_back();
  for (auto& e : s.edits) jig()->vol.put(e.p.x, e.p.y, e.p.z, e.after);
  undo_.push_back(std::move(s));
  afterEdit();
  g_.audio.playUi(SFX_UI, 0.5f, 1.1f);
}

void Workshop::place(const ivec3& cell, bool erase) {
  Body* j = jig();
  if (!j) return;
  std::vector<Edit> edits;
  auto doAt = [&](const ivec3& c, bool mirrored) {
    if (erase) {
      // Erase removes the whole part under the cursor if it's a catalogue part, else a brush of voxels.
      uint32_t cid = j->vol.comp(c.x, c.y, c.z);
      if (cid && comp(cid).type != C_PART && j->vol.matI(c) != AIR) {
        j->vol.forEach([&](int x, int y, int z, Brick* br, int l) { if (br->comp[l] == cid) edits.push_back({{x, y, z}, j->vol.get(x, y, z), Cell{}}); });
        for (auto& e : edits) j->vol.set(e.p.x, e.p.y, e.p.z, AIR);
        return;
      }
      int r = brush_ - 1;
      for (int dz = -r; dz <= r; dz++) for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++)
        applyEdit(edits, c + ivec3(dx, dy, dz), Cell{});
      return;
    }
    if (tab_ == TAB_MATS) {
      uint8_t m = WS_MATS[std::clamp(mat_, 0, WS_MATS_N - 1)];
      static uint32_t strokeComp = 0;
      static double strokeTime = -1;
      double now = plat::now();
      if (strokeComp == 0 || now - strokeTime > 0.6 || comp(strokeComp).name.find(MATS[m].name) == std::string::npos) {
        Component cc;
        cc.name = std::string(MATS[m].name) + " member";
        cc.object = "Workshop build";
        cc.joint = jointFor(m);
        strokeComp = newComp(cc);
      }
      strokeTime = now;
      int r = brush_ - 1;
      for (int dz = -r; dz <= r; dz++) for (int dy = 0; dy <= 2 * r; dy++) for (int dx = -r; dx <= r; dx++) {
        ivec3 q = c + ivec3(dx, dy, dz);
        if (j->vol.inb(q.x, q.y, q.z) && j->vol.matI(q) == AIR) { Cell cc; cc.mat = m; cc.comp = strokeComp; applyEdit(edits, q, cc); }
      }
      (void)mirrored;
      return;
    }
    auto t = currentTemplate();
    if (!t) return;
    if (mirrored) t = mirroredX(*t);
    ivec3 o(c.x - t->vol.nx / 2, c.y, c.z - t->vol.nz / 2);
    // Validate: inside the jig and not overlapping.
    bool ok = true;
    t->vol.forEach([&](int x, int y, int z, Brick*, int) {
      ivec3 q = o + ivec3(x, y, z);
      if (!j->vol.inb(q.x, q.y, q.z) || j->vol.matI(q) != AIR) ok = false;
    });
    if (!ok) { status_ = "Doesn't fit there"; statusT_ = 1.5f; return; }
    Component pc = t->proto;
    pc.object = "Workshop build";
    if (pc.type == C_BATTERY) pc.charge = pc.capacity > 0 ? pc.capacity : 100;
    if (pc.type == C_ROCKET && pc.fuel <= 0) pc.fuel = 6;
    pc.lit = false; pc.fuse = -1;
    uint32_t id = newComp(pc);
    t->vol.forEach([&](int x, int y, int z, Brick* br, int l) {
      Cell cc;
      cc.mat = br->mat[l];
      cc.comp = id;
      applyEdit(edits, o + ivec3(x, y, z), cc);
    });
    if (tab_ == TAB_SALVAGE && !mirrored && salv_ < (int)g_.salvage.size()) {
      g_.salvage.erase(g_.salvage.begin() + salv_);
      salv_ = std::max(0, std::min(salv_, (int)g_.salvage.size() - 1));
    }
  };
  doAt(cell, false);
  if (mirror_) {
    ivec3 mc(j->vol.nx - 1 - cell.x, cell.y, cell.z);
    if (mc != cell) doAt(mc, true);
  }
  if (!edits.empty()) {
    commit(std::move(edits));
    g_.audio.playUi(erase ? SFX_POP : SFX_CLICK, 0.5f, erase ? 1.2f : 0.9f);
  }
}

void Workshop::paint(const ivec3& c) {
  Body* j = jig();
  if (!j || j->vol.matI(c) == AIR) return;
  std::vector<Edit> edits;
  Cell cc = j->vol.get(c.x, c.y, c.z);
  if (MATS[cc.mat].wire || isSpecialElectric(cc.mat)) { status_ = "Can't repaint electrical parts"; statusT_ = 1.5f; return; }
  cc.mat = WS_MATS[mat_];
  applyEdit(edits, c, cc);
  commit(std::move(edits));
}

void Workshop::wireClick(const ivec3& c) {
  Body* j = jig();
  if (!j) return;
  uint8_t m = j->vol.matI(c);
  if (!MATS[m].wire) {
    status_ = "Click a conductor: a battery terminal, a copper lead or tab, a switch or an existing wire";
    statusT_ = 3;
    return;
  }
  if (!wireStarted_) {
    wireStarted_ = true;
    wireStart_ = c;
    status_ = std::string("Wire from ") + MATS[m].name + " \xE2\x86\x92 now click where it should go";
    statusT_ = 6;
    g_.audio.playUi(SFX_CLICK, 0.5f, 1.2f);
    return;
  }
  wireStarted_ = false;
  if (c == wireStart_) { status_ = "Wire cancelled"; statusT_ = 2; return; }
  Component wc;
  wc.name = "Copper wire";
  wc.object = "Workshop build";
  wc.joint = joints::glued;
  uint32_t id = newComp(wc);
  std::vector<ivec3> laid;
  if (!routeWire(j->vol, wireStart_, c, id, COPPER, &laid)) {
    status_ = "No clear route: wires never touch other conductors (that would short). Move parts apart or wire by hand.";
    statusT_ = 5;
    return;
  }
  std::vector<Edit> edits;
  for (auto& p : laid) {
    Cell after = j->vol.get(p.x, p.y, p.z);
    edits.push_back({p, Cell{}, after});
  }
  commit(std::move(edits));
  char b[96];
  snprintf(b, sizeof b, "Wired: %.0f cm of copper", laid.size() * g_.sim.vs * 100);
  status_ = b;
  statusT_ = 3;
  g_.audio.playUi(SFX_CLICK, 0.6f, 0.8f);
}

void Workshop::pick(const ivec3& c) {
  Body* j = jig();
  if (!j) return;
  uint8_t m = j->vol.matI(c);
  for (int i = 0; i < WS_MATS_N; i++) if (WS_MATS[i] == m) { mat_ = i; tab_ = TAB_MATS; tool_ = E_PLACE; }
}

void Workshop::clearJig() {
  Body* j = jig();
  if (!j) return;
  std::vector<Edit> edits;
  j->vol.forEach([&](int x, int y, int z, Brick*, int) { edits.push_back({{x, y, z}, j->vol.get(x, y, z), Cell{}}); });
  for (auto& e : edits) j->vol.set(e.p.x, e.p.y, e.p.z, AIR);
  commit(std::move(edits));
}

void Workshop::launch() {
  Body* j = jig();
  if (!j || j->voxels == 0) { status_ = "Nothing to launch yet"; statusT_ = 2; return; }
  saveBlueprint("Last launch");
  j->frozen = false;
  j->mass = 0;
  g_.sim.rebuildBody(*j);
  j->pos = g_.scene.jigOrigin + j->com * g_.sim.vs + vec3(0, 0.01f, 0);
  j->updateInertiaWorld();
  j->wake();
  j->splitCheck = true;
  std::string name = an_.pieces > 1 ? "Launched (" + std::to_string(an_.pieces) + " separate pieces)" : "Launched!";
  g_.toast(name, ui::theme::accent, I_PLAY);
  g_.audio.playUi(SFX_POP, 0.8f, 0.7f);
  if (Body* gh = g_.sim.bodyById(ghostId_)) { int i = g_.sim.bodyIndex(gh); if (i >= 0) g_.sim.removeBody(i); }
  jigId_ = 0;
  createJig();
  // Back to first person, standing at the pad looking at the launch.
  g_.mode_ = Game::M_PLAY;
  vec3 c = g_.scene.jigOrigin + vec3(g_.scene.jigDims.x, 0, g_.scene.jigDims.z) * (g_.sim.vs * 0.5f);
  g_.pl_.pos = c + vec3(0, 0, 2.4f);
  g_.pl_.pos.y = g_.scene.groundY + 0.06f;
  g_.pl_.yaw = 0;
  g_.pl_.pitch = -0.35f;
  g_.pl_.vel = vec3(0);
}

// ------------------------------------------------------------- blueprints

void Workshop::saveBlueprint(const std::string& name) {
  Body* j = jig();
  if (!j || j->voxels == 0) return;
  std::string path = bpDir() + "/" + name + ".rbp";
  std::ofstream f(path, std::ios::binary);
  if (!f) { status_ = "Couldn't save " + path; statusT_ = 3; return; }
  std::vector<uint32_t> ids;
  struct Rec { uint16_t x, y, z; uint8_t m; uint16_t c; };
  std::vector<Rec> recs;
  j->vol.forEach([&](int x, int y, int z, Brick* br, int l) {
    uint32_t c = br->comp[l];
    auto it = std::find(ids.begin(), ids.end(), c);
    uint16_t ci = (uint16_t)(it - ids.begin());
    if (it == ids.end()) ids.push_back(c);
    recs.push_back({(uint16_t)x, (uint16_t)y, (uint16_t)z, br->mat[l], ci});
  });
  auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
  auto wf = [&](float v) { f.write((const char*)&v, 4); };
  auto ws = [&](const std::string& s) { w32((uint32_t)s.size()); f.write(s.data(), s.size()); };
  f.write("RBP1", 4);
  wf(g_.sim.vs);
  w32((uint32_t)ids.size());
  for (uint32_t id : ids) {
    const Component& c = comp(id);
    ws(c.name); ws(c.object); w32(c.type); ws(c.joint.name); wf(c.joint.energy);
    wf(c.axis.x); wf(c.axis.y); wf(c.axis.z); w32(c.reversed); wf(c.capacity); wf(c.fuel);
  }
  w32((uint32_t)recs.size());
  for (auto& r : recs) f.write((const char*)&r, sizeof r);
  if (name != "Last launch") { status_ = "Saved blueprint \"" + name + "\""; statusT_ = 3; }
}

std::vector<std::string> Workshop::blueprintNames() const {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(bpDir(), ec))
    if (e.path().extension() == ".rbp") out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}

bool Workshop::loadBlueprint(const std::string& name) {
  Body* j = jig();
  if (!j) { createJig(); j = jig(); }
  // Clear (without undo history: a blueprint load is a fresh start).
  std::vector<ivec3> cells;
  j->vol.forEach([&](int x, int y, int z, Brick*, int) { cells.push_back({x, y, z}); });
  for (auto& c : cells) j->vol.set(c.x, c.y, c.z, AIR);
  undo_.clear(); redo_.clear();
  const float vs = g_.sim.vs;
  if (const Assembly* a = findAssembly(name)) {
    vec3 o((j->vol.nx * vs - a->size.x) * 0.5f, 0, (j->vol.nz * vs - a->size.z) * 0.5f);
    o = vec3(std::floor(o.x / vs) * vs, 0, std::floor(o.z / vs) * vs);
    bool ok = a->build(j->vol, o);
    afterEdit();
    status_ = ok ? "Loaded " + name + ". Point at its switch and press E to test it, then launch." : name + " loaded, but a wire couldn't be routed";
    statusT_ = 4;
    return true;
  }
  // Never let a name escape the blueprint directory, and check the file size
  // before trusting any count read out of it.
  if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name.find("..") != std::string::npos) {
    status_ = "Invalid blueprint name";
    statusT_ = 2;
    return false;
  }
  std::ifstream f(bpDir() + "/" + name + ".rbp", std::ios::binary);
  if (!f) { status_ = "Blueprint not found"; statusT_ = 2; return false; }
  f.seekg(0, std::ios::end);
  const std::streamoff fileBytes = f.tellg();
  f.seekg(0, std::ios::beg);
  if (fileBytes < 8 || fileBytes > (std::streamoff)64 * 1024 * 1024) {
    status_ = "Blueprint file is empty or implausibly large";
    statusT_ = 3;
    return false;
  }
  char magic[4] = {};
  f.read(magic, 4);
  if (!f || memcmp(magic, "RBP1", 4) != 0) {
    status_ = "Not a Realism blueprint";
    statusT_ = 3;
    return false;
  }
  bool ok = true;
  auto r32 = [&]() { uint32_t v = 0; if (!f.read((char*)&v, 4)) ok = false; return v; };
  auto rf = [&]() { float v = 0; if (!f.read((char*)&v, 4)) ok = false; return v; };
  auto rs = [&]() {
    uint32_t n = r32();
    if (!ok || n > 128) { ok = false; return std::string(); }
    std::string s(n, '\0');
    if (n && !f.read(s.data(), n)) ok = false;
    return s;
  };
  const float fileVs = rf();
  const uint32_t nc = r32();
  if (!ok || !std::isfinite(fileVs) || fileVs <= 0 || fileVs > 1 || nc > 4096) {
    status_ = "Blueprint header is corrupt";
    statusT_ = 3;
    return false;
  }
  // Joint names are stored by pointer in Component; a deque keeps those
  // strings at stable addresses while more blueprints are loaded.
  static std::deque<std::string> jointNames;
  std::vector<Component> defs;
  defs.reserve(nc);
  for (uint32_t i = 0; i < nc && ok; i++) {
    Component c;
    c.name = rs();
    c.object = rs();
    const uint32_t rawType = r32();
    if (rawType >= C_TYPE_COUNT) { ok = false; break; }
    c.type = (CompType)rawType;
    jointNames.push_back(rs());
    c.joint = Joint{jointNames.back().c_str(), rf()};
    c.axis.x = rf(); c.axis.y = rf(); c.axis.z = rf();
    c.reversed = r32() != 0;
    c.capacity = rf();
    c.fuel = rf();
    if (!std::isfinite(c.axis.x) || !std::isfinite(c.axis.y) || !std::isfinite(c.axis.z) ||
        !std::isfinite(c.capacity) || !std::isfinite(c.fuel) || c.capacity < 0 || c.fuel < 0) {
      ok = false;
      break;
    }
    if (c.type == C_BATTERY) c.charge = c.capacity;
    defs.push_back(c);
  }
  const uint32_t nr = r32();
  if (!ok || nr > 2000000) {
    status_ = "Blueprint part list is corrupt";
    statusT_ = 3;
    return false;
  }
  struct Rec { uint16_t x, y, z; uint8_t m; uint16_t c; };
  std::vector<Rec> recs(nr);
  if (nr) f.read((char*)recs.data(), (std::streamsize)nr * sizeof(Rec));
  if (!f) ok = false;
  for (const Rec& r : recs) {
    if (r.m >= MAT_COUNT || r.c >= nc) { ok = false; break; }
  }
  if (!ok) {
    status_ = "Blueprint is corrupt or truncated";
    statusT_ = 3;
    return false;
  }
  std::vector<uint32_t> ids;
  ids.reserve(nc);
  for (const Component& c : defs) ids.push_back(newComp(c));
  const float scaleF = fileVs / vs;  // blueprints built at another voxel size are resampled
  if (scaleF > 6.f) {
    status_ = "Blueprint resolution differs too much to import";
    statusT_ = 3;
    return false;
  }
  const size_t maxCells = 4000000;
  size_t expanded = 0;
  for (const Rec& r : recs) {
    Cell c;
    c.mat = r.m;
    c.comp = r.c < ids.size() ? ids[r.c] : 0;
    if (std::fabs(scaleF - 1) < 1e-3f) {
      j->vol.put(r.x, r.y, r.z, c);
      expanded++;
    } else {
      int n = std::max(1, (int)std::ceil(scaleF));
      expanded += (size_t)n * n * n;
      if (expanded > maxCells) {
        status_ = "Blueprint is too large to import at this voxel size";
        statusT_ = 3;
        return false;
      }
      for (int a = 0; a < n; a++) for (int b = 0; b < n; b++) for (int d = 0; d < n; d++)
        j->vol.put((int)(r.x * scaleF) + a, (int)(r.y * scaleF) + b, (int)(r.z * scaleF) + d, c);
    }
  }
  afterEdit();
  status_ = "Loaded blueprint \"" + name + "\"";
  statusT_ = 3;
  return true;
}

// ------------------------------------------------------------- analysis

void Workshop::refreshAnalysis() {
  an_ = Analysis{};
  Body* j = jig();
  if (!j) return;
  an_.voxels = j->voxels;
  double m = 0;
  j->vol.forEach([&](int, int, int, Brick* br, int l) { m += MATS[br->mat[l]].voxMass; });
  an_.mass = (float)m;
  for (uint32_t c : j->compIds) {
    const Component& cp = comp(c);
    switch (cp.type) {
      case C_BATTERY: an_.batteries++; an_.chargeWh += cp.charge; an_.drawW += cp.drawW; if (cp.shorted) an_.shorted = true; break;
      case C_SOLAR: an_.batteries++; break;
      case C_BULB: case C_HEATER: an_.loads++; if (cp.powered) an_.poweredLoads++; break;
      case C_MOTOR: an_.loads++; an_.motors++; if (cp.powered) an_.poweredLoads++; break;
      case C_WHEEL: an_.wheels++; break;
      case C_PROP: an_.props++; break;
      case C_ROCKET: an_.rockets++; break;
      case C_CHARGE: an_.charges++; break;
      default: break;
    }
  }
  // Separate pieces (they'd fall apart on launch).
  if (j->voxels > 0 && j->voxels < 400000) {
    Volume& v = j->vol;
    std::vector<char> seen((size_t)v.nx * v.ny * v.nz, 0);
    auto lin = [&](int x, int y, int z) { return (size_t)x + (size_t)v.nx * ((size_t)z + (size_t)v.nz * y); };
    int pieces = 0;
    std::vector<ivec3> st;
    v.forEach([&](int x, int y, int z, Brick*, int) {
      if (seen[lin(x, y, z)]) return;
      pieces++;
      st.push_back({x, y, z});
      seen[lin(x, y, z)] = 1;
      while (!st.empty()) {
        ivec3 c = st.back();
        st.pop_back();
        for (auto& d : DIRS6) {
          int xx = c.x + d[0], yy = c.y + d[1], zz = c.z + d[2];
          if (!v.inb(xx, yy, zz) || v.mat(xx, yy, zz) == AIR || seen[lin(xx, yy, zz)]) continue;
          seen[lin(xx, yy, zz)] = 1;
          st.push_back({xx, yy, zz});
        }
      }
    });
    an_.pieces = pieces;
  }
  if (an_.shorted) an_.notes.push_back("\xE2\x9A\xA1 Short circuit: + and \xE2\x88\x92 are joined by wire");
  if (an_.pieces > 1) an_.notes.push_back(std::to_string(an_.pieces) + " separate pieces: they'll come apart");
  if (an_.loads > an_.poweredLoads && an_.batteries > 0 && !an_.shorted)
    an_.notes.push_back("Some devices aren't in a closed circuit (or a switch is open)");
  if (an_.loads > 0 && an_.batteries == 0) an_.notes.push_back("Add a battery or solar panel to power devices");
  if (an_.motors > 0 && an_.wheels == 0 && an_.props == 0) an_.notes.push_back("Motors need a shaft to a wheel or propeller");
}

// ------------------------------------------------------------- update

void Workshop::updateGhost(const ivec3& at, bool valid) {
  Body* gh = g_.sim.bodyById(ghostId_);
  if (!gh) return;
  static ivec3 lastAt(-999, -999, -999);
  static int lastKey = -1;
  auto t = (tab_ == TAB_MATS || tool_ != E_PLACE || !valid) ? nullptr : currentTemplate();
  int key = (int)tab_ * 100000 + part_ * 1000 + salv_ * 10 + rotYaw_ + rotPitch_ * 4 + (t ? 1 : 0) * 7777;
  if (at == lastAt && key == lastKey) return;
  lastAt = at;
  lastKey = key;
  if (!t) {
    gh->vol.resize(1, 1, 1);
    gh->voxels = 0;
    gh->vol.version++;
    if (tab_ == TAB_MATS && valid && tool_ == E_PLACE) {
      gh->vol.resize(brush_ * 2 - 1, brush_ * 2 - 1, brush_ * 2 - 1);
      for (int x = 0; x < gh->vol.nx; x++) for (int y = 0; y < gh->vol.ny; y++) for (int z = 0; z < gh->vol.nz; z++) gh->vol.set(x, y, z, WS_MATS[mat_], 0);
      gh->pos = g_.scene.jigOrigin + (toF(at) + vec3((float)-(brush_ - 1), 0, (float)-(brush_ - 1)) + vec3(gh->vol.nx, gh->vol.ny, gh->vol.nz) * 0.5f) * g_.sim.vs;
      gh->com = vec3(gh->vol.nx, gh->vol.ny, gh->vol.nz) * 0.5f;
      gh->voxels = gh->vol.nx * gh->vol.ny * gh->vol.nz;
    }
    return;
  }
  gh->vol.resize(t->vol.nx, t->vol.ny, t->vol.nz);
  t->vol.forEach([&](int x, int y, int z, Brick* br, int l) { gh->vol.set(x, y, z, br->mat[l], 0); });
  gh->com = vec3(t->vol.nx, t->vol.ny, t->vol.nz) * 0.5f;
  ivec3 o(at.x - t->vol.nx / 2, at.y, at.z - t->vol.nz / 2);
  gh->pos = g_.scene.jigOrigin + (toF(o) + gh->com) * g_.sim.vs;
  gh->rot = quat();
  gh->voxels = 1;
}

void Workshop::update(float dt, plat::Input& in, Camera& cam) {
  if (!jig()) createJig();
  statusT_ = std::max(0.f, statusT_ - dt);
  // Typing a blueprint name.
  if (typingName_) {
    for (char c : in.text) if (saveName_.size() < 28 && c != '/' && c != '\\' && c != ':') saveName_.push_back(c);
    if (in.pressed[key::Backspace] && !saveName_.empty()) saveName_.pop_back();
    if (in.pressed[key::Enter]) { typingName_ = false; if (!saveName_.empty()) saveBlueprint(saveName_); }
    in.text.clear();
  } else {
    bool ctrl = in.down[key::Ctrl];
    if (ctrl && in.pressed['Z']) undo();
    else if (ctrl && in.pressed['Y']) redo();
    else {
      if (in.pressed['R']) { rotYaw_ = (rotYaw_ + 1) % 4; g_.audio.playUi(SFX_UI, 0.4f); }
      if (in.pressed['F']) { rotPitch_ = (rotPitch_ + 1) % 4; g_.audio.playUi(SFX_UI, 0.4f); }
      if (in.pressed['M']) { mirror_ = !mirror_; status_ = mirror_ ? "Mirror on: builds are symmetric about the centre line" : "Mirror off"; statusT_ = 2; }
      if (in.pressed['1']) tool_ = E_PLACE;
      if (in.pressed['2']) tool_ = E_ERASE;
      if (in.pressed['3']) tool_ = E_PAINT;
      if (in.pressed['4']) tool_ = E_PICK;
      if (in.pressed['5']) { tool_ = E_WIRE; wireStarted_ = false; }
      if (in.pressed[key::LBracket]) brush_ = std::max(1, brush_ - 1);
      if (in.pressed[key::RBracket]) brush_ = std::min(4, brush_ + 1);
      if (in.pressed[key::Enter]) { launch(); return; }
    }
  }
  // Orbit camera.
  bool overUI = ui::mouseOverUI();
  if (in.wheel != 0 && !overUI) tDist_ = clampf(tDist_ * std::pow(0.88f, in.wheel), 0.6f, 7.f);
  if (in.mpressed[1]) { rightDown_ = true; downX_ = in.mx; downY_ = in.my; orbiting_ = false; }
  if (rightDown_ && in.mdown[1]) {
    if (std::fabs(in.mx - downX_) + std::fabs(in.my - downY_) > 5) orbiting_ = true;
    if (orbiting_) { tYaw_ += in.dmx * 0.006f; tPitch_ = clampf(tPitch_ - in.dmy * 0.006f, -1.45f, 0.3f); }
  }
  if (in.mdown[2]) {
    tTarget_ -= cam.right * (in.dmx * 0.0025f * dist_);
    tTarget_ += cam.up * (in.dmy * 0.0025f * dist_);
  }
  float k = std::min(1.f, dt * 10);
  yaw_ += (tYaw_ - yaw_) * k;
  pitch_ += (tPitch_ - pitch_) * k;
  dist_ += (tDist_ - dist_) * k;
  target_ += (tTarget_ - target_) * k;
  vec3 fwd(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_), -std::cos(pitch_) * std::cos(yaw_));
  cam.pos = target_ - fwd * dist_;
  cam.yaw = yaw_;
  cam.pitch = pitch_;
  cam.update((float)plat::width() / std::max(1, plat::height()));

  // Cursor ray.
  float nx = in.mx / std::max(1, plat::width()) * 2 - 1, ny = 1 - in.my / std::max(1, plat::height()) * 2;
  vec4 a = cam.invViewProj * vec4(nx, ny, -1, 1), b = cam.invViewProj * vec4(nx, ny, 1, 1);
  vec3 ro = a.xyz() / a.w, rd = normalize(b.xyz() / b.w - ro);
  ivec3 cell, hitCell;
  bool onFloor = false;
  hoverValid_ = !overUI && cursorCell(ro, rd, cell, hitCell, onFloor);
  hoverCell_ = (tool_ == E_PLACE) ? cell : hitCell;
  if (tool_ == E_WIRE && !hoverValid_) {}
  updateGhost(cell, hoverValid_);

  // Highlight the part under the cursor.
  g_.hover_ = HoverInfo{};
  if (hoverValid_ && !onFloor) {
    Body* j = jig();
    uint32_t c = j->vol.comp(hitCell.x, hitCell.y, hitCell.z);
    if (c) {
      ivec3 lo = hitCell, hi = hitCell;
      j->vol.forEach([&](int x, int y, int z, Brick* br, int l) {
        if (br->comp[l] != c) return;
        lo = {std::min(lo.x, x), std::min(lo.y, y), std::min(lo.z, z)};
        hi = {std::max(hi.x, x), std::max(hi.y, y), std::max(hi.z, z)};
      });
      g_.hover_.body = g_.sim.bodyIndex(j);
      g_.hover_.comp = c;
      g_.hover_.lo = lo;
      g_.hover_.hi = hi;
    }
  }

  // Edits.
  if (hoverValid_ && !overUI) {
    bool lp = in.mpressed[0];
    bool held = in.mdown[0] && tab_ == TAB_MATS && (tool_ == E_PLACE || tool_ == E_ERASE || tool_ == E_PAINT);
    ivec3 target = tool_ == E_PLACE ? cell : hitCell;
    if (lp || (held && target != lastPlaced_)) {
      // A click on a switch in the jig flips it (handy for testing).
      Body* j = jig();
      uint32_t c = onFloor ? 0 : j->vol.comp(hitCell.x, hitCell.y, hitCell.z);
      if (lp && c && comp(c).type == C_SWITCH && in.down[key::Alt] == false && tool_ == E_PLACE && in.down[key::Shift]) {
        g_.toggleSwitch(g_.sim.bodyIndex(j), c);
      } else if (tool_ == E_PLACE) {
        place(cell, false);
      } else if (tool_ == E_ERASE && !onFloor) {
        place(hitCell, true);
      } else if (tool_ == E_PAINT && !onFloor) {
        paint(hitCell);
      } else if (tool_ == E_PICK && !onFloor) {
        pick(hitCell);
      } else if (tool_ == E_WIRE && !onFloor && lp) {
        wireClick(hitCell);
      }
      lastPlaced_ = target;
    }
    // Right click (no drag) erases.
    if (in.mreleased[1] && rightDown_ && !orbiting_ && !onFloor) place(hitCell, true);
    // E flips switches / reverses motors on the build.
    if (in.pressed['E'] && !onFloor) {
      Body* j = jig();
      uint32_t c = j->vol.comp(hitCell.x, hitCell.y, hitCell.z);
      if (c && comp(c).type == C_SWITCH) g_.toggleSwitch(g_.sim.bodyIndex(j), c);
      else if (c && comp(c).type == C_MOTOR) { comp(c).reversed = !comp(c).reversed; status_ = comp(c).reversed ? "Motor reversed" : "Motor forward"; statusT_ = 2; }
    }
  }
  if (!in.mdown[0]) lastPlaced_ = ivec3(-999, -999, -999);
  if (in.mreleased[1]) { rightDown_ = false; orbiting_ = false; }

  anTimer_ -= dt;
  if (anTimer_ <= 0) { refreshAnalysis(); anTimer_ = 0.25f; }
}

// ------------------------------------------------------------- UI

void Workshop::drawUI(float dt) {
  float s = scale();
  const float W = (float)width(), H = (float)height();
  // ---- build grid projected onto the jig floor (10 cm cells, 50 cm majors)
  if (Body* j = jig()) {
    const Camera& cam = g_.cam;
    auto proj = [&](const vec3& p, vec2& out) {
      vec4 c = cam.viewProj * vec4(p, 1.f);
      if (c.w <= 0.05f) return false;
      out = vec2((c.x / c.w * 0.5f + 0.5f) * W, (0.5f - c.y / c.w * 0.5f) * H);
      return true;
    };
    const float vs = g_.sim.vs;
    vec3 o = g_.scene.jigOrigin + vec3(0, 0.002f, 0);
    float X = j->vol.nx * vs, Z = j->vol.nz * vs;
    auto seg = [&](vec3 a, vec3 b, float th, Color c) {
      // Split long lines so the projection stays well-behaved near the camera.
      const int n = 8;
      for (int i = 0; i < n; i++) {
        vec2 p0, p1;
        if (proj(lerp(a, b, (float)i / n), p0) && proj(lerp(a, b, (float)(i + 1) / n), p1)) line(p0.x, p0.y, p1.x, p1.y, th, c);
      }
    };
    for (float x = 0; x <= X + 1e-3f; x += 0.1f) {
      bool major = std::fabs(std::fmod(x + 1e-3f, 0.5f)) < 0.01f;
      seg(o + vec3(x, 0, 0), o + vec3(x, 0, Z), major ? 1.4f : 1.f, Color(1, 1, 1, major ? 0.22f : 0.08f));
    }
    for (float z = 0; z <= Z + 1e-3f; z += 0.1f) {
      bool major = std::fabs(std::fmod(z + 1e-3f, 0.5f)) < 0.01f;
      seg(o + vec3(0, 0, z), o + vec3(X, 0, z), major ? 1.4f : 1.f, Color(1, 1, 1, major ? 0.22f : 0.08f));
    }
    // Jig volume outline.
    float Y = j->vol.ny * vs;
    Color edge = theme::accent.alpha(0.35f);
    vec3 c8[8];
    for (int i = 0; i < 8; i++) c8[i] = o + vec3((i & 1) ? X : 0, (i & 2) ? Y : 0, (i & 4) ? Z : 0);
    int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& k : e) seg(c8[k[0]], c8[k[1]], 1.5f, edge);
    if (mirror_) seg(o + vec3(X * 0.5f, 0, 0), o + vec3(X * 0.5f, 0, Z), 2.f, theme::warm.alpha(0.6f));
  }
  // ---- left palette
  float px = 16 * s, py = 16 * s, pw = 320 * s, ph = H - 32 * s;
  shadow(px, py, pw, ph, 16 * s, 20 * s, 0.45f);
  rectBorder(px, py, pw, ph, theme::panel, theme::line, 16 * s);
  text(px + 20 * s, py + 16 * s, "WORKSHOP", 22 * s, theme::text, 0, 0.08f);
  text(px + 20 * s, py + 46 * s, "Assemble anything from parts and materials", 13 * s, theme::faint);
  const char* tabs[] = {"Materials", "Parts", "Salvage", "Blueprints"};
  float tw = (pw - 32 * s) / 4;
  for (int i = 0; i < 4; i++) {
    float tx = px + 16 * s + i * tw, ty = py + 76 * s;
    bool sel = (int)tab_ == i;
    bool hv = hover(tx, ty, tw - 4 * s, 32 * s);
    if (hv) claimMouse();
    rect(tx, ty, tw - 4 * s, 32 * s, sel ? theme::accent.alpha(0.2f) : (hv ? Color(1, 1, 1, 0.08f) : Color(1, 1, 1, 0.03f)), 8 * s);
    std::string lbl = tabs[i];
    if (i == 2 && !g_.salvage.empty()) lbl += " " + std::to_string(g_.salvage.size());
    text(tx + (tw - 4 * s) * 0.5f, ty + 8 * s, lbl, 13 * s, sel ? theme::accent : theme::dim, 1);
    if (hv && mouse().released) { tab_ = (Tab)i; tool_ = E_PLACE; g_.audio.playUi(SFX_UI, 0.4f); }
  }
  float ly = py + 122 * s, lh = ph - 138 * s;
  pushClip(px, ly, pw, lh);
  if (hover(px, ly, pw, lh)) { claimMouse(); paletteScroll_ = std::max(0.f, paletteScroll_ - mouse().wheel * 60 * s); }
  float y = ly - paletteScroll_;
  float content = 0;
  if (tab_ == TAB_MATS) {
    float cw = (pw - 40 * s) / 3, chh = 74 * s;
    for (int i = 0; i < WS_MATS_N; i++) {
      float cx = px + 16 * s + (i % 3) * (cw + 4 * s), cy = y + (i / 3) * (chh + 4 * s);
      const Material& M = MATS[WS_MATS[i]];
      bool sel = mat_ == i;
      bool hv = hover(cx, cy, cw, chh);
      rectBorder(cx, cy, cw, chh, sel ? theme::accent.alpha(0.16f) : (hv ? Color(1, 1, 1, 0.07f) : Color(1, 1, 1, 0.03f)), sel ? theme::accent : theme::line, 10 * s, sel ? 1.5f * s : 1);
      uint32_t col = M.color;
      Color sw = Color::hex(col);
      gradient(cx + 10 * s, cy + 10 * s, cw - 20 * s, 30 * s, sw.mix(Color(1, 1, 1), 0.12f), sw.mix(Color(0, 0, 0), 0.25f), 6 * s);
      if (M.transparent) rectBorder(cx + 10 * s, cy + 10 * s, cw - 20 * s, 30 * s, Color(0, 0, 0, 0), Color(1, 1, 1, 0.4f), 6 * s);
      std::string nm = M.name;
      if (nm.size() > 14) nm = nm.substr(0, 13) + "\xE2\x80\xA6";
      text(cx + cw * 0.5f, cy + 48 * s, nm, 12 * s, theme::text, 1);
      if (hv) {
        claimMouse();
        char tip[200];
        snprintf(tip, sizeof tip, "%s \xC2\xB7 %.0f kg/m\xC2\xB3%s%s", M.name, M.density, M.wire ? " \xC2\xB7 conducts" : "", M.ignite > 0 ? " \xC2\xB7 flammable" : "");
        tooltip(tip);
        if (mouse().released) { mat_ = i; tool_ = E_PLACE; g_.audio.playUi(SFX_UI, 0.4f); }
      }
    }
    content = ((WS_MATS_N + 2) / 3) * (chh + 4 * s);
    float iy = y + content + 10 * s;
    textWrapped(px + 18 * s, iy, pw - 36 * s, MATS[WS_MATS[mat_]].info, 13 * s, theme::dim);
    content += 80 * s;
  } else if (tab_ == TAB_PARTS || tab_ == TAB_SALVAGE) {
    int n = tab_ == TAB_PARTS ? (int)partCatalogue().size() : (int)g_.salvage.size();
    if (n == 0) textWrapped(px + 18 * s, y + 6 * s, pw - 36 * s, "Nothing salvaged yet. In the world, right-click a part with the Wrench (3) to take it home.", 14 * s, theme::dim);
    for (int i = 0; i < n; i++) {
      float rh = 64 * s, rx = px + 14 * s, ry = y + i * (rh + 6 * s), rw = pw - 28 * s;
      bool sel = tab_ == TAB_PARTS ? part_ == i : salv_ == i;
      bool hv = hover(rx, ry, rw, rh);
      rectBorder(rx, ry, rw, rh, sel ? theme::accent.alpha(0.16f) : (hv ? Color(1, 1, 1, 0.07f) : Color(1, 1, 1, 0.03f)), sel ? theme::accent : theme::line, 10 * s);
      std::string name, blurb;
      int ic = 0;
      if (tab_ == TAB_PARTS) { auto& p = partCatalogue()[i]; name = p.name; blurb = p.blurb; ic = p.icon; }
      else { auto& t = *g_.salvage[i]; name = t.name; blurb = std::string(compTypeName(t.proto.type)) + (t.proto.object.empty() ? "" : " \xC2\xB7 from " + t.proto.object); ic = 12; }
      icon(ic, rx + 32 * s, ry + rh * 0.5f, 40 * s, sel ? theme::accent : theme::text);
      text(rx + 62 * s, ry + 10 * s, name, 15 * s, theme::text, 0, 0.04f);
      std::string b2 = blurb;
      if (textWidth(b2, 12 * s) > rw - 72 * s) { while (b2.size() > 3 && textWidth(b2 + "\xE2\x80\xA6", 12 * s) > rw - 72 * s) b2.pop_back(); b2 += "\xE2\x80\xA6"; }
      text(rx + 62 * s, ry + 34 * s, b2, 12 * s, theme::faint);
      if (hv) {
        claimMouse();
        tooltip(blurb);
        if (mouse().released) { if (tab_ == TAB_PARTS) part_ = i; else salv_ = i; tool_ = E_PLACE; g_.audio.playUi(SFX_UI, 0.4f); }
      }
    }
    content = n * 70 * s;
  } else {
    float ry = y;
    text(px + 18 * s, ry, "EXAMPLE MACHINES", 12 * s, theme::faint, 0, 0.05f);
    ry += 24 * s;
    for (auto& a : assemblies()) {
      float rh = 62 * s, rx = px + 14 * s, rw = pw - 28 * s;
      bool hv = hover(rx, ry, rw, rh);
      rectBorder(rx, ry, rw, rh, hv ? Color(1, 1, 1, 0.08f) : Color(1, 1, 1, 0.03f), theme::line, 10 * s);
      text(rx + 14 * s, ry + 9 * s, a.name, 15 * s, theme::text, 0, 0.04f);
      std::string b2 = a.blurb;
      if (textWidth(b2, 12 * s) > rw - 28 * s) { while (b2.size() > 3 && textWidth(b2 + "\xE2\x80\xA6", 12 * s) > rw - 28 * s) b2.pop_back(); b2 += "\xE2\x80\xA6"; }
      text(rx + 14 * s, ry + 33 * s, b2, 12 * s, theme::faint);
      if (hv) { claimMouse(); tooltip(a.blurb); if (mouse().released) { loadBlueprint(a.name); g_.audio.playUi(SFX_POP, 0.5f); } }
      ry += rh + 6 * s;
    }
    auto names = blueprintNames();
    ry += 10 * s;
    text(px + 18 * s, ry, "YOUR BLUEPRINTS", 12 * s, theme::faint, 0, 0.05f);
    ry += 24 * s;
    if (names.empty()) { text(px + 18 * s, ry, "Save a build to see it here.", 13 * s, theme::dim); ry += 24 * s; }
    for (auto& n : names) {
      float rh = 40 * s, rx = px + 14 * s, rw = pw - 28 * s;
      bool hv = hover(rx, ry, rw, rh);
      rectBorder(rx, ry, rw, rh, hv ? Color(1, 1, 1, 0.08f) : Color(1, 1, 1, 0.03f), theme::line, 10 * s);
      text(rx + 14 * s, ry + 11 * s, n, 14 * s, theme::text);
      if (hv) { claimMouse(); if (mouse().released) loadBlueprint(n); }
      ry += rh + 6 * s;
    }
    content = ry - y;
  }
  popClip();
  paletteScroll_ = std::min(paletteScroll_, std::max(0.f, content - lh + 20 * s));

  // ---- top toolbar
  {
    float bs = 44 * s, gap = 6 * s;
    int n = 12;
    float tw2 = n * bs + (n - 1) * gap + 24 * s;
    float tx = W * 0.5f - tw2 * 0.5f + 60 * s, ty = 16 * s;
    shadow(tx, ty, tw2, bs + 16 * s, 14 * s, 16 * s, 0.4f);
    rectBorder(tx, ty, tw2, bs + 16 * s, theme::panel, theme::line, 14 * s);
    float x = tx + 12 * s, y2 = ty + 8 * s;
    if (iconButton(I_BUILD, x, y2, bs, tool_ == E_PLACE, "Place (1)")) { tool_ = E_PLACE; }
    x += bs + gap;
    if (iconButton(I_ERASE, x, y2, bs, tool_ == E_ERASE, "Erase (2)")) { tool_ = E_ERASE; }
    x += bs + gap;
    if (iconButton(I_PAINT, x, y2, bs, tool_ == E_PAINT, "Paint material (3)")) { tool_ = E_PAINT; tab_ = TAB_MATS; } x += bs + gap;
    if (iconButton(I_EYEDROP, x, y2, bs, tool_ == E_PICK, "Pick material (4)")) { tool_ = E_PICK; }
    x += bs + gap;
    if (iconButton(I_BOLT, x, y2, bs, tool_ == E_WIRE, "Wire: click two conductors (5)")) { tool_ = E_WIRE; wireStarted_ = false; }
    x += bs + gap;
    x += 8 * s;
    if (iconButton(I_ROTATE, x, y2, bs, false, "Rotate part (R)")) rotYaw_ = (rotYaw_ + 1) % 4;
    text(x + bs - 6 * s, y2 + bs - 16 * s, std::to_string(rotYaw_ * 90) + "\xC2\xB0", 11 * s, theme::faint, 2);
    x += bs + gap;
    if (iconButton(I_REDO, x, y2, bs, rotPitch_ != 0, "Tip part over (F)")) { rotPitch_ = (rotPitch_ + 1) % 4; }
    x += bs + gap;
    if (iconButton(I_MIRROR, x, y2, bs, mirror_, "Mirror left/right (M)")) { mirror_ = !mirror_; }
    x += bs + gap;
    x += 8 * s;
    if (iconButton(I_UNDO, x, y2, bs, false, "Undo (Ctrl+Z)")) { undo(); }
    x += bs + gap;
    if (iconButton(I_REDO, x, y2, bs, false, "Redo (Ctrl+Y)")) { redo(); }
    x += bs + gap;
    if (iconButton(I_TRASH, x, y2, bs, false, "Clear the jig")) { clearJig(); }
    x += bs + gap;
    // Brush size.
    float bx = x;
    rectBorder(bx, y2, bs, bs, Color(1, 1, 1, 0.04f), theme::line, 8 * s);
    text(bx + bs * 0.5f, y2 + 6 * s, std::to_string(brush_), 16 * s, theme::text, 1, 0.05f);
    text(bx + bs * 0.5f, y2 + 26 * s, "brush", 10 * s, theme::faint, 1);
    if (hover(bx, y2, bs, bs)) { claimMouse(); tooltip("Brush size: [ and ] (or click)"); if (mouse().released) brush_ = brush_ % 4 + 1; }
  }

  // ---- right panel: analysis + launch
  {
    float rw = 300 * s, rx = W - rw - 16 * s, ry = 16 * s, rh = 520 * s;
    shadow(rx, ry, rw, rh, 16 * s, 20 * s, 0.45f);
    rectBorder(rx, ry, rw, rh, theme::panel, theme::line, 16 * s);
    text(rx + 20 * s, ry + 16 * s, "Build analysis", 18 * s, theme::text, 0, 0.05f);
    float yy = ry + 52 * s;
    auto row = [&](const char* k, const std::string& v, Color c = theme::text) {
      text(rx + 20 * s, yy, k, 14 * s, theme::faint);
      text(rx + rw - 20 * s, yy, v, 14 * s, c, 2);
      yy += 24 * s;
    };
    char b[64];
    snprintf(b, sizeof b, an_.mass < 100 ? "%.1f kg" : "%.0f kg", an_.mass);
    row("Mass", b);
    snprintf(b, sizeof b, "%zu", an_.voxels);
    row("Voxels", b);
    row("Power sources", std::to_string(an_.batteries), an_.batteries ? theme::power : theme::faint);
    if (an_.batteries) {
      snprintf(b, sizeof b, "%.0f Wh \xC2\xB7 %.0f W draw", an_.chargeWh, an_.drawW);
      row("Energy", b, theme::power);
      if (an_.drawW > 1) {
        float hrs = an_.chargeWh / an_.drawW;
        snprintf(b, sizeof b, hrs < 1 ? "%.0f min" : "%.1f h", hrs < 1 ? hrs * 60 : hrs);
        row("Runtime", b, theme::dim);
      }
    }
    snprintf(b, sizeof b, "%d of %d", an_.poweredLoads, an_.loads);
    row("Devices powered", b, an_.loads && an_.poweredLoads == an_.loads ? theme::good : theme::dim);
    if (an_.wheels) row("Wheels", std::to_string(an_.wheels));
    if (an_.props) row("Propellers", std::to_string(an_.props));
    if (an_.rockets) row("Rocket motors", std::to_string(an_.rockets), theme::warm);
    if (an_.charges) row("Explosives", std::to_string(an_.charges), theme::danger);
    yy += 6 * s;
    for (auto& n : an_.notes) {
      bool bad = n.find("Short") != std::string::npos;
      float hh = textWrapped(rx + 32 * s, yy + 2 * s, rw - 50 * s, n, 13 * s, bad ? theme::danger : theme::warm);
      circle(rx + 22 * s, yy + 9 * s, 3 * s, bad ? theme::danger : theme::warm);
      yy += hh + 8 * s;
    }
    if (an_.notes.empty() && an_.voxels > 0) { text(rx + 20 * s, yy, "\xE2\x9C\x93 Looks good", 14 * s, theme::good); yy += 24 * s; }
    // Launch.
    float by = ry + rh - 170 * s;
    if (button("LAUNCH  (Enter)", rx + 20 * s, by, rw - 40 * s, 52 * s, true, an_.voxels > 0)) launch();
    by += 64 * s;
    // Save row.
    float fw = rw - 40 * s - 90 * s;
    bool hvName = hover(rx + 20 * s, by, fw, 40 * s);
    if (hvName) claimMouse();
    if (hvName && mouse().released) typingName_ = true;
    rectBorder(rx + 20 * s, by, fw, 40 * s, Color(0, 0, 0, 0.25f), typingName_ ? theme::accent : theme::line, 8 * s);
    std::string nm = saveName_ + (typingName_ && std::fmod(plat::now(), 1.0) < 0.5 ? "|" : "");
    text(rx + 32 * s, by + 11 * s, nm, 14 * s, typingName_ ? theme::text : theme::dim);
    if (button("Save", rx + rw - 20 * s - 82 * s, by, 82 * s, 40 * s, false, an_.voxels > 0)) { typingName_ = false; saveBlueprint(saveName_); }
    by += 52 * s;
    text(rx + rw * 0.5f, by, "Tab / Esc: back to the world", 12 * s, theme::faint, 1);
  }

  // ---- hover readout near the cursor
  if (hoverValid_ && g_.hover_.comp && !mouseOverUI()) {
    const Component& c = comp(g_.hover_.comp);
    std::string t = c.name;
    if (c.type == C_SWITCH) t += "  \xC2\xB7  E to flip";
    else if (c.type == C_MOTOR) t += c.powered ? "  \xC2\xB7  running" : "  \xC2\xB7  unpowered";
    else if (c.type == C_BULB || c.type == C_HEATER) t += c.powered ? "  \xC2\xB7  on" : "  \xC2\xB7  off";
    float w = textWidth(t, 14 * s) + 20 * s;
    float x = mouse().x + 18 * s, yq = mouse().y + 14 * s;
    rectBorder(x, yq, w, 28 * s, theme::panelHi, theme::line, 8 * s);
    text(x + 10 * s, yq + 6 * s, t, 14 * s, theme::text);
  }

  // ---- bottom hints + status
  {
    std::string h = tool_ == E_WIRE ? "Wire: click a terminal, lead or switch end, then another \xC2\xB7 RMB click erases a wire \xC2\xB7 Esc cancels" : "LMB place  \xC2\xB7  RMB click erase  \xC2\xB7  RMB drag orbit  \xC2\xB7  MMB pan  \xC2\xB7  wheel zoom  \xC2\xB7  R rotate  \xC2\xB7  F tip  \xC2\xB7  M mirror  \xC2\xB7  E use  \xC2\xB7  Ctrl+Z undo";
    float w = textWidth(h, 13 * s) + 30 * s;
    float x = W * 0.5f - w * 0.5f + 60 * s, y2 = H - 46 * s;
    rectBorder(x, y2, w, 30 * s, theme::panel, theme::line, 15 * s);
    text(W * 0.5f + 60 * s, y2 + 8 * s, h, 13 * s, theme::dim, 1);
    if (statusT_ > 0 && !status_.empty()) {
      float a = std::min(1.f, statusT_ * 2);
      float sw = textWidth(status_, 15 * s) + 34 * s;
      rectBorder(W * 0.5f - sw * 0.5f + 60 * s, y2 - 46 * s, sw, 34 * s, theme::panelHi.alpha(a), theme::accent.alpha(0.5f * a), 17 * s);
      text(W * 0.5f + 60 * s, y2 - 38 * s, status_, 15 * s, theme::text.alpha(a), 1);
    }
  }
}
