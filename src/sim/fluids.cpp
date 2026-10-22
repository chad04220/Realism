// Liquids flow and powders slump on the world grid; loose debris flies
// ballistically and settles back into the world where it lands.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include "sim.h"

// Water is whole voxels, like everything else. A water voxel with nothing
// under it falls (one cell at a time over a short drop, as a free-falling
// voxel over a longer one) and pours over edges. Pressure acts on each
// connected body of water as a whole: voxels are taken from its highest
// surface and put into its lowest open cells, which is how a pond finds one
// level, a heap spreads out and water in a pipe climbs to the height of its
// tank. An opening well below the surface jets: the voxels leave at
// Torricelli's speed, sqrt(2 g h), as many per second as the hole passes.
// Water is conserved exactly, voxel for voxel.
namespace {
inline bool isWaterOpen(uint8_t m) { return m == AIR; }
}  // namespace

void Sim::settleWater() {
  // Every water cell is a whole voxel (the fuel field is unused for water).
  world.forEach([&](int, int, int, Brick* br, int l) {
    if (br->mat[l] == WATER) br->fuel[l] = 0;
  });
}

// Put a voxel of water as near as possible to `c`. Landing in water, it joins
// that body at its lowest open cell (where its pressure would push it anyway);
// landing inside something solid, it comes out on top; otherwise it goes
// where it is. Returns false only if there is nowhere at all.
bool Sim::placeWater(ivec3 c, float temp) {
  if (!world.inb(c.x, 0, c.z)) return false;
  c.y = std::clamp(c.y, 0, world.ny - 1);
  auto put = [&](const ivec3& q) {
    Cell cell;
    cell.mat = WATER; cell.temp = temp;
    putWorld(q.x, q.y, q.z, cell);
    if (temp != AMBIENT) markHot(world, worldHot, q.x, q.y, q.z);
    return true;
  };
  for (int up = 0; up < 64 && world.matI(c) != AIR && world.matI(c) != WATER && c.y + 1 < world.ny; up++) c.y++;
  uint8_t m0 = world.matI(c);
  if (m0 == AIR) return put(c);
  if (m0 == WATER) {
    std::unordered_set<int64_t> seen{packKey(c.x, c.y, c.z)};
    std::vector<ivec3> stack{c};
    bool found = false;
    ivec3 best;
    while (!stack.empty() && seen.size() < 200000) {
      ivec3 q = stack.back();
      stack.pop_back();
      for (auto& d : DIRS6) {
        ivec3 n(q.x + d[0], q.y + d[1], q.z + d[2]);
        if (!world.inb(n.x, n.y, n.z)) continue;
        uint8_t m = world.matI(n);
        if (m == WATER) { if (seen.insert(packKey(n.x, n.y, n.z)).second) stack.push_back(n); }
        else if (m == AIR && (!found || n.y < best.y)) { best = n; found = true; }
      }
    }
    if (found) return put(best);
  }
  for (int r = 1; r <= 3; r++)
    for (int dy = -r; dy <= r; dy++)
      for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
          if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r) continue;
          ivec3 q(c.x + dx, c.y + dy, c.z + dz);
          if (world.inb(q.x, q.y, q.z) && world.matI(q) == AIR) return put(q);
        }
  fprintf(stderr, "realism: nowhere to put a water voxel near %d %d %d\n", c.x, c.y, c.z);
  return false;
}

void Sim::stepWater(std::vector<int64_t>& list, float tick) {
  const float g = 9.81f;
  if (waterFlow.size() != air.T.size()) { waterFlow.assign(air.T.size(), vec3(0)); waterFlowN.assign(air.T.size(), 0); }
  std::fill(waterFlowN.begin(), waterFlowN.end(), 0.f);
  std::vector<vec3> flowNew(waterFlow.size(), vec3(0));
  auto coarse = [&](int x, int y, int z) {
    if (air.cellVox <= 0) return -1;
    int cx = x / air.cellVox, cy = y / air.cellVox, cz = z / air.cellVox;
    if (cx < 0 || cy < 0 || cz < 0 || cx >= air.nx || cy >= air.ny || cz >= air.nz) return -1;
    return air.cid(cx, cy, cz);
  };
  auto activate = [&](int x, int y, int z) {
    for (auto& d : DIRS6) {
      int xx = x + d[0], yy = y + d[1], zz = z + d[2];
      uint8_t m = world.mat(xx, yy, zz);
      if (m && isFluidish(m)) fluidActive.insert(packKey(xx, yy, zz));
    }
  };
  auto flow = [&](int x, int y, int z, const vec3& v) {
    int cc = coarse(x, y, z);
    if (cc >= 0) { flowNew[cc] += v; waterFlowN[cc] += 1; }
  };
  // Move one water voxel to an empty cell (keeping its temperature).
  auto move = [&](int x, int y, int z, int nx_, int ny_, int nz_) {
    Cell c = world.get(x, y, z);
    world.set(x, y, z, AIR);
    world.put(nx_, ny_, nz_, c);
    if (c.temp != AMBIENT) markHot(world, worldHot, nx_, ny_, nz_);
    fluidActive.insert(packKey(nx_, ny_, nz_));
    activate(x, y, z);
    activate(nx_, ny_, nz_);
    flow(x, y, z, vec3((float)(nx_ - x), (float)(ny_ - y), (float)(nz_ - z)) * (vs / tick));
  };
  static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

  // 1. Local motion: falling and pouring over edges, bottom-up so columns
  // fall together.
  std::vector<int64_t> settled;
  settled.reserve(list.size());
  for (int64_t k : list) {
    ivec3 q = unpackKey(k);
    int x = q.x, y = q.y, z = q.z;
    if (world.mat(x, y, z) != WATER) continue;
    if (y > 0 && world.mat(x, y - 1, z) == AIR) {
      if (y > 1 && world.mat(x, y - 2, z) == AIR) {
        // Nothing to stand on: it falls freely.
        Cell c = world.get(x, y, z);
        world.set(x, y, z, AIR);
        activate(x, y, z);
        spawnDebris(cellCenter(x, y, z), vec3(rng().range(-.04f, .04f), -std::sqrt(2 * g * vs), rng().range(-.04f, .04f)), c);
        continue;
      }
      move(x, y, z, x, y - 1, z);
      continue;
    }
    int r = rng().next() & 3;
    bool moved = false;
    for (int i = 0; i < 4 && !moved; i++) {
      int dx = dirs[(i + r) & 3][0], dz = dirs[(i + r) & 3][1];
      if (y > 0 && world.mat(x + dx, y, z + dz) == AIR && world.inb(x + dx, y, z + dz) && world.mat(x + dx, y - 1, z + dz) == AIR) {
        move(x, y, z, x + dx, y, z + dz);  // over the edge; it drops from there next tick
        moved = true;
      }
    }
    if (!moved) settled.push_back(k);
  }

  // 2. Pressure, per connected body of water. Only bodies touched by an
  // active voxel are walked, so still ponds cost nothing.
  std::unordered_set<int64_t> seen;
  std::vector<int64_t> stack;
  for (int64_t seed : settled) {
    if (seen.count(seed)) continue;
    ivec3 s0 = unpackKey(seed);
    if (world.mat(s0.x, s0.y, s0.z) != WATER) continue;
    std::vector<ivec3> tops;                  // water with open air above
    std::vector<std::pair<ivec3, ivec3>> slots;  // (open cell, the water beside or below it)
    std::unordered_set<int64_t> slotSeen;
    stack.assign(1, seed);
    seen.insert(seed);
    size_t count = 0;
    const size_t CAP = 600000;
    while (!stack.empty() && count < CAP) {
      int64_t k = stack.back();
      stack.pop_back();
      count++;
      ivec3 c = unpackKey(k);
      for (int d = 0; d < 6; d++) {
        int xx = c.x + DIRS6[d][0], yy = c.y + DIRS6[d][1], zz = c.z + DIRS6[d][2];
        if (!world.inb(xx, yy, zz)) continue;
        uint8_t m = world.mat(xx, yy, zz);
        int64_t kk = packKey(xx, yy, zz);
        if (m == WATER) {
          if (seen.insert(kk).second) stack.push_back(kk);
        } else if (isWaterOpen(m) && d != 3) {  // not below: that is handled by falling
          if (d == 2) tops.push_back(c);
          if (slotSeen.insert(kk).second) slots.push_back({ivec3(xx, yy, zz), c});
        }
      }
    }
    static const bool dbg = getenv("REALISM_DEBUG_WATER") != nullptr;
    if (dbg) fprintf(stderr, "water body at %d %d %d: %zu voxels, %zu tops, %zu slots, lowest slot y %d, top y %d\n", s0.x, s0.y,
                     s0.z, count, tops.size(), slots.size(), slots.empty() ? -1 : std::min_element(slots.begin(), slots.end(), [](auto& a, auto& b) { return a.first.y < b.first.y; })->first.y,
                     tops.empty() ? -1 : std::max_element(tops.begin(), tops.end(), [](auto& a, auto& b) { return a.y < b.y; })->y);
    if (tops.empty() || slots.empty()) continue;
    std::sort(tops.begin(), tops.end(), [](const ivec3& a, const ivec3& b) { return a.y > b.y; });
    std::sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) { return a.first.y < b.first.y; });
    const int surface = tops[0].y;
    size_t ti = 0;
    int moves = 0;
    // Not yet level: the body stays awake until it is, even on a pass where
    // a slow leak happens to move nothing.
    if (surface > slots[0].first.y) fluidActive.insert(seed);
    const int MAX_MOVES = 4096;
    for (auto& [slot, from] : slots) {
      if (ti >= tops.size() || moves >= MAX_MOVES) break;
      // Highest remaining surface voxel that hasn't been moved this pass.
      while (ti < tops.size() && world.mat(tops[ti].x, tops[ti].y, tops[ti].z) != WATER) ti++;
      if (ti >= tops.size()) break;
      if (tops[ti].y <= slot.y) break;   // level (to within a voxel): nothing more to do
      if (world.matI(slot) != AIR) continue;
      const int depth = surface - slot.y;  // voxels of water standing above this opening
      const bool side = slot.y == from.y;
      if (side && depth >= 3) {
        // An opening under a head of water: a jet at sqrt(2 g h), passing
        // v * dt / vs voxels per tick through each opening.
        const float h = (depth + 0.5f) * vs;
        const float v = std::sqrt(2 * g * h);
        const vec3 dir = normalize(vec3((float)(slot.x - from.x), 0, (float)(slot.z - from.z)));
        float want = v * tick / vs;
        int n = (int)want + (rng().f() < want - (int)want ? 1 : 0);
        for (int i = 0; i < n && ti < tops.size(); i++) {
          while (ti < tops.size() && world.mat(tops[ti].x, tops[ti].y, tops[ti].z) != WATER) ti++;
          if (ti >= tops.size() || tops[ti].y <= slot.y) break;
          ivec3 t = tops[ti++];
          Cell c = world.get(t.x, t.y, t.z);
          world.set(t.x, t.y, t.z, AIR);
          activate(t.x, t.y, t.z);
          fluidActive.insert(packKey(t.x, t.y - 1, t.z));
          // Leaves from inside the opening itself, never from the cell beyond it.
          spawnDebris(cellCenter(slot.x, slot.y, slot.z) + dir * (vs * rng().range(-0.3f, 0.3f)),
                      dir * v + vec3(rng().range(-.05f, .05f), rng().range(-.03f, .03f), rng().range(-.05f, .05f)), c);
          flow(from.x, from.y, from.z, dir * v);
          moves++;
        }
        continue;
      }
      ivec3 t = tops[ti++];
      Cell c = world.get(t.x, t.y, t.z);
      world.set(t.x, t.y, t.z, AIR);
      world.put(slot.x, slot.y, slot.z, c);
      if (c.temp != AMBIENT) markHot(world, worldHot, slot.x, slot.y, slot.z);
      activate(t.x, t.y, t.z);
      activate(slot.x, slot.y, slot.z);
      fluidActive.insert(packKey(slot.x, slot.y, slot.z));
      fluidActive.insert(packKey(t.x, t.y - 1, t.z));
      flow(from.x, from.y, from.z, vec3((float)(slot.x - from.x), 0, (float)(slot.z - from.z)) * (vs / tick));
      moves++;
    }
  }
  // Currents for floating things: a smoothed mean flow per air cell.
  for (size_t c = 0; c < waterFlow.size(); c++) {
    vec3 target = waterFlowN[c] > 0 ? flowNew[c] / waterFlowN[c] : vec3(0);
    waterFlow[c] = waterFlow[c] * 0.8f + target * 0.2f;
    if (waterFlowN[c] == 0 && length2(waterFlow[c]) < 1e-6f) waterFlow[c] = vec3(0);
  }
}

vec3 Sim::waterCurrent(const vec3& p) const {
  if (waterFlow.empty() || !airReady) return vec3(0);
  int cx = (int)(p.x / air.h), cy = (int)(p.y / air.h), cz = (int)(p.z / air.h);
  if (cx < 0 || cy < 0 || cz < 0 || cx >= air.nx || cy >= air.ny || cz >= air.nz) return vec3(0);
  return waterFlow[air.cid(cx, cy, cz)];
}

void Sim::stepFluids(float tick) {
  if (fluidActive.empty()) return;
  std::vector<int64_t> list(fluidActive.begin(), fluidActive.end());
  fluidActive.clear();
  // Bottom-up so columns fall together.
  std::sort(list.begin(), list.end());
  const size_t cap = 220000;
  if (list.size() > cap) {
    for (size_t i = cap; i < list.size(); i++) fluidActive.insert(list[i]);
    list.resize(cap);
  }
  std::vector<int64_t> water;
  for (int64_t k : list) {
    ivec3 p = unpackKey(k);
    if (world.mat(p.x, p.y, p.z) == WATER) water.push_back(k);
  }
  if (!water.empty()) {
    // Several sub-steps a tick, so water levels out at a believable pace.
    const int SUB = 4;
    stepWater(water, tick / SUB);
    for (int s = 1; s < SUB; s++) {
      std::vector<int64_t> again;
      for (int64_t k : fluidActive) { ivec3 p = unpackKey(k); if (world.mat(p.x, p.y, p.z) == WATER) again.push_back(k); }
      if (again.empty()) break;
      for (int64_t k : again) fluidActive.erase(k);
      std::sort(again.begin(), again.end());
      stepWater(again, tick / SUB);
    }
  }
  static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  auto activateAround = [&](int x, int y, int z) {
    for (int dy = 0; dy <= 1; dy++)
      for (auto& d : DIRS6) {
        int xx = x + d[0], yy = y + d[1] + dy, zz = z + d[2];
        uint8_t m = world.mat(xx, yy, zz);
        if (m && isFluidish(m)) fluidActive.insert(packKey(xx, yy, zz));
      }
  };
  auto move = [&](int x, int y, int z, int nx, int ny, int nz) {
    Cell c = world.get(x, y, z);
    world.put(nx, ny, nz, c);
    world.set(x, y, z, AIR);
    fluidActive.insert(packKey(nx, ny, nz));
    activateAround(x, y, z);
    if (c.temp != AMBIENT) markHot(world, worldHot, nx, ny, nz);
  };
  // Molten material, sand, ash and gravel move as whole voxels.
  for (int64_t k : list) {
    ivec3 p = unpackKey(k);
    int x = p.x, y = p.y, z = p.z;
    uint8_t m = world.mat(x, y, z);
    if (!m || m == WATER || !isFluidish(m) || y <= 0) continue;
    if (world.mat(x, y - 1, z) == AIR) { move(x, y, z, x, y - 1, z); continue; }
    bool liquid = MATS[m].phase == LIQUID;
    if (m == MOLTEN && rng().f() < 0.6f) { fluidActive.insert(k); continue; }  // viscous
    int r = rng().next() & 3;
    bool moved = false;
    for (int i = 0; i < 4 && !moved; i++) {
      int dx = dirs[(i + r) & 3][0], dz = dirs[(i + r) & 3][1];
      if (world.mat(x + dx, y, z + dz) == AIR && world.mat(x + dx, y - 1, z + dz) == AIR && world.inb(x + dx, y - 1, z + dz)) {
        // Powders only slide if the slope is steep (angle of repose ~ 35-45 degrees).
        if (!liquid && rng().f() < 0.25f) { fluidActive.insert(k); continue; }
        move(x, y, z, x + dx, y - 1, z + dz);
        moved = true;
      }
    }
    if (moved || !liquid) continue;
    uint8_t above = world.mat(x, y + 1, z);
    bool pressured = above && MATS[above].phase == LIQUID;
    for (int i = 0; i < 4 && !moved; i++) {
      int dx = dirs[(i + r) & 3][0], dz = dirs[(i + r) & 3][1];
      if (!world.inb(x + dx, y, z + dz) || world.mat(x + dx, y, z + dz) != AIR) continue;
      uint8_t under = world.mat(x + dx, y - 1, z + dz);
      if (pressured || under == AIR || MATS[under].phase == LIQUID) { move(x, y, z, x + dx, y, z + dz); moved = true; }
    }
  }
}

void Sim::stepDebris(float dt) {
  const float drag = 0.05f;
  for (size_t i = 0; i < debris.size();) {
    Debris& d = debris[i];
    d.life -= dt;
    bool kill = d.life <= 0;
    bool deposit = false;
    if (!kill) {
      d.v += gravity * dt;
      d.v *= (1 - drag * dt);
      // Aerodynamic drag towards the local wind (light flakes drift, rubble barely notices).
      if (airReady) {
        vec3 rel = air.velocity(d.p) - d.v;
        float k = 0.6f * vs * vs / std::max(1e-6f, MATS[d.mat].voxMass);
        d.v += rel * std::min(1.f, k * length(rel) * dt);
      }
      // Hot debris cools by radiation as it flies (sparks of molten metal).
      if (d.temp > 200) d.temp -= (d.temp - AMBIENT) * 0.08f * dt * cfg.thermalWarp * 0.1f;
      // March in sub-voxel steps so fast fragments can't tunnel through thin walls.
      vec3 np = d.p + d.v * dt;
      {
        int n = std::min(64, (int)std::ceil(length(d.v) * dt / (0.8f * vs)));
        for (int k = 1; k < n; k++) {
          vec3 ps = d.p + d.v * (dt * k / n);
          if (world.matI(cellOf(ps)) != AIR) { np = ps; break; }
        }
        if (n > 1) {
          // Back up to the last free point before whatever we reached.
          vec3 step = d.v * (dt / n);
          vec3 prev = np - step;
          if (world.matI(cellOf(np)) != AIR && world.matI(cellOf(prev)) == AIR) d.p = prev;
        }
      }
      ivec3 c = cellOf(np);
      uint8_t m = world.matI(c);
      bool solidHit = m != AIR && !isLiquid(m) && world.inb(c.x, c.y, c.z);
      if (!world.inb(c.x, c.y, c.z) && c.y < 0) solidHit = true;
      bool liquidDeb = isLiquid(d.mat);
      if (d.mat == WATER && m == WATER) {
        // A falling water voxel landing in water joins it at its surface.
        placeWater(c, d.temp);
        if (rng().f() < 0.05f && length(d.v) > 2) emit(FX_SPRAY, np, vec3(0, 1.0f, 0) + rng().unitVec() * 0.4f, 0.4f, vs);
        d = debris.back(); debris.pop_back();
        continue;
      }
      if (m != AIR && isLiquid(m) && !liquidDeb) {
        d.v *= (1 - 4 * dt);  // splash and sink
        if (rng().f() < 0.02f) emit(FX_SPRAY, np, vec3(0, 1.2f, 0), 0.4f, vs);
      }
      if (solidHit) {
        // Water hitting something hot or burning cools it and may flash to steam.
        if (d.mat == WATER && world.inb(c.x, c.y, c.z)) {
          Brick* br = world.brick(c.x, c.y, c.z);
          int l = lidx(c.x, c.y, c.z);
          if (br && br->mat[l]) {
            float m_w = MATS[WATER].voxMass;
            if (br->temp[l] > 100 || (br->flags[l] & F_BURN)) {
              float absorb = m_w * (4186.f * 80.f + 2.26e6f);  // heat to boil it off
              br->temp[l] = std::max(AMBIENT, br->temp[l] - absorb / MATS[br->mat[l]].heatCap);
              if (br->flags[l] & F_BURN) { br->flags[l] &= ~F_BURN; world.markDirty(c.x, c.y, c.z); }
              // Damp neighbours too: water runs over the surface.
              for (auto& dd : DIRS6) {
                int xx = c.x + dd[0], yy = c.y + dd[1], zz = c.z + dd[2];
                if (!world.inb(xx, yy, zz)) continue;
                Brick* nb = world.brick(xx, yy, zz);
                if (!nb) continue;
                int nl = lidx(xx, yy, zz);
                if (nb->flags[nl] & F_BURN) { nb->flags[nl] &= ~F_BURN; world.markDirty(xx, yy, zz); }
                nb->temp[nl] = std::max(AMBIENT, nb->temp[nl] - 0.3f * absorb / std::max(1e-6f, MATS[nb->mat[nl]].heatCap));
              }
              worldHot.add(world.bindex(c.x, c.y, c.z));
              if (rng().f() < 0.3f) emit(FX_STEAM, d.p, vec3(0, 0.8f, 0), 1.5f, vs * 4);
              kill = true;
            }
          }
        }
        if (!kill) {
          float speed = length(d.v);
          if (liquidDeb || speed < 0.9f || d.bounces >= 3 || d.mat == ASH || isPowder(d.mat)) {
            deposit = true;
          } else {
            // Bounce off the face we crossed.
            ivec3 pc = cellOf(d.p);
            vec3 n(0);
            if (pc.x != c.x) n.x = (float)(pc.x - c.x);
            if (pc.y != c.y) n.y = (float)(pc.y - c.y);
            if (pc.z != c.z) n.z = (float)(pc.z - c.z);
            if (length2(n) == 0) n = vec3(0, 1, 0);
            n = normalize(n);
            float vn = dot(d.v, n);
            float e = MATS[d.mat].restitution * 0.8f + 0.1f;
            d.v = (d.v - n * vn) * 0.6f - n * vn * e;
            d.bounces++;
            if (speed > 4 && rng().f() < 0.15f && MATS[d.mat].metal > 0.5f) emit(FX_SPARK, d.p, d.v * 0.3f + rng().unitVec(), 0.3f, vs * 0.5f, vec3(3, 2, 1));
          }
        }
      } else {
        d.p = np;
      }
    }
    // Water is never lost: a voxel whose time runs out lands where it is.
    if (kill && !deposit && d.mat == WATER && d.life <= 0) { kill = false; deposit = true; }
    if (deposit && d.mat == WATER) {
      ivec3 c = cellOf(d.p);
      placeWater(c, d.temp);
      kill = true;
      deposit = false;
    }
    if (deposit) {
      // Settle into the nearest free cell at or above where it landed.
      ivec3 c = cellOf(d.p);
      bool placed = false;
      for (int up = 0; up < 4 && !placed; up++) {
        ivec3 q(c.x, c.y + up, c.z);
        if (world.inb(q.x, q.y, q.z) && world.matI(q) == AIR) {
          Cell cell;
          cell.mat = d.mat; cell.temp = d.temp; cell.comp = d.comp; cell.aux = d.aux;
          // A lone fragment is loose rubble: no longer part of its original part.
          if (cell.comp && comp(cell.comp).type != C_TERRAIN) cell.comp = 0;
          cell.hp = 200;
          putWorld(q.x, q.y, q.z, cell);
          if (isFluidish(d.mat)) fluidActive.insert(packKey(q.x, q.y, q.z));
          placed = true;
        }
      }
      kill = true;
    }
    if (kill) { d = debris.back(); debris.pop_back(); continue; }
    i++;
  }
}
