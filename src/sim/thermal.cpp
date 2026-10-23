// Heat: conduction between touching voxels (harmonic-mean conductance),
// convection and radiation to the air, combustion with oxygen from exposed
// faces, convective plumes above fires, and phase changes (melting, freezing,
// boiling), plus the things heat sets off: charges, rockets, battery runaway.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include "../core/jobs.h"
#include "sim.h"

namespace {

enum TEv : uint8_t { T_IGNITE, T_BURNOUT, T_MELT, T_FREEZE, T_BOIL, T_DETONATE, T_ROCKET, T_RUNAWAY, T_EXTINGUISH, T_THAW };
struct ThermEvent { ivec3 c; TEv kind; };

constexpr float SIGMA = 5.670e-8f, EMISS = 0.85f;

struct Scratch {
  std::vector<float> temps;   // new temperatures, 4096 per hot brick
  std::vector<uint8_t> keep;  // brick still active
};

}  // namespace

static void simulateVolume(Sim& s, Volume& v, HotSet& hs, float dt, Body* owner, int& burning, int& hot) {
  if (hs.list.empty()) return;
  std::vector<int> list;
  list.swap(hs.list);
  for (int b : list) hs.on[b] = 0;
  const float vs = s.vs;
  const float A = vs * vs;
  const float Tamb = AMBIENT + 273.15f;
  const float Tamb4 = Tamb * Tamb * Tamb * Tamb;
  const float litres = vs * vs * vs / 0.001f;
  Scratch sc;
  sc.temps.resize(list.size() * (size_t)BS3);
  sc.keep.assign(list.size(), 0);
  std::vector<ThermEvent> events;
  std::mutex mu;
  std::atomic<int> burnCount{0}, hotCount{0};
  // Which bricks are being processed (heat only enters through faces they share).
  std::vector<uint8_t> inList(v.bricks.size(), 0);
  for (int b : list) inList[b] = 1;
  std::vector<uint8_t> faceWarm(list.size(), 0);  // bit per face: hot cells there after the step

  std::vector<std::pair<int, float>> airHeatAll;
  Jobs::get().parallelFor((int)list.size(), 2, [&](int b0, int b1, int) {
    std::vector<ThermEvent> loc;
    std::vector<std::pair<int, float>> airHeat;
    std::vector<Radiator> rads;
    for (int li = b0; li < b1; li++) {
      int bi = list[li];
      Brick* br = v.brickAt(bi);
      float* out = &sc.temps[(size_t)li * BS3];
      if (!br || !br->count) { sc.keep[li] = 0; continue; }
      int bx = bi % v.bx, bz = (bi / v.bx) % v.bz, by = bi / (v.bx * v.bz);
      bool active = false;
      // Only cells that are off ambient (plus their neighbours and the brick's
      // faces, where heat arrives from next door) need work.
      memcpy(out, br->temp, sizeof(float) * BS3);
      uint8_t mark[BS3];
      memset(mark, 0, sizeof mark);
      int work[BS3];
      int nwork = 0;
      auto addCell = [&](int l) { if (!mark[l]) { mark[l] = 1; work[nwork++] = l; } };
      for (int l = 0; l < BS3; l++) {
        if (!br->mat[l]) continue;
        if (br->temp[l] != AMBIENT || br->flags[l]) {
          addCell(l);
          int x = l & 15, z = (l >> 4) & 15, y = l >> 8;
          if (x > 0) addCell(l - 1);
          if (x < 15) addCell(l + 1);
          if (z > 0) addCell(l - 16);
          if (z < 15) addCell(l + 16);
          if (y > 0) addCell(l - 256);
          if (y < 15) addCell(l + 256);
        }
      }
      // Faces next to bricks in this tick's list may receive heat.
      auto nbIn = [&](int dx, int dy, int dz) {
        int x = bx + dx, y = by + dy, z = bz + dz;
        if (x < 0 || y < 0 || z < 0 || x >= v.bx || y >= v.by || z >= v.bz) return false;
        return inList[x + v.bx * (z + v.bz * y)] != 0;
      };
      bool fy0 = nbIn(0, -1, 0), fy1 = nbIn(0, 1, 0), fz0 = nbIn(0, 0, -1), fz1 = nbIn(0, 0, 1), fx0 = nbIn(-1, 0, 0), fx1 = nbIn(1, 0, 0);
      for (int a = 0; a < 16; a++)
        for (int b2 = 0; b2 < 16; b2++) {
          if (fy0) addCell(a | (b2 << 4));
          if (fy1) addCell(a | (b2 << 4) | (15 << 8));
          if (fz0) addCell(a | (b2 << 8));
          if (fz1) addCell(a | (15 << 4) | (b2 << 8));
          if (fx0) addCell((a << 4) | (b2 << 8));
          if (fx1) addCell(15 | (a << 4) | (b2 << 8));
        }
      for (int wi = 0; wi < nwork; wi++) {
        int l = work[wi];
        uint8_t m = br->mat[l];
        float T = br->temp[l];
        if (!m) continue;
        const Material& M = MATS[m];
        int x = bx * 16 + (l & 15), z = bz * 16 + ((l >> 4) & 15), y = by * 16 + (l >> 8);
        uint8_t fl = br->flags[l];
        if (T == AMBIENT && !(fl & F_BURN)) {
          // Quick check: any neighbour off ambient? If not, nothing to do.
          bool any = false;
          for (auto& d : DIRS6) {
            int xx = x + d[0], yy = y + d[1], zz = z + d[2];
            if (!v.inb(xx, yy, zz)) continue;
            const Brick* nb = v.brick(xx, yy, zz);
            if (!nb) continue;
            int nl = lidx(xx, yy, zz);
            if (nb->mat[nl] && (nb->temp[nl] != AMBIENT || (nb->flags[nl] & F_BURN))) { any = true; break; }
          }
          if (!any) continue;
        }
        float C = M.heatCap;
        float sumG = 0, sumGT = 0, extra = 0;
        int airFaces = 0;
        vec3 nrm(0);
        bool water = false;
        float Tlo = std::min(T, AMBIENT);  // nothing here can cool it below the coldest thing it touches
        for (int k = 0; k < 6; k++) {
          int xx = x + DIRS6[k][0], yy = y + DIRS6[k][1], zz = z + DIRS6[k][2];
          uint8_t n;
          float Tn = AMBIENT;
          uint8_t nf = 0;
          if (!v.inb(xx, yy, zz)) {
            n = (v.solidBelow && yy < 0) ? BEDROCK : AIR;
          } else {
            const Brick* nb = v.brick(xx, yy, zz);
            int nl = lidx(xx, yy, zz);
            n = nb ? nb->mat[nl] : AIR;
            if (n) { Tn = nb->temp[nl]; nf = nb->flags[nl]; }
          }
          if (n == AIR || MATS[n].phase == GAS) { airFaces++; nrm += vec3((float)DIRS6[k][0], (float)DIRS6[k][1], (float)DIRS6[k][2]); continue; }
          const Material& N = MATS[n];
          float G = 2 * M.k * N.k / (M.k + N.k) * vs;
          sumG += G;
          sumGT += G * Tn;
          Tlo = std::min(Tlo, Tn);
          if (n == WATER) water = true;
          if (nf & F_BURN) {
            // Flames lick neighbouring fuel; much more strongly upwards.
            float hf = DIRS6[k][1] < 0 ? 180.f : 60.f;
            extra += hf * A * (MATS[n].flameTemp - T);
          }
        }
        // Convection to the air actually touching the surface (forced
        // convection grows with the local wind), radiation to the surroundings.
        float Tk = T + 273.15f;
        float vAir = 0;
        int airCell = -1;
        if (airFaces) {
          float Tair = AMBIENT;
          if (s.airReady) {
            vec3 wp = owner ? owner->toWorld(vec3(x + 0.5f, y + 0.5f, z + 0.5f), vs) : vec3((x + 0.5f) * vs, (y + 0.5f) * vs, (z + 0.5f) * vs);
            const AirSim& air = s.air;
            airCell = air.openCellAt(wp);
            if (airCell >= 0) {
              Tair = air.T[airCell];
              Tlo = std::min(Tlo, Tair);
              if ((fl & F_BURN) || std::fabs(T - Tair) > 40) vAir = length(air.velocity(wp));
            }
          }
          float hc = 10.f + 6.f * std::min(vAir, 10.f);
          float conv = airFaces * A * hc * (Tair - T);
          extra += conv + airFaces * A * EMISS * SIGMA * (Tamb4 - Tk * Tk * Tk * Tk);
          if (airCell >= 0 && std::fabs(conv) > 1e-3f) airHeat.push_back({airCell, -conv * dt / s.cfg.thermalWarp});
        }
        // Combustion.
        if (fl & F_BURN) {
          float P = (m == LIION) ? 8000.f * litres : M.power;
          if (airFaces == 0) P *= 0.1f;  // starved of oxygen: smoulders
          else P *= 1.f + 0.25f * std::min(vAir, 6.f);  // a draught feeds the fire
          if (T < M.flameTemp * 1.15f) extra += P;
          // Most of a flame's heat is released in the gas above the surface
          // and carried off by the air rather than soaking into the fuel.
          if (airCell >= 0 && airFaces > 0) airHeat.push_back({airCell, 0.45f * P * dt / std::sqrt(s.cfg.thermalWarp)});
          if (water) loc.push_back({{x, y, z}, T_EXTINGUISH});
          br->fuel[l] -= dt;
          if (br->fuel[l] <= 0) loc.push_back({{x, y, z}, T_BURNOUT});
          burnCount++;
        }
        // Hot surfaces and flames shine heat across the open air (rays cast later).
        if (airFaces && (T > 180 || (fl & F_BURN))) {
          float W = airFaces * A * EMISS * SIGMA * (Tk * Tk * Tk * Tk - Tamb4);
          if (fl & F_BURN) W += 0.25f * M.power;
          if (W > 0 && length2(nrm) > 0) {
            vec3 lp(x + 0.5f, y + 0.5f, z + 0.5f), n = normalize(nrm);
            Radiator r;
            r.p = owner ? owner->toWorld(lp, vs) : lp * vs;
            r.n = owner ? owner->rot.rotate(n) : n;
            r.W = W;
            r.body = owner ? owner->id : -1;
            rads.push_back(r);
          }
        }
        // Point-implicit update: unconditionally stable for conduction.
        float Tnew = (T + dt / C * (sumGT + extra)) / (1 + dt / C * sumG);
        // Radiation/convection can't push past ambient in one step.
        if ((T > AMBIENT && Tnew < AMBIENT && sumG == 0) || (T < AMBIENT && Tnew > AMBIENT && sumG == 0)) Tnew = AMBIENT;
        // Radiation is applied explicitly; in a voxel with little heat capacity
        // (foliage, bulb glass) a hot step could overshoot far below ambient
        // and run away. Cooling only ever brings it down to its surroundings.
        Tnew = std::clamp(Tnew, Tlo, 3600.f);
        if (std::fabs(Tnew - AMBIENT) < 0.05f && !(fl & F_BURN)) Tnew = AMBIENT;
        out[l] = Tnew;
        if (Tnew != AMBIENT || (fl & F_BURN)) {
          active = true;
          int lx = l & 15, lz = (l >> 4) & 15, ly = l >> 8;
          uint8_t& fw = faceWarm[li];
          fw |= (lx == 0 ? 1 : 0) | (lx == 15 ? 2 : 0) | (ly == 0 ? 4 : 0) | (ly == 15 ? 8 : 0) | (lz == 0 ? 16 : 0) | (lz == 15 ? 32 : 0);
        }
        if (std::fabs(Tnew - AMBIENT) > 30) hotCount++;
        // Thresholds.
        if (!(fl & F_BURN) && M.ignite > 0 && Tnew >= M.ignite && airFaces > 0) loc.push_back({{x, y, z}, T_IGNITE});
        if (M.melt > 0 && m != MOLTEN && m != ICE && Tnew >= M.melt) loc.push_back({{x, y, z}, T_MELT});
        if (m == ICE && Tnew > 0.5f) loc.push_back({{x, y, z}, T_THAW});
        if (m == MOLTEN) {
          uint8_t base = br->aux[l] ? br->aux[l] : STEEL;
          if (Tnew < MATS[base].melt - 40) loc.push_back({{x, y, z}, T_FREEZE});
        }
        if (M.boil > 0 && Tnew >= M.boil) loc.push_back({{x, y, z}, T_BOIL});
        if (m == CHARGE && Tnew >= 300) loc.push_back({{x, y, z}, T_DETONATE});
        if (m == PROPELLANT && Tnew >= 250) loc.push_back({{x, y, z}, T_ROCKET});
        if (m == LIION && Tnew >= 150 && !(fl & F_BURN)) loc.push_back({{x, y, z}, T_RUNAWAY});
      }
      sc.keep[li] = active;
    }
    if (!loc.empty() || !airHeat.empty() || !rads.empty()) {
      std::lock_guard<std::mutex> lk(mu);
      s.radiators.insert(s.radiators.end(), rads.begin(), rads.end());
      events.insert(events.end(), loc.begin(), loc.end());
      airHeatAll.insert(airHeatAll.end(), airHeat.begin(), airHeat.end());
    }
  });
  if (s.airReady) for (auto& [c, J] : airHeatAll) s.air.addHeatCell(c, J);

  // Write back and keep the hot set current.
  for (size_t li = 0; li < list.size(); li++) {
    int bi = list[li];
    Brick* br = v.brickAt(bi);
    if (!br) continue;
    float* nt = &sc.temps[li * BS3];
    bool glowChanged = false;
    for (int l = 0; l < BS3; l++) {
      if (!br->mat[l]) continue;
      if (glowByte(br->temp[l], br->flags[l]) != glowByte(nt[l], br->flags[l])) glowChanged = true;
      br->temp[l] = nt[l];
    }
    if (glowChanged) v.markBrickDirty(bi);
    if (sc.keep[li]) {
      hs.add(bi);
      int bx = bi % v.bx, bz = (bi / v.bx) % v.bz, by = bi / (v.bx * v.bz);
      const int nbs[6][4] = {{-1, 0, 0, 1}, {1, 0, 0, 2}, {0, -1, 0, 4}, {0, 1, 0, 8}, {0, 0, -1, 16}, {0, 0, 1, 32}};
      for (auto& d : nbs) {
        if (!(faceWarm[li] & d[3])) continue;  // nothing warm on that face: neighbour stays cold
        int x = bx + d[0], y = by + d[1], z = bz + d[2];
        if (x < 0 || y < 0 || z < 0 || x >= v.bx || y >= v.by || z >= v.bz) continue;
        int nb = x + v.bx * (z + v.bz * y);
        if (v.bricks[nb]) hs.add(nb);
      }
    }
  }
  burning += burnCount;
  hot += hotCount;

  // Apply threshold events serially (they change structure).
  auto worldPos = [&](const ivec3& c) {
    vec3 lc(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f);
    return owner ? owner->toWorld(lc, vs) : lc * vs;
  };
  // A voxel that turns to gas leaves its volume and joins the gas layer at
  // the same place in the world (for a body, wherever that voxel now is).
  auto toGas = [&](const ivec3& c, uint8_t gasMat, float T) {
    ivec3 wc = owner ? s.cellOf(worldPos(c)) : c;
    if (owner) { owner->vol.set(c.x, c.y, c.z, AIR); owner->shapeDirty = true; owner->splitCheck = true; }
    else {
      uint8_t old = s.world.mat(c.x, c.y, c.z);
      s.world.set(c.x, c.y, c.z, AIR);
      s.worldChanged(c.x, c.y, c.z, old);
    }
    s.gas.add(s, wc, gasMat, T);
  };
  for (auto& e : events) {
    Brick* br = v.inb(e.c.x, e.c.y, e.c.z) ? v.brick(e.c.x, e.c.y, e.c.z) : nullptr;
    if (!br) continue;
    int l = lidx(e.c.x, e.c.y, e.c.z);
    uint8_t m = br->mat[l];
    if (!m) continue;
    switch (e.kind) {
      case T_IGNITE:
        if (!(br->flags[l] & F_BURN)) {
          br->flags[l] |= F_BURN;
          br->fuel[l] = MATS[m].burnTime * (0.7f + 0.6f * rng().f());
          v.markDirty(e.c.x, e.c.y, e.c.z);
          if (rng().f() < 0.02f) s.event(EV_IGNITE, worldPos(e.c), 0, m);
        }
        break;
      case T_EXTINGUISH:
        br->flags[l] &= ~F_BURN;
        br->temp[l] = std::min(br->temp[l], 90.f);
        v.markDirty(e.c.x, e.c.y, e.c.z);
        break;
      case T_BURNOUT: {
        uint8_t res = MATS[m].residue;
        if (m == LIION) res = ASH;
        float T = br->temp[l];
        // What burns goes up in smoke, still hot enough to glow: that is the
        // flame. Some of it (more for wood than for plastic) stays as ash.
        float pAsh = res == ASH ? (m == RUBBER || m == PLASTIC ? 0.15f : 0.3f) : 0.f;
        if (res == AIR || (res == ASH && rng().f() >= pAsh)) {
          toGas(e.c, SMOKE, std::max(T, MATS[m].flameTemp));
          for (auto& d : DIRS6) s.supportSeeds.push_back(packKey(e.c.x + d[0], e.c.y + d[1], e.c.z + d[2]));
          break;
        }
        if (owner) {
          Cell c = owner->vol.get(e.c.x, e.c.y, e.c.z);
          c.mat = res; c.flags = 0; c.fuel = 0; c.temp = T;
          if (isFluidish(res)) {  // ash falls out of a body as loose dust
            owner->vol.set(e.c.x, e.c.y, e.c.z, AIR);
            owner->shapeDirty = true; owner->splitCheck = true;
            s.spawnDebris(worldPos(e.c), owner->pointVel(worldPos(e.c)), c);
          } else {
            owner->vol.put(e.c.x, e.c.y, e.c.z, c);
            owner->shapeDirty = true;
          }
        } else {
          Cell c = s.world.get(e.c.x, e.c.y, e.c.z);
          c.mat = res; c.flags = 0; c.fuel = 0; c.temp = T;
          s.putWorld(e.c.x, e.c.y, e.c.z, c);
          if (res == AIR || isFluidish(res)) for (auto& d : DIRS6) s.supportSeeds.push_back(packKey(e.c.x + d[0], e.c.y + d[1], e.c.z + d[2]));
        }
        break;
      }
      case T_MELT: {
        Cell c = v.get(e.c.x, e.c.y, e.c.z);
        uint8_t orig = c.mat;
        c.aux = MATS[orig].solidifyTo;
        c.mat = MOLTEN;
        c.flags = 0;
        if (owner) {
          vec3 wp = worldPos(e.c);
          s.spawnDebris(wp, owner->pointVel(wp), c);
          owner->vol.set(e.c.x, e.c.y, e.c.z, AIR);
          owner->shapeDirty = true; owner->splitCheck = true;
        } else {
          s.putWorld(e.c.x, e.c.y, e.c.z, c);
          for (auto& d : DIRS6) s.supportSeeds.push_back(packKey(e.c.x + d[0], e.c.y + d[1], e.c.z + d[2]));
        }
        if (rng().f() < 0.05f) s.event(EV_MELT, worldPos(e.c), 0, orig);
        break;
      }
      case T_FREEZE: {
        if (owner) break;
        Cell c = s.world.get(e.c.x, e.c.y, e.c.z);
        c.mat = c.aux ? c.aux : STEEL;
        c.aux = 0;
        s.putWorld(e.c.x, e.c.y, e.c.z, c);
        break;
      }
      case T_THAW: {
        Cell c = v.get(e.c.x, e.c.y, e.c.z);
        c.mat = WATER;
        if (owner) {
          s.spawnDebris(worldPos(e.c), owner->pointVel(worldPos(e.c)), c);
          owner->vol.set(e.c.x, e.c.y, e.c.z, AIR); owner->shapeDirty = true; owner->splitCheck = true;
        } else s.putWorld(e.c.x, e.c.y, e.c.z, c);
        break;
      }
      case T_BOIL:
        toGas(e.c, STEAM, std::max(105.f, br->temp[l]));
        break;
      case T_ROCKET: {
        uint32_t cid = br->comp[l];
        if (cid && comp(cid).type == C_ROCKET && !comp(cid).lit && comp(cid).fuel > 0) {
          comp(cid).lit = true;
          s.event(EV_IGNITE, worldPos(e.c), 1, m, cid);
        }
        break;
      }
      case T_RUNAWAY:
        br->flags[l] |= F_BURN;
        br->fuel[l] = 200;  // ~10 s of venting flame at the default thermal warp
        br->temp[l] = std::max(br->temp[l], 400.f);
        v.markDirty(e.c.x, e.c.y, e.c.z);
        s.event(EV_IGNITE, worldPos(e.c), 2, m, br->comp[l]);
        break;
      case T_DETONATE: {
        // Every voxel of explosive connected to this one goes up together.
        // A hash set keeps this linear; a vector scan made a large charge
        // kernel quadratic (tens of billions of comparisons at the 200k cap).
        std::vector<ivec3> stack{e.c}, cells;
        std::unordered_set<int64_t> seen;
        seen.reserve(256);
        seen.insert(packKey(e.c.x, e.c.y, e.c.z));
        while (!stack.empty() && cells.size() < 200000) {
          ivec3 c = stack.back();
          stack.pop_back();
          cells.push_back(c);
          for (auto& d : DIRS6) {
            ivec3 q(c.x + d[0], c.y + d[1], c.z + d[2]);
            if (v.matI(q) != CHARGE) continue;
            if (seen.insert(packKey(q.x, q.y, q.z)).second) stack.push_back(q);
          }
        }
        vec3 centre;
        for (auto& c : cells) centre += worldPos(c);
        centre = centre / (float)cells.size();
        float E = cells.size() * MATS[CHARGE].explosive * litres;
        // The explosive itself becomes the fireball: white-hot gas.
        for (auto& c : cells) toGas(c, SMOKE, 2600.f);
        s.explode(centre, E, owner ? owner->id : -1);
        return;  // the volume may have changed shape under us; finish next tick
      }
    }
  }

  // Fire and smoke from exposed burning voxels, and convective plumes that
  // heat whatever hangs above a fire.
  if (burning > 0) {
    float pEmit = std::min(1.f, 120.f / std::max(1, burning));
    const size_t nlist = hs.list.size();
    for (size_t hi = 0; hi < nlist; hi++) {
      int bi = hs.list[hi];
      Brick* br = v.brickAt(bi);
      if (!br) continue;
      int bx = bi % v.bx, bz = (bi / v.bx) % v.bz, by = bi / (v.bx * v.bz);
      for (int l = 0; l < BS3; l++) {
        if (!(br->flags[l] & F_BURN)) continue;
        int x = bx * 16 + (l & 15), z = bz * 16 + ((l >> 4) & 15), y = by * 16 + (l >> 8);
        // Burning surfaces light their surroundings (and crackle).
        if (s.fireSpots.size() < 64 && rng().f() < pEmit * 0.25f) {
          bool open = v.mat(x, y + 1, z) == AIR || v.mat(x + 1, y, z) == AIR || v.mat(x - 1, y, z) == AIR ||
                      v.mat(x, y, z + 1) == AIR || v.mat(x, y, z - 1) == AIR;
          if (open) s.fireSpots.push_back(worldPos({x, y, z}) + vec3(0, vs * 2, 0));
        }
      }
    }
  }
}

void Sim::stepThermal(float dt) {
  int burning = 0, hot = 0;
  radiators.clear();
  fireSpots.clear();
  simulateVolume(*this, world, worldHot, dt, nullptr, burning, hot);
  for (size_t i = 0; i < bodies.size(); i++) {
    Body* b = bodies[i].get();
    if (b->ghost) continue;
    if (b->heat.on.size() != b->vol.bricks.size()) b->heat.reset(b->vol.bricks.size());
    simulateVolume(*this, b->vol, b->heat, dt, b, burning, hot);
  }
  stats.burning = burning;
  stats.hot = hot;
  radiate(dt);
}

// Thermal radiation crosses open air: each tick a fixed budget of rays leaves
// the hot surfaces (chosen in proportion to what they emit, cosine-weighted
// about the surface normal) and the energy lands on whatever they hit first,
// so a fire warms the logs beside it and a glowing ingot scorches the bench
// it hangs over. Rays that reach the sky are lost.
void Sim::radiate(float dt) {
  radiantTotal = 0;
  if (radiators.empty()) return;
  // The strongest emitters first; flux queries only look at those.
  std::sort(radiators.begin(), radiators.end(), [](const Radiator& a, const Radiator& b) { return a.W > b.W; });
  double total = 0;
  for (auto& r : radiators) total += r.W;
  radiantTotal = (float)total;
  const int rays = 320;
  const float Eray = (float)(total * dt / rays);
  // Systematic sampling along the cumulative power gives a low-variance pick.
  struct Ray { vec3 o, d; int body; };
  std::vector<Ray> rs(rays);
  double step = total / rays, acc = rng().f() * step, cum = 0;
  size_t i = 0;
  for (int k = 0; k < rays; k++, acc += step) {
    while (i + 1 < radiators.size() && cum + radiators[i].W < acc) { cum += radiators[i].W; i++; }
    const Radiator& r = radiators[i];
    // Cosine-weighted direction about the normal.
    float u1 = rng().f(), u2 = rng().f();
    float rr = std::sqrt(u1), phi = 2 * PI * u2;
    vec3 t = std::fabs(r.n.y) < 0.9f ? normalize(cross(r.n, vec3(0, 1, 0))) : normalize(cross(r.n, vec3(1, 0, 0)));
    vec3 b = cross(r.n, t);
    vec3 d = normalize(t * (rr * std::cos(phi)) + b * (rr * std::sin(phi)) + r.n * std::sqrt(std::max(0.f, 1 - u1)));
    rs[k] = {r.p + r.n * (vs * 1.8f), d, r.body};
  }
  std::vector<Hit> hitv(rays);
  Jobs::get().parallelFor(rays, 16, [&](int k0, int k1, int) {
    for (int k = k0; k < k1; k++) hitv[k] = raycast(rs[k].o, rs[k].d, 6.f);
  });
  std::vector<std::pair<Hit, float>> hits;
  for (int k = 0; k < rays; k++) {
    const Hit& h = hitv[k];
    if (!h.hit) continue;
    if (h.body >= 0 && bodies[h.body]->id == rs[k].body && h.t < vs * 0.5f) continue;
    hits.push_back({h, Eray});
  }
  for (auto& [h, E] : hits) {
    Body* B = h.body >= 0 ? bodies[h.body].get() : nullptr;
    Volume& v = B ? B->vol : world;
    Brick* br = v.brick(h.cell.x, h.cell.y, h.cell.z);
    if (!br) continue;
    int l = lidx(h.cell.x, h.cell.y, h.cell.z);
    uint8_t m = br->mat[l];
    if (!m) continue;
    // Dark, rough surfaces absorb most of it; polished metal reflects.
    float absorb = MATS[m].metal > 0.5f ? 0.35f : 0.9f;
    br->temp[l] = std::min(3600.f, br->temp[l] + E * absorb / MATS[m].heatCap);
    if (B) B->heat.add(v.bindex(h.cell.x, h.cell.y, h.cell.z)); else worldHot.add(v.bindex(h.cell.x, h.cell.y, h.cell.z));
  }
}

// Radiant flux (W/m^2) arriving at a point from the strongest emitters, with
// line of sight. Used for the warmth you feel standing near a fire.
float Sim::radiantFlux(const vec3& p) const {
  float F = 0;
  size_t n = std::min<size_t>(radiators.size(), 48);
  for (size_t i = 0; i < n; i++) {
    const Radiator& r = radiators[i];
    vec3 d = p - r.p;
    float d2 = std::max(0.04f, length2(d));
    float dist = std::sqrt(d2);
    float c = dot(r.n, d) / dist;
    if (c <= 0) continue;
    Hit h = raycast(r.p + r.n * (vs * 1.8f), d / dist, dist - 0.1f);
    if (h.hit) continue;
    F += r.W * c / (PI * d2);
  }
  // The strongest emitters stand in for the rest.
  float shown = 0;
  for (size_t i = 0; i < n; i++) shown += radiators[i].W;
  return shown > 0 ? F * radiantTotal / shown : 0;
}
