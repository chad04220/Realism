// Structural stability. Every voxel has weight, and nothing holds it up but
// its neighbours: the voxel it rests on, the voxels it is bonded to beside it,
// or the one it hangs from. A voxel standing on an unbroken column of matter
// down to the bedrock is carried straight down and needs no more thought. The
// rest (anything above a gap: lintels, roofs, overhangs, branches, the roof of
// a tunnel) must pass its weight sideways or upwards through bonds to that
// ground-supported matter, and every bond has a strength:
//
//   * inside one part, the material's own tensile strength (dirt ~15 kPa,
//     brick 3 MPa, oak 50 MPa, steel 400 MPa) over the face between them;
//   * between two parts, their joint (mortar, nails, bolts, glue, a weld);
//   * loose rubble, or parts that merely rest, carry nothing sideways or in
//     tension: they can only rest on what is under them.
//
// The load path is found as a tree: each hanging voxel passes its weight, and
// the weight passed to it, to the neighbour nearest the supported matter
// (preferring the one under it, then beside, then above). That gives every
// bond a shear force and, for sideways bonds, a bending moment. Sideways bonds
// are checked a cross-section at a time (all the bonds stacked in one column
// carrying load the same way act as one beam section: moment capacity
// sigma * b * d^2 / 6); bonds hung from above are checked in tension. When a
// section or bond is overloaded, everything carried through it breaks away
// and falls as a rigid body, and what is left is checked again.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "sim.h"

namespace {

constexpr float G = 9.81f;

// Can this voxel be part of a structure (carry load through bonds)?
inline bool structural(uint8_t m) { return m != AIR && isSolid(m); }
// Can it carry weight resting on it (in compression)? Powders can.
inline bool bearing(uint8_t m) { return m != AIR && (isSolid(m) || isPowder(m)); }

// Strength of a joint between two different parts, Pa over the shared face.
float jointStress(const Joint& j) {
  const char* n = j.name ? j.name : "";
  if (!strcmp(n, "welded") || !strcmp(n, "cast") || !strcmp(n, "grown") || !strcmp(n, "ground")) return INFINITY;  // as the material
  if (!strcmp(n, "bolted")) return 1e7f;
  if (!strcmp(n, "press-fit")) return 2e6f;
  if (!strcmp(n, "glued")) return 3e6f;
  if (!strcmp(n, "screwed")) return 1e6f;
  if (!strcmp(n, "nailed")) return 5e5f;
  if (!strcmp(n, "mortared")) return 4e5f;
  if (!strcmp(n, "clipped")) return 1e5f;
  if (!strcmp(n, "resting")) return 0;
  // A joint loaded from a blueprint: scale its knock-off energy like the above.
  return std::min(1e7f, std::max(0.f, j.energy) * 1e4f);
}

}  // namespace

// Strength (Pa) of the bond across the face between two voxels.
static float bondStress(uint8_t ma, uint32_t ca, uint8_t hpa, uint8_t mb, uint32_t cb, uint8_t hpb) {
  float s = std::min(MATS[ma].tensile, MATS[mb].tensile);
  if (ca == 0 || cb == 0) return 0;  // loose rubble: it only rests
  if (ca != cb) {
    const auto& cs = comps();
    if (ca >= cs.size() || cb >= cs.size()) return 0;
    s = std::min(s, std::min(jointStress(cs[ca].joint), jointStress(cs[cb].joint)));
  }
  // Damaged voxels hold less.
  return s * (std::min(hpa, hpb) / 255.f);
}

namespace {

// One connected piece of hanging matter, solved on dense arrays over its
// bounding box (hash maps were the cost at 100k+ voxels).
struct Cluster {
  ivec3 lo, dim;  // bounding box, grown by one cell for neighbour lookups
  std::vector<ivec3> pos;
  std::vector<int> grid;  // local cell -> index into pos, or -1
  void build() {
    ivec3 a = pos[0], b = pos[0];
    for (auto& q : pos) { a = ivec3(std::min(a.x, q.x), std::min(a.y, q.y), std::min(a.z, q.z)); b = ivec3(std::max(b.x, q.x), std::max(b.y, q.y), std::max(b.z, q.z)); }
    lo = a - ivec3(1, 1, 1);
    dim = b - a + ivec3(3, 3, 3);
    grid.assign((size_t)dim.x * dim.y * dim.z, -1);
    for (int i = 0; i < (int)pos.size(); i++) {
      const ivec3& q = pos[i];
      grid[(size_t)(q.x - lo.x) + (size_t)dim.x * ((size_t)(q.z - lo.z) + (size_t)dim.z * (q.y - lo.y))] = i;
    }
  }
  int at(int x, int y, int z) const {
    x -= lo.x; y -= lo.y; z -= lo.z;
    if ((unsigned)x >= (unsigned)dim.x || (unsigned)y >= (unsigned)dim.y || (unsigned)z >= (unsigned)dim.z) return -1;
    return grid[(size_t)x + (size_t)dim.x * ((size_t)z + (size_t)dim.z * y)];
  }
};

}  // namespace

void Sim::processSupport() {
  if (supportSeeds.empty()) return;
  // Gathered and solved at most ten times a second: a burning tree changes
  // hundreds of voxels a second and one solve covers them all.
  // After a check that took t, the next waits at least 10 t: at most a tenth
  // of a core goes on structure however much is burning.
  const float wait = std::clamp(stats.structureMs * 0.01f, 0.1f, 0.5f);
  if (!fullCheck_ && !supportUrgent_ && time - lastSupportTime_ < wait && time >= lastSupportTime_) return;
  lastSupportTime_ = time;
  supportUrgent_ = false;
  auto t0 = std::chrono::steady_clock::now();
  std::vector<int64_t> seeds;
  seeds.swap(supportSeeds);
  std::sort(seeds.begin(), seeds.end());
  seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
  static const bool dbg = getenv("REALISM_DEBUG_STRUCT") != nullptr;

  // Ground-supported: an unbroken column of bearing matter down to y = 0.
  std::vector<int> gap((size_t)world.nx * world.nz, -1);
  auto gapOf = [&](int x, int z) {
    int& g = gap[(size_t)x + (size_t)world.nx * z];
    if (g < 0) { int y = 0; while (y < world.ny && bearing(world.mat(x, y, z))) y++; g = y; }
    return g;
  };
  auto anchored = [&](int x, int y, int z) { return y < gapOf(x, z) || world.mat(x, y, z) == BEDROCK; };

  // One bit per world cell marks matter already gathered into a cluster.
  const size_t cellsN = (size_t)world.nx * world.ny * world.nz;
  if (supportVisit_.size() != (cellsN + 63) / 64) supportVisit_.assign((cellsN + 63) / 64, 0);
  auto lin = [&](int x, int y, int z) { return (size_t)x + (size_t)world.nx * ((size_t)z + (size_t)world.nz * y); };
  auto seen = [&](int x, int y, int z) { size_t i = lin(x, y, z); return (supportVisit_[i >> 6] >> (i & 63)) & 1ull; };
  auto mark = [&](int x, int y, int z) { size_t i = lin(x, y, z); supportVisit_[i >> 6] |= 1ull << (i & 63); };

  std::vector<Cluster> clusters;
  std::vector<uint8_t> windowed;  // per cluster: any matter just outside it counts as holding it
  std::vector<int64_t> falling;
  if (fullCheck_) {
    const size_t LIMIT = 2000000;
    for (int64_t sk : seeds) {
      ivec3 p = unpackKey(sk);
      if (!world.inb(p.x, p.y, p.z) || seen(p.x, p.y, p.z) || !structural(world.matI(p)) || anchored(p.x, p.y, p.z)) continue;
      Cluster c;
      std::vector<ivec3> stack{p};
      mark(p.x, p.y, p.z);
      ivec3 lo = p, hi = p;
      bool truncated = false;
      while (!stack.empty()) {
        ivec3 q = stack.back();
        stack.pop_back();
        c.pos.push_back(q);
        lo = ivec3(std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z));
        hi = ivec3(std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z));
        for (auto& d : DIRS6) {
          int xx = q.x + d[0], yy = q.y + d[1], zz = q.z + d[2];
          if (!world.inb(xx, yy, zz) || seen(xx, yy, zz) || !structural(world.mat(xx, yy, zz)) || anchored(xx, yy, zz)) continue;
          if (c.pos.size() + stack.size() >= LIMIT) { truncated = true; continue; }
          mark(xx, yy, zz);
          stack.push_back({xx, yy, zz});
        }
      }
      if (truncated) fprintf(stderr, "realism: structure check limited to %zu voxels\n", LIMIT);
      (void)hi;
      c.build();
      if (dbg && c.pos.size() > 20000)
        fprintf(stderr, "structure: cluster of %zu from seed %d %d %d (%s), box %d..%d %d..%d %d..%d\n", c.pos.size(), p.x, p.y, p.z,
                MATS[world.matI(p)].name, c.lo.x, c.lo.x + c.dim.x, c.lo.y, c.lo.y + c.dim.y, c.lo.z, c.lo.z + c.dim.z);
      clusters.push_back(std::move(c));
    }
    for (auto& c : clusters)
      for (auto& q : c.pos) { size_t i = lin(q.x, q.y, q.z); supportVisit_[i >> 6] &= ~(1ull << (i & 63)); }

    windowed.assign(clusters.size(), 0);
  } else {
    // (a) Anything cut off from the ground entirely falls, however big: walk
    // from each change towards the ground (down last, so depth first heads
    // there) and stop as soon as ground-supported matter is reached.
    std::unordered_set<int64_t> fallen;
    std::vector<ivec3> stack, walk;
    // Matter an earlier walk this pass already found connected to the ground:
    // reaching it is as good as reaching the ground (it is one connected piece).
    if (supportOk_.size() != supportVisit_.size()) supportOk_.assign(supportVisit_.size(), 0);
    std::vector<ivec3> okList;
    auto known = [&](int x, int y, int z) { size_t i = lin(x, y, z); return (supportOk_[i >> 6] >> (i & 63)) & 1ull; };
    for (int64_t sk : seeds) {
      ivec3 p = unpackKey(sk);
      if (!world.inb(p.x, p.y, p.z) || seen(p.x, p.y, p.z) || known(p.x, p.y, p.z) || fallen.count(sk) || !structural(world.matI(p)) ||
          anchored(p.x, p.y, p.z))
        continue;
      stack.assign(1, p);
      walk.clear();
      mark(p.x, p.y, p.z);
      bool supported = false;
      while (!stack.empty() && !supported) {
        ivec3 q = stack.back();
        stack.pop_back();
        walk.push_back(q);
        if (walk.size() > 4000000) { supported = true; break; }
        static const int orderDirs[6] = {2, 0, 1, 4, 5, 3};  // up, sides, then down (popped first)
        for (int k : orderDirs) {
          int xx = q.x + DIRS6[k][0], yy = q.y + DIRS6[k][1], zz = q.z + DIRS6[k][2];
          if (yy < 0) { supported = true; break; }
          if (!world.inb(xx, yy, zz) || !structural(world.mat(xx, yy, zz))) continue;
          if (anchored(xx, yy, zz) || known(xx, yy, zz)) { supported = true; break; }
          if (seen(xx, yy, zz)) continue;
          mark(xx, yy, zz);
          stack.push_back({xx, yy, zz});
        }
      }
      for (auto& q : walk) { size_t i = lin(q.x, q.y, q.z); supportVisit_[i >> 6] &= ~(1ull << (i & 63)); }
      for (auto& q : stack) { size_t i = lin(q.x, q.y, q.z); supportVisit_[i >> 6] &= ~(1ull << (i & 63)); }
      if (supported)
        for (auto& q : walk) { size_t i = lin(q.x, q.y, q.z); supportOk_[i >> 6] |= 1ull << (i & 63); okList.push_back(q); }
      if (!supported)
        for (auto& q : walk) { int64_t k = packKey(q.x, q.y, q.z); if (fallen.insert(k).second) falling.push_back(k); }
    }
    for (auto& q : okList) { size_t i = lin(q.x, q.y, q.z); supportOk_[i >> 6] &= ~(1ull << (i & 63)); }
    // (b) Strength is re-checked for the matter within R bonds of the
    // changes. What lies beyond is taken from the last solve: matter that was
    // farther from the ground still hangs its load and moment on the window's
    // edge (so notching a beam's root feels the whole beam), matter that was
    // nearer holds it up.
    const int R = std::max(6, (int)std::lround(0.3f / vs));
    Cluster W;
    std::vector<ivec3> frontier, next;
    for (int64_t sk : seeds) {
      ivec3 p = unpackKey(sk);
      if (!world.inb(p.x, p.y, p.z) || seen(p.x, p.y, p.z) || fallen.count(sk) || !structural(world.matI(p)) || anchored(p.x, p.y, p.z)) continue;
      mark(p.x, p.y, p.z);
      frontier.push_back(p);
    }
    for (int depth = 0; depth <= R && !frontier.empty(); depth++) {
      next.clear();
      for (auto& q : frontier) {
        W.pos.push_back(q);
        if (depth == R) continue;
        for (auto& dd : DIRS6) {
          int xx = q.x + dd[0], yy = q.y + dd[1], zz = q.z + dd[2];
          if (!world.inb(xx, yy, zz) || seen(xx, yy, zz) || !structural(world.mat(xx, yy, zz)) || anchored(xx, yy, zz)) continue;
          if (fallen.count(packKey(xx, yy, zz))) continue;
          mark(xx, yy, zz);
          next.push_back({xx, yy, zz});
        }
      }
      frontier.swap(next);
    }
    for (auto& q : frontier) { size_t i = lin(q.x, q.y, q.z); supportVisit_[i >> 6] &= ~(1ull << (i & 63)); }
    for (auto& q : W.pos) { size_t i = lin(q.x, q.y, q.z); supportVisit_[i >> 6] &= ~(1ull << (i & 63)); }
    if (!W.pos.empty()) {
      W.build();
      clusters.push_back(std::move(W));
      windowed.push_back(1);
    }
  }
  const float gatherMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  float routeMs = 0, loadMs = 0;
  int totalN = 0, totalSections = 0;
  size_t totalFailed = 0;
  for (size_t ci = 0; ci < clusters.size(); ci++) {
    const Cluster& C = clusters[ci];
    const bool win = windowed[ci];
    const int n = (int)C.pos.size();
    totalN += n;
    const auto& pos = C.pos;
    std::vector<Cell> cells(n);
    for (int i = 0; i < n; i++) cells[i] = world.get(pos[i].x, pos[i].y, pos[i].z);
    // Bonds that broke in an earlier round of this check. When one support
    // gives way, a real structure carries the load round it if it can; only
    // what then has no route that holds falls.
    std::unordered_set<int64_t> broken;
    auto faceKey = [&](const ivec3& p, int k) {
      ivec3 q = (k & 1) ? ivec3(p.x + DIRS6[k][0], p.y + DIRS6[k][1], p.z + DIRS6[k][2]) : p;
      return packKey(q.x, q.y, q.z) * 4 + (k >> 1);
    };
    for (int round = 0;; round++) {

    // 2. Each voxel's route to the ground-supported matter: the path of least
    // effort, which is the path a real structure's load takes. Bearing down
    // on what is underneath (compression, very stiff) is cheap; passing load
    // sideways or hanging from above (bending, tension) costs more the weaker
    // the bond, and is impossible through a bond with no strength.
    const float SREF = 1e6f;
    auto stepCost = [&](int k, float s) { return k == 3 ? 0.05f : (s > 0 ? 1.f + SREF / s : INFINITY); };
    // Neighbour j of voxel i in direction k: index >= 0, -1 ground-supported
    // matter (or the bedrock floor), -2 nothing that can hold it.
    auto neighbour = [&](int i, int k, float& s) -> int {
      const ivec3& p = pos[i];
      int xx = p.x + DIRS6[k][0], yy = p.y + DIRS6[k][1], zz = p.z + DIRS6[k][2];
      if (yy < 0) { s = INFINITY; return -1; }
      int j = C.at(xx, yy, zz);
      if (j >= 0) {
        s = bondStress(cells[i].mat, cells[i].comp, cells[i].hp, cells[j].mat, cells[j].comp, cells[j].hp);
        if (!broken.empty() && broken.count(faceKey(p, k))) s = 0;  // still bears in compression
        return j;
      }
      if (!world.inb(xx, yy, zz)) return -2;
      Cell o = world.get(xx, yy, zz);
      if (!structural(o.mat)) return -2;
      if (win && !anchored(xx, yy, zz)) {
        // Just outside the window: it holds this voxel up only if it was
        // nearer the ground than this voxel last time.
        const VoxLoad *us = loadState_.find(packKey(xx, yy, zz)), *me = loadState_.find(packKey(p.x, p.y, p.z));
        if (us && me && us->dist > me->dist) return -2;
      } else if (!anchored(xx, yy, zz)) return -2;
      s = bondStress(cells[i].mat, cells[i].comp, cells[i].hp, o.mat, o.comp, o.hp);
      if (!broken.empty() && broken.count(faceKey(p, k))) s = 0;
      return -1;
    };
    std::vector<float> dist(n, INFINITY);
    using QE = std::pair<float, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    for (int i = 0; i < n; i++) {
      for (int k = 0; k < 6; k++) {
        float s;
        if (neighbour(i, k, s) != -1) continue;
        dist[i] = std::min(dist[i], stepCost(k, s));
      }
      if (std::isfinite(dist[i])) pq.push({dist[i], i});
    }
    std::vector<int> order;  // settled nearest first
    order.reserve(n);
    std::vector<uint8_t> done(n, 0);
    while (!pq.empty()) {
      auto [d, i] = pq.top();
      pq.pop();
      if (done[i] || d > dist[i]) continue;
      done[i] = 1;
      order.push_back(i);
      for (int k = 0; k < 6; k++) {
        // The neighbour on the far side of face k passes load to i in the opposite direction.
        const int back = k ^ 1;
        float s;
        int j = neighbour(i, k, s);
        if (j < 0 || done[j]) continue;
        float nd = d + stepCost(back, s);
        if (nd < dist[j]) { dist[j] = nd; pq.push({nd, j}); }
      }
    }

    auto tRoute = std::chrono::steady_clock::now();
    // 3. A voxel shares its load among every neighbour nearer the ground, in
    // proportion to how stiff each path is (a voxel standing on another puts
    // nearly all its weight straight down; foliage round a branch hangs from
    // every twig that touches it, not from one).
    struct Edge { int to; uint8_t k; float s, share; bool failed; };
    std::vector<Edge> edges;
    std::vector<int> firstEdge(n + 1, 0);
    for (int i = 0; i < n; i++) {
      firstEdge[i] = (int)edges.size();
      if (!done[i]) continue;
      float wsum = 0;
      const int e0 = (int)edges.size();
      for (int k = 0; k < 6; k++) {
        float s;
        int j = neighbour(i, k, s);
        if (j == -2 || (j >= 0 && (!done[j] || dist[j] >= dist[i]))) continue;
        float c = stepCost(k, s);
        if (!std::isfinite(c)) continue;
        edges.push_back({j, (uint8_t)k, s, 1.f / c, false});
        wsum += 1.f / c;
      }
      for (int e = e0; e < (int)edges.size(); e++) edges[e].share /= wsum;
    }
    firstEdge[n] = (int)edges.size();
    std::vector<float> weight(n), load(n, 0);
    // Moment of everything a voxel carries about that voxel, N m, as two
    // signed components (lever arms along x and along z), so loads on
    // opposite sides cancel and twisting isn't mistaken for bending.
    std::vector<float> mx(n, 0), mz(n, 0);
    for (int i = 0; i < n; i++) {
      const ivec3& p = pos[i];
      float w = MATS[cells[i].mat].voxMass * G;
      // Liquid or loose matter standing on it presses down too.
      for (int yy = p.y + 1; yy < world.ny; yy++) {
        uint8_t m = world.mat(p.x, yy, p.z);
        if (m == AIR || isSolid(m) || MATS[m].phase == GAS) break;
        w += MATS[m].voxMass * G;
      }
      weight[i] = w;
    }
    if (win) {
      // Load hung on the window's edge from outside it, as solved last time,
      // shared like the outside voxel shared it among its supports.
      for (int i = 0; i < n; i++) {
        const ivec3& p = pos[i];
        const VoxLoad* me = loadState_.find(packKey(p.x, p.y, p.z));
        if (!me) continue;
        for (int k = 0; k < 6; k++) {
          int ux = p.x - DIRS6[k][0], uy = p.y - DIRS6[k][1], uz = p.z - DIRS6[k][2];  // u's step to i is direction k
          if (C.at(ux, uy, uz) >= 0 || !world.inb(ux, uy, uz) || !structural(world.mat(ux, uy, uz))) continue;
          const VoxLoad* us = loadState_.find(packKey(ux, uy, uz));
          if (!us || us->dist <= me->dist) continue;
          int supports = 0;
          for (int kk = 0; kk < 6; kk++) {
            const VoxLoad* v2 = loadState_.find(packKey(ux + DIRS6[kk][0], uy + DIRS6[kk][1], uz + DIRS6[kk][2]));
            if (v2 && v2->dist < us->dist) supports++;
          }
          const float share = 1.f / std::max(1, supports);
          const VoxLoad& U = *us;
          load[i] += U.load * share;
          mx[i] += (U.mx - U.load * DIRS6[k][0] * vs) * share;
          mz[i] += (U.mz - U.load * DIRS6[k][2] * vs) * share;
        }
      }
    }

    auto tLoad = std::chrono::steady_clock::now();
    routeMs += std::chrono::duration<float, std::milli>(tLoad - tRoute).count();
    // 4. Carry the weight down, farthest first, and check every bond.
    const float A = vs * vs;
    struct SideEdge { int edge, from, y; float M, V; };
    std::unordered_map<int64_t, std::vector<SideEdge>> sections;  // keyed by the interface: column and direction
    for (int q = (int)order.size() - 1; q >= 0; q--) {
      int i = order[q];
      load[i] += weight[i];
      for (int e = firstEdge[i]; e < firstEdge[i + 1]; e++) {
        Edge& E = edges[e];
        const int k = E.k;
        const float V = load[i] * E.share;
        // Moving the reference point one voxel towards the support lengthens
        // every lever arm by one voxel the other way.
        const float Mx = (mx[i] - load[i] * DIRS6[k][0] * vs) * E.share;
        const float Mz = (mz[i] - load[i] * DIRS6[k][2] * vs) * E.share;
        if (k == 2 && V > E.s * A) {  // hanging from the voxel above: tension
          E.failed = true;
          if (dbg) {
            const ivec3& p = pos[i];
            Cell a = world.get(p.x, p.y, p.z), b = world.get(p.x, p.y + 1, p.z);
            auto nm = [&](const Cell& c) { return c.comp && c.comp < comps().size() ? comps()[c.comp].name + "/" + comps()[c.comp].object : std::string("loose"); };
            fprintf(stderr, "structure: hanger at %d %d %d fails: %.3g N > %.3g N: %s %s from %s %s\n", p.x, p.y, p.z, V, E.s * A,
                    MATS[a.mat].name, nm(a).c_str(), MATS[b.mat].name, nm(b).c_str());
          }
        }
        if (DIRS6[k][1] == 0) {
          const ivec3& p = pos[i];
          int64_t key = ((int64_t)p.x * 8192 + p.z) * 8 + k;
          sections[key].push_back({e, i, p.y, DIRS6[k][0] != 0 ? Mx : Mz, V});
        }
        if (E.to >= 0) { load[E.to] += V; mx[E.to] += Mx; mz[E.to] += Mz; }
      }
    }
    // Sideways sections. Where load crosses between two neighbouring
    // columns, the beam that resists it is all the matter bonded across that
    // interface, however the load was routed: the vertical run of bonded
    // faces around the crossing (a 10 cm rafter is a 10 cm beam even if its
    // load reaches the wall along its bottom voxels). Crossings that share a
    // run share a section.
    int sectionFails = 0;
    for (auto& [key, list] : sections) {
      std::sort(list.begin(), list.end(), [](const SideEdge& a, const SideEdge& b) { return a.y < b.y; });
      const ivec3& c0 = pos[list[0].from];
      const int k = edges[list[0].edge].k;
      const int dx = DIRS6[k][0], dz = DIRS6[k][2];
      auto faceStress = [&](int y) -> float {
        if (y < 0 || y >= world.ny) return 0;
        Cell a = world.get(c0.x, y, c0.z), b = world.get(c0.x + dx, y, c0.z + dz);
        if (!structural(a.mat) || !structural(b.mat)) return 0;
        return bondStress(a.mat, a.comp, a.hp, b.mat, b.comp, b.hp);
      };
      for (size_t a = 0; a < list.size();) {
        int y0 = list[a].y, y1 = list[a].y;
        double sSum = faceStress(y0);
        while (y1 - y0 < 255 && faceStress(y1 + 1) > 0) { y1++; sSum += faceStress(y1); }
        while (y1 - y0 < 255 && faceStress(y0 - 1) > 0) { y0--; sSum += faceStress(y0); }
        const int runLen = y1 - y0 + 1;
        size_t b = a;
        double Ms = 0, Vs = 0;
        while (b < list.size() && list[b].y <= y1) { Ms += list[b].M; Vs += list[b].V; b++; }
        Ms = std::fabs(Ms);
        const float s = (float)(sSum / runLen);  // mortar and brick across a wall: their mean
        const float depth = runLen * vs;
        const double Mcap = (double)s * vs * depth * depth / 6.0;
        const double Vcap = 0.6 * s * runLen * A;
        if (Ms > Mcap || Vs > Vcap) {
          for (size_t e = a; e < b; e++) edges[list[e].edge].failed = true;
          sectionFails++;
          if (dbg) {
            Cell cc = world.get(c0.x, list[a].y, c0.z), pc = world.get(c0.x + dx, list[a].y, c0.z + dz);
            auto nm = [&](const Cell& c) {
              return c.comp && c.comp < comps().size() ? comps()[c.comp].name + "/" + comps()[c.comp].object + " (" +
                                                             (comps()[c.comp].joint.name ? comps()[c.comp].joint.name : "?") + ")"
                                                       : std::string("loose");
            };
            fprintf(stderr, "structure: section at %d %d %d (%d deep) fails: M %.3g > %.3g N m or V %.3g > %.3g N: %s %s -> %s %s\n",
                    c0.x, list[a].y, c0.z, runLen, Ms, Mcap, Vs, Vcap, MATS[cc.mat].name, nm(cc).c_str(), MATS[pc.mat].name,
                    nm(pc).c_str());
          }
        }
        a = b;
      }
    }
    totalSections += sectionFails;
    loadMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - tLoad).count();

    // 5. A voxel falls when every one of its supports has failed or is
    // falling itself (nearest the ground first, so that is known), or when it
    // has no route to the ground at all.
    std::vector<uint8_t> falls(n, 0);
    for (auto& E : edges) totalFailed += E.failed;
    if (!dryRun_) for (auto& E : edges) stats.bondsFailed += E.failed;
    for (int i : order) {
      bool held = false;
      for (int e = firstEdge[i]; e < firstEdge[i + 1] && !held; e++)
        if (!edges[e].failed && (edges[e].to < 0 || !falls[edges[e].to])) held = true;
      if (!held) falls[i] = 1;
    }
    int failedNow = 0;
    for (auto& E : edges) failedNow += E.failed;
    if (failedNow && round < 5) {
      for (int i = 0; i < n; i++)
        for (int e = firstEdge[i]; e < firstEdge[i + 1]; e++)
          if (edges[e].failed) broken.insert(faceKey(pos[i], edges[e].k));
      continue;  // re-route round them
    }
    for (int i = 0; i < n; i++) {
      const int64_t key = packKey(pos[i].x, pos[i].y, pos[i].z);
      if (!done[i] || falls[i]) { falling.push_back(key); loadState_.put(key, {INFINITY, 0, 0, 0}); continue; }
      // Remember the answer: the next local check uses it as its boundary.
      loadState_.put(key, {dist[i], load[i], mx[i], mz[i]});
    }
    break;
    }  // rounds
  }
  if (loadState_.count > 8000000) loadState_.clear();  // never grows without bound
  if (dryRun_) falling.clear();
  stats.structureMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (dbg) fprintf(stderr, "structure: %zu clusters, %d hanging voxels, %zu bonds failed (%d sections), %zu voxels fall, %.1f ms (gather %.1f, edges %.1f, loads %.1f, %zu seeds)\n",
                   clusters.size(), totalN, totalFailed, totalSections, falling.size(), stats.structureMs, gatherMs, routeMs, loadMs, seeds.size());
  if (falling.empty()) return;
  if (dbg && falling.size() > 100) {
    std::unordered_map<std::string, int> what;
    ivec3 lo(INT32_MAX, INT32_MAX, INT32_MAX), hi(INT32_MIN, INT32_MIN, INT32_MIN);
    for (int64_t k : falling) {
      ivec3 c = unpackKey(k);
      Cell cc = world.get(c.x, c.y, c.z);
      what[std::string(MATS[cc.mat].name) + " " + (cc.comp && cc.comp < comps().size() ? comps()[cc.comp].name : "loose")]++;
      lo = ivec3(std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z));
      hi = ivec3(std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z));
    }
    fprintf(stderr, "structure: falling box %d..%d %d..%d %d..%d:", lo.x, hi.x, lo.y, hi.y, lo.z, hi.z);
    for (auto& [w, n] : what) fprintf(stderr, " %s x%d;", w.c_str(), n);
    fprintf(stderr, "\n");
  }
  stats.collapses++;
  // What is left has lost a neighbour: check it again.
  std::unordered_set<int64_t> fallSet(falling.begin(), falling.end());
  for (int64_t k : falling) {
    ivec3 c = unpackKey(k);
    for (auto& d : DIRS6) {
      int xx = c.x + d[0], yy = c.y + d[1], zz = c.z + d[2];
      if (world.inb(xx, yy, zz) && structural(world.mat(xx, yy, zz)) && !fallSet.count(packKey(xx, yy, zz)))
        supportSeeds.push_back(packKey(xx, yy, zz));
    }
  }
  // Each separate falling piece becomes its own body (or loose debris if tiny).
  std::vector<int64_t> piece;
  std::vector<int64_t> st;
  for (int64_t k0 : falling) {
    if (!fallSet.count(k0)) continue;
    piece.clear();
    st.assign(1, k0);
    fallSet.erase(k0);
    while (!st.empty()) {
      int64_t k = st.back();
      st.pop_back();
      piece.push_back(k);
      ivec3 c = unpackKey(k);
      for (auto& d : DIRS6) {
        int64_t kk = packKey(c.x + d[0], c.y + d[1], c.z + d[2]);
        if (fallSet.erase(kk)) st.push_back(kk);
      }
    }
    if (piece.size() <= 10) {
      for (int64_t k : piece) { ivec3 c = unpackKey(k); removeWorld(c.x, c.y, c.z, true); }
    } else if (Body* b = detachWorld(piece)) {
      b->splitCheck = true;
    }
  }
}

void Sim::checkAllStructures(bool apply) {
  fullCheck_ = true;
  dryRun_ = !apply;
  loadState_.clear();
  lastSupportTime_ = -1;
  // Seed every voxel above the first gap in its column: all hanging matter.
  for (int z = 0; z < world.nz; z++)
    for (int x = 0; x < world.nx; x++) {
      int y = 0;
      while (y < world.ny && bearing(world.mat(x, y, z))) y++;
      for (; y < world.ny; y++)
        if (structural(world.mat(x, y, z))) supportSeeds.push_back(packKey(x, y, z));
    }
  processSupport();
  fullCheck_ = false;
  dryRun_ = false;
}
