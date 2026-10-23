#pragma once
// Gas voxels: smoke and steam. They are matter like every other voxel, never
// made from nothing and never lost: a burnt-out log becomes smoke, boiling
// water becomes steam, steam condenses back into water and cool smoke settles
// out as soot. They live in a sparse layer over the world grid, only where
// the world itself is air, so the solid and liquid systems (physics,
// structure, tools, water) see open air there. If anything moves into a gas
// voxel's cell, the gas is pushed aside. The only way out is through the
// open sides or top of the simulated world, which is counted.
#include <memory>
#include <vector>
#include "volume.h"

class Sim;

struct GasBrick {
  uint8_t mat[BS3];
  uint8_t stamp[BS3];   // the step a voxel last moved in (so it moves once per step)
  float temp[BS3];
  uint16_t rows[BS * BS];  // per (z, y) row, a bit per x that holds gas (thin smoke spreads over many bricks)
  int count = 0;
  bool lively = true;      // holds hot gas or steam (stepped every time; cool smoke every 4th)
  GasBrick() { memset(mat, 0, sizeof mat); memset(stamp, 0, sizeof stamp); memset(rows, 0, sizeof rows); for (float& t : temp) t = AMBIENT; }
};

class GasLayer {
 public:
  void reset(const Volume& world);
  uint8_t at(int x, int y, int z) const {
    if ((unsigned)x >= (unsigned)nx_ || (unsigned)y >= (unsigned)ny_ || (unsigned)z >= (unsigned)nz_) return 0;
    const GasBrick* b = bricks[bindex(x, y, z)].get();
    return b ? b->mat[lidx(x, y, z)] : 0;
  }
  float tempAt(int x, int y, int z) const {
    const GasBrick* b = at(x, y, z) ? bricks[bindex(x, y, z)].get() : nullptr;
    return b ? b->temp[lidx(x, y, z)] : AMBIENT;
  }
  // Put one gas voxel at c, or in the nearest free cell (open air with no gas
  // in it). Never loses it: with nowhere free nearby it waits and retries.
  void add(Sim& s, const ivec3& c, uint8_t mat, float temp);
  void step(Sim& s, float dt);
  // Exactly as saved: at c even if something else is there now (it moves on next step).
  void restore(Sim& s, const ivec3& c, uint8_t mat, float temp) { put(s, c.x, c.y, c.z, mat, temp, 0); }
  size_t count() const { return total_ + pending_.size(); }
  size_t countOf(uint8_t mat) const;

  std::vector<std::unique_ptr<GasBrick>> bricks;  // same brick grid as the world
  long escaped = 0;                               // voxels that left through the world's open sides or top
  std::vector<vec3> hot;                          // a sample of glowing gas this step (fire light, sound)
  struct Pending { ivec3 c; uint8_t mat; float temp; };

 private:
  int nx_ = 0, ny_ = 0, nz_ = 0, bx_ = 0, by_ = 0, bz_ = 0;
  size_t total_ = 0;
  uint8_t stepStamp_ = 1;
  std::vector<Pending> pending_;
  // The air's wind and temperature, sampled once per air cell per step.
  struct AirSample { vec3 v; float T; uint32_t step; };
  std::vector<AirSample> airCache_;
  uint32_t airStep_ = 0;
  int bindex(int x, int y, int z) const { return (x >> 4) + bx_ * ((z >> 4) + bz_ * (y >> 4)); }
  bool free(const Volume& w, int x, int y, int z) const { return w.inb(x, y, z) && w.mat(x, y, z) == AIR && !at(x, y, z); }
  void put(Sim& s, int x, int y, int z, uint8_t mat, float temp, uint8_t stamp);
  void take(Sim& s, int x, int y, int z);
  bool findFree(const Volume& w, const ivec3& c, ivec3& out) const;
 public:
  const std::vector<Pending>& pending() const { return pending_; }
  void setPending(std::vector<Pending> p) { pending_ = std::move(p); }
};
