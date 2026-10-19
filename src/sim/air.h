#pragma once
// The air: an incompressible-flow simulation (stable fluids on a staggered
// grid) over the whole world at ~20 cm resolution. It carries velocity,
// temperature, smoke and steam. Hot air rises (Boussinesq buoyancy), flows
// around walls and through doorways and chimneys, is stirred by fans,
// moving bodies and explosions, and is pushed by a gusty breeze at the open
// boundaries. Surfaces exchange heat with the air touching them, so a fire
// heats the air above it and that hot plume heats whatever it reaches.
#include <cstdint>
#include <vector>
#include "../core/math.h"

class Volume;

class AirSim {
 public:
  void init(const Volume& world, float voxelSize, float cellSize);
  void step(float dt, const Volume& world);

  // Queries (world space, metres).
  vec3 velocity(const vec3& p) const;
  float temperature(const vec3& p) const;
  float smoke(const vec3& p) const;
  // Sources.
  void addHeat(const vec3& p, float joules);
  void addHeatCell(int c, float joules) { heatAcc[c] += joules; }
  void driveTowards(const vec3& p, const vec3& target, float radius, float k);  // fans, jets
  void addSmoke(const vec3& p, float amount, float steam = 0);
  void addVelocity(const vec3& p, const vec3& dv, float radius);
  void blast(const vec3& p, float energyJ);
  void setBodyCell(const vec3& p, const vec3& vel);   // a moving body occupies this point this step
  void markWorldDirty(int bx, int by, int bz);        // a world brick changed shape

  // Grid access for rendering.
  int nx = 0, ny = 0, nz = 0;
  float h = 0.2f;                 // cell size, metres
  int cellVox = 8;                // voxels per cell edge
  std::vector<float> T, smk, stm; // cell-centred temperature (C), smoke, steam
  std::vector<uint8_t> solid;     // 1 = blocked by matter
  bool texDirty = true;
  vec3 wind{0.9f, 0, 0.35f};      // breeze at the open boundaries, m/s
  float gust = 0;
  float stepMs = 0;
  int activeCells = 0;

  int cid(int x, int y, int z) const { return x + nx * (z + nz * y); }

 private:
  std::vector<float> u, v, w;     // staggered face velocities
  std::vector<float> u0, v0, w0, T0, s0, q0, div;
  struct MGLevel {
    int nx, ny, nz;
    std::vector<float> p, b, r, inv;
    std::vector<uint8_t> mask, cnt, fluid;
    int id(int x, int y, int z) const { return x + nx * (z + nz * y); }
  };
  std::vector<vec3> om; std::vector<float> omL;
  std::vector<MGLevel> mg;        // pressure multigrid, finest first
  std::vector<uint8_t> bodyMask;
  std::vector<vec3> bodyVel;
  std::vector<int> bodyCells;
  std::vector<float> heatAcc;     // joules queued for each cell this step
  std::vector<float> occCount;    // solid voxels per cell
  std::vector<int> dirtyBricks;
  float time_ = 0;
  int ui(int x, int y, int z) const { return x + (nx + 1) * (z + nz * y); }
  int vi(int x, int y, int z) const { return x + nx * (z + nz * y); }
  int wi(int x, int y, int z) const { return x + nx * (z + (nz + 1) * y); }
  float sampleU(float x, float y, float z, const std::vector<float>& f) const;
  float sampleV(float x, float y, float z, const std::vector<float>& f) const;
  float sampleW(float x, float y, float z, const std::vector<float>& f) const;
  float sampleC(float x, float y, float z, const std::vector<float>& f, float outside) const;
  void recountCell(const Volume& world, int x, int y, int z);
  void applyBoundaries();
  void project(float dt);
};
