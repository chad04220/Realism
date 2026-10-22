#pragma once
// The simulation: a static world volume, free rigid bodies, loose debris and
// visual effects, plus the systems that move energy between them: impacts,
// heat conduction, radiation, fire, melting, fluids, circuits and explosions.
#include <memory>
#include <unordered_set>
#include <vector>
#include "body.h"
#include "comps.h"
#include "volume.h"
#include "air.h"

namespace save { struct Access; }

struct Debris {          // a single loose voxel in flight
  vec3 p, v;
  uint8_t mat;
  float temp;
  float life;
  uint32_t comp;
  uint8_t bounces;
  uint8_t aux = 0;       // molten: what it was
  float fill = 1;        // water: how much of a cell it carries
};

enum FxKind : uint8_t { FX_FIRE, FX_SMOKE, FX_SPARK, FX_STEAM, FX_DUST, FX_FLASH, FX_SPRAY, FX_EMBER };
struct Fx {
  vec3 p, v;
  float life, maxLife, size;
  vec3 color;
  FxKind kind;
};

enum EventKind : uint8_t { EV_IMPACT, EV_BREAK, EV_DETACH, EV_EXPLODE, EV_IGNITE, EV_SHATTER, EV_TOGGLE, EV_SHORT, EV_MELT, EV_SPLASH };
struct SimEvent {
  EventKind kind;
  vec3 p;
  float energy;
  uint8_t mat;
  uint32_t comp;
};

struct Radiator {
  vec3 p, n;      // world position and outward normal
  float W = 0;    // radiated power, watts
  int body = -1;  // owning body id, -1 = world
};

struct Hit {
  bool hit = false;
  int body = -1;          // index into bodies, -1 = world
  ivec3 cell;             // voxel in that volume
  ivec3 face;             // face normal in that volume's grid
  vec3 point, normal;     // world space
  float t = 0;
};

struct SimSettings {
  float voxelSize = 0.025f;
  vec3 extent{16, 8, 16};   // metres
  float thermalWarp = 20;   // thermal time runs faster so fires and furnaces play out in seconds
  int threads = 0;
};

class Sim {
 public:
  SimSettings cfg;
  float vs = 0.025f;
  Volume world;
  AirSim air;
  bool airReady = false;
  void initAir();
  HotSet worldHot;
  std::vector<std::unique_ptr<Body>> bodies;
  std::vector<Debris> debris;
  std::vector<Fx> fx;
  std::vector<SimEvent> events;
  std::unordered_set<int64_t> fluidActive;
  std::vector<int64_t> supportSeeds;
  std::unordered_set<int64_t> worldSpecials;   // electrical parts placed in the world
  int nextBodyId = 1;
  float time = 0;
  float timeScale = 1;
  vec3 gravity{0, -9.81f, 0};
  vec3 sunDir = normalize(vec3(0.45f, 0.8f, 0.35f));
  vec3 wind{0.4f, 0, 0.15f};
  // Live statistics for the HUD.
  struct {
    int burning = 0, hot = 0, awake = 0, contacts = 0, poweredDevices = 0, circuits = 0;
    float thermalMs = 0, physicsMs = 0, fluidMs = 0, electricMs = 0;
    double heatJ = 0;
  } stats;

  void init(const SimSettings& s);
  void step(float dt);

  // World editing (keeps fluid, support, specials and heat bookkeeping right).
  void setWorld(int x, int y, int z, uint8_t m, uint32_t comp = 0);
  void putWorld(int x, int y, int z, const Cell& c);
  void removeWorld(int x, int y, int z, bool debris, const vec3& kick = vec3(0));
  void worldChanged(int x, int y, int z, uint8_t oldMat);

  // Bodies.
  Body* newBody();
  Body* bodyById(int id);
  int bodyIndex(const Body* b) const;
  void rebuildBody(Body& b);           // mass properties, collision samples, devices
  void splitBody(Body& b);             // separate disconnected pieces
  void removeBody(int index);
  // Lift a set of world voxels out into a new rigid body.
  Body* detachWorld(const std::vector<int64_t>& cells, const vec3& kick = vec3(0));
  // Take every voxel of component `comp` out of a body (or, for bodyIndex -1,
  // out of the world starting from voxel `seed`) into its own body.
  Body* detachComponent(int bodyIndex, uint32_t comp, const vec3& kick, const ivec3& seed = ivec3());
  void settleBody(int index);          // freeze a resting fragment back into the world
  void wakeNear(const vec3& p, float radius);
  void removeBodyVoxel(Body& b, int x, int y, int z, bool debris);

  Hit raycast(const vec3& o, const vec3& d, float maxDist, int ignoreBody = -1) const;
  float radiantFlux(const vec3& p) const;   // W/m^2 of thermal radiation arriving here
  std::vector<Radiator> radiators;          // this thermal tick's hot surfaces, strongest first
  float radiantTotal = 0;

  // Damage and energy delivery.
  void impact(const Hit& h, float energy, const vec3& dir, bool canDetach = true);
  void explode(const vec3& p, float energyJ, int sourceBody = -1);
  void fracture(Volume& v, const vec3& centerVoxel, float radiusVoxels, int seeds, Body* owner);
  void addHeat(const Hit& h, float watts, float dt, float radiusVoxels);
  void spray(const vec3& p, const vec3& v, uint8_t mat, int count);

  // Effects.
  void emit(FxKind k, const vec3& p, const vec3& v, float life, float size, const vec3& color = vec3(1));
  void spawnDebris(const vec3& p, const vec3& v, const Cell& c);
  void event(EventKind k, const vec3& p, float e, uint8_t m = 0, uint32_t comp = 0) { if (events.size() < 512) events.push_back({k, p, e, m, comp}); }

  // Helpers.
  vec3 cellCenter(int x, int y, int z) const { return vec3((x + 0.5f) * vs, (y + 0.5f) * vs, (z + 0.5f) * vs); }
  ivec3 cellOf(const vec3& p) const { return floorI(p / vs); }
  void markHot(Volume& v, HotSet& hs, int x, int y, int z) { if (v.inb(x, y, z)) hs.add(v.bindex(x, y, z)); }

  // Systems (separate translation units).
  void stepPhysics(float dt);
  void stepThermal(float dtThermal);
  void radiate(float dtThermal);
  void stepFluids(float tick);
  void stepWater(std::vector<int64_t>& list, float tick);
  void settleWater();
  vec3 waterCurrent(const vec3& p) const;   // mean water flow here, m/s
  std::vector<vec3> waterFlow;              // per air cell
  std::vector<float> waterFlowN;
  void stepElectric(float dt);
  void stepDevices(float dt);
  void stepDebris(float dt);
  void stepFx(float dt);
  void processSupport();

 private:
  friend struct save::Access;
  float thermalAcc_ = 0, fluidAcc_ = 0, electricAcc_ = 0, airAcc_ = 0;
  void airCoupling(float dt);
  // Visited set for the support flood: one bit per world cell. The flood is
  // single-threaded and only needs to know which cells this particular walk
  // has already queued, so a byte or word per cell would be wasted memory
  // (16 MB at the default world size instead of 512 MB).
  std::vector<uint64_t> supportVisit_;
};

extern Sim* g_sim;
