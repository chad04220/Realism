#pragma once
#include <cstdint>
#include "../core/math.h"

// Physical material library. Properties are real-world SI values; per-voxel
// mass, heat capacity and strength are derived from the voxel size at start-up.
enum Mat : uint8_t {
  AIR, BEDROCK, STONE, DIRT, GRASS, SAND, WATER, OAK, PINE, LEAVES, BRICK, MORTAR, CONCRETE, STEEL, IRON,
  COPPER, ALU, GLASS, BULBGLASS, RUBBER, PLASTIC, LIION, TERM_POS, TERM_NEG, FILAMENT, MOTOR, SHAFT,
  SWITCH_ON, SWITCH_OFF, NICHROME, SOLAR, CHARGE, PROPELLANT, ASH, MOLTEN, TILE, CHARCOAL, BRASS, GRAVEL,
  BASALT, CANVAS, BARK, MAGNET, GOLD, PAINT_RED, PAINT_BLUE, PAINT_WHITE, CARBON, ICE, STEAM,
  SMOKE, MAT_COUNT
};

enum Phase : uint8_t { GAS, SOLID, POWDER, LIQUID };
enum Pattern : uint8_t { P_NONE, P_WOOD, P_BRICK, P_NOISE, P_GRASS, P_METAL, P_STONE, P_LEAF, P_CELL };

struct Material {
  const char* name;
  uint32_t color;        // sRGB 0xRRGGBB
  float density;         // kg/m^3
  float cp;              // J/(kg K)
  float k;               // W/(m K)
  float toughness;       // J to fracture one litre (scaled to voxel volume at run time)
  float melt;            // deg C (0 = doesn't melt)
  float ignite;          // deg C (0 = not flammable)
  float burnTime;        // s of burning per voxel (thermal time)
  float burnPower;       // W per litre while burning
  float flameTemp;       // deg C a burning voxel heads towards
  Phase phase;
  bool wire;             // conducts circuit current
  bool transparent;
  float friction, restitution;
  float roughness, metal;
  Pattern pattern;
  uint8_t residue;       // what burning leaves behind
  uint8_t solidifyTo;    // what a melt of this freezes into
  float boil;            // liquid -> vapour
  float explosive;       // J per litre
  float tensile;         // Pa: what a bond within this material holds in tension/bending (0 = holds only by resting)
  const char* info;
  // Derived at start-up:
  float voxMass = 0, heatCap = 0, tough = 0, power = 0;
};

extern Material MATS[MAT_COUNT];
void initMaterials(float voxelSize);

inline bool isSolid(uint8_t m) { return MATS[m].phase == SOLID; }
inline bool isLiquid(uint8_t m) { return MATS[m].phase == LIQUID; }
inline bool isPowder(uint8_t m) { return MATS[m].phase == POWDER; }
inline bool isGas(uint8_t m) { return m != 0 && MATS[m].phase == GAS; }
inline bool isFluidish(uint8_t m) { return MATS[m].phase == LIQUID || MATS[m].phase == POWDER; }
inline bool isSpecialElectric(uint8_t m) {
  return m == TERM_POS || m == TERM_NEG || m == SWITCH_ON || m == SWITCH_OFF || m == FILAMENT || m == MOTOR ||
         m == NICHROME || m == SOLAR || m == CHARGE || m == PROPELLANT || m == LIION;
}
vec3 matLinear(uint8_t m);  // linear-space albedo

constexpr float AMBIENT = 20.f;
