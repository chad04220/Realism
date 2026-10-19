#pragma once
// Sparse bricked voxel storage shared by the world, every rigid body and the
// workshop bench. Only 16^3 bricks that have ever held matter are allocated,
// so the open air above the ground costs nothing.
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "../core/math.h"
#include "materials.h"

constexpr int BS = 16, BS3 = BS * BS * BS;

enum CellFlag : uint8_t { F_BURN = 1, F_LIT = 2, F_POWER = 4, F_ARMED = 8, F_SPARK = 16, F_WVEL = 32 };  // F_WVEL: water velocity in hp (x) and aux (z)

struct Brick {
  uint8_t mat[BS3];
  uint8_t flags[BS3];
  uint8_t hp[BS3];       // 255 = intact
  uint8_t aux[BS3];      // molten: what it was
  uint32_t comp[BS3];
  float temp[BS3];
  float fuel[BS3];
  int count = 0;
  Brick() {
    memset(mat, 0, sizeof mat); memset(flags, 0, sizeof flags); memset(hp, 255, sizeof hp);
    memset(aux, 0, sizeof aux); memset(comp, 0, sizeof comp); memset(fuel, 0, sizeof fuel);
    for (float& t : temp) t = AMBIENT;
  }
};

inline int lidx(int x, int y, int z) { return (x & 15) | ((z & 15) << 4) | ((y & 15) << 8); }

struct Cell {
  uint8_t mat = 0, flags = 0, hp = 255, aux = 0;
  uint32_t comp = 0;
  float temp = AMBIENT, fuel = 0;
};

class Volume {
 public:
  int nx = 0, ny = 0, nz = 0, bx = 0, by = 0, bz = 0;
  bool solidBelow = false;  // the world treats y < 0 as bedrock
  std::vector<std::unique_ptr<Brick>> bricks;
  std::vector<uint8_t> dirty;      // per brick: needs GPU upload
  std::vector<int> dirtyList;
  std::vector<uint8_t> shapeFlag;  // per brick: matter added/removed (for the air grid)
  std::vector<int> shapeList;
  uint32_t version = 0;            // bumps on every structural change

  Volume() = default;
  Volume(int x, int y, int z) { resize(x, y, z); }
  void resize(int x, int y, int z) {
    nx = x; ny = y; nz = z;
    bx = (x + BS - 1) / BS; by = (y + BS - 1) / BS; bz = (z + BS - 1) / BS;
    bricks.clear(); bricks.resize((size_t)bx * by * bz);
    dirty.assign(bricks.size(), 0); dirtyList.clear();
    shapeFlag.assign(bricks.size(), 0); shapeList.clear();
  }
  void markShape(int x, int y, int z) {
    int b = bindex(x, y, z);
    if (!shapeFlag[b]) { shapeFlag[b] = 1; shapeList.push_back(b); }
  }
  bool inb(int x, int y, int z) const { return (unsigned)x < (unsigned)nx && (unsigned)y < (unsigned)ny && (unsigned)z < (unsigned)nz; }
  int bindex(int x, int y, int z) const { return (x >> 4) + bx * ((z >> 4) + bz * (y >> 4)); }
  Brick* brick(int x, int y, int z) const { return bricks[bindex(x, y, z)].get(); }
  Brick* brickAt(int b) const { return bricks[b].get(); }
  Brick& brickW(int x, int y, int z) {
    auto& p = bricks[bindex(x, y, z)];
    if (!p) p = std::make_unique<Brick>();
    return *p;
  }
  uint8_t mat(int x, int y, int z) const {
    if (!inb(x, y, z)) return (solidBelow && y < 0) ? BEDROCK : AIR;
    const Brick* b = brick(x, y, z);
    return b ? b->mat[lidx(x, y, z)] : AIR;
  }
  uint8_t matI(const ivec3& p) const { return mat(p.x, p.y, p.z); }
  void markDirty(int x, int y, int z) {
    int b = bindex(x, y, z);
    if (!dirty[b]) { dirty[b] = 1; dirtyList.push_back(b); }
    // Neighbour bricks sample across the border for smoothing and AO.
    int lx = x & 15, ly = y & 15, lz = z & 15;
    auto mark = [&](int xx, int yy, int zz) { if (inb(xx, yy, zz)) { int c = bindex(xx, yy, zz); if (!dirty[c]) { dirty[c] = 1; dirtyList.push_back(c); } } };
    if (lx == 0) mark(x - 1, y, z); else if (lx == 15) mark(x + 1, y, z);
    if (ly == 0) mark(x, y - 1, z); else if (ly == 15) mark(x, y + 1, z);
    if (lz == 0) mark(x, y, z - 1); else if (lz == 15) mark(x, y, z + 1);
  }
  void markBrickDirty(int b) { if (!dirty[b]) { dirty[b] = 1; dirtyList.push_back(b); } }
  void markAllDirty() { for (size_t b = 0; b < bricks.size(); b++) markBrickDirty((int)b); }

  void set(int x, int y, int z, uint8_t m, uint32_t comp = 0) {
    if (!inb(x, y, z)) return;
    if (m == AIR) {
      Brick* b = brick(x, y, z);
      if (!b) return;
      int l = lidx(x, y, z);
      if (b->mat[l]) b->count--;
      b->mat[l] = 0; b->flags[l] = 0; b->hp[l] = 255; b->aux[l] = 0; b->comp[l] = 0; b->temp[l] = AMBIENT; b->fuel[l] = 0;
    } else {
      Brick& b = brickW(x, y, z);
      int l = lidx(x, y, z);
      if (!b.mat[l]) b.count++;
      b.mat[l] = m; b.flags[l] = 0; b.hp[l] = 255; b.aux[l] = 0; b.comp[l] = comp; b.temp[l] = AMBIENT; b.fuel[l] = 0;
    }
    markDirty(x, y, z);
    markShape(x, y, z);
    version++;
  }
  Cell get(int x, int y, int z) const {
    Cell c;
    if (!inb(x, y, z)) { c.mat = (solidBelow && y < 0) ? BEDROCK : AIR; return c; }
    const Brick* b = brick(x, y, z);
    if (!b) return c;
    int l = lidx(x, y, z);
    c.mat = b->mat[l]; c.flags = b->flags[l]; c.hp = b->hp[l]; c.aux = b->aux[l];
    c.comp = b->comp[l]; c.temp = b->temp[l]; c.fuel = b->fuel[l];
    return c;
  }
  void put(int x, int y, int z, const Cell& c) {
    if (!inb(x, y, z)) return;
    if (c.mat == AIR) { set(x, y, z, AIR); return; }
    Brick& b = brickW(x, y, z);
    int l = lidx(x, y, z);
    if (!b.mat[l]) b.count++;
    b.mat[l] = c.mat; b.flags[l] = c.flags; b.hp[l] = c.hp; b.aux[l] = c.aux;
    b.comp[l] = c.comp; b.temp[l] = c.temp; b.fuel[l] = c.fuel;
    markDirty(x, y, z);
    markShape(x, y, z);
    version++;
  }
  // Field accessors for simulation code. Callers must know the cell exists.
  float& temp(int x, int y, int z) { return brick(x, y, z)->temp[lidx(x, y, z)]; }
  uint8_t& flags(int x, int y, int z) { return brick(x, y, z)->flags[lidx(x, y, z)]; }
  uint32_t comp(int x, int y, int z) const { const Brick* b = inb(x, y, z) ? brick(x, y, z) : nullptr; return b ? b->comp[lidx(x, y, z)] : 0; }
  float tempAt(int x, int y, int z) const { const Brick* b = inb(x, y, z) ? brick(x, y, z) : nullptr; return b ? b->temp[lidx(x, y, z)] : AMBIENT; }

  template <class F> void forEach(F&& f) const {  // f(x,y,z,brick,l) for every non-air cell
    for (int by_ = 0; by_ < by; by_++)
      for (int bz_ = 0; bz_ < bz; bz_++)
        for (int bx_ = 0; bx_ < bx; bx_++) {
          Brick* b = bricks[bx_ + bx * (bz_ + bz * by_)].get();
          if (!b || !b->count) continue;
          for (int l = 0; l < BS3; l++) {
            if (!b->mat[l]) continue;
            int x = bx_ * 16 + (l & 15), z = bz_ * 16 + ((l >> 4) & 15), y = by_ * 16 + (l >> 8);
            f(x, y, z, b, l);
          }
        }
  }
  size_t countSolid() const { size_t n = 0; for (auto& b : bricks) if (b) n += b->count; return n; }
  size_t memoryBytes() const { size_t n = 0; for (auto& b : bricks) if (b) n += sizeof(Brick); return n; }
};

struct HotSet {           // bricks with thermal activity
  std::vector<uint8_t> on;
  std::vector<int> list;
  void reset(size_t n) { on.assign(n, 0); list.clear(); }
  void add(int b) { if (b >= 0 && b < (int)on.size() && !on[b]) { on[b] = 1; list.push_back(b); } }
};

// Pack/unpack world coordinates into one int key for sets and queues.
inline int64_t packKey(int x, int y, int z) {
  return ((int64_t)(y + 524288) << 40) | ((int64_t)(z + 524288) << 20) | (int64_t)(x + 524288);
}
inline ivec3 unpackKey(int64_t k) {
  return {(int)(k & 0xFFFFF) - 524288, (int)((k >> 40) & 0xFFFFF) - 524288, (int)((k >> 20) & 0xFFFFF) - 524288};
}

constexpr int DIRS6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

// Glow byte sent to the GPU: bits 0-5 incandescence, bit 6 burning, bit 7 lit filament.
inline uint8_t glowByte(float t, uint8_t flags) {
  uint8_t g = 0;
  if (t > 430) g = (uint8_t)std::min(63.f, (t - 430.f) / 25.f + 1.f);
  if (flags & F_BURN) g |= 64;
  if (flags & (F_LIT | F_ARMED | F_SPARK)) g |= 128;
  return g;
}
