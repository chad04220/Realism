#include "materials.h"
#include <cmath>
#include <utility>

Material MATS[MAT_COUNT];

namespace {
Material& def(Mat id, const char* name, uint32_t color, float rho, float cp, float k, float tough, const char* info) {
  Material& m = MATS[id];
  m = Material{};
  m.name = name; m.color = color; m.density = rho; m.cp = cp; m.k = k; m.toughness = tough; m.info = info;
  m.phase = SOLID; m.friction = 0.6f; m.restitution = 0.15f; m.roughness = 0.8f; m.metal = 0;
  m.flameTemp = 850; m.pattern = P_NOISE; m.residue = AIR; m.solidifyTo = id;
  return m;
}
void flammable(Material& m, float ignite, float burnTime, float power, uint8_t residue, float flame = 850) {
  m.ignite = ignite; m.burnTime = burnTime; m.burnPower = power; m.residue = residue; m.flameTemp = flame;
}
}  // namespace

void initMaterials(float vs) {
  // Toughness and burn power are quoted per litre (a 10 cm cube) and scaled below.
  def(AIR, "Air", 0x000000, 1.2f, 1005, 0.026f, 0, "").phase = GAS;
  def(BEDROCK, "Bedrock", 0x3b3a40, 3000, 800, 3, INFINITY, "The bottom of the world. Unbreakable.").pattern = P_STONE;
  { auto& m = def(STONE, "Granite", 0x8b8a90, 2700, 790, 2.8f, 400, "Hard igneous rock. Melts into lava around 1200 \xC2\xB0""C.");
    m.melt = 1215; m.solidifyTo = BASALT; m.pattern = P_STONE; m.friction = 0.7f; }
  { auto& m = def(DIRT, "Soil", 0x6f4b2e, 1500, 1000, 0.5f, 20, "Packed earth: clay, silt and organic matter."); m.friction = 0.8f; }
  { auto& m = def(GRASS, "Turf", 0x4a8a30, 1300, 1100, 0.4f, 15, "Grass roots binding the topsoil. The blades burn off to bare soil.");
    flammable(m, 340, 50, 700, DIRT, 650); m.pattern = P_GRASS; }
  { auto& m = def(SAND, "Sand", 0xc9ae78, 1600, 830, 0.3f, 2, "Loose quartz grains. Melt it hot enough and it becomes glass.");
    m.phase = POWDER; m.melt = 1700; m.solidifyTo = GLASS; }
  { auto& m = def(WATER, "Water", 0x2f6fb8, 1000, 4186, 0.6f, 0, "Huge heat capacity: the best fire extinguisher there is.");
    m.phase = LIQUID; m.transparent = true; m.boil = 100; m.roughness = 0.05f; m.pattern = P_NONE; }
  { auto& m = def(OAK, "Oak", 0x8f5f33, 700, 1700, 0.17f, 300, "Dense hardwood. Chars slowly; a poor conductor of heat.");
    flammable(m, 300, 900, 2600, ASH); m.pattern = P_WOOD; }
  { auto& m = def(PINE, "Pine plank", 0xd2a86e, 450, 1700, 0.12f, 150, "Light softwood. Nailed planks pop off under a good hit.");
    flammable(m, 280, 600, 2200, ASH); m.pattern = P_WOOD; }
  { auto& m = def(LEAVES, "Leaves", 0x3f8f35, 40, 2000, 0.1f, 3, "Thin and airy: a voxel of foliage is mostly air.");
    flammable(m, 260, 40, 1600, AIR, 700); m.pattern = P_LEAF; }
  { auto& m = def(BRICK, "Fired clay brick", 0xa84a33, 1900, 840, 0.7f, 150, "Strong in compression. The mortar joints give way first.");
    m.pattern = P_BRICK; m.friction = 0.75f; }
  { auto& m = def(MORTAR, "Lime mortar", 0xcfc6b4, 1700, 840, 0.6f, 15, "Weak lime-sand mortar holding the bricks together."); m.friction = 0.7f; }
  { auto& m = def(CONCRETE, "Concrete", 0xa3a29a, 2400, 880, 1.7f, 250, "Cement, sand and gravel, reinforced with steel rebar.");
    m.pattern = P_STONE; m.friction = 0.75f; }
  { auto& m = def(STEEL, "Steel", 0x8f99a3, 7850, 490, 50, 20000, "Tough iron alloy. Glows red near 600 \xC2\xB0""C, melts at 1450 \xC2\xB0""C.");
    m.melt = 1450; m.restitution = 0.25f; m.friction = 0.5f; m.roughness = 0.35f; m.metal = 1; m.pattern = P_METAL; }
  { auto& m = def(IRON, "Cast iron", 0x4d5157, 7200, 460, 55, 3000, "Heavy and brittle. Ideal for stable bases.");
    m.melt = 1200; m.roughness = 0.6f; m.metal = 1; m.pattern = P_METAL; }
  { auto& m = def(COPPER, "Copper", 0xd9824a, 8960, 385, 400, 8000, "Superb conductor of current and heat. Circuits run through copper.");
    m.melt = 1085; m.wire = true; m.roughness = 0.3f; m.metal = 1; m.pattern = P_METAL; }
  { auto& m = def(ALU, "Aluminium", 0xc8cdd2, 2700, 900, 237, 6000, "Light structural metal. Melts at only 660 \xC2\xB0""C.");
    m.melt = 660; m.roughness = 0.35f; m.metal = 1; m.pattern = P_METAL; }
  { auto& m = def(GLASS, "Glass", 0xcfeef6, 2500, 840, 1.0f, 5, "Brittle: it shatters at the slightest impact.");
    m.melt = 1400; m.transparent = true; m.restitution = 0.3f; m.friction = 0.4f; m.roughness = 0.05f; m.pattern = P_NONE; }
  { auto& m = def(BULBGLASS, "Bulb glass", 0xfff4d6, 250, 840, 1.0f, 2, "A thin blown-glass envelope around an argon fill.");
    m.melt = 1400; m.solidifyTo = GLASS; m.transparent = true; m.roughness = 0.05f; m.pattern = P_NONE; }
  { auto& m = def(RUBBER, "Rubber", 0x242424, 1100, 2000, 0.16f, 1500, "Grippy and bouncy. Burns with thick black smoke.");
    flammable(m, 400, 300, 2600, ASH); m.restitution = 0.75f; m.friction = 1.1f; m.roughness = 0.9f; }
  { auto& m = def(PLASTIC, "ABS plastic", 0xf0c64a, 1050, 1400, 0.2f, 200, "Moulded housing plastic. Softens and drips above 220 \xC2\xB0""C.");
    flammable(m, 420, 200, 2000, ASH); m.melt = 220; m.roughness = 0.45f; m.pattern = P_NONE; }
  { auto& m = def(LIION, "Li-ion cells", 0x2f63c8, 2500, 1000, 3, 100, "Stores the pack's energy. Past 150 \xC2\xB0""C it goes into thermal runaway.");
    m.roughness = 0.5f; m.pattern = P_CELL; }
  { auto& m = def(TERM_POS, "+ terminal", 0xd8352f, 8500, 380, 110, 3000, "Positive battery terminal.");
    m.wire = true; m.metal = 0.5f; m.roughness = 0.4f; m.pattern = P_NONE; }
  { auto& m = def(TERM_NEG, "\xE2\x88\x92 terminal", 0x26262c, 8500, 380, 110, 3000, "Negative battery terminal.");
    m.wire = true; m.metal = 0.5f; m.roughness = 0.4f; m.pattern = P_NONE; }
  { auto& m = def(FILAMENT, "Tungsten filament", 0xf3e9c4, 400, 134, 170, 5, "Tungsten coil in argon. White-hot when powered.");
    m.melt = 3400; m.solidifyTo = STEEL; m.metal = 1; m.pattern = P_NONE; }
  { auto& m = def(MOTOR, "Motor windings", 0x3a6488, 5000, 450, 40, 2000, "Copper coils round a steel stator: current in, torque out.");
    m.metal = 0.6f; m.roughness = 0.45f; m.pattern = P_METAL; }
  { auto& m = def(SHAFT, "Drive shaft", 0xc9d0d6, 7850, 490, 50, 15000, "Hardened steel shaft that carries torque to wheels and propellers.");
    m.melt = 1450; m.solidifyTo = STEEL; m.metal = 1; m.roughness = 0.25f; m.friction = 0.4f; m.pattern = P_METAL; }
  { auto& m = def(SWITCH_ON, "Switch (closed)", 0x35c86a, 3000, 500, 50, 200, "Press E to open the circuit.");
    m.wire = true; m.pattern = P_NONE; m.roughness = 0.4f; }
  { auto& m = def(SWITCH_OFF, "Switch (open)", 0xd04848, 3000, 500, 50, 200, "Press E to close the circuit.");
    m.pattern = P_NONE; m.roughness = 0.4f; }
  { auto& m = def(NICHROME, "Heater coil", 0x9a5f48, 2000, 450, 11, 3000, "Nickel-chromium resistance wire: 1.5 kW of heat when powered.");
    m.melt = 1400; m.solidifyTo = STEEL; m.metal = 0.8f; m.pattern = P_METAL; }
  { auto& m = def(SOLAR, "Solar cell", 0x1c2c62, 2300, 700, 1.5f, 15, "Silicon photovoltaic: about 30 W in full sun, nothing in shade.");
    m.roughness = 0.15f; m.pattern = P_CELL; }
  { auto& m = def(CHARGE, "Blasting charge", 0xc8352e, 1700, 1000, 0.2f, 60, "Detonates when heated past 300 \xC2\xB0""C or powered by a circuit.");
    m.explosive = 40000; m.pattern = P_NONE; }
  { auto& m = def(PROPELLANT, "Solid propellant", 0xd9c46a, 1800, 1200, 0.3f, 100, "Composite rocket fuel. Light it with heat or current.");
    m.pattern = P_NOISE; }
  { auto& m = def(ASH, "Ash", 0x56524c, 300, 800, 0.1f, 1, "What is left after burning."); m.phase = POWDER; }
  { auto& m = def(MOLTEN, "Molten material", 0xff6a10, 2600, 900, 20, 0, "Liquid, glowing and very hot. Freezes as it cools.");
    m.phase = LIQUID; m.roughness = 0.2f; m.pattern = P_NONE; }
  { auto& m = def(TILE, "Clay roof tile", 0xb5573a, 1900, 800, 0.8f, 60, "Terracotta tiles resting on wooden rafters."); m.pattern = P_STONE; }
  { auto& m = def(CHARCOAL, "Charcoal", 0x1f1d1c, 600, 1000, 0.2f, 30, "Burns long and very hot.");
    flammable(m, 400, 1500, 3500, ASH, 1000); }
  { auto& m = def(BRASS, "Brass", 0xc9a640, 8500, 380, 110, 8000, "Hinges, screws and fittings.");
    m.melt = 930; m.metal = 1; m.roughness = 0.3f; m.pattern = P_METAL; }
  { auto& m = def(GRAVEL, "Gravel", 0x8d867c, 1700, 800, 0.4f, 5, "Loose stones."); m.phase = POWDER; m.friction = 0.8f; m.pattern = P_STONE; }
  { auto& m = def(BASALT, "Basalt", 0x2c2829, 2900, 840, 1.7f, 350, "Cooled lava."); m.melt = 1215; m.pattern = P_STONE; }
  { auto& m = def(CANVAS, "Canvas", 0xd8cfb8, 300, 1300, 0.05f, 20, "Woven cotton."); flammable(m, 260, 80, 1500, ASH); }
  { auto& m = def(BARK, "Bark", 0x5b4330, 600, 1600, 0.12f, 120, "Rough outer layer of the trunk.");
    flammable(m, 300, 500, 2400, ASH); m.pattern = P_WOOD; }
  { auto& m = def(MAGNET, "Ferrite magnet", 0x3c3c44, 5000, 700, 4, 200, "Permanent magnet inside the motor."); m.pattern = P_NONE; }
  { auto& m = def(GOLD, "Gold contact", 0xe8b84a, 19300, 129, 318, 5000, "Corrosion-free electrical contact.");
    m.wire = true; m.melt = 1064; m.metal = 1; m.roughness = 0.2f; m.pattern = P_NONE; }
  { auto& m = def(PAINT_RED, "Red-painted steel", 0xc23b30, 7850, 490, 50, 20000, "Painted steel.");
    m.melt = 1450; m.solidifyTo = STEEL; m.metal = 0.2f; m.roughness = 0.4f; m.pattern = P_NONE; }
  { auto& m = def(PAINT_BLUE, "Blue-painted steel", 0x2f6db5, 7850, 490, 50, 20000, "Painted steel.");
    m.melt = 1450; m.solidifyTo = STEEL; m.metal = 0.2f; m.roughness = 0.4f; m.pattern = P_NONE; }
  { auto& m = def(PAINT_WHITE, "White-painted steel", 0xe6e4de, 7850, 490, 50, 20000, "Painted steel.");
    m.melt = 1450; m.solidifyTo = STEEL; m.metal = 0.2f; m.roughness = 0.4f; m.pattern = P_NONE; }
  { auto& m = def(CARBON, "Carbon fibre", 0x2a2d33, 1600, 800, 5, 5000, "Woven carbon in epoxy: stiff and light.");
    m.roughness = 0.3f; m.pattern = P_CELL; }
  { auto& m = def(ICE, "Ice", 0xd8f0ff, 917, 2100, 2.2f, 40, "Frozen water.");
    m.melt = 0.5f; m.solidifyTo = ICE; m.transparent = true; m.roughness = 0.1f; m.pattern = P_NONE; m.friction = 0.05f; }
  { auto& m = def(STEAM, "Steam", 0xf2f4f6, 0.6f, 2000, 0.02f, 0, "Water vapour. Condenses back into water as it cools.");
    m.phase = GAS; m.transparent = true; m.pattern = P_NONE; }
  { auto& m = def(SMOKE, "Smoke", 0x5a5754, 1.0f, 1100, 0.03f, 0, "What burning wood and plastic turn into. Glows while it is hot; settles as soot.");
    m.phase = GAS; m.transparent = true; m.pattern = P_NONE; }

  // Tensile (and bending) strength, Pa. Within one part, neighbouring voxels
  // are held together by this; between parts, by their joint (structure.cpp).
  // Typical values for the real materials; soil is cohesive but weak, so an
  // undercut bank or a wide tunnel caves in.
  const std::pair<Mat, float> tensile[] = {
      {BEDROCK, INFINITY}, {STONE, 1e7f}, {DIRT, 1.5e4f}, {GRASS, 2.5e4f}, {OAK, 5e7f}, {PINE, 3e7f}, {LEAVES, 2.5e7f} /* a voxel of foliage stands in for the twigs (wood) holding it */,
      {BRICK, 3e6f}, {MORTAR, 5e5f}, {CONCRETE, 1e7f}, {STEEL, 4e8f}, {IRON, 1.5e8f}, {COPPER, 2e8f}, {ALU, 2e8f},
      {GLASS, 3e7f}, {BULBGLASS, 2e7f}, {RUBBER, 1.5e7f}, {PLASTIC, 4e7f}, {LIION, 2e7f}, {TERM_POS, 2e8f},
      {TERM_NEG, 2e8f}, {FILAMENT, 1e8f}, {MOTOR, 1e8f}, {SHAFT, 5e8f}, {SWITCH_ON, 5e7f}, {SWITCH_OFF, 5e7f},
      {NICHROME, 3e8f}, {SOLAR, 5e7f}, {CHARGE, 1e6f}, {PROPELLANT, 2e6f}, {TILE, 3e6f}, {CHARCOAL, 3e5f},
      {BRASS, 3e8f}, {BASALT, 1.5e7f}, {CANVAS, 2e7f}, {BARK, 5e6f}, {MAGNET, 3e7f}, {GOLD, 1.2e8f},
      {PAINT_RED, 4e8f}, {PAINT_BLUE, 4e8f}, {PAINT_WHITE, 4e8f}, {CARBON, 6e8f}, {ICE, 1e6f}};
  for (auto& [m, t] : tensile) MATS[m].tensile = t;

  const float vol = vs * vs * vs;
  const float litres = vol / 0.001f;
  for (auto& m : MATS) {
    m.voxMass = m.density * vol;
    m.heatCap = m.density * m.cp * vol;
    m.tough = m.toughness * litres;
    m.power = m.burnPower * litres;
  }
}

vec3 matLinear(uint8_t id) {
  uint32_t c = MATS[id].color;
  auto lin = [](float s) { return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f); };
  return {lin(((c >> 16) & 255) / 255.f), lin(((c >> 8) & 255) / 255.f), lin((c & 255) / 255.f)};
}
