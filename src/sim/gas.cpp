#include "gas.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include "sim.h"
#include "../core/jobs.h"

void GasLayer::reset(const Volume& w) {
  nx_ = w.nx; ny_ = w.ny; nz_ = w.nz; bx_ = w.bx; by_ = w.by; bz_ = w.bz;
  bricks.clear();
  bricks.resize(w.bricks.size());
  total_ = 0;
  escaped = 0;
  stepStamp_ = 1;
  pending_.clear();
  hot.clear();
  airCache_.clear();
}

size_t GasLayer::countOf(uint8_t mat) const {
  size_t n = 0;
  for (auto& b : bricks)
    if (b && b->count)
      for (int l = 0; l < BS3; l++) n += b->mat[l] == mat;
  for (auto& p : pending_) n += p.mat == mat;
  return n;
}

void GasLayer::put(Sim& s, int x, int y, int z, uint8_t mat, float temp, uint8_t stamp) {
  int bi = bindex(x, y, z);
  auto& p = bricks[bi];
  if (!p) p = std::make_unique<GasBrick>();
  int l = lidx(x, y, z);
  if (!p->mat[l]) { p->count++; total_++; p->rows[l >> 4] |= (uint16_t)(1u << (l & 15)); }
  p->mat[l] = mat;
  p->temp[l] = temp;
  p->stamp[l] = stamp;
  if (mat == STEAM || temp > 60) p->lively = true;
  s.world.markBrickDirty(bi);
}

void GasLayer::take(Sim& s, int x, int y, int z) {
  int bi = bindex(x, y, z);
  GasBrick* b = bricks[bi].get();
  int l = lidx(x, y, z);
  if (!b || !b->mat[l]) return;
  b->mat[l] = 0;
  b->rows[l >> 4] &= (uint16_t)~(1u << (l & 15));
  b->count--;
  total_--;
  s.world.markBrickDirty(bi);
}

// Nearest free cell to c, searching outwards (upwards first at each distance:
// a gas displaced by something filling its cell squeezes out over the top).
bool GasLayer::findFree(const Volume& w, const ivec3& c, ivec3& out) const {
  if (free(w, c.x, c.y, c.z)) { out = c; return true; }
  for (int r = 1; r <= 8; r++) {
    int best = INT32_MAX;
    for (int dy = r; dy >= -r; dy--)
      for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
          if (std::max(std::abs(dx), std::max(std::abs(dy), std::abs(dz))) != r) continue;
          int d2 = dx * dx + dy * dy + dz * dz - dy;  // prefer up
          if (d2 >= best || !free(w, c.x + dx, c.y + dy, c.z + dz)) continue;
          best = d2;
          out = ivec3(c.x + dx, c.y + dy, c.z + dz);
        }
    if (best != INT32_MAX) return true;
  }
  return false;
}

void GasLayer::add(Sim& s, const ivec3& c, uint8_t mat, float temp) {
  ivec3 q(std::clamp(c.x, 0, nx_ - 1), std::clamp(c.y, 0, ny_ - 1), std::clamp(c.z, 0, nz_ - 1));
  ivec3 f;
  if (findFree(s.world, q, f)) put(s, f.x, f.y, f.z, mat, temp, stepStamp_);
  else pending_.push_back({q, mat, temp});
}

void GasLayer::step(Sim& s, float dt) {
  const Volume& w = s.world;
  hot.clear();
  // Anything that found no room last time tries again.
  if (!pending_.empty()) {
    std::vector<Pending> p;
    p.swap(pending_);
    for (auto& g : p) add(s, g.c, g.mat, g.temp);
  }
  if (!total_) return;
  stepStamp_ = stepStamp_ == 255 ? 1 : stepStamp_ + 1;
  const uint8_t stamp = stepStamp_;
  airStep_++;
  const float vs = s.vs;
  const AirSim& air = s.air;
  // Sample the air once per air cell (serially, so the workers only read it).
  std::vector<int> active;
  for (size_t b = 0; b < bricks.size(); b++) if (bricks[b] && bricks[b]->count) active.push_back((int)b);
  if (s.airReady) {
    if (airCache_.size() != (size_t)air.nx * air.ny * air.nz) airCache_.assign((size_t)air.nx * air.ny * air.nz, {vec3(0), AMBIENT, 0});
    int per = std::max(1, (int)std::lround(BS * vs / air.h));  // air cells a brick spans (rounded up below)
    for (int bi : active) {
      int bx = bi % bx_, bz = (bi / bx_) % bz_, by = bi / (bx_ * bz_);
      int ax0 = (int)(bx * BS * vs / air.h), ay0 = (int)(by * BS * vs / air.h), az0 = (int)(bz * BS * vs / air.h);
      for (int ay = ay0; ay <= ay0 + per && ay < air.ny; ay++)
        for (int az = az0; az <= az0 + per && az < air.nz; az++)
          for (int ax = ax0; ax <= ax0 + per && ax < air.nx; ax++) {
            AirSample& c = airCache_[(size_t)air.cid(ax, ay, az)];
            if (c.step == airStep_) continue;
            vec3 p((ax + 0.5f) * air.h, (ay + 0.5f) * air.h, (az + 0.5f) * air.h);
            c.v = air.velocity(p); c.T = air.temperature(p); c.step = airStep_;
          }
    }
  }
  auto airAt = [&](const vec3& p, vec3& v, float& T) {
    if (!s.airReady) { v = s.wind; T = AMBIENT; return; }
    int ax = std::clamp((int)(p.x / air.h), 0, air.nx - 1), ay = std::clamp((int)(p.y / air.h), 0, air.ny - 1),
        az = std::clamp((int)(p.z / air.h), 0, air.nz - 1);
    const AirSample& a = airCache_[(size_t)air.cid(ax, ay, az)];
    if (a.step == airStep_) { v = a.v; T = a.T; }
    else { v = air.velocity(p); T = air.temperature(p); }  // (only reached at a brick's far edge)
  };
  // Cooling per step, for a full-rate brick and a quiet one stepped every 4th time.
  float kHot[2], kCool[2], kSteam[2];
  for (int q = 0; q < 2; q++) {
    float h = q ? dt * 4 : dt;
    kHot[q] = 1.f - std::exp(-h / 0.45f); kCool[q] = 1.f - std::exp(-h / 2.f); kSteam[q] = 1.f - std::exp(-h / 3.f);
  }

  // Bricks are stepped in eight interleaved sets: two bricks in one set are
  // never neighbours, so the workers each change only their own brick. A
  // voxel crossing into a neighbouring brick, and anything that changes the
  // world (condensing, soot), is queued and applied afterwards.
  struct Out {
    std::vector<Pending> moves, condense, soot;
    std::vector<int> dirty;
    std::vector<vec3> hot;
    long escaped = 0, taken = 0;
  };
  std::vector<Out> outs(Jobs::get().threads());
  auto stepBrick = [&](int bi, Out& out) {
    GasBrick* gb = bricks[bi].get();
    // Cool smoke drifting on its own is stepped a quarter as often.
    int quiet = gb->lively ? 0 : 1;
    if (quiet && ((airStep_ + (uint32_t)bi) & 3)) return;
    const float h = quiet ? dt * 4 : dt;
    const int maxHop = quiet ? 4 : 2;
    bool lively = false, changed = false;
    int bx = bi % bx_, bz = (bi / bx_) % bz_, by = bi / (bx_ * bz_);
    auto takeHere = [&](int l) {
      gb->mat[l] = 0;
      gb->rows[l >> 4] &= (uint16_t)~(1u << (l & 15));
      gb->count--;
      out.taken++;
      changed = true;
    };
    for (int row = 0; row < BS * BS && gb->count; row++) {
      for (uint32_t bits = gb->rows[row]; bits; bits &= bits - 1) {
        int l = (row << 4) | std::countr_zero(bits);
        uint8_t m = gb->mat[l];
        if (!m || gb->stamp[l] == stamp) continue;
        int x = bx * 16 + (l & 15), z = bz * 16 + ((l >> 4) & 15), y = by * 16 + (l >> 8);
        float T = gb->temp[l];
        // Something solid or liquid moved in: squeeze out to the nearest free cell.
        if (w.mat(x, y, z) != AIR) {
          takeHere(l);
          out.moves.push_back({ivec3(x, y, z), m, T});
          continue;
        }
        vec3 p = s.cellCenter(x, y, z);
        vec3 v;
        float Tair;
        airAt(p, v, Tair);
        // Cool towards the surrounding air: flames dim within a second, steam
        // keeps its heat longer (it still has its latent heat to give up).
        float k = m == STEAM ? kSteam[quiet] : (T > 300 ? kHot[quiet] : kCool[quiet]);
        float Tn = T + (Tair - T) * k;
        if (glowByte(Tn, 0) != glowByte(T, 0)) changed = true;
        T = gb->temp[l] = Tn;
        if (m == STEAM || T > Tair + 15) lively = true;
        if (T > 520 && out.hot.size() < 12 && (rng().next() & 7) == 0) out.hot.push_back(p);
        // Steam below its dew point turns back into water.
        if (m == STEAM && T < 60) { takeHere(l); out.condense.push_back({ivec3(x, y, z), m, T}); continue; }
        // Cool smoke against a surface settles out of the air as soot.
        if (m == SMOKE && T < 60 && rng().f() < 0.02f * h) {
          bool touching = false;
          for (auto& d : DIRS6) {
            uint8_t n = w.mat(x + d[0], y + d[1], z + d[2]);
            if (n != AIR && !isLiquid(n)) { touching = true; break; }
          }
          if (touching) { takeHere(l); out.soot.push_back({ivec3(x, y, z), m, T}); continue; }
        }
        // Ride the air, rise by buoyancy, and wander (turbulent mixing).
        v.y += std::clamp((T - Tair) / 220.f, 0.f, 4.f) + (m == STEAM ? 0.35f : 0.05f);
        v += vec3(rng().f() - 0.5f, rng().f() - 0.5f, rng().f() - 0.5f) * 0.8f;
        vec3 d = v * (h / vs);
        ivec3 mv;
        for (int a = 0; a < 3; a++) mv[a] = std::clamp((int)std::floor(d[a] + rng().f()), -maxHop, maxHop);
        if (mv.x == 0 && mv.y == 0 && mv.z == 0) continue;
        ivec3 tries[4] = {mv, ivec3(0, mv.y, 0), ivec3(mv.x, 0, mv.z), ivec3(mv.x, 0, 0)};
        for (const ivec3& t : tries) {
          if (t.x == 0 && t.y == 0 && t.z == 0) continue;
          int tx = x + t.x, ty = y + t.y, tz = z + t.z;
          if (ty < 0) continue;
          if (tx < 0 || tz < 0 || tx >= nx_ || tz >= nz_ || ty >= ny_) {
            // Out through the open side or top of the world.
            takeHere(l);
            out.escaped++;
            break;
          }
          if (!free(w, tx, ty, tz)) continue;
          // A hop of several cells must not pass through a wall.
          int n = std::max(std::abs(t.x), std::max(std::abs(t.y), std::abs(t.z)));
          bool clear = true;
          for (int i = 1; i < n && clear; i++) clear = free(w, x + t.x * i / n, y + t.y * i / n, z + t.z * i / n);
          if (!clear) continue;
          takeHere(l);
          if ((tx >> 4) == bx && (ty >> 4) == by && (tz >> 4) == bz) {
            int tl = lidx(tx, ty, tz);
            gb->mat[tl] = m; gb->temp[tl] = T; gb->stamp[tl] = stamp;
            gb->rows[tl >> 4] |= (uint16_t)(1u << (tl & 15));
            gb->count++;
            out.taken--;
          } else {
            out.moves.push_back({ivec3(tx, ty, tz), m, T});
          }
          break;
        }
      }
    }
    gb->lively = lively;
    if (changed) out.dirty.push_back(bi);
  };
  for (int set = 0; set < 8; set++) {
    std::vector<int> batch;
    for (int bi : active) {
      int bx = bi % bx_, bz = (bi / bx_) % bz_, by = bi / (bx_ * bz_);
      if ((bx & 1) + 2 * (by & 1) + 4 * (bz & 1) == set) batch.push_back(bi);
    }
    if (batch.empty()) continue;
    Jobs::get().parallelFor((int)batch.size(), 4, [&](int b0, int b1, int t) {
      for (int i = b0; i < b1; i++) stepBrick(batch[i], outs[t]);
    });
    // Voxels that crossed into a neighbouring brick land now, before the
    // next set (which may hold that brick) is stepped.
    for (Out& o : outs) {
      total_ -= (size_t)o.taken;
      o.taken = 0;
      for (int bi : o.dirty) s.world.markBrickDirty(bi);
      o.dirty.clear();
      for (auto& g : o.moves) {
        if (free(w, g.c.x, g.c.y, g.c.z)) put(s, g.c.x, g.c.y, g.c.z, g.mat, g.temp, stamp);
        else add(s, g.c, g.mat, g.temp);
      }
      o.moves.clear();
    }
  }
  for (Out& o : outs) {
    escaped += o.escaped;
    for (auto& g : o.condense)
      if (!s.placeWater(g.c, g.temp)) add(s, g.c, STEAM, g.temp);
    for (auto& g : o.soot) {
      if (w.mat(g.c.x, g.c.y, g.c.z) == AIR) {
        Cell c;
        c.mat = ASH; c.temp = g.temp;
        s.putWorld(g.c.x, g.c.y, g.c.z, c);
      } else add(s, g.c, SMOKE, g.temp);
    }
    for (auto& p : o.hot) if (hot.size() < 48) hot.push_back(p);
  }
  // Drifting smoke passes through thousands of bricks: free the ones it has left.
  for (int bi : active)
    if (bricks[bi] && !bricks[bi]->count) bricks[bi].reset();
}
