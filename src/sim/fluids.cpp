// Liquids flow and powders slump on the world grid; loose debris flies
// ballistically and settles back into the world where it lands.
#include <algorithm>
#include <unordered_map>
#include "sim.h"

// Water is simulated by mass rather than as whole voxels. Each water cell
// holds a fill (stored in the otherwise unused fuel field; 0 means full) and
// cells under others may hold slightly more than full. That small
// compression is pressure: it pushes water sideways and up, so levels even
// out, water rises in a connected pipe to the height of its tank, and a hole
// low in a tank squirts harder than one near the top. Water with nothing
// beneath it leaves the grid as falling droplets and splashes back in where
// it lands, and pressured water escaping into open air becomes a jet with
// Torricelli's speed, sqrt(2 g h).
namespace {
constexpr float W_COMP = 0.02f;   // extra fill per cell of depth
constexpr float W_MIN = 0.004f;   // less than this evaporates into the cells around
constexpr float W_FLOW = 1.0f;    // most fill moved per tick along one link
inline float fillOf(float f) { return f > 0 ? f : 1.f; }
// How much of a two-cell total the lower cell holds when settled.
inline float stableLower(float total) {
  if (total <= 1) return 1;
  if (total < 2 + W_COMP) return (1 + total * W_COMP) / (1 + W_COMP);
  return (total + W_COMP) * 0.5f;
}
}  // namespace

void Sim::settleWater() {
  // Give standing water its hydrostatic fill so ponds start at rest.
  world.forEach([&](int x, int y, int z, Brick* br, int l) {
    if (br->mat[l] != WATER) return;
    int depth = 0;
    while (depth < 400 && world.mat(x, y + depth + 1, z) == WATER) depth++;
    br->fuel[l] = 1 + W_COMP * depth;
  });
}

double g_wlost[10];
// Horizontal water velocity is kept per cell in two spare bytes.
namespace {
constexpr float W_VQ = 0.04f;  // m/s per step
inline vec3 wvel(const Brick* b, int l) {
  if (!(b->flags[l] & F_WVEL)) return vec3(0);
  return vec3((b->hp[l] - 128) * W_VQ, 0, (b->aux[l] - 128) * W_VQ);
}
inline void setWvel(Brick* b, int l, vec3 v) {
  // Truncate towards zero so slow drift dies out instead of rounding back up.
  int qx = std::clamp((int)(v.x / W_VQ), -127, 127), qz = std::clamp((int)(v.z / W_VQ), -127, 127);
  if (qx == 0 && qz == 0) { b->flags[l] &= ~F_WVEL; b->hp[l] = 255; b->aux[l] = 0; return; }
  b->flags[l] |= F_WVEL; b->hp[l] = (uint8_t)(qx + 128); b->aux[l] = (uint8_t)(qz + 128);
}
}  // namespace

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
    if (colStack.empty()) return 0.f;
    // Water squeezed in beyond its column's weight pushes back too.
    float f = fillAt(x, y, z);
    float h = std::max(headMemo[k0], f > 1 ? ((f - 1) / W_COMP + 1) * vs : 0.f);
    headMemo[k0] = h;
    return h;
  };
  struct Acc { float dm = 0, inM = 0, inMT = 0; vec3 inMV; vec3 selfV; bool self = false; };
  std::unordered_map<int64_t, Acc> acc;
  acc.reserve(list.size() * 2);
  if (waterFlow.size() != air.T.size()) { waterFlow.assign(air.T.size(), vec3(0)); waterFlowN.assign(air.T.size(), 0); }
  std::fill(waterFlowN.begin(), waterFlowN.end(), 0.f);
  std::vector<vec3> flowNew(waterFlow.size(), vec3(0));
  auto coarse = [&](int x, int y, int z) {
    int cx = x / air.cellVox, cy = y / air.cellVox, cz = z / air.cellVox;
    if (cx < 0 || cy < 0 || cz < 0 || cx >= air.nx || cy >= air.ny || cz >= air.nz) return -1;
    return air.cid(cx, cy, cz);
  };
  struct Drop { vec3 p, v; float fill, temp; };
  std::vector<Drop> drops;
  const float damp = std::pow(0.5f, tick / 1.5f);  // sloshing dies down over a few seconds
  for (int64_t k : list) {
    ivec3 q = unpackKey(k);
    int x = q.x, y = q.y, z = q.z;
    Brick* br = world.brick(x, y, z);
    if (!br) continue;
    int l = lidx(x, y, z);
    if (br->mat[l] != WATER) continue;
    float T = br->temp[l];
    float f0 = fillOf(br->fuel[l]);
    float rem = f0;
    vec3 vel = wvel(br, l);
    Acc& self = acc[k];
    int cc = coarse(x, y, z);
    if (cc >= 0) waterFlowN[cc] += 1;
    ivec3 best(0, 0, 0);
    float bestF = 0;
    auto move = [&](int nx_, int ny_, int nz_, float f, const vec3& v, bool force = false) {
      if (f <= 0) return;
      if (!force && f < W_MIN && world.mat(nx_, ny_, nz_) == AIR) return;  // too little to wet a new cell
      if (f > bestF) { bestF = f; best = ivec3(nx_, ny_, nz_); }
      rem -= f;
      self.dm -= f;
      Acc& o = acc[packKey(nx_, ny_, nz_)];
      o.dm += f; o.inM += f; o.inMT += f * T; o.inMV += v * f;
      if (cc >= 0) flowNew[cc] += vec3((float)(nx_ - x), (float)(ny_ - y), (float)(nz_ - z)) * (f * vs / tick);
    };
    // Nothing under it: it falls freely as a droplet.
    float below = fillAt(x, y - 1, z);
    if (below == 0 && fillAt(x, y - 2, z) == 0 && fillAt(x, y + 1, z) <= 0) {
      drops.push_back({cellCenter(x, y, z), vel + vec3(rng().range(-.05f, .05f), -0.8f, rng().range(-.05f, .05f)), rem, T});
      self.dm -= rem;
      continue;
    }
    // Down: the lower cell fills first (and takes a little extra under load).
    if (below >= 0) {
      float f = std::clamp(stableLower(rem + below) - below, 0.f, std::min(rem, W_FLOW));
      move(x, y - 1, z, f, vel);
    }
    // Sideways: the pressure difference accelerates the water, and the water
    // carries its momentum, so it surges, spreads at wave speed and sloshes.
    float hE = fillAt(x + 1, y, z), hW = fillAt(x - 1, y, z), hN = fillAt(x, y, z + 1), hS = fillAt(x, y, z - 1);
    float hs = headAt(x, y, z);
    auto hOf = [&](float f, int dx, int dz) { return f < 0 ? hs : f == 0 ? 0.f : headAt(x + dx, y, z + dz); };
    float hEv = hOf(hE, 1, 0), hWv = hOf(hW, -1, 0), hNv = hOf(hN, 0, 1), hSv = hOf(hS, 0, -1);
    vel.x += -g * (hEv - hWv) / (2 * vs) * tick;
    vel.z += -g * (hNv - hSv) / (2 * vs) * tick;
    // Viscosity: velocities relax towards their neighbours', which damps the
    // grid-scale ripples a collocated scheme otherwise lets grow.
    {
      vec3 sumv(0);
      int nv = 0;
      const int nb4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (auto& d : nb4) {
        const Brick* nb = world.brick(x + d[0], y, z + d[1]);
        if (!nb) continue;
        int nl = lidx(x + d[0], y, z + d[1]);
        if (nb->mat[nl] != WATER) continue;
        sumv += wvel(nb, nl);
        nv++;
      }
      if (nv) vel = lerp(vel, sumv / (float)nv, 0.3f);
    }
    // Thin films drag on the ground.
    vel *= damp * (f0 < 0.25f ? std::pow(0.5f, tick / 0.4f) : 1.f);
    if ((vel.x > 0 && hE < 0) || (vel.x < 0 && hW < 0)) vel.x = 0;
    if ((vel.z > 0 && hN < 0) || (vel.z < 0 && hS < 0)) vel.z = 0;
    vel = vec3(0);  // levelling is pressure-driven; momentum lives in droplets and jets
    float vmax = 0.9f * vs / tick;  // keep each step within one cell
    float sp = length(vel);
    if (sp > vmax) vel *= vmax / sp;
    if (rem > 0) {
      float avail = std::min(rem, 1.f);
      struct Out2 { int dx, dz; float f, nf, h; };
      Out2 outs[4] = {{1, 0, std::max(0.f, vel.x), hE, hEv}, {-1, 0, std::max(0.f, -vel.x), hW, hWv}, {0, 1, std::max(0.f, vel.z), hN, hNv}, {0, -1, std::max(0.f, -vel.z), hS, hSv}};
      float sum = 0;
      for (auto& o : outs) {
        // Advective flux plus a face-wise flux down the head difference (which
        // also stops the collocated velocities settling into a checkerboard).
        o.f = o.nf < 0 ? 0 : o.f * tick / vs * avail + std::clamp((hs - o.h) / vs, 0.f, 1.f) * 0.18f;
        sum += o.f;
      }
      float scale = sum > avail * 0.9f ? avail * 0.9f / sum : 1.f;
      for (auto& o : outs) {
        float f = o.f * scale;
        if (f < 0.002f) continue;
        int nx_ = x + o.dx, nz_ = z + o.dz;
        if (o.nf == 0 && fillAt(nx_, y - 1, nz_) == 0) {
          // Over an edge or out of a hole: it leaves as a jet.
          vec3 dir((float)o.dx, 0, (float)o.dz);
          vec3 v0 = vel;
          // Water in a hole is pushed by the tank behind it: use the highest head around.
          float hj = std::max({hs, hEv, hWv, hNv, hSv});
          if (hj > 1.5f * vs) v0 = dir * std::sqrt(2 * g * (hj - 0.5f * vs));  // Torricelli
          else v0 += dir * 0.3f;
          drops.push_back({cellCenter(x, y, z) + dir * (vs * 0.9f), v0 + vec3(rng().range(-.06f, .06f), rng().range(-.04f, .04f), rng().range(-.06f, .06f)), f, T});
          rem -= f; self.dm -= f;
          if (cc >= 0) flowNew[cc] += v0;
          continue;
        }
        move(nx_, y, nz_, f, vel);
      }
    }
    // Up, only under pressure.
    if (rem > 1) {
      float up = fillAt(x, y + 1, z);
      if (up >= 0) {
        float f = std::clamp(rem - stableLower(rem + up), 0.f, std::min(rem, W_FLOW));
        move(x, y + 1, z, f, vel);
      }
    }
    // Don't leave a scrap too thin to keep: it goes with the main flow.
    if (rem > 0 && rem < W_MIN && bestF > 0) move(best.x, best.y, best.z, rem, vel, true);
    self.selfV = vel;
    self.self = true;
    if (f0 - rem > 1e-4f || length2(vel) > W_VQ * W_VQ) {
      fluidActive.insert(k);  // still moving: stay awake
      static int dbgN = 0;
      if (getenv("REALISM_DEBUG_WATER") && x > 100 && (++dbgN % 20000) == 0)
        fprintf(stderr, "awake %d %d %d f0 %.4f moved %.5f vel %.3f %.3f hs %.3f\n", x, y, z, f0, f0 - rem, vel.x, vel.z, hs);
    }
  }
  // Apply.
  auto activate = [&](int x, int y, int z) {
    for (int dy = -1; dy <= 1; dy++)
      for (auto& d : DIRS6) {
        int xx = x + d[0], yy = y + d[1] + dy, zz = z + d[2];
        uint8_t m = world.mat(xx, yy, zz);
        if (m && isFluidish(m)) fluidActive.insert(packKey(xx, yy, zz));
      }
  };
  for (auto& [key, a] : acc) {
    ivec3 q = unpackKey(key);
    int x = q.x, y = q.y, z = q.z;
    Brick* br = world.brick(x, y, z);
    int l = lidx(x, y, z);
    uint8_t m = br ? br->mat[l] : (uint8_t)AIR;
    if (m == AIR) {
      if (a.dm < W_MIN) { g_wlost[0] += a.dm; continue; }  // a trickle too thin to hold
      Cell c;
      c.mat = WATER; c.temp = a.inMT / std::max(1e-6f, a.inM); c.fuel = a.dm;
      world.put(x, y, z, c);
      Brick* nb = world.brick(x, y, z);
      setWvel(nb, l, a.inMV / std::max(1e-6f, a.inM));
      if (c.temp != AMBIENT) markHot(world, worldHot, x, y, z);
      fluidActive.insert(key);
      activate(x, y, z);
      continue;
    }
    if (m != WATER) { g_wlost[1] += a.dm; continue; }  // something else moved in this tick
    float old = fillOf(br->fuel[l]);
    float nf = old + a.dm;
    if (nf < W_MIN) {
      g_wlost[2] += nf;
      world.set(x, y, z, AIR);
      activate(x, y, z);
      continue;
    }
    float stayed = std::max(0.f, nf - a.inM);
    vec3 v0 = a.self ? a.selfV : wvel(br, l);
    vec3 nv = (v0 * stayed + a.inMV) / std::max(1e-6f, stayed + a.inM);
    if (a.inM > 0) {
      br->temp[l] = (stayed * br->temp[l] + a.inMT) / std::max(1e-6f, stayed + a.inM);
      if (br->temp[l] != AMBIENT) markHot(world, worldHot, x, y, z);
    }
    setWvel(br, l, nv);
    uint8_t ob = (uint8_t)(std::min(1.f, old) * 255), nb = (uint8_t)(std::min(1.f, nf) * 255);
    br->fuel[l] = nf;
    if (ob != nb) world.markDirty(x, y, z);
    if (std::fabs(a.dm) > 2e-4f || length2(nv) > W_VQ * W_VQ) { fluidActive.insert(key); activate(x, y, z); }
  }
  if (getenv("REALISM_DEBUG_WATER")) {
    double bal = 0, dr = 0;
    for (auto& [key, a] : acc) bal += a.dm;
    for (auto& d : drops) dr += d.fill;
    if (std::fabs(bal + dr) > 0.01) fprintf(stderr, "water imbalance %.3f (dm %.3f drops %.3f)\n", bal + dr, bal, dr);
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
