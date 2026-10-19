// Liquids flow and powders slump on the world grid; loose debris flies
// ballistically and settles back into the world where it lands.
#include <algorithm>
#include "sim.h"

void Sim::stepFluids() {
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
  for (int64_t k : list) {
    ivec3 p = unpackKey(k);
    int x = p.x, y = p.y, z = p.z;
    uint8_t m = world.mat(x, y, z);
    if (!m || !isFluidish(m) || y <= 0) continue;
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
      // Hot debris cools by radiation as it flies (sparks of molten metal).
      if (d.temp > 200) d.temp -= (d.temp - AMBIENT) * 0.08f * dt * cfg.thermalWarp * 0.1f;
      vec3 np = d.p + d.v * dt;
      ivec3 c = cellOf(np);
      uint8_t m = world.matI(c);
      bool solidHit = m != AIR && !isLiquid(m) && world.inb(c.x, c.y, c.z);
      if (!world.inb(c.x, c.y, c.z) && c.y < 0) solidHit = true;
      bool liquidDeb = isLiquid(d.mat);
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
