// Delivering energy to matter: hammer blows and collisions, brittle fracture,
// explosions, and direct heating (torches).
#include <algorithm>
#include <mutex>
#include "../core/jobs.h"
#include "sim.h"

static bool brittle(uint8_t m) {
  return m == GLASS || m == BULBGLASS || m == BRICK || m == STONE || m == CONCRETE || m == TILE || m == IRON ||
         m == BASALT || m == MORTAR || m == ICE || m == SOLAR;
}

void Sim::impact(const Hit& h, float E, const vec3& dir, bool canDetach) {
  if (!h.hit || E <= 0) return;
  structureUrgent();  // what this knocks out may have been holding something up
  Body* B = h.body >= 0 && h.body < (int)bodies.size() ? bodies[h.body].get() : nullptr;
  Volume& v = B ? B->vol : world;
  Cell c = v.get(h.cell.x, h.cell.y, h.cell.z);
  if (c.mat == AIR || c.mat == BEDROCK) return;
  if (B) B->wake();
  // Part of every blow ends up as heat in the struck voxel.
  if (Brick* br = v.brick(h.cell.x, h.cell.y, h.cell.z)) {
    br->temp[lidx(h.cell.x, h.cell.y, h.cell.z)] += 0.25f * E / MATS[c.mat].heatCap;
    if (B) B->heat.add(v.bindex(h.cell.x, h.cell.y, h.cell.z)); else worldHot.add(v.bindex(h.cell.x, h.cell.y, h.cell.z));
  }
  vec3 n = h.normal;
  // A hard enough blow pops the whole part off its fixings.
  if (canDetach && c.comp && comp(c.comp).type != C_TERRAIN) {
    const Component& cp = comp(c.comp);
    if (E > cp.joint.energy) {
      float spare = E - cp.joint.energy;
      Body* nb = detachComponent(h.body, c.comp, vec3(0), h.cell);
      if (nb && nb->mass > 0) {
        float sp = std::min(8.f, std::sqrt(2 * spare * 0.5f / nb->mass));
        nb->vel += normalize(dir) * sp;
        nb->angVel += rng().unitVec() * sp * 2.f;
        nb->wake();
        event(EV_DETACH, h.point, E, c.mat, c.comp);
      }
      return;
    }
  }
  // Otherwise chip a crater: break voxels nearest the blow while energy lasts.
  float energy = E;
  const int R = 4;
  struct Cand { ivec3 p; float d; };
  std::vector<Cand> cand;
  for (int dz = -R; dz <= R; dz++)
    for (int dy = -R; dy <= R; dy++)
      for (int dx = -R; dx <= R; dx++) {
        float d = std::sqrt((float)(dx * dx + dy * dy + dz * dz));
        if (d > R) continue;
        ivec3 p = h.cell + ivec3(dx, dy, dz);
        uint8_t m = v.matI(p);
        if (m == AIR || m == BEDROCK || !v.inb(p.x, p.y, p.z)) continue;
        // Bias towards the struck surface so craters open outwards.
        vec3 off((float)dx, (float)dy, (float)dz);
        cand.push_back({p, d - 0.35f * dot(off, B ? B->rot.conj().rotate(n) : n)});
      }
  std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
  int broken = 0;
  uint8_t firstMat = c.mat;
  for (auto& k : cand) {
    Brick* br = v.brick(k.p.x, k.p.y, k.p.z);
    if (!br) continue;
    int l = lidx(k.p.x, k.p.y, k.p.z);
    uint8_t m = br->mat[l];
    if (!m) continue;
    float cost = MATS[m].tough * (br->hp[l] / 255.f);
    if (energy >= cost) {
      energy -= cost;
      vec3 kick = (n * 1.5f + rng().unitVec()) * rng().range(0.8f, 3.f);
      if (B) {
        vec3 wp = B->toWorld(vec3(k.p.x + 0.5f, k.p.y + 0.5f, k.p.z + 0.5f), vs);
        Cell cc = v.get(k.p.x, k.p.y, k.p.z);
        spawnDebris(wp, B->pointVel(wp) + kick, cc);
        B->vol.set(k.p.x, k.p.y, k.p.z, AIR);
        B->shapeDirty = true;
        B->splitCheck = true;
      } else {
        removeWorld(k.p.x, k.p.y, k.p.z, true, kick);
      }
      broken++;
    } else {
      int dmg = (int)(255.f * energy / std::max(1e-6f, MATS[m].tough));
      br->hp[l] = (uint8_t)std::max(1, br->hp[l] - dmg);
      v.markDirty(k.p.x, k.p.y, k.p.z);
      energy = 0;
      break;
    }
    if (energy <= 0) break;
  }
  if (broken) event(brittle(firstMat) ? EV_SHATTER : EV_BREAK, h.point, E, firstMat, c.comp);
  // Brittle materials crack well beyond the crater.
  if (brittle(firstMat) && E > MATS[firstMat].tough * 30) {
    float r = std::min(18.f, 2.f + std::cbrt(E / MATS[firstMat].tough) * 0.9f);
    fracture(v, vec3(h.cell.x + 0.5f, h.cell.y + 0.5f, h.cell.z + 0.5f), r, 3 + (int)std::min(9.f, E / (MATS[firstMat].tough * 60)), B);
  }
}

// Voronoi cracking: cells on the boundary between seed regions break, leaving
// separate chunks that the support/split logic turns into falling pieces.
void Sim::fracture(Volume& v, const vec3& c, float R, int nseeds, Body* owner) {
  std::vector<vec3> seeds;
  for (int i = 0; i < nseeds; i++) seeds.push_back(c + rng().unitVec() * (R * std::sqrt(rng().f())));
  int r = (int)std::ceil(R);
  ivec3 ci = floorI(c);
  auto nearest = [&](const vec3& p) {
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < (int)seeds.size(); i++) { float d = length2(p - seeds[i]); if (d < bd) { bd = d; best = i; } }
    return best;
  };
  std::vector<ivec3> cracks;
  for (int dz = -r; dz <= r; dz++)
    for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++) {
        if (dx * dx + dy * dy + dz * dz > R * R) continue;
        ivec3 p = ci + ivec3(dx, dy, dz);
        uint8_t m = v.matI(p);
        if (!m || !brittle(m) || !v.inb(p.x, p.y, p.z)) continue;
        vec3 pc(p.x + 0.5f, p.y + 0.5f, p.z + 0.5f);
        int s = nearest(pc);
        // Crack if a neighbour belongs to a different region (check +x,+y,+z only so cracks are 1 voxel thick).
        for (int a = 0; a < 3; a++) {
          vec3 q = pc;
          q[a] += 1;
          if (nearest(q) != s) { cracks.push_back(p); break; }
        }
      }
  for (auto& p : cracks) {
    if (owner) {
      // A crack's voxels crumble out as loose fragments.
      Cell cc = v.get(p.x, p.y, p.z);
      vec3 wp = owner->toWorld(vec3(p.x + 0.5f, p.y + 0.5f, p.z + 0.5f), vs);
      spawnDebris(wp, owner->pointVel(wp) + rng().unitVec() * 0.5f, cc);
      owner->vol.set(p.x, p.y, p.z, AIR);
      owner->shapeDirty = true;
      owner->splitCheck = true;
    } else {
      removeWorld(p.x, p.y, p.z, true, rng().unitVec() * 0.5f);
    }
  }
}

void Sim::explode(const vec3& p, float E, int sourceBody) {
  (void)sourceBody;
  structureUrgent();
  event(EV_EXPLODE, p, E);
  if (airReady) air.blast(p, E);
  // Energy available to break one voxel at distance r: the blast energy spread
  // through the sphere of radius r (near-field decay ~ 1/r^3), with a factor
  // for the shock. Buried voxels are shielded: material is eaten from the
  // exposed surface inwards, so explosions dig craters instead of stripping
  // everything in range.
  const float VV = vs * vs * vs;
  const float K = 2.0f;
  auto energyAt = [&](float r) { return K * E * VV / (4.18879f * r * r * r); };
  const float weakest = 0.2f;
  float R = std::min(4.0f, std::cbrt(K * E * VV / (4.18879f * weakest)));
  int Rv = (int)std::ceil(R / vs);
  ivec3 pc = cellOf(p);
  // The flash is light only; the fireball is the charge's own matter, now
  // white-hot gas (thermal.cpp), and the debris cloud is what the blast breaks.
  if (flashes.size() < 16) flashes.push_back({p, 0.2f, 0.2f});
  // World: gather candidates in parallel.
  struct Cand { ivec3 c; float d; float e; };
  std::vector<Cand> cands;
  std::vector<std::pair<ivec3, float>> heats;
  std::mutex mu;
  int ylo = std::max(0, pc.y - Rv), yhi = std::min(world.ny - 1, pc.y + Rv);
  Jobs::get().parallelFor(yhi - ylo + 1, 1, [&](int b0, int b1, int) {
    std::vector<Cand> loc;
    std::vector<std::pair<ivec3, float>> lh;
    for (int yy = b0; yy < b1; yy++) {
      int y = ylo + yy;
      for (int z = pc.z - Rv; z <= pc.z + Rv; z++)
        for (int x = pc.x - Rv; x <= pc.x + Rv; x++) {
          if (!world.inb(x, y, z)) continue;
          Brick* br = world.brick(x, y, z);
          if (!br) { x |= 15; continue; }
          int l = lidx(x, y, z);
          uint8_t m = br->mat[l];
          if (!m || m == BEDROCK) continue;
          vec3 cc((x + 0.5f) * vs, (y + 0.5f) * vs, (z + 0.5f) * vs);
          float dist = std::max(vs, length(cc - p));
          if (dist > R) continue;
          float e = energyAt(dist);
          float tough = MATS[m].tough * (br->hp[l] / 255.f);
          if (MATS[m].phase == LIQUID) tough = 0.05f;
          if (e > tough) loc.push_back({{x, y, z}, dist, e});
          else if (e > tough * 0.2f || dist < R * 0.5f) lh.push_back({{x, y, z}, e});
        }
    }
    std::lock_guard<std::mutex> lk(mu);
    cands.insert(cands.end(), loc.begin(), loc.end());
    heats.insert(heats.end(), lh.begin(), lh.end());
  });
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
  int removed = 0;
  for (auto& c : cands) {
    uint8_t m = world.matI(c.c);
    if (!m) continue;
    // Only exposed material goes (the blast reaches it).
    bool exposed = c.d < vs * 2.5f;
    for (int k = 0; k < 6 && !exposed; k++) {
      ivec3 q(c.c.x + DIRS6[k][0], c.c.y + DIRS6[k][1], c.c.z + DIRS6[k][2]);
      uint8_t n = world.matI(q);
      if (n == AIR || isLiquid(n)) exposed = true;
    }
    if (!exposed) { heats.push_back({c.c, c.e * 0.5f}); continue; }
    if (m == CHARGE) {  // sympathetic detonation: the thermal step sets it off
      world.temp(c.c.x, c.c.y, c.c.z) = 900;
      worldHot.add(world.bindex(c.c.x, c.c.y, c.c.z));
      continue;
    }
    float tough = std::max(1e-3f, MATS[m].tough);
    vec3 dir = normalize(cellCenter(c.c.x, c.c.y, c.c.z) - p);
    float sp = std::min(26.f, std::sqrt(2 * (c.e - tough * 0.5f) / std::max(1e-5f, MATS[m].voxMass)) * 0.3f);
    // Everything the blast breaks is thrown, however finely it is broken.
    removeWorld(c.c.x, c.c.y, c.c.z, true, dir * sp + rng().unitVec() * sp * 0.25f);
    removed++;
  }
  // Survivors near the blast get cracked, damaged and heated.
  for (auto& [c, e] : heats) {
    Brick* br = world.inb(c.x, c.y, c.z) ? world.brick(c.x, c.y, c.z) : nullptr;
    if (!br) continue;
    int l = lidx(c.x, c.y, c.z);
    uint8_t m = br->mat[l];
    if (!m) continue;
    float tough = MATS[m].tough;
    if (tough > 0 && tough < 1e20f) br->hp[l] = (uint8_t)std::max(1.f, br->hp[l] - 255.f * std::min(1.f, e / tough) * 0.7f);
    br->temp[l] += std::min(900.f, 0.05f * e / MATS[m].heatCap);
    if (m == CHARGE) br->temp[l] = 900;
    worldHot.add(world.bindex(c.x, c.y, c.z));
    world.markDirty(c.x, c.y, c.z);
  }
  // Brittle walls crack beyond the crater.
  float Rb = std::cbrt(K * E * VV / (4.18879f * MATS[BRICK].tough));
  for (int i = 0; i < 6; i++) {
    vec3 q = p + rng().unitVec() * Rb * rng().range(0.8f, 1.6f);
    ivec3 qc = cellOf(q);
    if (brittle(world.matI(qc))) fracture(world, vec3(qc.x + 0.5f, qc.y + 0.5f, qc.z + 0.5f), std::min(24.f, Rb / vs * 0.6f), 6, nullptr);
  }
  // Bodies: voxel damage (same 1/r^3 law), heat and a push from the blast wave.
  int nb = (int)bodies.size();
  for (int bi = 0; bi < nb; bi++) {
    Body& b = *bodies[bi];
    if (b.ghost) continue;
    float dc = length(b.pos - p);
    float Rpush = R * 2.5f;
    if (dc > Rpush + b.radius) continue;
    b.wake();
    std::vector<ivec3> kill;
    b.vol.forEach([&](int x, int y, int z, Brick* br, int l) {
      vec3 wp = b.toWorld(vec3(x + 0.5f, y + 0.5f, z + 0.5f), vs);
      float dist = std::max(vs, length(wp - p));
      if (dist > R) return;
      float e = energyAt(dist);
      uint8_t m = br->mat[l];
      if (e > MATS[m].tough * (br->hp[l] / 255.f) && m != CHARGE) kill.push_back({x, y, z});
      else {
        br->hp[l] = (uint8_t)std::max(1.f, br->hp[l] - 255.f * std::min(1.f, e / std::max(1e-3f, MATS[m].tough)) * 0.5f);
        br->temp[l] += std::min(900.f, 0.05f * e / MATS[m].heatCap);
        if (m == CHARGE) br->temp[l] = 900;
        b.heat.add(b.vol.bindex(x, y, z));
      }
    });
    // Blast impulse ~ E^(1/2) / r^2 on each exposed sample's area.
    mat3 Rm = b.R();
    for (const Sample& s : b.samples) {
      vec3 wp = b.pos + Rm * s.local;
      vec3 d = wp - p;
      float dist = std::max(vs * 2, length(d));
      if (dist > Rpush) continue;
      float area = PI * s.r * s.r;
      float j = 0.06f * std::sqrt(E) * area / (dist * dist);
      b.applyImpulse(normalize(d) * j, wp);
    }
    for (auto& k : kill) {
      Cell cc = b.vol.get(k.x, k.y, k.z);
      vec3 wp = b.toWorld(vec3(k.x + 0.5f, k.y + 0.5f, k.z + 0.5f), vs);
      spawnDebris(wp, b.pointVel(wp) + normalize(wp - p) * rng().range(4, 14), cc);
      b.vol.set(k.x, k.y, k.z, AIR);
    }
    if (!kill.empty()) { b.shapeDirty = true; b.splitCheck = true; }
  }
  for (auto& d : debris) {
    vec3 dd = d.p - p;
    float dist = std::max(vs, length(dd));
    if (dist < R * 2) d.v += normalize(dd) * std::min(20.f, 0.004f * std::sqrt(E) / (dist * dist));
  }
  wakeNear(p, R * 2.5f);
}

void Sim::addHeat(const Hit& h, float watts, float dt, float rad) {
  if (!h.hit) return;
  Body* B = h.body >= 0 ? bodies[h.body].get() : nullptr;
  Volume& v = B ? B->vol : world;
  float J = watts * dt * cfg.thermalWarp;
  int r = (int)std::ceil(rad);
  float wsum = 0;
  std::vector<std::pair<ivec3, float>> cells;
  for (int dz = -r; dz <= r; dz++)
    for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++) {
        float d2 = (float)(dx * dx + dy * dy + dz * dz);
        if (d2 > rad * rad) continue;
        ivec3 p = h.cell + ivec3(dx, dy, dz);
        if (v.matI(p) == AIR || !v.inb(p.x, p.y, p.z)) continue;
        float w = std::exp(-d2 / (rad * rad * 0.5f));
        cells.push_back({p, w});
        wsum += w;
      }
  if (wsum <= 0) return;
  for (auto& [p, w] : cells) {
    Brick* br = v.brick(p.x, p.y, p.z);
    int l = lidx(p.x, p.y, p.z);
    br->temp[l] += J * (w / wsum) / MATS[br->mat[l]].heatCap;
    br->temp[l] = std::min(br->temp[l], 3600.f);
    if (B) B->heat.add(v.bindex(p.x, p.y, p.z)); else worldHot.add(v.bindex(p.x, p.y, p.z));
  }
}

void Sim::spray(const vec3& p, const vec3& vel, uint8_t m, int count) {
  for (int i = 0; i < count; i++) {
    Cell c;
    c.mat = m;
    c.temp = m == WATER ? 15.f : AMBIENT;
    spawnDebris(p, vel + rng().unitVec() * 0.6f, c);
    if (debris.size()) debris.back().life = 6;
  }
}
