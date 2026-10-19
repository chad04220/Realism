// Liquids flow and powders slump on the world grid; loose debris flies
// ballistically and settles back into the world where it lands.
#include <algorithm>
#include <unordered_map>
#include "sim.h"

// Water is simulated by volume rather than as whole voxels. Each water cell
// holds a fill from 0 to 1 (stored in the otherwise unused fuel field; 0
// means full). Pressure is the depth of the water standing on a cell, so
// water flows sideways from a taller column to a shorter one; pushed into a
// full cell it rises to the top of that column, which is how a pond finds
// one level and water in a connected pipe climbs to the height of its tank.
// Water with nothing beneath it leaves the grid as falling droplets and
// splashes back in where it lands, and water escaping from under a head of
// water into open air becomes a jet with Torricelli's speed, sqrt(2 g h).
namespace {
constexpr float W_MIN = 0.004f;   // thinner than this can't wet a new cell
inline float fillOf(float f) { return f > 0 ? f : 1.f; }
}  // namespace

void Sim::settleWater() {
  // Standing water starts full and at rest.
  world.forEach([&](int, int, int, Brick* br, int l) {
    if (br->mat[l] == WATER) br->fuel[l] = 0;
  });
}

double g_wlost[10];

void Sim::stepWater(std::vector<int64_t>& list, float tick) {
  const float g = 9.81f;
  // Fill of a cell: -1 blocked, 0 open air, otherwise the water's fill.
  auto fillAt = [&](int x, int y, int z) -> float {
    if (!world.inb(x, y, z)) return -1;
    const Brick* b = world.brick(x, y, z);
    if (!b) return 0;
    int l = lidx(x, y, z);
    uint8_t m = b->mat[l];
    if (m == AIR) return 0;
    if (m == WATER) return fillOf(b->fuel[l]);
    return -1;
  };
  // Pressure head at a cell's floor: the depth of water standing on it,
  // counted up the column (memoised for this step).
  std::unordered_map<int64_t, float> headMemo;
  headMemo.reserve(list.size() * 2);
  std::vector<std::pair<int64_t, float>> colStack;
  auto headAt = [&](int x, int y, int z) -> float {
    int64_t k0 = packKey(x, y, z);
    auto it = headMemo.find(k0);
    if (it != headMemo.end()) return it->second;
    colStack.clear();
    float above = 0;
    int yy = y;
    while (true) {
      int64_t kk = packKey(x, yy, z);
      auto jt = headMemo.find(kk);
      if (jt != headMemo.end()) { above = jt->second; break; }
      float f = fillAt(x, yy, z);
      if (f <= 0) { above = 0; break; }
      colStack.push_back({kk, std::min(f, 1.f) * vs});
      if (f < 0.98f || yy - y > 400) { above = 0; break; }
      yy++;
    }
    for (int i = (int)colStack.size() - 1; i >= 0; i--) {
      above += colStack[i].second;
      headMemo[colStack[i].first] = above;
    }
    return colStack.empty() ? 0.f : headMemo[k0];
  };
  // The first cell up a column, starting at y, with room for water
  // (open air or a part-filled cell); -1 if something solid caps it.
  auto topOf = [&](int x, int y, int z) -> int {
    for (int yy = y; yy < y + 400; yy++) {
      float f = fillAt(x, yy, z);
      if (f < 0) return -1;
      if (f < 0.999f) return yy;
    }
    return -1;
  };
  if (waterFlow.size() != air.T.size()) { waterFlow.assign(air.T.size(), vec3(0)); waterFlowN.assign(air.T.size(), 0); }
  std::fill(waterFlowN.begin(), waterFlowN.end(), 0.f);
  std::vector<vec3> flowNew(waterFlow.size(), vec3(0));
  auto coarse = [&](int x, int y, int z) {
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
  // Store a fill, telling the renderer when the visible level changes.
  auto storeFill = [&](Brick* b, int l, int x, int y, int z, float oldF, float f) {
    b->fuel[l] = std::fabs(f - 1) < 1e-4f ? 0.f : f;  // 0 means exactly full
    if ((int)(std::min(1.f, oldF) * 255) != (int)(std::min(1.f, f) * 255)) world.markDirty(x, y, z);
  };
  struct Drop { vec3 p, v; float fill, temp; };
  std::vector<Drop> drops;
  // Moves are applied as they are decided (in place), each limited by the
  // room left in its target, so water is conserved exactly and no cell is
  // ever filled past full by several neighbours at once.
  for (int64_t k : list) {
    ivec3 q = unpackKey(k);
    int x = q.x, y = q.y, z = q.z;
    Brick* br = world.brick(x, y, z);
    if (!br) continue;
    int l = lidx(x, y, z);
    if (br->mat[l] != WATER) continue;
    const float T = br->temp[l];
    const float f0 = fillOf(br->fuel[l]);
    float rem = f0;
    int cc = coarse(x, y, z);
    if (cc >= 0) waterFlowN[cc] += 1;
    ivec3 best(0, 0, 0);
    float bestF = 0;
    auto give = [&](int nx_, int ny_, int nz_, float f, bool tiny = false) -> float {
      float tf = fillAt(nx_, ny_, nz_);
      if (tf < 0) return 0;
      f = std::min({f, rem, 1.f - tf});
      if (f <= 1e-4f) return 0;
      if (tf == 0) {
        if (f < W_MIN && !tiny) return 0;  // too little to wet a new cell
        Cell c;
        c.mat = WATER; c.temp = T; c.fuel = f >= 0.9999f ? 0.f : f;
        world.put(nx_, ny_, nz_, c);
      } else {
        Brick* nb = world.brick(nx_, ny_, nz_);
        int nl = lidx(nx_, ny_, nz_);
        nb->temp[nl] = (nb->temp[nl] * tf + T * f) / (tf + f);
        storeFill(nb, nl, nx_, ny_, nz_, tf, tf + f);
      }
      if (T != AMBIENT) markHot(world, worldHot, nx_, ny_, nz_);
      rem -= f;
      if (f > bestF) { bestF = f; best = ivec3(nx_, ny_, nz_); }
      fluidActive.insert(packKey(nx_, ny_, nz_));
      activate(nx_, ny_, nz_);
      if (cc >= 0) flowNew[cc] += vec3((float)(nx_ - x), (float)(ny_ - y), (float)(nz_ - z)) * (f * vs / tick);
      return f;
    };
    float below = fillAt(x, y - 1, z);
    if (below == 0 && rem >= 0.03f && fillAt(x, y - 2, z) == 0 && fillAt(x, y + 1, z) <= 0) {
      // Nothing under it: it falls freely as a droplet.
      drops.push_back({cellCenter(x, y, z), vec3(rng().range(-.05f, .05f), -0.8f, rng().range(-.05f, .05f)), rem, T});
      world.set(x, y, z, AIR);
      activate(x, y, z);
      continue;
    }
    // Down: the cell below fills first.
    if (below >= 0 && below < 1) give(x, y - 1, z, rem, true);
    // Overfull (a droplet landed in it): the excess rises up the column.
    if (rem > 1) {
      int t = topOf(x, y + 1, z);
      if (t >= 0) give(x, t, z, rem - 1, true);
    }
    // Sideways, down the difference in head.
    if (rem > 0) {
      float hs = headAt(x, y, z);
      struct Out { int dx, dz; float nf, h, f; };
      Out outs[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      float sum = 0, hj = hs;
      for (auto& o : outs) {
        o.nf = fillAt(x + o.dx, y, z + o.dz);
        o.h = o.nf < 0 ? hs : o.nf == 0 ? 0.f : headAt(x + o.dx, y, z + o.dz);
        if (o.nf > 0) hj = std::max(hj, o.h);
        // Two water columns trade only at the highest level where both hold
        // water; otherwise every cell down the column would move the same
        // surface difference and the pond would slosh forever.
        bool handledAbove = o.nf > 0 && fillAt(x, y + 1, z) > 0 && fillAt(x + o.dx, y + 1, z + o.dz) > 0;
        // A small dead band lets level water come to rest.
        o.f = (o.nf < 0 || handledAbove) ? 0 : std::clamp((hs - o.h) / vs - 0.02f, 0.f, 1.f) * 0.24f;
        sum += o.f;
      }
      float avail = std::min(rem, 1.f);
      float scale = sum > avail * 0.9f ? avail * 0.9f / sum : 1.f;
      int r0 = rng().next() & 3;
      for (int i = 0; i < 4; i++) {
        Out& o = outs[(i + r0) & 3];
        float f = o.f * scale;
        if (f < 0.003f) continue;
        int nx_ = x + o.dx, nz_ = z + o.dz;
        if (o.nf == 0 && f >= 0.03f && fillAt(nx_, y - 1, nz_) == 0 && fillAt(nx_, y - 2, nz_) == 0) {
          // Over an edge or out of a hole: it leaves as a jet, pushed by the
          // water standing behind it (Torricelli).
          vec3 dir((float)o.dx, 0, (float)o.dz);
          vec3 v0 = hj > 1.5f * vs ? dir * std::sqrt(2 * g * (hj - 0.5f * vs)) : dir * 0.3f;
          f = std::min(f, rem);
          drops.push_back({cellCenter(x, y, z) + dir * (vs * 0.9f), v0 + vec3(rng().range(-.06f, .06f), rng().range(-.04f, .04f), rng().range(-.06f, .06f)), f, T});
          rem -= f;
          if (cc >= 0) flowNew[cc] += v0;
          continue;
        }
        if (o.nf >= 0.999f) {
          // Into a full cell: it rises to the top of that column.
          int t = topOf(nx_, y, nz_);
          if (t >= 0) give(nx_, t, nz_, f);
          continue;
        }
        give(nx_, y, nz_, f);
      }
    }
    // Write back what's left. A scrap too thin to keep goes with the main flow.
    br = world.brick(x, y, z);
    if (rem < W_MIN && rem > 0 && bestF > 0) give(best.x, best.y, best.z, rem, true);
    if (rem < W_MIN && rem > 1e-5f) {
      // Otherwise it joins any water touching it.
      const int nb[5][3] = {{0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
      for (auto& d : nb)
        if (rem > 1e-5f && fillAt(x + d[0], y + d[1], z + d[2]) > 0) give(x + d[0], y + d[1], z + d[2], rem, true);
    }
    if (rem <= 1e-5f) {
      if (rem > 0) g_wlost[2] += rem;
      world.set(x, y, z, AIR);
      activate(x, y, z);
      continue;
    }
    storeFill(br, l, x, y, z, f0, rem);
    if (f0 - rem > 1e-3f) { fluidActive.insert(k); activate(x, y, z); g_wlost[9] += 1; }
  }
  if (getenv("REALISM_DEBUG_WATER")) {
    static int pn = 0;
    static double proc = 0, moved = 0;
    proc += list.size(); moved += g_wlost[9]; g_wlost[9] = 0;
    if (++pn % 135 == 0) { fprintf(stderr, "water passes: %.0f cells/pass, %.0f moving/pass, total active set %zu\n", proc / 135, moved / 135, fluidActive.size()); proc = moved = 0; }
  }
  if (getenv("REALISM_DEBUG_WATER")) {
    static int dn = 0;
    for (auto& d : drops)
      if ((++dn % 4000) == 0) {
        ivec3 c = cellOf(d.p);
        fprintf(stderr, "drop at %d %d %d v %.2f %.2f %.2f fill %.3f | below %d %d above %d\n", c.x, c.y, c.z, d.v.x, d.v.y, d.v.z, d.fill,
                world.mat(c.x, c.y - 1, c.z), world.mat(c.x, c.y - 2, c.z), world.mat(c.x, c.y + 1, c.z));
      }
  }
  for (auto& d : drops) {
    if (d.fill < W_MIN) { g_wlost[3] += d.fill; continue; }
    Cell c;
    c.mat = WATER; c.temp = d.temp;
    spawnDebris(d.p, d.v, c);
    if (!debris.empty()) { debris.back().fill = d.fill; debris.back().life = 8; }
    g_wlost[8] += d.fill;
    ivec3 q = cellOf(d.p);
    activate(q.x, q.y, q.z);
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
    const int SUB = 3;
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
        // A droplet falling into water joins it.
        Brick* wb = world.brick(c.x, c.y, c.z);
        int wl = lidx(c.x, c.y, c.z);
        float f0 = wb->fuel[wl] > 0 ? wb->fuel[wl] : 1.f;
        wb->temp[wl] = (wb->temp[wl] * f0 + d.temp * d.fill) / (f0 + d.fill);
        wb->fuel[wl] = f0 + d.fill;
        g_wlost[6] += d.fill;
        fluidActive.insert(packKey(c.x, c.y, c.z));
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
    if (deposit) {
      // Settle into the nearest free cell at or above where it landed.
      ivec3 c = cellOf(d.p);
      bool placed = false;
      for (int up = 0; up < 4 && !placed; up++) {
        ivec3 q(c.x, c.y + up, c.z);
        if (d.mat == WATER && world.matI(q) == WATER) {
          // Water has flowed in under the droplet: it simply joins.
          Brick* wb = world.brick(q.x, q.y, q.z);
          int wl = lidx(q.x, q.y, q.z);
          float f0 = wb->fuel[wl] > 0 ? wb->fuel[wl] : 1.f;
          wb->fuel[wl] = f0 + d.fill;
          fluidActive.insert(packKey(q.x, q.y, q.z));
          placed = true;
          break;
        }
        if (world.inb(q.x, q.y, q.z) && world.matI(q) == AIR) {
          Cell cell;
          cell.mat = d.mat; cell.temp = d.temp; cell.comp = d.comp; cell.aux = d.aux;
          // A lone fragment is loose rubble: no longer part of its original part.
          if (cell.comp && comp(cell.comp).type != C_TERRAIN) cell.comp = 0;
          cell.hp = 200;
          if (d.mat == WATER) { cell.fuel = d.fill; g_wlost[7] += d.fill; }
          putWorld(q.x, q.y, q.z, cell);
          if (isFluidish(d.mat)) fluidActive.insert(packKey(q.x, q.y, q.z));
          placed = true;
        }
      }
      if (!placed && d.mat == WATER) g_wlost[4] += d.fill;
      kill = true;
    } else if (kill && d.mat == WATER) g_wlost[5] += d.fill;
    if (kill) { d = debris.back(); debris.pop_back(); continue; }
    i++;
  }
}
