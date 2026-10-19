#include "assemblies.h"
#include <cmath>
#include "parts.h"

Component makePart(const std::string& name, const std::string& object, Joint j, CompType t) {
  Component c;
  c.name = name; c.object = object; c.joint = j; c.type = t;
  return c;
}

std::vector<ivec3> cellsOf(const Volume& v, uint32_t comp, uint8_t mat) {
  std::vector<ivec3> out;
  v.forEach([&](int x, int y, int z, Brick* br, int l) { if (br->comp[l] == comp && br->mat[l] == mat) out.push_back({x, y, z}); });
  return out;
}
ivec3 topmost(const std::vector<ivec3>& c) {
  ivec3 best = c.empty() ? ivec3() : c[0];
  for (auto& p : c) if (p.y > best.y) best = p;
  return best;
}
ivec3 extremal(const std::vector<ivec3>& c, int axis, bool maxSide) {
  ivec3 best = c.empty() ? ivec3() : c[0];
  for (auto& p : c) if (maxSide ? p[axis] > best[axis] : p[axis] < best[axis]) best = p;
  return best;
}

namespace {

float VS() { return g_sim ? g_sim->vs : 0.025f; }

void setOff(Volume& v, uint32_t sw) {
  for (auto& c : cellsOf(v, sw, SWITCH_ON)) v.set(c.x, c.y, c.z, SWITCH_OFF, sw);
}
std::vector<ivec3> switchCells(const Volume& v, uint32_t sw) {
  auto a = cellsOf(v, sw, SWITCH_ON);
  if (a.empty()) a = cellsOf(v, sw, SWITCH_OFF);
  return a;
}
// The top cell of each of a motor's two contact tabs, ordered along `axis`.
std::pair<ivec3, ivec3> motorTabs(const Volume& v, uint32_t mot, int axis) {
  auto tabs = cellsOf(v, mot, COPPER);
  ivec3 a = extremal(tabs, axis, false), b = extremal(tabs, axis, true);
  for (auto& c : tabs) {
    if (c[axis] == a[axis] && c.y > a.y) a = c;
    if (c[axis] == b[axis] && c.y > b.y) b = c;
  }
  return {a, b};
}

bool lamp(Volume& v, const vec3& o) {
  const float vs = VS();
  Shaper b(&v, vs);
  const std::string obj = "Desk lamp";
  uint32_t cBase = newComp(makePart("Lamp base", obj, joints::bolted));
  b.with(IRON, cBase).box(o + vec3(0.02f, 0, 0.02f), o + vec3(0.32f, 0.03f, 0.3f));
  uint32_t bat = stamp(v, *findPart("Battery")->tpl, b.toCell(o + vec3(0.06f, 0.03f, 0.12f)), obj);
  uint32_t sw = stamp(v, *findPart("Switch")->tpl, b.toCell(o + vec3(0.16f, 0.03f, 0.03f)), obj);
  uint32_t cPole = newComp(makePart("Pole and arm", obj, joints::welded));
  b.with(STEEL, cPole).box(o + vec3(0.14f, 0.03f, 0.25f), o + vec3(0.19f, 0.55f, 0.3f));
  b.with(STEEL, cPole).box(o + vec3(0.14f, 0.5f, 0.09f), o + vec3(0.19f, 0.55f, 0.3f));
  uint32_t cShade = newComp(makePart("Shade", obj, joints::screwed));
  vec3 bc = o + vec3(0.165f, 0.4f, 0.13f);
  b.with(ALU, cShade).boxIf(bc - vec3(0.11f, 0, 0.11f), bc + vec3(0.11f, 0.1f, 0.11f), [&](const vec3& p) {
    float h = (p.y - bc.y) / 0.1f;
    float r = length(vec3(p.x - bc.x, 0, p.z - bc.z));
    return std::fabs(r - lerpf(0.105f, 0.04f, h)) < vs * 0.75f;
  });
  b.with(PLASTIC, cShade).box(bc + vec3(-0.025f, 0.08f, -0.025f), bc + vec3(0.025f, 0.1f, 0.025f));
  uint32_t bulb = stamp(v, *findPart("Light bulb")->tpl, b.toCell(bc + vec3(-0.075f, -0.02f, -0.05f)), obj);
  b.with(PLASTIC, cShade).box(bc + vec3(-0.0125f, 0.075f, -0.0125f), bc + vec3(0.0125f, 0.08f, 0.0125f));
  uint32_t wire = newComp(makePart("Lamp flex", obj, joints::glued));
  auto pos = cellsOf(v, bat, TERM_POS), neg = cellsOf(v, bat, TERM_NEG);
  auto swc = switchCells(v, sw);
  auto leads = cellsOf(v, bulb, COPPER);
  bool ok = routeWire(v, topmost(pos), extremal(swc, 0, false), wire);
  ok &= routeWire(v, extremal(swc, 0, true), extremal(leads, 0, true), wire);
  ok &= routeWire(v, topmost(neg), extremal(leads, 0, false), wire);
  return ok;
}

bool fan(Volume& v, const vec3& o) {
  const float vs = VS();
  Shaper b(&v, vs);
  const std::string obj = "Desk fan";
  uint32_t cBase = newComp(makePart("Fan base", obj, joints::glued));
  b.with(PLASTIC, cBase).box(o + vec3(0.1f, 0, 0.1f), o + vec3(0.45f, 0.04f, 0.4f));
  uint32_t bat = stamp(v, *findPart("Battery")->tpl, b.toCell(o + vec3(0.15f, 0.04f, 0.28f)), obj);
  uint32_t sw = stamp(v, *findPart("Switch")->tpl, b.toCell(o + vec3(0.33f, 0.04f, 0.14f)), obj);
  setOff(v, sw);
  uint32_t cPost = newComp(makePart("Fan post", obj, joints::glued));
  b.with(PLASTIC, cPost).box(o + vec3(0.225f, 0.04f, 0.2f), o + vec3(0.275f, 0.3f, 0.25f));
  auto motorT = rotated(*findPart("Motor")->tpl, 1, 0);  // axis -z
  uint32_t mot = stamp(v, *motorT, b.toCell(o + vec3(0.2f, 0.3f, 0.1f)), obj);
  auto propT = rotated(*findPart("Propeller")->tpl, 1, 0);
  auto shaft = cellsOf(v, mot, SHAFT);
  ivec3 front = extremal(shaft, 2, false);
  ivec3 at(front.x - propT->vol.nx / 2, front.y - propT->vol.ny / 2, front.z - propT->vol.nz);
  stamp(v, *propT, at, obj);
  uint32_t wire = newComp(makePart("Fan wiring", obj, joints::glued));
  auto pos = cellsOf(v, bat, TERM_POS), neg = cellsOf(v, bat, TERM_NEG);
  auto swc = switchCells(v, sw);
  auto [tabA, tabB] = motorTabs(v, mot, 2);
  bool ok = routeWire(v, topmost(pos), extremal(swc, 0, true), wire);
  ok &= routeWire(v, extremal(swc, 0, false), tabA, wire);
  ok &= routeWire(v, topmost(neg), tabB, wire);
  return ok;
}

// Centroid (in cell units) of a template's or volume's cells of one material/component.
vec3 centroidOf(const Volume& v, uint8_t mat, uint32_t comp = 0) {
  vec3 s;
  int n = 0;
  v.forEach([&](int x, int y, int z, Brick* br, int l) {
    if (br->mat[l] != mat || (comp && br->comp[l] != comp)) return;
    s += vec3(x + 0.5f, y + 0.5f, z + 0.5f);
    n++;
  });
  return n ? s / (float)n : vec3(0);
}
ivec3 roundI(const vec3& p) { return ivec3((int)std::lround(p.x), (int)std::lround(p.y), (int)std::lround(p.z)); }

// Chassis with four wheels; the rear axle is driven by a motor if `driven`.
// Parts are aligned by their actual shaft voxels so this works at any voxel size.
bool chassis(Volume& v, const vec3& o, const std::string& obj, bool driven, uint32_t& motOut, uint32_t& plateOut) {
  const float vs = VS();
  Shaper b(&v, vs);
  auto wheelT = findPart("Wheel")->tpl;
  const float hubY = 0.125f, rearZ = 0.75f, frontZ = 0.2f, x0 = 0.05f, x1 = 0.575f;
  plateOut = newComp(makePart("Plywood deck", obj, joints::screwed));
  b.with(PINE, plateOut).box(o + vec3(x0, 0.225f, 0.02f), o + vec3(x1, 0.25f, 0.94f));
  uint32_t raxle = newComp(makePart(driven ? "Drive shaft" : "Rear axle", obj, joints::pressfit));
  uint32_t faxle = newComp(makePart("Front axle", obj, joints::pressfit));
  b.with(SHAFT, raxle).box(o + vec3(x0, hubY - vs, rearZ - vs), o + vec3(x1, hubY + vs, rearZ + vs));
  b.with(SHAFT, faxle).box(o + vec3(x0, hubY - vs, frontZ - vs), o + vec3(x1, hubY + vs, frontZ + vs));
  vec3 rc = centroidOf(v, SHAFT, raxle), fc = centroidOf(v, SHAFT, faxle);
  ivec3 axLo(1 << 30, 0, 0), axHi(-(1 << 30), 0, 0);
  v.forEach([&](int x, int, int, Brick* br, int l) { if (br->comp[l] == raxle) { axLo.x = std::min(axLo.x, x); axHi.x = std::max(axHi.x, x); } });
  motOut = 0;
  if (driven) {
    auto mt = findPart("Motor")->tpl;
    vec3 mc = centroidOf(mt->vol, SHAFT);
    ivec3 at = roundI(vec3((axLo.x + axHi.x + 1) * 0.5f, rc.y, rc.z) - mc);
    motOut = stamp(v, *mt, at, obj);
  }
  for (int k = 0; k < (driven ? 2 : 4); k++) {
    uint32_t c = newComp(makePart("Bearing block", obj, joints::bolted));
    float bx = (k & 1) ? 0.45f : 0.12f;
    float bz = k < 2 ? frontZ : rearZ;
    b.with(STEEL, c).box(o + vec3(bx, hubY + vs, bz - 0.025f), o + vec3(bx + 0.05f, 0.225f, bz + 0.025f));
  }
  // Wheels: hub centred on each axle end.
  vec3 wc;
  {
    int n = 0;
    wheelT->vol.forEach([&](int x, int y, int z, Brick*, int) { wc += vec3(x + 0.5f, y + 0.5f, z + 0.5f); n++; });
    wc = wc / (float)std::max(1, n);
  }
  for (int k = 0; k < 4; k++) {
    vec3 ac = (k & 2) ? rc : fc;
    int wx = (k & 1) ? axHi.x + 1 : axLo.x - wheelT->vol.nx;
    ivec3 at(wx, (int)std::lround(ac.y - wc.y), (int)std::lround(ac.z - wc.z));
    stamp(v, *wheelT, at, obj);
  }
  return true;
}

bool rover(Volume& v, const vec3& o) {
  const float vs = VS();
  Shaper b(&v, vs);
  const std::string obj = "Rover";
  uint32_t mot, plate;
  chassis(v, o, obj, true, mot, plate);
  uint32_t bat = stamp(v, *findPart("Battery")->tpl, b.toCell(o + vec3(0.21f, 0.25f, 0.4f)), obj);
  uint32_t sw = stamp(v, *findPart("Switch")->tpl, b.toCell(o + vec3(0.27f, 0.25f, 0.62f)), obj);
  setOff(v, sw);
  uint32_t crate = newComp(makePart("Cargo box", obj, joints::screwed));
  b.with(PINE, crate).boxIf(o + vec3(0.1f, 0.25f, 0.05f), o + vec3(0.5f, 0.385f, 0.32f), [&](const vec3& p) {
    vec3 q = p - o;
    return q.y < 0.275f || q.x < 0.125f || q.x > 0.475f || q.z < 0.075f || q.z > 0.295f;
  });
  uint32_t wire = newComp(makePart("Rover wiring", obj, joints::glued));
  auto pos = cellsOf(v, bat, TERM_POS), neg = cellsOf(v, bat, TERM_NEG);
  auto swc = switchCells(v, sw);
  auto [tabA, tabB] = motorTabs(v, mot, 0);
  bool ok = routeWire(v, topmost(pos), extremal(swc, 0, true), wire);
  ok &= routeWire(v, extremal(swc, 0, false), tabA, wire);
  ok &= routeWire(v, topmost(neg), tabB, wire);
  return ok;
}

bool fanCart(Volume& v, const vec3& o) {
  const float vs = VS();
  Shaper b(&v, vs);
  const std::string obj = "Fan cart";
  uint32_t mot, plate;
  chassis(v, o, obj, false, mot, plate);
  uint32_t mast = newComp(makePart("Motor mast", obj, joints::welded));
  b.with(STEEL, mast).box(o + vec3(0.28f, 0.25f, 0.8f), o + vec3(0.33f, 0.42f, 0.85f));
  auto motorT = rotated(*findPart("Motor")->tpl, 1, 0);  // axis -z: thrust towards the front (-z)
  mot = stamp(v, *motorT, b.toCell(o + vec3(0.255f, 0.42f, 0.72f)), obj);
  auto propT = rotated(*findPart("Propeller")->tpl, 1, 0);
  auto shaft = cellsOf(v, mot, SHAFT);
  ivec3 back = extremal(shaft, 2, true);
  stamp(v, *propT, ivec3(back.x - propT->vol.nx / 2, back.y - propT->vol.ny / 2, back.z + 1), obj);
  uint32_t bat = stamp(v, *findPart("Battery")->tpl, b.toCell(o + vec3(0.21f, 0.25f, 0.3f)), obj);
  uint32_t sw = stamp(v, *findPart("Switch")->tpl, b.toCell(o + vec3(0.27f, 0.25f, 0.55f)), obj);
  setOff(v, sw);
  uint32_t wire = newComp(makePart("Cart wiring", obj, joints::glued));
  auto pos = cellsOf(v, bat, TERM_POS), neg = cellsOf(v, bat, TERM_NEG);
  auto swc = switchCells(v, sw);
  auto [tabA, tabB] = motorTabs(v, mot, 2);
  bool ok = routeWire(v, topmost(pos), extremal(swc, 0, true), wire);
  ok &= routeWire(v, extremal(swc, 0, false), tabA, wire);
  ok &= routeWire(v, topmost(neg), tabB, wire);
  return ok;
}

bool rocketSled(Volume& v, const vec3& o) {
  const float vs = VS();
  Shaper b(&v, vs);
  const std::string obj = "Rocket sled";
  uint32_t mot, plate;
  chassis(v, o, obj, false, mot, plate);
  auto rocketT = rotated(*findPart("Rocket")->tpl, 1, 0);  // thrust axis -z (towards the front)
  // Nozzle (the template's -x end) faces +z after the turn: mount at the rear.
  uint32_t rk = stamp(v, *rocketT, b.toCell(o + vec3(0.26f, 0.25f, 0.5f)), obj);
  uint32_t strap = newComp(makePart("Mounting strap", obj, joints::bolted));
  b.with(STEEL, strap).box(o + vec3(0.24f, 0.25f, 0.6f), o + vec3(0.39f, 0.375f, 0.65f));
  uint32_t bat = stamp(v, *findPart("Battery")->tpl, b.toCell(o + vec3(0.21f, 0.25f, 0.1f)), obj);
  uint32_t sw = stamp(v, *findPart("Switch")->tpl, b.toCell(o + vec3(0.1f, 0.25f, 0.35f)), obj);
  setOff(v, sw);
  uint32_t wire = newComp(makePart("Igniter wiring", obj, joints::glued));
  auto leads = cellsOf(v, rk, COPPER);
  ivec3 la = leads.empty() ? ivec3() : leads[0], lb = la;
  for (auto& c : leads) { if (c.y < la.y || (c.y == la.y && c.z > la.z)) la = c; }
  for (auto& c : leads) { if (c.y > lb.y || (c.y == lb.y && c.z > lb.z)) lb = c; }
  // Outermost cell (nozzle end) of each lead.
  for (auto& c : leads) { if (c.y == la.y && c.x == la.x && c.z > la.z) la = c; if (c.y == lb.y && c.x == lb.x && c.z > lb.z) lb = c; }
  auto pos = cellsOf(v, bat, TERM_POS), neg = cellsOf(v, bat, TERM_NEG);
  auto swc = switchCells(v, sw);
  bool ok = routeWire(v, topmost(pos), extremal(swc, 0, true), wire);
  ok &= routeWire(v, extremal(swc, 0, false), la, wire);
  ok &= routeWire(v, topmost(neg), lb, wire);
  return ok;
}

bool heater(Volume& v, const vec3& o) {
  const float vs = VS();
  Shaper b(&v, vs);
  const std::string obj = "Space heater";
  uint32_t base = newComp(makePart("Heater base", obj, joints::bolted));
  b.with(IRON, base).box(o + vec3(0, 0, 0), o + vec3(0.45f, 0.03f, 0.35f));
  uint32_t legs = newComp(makePart("Element stand", obj, joints::welded));
  b.with(STEEL, legs).box(o + vec3(0.02f, 0.03f, 0.02f), o + vec3(0.05f, 0.2f, 0.05f));
  b.with(STEEL, legs).box(o + vec3(0.22f, 0.03f, 0.02f), o + vec3(0.25f, 0.2f, 0.05f));
  b.with(STEEL, legs).box(o + vec3(0.02f, 0.03f, 0.17f), o + vec3(0.05f, 0.2f, 0.2f));
  b.with(STEEL, legs).box(o + vec3(0.22f, 0.03f, 0.17f), o + vec3(0.25f, 0.2f, 0.2f));
  uint32_t el = stamp(v, *findPart("Heater")->tpl, b.toCell(o + vec3(0.02f, 0.2f, 0.03f)), obj);
  uint32_t bat = stamp(v, *findPart("Battery")->tpl, b.toCell(o + vec3(0.22f, 0.03f, 0.23f)), obj);
  uint32_t sw = stamp(v, *findPart("Switch")->tpl, b.toCell(o + vec3(0.33f, 0.03f, 0.05f)), obj);
  setOff(v, sw);
  uint32_t wire = newComp(makePart("Heater wiring", obj, joints::glued));
  auto leads = cellsOf(v, el, COPPER);
  ivec3 la = extremal(leads, 2, false), lb = extremal(leads, 2, true);
  for (auto& c : leads) { if (c.z == la.z && c.x < la.x) la = c; if (c.z == lb.z && c.x < lb.x) lb = c; }
  auto pos = cellsOf(v, bat, TERM_POS), neg = cellsOf(v, bat, TERM_NEG);
  auto swc = switchCells(v, sw);
  bool ok = routeWire(v, topmost(pos), extremal(swc, 0, true), wire);
  ok &= routeWire(v, extremal(swc, 0, false), la, wire);
  ok &= routeWire(v, topmost(neg), lb, wire);
  return ok;
}

bool demolition(Volume& v, const vec3& o) {
  const float vs = VS();
  Shaper b(&v, vs);
  const std::string obj = "Demolition rig";
  uint32_t board = newComp(makePart("Firing board", obj, joints::screwed));
  b.with(PINE, board).box(o, o + vec3(0.5f, 0.03f, 0.35f));
  // Three sticks bundled: touching charges go up together.
  uint32_t c0 = stamp(v, *findPart("Charge")->tpl, b.toCell(o + vec3(0.05f, 0.03f, 0.2f)), obj);
  stamp(v, *findPart("Charge")->tpl, b.toCell(o + vec3(0.05f, 0.03f, 0.25f)), obj);
  stamp(v, *findPart("Charge")->tpl, b.toCell(o + vec3(0.05f, 0.08f, 0.225f)), obj);
  uint32_t bat = stamp(v, *findPart("Battery")->tpl, b.toCell(o + vec3(0.25f, 0.03f, 0.02f)), obj);
  uint32_t sw = stamp(v, *findPart("Switch")->tpl, b.toCell(o + vec3(0.05f, 0.03f, 0.05f)), obj);
  setOff(v, sw);
  uint32_t wire = newComp(makePart("Firing wire", obj, joints::glued));
  auto leads = cellsOf(v, c0, COPPER);
  ivec3 la = leads.empty() ? ivec3() : leads[0], lb = la;
  for (auto& c : leads) { if (c.y < la.y) la = c; if (c.y > lb.y) lb = c; }
  for (auto& c : leads) { if (c.y == la.y && c.x > la.x) la = c; if (c.y == lb.y && c.x > lb.x) lb = c; }
  auto pos = cellsOf(v, bat, TERM_POS), neg = cellsOf(v, bat, TERM_NEG);
  auto swc = switchCells(v, sw);
  bool ok = routeWire(v, topmost(pos), extremal(swc, 0, true), wire);
  ok &= routeWire(v, extremal(swc, 0, false), la, wire);
  ok &= routeWire(v, topmost(neg), lb, wire);
  return ok;
}

bool solarLamp(Volume& v, const vec3& o) {
  const float vs = VS();
  Shaper b(&v, vs);
  const std::string obj = "Solar lamp";
  uint32_t post = newComp(makePart("Lamp post", obj, joints::welded));
  b.with(STEEL, post).box(o + vec3(0.15f, 0, 0.12f), o + vec3(0.25f, 0.02f, 0.22f));
  b.with(STEEL, post).box(o + vec3(0.18f, 0.02f, 0.15f), o + vec3(0.22f, 0.6f, 0.19f));
  uint32_t panel = stamp(v, *findPart("Solar panel")->tpl, b.toCell(o + vec3(0.0f, 0.6f, 0.02f)), obj);
  uint32_t bulb = stamp(v, *findPart("Light bulb")->tpl, b.toCell(o + vec3(0.25f, 0.4f, 0.12f)), obj);
  uint32_t wire = newComp(makePart("Lamp wiring", obj, joints::glued));
  auto leads = cellsOf(v, bulb, COPPER);
  auto pos = cellsOf(v, panel, TERM_POS), neg = cellsOf(v, panel, TERM_NEG);
  bool ok = routeWire(v, extremal(pos, 1, false), extremal(leads, 0, false), wire);
  ok &= routeWire(v, extremal(neg, 1, false), extremal(leads, 0, true), wire);
  return ok;
}

}  // namespace

const std::vector<Assembly>& assemblies() {
  static std::vector<Assembly> a = {
      {"Rover", "Battery, switch and a motor driving the rear axle. Flip the switch and it drives off.", vec3(0.62f, 0.45f, 0.96f), rover},
      {"Fan cart", "Free-wheeling cart pushed along by a propeller.", vec3(0.62f, 0.66f, 1.2f), fanCart},
      {"Rocket sled", "Solid rocket on a wheeled sled; the switch fires the igniter.", vec3(0.62f, 0.45f, 1.08f), rocketSled},
      {"Desk lamp", "40 W bulb, iron base, steel arm and an aluminium shade.", vec3(0.36f, 0.62f, 0.34f), lamp},
      {"Desk fan", "Motor and propeller on a plastic stand.", vec3(0.6f, 0.62f, 0.5f), fan},
      {"Space heater", "1.5 kW nichrome element. Glows, and sets fire to things above it.", vec3(0.46f, 0.32f, 0.36f), heater},
      {"Demolition rig", "Three bundled charges on a firing board. Flip the switch and run.", vec3(0.52f, 0.16f, 0.36f), demolition},
      {"Solar lamp", "Panel powers a bulb while the sun is on it.", vec3(0.42f, 0.66f, 0.36f), solarLamp},
  };
  return a;
}

const Assembly* findAssembly(const std::string& name) {
  for (auto& a : assemblies()) if (a.name == name) return &a;
  return nullptr;
}
