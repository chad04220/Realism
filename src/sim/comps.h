#pragma once
// Component registry. Every voxel belongs to a component (a brick, a battery,
// a wheel, a hinge...), and every component belongs to a named object. A
// component's joint is the energy (J) that knocks the whole part loose; below
// that, hits damage individual voxels instead.
#include <string>
#include <deque>
#include <vector>
#include "../core/math.h"

enum CompType : uint8_t {
  C_PART, C_TERRAIN, C_BATTERY, C_SOLAR, C_BULB, C_MOTOR, C_WHEEL, C_PROP, C_SWITCH, C_HEATER, C_CHARGE, C_ROCKET,
  C_VALVE,
  C_TYPE_COUNT
};

struct Joint { const char* name; float energy; };
namespace joints {
constexpr Joint welded{"welded", 900}, bolted{"bolted", 320}, pressfit{"press-fit", 110}, mortar{"mortared", 45},
    nailed{"nailed", 40}, screwed{"screwed", 35}, glued{"glued", 25}, clipped{"clipped", 14}, grown{"grown", 160},
    cast{"cast", 5000}, terrain{"ground", 1e30f}, resting{"resting", 0};
}

struct Component {
  std::string name, object;
  CompType type = C_PART;
  Joint joint = joints::glued;
  vec3 axis{1, 0, 0};       // wheel/prop/motor/rocket axis (local to whatever holds it)
  bool reversed = false;    // motor direction
  float capacity = 0, charge = 0;  // Wh
  float fuel = 0;           // rocket burn seconds left
  bool lit = false;         // rocket burning
  float exhaustCarry = 0;   // fraction of a propellant voxel due to burn next step
  float fuse = -1;          // seconds until a charge detonates
  // Live electrical state, refreshed by the circuit solver.
  bool powered = false, shorted = false;
  float powerFactor = 0;    // 0..1 how much of its rated power it is getting
  float drawW = 0;          // battery: current output
  float spin = 0, spinRate = 0;  // rotating parts: angle (rad) and rad/s for visuals
  float driveSpeed = 0;     // wheels: surface speed the motor is pushing for (m/s, signed)
  float driveForce = 0;     // wheels: traction force available (N); props: thrust (N)
  int driveContacts = 0;    // wheel contacts this substep (shares the force)
};

std::deque<Component>& comps();  // deque: references stay valid as parts are added
uint32_t newComp(const Component& c);
inline Component& comp(uint32_t id) { return comps()[id]; }
uint32_t cloneComp(uint32_t id);
const char* compTypeName(CompType t);
float devicePower(CompType t);   // rated electrical draw, W
