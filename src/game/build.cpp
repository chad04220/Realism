#include "build.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

void Shaper::put(int x, int y, int z) {
  if (!v->inb(x, y, z)) return;
  if (!overwrite && v->mat(x, y, z) != AIR) return;
  if (mask && !mask(centre(x, y, z))) return;
  v->set(x, y, z, mat, comp);
}

void Shaper::box(const vec3& lo, const vec3& hi) {
  ivec3 a = toCell(lo + vec3(vs * 0.5f)), b = toCell(hi - vec3(vs * 0.5f));
  for (int y = a.y; y <= b.y; y++)
    for (int z = a.z; z <= b.z; z++)
      for (int x = a.x; x <= b.x; x++) put(x, y, z);
}

void Shaper::boxIf(const vec3& lo, const vec3& hi, const std::function<bool(const vec3&)>& f) {
  ivec3 a = toCell(lo + vec3(vs * 0.5f)), b = toCell(hi - vec3(vs * 0.5f));
  for (int y = a.y; y <= b.y; y++)
    for (int z = a.z; z <= b.z; z++)
      for (int x = a.x; x <= b.x; x++)
        if (f(centre(x, y, z))) put(x, y, z);
}

void Shaper::sphere(const vec3& c, float r) { ellipsoid(c, vec3(r)); }

void Shaper::ellipsoid(const vec3& c, const vec3& r) {
  boxIf(c - r, c + r, [&](const vec3& p) {
    vec3 d = (p - c) / r;
    return dot(d, d) <= 1.f;
  });
}

void Shaper::cylinder(const vec3& a, const vec3& b, float r) {
  vec3 ab = b - a;
  float L2 = dot(ab, ab);
  vec3 lo = vmin(a, b) - vec3(r), hi = vmax(a, b) + vec3(r);
  boxIf(lo, hi, [&](const vec3& p) {
    float t = dot(p - a, ab) / L2;
    if (t < 0 || t > 1) return false;
    return length(p - (a + ab * t)) <= r;
  });
}

void Shaper::tube(const vec3& a, const vec3& b, float rOut, float rIn) {
  vec3 ab = b - a;
  float L2 = dot(ab, ab);
  vec3 lo = vmin(a, b) - vec3(rOut), hi = vmax(a, b) + vec3(rOut);
  boxIf(lo, hi, [&](const vec3& p) {
    float t = dot(p - a, ab) / L2;
    if (t < 0 || t > 1) return false;
    float d = length(p - (a + ab * t));
    return d <= rOut && d >= rIn;
  });
}

void Shaper::line(const vec3& a, const vec3& b) {
  ivec3 p = toCell(a), q = toCell(b);
  // Manhattan path so the wire is 6-connected (current flows face to face).
  ivec3 c = p;
  put(c.x, c.y, c.z);
  while (c != q) {
    if (c.x != q.x) c.x += q.x > c.x ? 1 : -1;
    else if (c.y != q.y) c.y += q.y > c.y ? 1 : -1;
    else c.z += q.z > c.z ? 1 : -1;
    put(c.x, c.y, c.z);
  }
}

void Shaper::clear(const vec3& lo, const vec3& hi) {
  ivec3 a = toCell(lo + vec3(vs * 0.5f)), b = toCell(hi - vec3(vs * 0.5f));
  for (int y = a.y; y <= b.y; y++)
    for (int z = a.z; z <= b.z; z++)
      for (int x = a.x; x <= b.x; x++)
        if (v->inb(x, y, z)) v->set(x, y, z, AIR);
}

Body* spawnBody(Sim& s, Volume&& vol, const vec3& worldMin) {
  // Crop to the occupied region so bodies stay small.
  ivec3 lo(1 << 30, 1 << 30, 1 << 30), hi(-1, -1, -1);
  vol.forEach([&](int x, int y, int z, Brick*, int) {
    lo = {std::min(lo.x, x), std::min(lo.y, y), std::min(lo.z, z)};
    hi = {std::max(hi.x, x), std::max(hi.y, y), std::max(hi.z, z)};
  });
  if (hi.x < 0) return nullptr;
  Body* b = s.newBody();
  b->vol.resize(hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1);
  vol.forEach([&](int x, int y, int z, Brick* br, int l) {
    Cell c;
    c.mat = br->mat[l]; c.flags = br->flags[l]; c.hp = br->hp[l]; c.aux = br->aux[l];
    c.comp = br->comp[l]; c.temp = br->temp[l]; c.fuel = br->fuel[l];
    b->vol.put(x - lo.x, y - lo.y, z - lo.z, c);
  });
  b->mass = 0;
  s.rebuildBody(*b);
  b->pos = worldMin + (b->com + toF(lo)) * s.vs;
  b->rot = quat();
  b->updateInertiaWorld();
  b->heat.reset(b->vol.bricks.size());
  for (size_t i = 0; i < b->vol.bricks.size(); i++) if (b->vol.bricks[i]) b->heat.add((int)i);
  return b;
}

bool routeWire(Volume& v, const ivec3& from, const ivec3& to, uint32_t comp, uint8_t wmat, std::vector<ivec3>* placed) {
  auto special = [&](int x, int y, int z) {
    uint8_t m = v.mat(x, y, z);
    return m != AIR && (MATS[m].wire || isSpecialElectric(m) || m == SWITCH_OFF);
  };
  // A cell may carry wire if its only electrical neighbours are the endpoints.
  auto okCell = [&](const ivec3& c, bool start, bool goal) {
    if (!v.inb(c.x, c.y, c.z) || v.matI(c) != AIR) return false;
    for (auto& d : DIRS6) {
      ivec3 q(c.x + d[0], c.y + d[1], c.z + d[2]);
      if (!special(q.x, q.y, q.z)) continue;
      if (start && q == from) continue;
      if (goal && q == to) continue;
      return false;
    }
    return true;
  };
  const int nx = v.nx, ny = v.ny, nz = v.nz;
  std::vector<int> prev((size_t)nx * ny * nz, -2);
  auto lin = [&](const ivec3& c) { return c.x + nx * (c.z + nz * c.y); };
  std::vector<ivec3> q;
  size_t head = 0;
  std::vector<char> isGoal((size_t)nx * ny * nz, 0);
  for (auto& d : DIRS6) {
    ivec3 g(to.x + d[0], to.y + d[1], to.z + d[2]);
    if (okCell(g, false, true)) isGoal[lin(g)] = 1;
  }
  for (auto& d : DIRS6) {
    ivec3 s(from.x + d[0], from.y + d[1], from.z + d[2]);
    if (!okCell(s, true, false)) {
      // A start cell that also touches the goal is fine (adjacent endpoints).
      if (!(v.inb(s.x, s.y, s.z) && v.matI(s) == AIR)) continue;
      bool ok = true;
      for (auto& e : DIRS6) {
        ivec3 n(s.x + e[0], s.y + e[1], s.z + e[2]);
        if (special(n.x, n.y, n.z) && n != from && n != to) ok = false;
      }
      if (!ok) continue;
    }
    prev[lin(s)] = -1;
    q.push_back(s);
  }
  int found = -1;
  while (head < q.size()) {
    ivec3 c = q[head++];
    int ci = lin(c);
    if (isGoal[ci]) { found = ci; break; }
    // Prefer straight runs: try the same order every time (x, z, y).
    static const int order[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}};
    for (auto& d : order) {
      ivec3 n(c.x + d[0], c.y + d[1], c.z + d[2]);
      if (!v.inb(n.x, n.y, n.z)) continue;
      int ni = lin(n);
      if (prev[ni] != -2) continue;
      if (!okCell(n, false, isGoal[ni] != 0)) continue;
      prev[ni] = ci;
      q.push_back(n);
    }
  }
  if (found < 0) {
    if (std::getenv("REALISM_DEBUG_WIRE"))
      fprintf(stderr, "routeWire %d,%d,%d -> %d,%d,%d failed (%d candidates)\n", from.x, from.y, from.z, to.x, to.y, to.z, (int)q.size());
    return false;
  }
  for (int i = found; i >= 0; i = prev[i]) {
    int x = i % nx, z = (i / nx) % nz, y = i / (nx * nz);
    v.set(x, y, z, wmat, comp);
    if (placed) placed->push_back({x, y, z});
    if (prev[i] == -1) break;
  }
  return true;
}
