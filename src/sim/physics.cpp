// Rigid-body dynamics: sphere-sampled voxel bodies against the voxel world and
// each other, solved with sequential impulses. Impacts deliver energy to the
// voxels involved (heat, damage, parts knocked loose); sliding friction heats
// both surfaces; hot bodies conduct heat into whatever they rest on.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "../core/jobs.h"
#include "sim.h"

namespace {

struct Contact {
  Body* a;
  Body* b;              // null = world
  vec3 p, n;            // n points from b (or world) towards a
  float depth;
  vec3 ra, rb;
  float kn = 0, target = 0, jn = 0;
  vec3 jt;
  float mu = 0.6f;
  float vn0 = 0;
  ivec3 cellA, cellB;   // voxels (cellB in world or body b)
  uint8_t matA, matB;
  uint32_t compA, compB;
  uint32_t wheel = 0;   // comp id if a's sample is on a wheel
  vec3 rollDir, axleDir;
};

struct DamageReq {
  int bodyId;           // 0 = world
  ivec3 cell;
  float energy;
  vec3 dir, p;
  uint32_t comp;
};

inline vec3 mulI(const mat3& I, const vec3& v) { return I * v; }

float effMass(const Body* a, const Body* b, const vec3& ra, const vec3& rb, const vec3& n) {
  float k = a->invMass + dot(n, cross(mulI(a->invIworld, cross(ra, n)), ra));
  if (b) k += b->invMass + dot(n, cross(mulI(b->invIworld, cross(rb, n)), rb));
  return k;
}

}  // namespace

void Sim::stepPhysics(float dtFrame) {
  if (bodies.empty()) return;
  int awakeCount = 0;
  for (auto& b : bodies) if (!b->asleep && !b->frozen && !b->ghost) awakeCount++;
  const int SUB = awakeCount > 120 ? 2 : awakeCount > 40 ? 3 : 4;
  const float h = dtFrame / SUB;
  const float slop = vs * 0.15f;
  std::vector<Contact> contacts;
  std::vector<DamageReq> damage;
  std::vector<std::vector<Contact>> perBody;
  int awake = 0;
  for (auto& b : bodies) if (!b->asleep && !b->frozen) awake++;
  stats.awake = awake;
  if (awake == 0) return;

  for (int sub = 0; sub < SUB; sub++) {
    // ---- forces
    for (auto& bp : bodies) {
      Body& b = *bp;
      if (b.asleep || b.frozen || b.invMass == 0) continue;
      b.updateInertiaWorld();
      b.vel += (gravity + b.force * b.invMass) * h;
      b.angVel += b.invIworld * b.torque * h;
      // Buoyancy and drag from water the body is sitting in.
      int wet = 0;
      vec3 wetCentre;
      mat3 R = b.R();
      for (const Sample& s : b.samples) {
        vec3 wp = b.pos + R * s.local;
        ivec3 c = cellOf(wp);
        uint8_t m = world.matI(c);
        if (m == WATER || m == MOLTEN) { wet++; wetCentre += wp; }
      }
      b.submerged = b.samples.empty() ? 0 : (float)wet / b.samples.size();
      if (wet) {
        // Displaced volume ~ voxel count times wet fraction.
        float vol = b.voxels * vs * vs * vs * b.submerged;
        vec3 fb = vec3(0, 1000.f * 9.81f * vol, 0);
        wetCentre = wetCentre / (float)wet;
        b.vel += fb * b.invMass * h;
        b.angVel += b.invIworld * cross(wetCentre - b.pos, fb) * h;
        float drag = 1.f - std::min(0.9f, 2.5f * b.submerged * h);
        b.vel *= drag;
        b.angVel *= drag;
      }
      b.vel *= (1.f - 0.01f * h);
      b.angVel *= (1.f - 0.04f * h);
    }

    // ---- world contacts (parallel per body)
    perBody.assign(bodies.size(), {});
    Jobs::get().parallelFor((int)bodies.size(), 1, [&](int beg, int end, int) {
      for (int bi = beg; bi < end; bi++) {
        Body& b = *bodies[bi];
        if (b.asleep || b.frozen) continue;
        auto& out = perBody[bi];
        mat3 R = b.R();
        for (const Sample& s : b.samples) {
          vec3 c = b.pos + R * s.local;
          float r = s.r;
          ivec3 lo = cellOf(c - vec3(r)), hi = cellOf(c + vec3(r));
          vec3 nsum;
          float maxPen = 0;
          ivec3 deep;
          uint8_t deepMat = 0;
          for (int y = lo.y; y <= hi.y; y++)
            for (int z = lo.z; z <= hi.z; z++)
              for (int x = lo.x; x <= hi.x; x++) {
                uint8_t m = world.mat(x, y, z);
                if (m == AIR || MATS[m].phase == LIQUID || MATS[m].phase == GAS) continue;
                vec3 bmin(x * vs, y * vs, z * vs), bmax = bmin + vec3(vs);
                vec3 q = vmax(bmin, vmin(c, bmax));
                vec3 d = c - q;
                float dist = length(d);
                float pen;
                vec3 n;
                if (dist > 1e-6f) {
                  if (dist >= r) continue;
                  n = d / dist; pen = r - dist;
                } else {
                  vec3 cc = (bmin + bmax) * 0.5f;
                  n = normalize(c - cc + vec3(0, 1e-4f, 0));
                  pen = r + vs * 0.5f;
                }
                nsum += n * pen;
                if (pen > maxPen) { maxPen = pen; deep = {x, y, z}; deepMat = m; }
              }
          if (maxPen <= 0) continue;
          Contact ct;
          ct.a = &b; ct.b = nullptr;
          ct.n = normalize(nsum);
          ct.depth = std::min(maxPen, r);
          ct.p = c - ct.n * r;
          ct.ra = ct.p - b.pos;
          ct.cellA = s.cell; ct.cellB = deep;
          ct.matA = s.mat; ct.matB = deepMat;
          ct.compA = s.comp; ct.compB = world.comp(deep.x, deep.y, deep.z);
          ct.mu = std::sqrt(MATS[s.mat].friction * MATS[deepMat].friction);
          if (s.comp && comp(s.comp).type == C_WHEEL) ct.wheel = s.comp;
          out.push_back(ct);
        }
      }
    });
    contacts.clear();
    for (auto& v : perBody) contacts.insert(contacts.end(), v.begin(), v.end());

    // ---- body-body contacts: sweep-and-prune on bounding boxes, then
    // sample-vs-sample tests for each overlapping pair.
    {
      const size_t n = bodies.size();
      struct Box { float lo[3], hi[3]; int i; };
      std::vector<Box> boxes;
      boxes.reserve(n);
      for (size_t i = 0; i < n; i++) {
        Body& b = *bodies[i];
        if (b.ghost || b.samples.empty()) continue;
        float r = b.radius + vs;
        boxes.push_back({{b.pos.x - r, b.pos.y - r, b.pos.z - r}, {b.pos.x + r, b.pos.y + r, b.pos.z + r}, (int)i});
      }
      std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.lo[0] < b.lo[0]; });
      std::vector<std::pair<int, int>> pairs;
      for (size_t a = 0; a < boxes.size(); a++)
        for (size_t c = a + 1; c < boxes.size() && boxes[c].lo[0] <= boxes[a].hi[0]; c++) {
          const Box& A = boxes[a];
          const Box& C = boxes[c];
          if (A.lo[1] > C.hi[1] || C.lo[1] > A.hi[1] || A.lo[2] > C.hi[2] || C.lo[2] > A.hi[2]) continue;
          Body& ba = *bodies[A.i];
          Body& bc = *bodies[C.i];
          bool aw = !ba.asleep && !ba.frozen, cw = !bc.asleep && !bc.frozen;
          if (!aw && !cw) continue;
          // The awake one is "A" in the contact (both awake: either).
          if (aw) pairs.push_back({A.i, C.i}); else pairs.push_back({C.i, A.i});
        }
      // World positions of samples, once per body.
      std::vector<std::vector<vec3>> wpos(n);
      std::vector<char> need(n, 0);
      for (auto& pr : pairs) { need[pr.first] = 1; need[pr.second] = 1; }
      for (size_t i = 0; i < n; i++) {
        if (!need[i]) continue;
        Body& b = *bodies[i];
        mat3 R = b.R();
        wpos[i].resize(b.samples.size());
        for (size_t k = 0; k < b.samples.size(); k++) wpos[i][k] = b.pos + R * b.samples[k].local;
      }
      std::vector<std::vector<Contact>> perPair(pairs.size());
      Jobs::get().parallelFor((int)pairs.size(), 4, [&](int b0, int b1, int) {
        for (int pi = b0; pi < b1; pi++) {
          Body& A = *bodies[pairs[pi].first];
          Body& B = *bodies[pairs[pi].second];
          const auto& pa = wpos[pairs[pi].first];
          const auto& pb = wpos[pairs[pi].second];
          vec3 bc = B.pos;
          float br = B.radius + vs * 3;
          for (size_t k = 0; k < A.samples.size(); k++) {
            const Sample& sa = A.samples[k];
            if (length2(pa[k] - bc) > (br + sa.r) * (br + sa.r)) continue;
            for (size_t kk = 0; kk < B.samples.size(); kk++) {
              const Sample& sb = B.samples[kk];
              vec3 d = pa[k] - pb[kk];
              float rr = sa.r + sb.r;
              float d2 = length2(d);
              if (d2 >= rr * rr || d2 < 1e-12f) continue;
              float dist = std::sqrt(d2);
              Contact ct;
              ct.a = &A; ct.b = &B;
              ct.n = d / dist;
              ct.depth = rr - dist;
              ct.p = pa[k] - ct.n * sa.r;
              ct.ra = ct.p - A.pos; ct.rb = ct.p - B.pos;
              ct.cellA = sa.cell; ct.cellB = sb.cell;
              ct.matA = sa.mat; ct.matB = sb.mat;
              ct.compA = sa.comp; ct.compB = sb.comp;
              ct.mu = std::sqrt(MATS[sa.mat].friction * MATS[sb.mat].friction);
              if (sa.comp && comp(sa.comp).type == C_WHEEL) ct.wheel = sa.comp;
              perPair[pi].push_back(ct);
            }
          }
        }
      });
      for (size_t pi = 0; pi < pairs.size(); pi++) {
        if (perPair[pi].empty()) continue;
        Body& A = *bodies[pairs[pi].first];
        Body& B = *bodies[pairs[pi].second];
        // A moving body pushes a sleeping one awake.
        if (B.asleep && length2(A.vel) + length2(A.angVel) * 0.05f > 0.02f) B.wake();
        contacts.insert(contacts.end(), perPair[pi].begin(), perPair[pi].end());
      }
    }
    stats.contacts = (int)contacts.size();
    if (getenv("REALISM_DEBUG_CONTACTS") && sub == 0) {
      for (auto& c : contacts) fprintf(stderr, "  contact body %d vs %s mat %s/%s comp=%s wheel=%u depth=%.4f p=%.3f %.3f %.3f cellA=%d %d %d\n", c.a->id, c.b ? "body" : "world", MATS[c.matA].name, MATS[c.matB].name, comp(c.compA).name.c_str(), c.wheel, c.depth, c.p.x, c.p.y, c.p.z, c.cellA.x, c.cellA.y, c.cellA.z);
    }

    // ---- prepare
    for (auto& c : contacts) {
      if (c.b && (c.b->frozen || c.b->asleep)) {
        // Treat sleeping/frozen partners as static this substep.
        c.b->updateInertiaWorld();
      }
      Body* B = (c.b && !c.b->asleep && !c.b->frozen) ? c.b : nullptr;
      vec3 va = c.a->pointVel(c.p);
      vec3 vb = c.b ? c.b->pointVel(c.p) : vec3(0);
      c.vn0 = dot(va - vb, c.n);
      c.kn = effMass(c.a, B, c.ra, c.rb, c.n);
      float e = 0;
      if (c.vn0 < -0.8f) e = std::max(MATS[c.matA].restitution, MATS[c.matB].restitution);
      float bias = 0.25f / h * std::max(0.f, c.depth - slop);
      c.target = std::max(-e * c.vn0, std::min(bias, 0.8f));
      if (c.wheel) {
        Component& w = comp(c.wheel);
        vec3 axisW = c.a->rot.rotate(normalize(w.axis));
        c.axleDir = normalize(axisW - c.n * dot(axisW, c.n));
        c.rollDir = normalize(cross(c.axleDir, c.n));
        w.driveContacts++;
      }
      // Impact energy (relative approach speed along the normal).
      if (c.vn0 < -2.0f) {
        float E = 0.5f * c.vn0 * c.vn0 / c.kn;
        damage.push_back({c.a->id, c.cellA, E, c.n * -1.f, c.p, c.compA});
        damage.push_back({c.b ? c.b->id : 0, c.cellB, E, c.n, c.p, c.compB});
      }
    }

    // ---- solve
    const int iters = contacts.size() > 6000 ? 5 : 8;
    for (int it = 0; it < iters; it++) {
      for (auto& c : contacts) {
        Body* A = c.a;
        Body* B = (c.b && !c.b->asleep && !c.b->frozen) ? c.b : nullptr;
        vec3 vrel = A->pointVel(c.p) - (c.b ? c.b->pointVel(c.p) : vec3(0));
        float vn = dot(vrel, c.n);
        float dl = (c.target - vn) / c.kn;
        float jn = std::max(c.jn + dl, 0.f);
        dl = jn - c.jn;
        c.jn = jn;
        vec3 J = c.n * dl;
        A->applyImpulse(J, c.p);
        if (B) B->applyImpulse(J * -1.f, c.p);

        // Friction.
        vrel = A->pointVel(c.p) - (c.b ? c.b->pointVel(c.p) : vec3(0));
        vec3 vt = vrel - c.n * dot(vrel, c.n);
        float maxF = c.mu * c.jn;
        if (c.wheel) {
          Component& w = comp(c.wheel);
          // Sideways: full grip. Along the rolling direction: the motor's push
          // (if driven) or a little rolling resistance. Impulses accumulate in
          // jt.x (axle) and jt.y (roll) so the limits hold across iterations.
          float va = dot(vt, c.axleDir);
          float ka = effMass(A, B, c.ra, c.rb, c.axleDir);
          float na = std::clamp(c.jt.x - va / ka, -maxF, maxF);
          float ja = na - c.jt.x;
          c.jt.x = na;
          vec3 Ja = c.axleDir * ja;
          float vr = dot(vt, c.rollDir);
          float kr = effMass(A, B, c.ra, c.rb, c.rollDir);
          float nr;
          if (w.driveForce > 0) {
            float lim = std::min(maxF, w.driveForce * h / std::max(1, w.driveContacts));
            nr = std::clamp(c.jt.y + (w.driveSpeed - vr) / kr, -lim, lim);
          } else {
            float rr = 0.015f * c.jn;
            nr = std::clamp(c.jt.y - vr / kr, -rr, rr);
          }
          float jr = nr - c.jt.y;
          c.jt.y = nr;
          vec3 Jt = Ja + c.rollDir * jr;
          A->applyImpulse(Jt, c.p);
          if (B) B->applyImpulse(Jt * -1.f, c.p);
          continue;
        }
        float vtl = length(vt);
        if (vtl < 1e-6f) continue;
        vec3 t = vt / vtl;
        float kt = effMass(A, B, c.ra, c.rb, t);
        vec3 jtNew = c.jt - t * (vtl / kt);
        float l = length(jtNew);
        if (l > maxF) jtNew = jtNew * (maxF / l);
        vec3 dJ = jtNew - c.jt;
        c.jt = jtNew;
        A->applyImpulse(dJ, c.p);
        if (B) B->applyImpulse(dJ * -1.f, c.p);
      }
    }

    // ---- energy into heat: sliding friction and contact conduction
    for (auto& c : contacts) {
      vec3 vrel = c.a->pointVel(c.p) - (c.b ? c.b->pointVel(c.p) : vec3(0));
      float slide = length(vrel - c.n * dot(vrel, c.n));
      float work = length(c.jt) * slide;  // J this substep
      Volume& va = c.a->vol;
      Volume* vb = c.b ? &c.b->vol : &world;
      Brick* ba = va.inb(c.cellA.x, c.cellA.y, c.cellA.z) ? va.brick(c.cellA.x, c.cellA.y, c.cellA.z) : nullptr;
      Brick* bb = vb->inb(c.cellB.x, c.cellB.y, c.cellB.z) ? vb->brick(c.cellB.x, c.cellB.y, c.cellB.z) : nullptr;
      if (!ba || !bb) continue;
      int la = lidx(c.cellA.x, c.cellA.y, c.cellA.z), lb = lidx(c.cellB.x, c.cellB.y, c.cellB.z);
      if (!ba->mat[la] || !bb->mat[lb]) continue;
      float Ca = MATS[ba->mat[la]].heatCap, Cb = MATS[bb->mat[lb]].heatCap;
      bool changed = false;
      if (work > 1e-4f) {
        ba->temp[la] += 0.5f * work / Ca;
        bb->temp[lb] += 0.5f * work / Cb;
        changed = work > 0.05f;
      }
      float dT = ba->temp[la] - bb->temp[lb];
      if (std::fabs(dT) > 2.f) {
        float ka = MATS[ba->mat[la]].k, kb = MATS[bb->mat[lb]].k;
        float G = 2 * ka * kb / (ka + kb) * vs * 0.5f;
        float Q = G * dT * h * cfg.thermalWarp;
        Q = std::clamp(Q, -0.4f * std::fabs(dT) * std::min(Ca, Cb), 0.4f * std::fabs(dT) * std::min(Ca, Cb));
        ba->temp[la] -= Q / Ca;
        bb->temp[lb] += Q / Cb;
        changed = true;
      }
      if (changed) {
        c.a->heat.add(va.bindex(c.cellA.x, c.cellA.y, c.cellA.z));
        if (c.b) c.b->heat.add(vb->bindex(c.cellB.x, c.cellB.y, c.cellB.z));
        else worldHot.add(world.bindex(c.cellB.x, c.cellB.y, c.cellB.z));
      }
    }

    // ---- integrate
    for (auto& bp : bodies) {
      Body& b = *bp;
      if (b.asleep || b.frozen || b.invMass == 0) continue;
      float sp = length(b.vel);
      if (sp > 60) b.vel = b.vel * (60 / sp);
      float wl = length(b.angVel);
      if (wl > 80) b.angVel = b.angVel * (80 / wl);
      b.pos += b.vel * h;
      b.rot = b.rot.integrated(b.angVel, h);
    }
    for (auto& c : contacts) if (c.wheel) comp(c.wheel).driveContacts = 0;
  }

  // ---- sleep, out-of-world cleanup
  for (int i = (int)bodies.size() - 1; i >= 0; i--) {
    Body& b = *bodies[i];
    b.force = vec3(0); b.torque = vec3(0);
    if (b.asleep || b.frozen) continue;
    if (b.pos.y < -4) { removeBody(i); continue; }
    // Rest detection: slow for a while -> sleep. Resting rubble jitters a
    // little under sequential impulses, so the threshold allows ~10 cm/s.
    float e = length2(b.vel) + length2(b.angVel) * 0.05f;
    bool driven = false;
    for (const RotPart& rp : b.rot_parts) if (comp(rp.comp).driveForce != 0) driven = true;
    if (e < 0.012f && !b.held && !driven) {
      b.sleepTimer += dtFrame;
      // Bleed off the jitter while it decides.
      b.vel *= 0.92f;
      b.angVel *= 0.88f;
      if (b.sleepTimer > 0.45f) { b.asleep = true; b.vel = vec3(0); b.angVel = vec3(0); }
    } else {
      b.sleepTimer = std::max(0.f, b.sleepTimer - dtFrame * 2);
    }
  }

  // ---- impacts: heat, sound, damage, parts knocked loose
  std::sort(damage.begin(), damage.end(), [](const DamageReq& a, const DamageReq& b) { return a.energy > b.energy; });
  int processed = 0;
  for (auto& d : damage) {
    if (processed++ > 24) break;
    Hit h;
    h.hit = true;
    h.cell = d.cell;
    h.point = d.p;
    h.normal = d.dir * -1.f;
    if (d.bodyId == 0) {
      h.body = -1;
      if (world.matI(d.cell) == AIR) continue;
    } else {
      Body* b = bodyById(d.bodyId);
      if (!b || b->vol.matI(d.cell) == AIR) continue;
      h.body = bodyIndex(b);
    }
    if (d.energy > 3) event(EV_IMPACT, d.p, d.energy, h.body < 0 ? world.matI(d.cell) : bodies[h.body]->vol.matI(d.cell), d.comp);
    impact(h, d.energy * 0.5f, d.dir, true);
  }
}
