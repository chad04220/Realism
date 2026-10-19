// Circuits and machines. Current flows through "wire" materials (copper,
// terminals, closed switches). A load (filament, motor winding, heater coil,
// detonator, igniter) runs when it touches one net reaching a source's +
// terminal and another reaching the same source's − terminal. A net that
// joins both terminals of one source is a short circuit: the battery dumps
// power into the wire and itself, which heats until something gives.
// Motors turn shafts; shafts turn wheels and propellers; rockets push.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include "sim.h"

namespace {

struct Net {
  std::vector<uint32_t> posOf, negOf;  // sources whose +/− terminals are in this net
  std::vector<ivec3> cells;
};

bool isLoadMat(uint8_t m) { return m == FILAMENT || m == MOTOR || m == NICHROME || m == CHARGE || m == PROPELLANT; }

struct ShortHeat { int bodyId; std::vector<ivec3> cells; uint32_t source; };
std::vector<ShortHeat> g_shorts;

// Motor -> wheels/props it turns (found through shaft voxels).
std::unordered_map<uint32_t, std::vector<uint32_t>> g_links;

template <class GetMat, class GetComp>
void solveVolume(Sim& s, const std::vector<ivec3>& specials, GetMat mat, GetComp compAt, int bodyId, float dt) {
  // 1. Flood nets from every wire cell next to... simply from every wire special.
  std::unordered_map<int64_t, int> netOf;
  std::vector<Net> nets;
  for (const ivec3& p : specials) {
    uint8_t m = mat(p);
    if (!MATS[m].wire) continue;
    if (netOf.count(packKey(p.x, p.y, p.z))) continue;
    int id = (int)nets.size();
    nets.emplace_back();
    std::vector<ivec3> stack{p};
    netOf[packKey(p.x, p.y, p.z)] = id;
    while (!stack.empty() && nets[id].cells.size() < 400000) {
      ivec3 c = stack.back();
      stack.pop_back();
      nets[id].cells.push_back(c);
      uint8_t cm = mat(c);
      if (cm == TERM_POS || cm == TERM_NEG) {
        uint32_t src = compAt(c);
        if (src && (comp(src).type == C_BATTERY || comp(src).type == C_SOLAR)) {
          auto& v = cm == TERM_POS ? nets[id].posOf : nets[id].negOf;
          if (std::find(v.begin(), v.end(), src) == v.end()) v.push_back(src);
        }
      }
      for (auto& d : DIRS6) {
        ivec3 q(c.x + d[0], c.y + d[1], c.z + d[2]);
        if (!MATS[mat(q)].wire) continue;
        int64_t k = packKey(q.x, q.y, q.z);
        if (netOf.count(k)) continue;
        netOf[k] = id;
        stack.push_back(q);
      }
    }
  }
  // 2. Sources: short circuits.
  std::vector<uint32_t> sources;
  for (auto& n : nets) {
    for (uint32_t c : n.posOf) if (std::find(sources.begin(), sources.end(), c) == sources.end()) sources.push_back(c);
    for (uint32_t c : n.negOf) if (std::find(sources.begin(), sources.end(), c) == sources.end()) sources.push_back(c);
  }
  for (uint32_t src : sources) { comp(src).shorted = false; comp(src).drawW = 0; }
  for (auto& n : nets)
    for (uint32_t p : n.posOf)
      if (std::find(n.negOf.begin(), n.negOf.end(), p) != n.negOf.end()) {
        Component& c = comp(p);
        if (!c.shorted && c.charge > 0) {
          c.shorted = true;
          s.event(EV_SHORT, vec3(0), 0, 0, p);
          g_shorts.push_back({bodyId, n.cells, p});
        }
      }
  // 3. Loads: group load voxels by component, collect adjacent nets.
  std::unordered_map<uint32_t, std::vector<int>> loadNets;
  for (const ivec3& p : specials) {
    uint8_t m = mat(p);
    if (!isLoadMat(m)) continue;
    uint32_t cid = compAt(p);
    if (!cid) continue;
    auto& v = loadNets[cid];
    for (auto& d : DIRS6) {
      ivec3 q(p.x + d[0], p.y + d[1], p.z + d[2]);
      auto it = netOf.find(packKey(q.x, q.y, q.z));
      if (it != netOf.end() && std::find(v.begin(), v.end(), it->second) == v.end()) v.push_back(it->second);
    }
  }
  // Demand per source, then supply.
  std::unordered_map<uint32_t, float> demand;
  std::vector<std::pair<uint32_t, uint32_t>> powered;  // load, source
  for (auto& [cid, nv] : loadNets) {
    Component& L = comp(cid);
    uint32_t chosen = 0;
    for (int a : nv)
      for (uint32_t src : nets[a].posOf) {
        Component& S = comp(src);
        if (S.shorted || (S.type == C_BATTERY && S.charge <= 0)) continue;
        for (int b : nv)
          if (b != a && std::find(nets[b].negOf.begin(), nets[b].negOf.end(), src) != nets[b].negOf.end()) chosen = src;
      }
    L.powered = chosen != 0;
    if (chosen) { demand[chosen] += devicePower(L.type); powered.push_back({cid, chosen}); }
    else L.powerFactor = 0;
  }
  for (auto& [cid, src] : powered) {
    Component& S = comp(src);
    float supply = 1e9f;
    if (S.type == C_SOLAR) supply = S.capacity;  // solar: current output, W (set by the device step)
    float f = std::min(1.f, supply / std::max(1e-3f, demand[src]));
    comp(cid).powerFactor = f;
    if (f < 0.05f) comp(cid).powered = false;
  }
  for (auto& [src, w] : demand) {
    Component& S = comp(src);
    S.drawW = w;
    if (S.type == C_BATTERY) S.charge = std::max(0.f, S.charge - w * dt / 3600.f);
  }
  s.stats.circuits += (int)sources.size();
  if (getenv("REALISM_DEBUG_ELEC")) {
    fprintf(stderr, "[elec body %d] specials=%zu nets=%zu sources=%zu loads=%zu\n", bodyId, specials.size(), nets.size(), sources.size(), loadNets.size());
    for (size_t i = 0; i < nets.size(); i++) fprintf(stderr, "   net %zu: %zu cells pos=%zu neg=%zu\n", i, nets[i].cells.size(), nets[i].posOf.size(), nets[i].negOf.size());
    for (auto& [cid, nv] : loadNets) { fprintf(stderr, "   load %u (%s) nets:", cid, comp(cid).name.c_str()); for (int a : nv) fprintf(stderr, " %d", a); fprintf(stderr, " powered=%d\n", comp(cid).powered); }
  }
  // 4. Mechanical links: motor -> shafts -> wheels/props.
  for (auto& [cid, nv] : loadNets) {
    if (comp(cid).type != C_MOTOR) continue;
    std::vector<uint32_t> links;
    std::vector<ivec3> stack;
    std::unordered_map<int64_t, char> seen;
    for (const ivec3& p : specials) if (mat(p) == MOTOR && compAt(p) == cid) { stack.push_back(p); seen[packKey(p.x, p.y, p.z)] = 1; }
    int guard = 0;
    while (!stack.empty() && guard++ < 20000) {
      ivec3 c = stack.back();
      stack.pop_back();
      for (auto& d : DIRS6) {
        ivec3 q(c.x + d[0], c.y + d[1], c.z + d[2]);
        int64_t k = packKey(q.x, q.y, q.z);
        if (seen.count(k)) continue;
        seen[k] = 1;
        uint8_t qm = mat(q);
        uint32_t qc = compAt(q);
        if (qc && (comp(qc).type == C_WHEEL || comp(qc).type == C_PROP)) {
          if (std::find(links.begin(), links.end(), qc) == links.end()) links.push_back(qc);
        } else if (qm == SHAFT) {
          stack.push_back(q);
        }
      }
    }
    g_links[cid] = links;
  }
}

}  // namespace

void Sim::stepElectric(float dt) {
  g_shorts.clear();
  g_links.clear();
  stats.circuits = 0;
  for (auto& c : comps()) c.powered = false;
  // World circuits.
  if (!worldSpecials.empty()) {
    std::vector<ivec3> sp;
    sp.reserve(worldSpecials.size());
    for (int64_t k : worldSpecials) sp.push_back(unpackKey(k));
    solveVolume(*this, sp, [&](const ivec3& p) { return world.matI(p); }, [&](const ivec3& p) { return world.comp(p.x, p.y, p.z); }, 0, dt);
  }
  // Body circuits.
  for (auto& bp : bodies) {
    Body& b = *bp;
    if (b.ghost) continue;
    bool hasDevice = false;
    for (uint32_t c : b.compIds) {
      CompType t = comp(c).type;
      if (t == C_BATTERY || t == C_SOLAR) { hasDevice = true; break; }
    }
    if (!hasDevice) continue;
    std::vector<ivec3> sp;
    b.vol.forEach([&](int x, int y, int z, Brick* br, int l) {
      uint8_t m = br->mat[l];
      if (MATS[m].wire || isSpecialElectric(m)) sp.push_back({x, y, z});
    });
    solveVolume(*this, sp, [&](const ivec3& p) { return b.vol.matI(p); }, [&](const ivec3& p) { return b.vol.comp(p.x, p.y, p.z); }, b.id, dt);
  }
  int poweredN = 0;
  for (auto& c : comps()) if (c.powered) poweredN++;
  stats.poweredDevices = poweredN;
}

// Per-frame device behaviour: light, heat, torque, thrust.
void Sim::stepDevices(float dt) {
  const float warp = cfg.thermalWarp;
  // Short circuits heat their wires and the battery.
  for (auto& sh : g_shorts) {
    Component& S = comp(sh.source);
    if (S.type == C_BATTERY) S.charge = std::max(0.f, S.charge - 2000.f * dt / 3600.f);
    if (S.charge <= 0 && S.type == C_BATTERY) continue;
    Body* b = sh.bodyId ? bodyById(sh.bodyId) : nullptr;
    Volume& v = b ? b->vol : world;
    if (sh.bodyId && !b) continue;
    // Internal resistance heats the cells too: ~600 W into the pack.
    if (b) {
      std::vector<std::pair<Brick*, int>> cellsIn;
      b->vol.forEach([&](int x, int y, int z, Brick* br, int l) { if (br->comp[l] == sh.source && br->mat[l] == LIION) cellsIn.push_back({br, l}); });
      float q = 600.f * dt * warp / std::max<size_t>(1, cellsIn.size());
      for (auto& [br, l] : cellsIn) br->temp[l] += q / MATS[LIION].heatCap;
      for (size_t k = 0; k < b->vol.bricks.size(); k++) if (b->vol.bricks[k]) b->heat.add((int)k);
    }
    float per = 1000.f * dt * warp / std::max<size_t>(1, sh.cells.size());
    for (auto& c : sh.cells) {
      Brick* br = v.inb(c.x, c.y, c.z) ? v.brick(c.x, c.y, c.z) : nullptr;
      if (!br) continue;
      int l = lidx(c.x, c.y, c.z);
      if (!br->mat[l]) continue;
      br->temp[l] += per / MATS[br->mat[l]].heatCap;
      if (b) b->heat.add(v.bindex(c.x, c.y, c.z)); else worldHot.add(v.bindex(c.x, c.y, c.z));
      if (rng().f() < 0.002f) emit(FX_SPARK, b ? b->toWorld(vec3(c.x + .5f, c.y + .5f, c.z + .5f), vs) : cellCenter(c.x, c.y, c.z), rng().unitVec() * 2.f, 0.3f, vs * 0.4f, vec3(3, 2.5f, 1.5f));
    }
  }

  // Device effects on every volume.
  auto run = [&](Volume& v, Body* b) {
    // Gather per-component data lazily: walk voxels of device components only.
    v.forEach([&](int x, int y, int z, Brick* br, int l) {
      uint8_t m = br->mat[l];
      if (m != FILAMENT && m != NICHROME && m != MOTOR && m != CHARGE && m != PROPELLANT && m != SOLAR) return;
      uint32_t cid = br->comp[l];
      if (!cid) return;
      Component& c = comp(cid);
      bool on = c.powered;
      uint8_t before = br->flags[l];
      if (m == FILAMENT) {
        if (on) br->flags[l] |= F_LIT; else br->flags[l] &= ~F_LIT;
        if (on) br->temp[l] += 36.f * c.powerFactor * dt * warp / 6.f / MATS[m].heatCap;
      } else if (m == NICHROME && on) {
        br->temp[l] += 1500.f * c.powerFactor * dt * warp / 24.f / MATS[m].heatCap;
      } else if (m == MOTOR && on) {
        br->temp[l] += 20.f * dt * warp / 40.f / MATS[m].heatCap;
      } else if (m == CHARGE && on) {
        br->temp[l] = 900;  // detonator fires
      } else if (m == PROPELLANT && on && !c.lit && c.fuel > 0) {
        c.lit = true;
        event(EV_IGNITE, b ? b->pos : cellCenter(x, y, z), 1, m, cid);
      }
      if (m == PROPELLANT && c.lit) br->flags[l] |= F_BURN;
      if (br->flags[l] != before || (m == FILAMENT && on)) v.markDirty(x, y, z);
      if (on || c.lit) { if (b) b->heat.add(v.bindex(x, y, z)); else worldHot.add(v.bindex(x, y, z)); }
    });
  };
  // World devices are rare; only walk the specials.
  for (int64_t k : worldSpecials) {
    ivec3 p = unpackKey(k);
    Brick* br = world.inb(p.x, p.y, p.z) ? world.brick(p.x, p.y, p.z) : nullptr;
    if (!br) continue;
    int l = lidx(p.x, p.y, p.z);
    uint8_t m = br->mat[l];
    uint32_t cid = br->comp[l];
    if (!cid) continue;
    Component& c = comp(cid);
    uint8_t before = br->flags[l];
    if (m == FILAMENT) { if (c.powered) br->flags[l] |= F_LIT; else br->flags[l] &= ~F_LIT; }
    if (m == NICHROME && c.powered) { br->temp[l] += 1500.f * c.powerFactor * dt * warp / 24.f / MATS[m].heatCap; worldHot.add(world.bindex(p.x, p.y, p.z)); }
    if (m == CHARGE && c.powered) { br->temp[l] = 900; worldHot.add(world.bindex(p.x, p.y, p.z)); }
    if (br->flags[l] != before) world.markDirty(p.x, p.y, p.z);
  }
  for (auto& bp : bodies) {
    Body& b = *bp;
    bool dev = false;
    for (uint32_t c : b.compIds) { CompType t = comp(c).type; if (t != C_PART && t != C_WHEEL) { dev = true; break; } }
    if (dev) run(b.vol, &b);
  }

  // Solar output: fraction of cells with open sky above them.
  for (auto& bp : bodies) {
    Body& b = *bp;
    for (uint32_t cid : b.compIds) {
      Component& c = comp(cid);
      if (c.type != C_SOLAR) continue;
      vec3 up = b.rot.rotate(vec3(0, 1, 0));
      float sunDot = std::max(0.f, dot(up, sunDir));
      Hit h = raycast(b.pos + up * (b.radius + vs), sunDir, 20.f, bodyIndex(&b));
      c.capacity = h.hit ? 3.f : 30.f * sunDot;
      c.charge = 1;  // never "empty"; output is what's limited
    }
  }

  // Motors -> wheels and propellers; rockets.
  for (auto& c : comps()) { if (c.type == C_WHEEL) { c.driveForce = 0; c.driveSpeed = 0; } }
  for (auto& [mid, links] : g_links) {
    Component& M = comp(mid);
    if (!M.powered) continue;
    float pf = M.powerFactor;
    for (uint32_t w : links) {
      Component& W = comp(w);
      float sign = (dot(normalize(W.axis), normalize(M.axis)) >= 0 ? 1.f : -1.f) * (M.reversed ? -1.f : 1.f);
      if (W.type == C_WHEEL) {
        // DC motor: torque is highest at stall and falls with speed (F = P / v, capped).
        float v = std::fabs(W.spinRate) * 0.12f;
        float total = std::min(360.f, 200.f * pf / std::max(0.35f, v));
        W.driveSpeed = 2.6f * pf * sign;
        W.driveForce = total / std::max<size_t>(1, links.size());
      } else if (W.type == C_PROP) {
        W.driveForce = 28.f * pf * sign;
      }
    }
  }
  for (auto& bp : bodies) {
    Body& b = *bp;
    for (const RotPart& rp : b.rot_parts) {
      Component& c = comp(rp.comp);
      if (c.type == C_WHEEL) {
        if (c.driveForce > 0) b.wake();  // a motor is pushing: don't doze off
        // Visual spin from the hub's actual motion (or the motor's push if spinning in the air).
        vec3 hub = b.toWorld(rp.pivot, vs);
        vec3 axisW = b.rot.rotate(rp.axis);
        vec3 fwd = normalize(cross(axisW, vec3(0, 1, 0)) * -1.f);
        float vf = dot(b.pointVel(hub), fwd);
        float target = c.driveForce > 0 && b.asleep == false ? c.driveSpeed : vf;
        if (std::fabs(vf) > 0.05f) target = vf;
        c.spinRate = target / std::max(0.02f, rp.radius);
      } else if (c.type == C_PROP) {
        float target = std::fabs(c.driveForce) > 0 ? 70.f * (c.driveForce > 0 ? 1 : -1) : 0;
        c.spinRate += (target - c.spinRate) * std::min(1.f, dt * 2.f);
        if (std::fabs(c.driveForce) > 0) {
          vec3 axisW = b.rot.rotate(rp.axis);
          vec3 at = b.toWorld(rp.pivot, vs);
          b.force += axisW * c.driveForce;
          b.torque += cross(at - b.pos, axisW * c.driveForce);
          b.wake();
          // Airflow: push light things in front of the prop and fan any flames.
          vec3 flow = axisW * (c.driveForce > 0 ? -1.f : 1.f);
          for (auto& ob : bodies) {
            if (ob.get() == &b) continue;
            vec3 d = ob->pos - at;
            float along = dot(d, flow);
            if (along <= 0 || along > 2.5f) continue;
            float off = length(d - flow * along);
            if (off > rp.radius * 1.5f + ob->radius * 0.5f) continue;
            float f = std::fabs(c.driveForce) * 0.6f / (1 + along * 2);
            ob->force += flow * f;
            ob->wake();
          }
          for (auto& f : fx) {
            vec3 d = f.p - at;
            float along = dot(d, flow);
            if (along > 0 && along < 2.5f && length(d - flow * along) < rp.radius * 1.5f) f.v += flow * dt * 6.f;
          }
        }
      }
      c.spin += c.spinRate * dt;
    }
    // Rockets: thrust along the motor axis while fuel lasts.
    for (uint32_t cid : b.compIds) {
      Component& c = comp(cid);
      if (c.type != C_ROCKET || !c.lit) continue;
      c.fuel -= dt;
      vec3 axisW = b.rot.rotate(normalize(c.axis));
      b.force += axisW * 160.f;
      b.wake();
      // Exhaust from the nozzle end.
      vec3 tail = b.pos - axisW * (b.radius * 0.6f);
      for (int i = 0; i < 6; i++)
        emit(FX_FIRE, tail + rng().unitVec() * vs, axisW * -rng().range(4, 9) + rng().unitVec() * 0.6f, rng().range(0.15f, 0.35f), vs * rng().range(2, 4), vec3(1.4f, 1.1f, 0.8f));
      emit(FX_SMOKE, tail, axisW * -2.f + rng().unitVec() * 0.4f, rng().range(2, 4), vs * 6, vec3(0.7f));
      // The exhaust heats whatever it hits.
      Hit h = raycast(tail, axisW * -1.f, 0.8f, bodyIndex(&b));
      if (h.hit) addHeat(h, 9000.f, dt, 2.5f);
      if (c.fuel <= 0) {
        c.lit = false;
        std::vector<ivec3> burnt;
        b.vol.forEach([&](int x, int y, int z, Brick* br, int l) { if (br->comp[l] == cid && br->mat[l] == PROPELLANT) burnt.push_back({x, y, z}); });
        for (auto& p : burnt) {
          Cell cc = b.vol.get(p.x, p.y, p.z);
          cc.mat = ASH; cc.flags = 0;
          b.vol.put(p.x, p.y, p.z, cc);
        }
        b.shapeDirty = true;
      }
    }
  }

  // Charges with a lit fuse.
  for (size_t i = 0; i < comps().size(); i++) {
    Component& c = comps()[i];
    if (c.type != C_CHARGE || c.fuse < 0) continue;
    c.fuse -= dt;
    if (c.fuse <= 0) {
      c.fuse = -1;
      for (auto& bp : bodies) {
        Body& b = *bp;
        if (std::find(b.compIds.begin(), b.compIds.end(), (uint32_t)i) == b.compIds.end()) continue;
        b.vol.forEach([&](int, int, int, Brick* br, int l) { if (br->comp[l] == i && br->mat[l] == CHARGE) br->temp[l] = 900; });
        b.heat.reset(b.vol.bricks.size());
        for (size_t k = 0; k < b.vol.bricks.size(); k++) if (b.vol.bricks[k]) b.heat.add((int)k);
      }
    }
  }
}
