#include "comps.h"

std::deque<Component>& comps() {
  static std::deque<Component> v(1);  // id 0 = no component
  return v;
}

uint32_t newComp(const Component& c) {
  auto& v = comps();
  v.push_back(c);
  Component& n = v.back();
  if (n.type == C_BATTERY && n.capacity == 0) { n.capacity = 100; n.charge = 100; }
  if (n.type == C_ROCKET && n.fuel == 0) n.fuel = 6;
  return (uint32_t)v.size() - 1;
}

uint32_t cloneComp(uint32_t id) {
  Component c = comps()[id];
  c.powered = c.shorted = false;
  return newComp(c);
}

const char* compTypeName(CompType t) {
  switch (t) {
    case C_PART: return "Structural part";
    case C_TERRAIN: return "Ground";
    case C_BATTERY: return "Battery \xC2\xB7 12 V Li-ion";
    case C_SOLAR: return "Solar panel";
    case C_BULB: return "Light bulb \xC2\xB7 40 W";
    case C_MOTOR: return "Electric motor \xC2\xB7 200 W";
    case C_WHEEL: return "Wheel";
    case C_PROP: return "Propeller";
    case C_SWITCH: return "Switch";
    case C_HEATER: return "Heater \xC2\xB7 1.5 kW";
    case C_CHARGE: return "Explosive charge";
    case C_ROCKET: return "Solid rocket motor";
    default: return "Part";
  }
}

float devicePower(CompType t) {
  switch (t) {
    case C_BULB: return 40;
    case C_MOTOR: return 200;
    case C_HEATER: return 1500;
    case C_CHARGE: return 1;
    case C_ROCKET: return 1;
    default: return 0;
  }
}
