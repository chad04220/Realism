#include "parts.h"
#include <algorithm>
#include <cmath>

namespace {
std::vector<PartDef> g_parts;
float g_vs = 0.025f;

// Build a template in a volume sized for `size` metres.
std::shared_ptr<Template> make(const std::string& name, const vec3& size, const Component& proto,
                               const std::function<void(Shaper&)>& fn) {
  auto t = std::make_shared<Template>();
  t->name = name;
  t->proto = proto;
  // Author features in metres, so a shape can protrude past the nominal size
  // by up to a voxel (a battery terminal, a heater coil). Rasterise into a
  // slightly padded volume, then trim the empty tail: at coarse voxel sizes a
  // tight declared size used to clip exactly those features, which silently
  // broke terminals and wires.
  const int pad = 2;
  int nx = std::max(1, (int)std::ceil(size.x / g_vs - 1e-3f)) + pad,
      ny = std::max(1, (int)std::ceil(size.y / g_vs - 1e-3f)) + pad,
      nz = std::max(1, (int)std::ceil(size.z / g_vs - 1e-3f)) + pad;
  t->vol.resize(nx, ny, nz);
  Shaper s(&t->vol, g_vs);
  s.comp = 1;
  fn(s);
  int hx = -1, hy = -1, hz = -1;
  t->vol.forEach([&](int x, int y, int z, Brick*, int) {
    hx = std::max(hx, x); hy = std::max(hy, y); hz = std::max(hz, z);
  });
  if (hx >= 0 && (hx + 1 < nx || hy + 1 < ny || hz + 1 < nz)) {
    Volume trimmed(hx + 1, hy + 1, hz + 1);
    t->vol.forEach([&](int x, int y, int z, Brick*, int) { trimmed.put(x, y, z, t->vol.get(x, y, z)); });
    t->vol = std::move(trimmed);
  }
  return t;
}

void add(const std::string& name, const std::string& blurb, int icon, std::shared_ptr<Template> t) {
  g_parts.push_back({name, blurb, icon, t});
}
}  // namespace

void initParts(float vs) {
  g_vs = vs;
  g_parts.clear();
  const float v = vs;

  {  // Battery: Li-ion pack, terminals on top at each end.
    Component c; c.name = "Battery pack"; c.type = C_BATTERY; c.joint = joints::clipped; c.capacity = 100; c.charge = 100;
    add("Battery", "12 V Li-ion pack, 100 Wh. Wire its + and \xE2\x88\x92 terminals to a load.", 1,
        make("Battery", vec3(0.2f, 0.1f, 0.1f), c, [&](Shaper& s) {
          s.mat = LIION; s.box(vec3(0), vec3(0.2f, 0.075f, 0.1f));
          s.mat = PLASTIC; s.box(vec3(0, 0.075f - v, 0), vec3(0.2f, 0.075f, 0.1f));
          // Put the terminals at opposite ends with at least one empty cell
          // between them; at coarse voxel sizes the old inset positions met in
          // the middle and shorted the pack to itself.
          s.mat = TERM_NEG; s.box(vec3(0, 0.075f, 0.05f - v), vec3(v, 0.075f + v, 0.05f));
          s.mat = TERM_POS; s.box(vec3(0.2f - v, 0.075f, 0.05f - v), vec3(0.2f, 0.075f + v, 0.05f));
        }));
  }
  {  // Motor: steel can, windings around a through-shaft (output at both ends), contact tabs on top.
    // The axis runs along x through (y 0.075, z 0.075), a cell boundary, so the shaft is 2x2 voxels.
    Component c; c.name = "Electric motor"; c.type = C_MOTOR; c.joint = joints::bolted; c.axis = vec3(1, 0, 0);
    add("Motor", "200 W DC motor with a through-shaft. Power it and it turns shafts, wheels and propellers touching either end.", 2,
        make("Motor", vec3(0.2f, 0.175f, 0.15f), c, [&](Shaper& s) {
          vec3 a(0.05f, 0.075f, 0.075f), b(0.15f, 0.075f, 0.075f);
          s.mat = STEEL; s.cylinder(a, b, 0.07f);
          s.mat = MOTOR; s.cylinder(a, b, 0.045f);
          s.mat = SHAFT; s.box(vec3(0, 0.075f - v, 0.075f - v), vec3(0.2f, 0.075f + v, 0.075f + v));
          // Contact tabs at each end of the windings, poking out of the can.
          // Start them at the winding surface: a 5 mm gap is less than a voxel
          // at 3 cm and left the motor with no electrical connection.
          s.mat = COPPER;
          s.box(vec3(0.05f, 0.12f, 0.075f - v), vec3(0.05f + v, 0.175f, 0.075f));
          s.box(vec3(0.15f - v, 0.12f, 0.075f - v), vec3(0.15f, 0.175f, 0.075f));
        }));
  }
  for (float r : {0.12f, 0.2f}) {  // Wheels: rubber tyre, steel rim, hub. Axis x.
    Component c; c.name = r < 0.15f ? "Wheel" : "Big wheel"; c.type = C_WHEEL; c.joint = joints::pressfit; c.axis = vec3(1, 0, 0);
    float w = r < 0.15f ? 0.05f : 0.075f;
    add(c.name, r < 0.15f ? "24 cm rubber tyre on a steel rim. Mount it on a shaft." : "40 cm off-road tyre.", 3,
        make(c.name, vec3(w, 2 * r, 2 * r), c, [&, r, w](Shaper& s) {
          vec3 a(0, r, r), b(w, r, r);
          s.mat = RUBBER; s.tube(a, b, r, r - 0.035f);
          s.mat = STEEL; s.tube(a, b, r - 0.035f, r - 0.05f);
          s.mat = STEEL; s.cylinder(a, b, 0.03f);
          // Spokes.
          for (int k = 0; k < 5; k++) {
            float ang = k * 2 * PI / 5;
            vec3 d(0, std::cos(ang), std::sin(ang));
            s.cylinder(a + vec3(w * 0.5f - v, 0, 0) + d * 0.02f, a + vec3(w * 0.5f - v, 0, 0) + d * (r - 0.045f), 0.012f);
          }
        }));
  }
  {  // Propeller: three blades in the YZ plane, hub on the axis (x).
    Component c; c.name = "Propeller"; c.type = C_PROP; c.joint = joints::pressfit; c.axis = vec3(1, 0, 0);
    add("Propeller", "Three-blade prop. Spun by a motor it pushes 28 N of thrust and a stiff breeze.", 4,
        make("Propeller", vec3(0.05f, 0.4f, 0.4f), c, [&](Shaper& s) {
          vec3 h(0.025f, 0.2f, 0.2f);
          s.mat = ALU; s.cylinder(h - vec3(0.025f, 0, 0), h + vec3(0.025f, 0, 0), 0.03f);
          s.mat = PLASTIC;
          for (int k = 0; k < 3; k++) {
            float ang = k * 2 * PI / 3;
            vec3 d(0, std::cos(ang), std::sin(ang)), side(0, -std::sin(ang), std::cos(ang));
            for (float t = 0.03f; t < 0.19f; t += v * 0.5f) {
              float wdt = 0.035f * (1.f - t / 0.24f);
              s.box(h + d * t - side * wdt + vec3(-v * 0.5f, 0, 0), h + d * t + side * wdt + vec3(v * 0.5f + (t < 0.1f ? v : 0), v * 0.5f, v * 0.5f));
            }
          }
        }));
  }
  {  // Switch: an in-line lever on a plastic base. Wire goes in one end, out the other.
    Component c; c.name = "Toggle switch"; c.type = C_SWITCH; c.joint = joints::screwed;
    add("Switch", "Press E on it to open or close the circuit.", 5,
        make("Switch", vec3(3 * v, 2 * v, 2 * v), c, [&](Shaper& s) {
          s.mat = PLASTIC; s.box(vec3(0), vec3(3 * v, v, 2 * v));
          s.mat = SWITCH_ON; s.box(vec3(0, v, 0), vec3(3 * v, 2 * v, v));
        }));
  }
  {  // Light bulb: filament with copper leads out through a glass globe.
    Component c; c.name = "Light bulb"; c.type = C_BULB; c.joint = joints::screwed;
    add("Light bulb", "40 W incandescent. Connect a wire to each lead.", 6,
        make("Light bulb", vec3(0.15f, 0.1f, 0.1f), c, [&](Shaper& s) {
          vec3 cen(0.075f, 0.05f, 0.05f);
          s.mat = BULBGLASS;
          s.boxIf(cen - vec3(0.05f), cen + vec3(0.05f), [&](const vec3& p) { float d = length(p - cen); return d <= 0.045f && d >= 0.02f; });
          s.mat = FILAMENT; s.box(cen - vec3(v, v * 0.5f, v * 0.5f), cen + vec3(v, v * 0.5f, v * 0.5f));
          s.mat = COPPER;
          // Keep each lead at least one voxel long so coarse grids don't clip a
          // lead away and leave the filament with only one terminal.
          s.box(vec3(0, 0.05f - v * 0.5f, 0.05f - v * 0.5f),
                vec3(std::max(0.05f, 0.075f - v), 0.05f + v * 0.5f, 0.05f + v * 0.5f));
          s.box(vec3(std::min(0.1f, 0.075f + v), 0.05f - v * 0.5f, 0.05f - v * 0.5f),
                vec3(0.15f, 0.05f + v * 0.5f, 0.05f + v * 0.5f));
        }));
  }
  {  // Heater: zig-zag nichrome on a ceramic-ish (stone) plate with copper leads.
    Component c; c.name = "Heater element"; c.type = C_HEATER; c.joint = joints::screwed;
    add("Heater", "1.5 kW resistance heater. Glows orange; sets fuel alight.", 7,
        make("Heater", vec3(0.2f, 0.05f, 0.15f), c, [&](Shaper& s) {
          s.mat = STONE; s.box(vec3(0, 0, 0), vec3(0.2f, v, 0.15f));
          s.mat = NICHROME;
          for (int k = 0; k < 4; k++) {
            float z = 0.025f + k * 0.03f;
            s.box(vec3(0.03f, v, z), vec3(0.17f, 2 * v, z + v));
            if (k < 3) { float x = (k % 2 == 0) ? 0.17f - v : 0.03f; s.box(vec3(x, v, z), vec3(x + v, 2 * v, z + 0.03f + v)); }
          }
          s.mat = COPPER;
          s.box(vec3(0, v, 0.025f), vec3(0.03f, 2 * v, 0.025f + v));
          s.box(vec3(0, v, 0.115f), vec3(0.03f, 2 * v, 0.115f + v));
        }));
  }
  {  // Solar panel: cells in an aluminium frame, terminals on one edge.
    Component c; c.name = "Solar panel"; c.type = C_SOLAR; c.joint = joints::bolted; c.charge = 1;
    add("Solar panel", "~30 W in direct sun. Shade it and the power drops.", 8,
        make("Solar panel", vec3(0.4f, 0.05f, 0.3f), c, [&](Shaper& s) {
          s.mat = ALU; s.box(vec3(0), vec3(0.4f, v, 0.3f));
          s.mat = SOLAR; s.box(vec3(v, v, v), vec3(0.4f - v, 2 * v, 0.3f - v));
          s.mat = TERM_POS; s.box(vec3(0.4f - 3 * v, 0, 0.3f), vec3(0.4f - 2 * v, v, 0.3f + v));
          s.mat = TERM_NEG; s.box(vec3(2 * v, 0, 0.3f), vec3(3 * v, v, 0.3f + v));
        }));
  }
  {  // Blasting charge: a stick with two detonator leads.
    Component c; c.name = "Blasting charge"; c.type = C_CHARGE; c.joint = joints::clipped;
    add("Charge", "20 kJ stick of high explosive. Set it off with heat, a circuit, or E to light the fuse.", 9,
        make("Charge", vec3(0.25f, 0.05f, 0.05f), c, [&](Shaper& s) {
          s.mat = CHARGE; s.box(vec3(0, 0, 0), vec3(0.2f, 0.05f, 0.05f));
          s.mat = COPPER; s.box(vec3(0.2f, 0, 0), vec3(0.25f, v, v));
          s.box(vec3(0.2f, 0.05f - v, 0.05f - v), vec3(0.25f, 0.05f, 0.05f));
        }));
  }
  {  // Rocket motor: steel case, propellant core, open nozzle at -x. Thrust along +x.
    Component c; c.name = "Rocket motor"; c.type = C_ROCKET; c.joint = joints::bolted; c.axis = vec3(1, 0, 0); c.fuel = 6;
    add("Rocket", "160 N for 6 seconds. Light the nozzle with the torch or wire the igniter.", 10,
        make("Rocket", vec3(0.4f, 0.1f, 0.1f), c, [&](Shaper& s) {
          vec3 a(0.05f, 0.05f, 0.05f), b(0.4f, 0.05f, 0.05f);
          s.mat = STEEL; s.tube(a, b, 0.05f, 0.03f);
          s.mat = PAINT_RED; s.cylinder(b - vec3(0.06f, 0, 0), b, 0.05f);
          s.mat = PROPELLANT; s.cylinder(a + vec3(v, 0, 0), b - vec3(0.06f, 0, 0), 0.03f);
          s.mat = STEEL; s.tube(vec3(0, 0.05f, 0.05f), a, 0.045f, 0.025f);
          // Igniter leads enter through the nozzle and touch the propellant face.
          s.mat = COPPER;
          s.box(vec3(0, 0.05f - v, 0.05f - v), vec3(0.075f, 0.05f, 0.05f));
          s.box(vec3(0, 0.05f, 0.05f), vec3(0.075f, 0.05f + v, 0.05f + v));
        }));
  }
  {  // Crate: hollow pine box.
    Component c; c.name = "Crate"; c.type = C_PART; c.joint = joints::nailed;
    add("Crate", "Nailed pine box, 40 cm.", 11, make("Crate", vec3(0.4f), c, [&](Shaper& s) {
      s.mat = PINE;
      s.boxIf(vec3(0), vec3(0.4f), [&](const vec3& p) {
        return p.x < 2 * v || p.x > 0.4f - 2 * v || p.y < 2 * v || p.y > 0.4f - 2 * v || p.z < 2 * v || p.z > 0.4f - 2 * v;
      });
    }));
  }
}

const std::vector<PartDef>& partCatalogue() { return g_parts; }

const PartDef* findPart(const std::string& name) {
  for (auto& p : g_parts) if (p.name == name) return &p;
  return nullptr;
}

std::shared_ptr<Template> rotated(const Template& t, int yaw, int pitch) {
  yaw = ((yaw % 4) + 4) % 4;
  pitch = ((pitch % 4) + 4) % 4;
  auto cur = std::make_shared<Template>();
  cur->name = t.name; cur->category = t.category; cur->proto = t.proto;
  cur->vol.resize(t.vol.nx, t.vol.ny, t.vol.nz);
  t.vol.forEach([&](int x, int y, int z, Brick* br, int l) {
    Cell c; c.mat = br->mat[l]; c.comp = br->comp[l]; c.flags = br->flags[l]; c.hp = br->hp[l]; c.aux = br->aux[l]; c.temp = br->temp[l];
    cur->vol.put(x, y, z, c);
  });
  for (int i = 0; i < yaw; i++) {
    auto n = std::make_shared<Template>();
    n->name = cur->name; n->category = cur->category; n->proto = cur->proto;
    n->vol.resize(cur->vol.nz, cur->vol.ny, cur->vol.nx);
    int nx = cur->vol.nx;
    cur->vol.forEach([&](int x, int y, int z, Brick* br, int l) {
      Cell c; c.mat = br->mat[l]; c.comp = br->comp[l]; c.flags = br->flags[l]; c.hp = br->hp[l]; c.aux = br->aux[l]; c.temp = br->temp[l];
      n->vol.put(z, y, nx - 1 - x, c);
    });
    vec3 a = n->proto.axis;
    n->proto.axis = vec3(a.z, a.y, -a.x);
    cur = n;
  }
  for (int i = 0; i < pitch; i++) {
    auto n = std::make_shared<Template>();
    n->name = cur->name; n->category = cur->category; n->proto = cur->proto;
    n->vol.resize(cur->vol.nx, cur->vol.nz, cur->vol.ny);
    int nz = cur->vol.nz;
    cur->vol.forEach([&](int x, int y, int z, Brick* br, int l) {
      Cell c; c.mat = br->mat[l]; c.comp = br->comp[l]; c.flags = br->flags[l]; c.hp = br->hp[l]; c.aux = br->aux[l]; c.temp = br->temp[l];
      n->vol.put(x, nz - 1 - z, y, c);
    });
    vec3 a = n->proto.axis;
    n->proto.axis = vec3(a.x, -a.z, a.y);
    cur = n;
  }
  return cur;
}

uint32_t stamp(Volume& dst, const Template& t, const ivec3& at, const std::string& objectName) {
  Component c = t.proto;
  if (!objectName.empty()) c.object = objectName;
  if (c.type == C_BATTERY && c.capacity == 0) { c.capacity = 100; c.charge = 100; }
  uint32_t id = newComp(c);
  t.vol.forEach([&](int x, int y, int z, Brick* br, int l) {
    int X = at.x + x, Y = at.y + y, Z = at.z + z;
    if (!dst.inb(X, Y, Z)) return;
    Cell cell;
    cell.mat = br->mat[l];
    cell.comp = id;
    cell.temp = AMBIENT;
    dst.put(X, Y, Z, cell);
  });
  return id;
}

std::shared_ptr<Template> captureComponent(const Volume& src, uint32_t cid) {
  ivec3 lo(1 << 30, 1 << 30, 1 << 30), hi(-1, -1, -1);
  src.forEach([&](int x, int y, int z, Brick* br, int l) {
    if (br->comp[l] != cid) return;
    lo = {std::min(lo.x, x), std::min(lo.y, y), std::min(lo.z, z)};
    hi = {std::max(hi.x, x), std::max(hi.y, y), std::max(hi.z, z)};
  });
  if (hi.x < 0) return nullptr;
  auto t = std::make_shared<Template>();
  t->proto = comp(cid);
  t->proto.powered = false;
  t->name = t->proto.name;
  t->vol.resize(hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1);
  src.forEach([&](int x, int y, int z, Brick* br, int l) {
    if (br->comp[l] != cid) return;
    Cell c; c.mat = br->mat[l]; c.comp = 1; c.hp = br->hp[l];
    t->vol.put(x - lo.x, y - lo.y, z - lo.z, c);
  });
  return t;
}
