#pragma once
// Shape rasteriser used by the scene, the part templates and the workshop.
// Everything is specified in metres, so content is independent of voxel size.
#include <functional>
#include "../sim/sim.h"

struct Shaper {
  Volume* v = nullptr;
  float vs = 0.025f;
  vec3 origin;          // world position of the volume's (0,0,0) corner
  uint8_t mat = STONE;
  uint32_t comp = 0;
  bool overwrite = true;
  std::function<bool(const vec3&)> mask;  // optional extra test on cell centres (metres)

  Shaper(Volume* vol, float voxel, vec3 o = vec3(0)) : v(vol), vs(voxel), origin(o) {}
  Shaper& with(uint8_t m, uint32_t c) { mat = m; comp = c; return *this; }

  ivec3 toCell(const vec3& p) const { return floorI((p - origin) / vs); }
  vec3 centre(int x, int y, int z) const { return origin + vec3((x + 0.5f) * vs, (y + 0.5f) * vs, (z + 0.5f) * vs); }
  void put(int x, int y, int z);
  // Fill cells whose centres lie inside the shape.
  void box(const vec3& lo, const vec3& hi);
  void boxIf(const vec3& lo, const vec3& hi, const std::function<bool(const vec3&)>& f);
  void sphere(const vec3& c, float r);
  void ellipsoid(const vec3& c, const vec3& r);
  void cylinder(const vec3& a, const vec3& b, float r);       // capsule-free cylinder between a and b
  void tube(const vec3& a, const vec3& b, float rOut, float rIn);
  void line(const vec3& a, const vec3& b);                    // 1-voxel-thick wire
  void clear(const vec3& lo, const vec3& hi);                 // carve to air
};

// Lay a copper wire through empty cells from one conductor to another without
// touching any other conductor or electrical part on the way (no accidental
// shorts). Returns false if no route exists.
bool routeWire(Volume& v, const ivec3& from, const ivec3& to, uint32_t comp, uint8_t mat = COPPER);

// Make a free rigid body from a volume whose (0,0,0) corner sits at worldMin.
Body* spawnBody(Sim& s, Volume&& vol, const vec3& worldMin);

// A reusable part: voxels plus the component record they belong to.
struct Template {
  std::string name;
  std::string category;
  Volume vol;
  Component proto;
  std::vector<std::pair<ivec3, uint32_t>> extraComps;  // sub-parts (e.g. a switch inside a lamp)
};
