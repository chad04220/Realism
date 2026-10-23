#include "sim.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include "../core/jobs.h"

Sim* g_sim = nullptr;

void Sim::init(const SimSettings& s) {
  cfg = s;
  vs = s.voxelSize;
  initMaterials(vs);
  int nx = (int)std::lround(s.extent.x / vs), ny = (int)std::lround(s.extent.y / vs), nz = (int)std::lround(s.extent.z / vs);
  nx = (nx + 15) & ~15; ny = (ny + 15) & ~15; nz = (nz + 15) & ~15;
  world.resize(nx, ny, nz);
  world.solidBelow = true;
  worldHot.reset(world.bricks.size());
  bodies.clear();
  debris.clear();
  gas.reset(world);
  debrisEscaped = 0;
  flashes.clear();
  fireSpots.clear();
  fluidActive.clear();
  supportSeeds.clear();
  worldSpecials.clear();
  events.clear();
  radiators.clear();
  radiantTotal = 0;
  waterFlow.clear();
  waterFlowN.clear();
  air = AirSim{};
  airReady = false;
  // A re-init is a new world: simulation clocks, statistics and timers must
  // not carry the previous run's state into it.
  time = 0;
  timeScale = 1;
  stats = {};
  thermalAcc_ = fluidAcc_ = electricAcc_ = airAcc_ = gasAcc_ = 0;
  lastSupportTime_ = -1;
  loadState_.clear();
  g_sim = this;
}

void Sim::initAir() {
  air.init(world, vs, 0.2f);
  for (int b : world.shapeList) world.shapeFlag[b] = 0;
  world.shapeList.clear();
  airReady = true;
}

// Air <-> everything else, once per air step.
void Sim::airCoupling(float dt) {
  // World edits reshape the air's obstacles.
  for (int b : world.shapeList) {
    world.shapeFlag[b] = 0;
    int bx = b % world.bx, bz = (b / world.bx) % world.bz, by = b / (world.bx * world.bz);
    air.markWorldDirty(bx, by, bz);
  }
  world.shapeList.clear();
  // Moving bodies push air out of their way; air drags light bodies.
  for (auto& bp : bodies) {
    Body& b = *bp;
    if (b.ghost || b.frozen || b.asleep) continue;
    mat3 R = b.R();
    size_t stride = std::max<size_t>(1, b.samples.size() / 48);
    vec3 F, Tq;
    for (size_t k = 0; k < b.samples.size(); k += stride) {
      vec3 wp = b.pos + R * b.samples[k].local;
      vec3 pv = b.pointVel(wp);
      air.setBodyCell(wp, pv);
      vec3 rel = air.velocity(wp + vec3(0, air.h, 0)) - pv;
      float s = length(rel);
      if (s < 0.3f) continue;
      float area = PI * b.samples[k].r * b.samples[k].r * (float)stride;
      vec3 f = rel * (0.5f * 1.2f * 1.0f * area * s);
      F += f;
      Tq += cross(wp - b.pos, f);
    }
    b.force += F;
    b.torque += Tq;
  }
  air.step(dt, world);
}

void Sim::step(float dtReal) {
  float dt = dtReal * timeScale;
  if (dt <= 0) return;
  time += dt;
  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();

  stepDevices(dt);
  stepPhysics(dt);
  auto t1 = clk::now();
  stats.physicsMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

  stepDebris(dt);
  stepFlashes(dtReal);
  gasAcc_ += dt;
  if (gasAcc_ >= 1.f / 30.f) {
    auto a = clk::now();
    gas.step(*this, std::min(gasAcc_, 0.1f));
    stats.gasMs = std::chrono::duration<float, std::milli>(clk::now() - a).count();
    gasAcc_ = 0;
  }

  thermalAcc_ += dt;
  const float thermalTick = 1.f / 20.f;
  int guard = 0;
  while (thermalAcc_ >= thermalTick && guard++ < 2) {
    auto a = clk::now();
    stepThermal(thermalTick * cfg.thermalWarp);
    stats.thermalMs = std::chrono::duration<float, std::milli>(clk::now() - a).count();
    thermalAcc_ -= thermalTick;
  }
  if (thermalAcc_ > thermalTick) thermalAcc_ = 0;

  fluidAcc_ += dt;
  if (fluidAcc_ >= 1.f / 45.f) {
    auto a = clk::now();
    stepFluids(1.f / 45.f);
    stats.fluidMs = std::chrono::duration<float, std::milli>(clk::now() - a).count();
    fluidAcc_ = 0;
  }

  electricAcc_ += dt;
  if (electricAcc_ >= 0.1f) {
    auto a = clk::now();
    stepElectric(electricAcc_);
    stats.electricMs = std::chrono::duration<float, std::milli>(clk::now() - a).count();
    electricAcc_ = 0;
  }

  if (airReady) {
    airAcc_ += dt;
    if (airAcc_ >= 1.f / 20.f) {
      airCoupling(std::min(airAcc_, 1.f / 10.f));
      airAcc_ = 0;
    }
  }

  processSupport();

  // Structural bookkeeping for bodies changed this frame.
  for (size_t i = 0; i < bodies.size(); i++) {
    Body& b = *bodies[i];
    if (b.splitCheck) { b.splitCheck = false; if (!b.frozen) splitBody(b); }
  }
  for (size_t i = 0; i < bodies.size(); i++) {
    Body& b = *bodies[i];
    if (b.shapeDirty) rebuildBody(b);
  }
  for (int i = (int)bodies.size() - 1; i >= 0; i--) {
    if (bodies[i]->voxels == 0 && !bodies[i]->frozen) { removeBody(i); continue; }
    Body& b = *bodies[i];
    b.age += dt;
    // Small rubble that has come to rest freezes back into the world to keep
    // the body count (and cost) bounded.
    bool small = b.voxels <= 40 || (bodies.size() > 220 && b.voxels <= 400);
    if (b.asleep && !b.held && !b.frozen && !b.ghost && small && b.compIds.size() <= 2) {
      b.restTime += dt;
      if (b.restTime > (bodies.size() > 150 ? 1.0f : 4.0f)) settleBody(i);
    } else {
      b.restTime = 0;
    }
  }
}

// ---------------------------------------------------------------- world edits

void Sim::worldChanged(int x, int y, int z, uint8_t oldMat) {
  uint8_t m = world.mat(x, y, z);
  int64_t key = packKey(x, y, z);
  if (isSpecialElectric(m) || MATS[m].wire) worldSpecials.insert(key); else worldSpecials.erase(key);
  if (isFluidish(m)) fluidActive.insert(key);
  // New matter is new weight on whatever holds it up.
  if (m != AIR && isSolid(m) && !(oldMat != AIR && isSolid(oldMat))) supportSeeds.push_back(key);
  if (m == AIR || isFluidish(m)) {
    for (auto& d : DIRS6) {
      int xx = x + d[0], yy = y + d[1], zz = z + d[2];
      uint8_t n = world.mat(xx, yy, zz);
      if (n == AIR || !world.inb(xx, yy, zz)) continue;
      if (isFluidish(n)) fluidActive.insert(packKey(xx, yy, zz));
      else if (oldMat != AIR && isSolid(oldMat)) supportSeeds.push_back(packKey(xx, yy, zz));
    }
    wakeNear(cellCenter(x, y, z), vs * 2.5f);
  }
  if (m != AIR) {
    Brick* b = world.brick(x, y, z);
    int l = lidx(x, y, z);
    if (b->temp[l] != AMBIENT || b->flags[l]) markHot(world, worldHot, x, y, z);
  }
}

void Sim::setWorld(int x, int y, int z, uint8_t m, uint32_t comp) {
  if (!world.inb(x, y, z)) return;
  uint8_t old = world.mat(x, y, z);
  if (old == BEDROCK) return;
  world.set(x, y, z, m, comp);
  worldChanged(x, y, z, old);
}

void Sim::putWorld(int x, int y, int z, const Cell& c) {
  if (!world.inb(x, y, z)) return;
  uint8_t old = world.mat(x, y, z);
  if (old == BEDROCK) return;
  world.put(x, y, z, c);
  worldChanged(x, y, z, old);
}

void Sim::removeWorld(int x, int y, int z, bool makeDebris, const vec3& kick) {
  if (!world.inb(x, y, z)) return;
  Cell c = world.get(x, y, z);
  if (c.mat == AIR || c.mat == BEDROCK) return;
  if (makeDebris) spawnDebris(cellCenter(x, y, z), kick, c);
  world.set(x, y, z, AIR);
  worldChanged(x, y, z, c.mat);
}

// processSupport() and the structural checks live in structure.cpp.

// ---------------------------------------------------------------- bodies

Body* Sim::newBody() {
  bodies.push_back(std::make_unique<Body>());
  Body* b = bodies.back().get();
  b->id = nextBodyId++;
  return b;
}

Body* Sim::bodyById(int id) {
  for (auto& b : bodies) if (b->id == id) return b.get();
  return nullptr;
}

int Sim::bodyIndex(const Body* b) const {
  for (size_t i = 0; i < bodies.size(); i++) if (bodies[i].get() == b) return (int)i;
  return -1;
}

void Sim::removeBody(int index) {
  bodies.erase(bodies.begin() + index);
}

void Sim::rebuildBody(Body& b) {
  b.shapeDirty = false;
  const Volume& v = b.vol;
  double m = 0;
  double cx = 0, cy = 0, cz = 0;
  size_t count = 0;
  v.forEach([&](int x, int y, int z, Brick* br, int l) {
    float mm = MATS[br->mat[l]].voxMass;
    m += mm; cx += mm * (x + 0.5); cy += mm * (y + 0.5); cz += mm * (z + 0.5);
    count++;
  });
  b.voxels = count;
  if (count == 0 || m <= 0) { b.voxels = 0; return; }
  vec3 newCom((float)(cx / m), (float)(cy / m), (float)(cz / m));
  if (b.mass > 0) b.pos += b.rot.rotate((newCom - b.com) * vs);  // keep voxels where they are
  b.com = newCom;
  b.mass = (float)m;
  b.invMass = b.frozen ? 0.f : 1.f / b.mass;
  // Inertia tensor about the COM (each voxel as a small cube).
  double I[6] = {0, 0, 0, 0, 0, 0};  // xx yy zz xy xz yz
  float rad2 = 0;
  const float cube = vs * vs / 6.f;
  v.forEach([&](int x, int y, int z, Brick* br, int l) {
    float mm = MATS[br->mat[l]].voxMass;
    vec3 r = (vec3(x + 0.5f, y + 0.5f, z + 0.5f) - b.com) * vs;
    I[0] += mm * (r.y * r.y + r.z * r.z + cube);
    I[1] += mm * (r.x * r.x + r.z * r.z + cube);
    I[2] += mm * (r.x * r.x + r.y * r.y + cube);
    I[3] -= mm * r.x * r.y; I[4] -= mm * r.x * r.z; I[5] -= mm * r.y * r.z;
    rad2 = std::max(rad2, length2(r));
  });
  mat3 Il;
  Il.m[0] = (float)I[0]; Il.m[4] = (float)I[1]; Il.m[8] = (float)I[2];
  Il.m[1] = Il.m[3] = (float)I[3]; Il.m[2] = Il.m[6] = (float)I[4]; Il.m[5] = Il.m[7] = (float)I[5];
  b.invIlocal = b.frozen ? mat3::zero() : Il.inverse();
  b.radius = std::sqrt(rad2) + vs;
  b.updateInertiaWorld();

  // Collision samples: one sphere per exposed coarse cell.
  const int C = count > 24000 ? 4 : 2;
  const int cxn = (v.nx + C - 1) / C, cyn = (v.ny + C - 1) / C, czn = (v.nz + C - 1) / C;
  struct Acc { float x = 0, y = 0, z = 0; int n = 0; bool surf = false; };
  std::vector<Acc> acc((size_t)cxn * cyn * czn);
  v.forEach([&](int x, int y, int z, Brick* br, int l) {
    Acc& a = acc[(x / C) + cxn * ((z / C) + czn * (y / C))];
    a.x += x + 0.5f; a.y += y + 0.5f; a.z += z + 0.5f; a.n++;
    if (!a.surf) {
      for (auto& d : DIRS6) if (v.mat(x + d[0], y + d[1], z + d[2]) == AIR) { a.surf = true; break; }
    }
    (void)br; (void)l;
  });
  b.samples.clear();
  for (int cy_ = 0; cy_ < cyn; cy_++)
    for (int cz_ = 0; cz_ < czn; cz_++)
      for (int cx_ = 0; cx_ < cxn; cx_++) {
        const Acc& a = acc[cx_ + cxn * (cz_ + czn * cy_)];
        if (!a.n || !a.surf) continue;
        vec3 c(a.x / a.n, a.y / a.n, a.z / a.n);
        ivec3 cell = floorI(c);
        if (v.matI(cell) == AIR) {  // centroid fell in a gap: use any voxel of the cell
          for (int k = 0; k < C * C * C; k++) {
            ivec3 q(cx_ * C + k % C, cy_ * C + (k / (C * C)), cz_ * C + (k / C) % C);
            if (v.matI(q) != AIR) { cell = q; break; }
          }
        }
        Sample s;
        s.local = (c - b.com) * vs;
        // Smaller spheres for sparsely filled cells (thin plates, wires).
        float fill = (float)a.n / (C * C * C);
        s.r = vs * C * 0.5f * std::max(0.55f, std::cbrt(fill));
        s.cell = cell;
        s.mat = v.matI(cell);
        s.comp = v.comp(cell.x, cell.y, cell.z);
        b.samples.push_back(s);
      }

  // Components, rotating parts.
  b.compIds.clear();
  std::vector<uint32_t> seen;
  struct RotAcc { vec3 sum; int n = 0; };
  std::vector<std::pair<uint32_t, RotAcc>> rots;
  v.forEach([&](int x, int y, int z, Brick* br, int l) {
    uint32_t c = br->comp[l];
    if (c && std::find(b.compIds.begin(), b.compIds.end(), c) == b.compIds.end()) b.compIds.push_back(c);
    if (c) {
      CompType t = comp(c).type;
      if (t == C_WHEEL || t == C_PROP) {
        auto it = std::find_if(rots.begin(), rots.end(), [&](auto& p) { return p.first == c; });
        if (it == rots.end()) { rots.push_back({c, RotAcc{}}); it = rots.end() - 1; }
        it->second.sum += vec3(x + 0.5f, y + 0.5f, z + 0.5f);
        it->second.n++;
      }
    }
  });
  b.rot_parts.clear();
  for (auto& [c, a] : rots) {
    RotPart rp;
    rp.comp = c;
    rp.pivot = a.sum / (float)a.n;
    rp.axis = normalize(comp(c).axis);
    float r = 0;
    v.forEach([&](int x, int y, int z, Brick* br, int l) {
      if (br->comp[l] != c) return;
      vec3 d = vec3(x + 0.5f, y + 0.5f, z + 0.5f) - rp.pivot;
      d -= rp.axis * dot(d, rp.axis);
      r = std::max(r, length(d));
    });
    rp.radius = (r + 0.5f) * vs;
    b.rot_parts.push_back(rp);
  }
  // Thermal bookkeeping sized to the volume.
  if (b.heat.on.size() != b.vol.bricks.size()) {
    b.heat.reset(b.vol.bricks.size());
    for (size_t i = 0; i < b.vol.bricks.size(); i++) if (b.vol.bricks[i]) b.heat.add((int)i);
  }
}

// Build a body from cells given in some source grid. srcToWorld maps a source
// voxel coordinate (corner-based) to world space; rot is the source's rotation.
static Body* buildBodyFrom(Sim& s, const std::vector<std::pair<ivec3, Cell>>& cells, const Body* parent) {
  if (cells.empty()) return nullptr;
  ivec3 lo = cells[0].first, hi = lo;
  for (auto& c : cells) {
    lo = {std::min(lo.x, c.first.x), std::min(lo.y, c.first.y), std::min(lo.z, c.first.z)};
    hi = {std::max(hi.x, c.first.x), std::max(hi.y, c.first.y), std::max(hi.z, c.first.z)};
  }
  Body* b = s.newBody();
  b->vol.resize(hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1);
  for (auto& c : cells) b->vol.put(c.first.x - lo.x, c.first.y - lo.y, c.first.z - lo.z, c.second);
  b->mass = 0;
  s.rebuildBody(*b);
  if (parent) {
    b->rot = parent->rot;
    b->pos = parent->toWorld(b->com + toF(lo), s.vs);
    b->vel = parent->pointVel(b->pos);
    b->angVel = parent->angVel;
  } else {
    b->rot = quat();
    b->pos = (b->com + toF(lo)) * s.vs;
  }
  b->updateInertiaWorld();
  b->heat.reset(b->vol.bricks.size());
  for (size_t i = 0; i < b->vol.bricks.size(); i++) if (b->vol.bricks[i]) b->heat.add((int)i);
  return b;
}

Body* Sim::detachWorld(const std::vector<int64_t>& keys, const vec3& kick) {
  std::vector<std::pair<ivec3, Cell>> cells;
  cells.reserve(keys.size());
  for (int64_t k : keys) {
    ivec3 p = unpackKey(k);
    Cell c = world.get(p.x, p.y, p.z);
    if (c.mat == AIR || c.mat == BEDROCK || isFluidish(c.mat)) continue;
    cells.push_back({p, c});
  }
  if (cells.empty()) return nullptr;
  for (auto& c : cells) {
    world.set(c.first.x, c.first.y, c.first.z, AIR);
    worldSpecials.erase(packKey(c.first.x, c.first.y, c.first.z));
  }
  // Fluids that rested on the piece can now flow; neighbours may lose support.
  for (auto& c : cells) {
    for (auto& d : DIRS6) {
      int xx = c.first.x + d[0], yy = c.first.y + d[1], zz = c.first.z + d[2];
      uint8_t n = world.mat(xx, yy, zz);
      if (n != AIR && isFluidish(n)) fluidActive.insert(packKey(xx, yy, zz));
    }
  }
  Body* b = buildBodyFrom(*this, cells, nullptr);
  if (b) {
    b->vel = kick;
    wakeNear(b->pos, b->radius + vs * 3);
    event(EV_DETACH, b->pos, b->mass, cells[0].second.mat, cells[0].second.comp);
  }
  return b;
}

Body* Sim::detachComponent(int bi, uint32_t cid, const vec3& kick, const ivec3& seed) {
  if (cid == 0 || comp(cid).type == C_TERRAIN) return nullptr;
  if (bi < 0) {
    // Flood through the world from the hit voxel over cells of the same part.
    std::vector<int64_t> keys;
    std::vector<ivec3> stack{seed};
    std::unordered_set<int64_t> seen{packKey(seed.x, seed.y, seed.z)};
    if (world.comp(seed.x, seed.y, seed.z) != cid) return nullptr;
    while (!stack.empty() && keys.size() < 2000000) {
      ivec3 c = stack.back();
      stack.pop_back();
      keys.push_back(packKey(c.x, c.y, c.z));
      for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            ivec3 q(c.x + dx, c.y + dy, c.z + dz);
            if (!world.inb(q.x, q.y, q.z) || world.comp(q.x, q.y, q.z) != cid || world.matI(q) == AIR) continue;
            if (seen.insert(packKey(q.x, q.y, q.z)).second) stack.push_back(q);
          }
    }
    for (int64_t k : keys) { ivec3 p = unpackKey(k); for (auto& d : DIRS6) supportSeeds.push_back(packKey(p.x + d[0], p.y + d[1], p.z + d[2])); }
    return detachWorld(keys, kick);
  }
  Body& src = *bodies[bi];
  if (src.compIds.size() <= 1) { src.vel += kick; src.wake(); return &src; }
  std::vector<std::pair<ivec3, Cell>> cells;
  src.vol.forEach([&](int x, int y, int z, Brick* br, int l) {
    if (br->comp[l] == cid) {
      Cell c;
      c.mat = br->mat[l]; c.flags = br->flags[l]; c.hp = br->hp[l]; c.aux = br->aux[l]; c.comp = cid; c.temp = br->temp[l]; c.fuel = br->fuel[l];
      cells.push_back({{x, y, z}, c});
    }
  });
  if (cells.empty()) return nullptr;
  for (auto& c : cells) src.vol.set(c.first.x, c.first.y, c.first.z, AIR);
  src.shapeDirty = true;
  src.splitCheck = true;
  src.wake();
  Body* parentCopy = bodies[bi].get();
  Body* b = buildBodyFrom(*this, cells, parentCopy);
  if (b) { b->vel += kick; b->wake(); event(EV_DETACH, b->pos, b->mass, cells[0].second.mat, cid); }
  return b;
}

void Sim::removeBodyVoxel(Body& b, int x, int y, int z, bool makeDebris) {
  Cell c = b.vol.get(x, y, z);
  if (c.mat == AIR) return;
  if (makeDebris) {
    vec3 p = b.toWorld(vec3(x + 0.5f, y + 0.5f, z + 0.5f), vs);
    spawnDebris(p, b.pointVel(p), c);
  }
  b.vol.set(x, y, z, AIR);
  b.shapeDirty = true;
  b.splitCheck = true;
  b.wake();
}

void Sim::splitBody(Body& b) {
  Volume& v = b.vol;
  const size_t n = (size_t)v.nx * v.ny * v.nz;
  std::vector<int> label(n, 0);
  auto lin = [&](int x, int y, int z) { return (size_t)x + (size_t)v.nx * ((size_t)z + (size_t)v.nz * y); };
  std::vector<std::vector<ivec3>> parts;
  std::vector<ivec3> stack;
  v.forEach([&](int x, int y, int z, Brick*, int) {
    if (label[lin(x, y, z)]) return;
    int id = (int)parts.size() + 1;
    parts.emplace_back();
    auto& part = parts.back();
    stack.clear();
    stack.push_back({x, y, z});
    label[lin(x, y, z)] = id;
    while (!stack.empty()) {
      ivec3 c = stack.back();
      stack.pop_back();
      part.push_back(c);
      for (auto& d : DIRS6) {
        int xx = c.x + d[0], yy = c.y + d[1], zz = c.z + d[2];
        if (!v.inb(xx, yy, zz) || v.mat(xx, yy, zz) == AIR) continue;
        int& L = label[lin(xx, yy, zz)];
        if (L) continue;
        L = id;
        stack.push_back({xx, yy, zz});
      }
    }
  });
  if (parts.size() <= 1) return;
  // The largest piece stays in this body; the rest fly off as new bodies.
  size_t keep = 0;
  for (size_t i = 1; i < parts.size(); i++) if (parts[i].size() > parts[keep].size()) keep = i;
  int bid = b.id;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i == keep) continue;
    Body* self = bodyById(bid);
    if (parts[i].size() <= 8) {
      for (auto& c : parts[i]) removeBodyVoxel(*self, c.x, c.y, c.z, true);
      continue;
    }
    std::vector<std::pair<ivec3, Cell>> cells;
    cells.reserve(parts[i].size());
    for (auto& c : parts[i]) cells.push_back({c, self->vol.get(c.x, c.y, c.z)});
    for (auto& c : parts[i]) self->vol.set(c.x, c.y, c.z, AIR);
    Body* nb = buildBodyFrom(*this, cells, self);
    if (nb) nb->wake();
  }
  Body* self = bodyById(bid);
  if (self) { self->shapeDirty = true; self->splitCheck = false; }
}

void Sim::settleBody(int index) {
  Body& b = *bodies[index];
  int placed = 0;
  b.vol.forEach([&](int x, int y, int z, Brick* br, int l) {
    vec3 p = b.toWorld(vec3(x + 0.5f, y + 0.5f, z + 0.5f), vs);
    ivec3 c = cellOf(p);
    Cell cell;
    cell.mat = br->mat[l]; cell.flags = br->flags[l]; cell.hp = br->hp[l]; cell.aux = br->aux[l];
    cell.comp = br->comp[l]; cell.temp = br->temp[l]; cell.fuel = br->fuel[l];
    // Find the nearest free cell (the snap may collide with a neighbour).
    bool done = false;
    for (int up = 0; up < 3 && !done; up++) {
      ivec3 q(c.x, c.y + up, c.z);
      if (world.inb(q.x, q.y, q.z) && world.matI(q) == AIR) {
        world.put(q.x, q.y, q.z, cell);
        worldChanged(q.x, q.y, q.z, AIR);
        done = true;
      }
    }
    // Further away if need be; failing that it stays loose as a fragment.
    if (!done) done = depositVoxel(c, cell);
    if (!done) spawnDebris(p, vec3(0), cell);
    placed++;
  });
  removeBody(index);
}

void Sim::wakeNear(const vec3& p, float r) {
  for (auto& b : bodies) {
    if (!b->asleep) continue;
    if (length(b->pos - p) <= b->radius + r) b->wake();
  }
}

// ---------------------------------------------------------------- raycast

static bool dda(const Volume& v, vec3 o, vec3 d, float maxT, Hit& out) {
  // Clip to the volume box first.
  float t0 = 0, t1 = maxT;
  vec3 lo(0), hi((float)v.nx, (float)v.ny, (float)v.nz);
  int entryAxis = -1;
  for (int a = 0; a < 3; a++) {
    if (std::fabs(d[a]) < 1e-9f) { if (o[a] < lo[a] || o[a] > hi[a]) return false; continue; }
    float ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
    if (ta > tb) std::swap(ta, tb);
    if (ta > t0) { t0 = ta; entryAxis = a; }
    t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  vec3 p = o + d * (t0 + 1e-4f);
  int x = (int)std::floor(p.x), y = (int)std::floor(p.y), z = (int)std::floor(p.z);
  x = std::clamp(x, 0, v.nx - 1); y = std::clamp(y, 0, v.ny - 1); z = std::clamp(z, 0, v.nz - 1);
  int sx = d.x > 0 ? 1 : -1, sy = d.y > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
  float tdx = d.x != 0 ? std::fabs(1 / d.x) : 1e30f, tdy = d.y != 0 ? std::fabs(1 / d.y) : 1e30f, tdz = d.z != 0 ? std::fabs(1 / d.z) : 1e30f;
  float tmx = d.x != 0 ? ((sx > 0 ? x + 1 - o.x : o.x - x) * tdx) : 1e30f;
  float tmy = d.y != 0 ? ((sy > 0 ? y + 1 - o.y : o.y - y) * tdy) : 1e30f;
  float tmz = d.z != 0 ? ((sz > 0 ? z + 1 - o.z : o.z - z) * tdz) : 1e30f;
  ivec3 face(0, 0, 0);
  if (entryAxis >= 0 && t0 > 0) { face[entryAxis] = d[entryAxis] > 0 ? -1 : 1; }
  float t = t0;
  for (int i = 0; i < 8192 && t <= t1; i++) {
    if (!v.inb(x, y, z)) break;
    const Brick* b = v.brick(x, y, z);
    if (b && b->mat[lidx(x, y, z)] != AIR) {
      out.hit = true; out.cell = {x, y, z}; out.face = face; out.t = t;
      return true;
    }
    if (tmx < tmy && tmx < tmz) { x += sx; t = tmx; tmx += tdx; face = {-sx, 0, 0}; }
    else if (tmy < tmz) { y += sy; t = tmy; tmy += tdy; face = {0, -sy, 0}; }
    else { z += sz; t = tmz; tmz += tdz; face = {0, 0, -sz}; }
  }
  return false;
}

Hit Sim::raycast(const vec3& o, const vec3& d, float maxDist, int ignoreBody) const {
  Hit best;
  best.t = maxDist;
  Hit h;
  if (dda(world, o / vs, d, maxDist / vs, h)) {
    float t = h.t * vs;
    if (t < best.t) {
      best = h; best.t = t; best.body = -1;
      best.point = o + d * t;
      best.normal = toF(h.face);
      if (h.face == ivec3(0, 0, 0)) best.normal = -d;
    }
  }
  for (size_t i = 0; i < bodies.size(); i++) {
    const Body& b = *bodies[i];
    if ((int)i == ignoreBody || b.ghost) continue;
    // Bounding-sphere reject.
    vec3 oc = o - b.pos;
    float bproj = dot(oc, d), c = dot(oc, oc) - b.radius * b.radius;
    if (c > 0 && bproj > 0) continue;
    if (bproj * bproj - c < 0) continue;
    quat inv = b.rot.conj();
    vec3 lo = inv.rotate(o - b.pos) / vs + b.com;
    vec3 ld = inv.rotate(d);
    Hit bh;
    if (dda(b.vol, lo, ld, best.t / vs, bh)) {
      float t = bh.t * vs;
      if (t < best.t) {
        best = bh; best.t = t; best.body = (int)i;
        best.point = o + d * t;
        best.normal = bh.face == ivec3(0, 0, 0) ? -d : b.rot.rotate(toF(bh.face));
      }
    }
  }
  best.hit = best.t < maxDist && (best.hit);
  return best;
}

// ---------------------------------------------------------------- effects

void Sim::spawnDebris(const vec3& p, const vec3& v, const Cell& c) {
  if (c.mat == AIR) return;
  if (debris.size() >= 30000) {
    // Too much in flight: it goes straight to where it would land.
    if (depositVoxel(cellOf(p), c)) return;
  }
  Debris d;
  d.p = p; d.v = v; d.mat = c.mat; d.temp = c.temp; d.life = 12; d.comp = c.comp; d.bounces = 0; d.aux = c.aux;
  debris.push_back(d);
}

void Sim::stepFlashes(float dt) {
  for (size_t i = 0; i < flashes.size();) {
    flashes[i].life -= dt;
    if (flashes[i].life <= 0) { flashes[i] = flashes.back(); flashes.pop_back(); continue; }
    i++;
  }
}

size_t Sim::matterCount() const {
  size_t n = 0;
  auto vol = [&](const Volume& v) { for (auto& b : v.bricks) if (b) n += (size_t)b->count; };
  vol(world);
  for (auto& b : bodies) if (!b->ghost && !b->viewmodel) vol(b->vol);
  return n + debris.size() + gas.count() + (size_t)gas.escaped + (size_t)debrisEscaped;
}
