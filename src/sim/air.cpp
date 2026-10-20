#include "air.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include "../core/jobs.h"
#include "materials.h"
#include "volume.h"

namespace {
constexpr float RHO = 1.2f, CP_AIR = 1005.f;
inline float clampi(float v, float hi) { return v < 0 ? 0 : (v > hi ? hi : v); }
}  // namespace

void AirSim::init(const Volume& world, float vs, float cellSize) {
  cellVox = std::max(2, (int)std::lround(cellSize / vs));
  h = cellVox * vs;
  nx = (world.nx + cellVox - 1) / cellVox;
  ny = (world.ny + cellVox - 1) / cellVox;
  nz = (world.nz + cellVox - 1) / cellVox;
  size_t nc = (size_t)nx * ny * nz;
  u.assign((size_t)(nx + 1) * ny * nz, 0);
  v.assign((size_t)nx * (ny + 1) * nz, 0);
  w.assign((size_t)nx * ny * (nz + 1), 0);
  u0 = u; v0 = v; w0 = w;
  T.assign(nc, AMBIENT); T0 = T;
  smk.assign(nc, 0); s0 = smk;
  stm.assign(nc, 0); q0 = stm;
  div.assign(nc, 0); mg.clear();
  solid.assign(nc, 0);
  bodyMask.assign(nc, 0);
  bodyVel.assign(nc, vec3(0));
  heatAcc.assign(nc, 0);
  occCount.assign(nc, 0);
  // Count solid voxels per cell from the allocated bricks.
  world.forEach([&](int x, int y, int z, Brick* br, int l) {
    uint8_t m = br->mat[l];
    if (m == AIR || MATS[m].phase == GAS) return;
    occCount[cid(x / cellVox, y / cellVox, z / cellVox)] += 1;
  });
  // Partly filled cells get the full test (does matter block the air?).
  float full = (float)(cellVox * cellVox * cellVox);
  Jobs::get().parallelFor(ny, 1, [&](int y0, int y1, int) {
    for (int y = y0; y < y1; y++)
      for (int z = 0; z < nz; z++)
        for (int x = 0; x < nx; x++) {
          float o = occCount[cid(x, y, z)];
          if (o >= full) solid[cid(x, y, z)] = 1;
          else if (o > 0) recountCell(world, x, y, z);
        }
  });
  // Start with the breeze everywhere above ground.
  for (int y = 0; y < ny; y++)
    for (int z = 0; z < nz; z++)
      for (int x = 0; x <= nx; x++) u[ui(x, y, z)] = wind.x;
  for (int y = 0; y < ny; y++)
    for (int z = 0; z <= nz; z++)
      for (int x = 0; x < nx; x++) w[wi(x, y, z)] = wind.z;
  texDirty = true;
}

void AirSim::markWorldDirty(int bx, int by, int bz) { dirtyBricks.push_back(bx | (by << 10) | (bz << 20)); }

void AirSim::recountCell(const Volume& world, int cx, int cy, int cz) {
  // A cell blocks the air when matter covers nearly all of it seen along
  // some axis: a thin wall or roof does, a pipe running through it doesn't.
  const int n = cellVox;
  static thread_local std::vector<uint8_t> px, py, pz;
  px.assign((size_t)n * n, 0); py.assign((size_t)n * n, 0); pz.assign((size_t)n * n, 0);
  int count = 0;
  for (int y = 0; y < n; y++)
    for (int z = 0; z < n; z++)
      for (int x = 0; x < n; x++) {
        uint8_t m = world.mat(cx * n + x, cy * n + y, cz * n + z);
        if (m == AIR || MATS[m].phase == GAS || MATS[m].phase == LIQUID) continue;
        count++;
        px[y * n + z] = 1; py[z * n + x] = 1; pz[y * n + x] = 1;
      }
  int bx = 0, by = 0, bz = 0;
  for (int i = 0; i < n * n; i++) { bx += px[i]; by += py[i]; bz += pz[i]; }
  int c = cid(cx, cy, cz);
  occCount[c] = (float)count;
  float cover = (float)std::max(bx, std::max(by, bz)) / (n * n);
  solid[c] = count > n * n * n / 2 || (cover > 0.85f && count >= n * n / 2);
}

// ---------------------------------------------------------------- sampling

float AirSim::sampleU(float x, float y, float z, const std::vector<float>& f) const {
  // u lives at (i, j+.5, k+.5) in cell units.
  y -= 0.5f; z -= 0.5f;
  x = clampi(x, (float)nx); y = clampi(y, (float)(ny - 1)); z = clampi(z, (float)(nz - 1));
  int i = std::min((int)x, nx - 1), j = std::min((int)y, ny - 2 < 0 ? 0 : ny - 2), k = std::min((int)z, nz - 2 < 0 ? 0 : nz - 2);
  float fx = x - i, fy = y - j, fz = z - k;
  int j1 = std::min(j + 1, ny - 1), k1 = std::min(k + 1, nz - 1);
  auto at = [&](int a, int b, int c) { return f[ui(a, b, c)]; };
  float c00 = lerpf(at(i, j, k), at(i + 1, j, k), fx), c10 = lerpf(at(i, j1, k), at(i + 1, j1, k), fx);
  float c01 = lerpf(at(i, j, k1), at(i + 1, j, k1), fx), c11 = lerpf(at(i, j1, k1), at(i + 1, j1, k1), fx);
  return lerpf(lerpf(c00, c10, fy), lerpf(c01, c11, fy), fz);
}
float AirSim::sampleV(float x, float y, float z, const std::vector<float>& f) const {
  x -= 0.5f; z -= 0.5f;
  x = clampi(x, (float)(nx - 1)); y = clampi(y, (float)ny); z = clampi(z, (float)(nz - 1));
  int i = std::min((int)x, std::max(0, nx - 2)), j = std::min((int)y, ny - 1), k = std::min((int)z, std::max(0, nz - 2));
  float fx = x - i, fy = y - j, fz = z - k;
  int i1 = std::min(i + 1, nx - 1), k1 = std::min(k + 1, nz - 1);
  auto at = [&](int a, int b, int c) { return f[vi(a, b, c)]; };
  float c00 = lerpf(at(i, j, k), at(i1, j, k), fx), c10 = lerpf(at(i, j + 1, k), at(i1, j + 1, k), fx);
  float c01 = lerpf(at(i, j, k1), at(i1, j, k1), fx), c11 = lerpf(at(i, j + 1, k1), at(i1, j + 1, k1), fx);
  return lerpf(lerpf(c00, c10, fy), lerpf(c01, c11, fy), fz);
}
float AirSim::sampleW(float x, float y, float z, const std::vector<float>& f) const {
  x -= 0.5f; y -= 0.5f;
  x = clampi(x, (float)(nx - 1)); y = clampi(y, (float)(ny - 1)); z = clampi(z, (float)nz);
  int i = std::min((int)x, std::max(0, nx - 2)), j = std::min((int)y, std::max(0, ny - 2)), k = std::min((int)z, nz - 1);
  float fx = x - i, fy = y - j, fz = z - k;
  int i1 = std::min(i + 1, nx - 1), j1 = std::min(j + 1, ny - 1);
  auto at = [&](int a, int b, int c) { return f[wi(a, b, c)]; };
  float c00 = lerpf(at(i, j, k), at(i1, j, k), fx), c10 = lerpf(at(i, j1, k), at(i1, j1, k), fx);
  float c01 = lerpf(at(i, j, k + 1), at(i1, j, k + 1), fx), c11 = lerpf(at(i, j1, k + 1), at(i1, j1, k + 1), fx);
  return lerpf(lerpf(c00, c10, fy), lerpf(c01, c11, fy), fz);
}
float AirSim::sampleC(float x, float y, float z, const std::vector<float>& f, float outside) const {
  x -= 0.5f; y -= 0.5f; z -= 0.5f;
  if (x < -0.5f || y < -0.5f || z < -0.5f || x > nx - 0.5f || y > ny - 0.5f || z > nz - 0.5f) return outside;
  x = clampi(x, (float)(nx - 1)); y = clampi(y, (float)(ny - 1)); z = clampi(z, (float)(nz - 1));
  int i = std::min((int)x, std::max(0, nx - 2)), j = std::min((int)y, std::max(0, ny - 2)), k = std::min((int)z, std::max(0, nz - 2));
  float fx = x - i, fy = y - j, fz = z - k;
  int i1 = std::min(i + 1, nx - 1), j1 = std::min(j + 1, ny - 1), k1 = std::min(k + 1, nz - 1);
  auto at = [&](int a, int b, int c) { return f[cid(a, b, c)]; };
  float c00 = lerpf(at(i, j, k), at(i1, j, k), fx), c10 = lerpf(at(i, j1, k), at(i1, j1, k), fx);
  float c01 = lerpf(at(i, j, k1), at(i1, j, k1), fx), c11 = lerpf(at(i, j1, k1), at(i1, j1, k1), fx);
  return lerpf(lerpf(c00, c10, fy), lerpf(c01, c11, fy), fz);
}

vec3 AirSim::velocity(const vec3& pw) const {
  if (u.empty()) return vec3(0);
  vec3 g = pw / h;
  return {sampleU(g.x, g.y, g.z, u), sampleV(g.x, g.y, g.z, v), sampleW(g.x, g.y, g.z, w)};
}
float AirSim::temperature(const vec3& pw) const { if (T.empty()) return AMBIENT; vec3 g = pw / h; return sampleC(g.x, g.y, g.z, T, AMBIENT); }
float AirSim::smoke(const vec3& pw) const { if (smk.empty()) return 0; vec3 g = pw / h; return sampleC(g.x, g.y, g.z, smk, 0); }

// ---------------------------------------------------------------- sources

// Sources inside matter (a fire in a stove, a burning log) feed the first
// open cell above them.
int AirSim::openCellAt(const vec3& pw) const {
  int x = (int)(pw.x / h), y = (int)(pw.y / h), z = (int)(pw.z / h);
  if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) return -1;
  for (int k = 0; k < 4 && y + k < ny; k++)
    if (!solid[cid(x, y + k, z)]) return cid(x, y + k, z);
  return cid(x, y, z);
}
void AirSim::addHeat(const vec3& pw, float J) {
  int c = openCellAt(pw);
  if (c >= 0) heatAcc[c] += J;
}
void AirSim::addSmoke(const vec3& pw, float amount, float steam) {
  int c = openCellAt(pw);
  if (c < 0) return;
  smk[c] = std::min(8.f, smk[c] + amount);
  stm[c] = std::min(8.f, stm[c] + steam);
}

void AirSim::addVelocity(const vec3& pw, const vec3& dv, float radius) {
  int r = std::max(1, (int)std::ceil(radius / h));
  int cx = (int)(pw.x / h), cy = (int)(pw.y / h), cz = (int)(pw.z / h);
  for (int y = cy - r; y <= cy + r; y++)
    for (int z = cz - r; z <= cz + r; z++)
      for (int x = cx - r; x <= cx + r; x++) {
        if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) continue;
        vec3 c((x + 0.5f) * h, (y + 0.5f) * h, (z + 0.5f) * h);
        float d = length(c - pw);
        if (d > radius + h * 0.5f) continue;
        float f = 1.f - d / (radius + h);
        u[ui(x, y, z)] += dv.x * f; u[ui(x + 1, y, z)] += dv.x * f;
        v[vi(x, y, z)] += dv.y * f; v[vi(x, y + 1, z)] += dv.y * f;
        w[wi(x, y, z)] += dv.z * f; w[wi(x, y, z + 1)] += dv.z * f;
      }
}

void AirSim::driveTowards(const vec3& pw, const vec3& target, float radius, float k) {
  int r = std::max(1, (int)std::ceil(radius / h));
  int cx = (int)(pw.x / h), cy = (int)(pw.y / h), cz = (int)(pw.z / h);
  for (int y = cy - r; y <= cy + r; y++)
    for (int z = cz - r; z <= cz + r; z++)
      for (int x = cx - r; x <= cx + r; x++) {
        if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz || solid[cid(x, y, z)]) continue;
        vec3 c((x + 0.5f) * h, (y + 0.5f) * h, (z + 0.5f) * h);
        if (length(c - pw) > radius + h * 0.5f) continue;
        auto pull = [&](float& f, float t) { f += (t - f) * k; };
        pull(u[ui(x, y, z)], target.x); pull(u[ui(x + 1, y, z)], target.x);
        pull(v[vi(x, y, z)], target.y); pull(v[vi(x, y + 1, z)], target.y);
        pull(w[wi(x, y, z)], target.z); pull(w[wi(x, y, z + 1)], target.z);
      }
}

void AirSim::blast(const vec3& pw, float E) {
  // A shock of outward air: velocity ~ sqrt(E) falling with distance.
  float R = std::min(6.f, 0.5f * std::cbrt(E / 1000.f));
  int r = (int)std::ceil(R / h);
  int cx = (int)(pw.x / h), cy = (int)(pw.y / h), cz = (int)(pw.z / h);
  float v0s = std::min(80.f, 0.08f * std::sqrt(E));
  for (int y = cy - r; y <= cy + r; y++)
    for (int z = cz - r; z <= cz + r; z++)
      for (int x = cx - r; x <= cx + r; x++) {
        if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) continue;
        vec3 c((x + 0.5f) * h, (y + 0.5f) * h, (z + 0.5f) * h);
        vec3 d = c - pw;
        float dist = std::max(h, length(d));
        if (dist > R) continue;
        vec3 dv = normalize(d) * (v0s / (1 + dist * dist * 4));
        u[ui(x, y, z)] += dv.x; v[vi(x, y, z)] += dv.y; w[wi(x, y, z)] += dv.z;
        int ci = cid(x, y, z);
        T[ci] += std::min(900.f, 0.002f * E / (1 + dist * dist * 20));
        smk[ci] = std::min(8.f, smk[ci] + 2.5f / (1 + dist * dist * 6));
      }
}

void AirSim::setBodyCell(const vec3& pw, const vec3& vel) {
  int x = (int)(pw.x / h), y = (int)(pw.y / h), z = (int)(pw.z / h);
  if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) return;
  int c = cid(x, y, z);
  if (!bodyMask[c]) { bodyMask[c] = 1; bodyCells.push_back(c); bodyVel[c] = vel; }
  else bodyVel[c] = (bodyVel[c] + vel) * 0.5f;
}

// ---------------------------------------------------------------- step

void AirSim::applyBoundaries() {
  // Open sides relax towards the breeze (stronger with height); faces next to
  // solid cells take the solid's velocity (zero, or a moving body's).
  float gx = wind.x * (1 + gust), gz = wind.z * (1 + gust);
  for (int y = 0; y < ny; y++) {
    float prof = clampf((y * h - 1.0f) / 2.5f, 0.15f, 1.f);
    for (int z = 0; z < nz; z++) {
      u[ui(0, y, z)] = lerpf(u[ui(0, y, z)], gx * prof, 0.5f);
      u[ui(nx, y, z)] = lerpf(u[ui(nx, y, z)], gx * prof, 0.5f);
    }
    for (int x = 0; x < nx; x++) {
      w[wi(x, y, 0)] = lerpf(w[wi(x, y, 0)], gz * prof, 0.5f);
      w[wi(x, y, nz)] = lerpf(w[wi(x, y, nz)], gz * prof, 0.5f);
    }
  }
  for (int z = 0; z < nz; z++)
    for (int x = 0; x < nx; x++) v[vi(x, 0, z)] = 0;  // bedrock floor
  Jobs::get().parallelFor(ny, 1, [&](int y0, int y1, int) {
    for (int y = y0; y < y1; y++)
      for (int z = 0; z < nz; z++)
        for (int x = 0; x < nx; x++) {
          int c = cid(x, y, z);
          if (!solid[c] && !bodyMask[c]) continue;
          vec3 bv = bodyMask[c] ? bodyVel[c] : vec3(0);
          u[ui(x, y, z)] = bv.x; u[ui(x + 1, y, z)] = bv.x;
          v[vi(x, y, z)] = bv.y; v[vi(x, y + 1, z)] = bv.y;
          w[wi(x, y, z)] = bv.z; w[wi(x, y, z + 1)] = bv.z;
        }
  });
}

void AirSim::project(float dt) {
  static double pp[6] = {}; static int ppN = 0; auto pt = std::chrono::steady_clock::now();
  auto plap = [&](int i) { auto n = std::chrono::steady_clock::now(); pp[i] += std::chrono::duration<double, std::milli>(n - pt).count(); pt = n; };
  const float ih = 1.f / h;
  auto blocked = [&](int x, int y, int z) {
    if (y < 0) return true;
    if (x < 0 || z < 0 || x >= nx || y >= ny || z >= nz) return false;  // open boundary
    int c = cid(x, y, z);
    return solid[c] != 0 || bodyMask[c] != 0;
  };
  Jobs::get().parallelFor(ny, 1, [&](int y0, int y1, int) {
    for (int y = y0; y < y1; y++)
      for (int z = 0; z < nz; z++)
        for (int x = 0; x < nx; x++) {
          int c = cid(x, y, z);
          div[c] = (u[ui(x + 1, y, z)] - u[ui(x, y, z)] + v[vi(x, y + 1, z)] - v[vi(x, y, z)] + w[wi(x, y, z + 1)] - w[wi(x, y, z)]) * ih;
        }
  });
  // Pressure Poisson equation, solved by geometric multigrid: red-black
  // Gauss-Seidel smoothing on a hierarchy of grids, each half the last, so
  // long-range pressure (a whole room pushing through a doorway) settles in a
  // couple of cycles instead of hundreds of sweeps. Open sides are p = 0.
  if (mg.empty()) {
    int lx = nx, ly = ny, lz = nz;
    while (true) {
      MGLevel L; L.nx = lx; L.ny = ly; L.nz = lz;
      size_t n = (size_t)lx * ly * lz;
      L.p.assign(n, 0); L.b.assign(n, 0); L.r.assign(n, 0); L.mask.assign(n, 0); L.cnt.assign(n, 0); L.fluid.assign(n, 0); L.inv.assign(n, 0.f);
      mg.push_back(std::move(L));
      if (lx < 8 || ly < 4 || lz < 8 || (lx & 1) || (ly & 1) || (lz & 1)) break;
      lx /= 2; ly /= 2; lz /= 2;
    }
  }
  // Fluid flags: finest from the solids, coarser if any child is fluid.
  {
    MGLevel& L = mg[0];
    Jobs::get().parallelFor(ny, 1, [&](int y0, int y1, int) {
      for (size_t c = (size_t)y0 * nx * nz; c < (size_t)y1 * nx * nz; c++) L.fluid[c] = !solid[c] && !bodyMask[c];
    });
    for (size_t l = 1; l < mg.size(); l++) {
      MGLevel &F = mg[l - 1], &C = mg[l];
      std::fill(C.fluid.begin(), C.fluid.end(), 0);
      for (int y = 0; y < F.ny; y++)
        for (int z = 0; z < F.nz; z++)
          for (int x = 0; x < F.nx; x++)
            if (F.fluid[F.id(x, y, z)]) C.fluid[C.id(x / 2, y / 2, z / 2)] = 1;
    }
    for (MGLevel& M : mg) {
      auto build = [&](int y0, int y1, int) {
        for (int y = y0; y < y1; y++)
          for (int z = 0; z < M.nz; z++)
            for (int x = 0; x < M.nx; x++) {
              int c = M.id(x, y, z);
              uint8_t m = 0, n = 0;
              if (M.fluid[c]) {
                const int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (int k = 0; k < 6; k++) {
                  int xx = x + d[k][0], yy = y + d[k][1], zz = z + d[k][2];
                  if (yy < 0) continue;                                                       // bedrock
                  if (xx < 0 || zz < 0 || xx >= M.nx || yy >= M.ny || zz >= M.nz) { n++; continue; }  // open: p = 0
                  if (!M.fluid[M.id(xx, yy, zz)]) continue;                                   // wall
                  n++; m |= (uint8_t)(1 << k);
                }
              }
              M.mask[c] = m; M.cnt[c] = n; M.inv[c] = n ? 1.f / n : 0.f;
            }
      };
      if (&M == &mg[0]) Jobs::get().parallelFor(M.ny, 1, build); else build(0, M.ny, 0);
    }
  }
  plap(0);
  {
    MGLevel& L = mg[0];
    const float scale = h * h / dt;
    for (size_t c = 0; c < L.b.size(); c++) L.b[c] = -div[c] * scale;
  }
  auto smooth = [&](MGLevel& M, int sweeps, bool par) {
    const int sx = 1, sz = M.nx, sy = M.nx * M.nz;
    for (int it = 0; it < sweeps; it++)
      for (int color = 0; color < 2; color++) {
        auto body = [&](int y0, int y1, int) {
          for (int y = y0; y < y1; y++)
            for (int z = 0; z < M.nz; z++)
              for (int x = (y + z + color) & 1; x < M.nx; x += 2) {
                int c = M.id(x, y, z);
                uint8_t n = M.cnt[c];
                if (!n) continue;
                uint8_t m = M.mask[c];
                float sum = 0;
                if (m & 1) sum += M.p[c + sx];
                if (m & 2) sum += M.p[c - sx];
                if (m & 4) sum += M.p[c + sy];
                if (m & 8) sum += M.p[c - sy];
                if (m & 16) sum += M.p[c + sz];
                if (m & 32) sum += M.p[c - sz];
                M.p[c] = (sum + M.b[c]) * M.inv[c];
              }
        };
        if (par) Jobs::get().parallelFor(M.ny, 1, body); else body(0, M.ny, 0);
      }
  };
  auto residual = [&](MGLevel& M, bool par) {
    const int sx = 1, sz = M.nx, sy = M.nx * M.nz;
    auto body = [&](int y0, int y1, int) {
      for (int y = y0; y < y1; y++)
        for (int z = 0; z < M.nz; z++)
          for (int x = 0; x < M.nx; x++) {
            int c = M.id(x, y, z);
            uint8_t n = M.cnt[c];
            if (!n) { M.r[c] = 0; continue; }
            uint8_t m = M.mask[c];
            float sum = 0;
            if (m & 1) sum += M.p[c + sx];
            if (m & 2) sum += M.p[c - sx];
            if (m & 4) sum += M.p[c + sy];
            if (m & 8) sum += M.p[c - sy];
            if (m & 16) sum += M.p[c + sz];
            if (m & 32) sum += M.p[c - sz];
            M.r[c] = M.b[c] - (n * M.p[c] - sum);
          }
    };
    if (par) Jobs::get().parallelFor(M.ny, 1, body); else body(0, M.ny, 0);
  };
  std::function<void(size_t)> vcycle = [&](size_t l) {
    MGLevel& M = mg[l];
    bool par = l == 0;
    if (l + 1 == mg.size()) { smooth(M, 24, false); return; }
    smooth(M, 2, par);
    residual(M, par);
    MGLevel& C = mg[l + 1];
    // Restrict: the coarse operator is (2h)^2 times the Laplacian, the fine
    // one h^2 times it, so the coarse right side is 4x the child average.
    std::fill(C.b.begin(), C.b.end(), 0.f);
    for (int y = 0; y < M.ny; y++)
      for (int z = 0; z < M.nz; z++)
        for (int x = 0; x < M.nx; x++) C.b[C.id(x / 2, y / 2, z / 2)] += 0.5f * M.r[M.id(x, y, z)];
    std::fill(C.p.begin(), C.p.end(), 0.f);
    vcycle(l + 1);
    // Prolong the coarse correction back (trilinear between coarse centres).
    auto body = [&](int y0, int y1, int) {
      for (int y = y0; y < y1; y++)
        for (int z = 0; z < M.nz; z++)
          for (int x = 0; x < M.nx; x++) {
            int c = M.id(x, y, z);
            if (!M.cnt[c]) continue;
            int cx = x / 2, cy = y / 2, cz = z / 2;
            int ox = (x & 1) ? 1 : -1, oy = (y & 1) ? 1 : -1, oz = (z & 1) ? 1 : -1;
            float acc = 0, wsum = 0;
            for (int k = 0; k < 8; k++) {
              int xx = cx + ((k & 1) ? ox : 0), yy = cy + ((k & 2) ? oy : 0), zz = cz + ((k & 4) ? oz : 0);
              float wgt = ((k & 1) ? 0.25f : 0.75f) * ((k & 2) ? 0.25f : 0.75f) * ((k & 4) ? 0.25f : 0.75f);
              float val;
              if (xx < 0 || zz < 0 || xx >= C.nx || yy >= C.ny || zz >= C.nz) val = 0;      // open side
              else if (yy < 0) continue;
              else {
                int cc = C.id(xx, yy, zz);
                if (!C.cnt[cc]) continue;
                val = C.p[cc];
              }
              acc += wgt * val; wsum += wgt;
            }
            if (wsum > 0) M.p[c] += acc / wsum;
          }
    };
    if (par) Jobs::get().parallelFor(M.ny, 1, body); else body(0, M.ny, 0);
    smooth(M, 2, par);
  };
  for (int cyc = 0; cyc < 2; cyc++) vcycle(0);
  plap(1);
  if (getenv("REALISM_DEBUG_AIR")) {
    static int cnt = 0;
    if (++cnt % 60 == 0) {
      residual(mg[0], true);
      double rn = 0, bn = 0;
      for (size_t c = 0; c < mg[0].r.size(); c++) { rn += (double)mg[0].r[c] * mg[0].r[c]; bn += (double)mg[0].b[c] * mg[0].b[c]; }
      fprintf(stderr, "air mg: |r|/|b| = %.4f\n", std::sqrt(rn / std::max(1e-30, bn)));
    }
  }
  std::vector<float>& p = mg[0].p;
  plap(2);
  // Subtract the pressure gradient.
  auto P = [&](int x, int y, int z) { return (x < 0 || z < 0 || x >= nx || y >= ny || z >= nz) ? 0.f : p[cid(x, y, z)]; };
  const float k = dt * ih;
  Jobs::get().parallelFor(ny, 1, [&](int y0, int y1, int) {
    for (int y = y0; y < y1; y++)
      for (int z = 0; z < nz; z++)
        for (int x = 0; x <= nx; x++) {
          bool a = blocked(x - 1, y, z), b = blocked(x, y, z);
          if (a || b) continue;
          u[ui(x, y, z)] -= k * (P(x, y, z) - P(x - 1, y, z));
        }
  });
  Jobs::get().parallelFor(ny + 1, 1, [&](int y0, int y1, int) {
    for (int y = y0; y < y1; y++)
      for (int z = 0; z < nz; z++)
        for (int x = 0; x < nx; x++) {
          if (y == 0) continue;
          bool a = blocked(x, y - 1, z), b = blocked(x, y, z);
          if (a || b) continue;
          v[vi(x, y, z)] -= k * (P(x, y, z) - P(x, y - 1, z));
        }
  });
  Jobs::get().parallelFor(ny, 1, [&](int y0, int y1, int) {
    for (int y = y0; y < y1; y++)
      for (int z = 0; z <= nz; z++)
        for (int x = 0; x < nx; x++) {
          bool a = blocked(x, y, z - 1), b = blocked(x, y, z);
          if (a || b) continue;
          w[wi(x, y, z)] -= k * (P(x, y, z) - P(x, y, z - 1));
        }
  });
  plap(3);
  if (++ppN == 60) { if (getenv("REALISM_DEBUG_AIR")) fprintf(stderr, "proj: setup %.2f solve %.2f dbg %.2f grad %.2f\n", pp[0] / 60, pp[1] / 60, pp[2] / 60, pp[3] / 60); for (double& d : pp) d = 0; ppN = 0; }
}

void AirSim::step(float dt, const Volume& world) {
  auto t0 = std::chrono::steady_clock::now();
  static double prof[8] = {}; static int profN = 0; auto tl = t0;
  auto lap = [&](int i) { auto n = std::chrono::steady_clock::now(); prof[i] += std::chrono::duration<double, std::milli>(n - tl).count(); tl = n; };
  time_ += dt;
  // Gusts: slow noise on the breeze.
  gust = 0.35f * std::sin(time_ * 0.37f) * std::sin(time_ * 0.11f + 1.3f) + 0.15f * std::sin(time_ * 1.7f);
  // Refresh obstacle cells whose bricks changed.
  if (!dirtyBricks.empty()) {
    std::sort(dirtyBricks.begin(), dirtyBricks.end());
    dirtyBricks.erase(std::unique(dirtyBricks.begin(), dirtyBricks.end()), dirtyBricks.end());
    int per = std::max(1, 16 / cellVox);
    for (int key : dirtyBricks) {
      int bx = key & 1023, by = (key >> 10) & 1023, bz = (key >> 20) & 1023;
      int x0 = bx * 16 / cellVox, y0 = by * 16 / cellVox, z0 = bz * 16 / cellVox;
      int x1 = std::min(nx - 1, ((bx + 1) * 16 - 1) / cellVox), y1 = std::min(ny - 1, ((by + 1) * 16 - 1) / cellVox), z1 = std::min(nz - 1, ((bz + 1) * 16 - 1) / cellVox);
      (void)per;
      for (int y = y0; y <= y1; y++)
        for (int z = z0; z <= z1; z++)
          for (int x = x0; x <= x1; x++) recountCell(world, x, y, z);
    }
    dirtyBricks.clear();
  }
  const float Cair = RHO * CP_AIR * h * h * h;
  // Heat delivered by surfaces this step.
  for (size_t c = 0; c < heatAcc.size(); c++) {
    if (heatAcc[c] != 0) { T[c] = std::min(1400.f, std::max(-40.f, T[c] + heatAcc[c] / Cair)); heatAcc[c] = 0; }
  }
  // Buoyancy on vertical faces (Boussinesq).
  Jobs::get().parallelFor(ny - 1, 1, [&](int y0, int y1, int) {
    for (int yy = y0; yy < y1; yy++) {
      int y = yy + 1;
      for (int z = 0; z < nz; z++)
        for (int x = 0; x < nx; x++) {
          int a = cid(x, y - 1, z), b = cid(x, y, z);
          float Tm = 0.5f * (T[a] + T[b]);
          v[vi(x, y, z)] += dt * 9.81f * (Tm - AMBIENT) / (AMBIENT + 273.15f);
        }
    }
  });
  applyBoundaries();
  lap(0);
  // Advect velocity (semi-Lagrangian). The other two components at each face
  // come from averaging the four surrounding faces, so each face needs just one
  // interpolated lookup.
  u0 = u; v0 = v; w0 = w;
  const float idt = dt / h;
  auto U0 = [&](int x, int y, int z) { return u0[ui(std::clamp(x, 0, nx), std::clamp(y, 0, ny - 1), std::clamp(z, 0, nz - 1))]; };
  auto V0 = [&](int x, int y, int z) { return v0[vi(std::clamp(x, 0, nx - 1), std::clamp(y, 0, ny), std::clamp(z, 0, nz - 1))]; };
  auto W0 = [&](int x, int y, int z) { return w0[wi(std::clamp(x, 0, nx - 1), std::clamp(y, 0, ny - 1), std::clamp(z, 0, nz))]; };
  Jobs::get().parallelFor(ny, 1, [&](int y0, int y1, int) {
    for (int y = y0; y < y1; y++)
      for (int z = 0; z < nz; z++) {
        for (int x = 0; x <= nx; x++) {
          float uu = u0[ui(x, y, z)];
          float vv = 0.25f * (V0(x - 1, y, z) + V0(x, y, z) + V0(x - 1, y + 1, z) + V0(x, y + 1, z));
          float ww = 0.25f * (W0(x - 1, y, z) + W0(x, y, z) + W0(x - 1, y, z + 1) + W0(x, y, z + 1));
          u[ui(x, y, z)] = sampleU(x - uu * idt, y + 0.5f - vv * idt, z + 0.5f - ww * idt, u0);
        }
        for (int x = 0; x < nx; x++) {
          int zmax = (z == nz - 1) ? nz : z;
          for (int zz = z; zz <= zmax; zz++) {
            float ww = w0[wi(x, y, zz)];
            float uu = 0.25f * (U0(x, y, zz - 1) + U0(x + 1, y, zz - 1) + U0(x, y, zz) + U0(x + 1, y, zz));
            float vv = 0.25f * (V0(x, y, zz - 1) + V0(x, y + 1, zz - 1) + V0(x, y, zz) + V0(x, y + 1, zz));
            w[wi(x, y, zz)] = sampleW(x + 0.5f - uu * idt, y + 0.5f - vv * idt, zz - ww * idt, w0);
          }
        }
      }
  });
  Jobs::get().parallelFor(ny + 1, 1, [&](int y0, int y1, int) {
    for (int y = y0; y < y1; y++)
      for (int z = 0; z < nz; z++)
        for (int x = 0; x < nx; x++) {
          float vv = v0[vi(x, y, z)];
          float uu = 0.25f * (U0(x, y - 1, z) + U0(x + 1, y - 1, z) + U0(x, y, z) + U0(x + 1, y, z));
          float ww = 0.25f * (W0(x, y - 1, z) + W0(x, y - 1, z + 1) + W0(x, y, z) + W0(x, y, z + 1));
          v[vi(x, y, z)] = sampleV(x + 0.5f - uu * idt, y - vv * idt, z + 0.5f - ww * idt, v0);
        }
  });
  lap(1);
  // Vorticity confinement keeps plumes curling instead of smearing out.
  {
    om.resize((size_t)nx * ny * nz); omL.resize(om.size());
    auto cv = [&](int x, int y, int z) {
      return vec3(0.5f * (u[ui(x, y, z)] + u[ui(x + 1, y, z)]), 0.5f * (v[vi(x, y, z)] + v[vi(x, y + 1, z)]), 0.5f * (w[wi(x, y, z)] + w[wi(x, y, z + 1)]));
    };
    Jobs::get().parallelFor(ny, 1, [&](int y0, int y1, int) {
      for (int y = y0; y < y1; y++)
        for (int z = 0; z < nz; z++)
          for (int x = 0; x < nx; x++) {
            if (x == 0 || y == 0 || z == 0 || x == nx - 1 || y == ny - 1 || z == nz - 1) { om[cid(x, y, z)] = vec3(0); omL[cid(x, y, z)] = 0; continue; }
            vec3 dx = (cv(x + 1, y, z) - cv(x - 1, y, z)) * (0.5f / h), dy = (cv(x, y + 1, z) - cv(x, y - 1, z)) * (0.5f / h),
                 dz = (cv(x, y, z + 1) - cv(x, y, z - 1)) * (0.5f / h);
            vec3 o(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x);
            om[cid(x, y, z)] = o; omL[cid(x, y, z)] = length(o);
          }
    });
    const float eps = 0.35f;
    Jobs::get().parallelFor(ny, 1, [&](int y0, int y1, int) {
      for (int y = y0; y < y1; y++)
        for (int z = 1; z < nz - 1; z++)
          for (int x = 1; x < nx - 1; x++) {
            if (y == 0 || y == ny - 1) continue;
            int c = cid(x, y, z);
            if (solid[c]) continue;
            if (omL[c] < 0.05f) continue;  // calm air: nothing to confine
            vec3 g(omL[c + 1] - omL[c - 1], omL[c + nx * nz] - omL[c - nx * nz], omL[c + nx] - omL[c - nx]);
            float gl = length(g);
            if (gl < 1e-5f) continue;
            vec3 f = cross(g / gl, om[c]) * (eps * h);
            u[ui(x, y, z)] += f.x * dt * 0.5f; u[ui(x + 1, y, z)] += f.x * dt * 0.5f;
            v[vi(x, y, z)] += f.y * dt * 0.5f; v[vi(x, y + 1, z)] += f.y * dt * 0.5f;
            w[wi(x, y, z)] += f.z * dt * 0.5f; w[wi(x, y, z + 1)] += f.z * dt * 0.5f;
          }
    });
  }
  applyBoundaries();
  lap(2);
  project(dt);
  applyBoundaries();
  lap(3);
  // Advect scalars with the divergence-free velocity; cool and clear slowly.
  u0 = u; v0 = v; w0 = w;
  T0 = T; s0 = smk; q0 = stm;
  int active = 0;
  std::vector<int> act(ny, 0);
  Jobs::get().parallelFor(ny, 1, [&](int y0, int y1, int) {
    for (int y = y0; y < y1; y++)
      for (int z = 0; z < nz; z++)
        for (int x = 0; x < nx; x++) {
          int c = cid(x, y, z);
          if (solid[c]) { T[c] = T0[c]; smk[c] = 0; stm[c] = 0; continue; }
          if (T0[c] == AMBIENT && s0[c] == 0 && q0[c] == 0) {
            // Quiet cell: only changes if something non-ambient is upwind.
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; dy++)
              for (int dz = -1; dz <= 1 && !near; dz++)
                for (int dx = -1; dx <= 1 && !near; dx++) {
                  int xx = x + dx, yy = y + dy, zz = z + dz;
                  if (xx < 0 || yy < 0 || zz < 0 || xx >= nx || yy >= ny || zz >= nz) continue;
                  int n2 = cid(xx, yy, zz);
                  if (T0[n2] != AMBIENT || s0[n2] != 0 || q0[n2] != 0) near = true;
                }
            if (!near) continue;
          }
          vec3 cvel(0.5f * (u0[ui(x, y, z)] + u0[ui(x + 1, y, z)]), 0.5f * (v0[vi(x, y, z)] + v0[vi(x, y + 1, z)]),
                    0.5f * (w0[wi(x, y, z)] + w0[wi(x, y, z + 1)]));
          vec3 b = vec3(x + 0.5f, y + 0.5f, z + 0.5f) - cvel * idt;
          bool edge = x == 0 || z == 0 || x == nx - 1 || z == nz - 1 || y == ny - 1;
          float t = sampleC(b.x, b.y, b.z, T0, AMBIENT);
          float s = sampleC(b.x, b.y, b.z, s0, 0);
          float q = sampleC(b.x, b.y, b.z, q0, 0);
          // Mixing with surrounding air: hot air cools by entrainment/radiation.
          t += (AMBIENT - t) * std::min(1.f, dt * (0.03f + 0.0015f * std::fabs(t - AMBIENT)));
          s *= (1 - std::min(1.f, dt * 0.06f));
          q *= (1 - std::min(1.f, dt * 0.45f));
          if (edge) { t = lerpf(t, AMBIENT, 0.3f); s *= 0.7f; q *= 0.7f; }
          if (std::fabs(t - AMBIENT) < 0.05f) t = AMBIENT;
          T[c] = t; smk[c] = s < 1e-4f ? 0 : s; stm[c] = q < 1e-4f ? 0 : q;
          if (s > 0.01f || q > 0.01f || std::fabs(t - AMBIENT) > 1) act[y]++;
        }
  });
  lap(4);
  if (++profN == 60) {
    if (getenv("REALISM_DEBUG_AIR")) fprintf(stderr, "air prof: pre %.2f adv %.2f vort %.2f proj %.2f scal %.2f\n", prof[0] / 60, prof[1] / 60, prof[2] / 60, prof[3] / 60, prof[4] / 60);
    for (double& d : prof) d = 0;
    profN = 0;
  }
  for (int a : act) active += a;
  activeCells = active;
  // Moving bodies only block for this step.
  for (int c : bodyCells) bodyMask[c] = 0;
  bodyCells.clear();
  texDirty = true;
  stepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
