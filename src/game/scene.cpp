#include "scene.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <unordered_map>
#include "build.h"
#include "parts.h"
#include "assemblies.h"

namespace {

float fbm2(float x, float z) {
  auto h = [](int a, int b) { return (hash3(a, b, 7) & 0xFFFF) / 65535.f; };
  auto n = [&](float x, float z) {
    int ix = (int)std::floor(x), iz = (int)std::floor(z);
    float fx = x - ix, fz = z - iz;
    fx = fx * fx * (3 - 2 * fx); fz = fz * fz * (3 - 2 * fz);
    float a = h(ix, iz), b = h(ix + 1, iz), c = h(ix, iz + 1), d = h(ix + 1, iz + 1);
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fz);
  };
  return n(x, z) * 0.6f + n(x * 2.1f, z * 2.1f) * 0.3f + n(x * 4.3f, z * 4.3f) * 0.1f;
}

void setBurning(Volume& v, const ivec3& c, float temp) {
  Brick* b = v.brick(c.x, c.y, c.z);
  if (!b) return;
  int l = lidx(c.x, c.y, c.z);
  if (!b->mat[l]) return;
  b->flags[l] |= F_BURN;
  b->fuel[l] = MATS[b->mat[l]].burnTime * 0.8f;
  b->temp[l] = temp;
}

}  // namespace

void rescanWorld(Sim& s) {
  s.worldSpecials.clear();
  s.worldHot.reset(s.world.bricks.size());
  s.world.forEach([&](int x, int y, int z, Brick* br, int l) {
    uint8_t m = br->mat[l];
    if (isSpecialElectric(m) || MATS[m].wire) s.worldSpecials.insert(packKey(x, y, z));
    if (br->temp[l] != AMBIENT || br->flags[l]) s.worldHot.add(s.world.bindex(x, y, z));
  });
}

SceneInfo buildScene(Sim& s) {
  SceneInfo info;
  const float vs = s.vs;
  const float GY = 1.0f;
  info.groundY = GY;
  Volume& W = s.world;
  Shaper sh(&W, vs);
  const float WX = W.nx * vs, WZ = W.nz * vs;

  // ---------------------------------------------------------------- terrain
  uint32_t cBed = newComp(makePart("Bedrock", "Ground", joints::terrain, C_TERRAIN));
  uint32_t cStone = newComp(makePart("Granite bed", "Ground", joints::terrain, C_TERRAIN));
  uint32_t cSoil = newComp(makePart("Subsoil", "Ground", joints::terrain, C_TERRAIN));
  uint32_t cTurf = newComp(makePart("Lawn", "Ground", joints::terrain, C_TERRAIN));
  auto groundH = [&](float x, float z) {
    // Flat in the middle, gently rolling towards the edges.
    float edge = std::min(std::min(x, WX - x), std::min(z, WZ - z));
    float roll = (fbm2(x * 0.35f, z * 0.35f) - 0.5f) * 0.35f;
    (void)roll; (void)edge;
    return GY;
  };
  for (int z = 0; z < W.nz; z++)
    for (int x = 0; x < W.nx; x++) {
      float px = (x + 0.5f) * vs, pz = (z + 0.5f) * vs;
      float h = groundH(px, pz);
      int top = (int)std::floor(h / vs);
      for (int y = 0; y < top && y < W.ny; y++) {
        float py = (y + 0.5f) * vs;
        uint8_t m;
        uint32_t c;
        if (py < 0.1f) { m = BEDROCK; c = cBed; }
        else if (py < h - 0.45f) { m = STONE; c = cStone; }
        else if (py < h - 0.05f) { m = DIRT; c = cSoil; }
        else { m = GRASS; c = cTurf; }
        W.set(x, y, z, m, c);
      }
    }

  // ---------------------------------------------------------------- pond
  {
    vec3 pc(12.0f, GY, 11.6f);
    vec3 pr(1.35f, 0.5f, 0.95f);
    uint32_t cWater = newComp(makePart("Pond water", "Pond", joints::terrain, C_TERRAIN));
    uint32_t cBedSand = newComp(makePart("Pond bed", "Pond", joints::terrain, C_TERRAIN));
    for (int z = 0; z < W.nz; z++)
      for (int x = 0; x < W.nx; x++) {
        float px = (x + 0.5f) * vs, pz = (z + 0.5f) * vs;
        float dx = (px - pc.x) / pr.x, dz = (pz - pc.z) / pr.z;
        float r2 = dx * dx + dz * dz + (fbm2(px * 2, pz * 2) - 0.5f) * 0.25f;
        if (r2 > 1.f) continue;
        float depth = 0.48f * std::sqrt(std::max(0.f, 1.f - r2));
        float bottom = GY - depth;
        for (int y = (int)(bottom / vs) - 2; y < (int)(GY / vs) + 3; y++) {
          float py = (y + 0.5f) * vs;
          if (py < bottom - 2 * vs) continue;
          if (py < bottom) W.set(x, y, z, SAND, cBedSand);
          else if (py < GY - 0.06f) W.set(x, y, z, WATER, cWater);
          else W.set(x, y, z, AIR);
        }
      }
    // Rocks around the rim.
    for (int i = 0; i < 9; i++) {
      float a = i * 2 * PI / 9 + 0.3f;
      vec3 p = pc + vec3(std::cos(a) * pr.x * 1.08f, 0.02f, std::sin(a) * pr.z * 1.1f);
      uint32_t c = newComp(makePart("Rim stone", "Pond", joints::resting));
      sh.with(STONE, c).ellipsoid(p, vec3(0.13f, 0.08f, 0.11f) * (0.8f + 0.4f * ((hash3(i, 5, 5) & 255) / 255.f)));
    }
  }

  // ---------------------------------------------------------------- sand pile
  {
    uint32_t c = newComp(makePart("Sand", "Sand pile", joints::resting));
    vec3 p(2.6f, GY, 11.0f);
    sh.with(SAND, c).boxIf(p - vec3(0.9f, 0, 0.9f), p + vec3(0.9f, 0.65f, 0.9f), [&](const vec3& q) {
      float r = length(vec3(q.x - p.x, 0, q.z - p.z));
      return q.y - p.y < 0.6f * (1 - r / 0.85f) + (fbm2(q.x * 6, q.z * 6) - 0.5f) * 0.05f;
    });
  }

  // ---------------------------------------------------------------- shed
  const float x0 = 2.4f, x1 = 6.4f, z0 = 2.2f, z1 = 5.6f, t = 0.1f;
  const float base = GY + 0.05f, wallH = 2.2f, top = base + wallH;
  const float zc = (z0 + z1) * 0.5f, hw = (z1 - z0) * 0.5f + 0.15f, rise = 1.15f;
  auto roofY = [&](float z) { return top + rise * (1.f - std::fabs(z - zc) / hw); };
  const float doorX0 = 3.4f, doorX1 = 4.4f, doorTop = base + 2.0f;
  const float winZ0 = 3.4f, winZ1 = 4.4f, winY0 = base + 0.9f, winY1 = base + 1.7f;
  {
    // Concrete slab.
    uint32_t slab = newComp(makePart("Floor slab", "Shed", joints::cast));
    sh.with(CONCRETE, slab).box(vec3(x0 - 0.1f, GY - 0.12f, z0 - 0.1f), vec3(x1 + 0.1f, base, z1 + 0.1f));
    std::unordered_map<int64_t, uint32_t> brickComp, mortarComp;
    auto brickAt = [&](int wall, int course, int idx) {
      int64_t k = ((int64_t)wall << 40) | ((int64_t)course << 20) | (idx & 0xFFFFF);
      auto it = brickComp.find(k);
      if (it != brickComp.end()) return it->second;
      uint32_t c = newComp(makePart("Brick", "Shed", joints::mortar));
      brickComp[k] = c;
      return c;
    };
    auto mortarAt = [&](int wall, int course) {
      int64_t k = ((int64_t)wall << 20) | course;
      auto it = mortarComp.find(k);
      if (it != mortarComp.end()) return it->second;
      uint32_t c = newComp(makePart("Mortar joint", "Shed", joints::mortar));
      mortarComp[k] = c;
      return c;
    };
    // Walls: 0 north, 1 south, 2 west, 3 east.
    struct Wall { vec3 lo, hi; int along; float start; };
    Wall walls[4] = {
        {vec3(x0, base, z0), vec3(x1, top + rise + 0.2f, z0 + t), 0, x0},
        {vec3(x0, base, z1 - t), vec3(x1, top + rise + 0.2f, z1), 0, x0},
        {vec3(x0, base, z0), vec3(x0 + t, top + rise + 0.2f, z1), 2, z0},
        {vec3(x1 - t, base, z0), vec3(x1, top + rise + 0.2f, z1), 2, z0},
    };
    for (int wi = 0; wi < 4; wi++) {
      const Wall& w = walls[wi];
      ivec3 a = sh.toCell(w.lo + vec3(vs * 0.5f)), b = sh.toCell(w.hi - vec3(vs * 0.5f));
      for (int y = a.y; y <= b.y; y++)
        for (int z = a.z; z <= b.z; z++)
          for (int x = a.x; x <= b.x; x++) {
            vec3 p = sh.centre(x, y, z);
            float h = p.y - base;
            // Gable triangles on the east and west walls; flat tops elsewhere.
            // Walls run up to the underside of the tiles (gables on east/west).
            if (p.y > roofY(p.z) - (wi >= 2 ? 0.06f : 0.0f)) continue;
            // Openings.
            if (wi == 1 && p.x > doorX0 && p.x < doorX1 && p.y < doorTop + 0.1f) continue;
            if (wi == 2 && p.z > winZ0 && p.z < winZ1 && p.y > winY0 && p.y < winY1 + 0.1f) continue;
            int course = (int)std::floor(h / 0.1f);
            float fv = h - course * 0.1f;
            float u = (w.along == 0 ? p.x : p.z) - w.start + (course % 2) * 0.125f;
            int bi = (int)std::floor(u / 0.25f);
            float fu = u - bi * 0.25f;
            bool mortar = fv >= 0.075f || fu >= 0.225f;
            if (mortar) W.set(x, y, z, MORTAR, mortarAt(wi, course));
            else W.set(x, y, z, BRICK, brickAt(wi, course, bi));
          }
    }
    // Oak lintels over the door and window.
    uint32_t lin1 = newComp(makePart("Door lintel", "Shed", joints::mortar));
    sh.with(OAK, lin1).box(vec3(doorX0 - 0.1f, doorTop, z1 - t), vec3(doorX1 + 0.1f, doorTop + 0.1f, z1));
    uint32_t lin2 = newComp(makePart("Window lintel", "Shed", joints::mortar));
    sh.with(OAK, lin2).box(vec3(x0, winY1, winZ0 - 0.1f), vec3(x0 + t, winY1 + 0.1f, winZ1 + 0.1f));
    // Window: oak frame and a glass pane.
    uint32_t frame = newComp(makePart("Window frame", "Shed", joints::screwed));
    sh.with(OAK, frame).boxIf(vec3(x0, winY0, winZ0), vec3(x0 + t, winY1, winZ1), [&](const vec3& p) {
      return p.y < winY0 + 0.05f || p.y > winY1 - 0.05f || p.z < winZ0 + 0.05f || p.z > winZ1 - 0.05f ||
             std::fabs(p.z - (winZ0 + winZ1) * 0.5f) < 0.02f;
    });
    for (int pane = 0; pane < 2; pane++) {
      uint32_t g = newComp(makePart("Window pane", "Shed", joints::glued));
      float za = pane == 0 ? winZ0 + 0.05f : (winZ0 + winZ1) * 0.5f + 0.0125f;
      float zb = pane == 0 ? (winZ0 + winZ1) * 0.5f - 0.0125f : winZ1 - 0.05f;
      sh.with(GLASS, g).box(vec3(x0 + 0.0375f, winY0 + 0.05f, za), vec3(x0 + 0.0625f, winY1 - 0.05f, zb));
    }
    // Rafters and clay tiles.
    for (float rx = x0 - 0.1f; rx <= x1 + 0.1f; rx += 0.5f) {
      uint32_t c = newComp(makePart("Rafter", "Shed roof", joints::nailed));
      float ra = std::min(rx, x1 + 0.05f);
      sh.with(OAK, c).boxIf(vec3(ra, top - 0.1f, z0 - 0.2f), vec3(ra + 0.05f, top + rise + 0.05f, z1 + 0.2f), [&](const vec3& p) {
        float ry = roofY(p.z);
        return p.y <= ry && p.y > ry - 0.1f;
      });
    }
    std::unordered_map<int64_t, uint32_t> tileComp;
    sh.mat = TILE;
    ivec3 a = sh.toCell(vec3(x0 - 0.15f, top - 0.2f, z0 - 0.25f)), b = sh.toCell(vec3(x1 + 0.15f, top + rise + 0.15f, z1 + 0.25f));
    for (int y = a.y; y <= b.y; y++)
      for (int z = a.z; z <= b.z; z++)
        for (int x = a.x; x <= b.x; x++) {
          vec3 p = sh.centre(x, y, z);
          float ry = roofY(p.z);
          if (!(p.y > ry && p.y <= ry + 0.05f)) continue;
          // Each tile covers 25 cm across and ~25 cm down the slope.
          int ti = (int)std::floor((p.x - x0 + 0.15f) / 0.25f);
          float s_ = std::fabs(p.z - zc);
          int tj = (int)std::floor(s_ / 0.22f) * (p.z < zc ? -1 : 1);
          int64_t k = ((int64_t)ti << 20) ^ (int64_t)(tj + 1000);
          auto it = tileComp.find(k);
          uint32_t c;
          if (it == tileComp.end()) { c = newComp(makePart("Roof tile", "Shed roof", joints::clipped)); tileComp[k] = c; }
          else c = it->second;
          W.set(x, y, z, TILE, c);
        }
    // Door: pine planks with battens, hanging open on two brass hinges.
    float dz0 = z1, dz1 = z1 + 0.95f, dx0 = doorX0, dx1 = doorX0 + 0.05f;
    for (int pl = 0; pl < 6; pl++) {
      uint32_t c = newComp(makePart("Door plank", "Shed door", joints::nailed));
      float za = dz0 + 0.05f + pl * 0.15f;
      sh.with(PINE, c).box(vec3(dx0, base + 0.03f, za), vec3(dx1, doorTop - 0.03f, std::min(za + 0.15f, dz1)));
    }
    for (int bt = 0; bt < 3; bt++) {
      uint32_t c = newComp(makePart("Door batten", "Shed door", joints::nailed));
      float y = base + 0.25f + bt * 0.7f;
      sh.with(PINE, c).box(vec3(dx1, y, dz0 + 0.1f), vec3(dx1 + 0.025f, y + 0.1f, dz1 - 0.05f));
    }
    for (int hi = 0; hi < 2; hi++) {
      uint32_t c = newComp(makePart("Hinge", "Shed door", joints::bolted));
      float y = base + 0.3f + hi * 1.3f;
      sh.with(BRASS, c).box(vec3(dx0 - 0.05f, y, z1 - 0.05f), vec3(dx1, y + 0.075f, z1 + 0.1f));
    }
    uint32_t handle = newComp(makePart("Door handle", "Shed door", joints::screwed));
    sh.with(IRON, handle).box(vec3(dx1 + 0.025f, base + 1.0f, dz1 - 0.2f), vec3(dx1 + 0.075f, base + 1.05f, dz1 - 0.1f));
    // Stone sill under the window, a ridge cap along the top, gutters on both eaves.
    uint32_t sill = newComp(makePart("Window sill", "Shed", joints::mortar));
    sh.with(STONE, sill).box(vec3(x0 - 0.075f, winY0 - 0.05f, winZ0 - 0.075f), vec3(x0 + 0.025f, winY0, winZ1 + 0.075f));
    uint32_t ridge = newComp(makePart("Ridge tile", "Shed roof", joints::mortar));
    sh.with(TILE, ridge).boxIf(vec3(x0 - 0.15f, top + rise - 0.05f, zc - 0.125f), vec3(x1 + 0.15f, top + rise + 0.125f, zc + 0.125f), [&](const vec3& p) {
      return p.y > roofY(p.z) + 0.025f && p.y < roofY(zc) + 0.1f - std::fabs(p.z - zc) * 0.4f;
    });
    for (int side = 0; side < 2; side++) {
      uint32_t g = newComp(makePart("Gutter", "Shed roof", joints::screwed));
      float ze = side ? z1 + 0.25f : z0 - 0.25f, dir = side ? 1.f : -1.f;
      float gy = roofY(ze) - 0.1f;
      sh.with(STEEL, g).boxIf(vec3(x0 - 0.15f, gy, std::min(ze, ze + dir * 0.125f)), vec3(x1 + 0.15f, gy + 0.1f, std::max(ze, ze + dir * 0.125f)), [&](const vec3& p) {
        float u = (p.z - ze) * dir;  // 0 at the roof edge, 0.125 at the lip
        return p.y < gy + 0.03f || u > 0.1f || u < 0.025f;
      });
    }
  }

  // ---------------------------------------------------------------- wood stove
  // A steel firebox in the shed's south-east corner, logs alight inside, and
  // a flue through the roof. The fire draws air in at the door vents and the
  // hot gas rises up the flue: smoke leaves at the chimney, not the room.
  {
    const std::string obj = "Wood stove";
    vec3 lo(5.55f, base, 4.45f), hi(6.05f, base + 0.65f, 4.95f);
    float legH = 0.1f, wallT = 0.05f;
    for (int k = 0; k < 4; k++) {
      uint32_t c = newComp(makePart("Stove leg", obj, joints::welded));
      float lx = (k & 1) ? hi.x - 0.075f : lo.x + 0.025f, lz = (k & 2) ? hi.z - 0.075f : lo.z + 0.025f;
      sh.with(IRON, c).box(vec3(lx, lo.y, lz), vec3(lx + 0.05f, lo.y + legH, lz + 0.05f));
    }
    vec3 bl = lo + vec3(0, legH, 0);
    uint32_t body = newComp(makePart("Firebox", obj, joints::welded));
    vec3 fc((lo.x + hi.x) * 0.5f, hi.y, (lo.z + hi.z) * 0.5f);  // flue centre on the top plate
    sh.with(STEEL, body).boxIf(bl, hi, [&](const vec3& p) {
      bool shell = p.x < bl.x + wallT || p.x > hi.x - wallT || p.y < bl.y + wallT || p.y > hi.y - wallT || p.z < bl.z + wallT || p.z > hi.z - wallT;
      if (!shell) return false;
      if (p.x < bl.x + wallT && p.y > bl.y + wallT && p.y < hi.y - wallT && p.z > bl.z + wallT && p.z < hi.z - wallT) return false;  // door opening
      if (p.y > hi.y - wallT && length(vec3(p.x - fc.x, 0, p.z - fc.z)) < 0.06f) return false;                                // flue collar
      return true;
    });
    // Door: an iron frame with a ceramic-glass window and air slots along the bottom.
    uint32_t door = newComp(makePart("Stove door", obj, joints::bolted));
    uint32_t glass = newComp(makePart("Door glass", obj, joints::glued));
    float dx = bl.x - 0.025f;
    sh.with(IRON, door).boxIf(vec3(dx, bl.y + wallT, bl.z + wallT), vec3(bl.x + wallT * 0.5f, hi.y - wallT, hi.z - wallT), [&](const vec3& p) {
      float yy = p.y - (bl.y + wallT);
      if (yy > 0.05f && yy < 0.075f && std::fmod(p.z - bl.z, 0.075f) < 0.05f) return false;  // air slots
      return yy < 0.1f || p.y > hi.y - wallT - 0.05f || p.z < bl.z + wallT + 0.05f || p.z > hi.z - wallT - 0.05f;
    });
    sh.with(GLASS, glass).box(vec3(dx, bl.y + wallT + 0.1f, bl.z + wallT + 0.05f), vec3(dx + vs, hi.y - wallT - 0.05f, hi.z - wallT - 0.05f));
    uint32_t knob = newComp(makePart("Door latch", obj, joints::screwed));
    sh.with(BRASS, knob).box(vec3(dx - 0.05f, bl.y + 0.3f, hi.z - 0.13f), vec3(dx, bl.y + 0.35f, hi.z - 0.08f));
    // Logs and embers inside, already burning.
    uint32_t ash = newComp(makePart("Ember bed", obj, joints::resting));
    sh.with(CHARCOAL, ash).box(bl + vec3(wallT, wallT, wallT), vec3(hi.x - wallT, bl.y + wallT + 0.05f, hi.z - wallT));
    for (int k = 0; k < 3; k++) {
      uint32_t lg = newComp(makePart("Split log", obj, joints::resting));
      float z = bl.z + 0.12f + k * 0.12f, y = bl.y + wallT + 0.1f + (k == 1 ? 0.08f : 0.f);
      sh.with(PINE, lg).cylinder(vec3(bl.x + 0.1f, y, z), vec3(hi.x - 0.1f, y, z), 0.045f);
    }
    W.forEach([&](int x, int y, int z, Brick* br, int l) {
      uint32_t cc = br->comp[l];
      if (cc == ash || (comp(cc).name == "Split log" && comp(cc).object == obj && rng().f() < 0.35f)) setBurning(W, {x, y, z}, 700);
    });
    // Flue: steel pipe from the collar up through the roof, with a rain cap.
    float flueTop = roofY(fc.z) + 0.7f;
    sh.clear(vec3(fc.x - 0.11f, top - 0.2f, fc.z - 0.11f), vec3(fc.x + 0.11f, roofY(fc.z) + 0.25f, fc.z + 0.11f));
    uint32_t flue = newComp(makePart("Flue pipe", obj, joints::pressfit));
    sh.with(STEEL, flue).tube(fc, vec3(fc.x, flueTop, fc.z), 0.075f, 0.05f);
    uint32_t flash = newComp(makePart("Roof flashing", obj, joints::screwed));
    sh.with(ALU, flash).boxIf(vec3(fc.x - 0.15f, roofY(fc.z) - 0.1f, fc.z - 0.15f), vec3(fc.x + 0.15f, roofY(fc.z) + 0.2f, fc.z + 0.15f), [&](const vec3& p) {
      float r = length(vec3(p.x - fc.x, 0, p.z - fc.z));
      return r > 0.075f && r < 0.15f && p.y > roofY(p.z) && p.y < roofY(p.z) + 0.03f;
    });
    uint32_t cap = newComp(makePart("Rain cap", obj, joints::screwed));
    sh.with(STEEL, cap).cylinder(vec3(fc.x, flueTop + 0.08f, fc.z), vec3(fc.x, flueTop + 0.105f, fc.z), 0.13f);
    for (int k = 0; k < 3; k++) {
      float a = k * 2 * PI / 3;
      sh.with(STEEL, cap).box(vec3(fc.x + std::cos(a) * 0.08f, flueTop - 0.01f, fc.z + std::sin(a) * 0.08f),
                              vec3(fc.x + std::cos(a) * 0.08f + vs, flueTop + 0.085f, fc.z + std::sin(a) * 0.08f + vs));
    }
  }

  // ---------------------------------------------------------------- workshop pad
  {
    uint32_t pad = newComp(makePart("Workshop pad", "Workshop", joints::cast));
    sh.with(CONCRETE, pad).box(vec3(8.0f, GY - 0.1f, 0.4f), vec3(11.2f, GY + 0.05f, 3.4f));
    uint32_t paint = newComp(makePart("Painted line", "Workshop", joints::glued));
    sh.with(PAINT_WHITE, paint).boxIf(vec3(8.1f, GY + 0.025f, 0.5f), vec3(11.1f, GY + 0.05f, 3.3f), [&](const vec3& p) {
      return p.x < 8.15f || p.x > 11.05f || p.z < 0.55f || p.z > 3.25f;
    });
    info.jigOrigin = vec3(8.4f, GY + 0.05f, 0.8f);
    info.jigDims = ivec3((int)std::lround(2.4f / vs), (int)std::lround(1.6f / vs), (int)std::lround(2.0f / vs));
  }

  // ---------------------------------------------------------------- tree
  {
    vec3 tp(12.3f, GY, 4.6f);
    uint32_t trunk = newComp(makePart("Trunk", "Oak tree", joints::grown));
    uint32_t bark = newComp(makePart("Bark", "Oak tree", joints::grown));
    sh.with(OAK, trunk).cylinder(tp - vec3(0, 0.35f, 0), tp + vec3(0, 2.5f, 0), 0.12f);
    sh.with(BARK, bark).tube(tp, tp + vec3(0, 2.3f, 0), 0.15f, 0.115f);
    for (int r = 0; r < 4; r++) {
      float a = r * PI * 0.5f + 0.4f;
      uint32_t root = newComp(makePart("Root", "Oak tree", joints::grown));
      sh.with(OAK, root).cylinder(tp + vec3(0, 0.05f, 0), tp + vec3(std::cos(a) * 0.45f, -0.25f, std::sin(a) * 0.45f), 0.05f);
    }
    Rng rr(77);
    vec3 crown = tp + vec3(0, 2.4f, 0);
    for (int bI = 0; bI < 6; bI++) {
      float a = bI * 2 * PI / 6 + rr.range(-0.3f, 0.3f);
      float h = rr.range(1.4f, 2.2f);
      vec3 from = tp + vec3(0, h, 0);
      vec3 to = from + vec3(std::cos(a) * rr.range(0.6f, 0.9f), rr.range(0.4f, 0.7f), std::sin(a) * rr.range(0.6f, 0.9f));
      uint32_t br = newComp(makePart("Branch", "Oak tree", joints::grown));
      sh.with(OAK, br).cylinder(from, to, 0.045f);
      uint32_t lf = newComp(makePart("Foliage", "Oak tree", joints::grown));
      sh.with(LEAVES, lf);
      sh.overwrite = false;
      sh.mask = [&](const vec3& p) { return (hash3((int)(p.x / vs), (int)(p.y / vs), (int)(p.z / vs)) & 255) > 70; };
      sh.ellipsoid(to + vec3(0, 0.1f, 0), vec3(rr.range(0.45f, 0.6f), rr.range(0.35f, 0.45f), rr.range(0.45f, 0.6f)));
      sh.mask = nullptr;
      sh.overwrite = true;
    }
    uint32_t lf = newComp(makePart("Foliage", "Oak tree", joints::grown));
    sh.with(LEAVES, lf);
    sh.overwrite = false;
    sh.mask = [&](const vec3& p) { return (hash3((int)(p.x / vs), (int)(p.y / vs), (int)(p.z / vs)) & 255) > 70; };
    sh.ellipsoid(crown + vec3(0, 0.3f, 0), vec3(0.9f, 0.65f, 0.9f));
    sh.mask = nullptr;
    sh.overwrite = true;
  }

  // ---------------------------------------------------------------- column
  {
    vec3 cp(14.0f, GY, 13.4f);
    uint32_t col = newComp(makePart("Concrete column", "Column", joints::cast));
    sh.with(CONCRETE, col).box(cp - vec3(0.15f, 0.2f, 0.15f), cp + vec3(0.15f, 2.3f, 0.15f));
    for (int k = 0; k < 4; k++) {
      uint32_t rb = newComp(makePart("Rebar", "Column", joints::cast));
      float ox = (k & 1) ? 0.075f : -0.1f, oz = (k & 2) ? 0.075f : -0.1f;
      sh.with(STEEL, rb).box(cp + vec3(ox, -0.15f, oz), cp + vec3(ox + vs, 2.25f, oz + vs));
    }
    uint32_t cap = newComp(makePart("Capstone", "Column", joints::mortar));
    sh.with(STONE, cap).box(cp + vec3(-0.25f, 2.3f, -0.25f), cp + vec3(0.25f, 2.42f, 0.25f));
  }

  // ---------------------------------------------------------------- campfire
  vec3 fireP(7.6f, GY, 9.4f);
  {
    uint32_t pad = newComp(makePart("Hearth stones", "Campfire", joints::resting));
    sh.with(STONE, pad).boxIf(fireP - vec3(0.5f, 0.1f, 0.5f), fireP + vec3(0.5f, 0.0f, 0.5f), [&](const vec3& p) {
      return length(vec3(p.x - fireP.x, 0, p.z - fireP.z)) < 0.48f;
    });
    for (int i = 0; i < 9; i++) {
      float a = i * 2 * PI / 9;
      uint32_t c = newComp(makePart("Ring stone", "Campfire", joints::resting));
      sh.with(STONE, c).ellipsoid(fireP + vec3(std::cos(a) * 0.42f, 0.05f, std::sin(a) * 0.42f), vec3(0.09f, 0.08f, 0.09f));
    }
    uint32_t coal = newComp(makePart("Charcoal bed", "Campfire", joints::resting));
    sh.with(CHARCOAL, coal).boxIf(fireP - vec3(0.3f, 0, 0.3f), fireP + vec3(0.3f, 0.05f, 0.3f), [&](const vec3& p) {
      return length(vec3(p.x - fireP.x, 0, p.z - fireP.z)) < 0.28f;
    });
    W.forEach([&](int x, int y, int z, Brick* br, int l) {
      if (br->comp[l] == coal && rng().f() < 0.7f) setBurning(W, {x, y, z}, 760);
    });
  }

  // ---------------------------------------------------------------- water tower
  // A steel tank on braced legs beside the pond, full of water. A downpipe
  // runs to a short spout with a brass gate valve: open it (E) and the water
  // jets out at the speed the head above it gives, sqrt(2 g h).
  {
    const std::string obj = "Water tower";
    auto V = [&](float m) { return (int)std::lround(m / vs); };
    int tx0 = V(14.55f), tz0 = V(10.2f), tw = V(0.65f), ty0 = V(GY + 1.45f), th = V(0.7f);
    uint32_t tank = newComp(makePart("Tank", obj, joints::welded));
    uint32_t water = newComp(makePart("Water", obj, joints::terrain, C_TERRAIN));
    for (int y = ty0; y < ty0 + th; y++)
      for (int z = tz0; z < tz0 + tw; z++)
        for (int x = tx0; x < tx0 + tw; x++) {
          bool wall = x == tx0 || x == tx0 + tw - 1 || z == tz0 || z == tz0 + tw - 1 || y < ty0 + 2;
          bool rim = y == ty0 + th - 1;
          if (wall) W.set(x, y, z, STEEL, tank);
          else if (!rim && y < ty0 + th - 3) W.set(x, y, z, WATER, water);
        }
    // Bands round the tank.
    for (int band = 0; band < 2; band++) {
      uint32_t b = newComp(makePart("Tank band", obj, joints::welded));
      int y0 = ty0 + 6 + band * V(0.38f);
      for (int y = y0; y < y0 + 2; y++)
        for (int z = tz0 - 1; z <= tz0 + tw; z++)
          for (int x = tx0 - 1; x <= tx0 + tw; x++)
            if (x == tx0 - 1 || x == tx0 + tw || z == tz0 - 1 || z == tz0 + tw) W.set(x, y, z, STEEL, b);
    }
    // Legs and cross bracing.
    int gy = V(GY);
    int lx[2] = {tx0 + 1, tx0 + tw - 3}, lz[2] = {tz0 + 1, tz0 + tw - 3};
    for (int i = 0; i < 2; i++)
      for (int j = 0; j < 2; j++) {
        uint32_t leg = newComp(makePart("Tower leg", obj, joints::bolted));
        for (int y = gy - 6; y < ty0; y++)
          for (int dz = 0; dz < 2; dz++)
            for (int dx = 0; dx < 2; dx++) W.set(lx[i] + dx, y, lz[j] + dz, STEEL, leg);
      }
    for (int f = 0; f < 4; f++) {
      uint32_t br = newComp(makePart("Brace", obj, joints::bolted));
      bool alongX = f < 2;
      int fixed = alongX ? (f == 0 ? lz[0] : lz[1] + 1) : (f == 2 ? lx[0] : lx[1] + 1);
      int a = alongX ? lx[0] : lz[0], b = alongX ? lx[1] + 1 : lz[1] + 1;
      for (int k = 0; k <= b - a; k++) {
        float t = (float)k / (b - a);
        int y1 = gy + 4 + (int)(t * (ty0 - gy - 8)), y2 = gy + 4 + (int)((1 - t) * (ty0 - gy - 8));
        for (int yy : {y1, y2})
          for (int t2 = 0; t2 < 2; t2++) {
            if (alongX) W.set(a + k, yy + t2, fixed, STEEL, br);
            else W.set(fixed, yy + t2, a + k, STEEL, br);
          }
      }
    }
    // Downpipe on the pond side: 2x2 voxels of water inside a steel wall,
    // from the tank floor to a one-cell spout pointing at the pond.
    uint32_t pipe = newComp(makePart("Downpipe", obj, joints::welded));
    int px = tx0 + 3, pz = tz0 + tw / 2 - 1, spoutY = gy + V(0.45f);
    for (int y = spoutY - 1; y < ty0 + 2; y++)
      for (int dz = -1; dz <= 2; dz++)
        for (int dx = -1; dx <= 2; dx++) {
          bool inner = dx >= 0 && dx <= 1 && dz >= 0 && dz <= 1 && y >= spoutY;
          if (inner) W.set(px + dx, y, pz + dz, WATER, water);
          else if (W.mat(px + dx, y, pz + dz) != WATER) W.set(px + dx, y, pz + dz, STEEL, pipe);
        }
    // The gate sits flush in the pipe's -x wall, facing the pond, so the
    // water behind it carries the whole head of the column above.
    uint32_t gate = newComp(makePart("Gate valve", obj, joints::bolted, C_VALVE));
    int gx = px - 1;
    for (int y = spoutY; y <= spoutY + 1; y++)
      for (int dz = 0; dz <= 1; dz++) {
        Cell c; c.mat = BRASS; c.comp = gate; c.aux = 1;
        W.put(gx, y, pz + dz, c);
      }
    // Valve body around the gate, a stem and a handwheel on top.
    for (int y = spoutY - 1; y <= spoutY + 2; y++)
      for (int dz = -1; dz <= 2; dz++) {
        if (y >= spoutY && y <= spoutY + 1 && dz >= 0 && dz <= 1) continue;
        Cell c; c.mat = BRASS; c.comp = gate; c.aux = 0;
        W.put(gx - 1, y, pz + dz, c);
      }
    for (int y = spoutY + 3; y <= spoutY + 6; y++) { Cell c; c.mat = STEEL; c.comp = gate; W.put(gx - 1, y, pz, c); }
    for (int a = 0; a < 24; a++) {
      float ang = a * 2 * PI / 24;
      int hx = gx - 1 + (int)std::lround(std::cos(ang) * 3), hz = pz + (int)std::lround(std::sin(ang) * 3);
      Cell c; c.mat = IRON; c.comp = gate; c.aux = 0;
      W.put(hx, spoutY + 6, hz, c);
    }
    // The gate's outer face is open air at the spout mouth.
    for (int y = spoutY; y <= spoutY + 1; y++)
      for (int dz = 0; dz <= 1; dz++) W.set(gx - 1, y, pz + dz, AIR);
  }

  // ---------------------------------------------------------------- fence
  // Post-and-rail along the north and west edges of the yard.
  {
    auto run = [&](vec3 a, vec3 b) {
      vec3 d = b - a;
      float len = length(d);
      int posts = (int)std::ceil(len / 1.2f);
      vec3 u = d / len;
      for (int i = 0; i <= posts; i++) {
        vec3 p = a + u * (len * i / posts);
        uint32_t c = newComp(makePart("Fence post", "Fence", joints::pressfit));
        sh.with(OAK, c).box(p - vec3(0.04f, 0.25f, 0.04f), p + vec3(0.04f, 1.0f, 0.04f));
        sh.with(OAK, c).boxIf(p + vec3(-0.04f, 1.0f, -0.04f), p + vec3(0.04f, 1.05f, 0.04f), [&](const vec3& q) {
          return q.y < p.y + 1.0f + 0.05f - std::max(std::fabs(q.x - p.x), std::fabs(q.z - p.z));
        });
      }
      for (int r = 0; r < 2; r++) {
        for (int i = 0; i < posts; i++) {
          uint32_t c = newComp(makePart("Fence rail", "Fence", joints::nailed));
          vec3 p0 = a + u * (len * i / posts), p1 = a + u * (len * (i + 1) / posts);
          vec3 side = normalize(cross(u, vec3(0, 1, 0))) * 0.06f;
          float y = 0.4f + r * 0.4f;
          sh.with(PINE, c).box(vec3(std::min(p0.x, p1.x), p0.y + y, std::min(p0.z, p1.z)) + vec3(std::min(side.x, 0.f), 0, std::min(side.z, 0.f)) * 0.5f + vec3(0, 0, 0),
                               vec3(std::max(p0.x, p1.x), p0.y + y + 0.1f, std::max(p0.z, p1.z)) + vec3(std::fabs(side.x), 0, std::fabs(side.z)) * 0.5f + vec3(0.025f, 0, 0.025f));
        }
      }
    };
    run(vec3(0.35f, GY, 0.35f), vec3(7.4f, GY, 0.35f));
    run(vec3(0.35f, GY, 0.35f), vec3(0.35f, GY, 9.2f));
  }

  rescanWorld(s);

  // ---------------------------------------------------------------- bodies
  // Workbench.
  const float benchTop = base + 0.9f;
  {
    vec3 o(2.9f, base, 2.35f);
    Volume v((int)std::ceil(1.8f / vs), (int)std::ceil(0.92f / vs), (int)std::ceil(0.8f / vs));
    Shaper b(&v, vs, o);
    for (int k = 0; k < 4; k++) {
      uint32_t c = newComp(makePart("Top board", "Workbench", joints::screwed));
      b.with(OAK, c).box(o + vec3(0.05f, 0.85f, 0.025f + k * 0.175f), o + vec3(1.75f, 0.9f, 0.025f + (k + 1) * 0.175f));
    }
    for (int k = 0; k < 4; k++) {
      uint32_t c = newComp(makePart("Steel leg", "Workbench", joints::bolted));
      float lx = (k & 1) ? 1.6f : 0.1f, lz = (k & 2) ? 0.6f : 0.075f;
      b.with(STEEL, c).box(o + vec3(lx, 0, lz), o + vec3(lx + 0.05f, 0.85f, lz + 0.05f));
    }
    for (int k = 0; k < 2; k++) {
      uint32_t c = newComp(makePart("Apron rail", "Workbench", joints::screwed));
      float lz = k ? 0.625f : 0.075f;
      b.with(PINE, c).box(o + vec3(0.15f, 0.75f, lz), o + vec3(1.6f, 0.85f, lz + 0.025f));
    }
    uint32_t shelf = newComp(makePart("Lower shelf", "Workbench", joints::screwed));
    b.with(PINE, shelf).box(o + vec3(0.1f, 0.15f, 0.075f), o + vec3(1.65f, 0.175f, 0.65f));
    // Bench vise at the right-hand end: cast-iron jaws on a steel screw.
    uint32_t jaw1 = newComp(makePart("Vise fixed jaw", "Workbench", joints::bolted));
    uint32_t jaw2 = newComp(makePart("Vise moving jaw", "Workbench", joints::pressfit));
    uint32_t screw = newComp(makePart("Vise screw", "Workbench", joints::pressfit));
    b.with(IRON, jaw1).box(o + vec3(1.5f, 0.9f, 0.6f), o + vec3(1.72f, 0.99f, 0.65f));
    b.with(IRON, jaw2).box(o + vec3(1.5f, 0.86f, 0.7f), o + vec3(1.72f, 0.99f, 0.75f));
    b.with(STEEL, screw).cylinder(o + vec3(1.61f, 0.89f, 0.6f), o + vec3(1.61f, 0.89f, 0.795f), 0.0125f);
    b.with(STEEL, screw).cylinder(o + vec3(1.51f, 0.89f, 0.78f), o + vec3(1.71f, 0.89f, 0.78f), 0.0125f);
    spawnBody(s, std::move(v), o);
  }

  // Machines from the assembly catalogue (also available as workshop blueprints).
  auto place = [&](const char* name, const vec3& worldMin) {
    const Assembly* as = findAssembly(name);
    Volume v((int)std::ceil(as->size.x / vs), (int)std::ceil(as->size.y / vs), (int)std::ceil(as->size.z / vs));
    if (!as->build(v, vec3(0))) fprintf(stderr, "%s: wiring failed\n", name);
    return spawnBody(s, std::move(v), worldMin);
  };
  place("Desk lamp", vec3(3.05f, benchTop, 2.4f));
  place("Desk fan", vec3(3.85f, benchTop, 2.35f));
  place("Rover", vec3(9.9f, GY + 0.002f, 5.6f));
  place("Solar lamp", vec3(6.9f, GY + 0.002f, 6.4f));

  // Logs on the campfire, already alight.
  for (int k = 0; k < 2; k++) {
    float a = k ? 0.9f : -0.7f;
    vec3 c = fireP + vec3(0, 0.12f + k * 0.11f, 0);
    vec3 d(std::cos(a) * 0.3f, 0, std::sin(a) * 0.3f);
    vec3 o = c - vec3(0.36f, 0.08f, 0.36f);
    Volume v((int)std::ceil(0.72f / vs), (int)std::ceil(0.16f / vs), (int)std::ceil(0.72f / vs));
    Shaper b(&v, vs, o);
    uint32_t heart = newComp(makePart("Heartwood", "Firewood log", joints::grown));
    uint32_t bk = newComp(makePart("Bark", "Firewood log", joints::grown));
    b.with(OAK, heart).cylinder(c - d, c + d, 0.055f);
    b.with(BARK, bk).tube(c - d * 0.98f, c + d * 0.98f, 0.07f, 0.055f);
    v.forEach([&](int x, int y, int z, Brick*, int) {
      vec3 p = b.centre(x, y, z);
      if (length(vec3(p.x - fireP.x, 0, p.z - fireP.z)) < 0.14f && p.y < c.y && rng().f() < 0.8f) setBurning(v, {x, y, z}, 620);
    });
    spawnBody(s, std::move(v), o);
  }

  // Powder keg: pine staves, iron hoops, a black-powder charge inside.
  {
    vec3 o(5.4f, GY, 8.7f);
    Volume v((int)std::ceil(0.46f / vs), (int)std::ceil(0.56f / vs), (int)std::ceil(0.46f / vs));
    Shaper b(&v, vs, o);
    vec3 c = o + vec3(0.23f, 0, 0.23f);
    const std::string obj = "Powder keg";
    for (int k = 0; k < 12; k++) {
      uint32_t st = newComp(makePart("Stave", obj, joints::pressfit));
      float a0 = k * 2 * PI / 12, a1 = (k + 1) * 2 * PI / 12;
      b.with(PINE, st).boxIf(c - vec3(0.21f, 0, 0.21f), c + vec3(0.21f, 0.53f, 0.21f), [&](const vec3& p) {
        float r = length(vec3(p.x - c.x, 0, p.z - c.z));
        float bulge = 0.185f + 0.02f * std::sin((p.y - o.y) / 0.53f * PI);
        if (r > bulge || r < bulge - 0.03f) return false;
        float a = std::atan2(p.z - c.z, p.x - c.x);
        if (a < 0) a += 2 * PI;
        return a >= a0 && a < a1;
      });
    }
    for (int k = 0; k < 3; k++) {
      uint32_t h = newComp(makePart("Iron hoop", obj, joints::pressfit));
      float y = 0.07f + k * 0.18f;
      float r = 0.185f + 0.02f * std::sin(y / 0.53f * PI);
      b.with(IRON, h).tube(c + vec3(0, y, 0), c + vec3(0, y + 0.04f, 0), r + 0.025f, r - 0.005f);
    }
    uint32_t lid = newComp(makePart("Lid", obj, joints::pressfit));
    b.with(PINE, lid).cylinder(c + vec3(0, 0.5f, 0), c + vec3(0, 0.53f, 0), 0.17f);
    uint32_t bottom = newComp(makePart("Bottom", obj, joints::pressfit));
    b.with(PINE, bottom).cylinder(c, c + vec3(0, 0.03f, 0), 0.18f);
    Component pc = makePart("Black powder", obj, joints::resting, C_CHARGE);
    uint32_t powder = newComp(pc);
    b.with(CHARGE, powder).cylinder(c + vec3(0, 0.03f, 0), c + vec3(0, 0.22f, 0), 0.08f);
    spawnBody(s, std::move(v), o);
  }

  // Crates.
  {
    auto crateT = findPart("Crate")->tpl;
    vec3 ps[3] = {vec3(13.0f, GY, 7.4f), vec3(13.45f, GY, 7.45f), vec3(13.2f, GY + 0.405f, 7.42f)};
    for (auto& p : ps) {
      Volume v(crateT->vol.nx, crateT->vol.ny, crateT->vol.nz);
      // Six nailed boards per crate so they come apart plank by plank.
      Shaper b(&v, vs, p);
      const float cs = 0.4f, th = 2 * vs;
      const char* names[6] = {"Bottom boards", "Lid boards", "Side board", "Side board", "End board", "End board"};
      vec3 lo[6] = {vec3(0), vec3(0, cs - th, 0), vec3(0, th, 0), vec3(cs - th, th, 0), vec3(th, th, 0), vec3(th, th, cs - th)};
      vec3 hi[6] = {vec3(cs, th, cs), vec3(cs, cs, cs), vec3(th, cs - th, cs), vec3(cs, cs - th, cs), vec3(cs - th, cs - th, th), vec3(cs - th, cs - th, cs)};
      for (int f = 0; f < 6; f++) {
        uint32_t c = newComp(makePart(names[f], "Crate", joints::nailed));
        b.with(PINE, c).box(p + lo[f], p + hi[f]);
      }
      spawnBody(s, std::move(v), p);
    }
  }

  info.spawn = vec3(8.0f, GY + 0.02f, 12.8f);
  info.spawnYaw = 0;
  return info;
}
