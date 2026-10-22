// Saved games: the simulation (sim/save.cpp) plus what only the game knows:
// the player, the tools, salvaged parts, collected material, armed charges,
// open valves and which body is the workshop jig.
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include "../sim/save.h"
#include "game.h"
#include "workshop.h"

namespace fs = std::filesystem;

#ifndef REALISM_VERSION
#define REALISM_VERSION "dev"
#endif

namespace {

void writeTemplate(save::Writer& w, const Template& t) {
  w.str(t.name);
  w.str(t.category);
  save::writeComponent(w, t.proto);
  save::writeVolume(w, t.vol);
  w.u32((uint32_t)t.extraComps.size());
  for (auto& [p, c] : t.extraComps) { w.iv3(p); w.u32(c); }
}

bool readTemplate(save::Reader& r, Template& t, const save::MatMap& mats) {
  t.name = r.str(256);
  t.category = r.str(256);
  t.proto = save::readComponent(r);
  // Template voxels carry template-local part numbers, not global ids.
  if (!save::readVolume(r, t.vol, mats, 1u << 30)) return false;
  uint32_t n = r.count(4096, 16, "sub-part");
  for (uint32_t i = 0; i < n && r.ok; i++) {
    ivec3 p = r.iv3();
    uint32_t c = r.u32();
    t.extraComps.push_back({p, c});
  }
  return r.ok;
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

}  // namespace

std::string Game::saveDir;

std::string Game::savePath(const std::string& slot) {
  return (saveDir.empty() ? plat::userDataDir() + "/saves" : saveDir) + "/" + slot + ".rsav";
}

bool Game::saveExists(const std::string& slot) {
  std::error_code ec;
  return fs::exists(savePath(slot), ec);
}

bool Game::saveGame(const std::string& slot, std::string* errOut) {
  std::string err;
  if (slot.empty() || slot.find_first_of("/\\") != std::string::npos || slot.find("..") != std::string::npos) {
    if (errOut) *errOut = "invalid save name";
    return false;
  }
  save::Writer w;
  save::writeSim(w, sim);

  w.tag("GAME");
  // Scene layout the rest of the game is placed against.
  w.v3(scene.spawn); w.f32(scene.spawnYaw); w.v3(scene.jigOrigin); w.iv3(scene.jigDims); w.f32(scene.groundY);
  // Player.
  w.v3(pl_.pos); w.v3(pl_.vel); w.f32(pl_.yaw); w.f32(pl_.pitch);
  w.u8(pl_.onGround); w.u8(pl_.fly); w.u8(pl_.crouch); w.f32(pl_.eye);
  // Tools and toggles.
  w.i32(tool_); w.i32(buildMat_); w.i32(buildSize_); w.i32(buildPart_); w.u8(flashlight_);
  w.i32(workshop ? workshop->jigId() : 0);
  // Salvaged parts and chiselled-out material.
  w.u32((uint32_t)salvage.size());
  for (auto& t : salvage) writeTemplate(w, *t);
  w.u32((uint32_t)collected.size());
  for (auto& [m, n] : collected) { w.u8(m); w.i32(n); }
  // Gate cells lifted out of open valves (to put back when they close).
  w.u32((uint32_t)valveCells_.size());
  for (auto& [c, cells] : valveCells_) {
    w.u32(c);
    w.u32((uint32_t)cells.size());
    for (auto& p : cells) w.iv3(p);
  }
  // Charges armed for the remote detonator.
  w.u32((uint32_t)remoteCharges_.size());
  for (size_t i = 0; i < remoteCharges_.size(); i++) {
    w.u32(remoteCharges_[i]);
    w.i32(i < remoteChargeBodies_.size() ? remoteChargeBodies_[i] : 0);
  }
  w.tag("END ");

  size_t bodies = 0;
  for (auto& b : sim.bodies) if (!b->ghost && !b->viewmodel) bodies++;
  char header[512];
  snprintf(header, sizeof header,
           "{\"game\":\"Realism\",\"version\":\"%s\",\"format\":%u,\"saved_unix\":%lld,\"slot\":\"%s\","
           "\"voxel_size\":%.4f,\"world_m\":[%.2f,%.2f,%.2f],\"sim_time_s\":%.2f,\"bodies\":%zu,\"voxels\":%zu}",
           REALISM_VERSION, save::FORMAT, (long long)std::time(nullptr), jsonEscape(slot).c_str(), sim.cfg.voxelSize,
           sim.cfg.extent.x, sim.cfg.extent.y, sim.cfg.extent.z, sim.time, bodies, sim.world.countSolid());
  bool ok = save::writeFile(savePath(slot), header, w.buf, err);
  if (!ok) fprintf(stderr, "realism: save failed: %s\n", err.c_str());
  if (errOut) *errOut = err;
  return ok;
}

bool Game::loadGame(const std::string& slot, std::string* errOut) {
  std::string header, err;
  std::vector<uint8_t> payload;
  auto fail = [&](const std::string& e) {
    fprintf(stderr, "realism: load of '%s' failed: %s\n", slot.c_str(), e.c_str());
    if (errOut) *errOut = e;
    return false;
  };
  if (slot.empty() || slot.find_first_of("/\\") != std::string::npos || slot.find("..") != std::string::npos)
    return fail("invalid save name");
  // Checksum, magic and format are verified before the live world is touched.
  if (!save::readFile(savePath(slot), header, payload, err)) return fail(err);

  save::Reader r(payload.data(), payload.size());
  save::MatMap mats;
  bool replaced = false;
  bool ok = save::readSim(r, sim, mats, replaced);

  SceneInfo sc;
  Player p;
  int tool = T_HAND, bMat = 0, bSize = 1, bPart = -1, jigId = 0;
  bool flash = false;
  std::vector<std::shared_ptr<Template>> salv;
  std::map<uint8_t, int> coll;
  std::map<uint32_t, std::vector<ivec3>> valves;
  std::vector<uint32_t> charges;
  std::vector<int> chargeBodies;
  if (ok && r.tag("GAME")) {
    sc.spawn = r.v3(); sc.spawnYaw = r.f32(); sc.jigOrigin = r.v3(); sc.jigDims = r.iv3(); sc.groundY = r.f32();
    p.pos = r.v3(); p.vel = r.v3(); p.yaw = r.f32(); p.pitch = r.f32();
    p.onGround = r.u8() != 0; p.fly = r.u8() != 0; p.crouch = r.u8() != 0; p.eye = r.f32();
    tool = r.i32(); bMat = r.i32(); bSize = r.i32(); bPart = r.i32(); flash = r.u8() != 0;
    jigId = r.i32();
    uint32_t ns = r.count(4096, 32, "salvaged part");
    for (uint32_t i = 0; i < ns && r.ok; i++) {
      auto t = std::make_shared<Template>();
      if (readTemplate(r, *t, mats)) salv.push_back(t);
    }
    uint32_t nm = r.count(256, 5, "material tally");
    for (uint32_t i = 0; i < nm && r.ok; i++) { uint8_t m = mats(r.u8()); int n = r.i32(); if (n > 0) coll[m] += n; }
    uint32_t nv = r.count(1u << 16, 8, "valve");
    for (uint32_t i = 0; i < nv && r.ok; i++) {
      uint32_t c = r.u32();
      uint32_t n = r.count(1u << 20, 12, "valve cell");
      auto& cells = valves[c];
      for (uint32_t k = 0; k < n && r.ok; k++) cells.push_back(r.iv3());
    }
    uint32_t nr = r.count(1u << 16, 8, "remote charge");
    for (uint32_t i = 0; i < nr && r.ok; i++) { charges.push_back(r.u32()); chargeBodies.push_back(r.i32()); }
    r.tag("END ");
    auto fin = [](const vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (r.ok && (!fin(p.pos) || !fin(p.vel) || !std::isfinite(p.yaw) || !std::isfinite(p.pitch) || !fin(sc.spawn) ||
                 !fin(sc.jigOrigin)))
      r.fail("player or scene data in the save is not finite");
    ok = r.ok;
  }
  if (!ok) {
    err = r.err.empty() ? "save data is malformed" : r.err;
    fprintf(stderr, "realism: '%s' failed part-way (%s)%s\n", slot.c_str(), err.c_str(),
            replaced ? "; rebuilding the default world" : "");
    if (replaced) {
      // The world was already cleared; never leave the player in a void.
      resetWorld();
      toast("Couldn't load save: " + err, ui::theme::danger);
    }
    return fail(err);
  }

  // Everything parsed: install the game state.
  initParts(sim.vs);
  sim.cfg.thermalWarp = settings.thermalWarp;  // a preference, not world state
  scene = sc;
  pl_ = p;
  pl_.yaw = std::fmod(pl_.yaw, 2 * PI);
  pl_.pitch = std::clamp(pl_.pitch, -1.55f, 1.55f);
  tool_ = std::clamp(tool, 0, (int)T_COUNT - 1);
  buildMat_ = std::clamp(bMat, 0, BUILD_PALETTE_N - 1);
  buildSize_ = (bSize == 1 || bSize == 2 || bSize == 4) ? bSize : 1;
  salvage = std::move(salv);
  buildPart_ = (bPart >= 0 && bPart < (int)salvage.size()) ? bPart : -1;
  flashlight_ = flash;
  collected = std::move(coll);
  valveCells_ = std::move(valves);
  remoteCharges_.clear();
  remoteChargeBodies_.clear();
  for (size_t i = 0; i < charges.size(); i++) {
    if (charges[i] == 0 || charges[i] >= comps().size()) continue;
    remoteCharges_.push_back(charges[i]);
    remoteChargeBodies_.push_back(chargeBodies[i]);
  }
  for (auto it = valveCells_.begin(); it != valveCells_.end();)
    it = (it->first == 0 || it->first >= comps().size()) ? valveCells_.erase(it) : std::next(it);
  sim.events.clear();
  if (workshop) {
    if (mode_ == M_WORKSHOP) workshop->leave();
    ren.groundY = scene.groundY - 0.005f;
    ren.uploadAllWorld();
    workshop->adoptJig(jigId);
    buildViewmodels();
  }
  clearTransientState();
  mode_ = M_PLAY;
  autosaveTimer_ = 0;
  if (errOut) errOut->clear();
  return true;
}

bool Game::autosave() {
  // Keep the previous autosave until the new one is safely in place, so a
  // crash or a corrupt write still leaves something to fall back to.
  std::error_code ec;
  std::string cur = savePath("autosave"), prev = savePath("autosave.1");
  if (fs::exists(cur, ec)) fs::copy_file(cur, prev, fs::copy_options::overwrite_existing, ec);
  std::string err;
  bool ok = saveGame("autosave", &err);
  autosaveTimer_ = 0;
  return ok;
}

bool Game::loadNewestAutosave(std::string* err) {
  std::string e1, e2;
  if (loadGame("autosave", &e1)) return true;
  if (loadGame("autosave.1", &e2)) {
    toast("Latest autosave was unreadable (" + e1 + "); loaded the one before it", ui::theme::warm);
    return true;
  }
  if (err) *err = e1;
  return false;
}
