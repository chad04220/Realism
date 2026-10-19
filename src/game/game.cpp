#include "game.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include "../core/jobs.h"
#include "../gfx/gl.h"
#include "assemblies.h"
#include "workshop.h"

const ToolInfo TOOLS[T_COUNT] = {
    {"Hand", "Grab & carry", "Throw / shove", ui::I_HAND},
    {"Hammer", "Hold to charge, release to strike", "Light tap", ui::I_HAMMER},
    {"Wrench", "Unbolt part (it drops free)", "Salvage part to workshop", ui::I_WRENCH},
    {"Chisel", "Carve a chunk", "Pick out one voxel", ui::I_CHISEL},
    {"Blowtorch", "Heat (12 kW flame)", "CO\xE2\x82\x82 cooler", ui::I_TORCH},
    {"Hose", "Spray water", "Wide mist", ui::I_HOSE},
    {"Charges", "Throw a lit stick (3 s fuse)", "Stick a remote charge \xC2\xB7 G detonates", ui::I_BOMB},
    {"Builder", "Place material", "Remove block", ui::I_BUILD},
    {"Scanner", "Deep inspect", "Pin readout", ui::I_SCAN},
};

const uint8_t BUILD_PALETTE[] = {STEEL, ALU, OAK, PINE, BRICK, CONCRETE, STONE, GLASS, COPPER, RUBBER, PLASTIC,
                                 IRON, BRASS, CHARCOAL, SAND, WATER, DIRT, CANVAS, ICE, CHARGE, NICHROME, SHAFT, CARBON};
const int BUILD_PALETTE_N = sizeof(BUILD_PALETTE);

Game::Game() = default;
Game::~Game() = default;

float Game::uiScale() const { return std::max(0.75f, plat::height() / 1080.f) * settings.uiScale; }

void Game::toast(const std::string& s, ui::Color c, int icon) {
  for (auto& t : toasts_) if (t.text == s && t.age < t.life - 0.5f) { t.age = std::min(t.age, 0.4f); return; }
  Toast t;
  t.text = s; t.color = c; t.icon = icon;
  toasts_.push_front(t);
  if (toasts_.size() > 6) toasts_.pop_back();
}

Body* Game::spawnTemplate(const Template& t, const vec3& worldMin, const quat& rot) {
  Volume v(t.vol.nx, t.vol.ny, t.vol.nz);
  stamp(v, t, ivec3(0, 0, 0));
  Body* b = spawnBody(sim, std::move(v), worldMin);
  if (b && (rot.x != 0 || rot.y != 0 || rot.z != 0)) { b->rot = rot; b->updateInertiaWorld(); }
  return b;
}

bool Game::init(const Args& args) {
  std::string userCfg = plat::userDataDir() + "/settings.cfg";
  settings.load("settings.cfg");
  settings.load(userCfg);
  if (args.voxelSize > 0) settings.voxelSize = args.voxelSize;
  int w = args.w ? args.w : settings.width, h = args.h ? args.h : settings.height;
  int threads = settings.threads > 0 ? settings.threads : plat::cpuThreads();
  Jobs::get().start(std::max(1, threads));
  if (!plat::init("Realism", w, h, settings.fullscreen && !scripted_, settings.vsync && !scripted_)) return false;
  if (!glLoad()) {
    std::string m = std::string("OpenGL 4.3 function missing: ") + glMissing() + "\nPlease update your graphics driver.";
    plat::messageBox("Realism", m.c_str());
    return false;
  }
  fprintf(stderr, "GL: %s | %s | %s\n", (const char*)glGetString(GL_VENDOR), (const char*)glGetString(GL_RENDERER),
          (const char*)glGetString(GL_VERSION));
  // Loading screen while the world builds.
  glClearColor(0.07f, 0.08f, 0.1f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  plat::swap();

  SimSettings ss;
  ss.voxelSize = settings.voxelSize;
  ss.extent = vec3(settings.worldX, settings.worldY, settings.worldZ);
  ss.thermalWarp = settings.thermalWarp;
  sim.init(ss);
  initParts(sim.vs);
  if (!ui::init()) return false;
  {
    ui::begin(plat::width(), plat::height(), uiScale());
    float s = uiScale();
    ui::text(plat::width() * 0.5f, plat::height() * 0.5f - 40 * s, "REALISM", 54 * s, ui::theme::text, 1, 0.08f);
    char buf[128];
    snprintf(buf, sizeof buf, "Building a %.0f \xC3\x97 %.0f m world of %.1f cm voxels on %d threads\xE2\x80\xA6",
             settings.worldX, settings.worldZ, sim.vs * 100, threads);
    ui::text(plat::width() * 0.5f, plat::height() * 0.5f + 30 * s, buf, 18 * s, ui::theme::dim, 1);
    ui::end();
    plat::swap();
  }
  double t0 = plat::now();
  scene = buildScene(sim);
  sim.settleWater();
  sim.initAir();
  // Let everything settle onto its supports, then put it to sleep.
  for (int i = 0; i < 90; i++) sim.step(1.f / 60.f);
  for (auto& b : sim.bodies) { b->vel = vec3(0); b->angVel = vec3(0); b->asleep = true; }
  sim.events.clear();
  fprintf(stderr, "World built in %.2f s: %zu voxels, %zu MB, %zu bodies\n", plat::now() - t0, sim.world.countSolid(),
          sim.world.memoryBytes() >> 20, sim.bodies.size());
  if (!ren.init(&sim)) {
    plat::messageBox("Realism", ("Shader compile failed:\n" + Shader::lastError).c_str());
    return false;
  }
  ren.groundY = scene.groundY - 0.005f;
  ren.uploadAllWorld();
  ropt.smooth = settings.smooth;
  ropt.shadows = settings.shadows;
  ropt.bloom = settings.bloom;
  ropt.volumetrics = settings.volumetrics;
  ropt.renderScale = settings.renderScale;
  ropt.exposure = settings.exposure;
  cam.fov = settings.fov * PI / 180.f;
  audio.init(settings.volume);
  workshop = std::make_unique<Workshop>(*this);
  workshop->createJig();
  buildViewmodels();
  pl_.pos = scene.spawn;
  pl_.yaw = scene.spawnYaw;
  pl_.pitch = -0.12f;
  lastTime_ = plat::now();
  toast("Welcome to Realism \xE2\x80\x94 press F1 for controls", ui::theme::accent, ui::I_GEAR);
  toast("Everything here is built from real parts. Take it apart.", ui::theme::text);
  return true;
}

int Game::run(const Args& args) {
  scripted_ = !args.script.empty() || args.selftest;
  if (!init(args)) return 1;
  if (args.selftest) {
    bool ok = selftest();
    plat::shutdown();
    Jobs::get().stop();
    return ok ? 0 : 2;
  }
  if (!args.script.empty()) {
    std::ifstream f(args.script);
    std::string line;
    while (std::getline(f, line)) script_.push_back(line);
  }
  int frame = 0;
  while (plat::pump() && !quit_) {
    double now = plat::now();
    float dt = (float)(now - lastTime_);
    lastTime_ = now;
    if (scripted_) dt = 1.f / 60.f;
    dt = std::min(dt, 1.f / 20.f);
    if (scripted_) {
      float mx = scriptIn_.mx, my = scriptIn_.my;
      scriptIn_ = plat::Input{};
      scriptIn_.mx = mx; scriptIn_.my = my;
      while (scriptWait_ <= 0 && scriptPos_ < script_.size() && !quit_) runScriptLine(script_[scriptPos_++], quit_);
      if (scriptWait_ > 0) scriptWait_--;
      if (scriptPos_ >= script_.size() && scriptWait_ <= 0) quit_ = true;
    }
    frame++;
    this->frame(dt);
    plat::swap();
    if (!args.screenshot.empty() && args.frames > 0 && frame >= args.frames) { saveScreenshot(args.screenshot); break; }
  }
  settings.save(plat::userDataDir() + "/settings.cfg");
  audio.shutdown();
  plat::shutdown();
  Jobs::get().stop();
  return 0;
}

// ----------------------------------------------------------------------- frame

void Game::frame(float dt) {
  plat::Input& in = scripted_ ? scriptIn_ : plat::input();
  time_ += dt;
  fps_ = fps_ * 0.95f + (1.f / std::max(dt, 1e-4f)) * 0.05f;
  // UI mouse state.
  ui::Mouse& m = ui::mouse();
  m.x = in.mx; m.y = in.my;
  m.down = in.mdown[0]; m.pressed = in.mpressed[0]; m.released = in.mreleased[0]; m.rpressed = in.mpressed[1];
  m.wheel = in.wheel;
  m.active = !plat::captured();

  // Global keys.
  if (in.pressed[key::F1]) help_ = !help_;
  if (in.pressed[key::F2]) hud_ = !hud_;
  if (in.pressed[key::F3]) stats_ = !stats_;
  if (in.pressed[key::F11]) plat::toggleFullscreen();
  if (in.pressed[key::Escape]) {
    if (help_) help_ = false;
    else if (mode_ == M_WORKSHOP && !menu_) { mode_ = M_PLAY; workshop->leave(); }
    else { menu_ = !menu_; menuPage_ = "main"; }
  }
  bool typing = mode_ == M_WORKSHOP && workshop->typing();
  if (!menu_ && !typing) {
    if (in.pressed[key::Tab]) {
      if (mode_ == M_PLAY) { mode_ = M_WORKSHOP; workshop->enter(); audio.playUi(SFX_UI); }
      else { mode_ = M_PLAY; workshop->leave(); audio.playUi(SFX_UI); }
    }
    if (in.pressed['P']) { paused_ = !paused_; toast(paused_ ? "Simulation paused (P)" : "Simulation running", ui::theme::dim); }
    if (in.pressed['T']) {
      sim.timeScale = sim.timeScale < 1 ? 1.f : 0.2f;
      toast(sim.timeScale < 1 ? "Slow motion \xC3\x97" "0.2 (T)" : "Normal speed", ui::theme::accent);
    }
    if (in.pressed['V']) { ropt.smooth = !ropt.smooth; toast(ropt.smooth ? "Smooth surfaces" : "Blocky voxels", ui::theme::dim); }
  }
  bool wantCapture = mode_ == M_PLAY && !menu_ && !help_ && plat::focused();
  if (plat::captured() != wantCapture) plat::setCaptured(wantCapture);

  if (mode_ == M_PLAY) {
    if (!menu_) updatePlay(dt, in);
    vec3 eye = pl_.pos + vec3(0, pl_.eye + std::sin(pl_.bob) * 0.02f, 0);
    cam.pos = eye;
    cam.yaw = pl_.yaw;
    cam.pitch = pl_.pitch;
    if (shake_ > 0) {
      cam.yaw += (rng().f() - 0.5f) * shake_ * 0.03f;
      cam.pitch += (rng().f() - 0.5f) * shake_ * 0.03f;
      shake_ = std::max(0.f, shake_ - dt * 3);
    }
  } else if (!menu_) {
    workshop->update(dt, in, cam);
  }
  cam.update((float)plat::width() / std::max(1, plat::height()));
  if (mode_ == M_PLAY && !menu_) updateHover();
  updateViewmodel(dt);

  if (!paused_ && !menu_) sim.step(dt);
  if (mode_ == M_PLAY && !menu_) updateHover();  // bodies may have been added or removed
  handleEvents();
  audio.setListener(cam.pos, cam.right);
  flash_ = std::max(0.f, flash_ - dt * 3);

  ren.extraLights.clear();
  if (flashlight_ && mode_ == M_PLAY) ren.extraLights.push_back({vec4(cam.pos + cam.forward * 0.6f + cam.right * 0.15f, 9.f), vec4(2.4f, 2.3f, 2.0f, 0)});
  ren.render(cam, ropt, hover_, time_, flash_, plat::width(), plat::height());

  ui::begin(plat::width(), plat::height(), uiScale());
  if (mode_ == M_PLAY) { if (hud_) drawHud(dt); }
  else workshop->drawUI(dt);
  drawToasts(dt);
  if (stats_) drawStats();
  if (help_) drawHelp();
  if (menu_) drawMenu();
  ui::end();
}

// ----------------------------------------------------------------------- play

void Game::updatePlay(float dt, plat::Input& in) {
  float sens = 0.0022f * settings.sensitivity;
  pl_.yaw += in.dmx * sens;
  pl_.pitch -= in.dmy * sens * (settings.invertY ? -1.f : 1.f);
  pl_.pitch = clampf(pl_.pitch, -1.55f, 1.55f);
  if (in.pressed['N']) { pl_.fly = !pl_.fly; toast(pl_.fly ? "Fly mode on (N)" : "Fly mode off", ui::theme::dim); }
  if (in.pressed['F']) flashlight_ = !flashlight_;
  for (int k = 0; k < T_COUNT; k++) if (in.pressed['1' + k]) { if (tool_ != k) { tool_ = k; toolAnim_ = 0; audio.playUi(SFX_UI, 0.5f, 1.2f); } }
  bool holding = grabId_ != 0;
  if (in.wheel != 0 && !holding && !in.down[key::Shift]) {
    int d = in.wheel > 0 ? -1 : 1;
    tool_ = (tool_ + d + T_COUNT) % T_COUNT;
    toolAnim_ = 0;
    audio.playUi(SFX_UI, 0.4f, 1.3f);
  }
  if (tool_ == T_BUILD) {
    if (in.pressed['Q'] || (in.wheel != 0 && in.down[key::Shift])) {
      int d = in.pressed['Q'] ? 1 : (in.wheel > 0 ? -1 : 1);
      if (buildPart_ >= 0) buildPart_ = -1;
      buildMat_ = (buildMat_ + d + BUILD_PALETTE_N) % BUILD_PALETTE_N;
    }
    if (in.pressed['Z']) buildSize_ = buildSize_ == 1 ? 2 : buildSize_ == 2 ? 4 : 1;
    if (in.pressed['X'] && !salvage.empty()) buildPart_ = (buildPart_ + 1) % (int)salvage.size();
  }
  if (in.pressed['G']) {
    int n = 0;
    for (size_t i = 0; i < remoteCharges_.size(); i++) {
      Body* b = remoteChargeBodies_[i] ? sim.bodyById(remoteChargeBodies_[i]) : nullptr;
      if (remoteChargeBodies_[i] && !b) continue;
      detonateCharge(b ? sim.bodyIndex(b) : -1, remoteCharges_[i]);
      n++;
    }
    remoteCharges_.clear();
    remoteChargeBodies_.clear();
    if (n) toast("Detonating " + std::to_string(n) + (n == 1 ? " charge" : " charges"), ui::theme::warm, ui::I_BOMB);
    else toast("No remote charges placed (right-click with Charges)", ui::theme::dim, ui::I_BOMB);
  }
  movePlayer(dt, in);
  // Aim from where we are now (body indices from last frame may be stale).
  cam.pos = pl_.pos + vec3(0, pl_.eye, 0);
  cam.yaw = pl_.yaw;
  cam.pitch = pl_.pitch;
  cam.update((float)plat::width() / std::max(1, plat::height()));
  updateHover();
  if (in.pressed['E']) useKey();
  useTools(dt, in);
  toolAnim_ = std::min(1.f, toolAnim_ + dt * 6);
}

bool Game::playerCollides(const vec3& feet, float h) const {
  const float r = 0.25f;
  ivec3 lo = sim.cellOf(feet - vec3(r, 0, r)), hi = sim.cellOf(feet + vec3(r, h, r));
  for (int y = lo.y; y <= hi.y; y++)
    for (int z = lo.z; z <= hi.z; z++)
      for (int x = lo.x; x <= hi.x; x++) {
        uint8_t m = sim.world.mat(x, y, z);
        if (m != AIR && MATS[m].phase != LIQUID && MATS[m].phase != GAS && m != LEAVES) return true;
      }
  return false;
}

void Game::movePlayer(float dt, plat::Input& in) {
  float h = pl_.crouch ? 1.2f : 1.75f;
  pl_.crouch = in.down[key::Ctrl] && !pl_.fly;
  float eyeT = pl_.crouch ? 1.1f : 1.62f;
  pl_.eye += (eyeT - pl_.eye) * std::min(1.f, dt * 12);
  vec3 fwd(std::sin(pl_.yaw), 0, -std::cos(pl_.yaw)), right(std::cos(pl_.yaw), 0, std::sin(pl_.yaw));
  vec3 wish;
  if (in.down['W']) wish += fwd;
  if (in.down['S']) wish -= fwd;
  if (in.down['D']) wish += right;
  if (in.down['A']) wish -= right;
  if (length2(wish) > 0) wish = normalize(wish);
  bool sprint = in.down[key::Shift];
  if (pl_.fly) {
    float sp = sprint ? 9.f : 4.f;
    vec3 v = wish * sp;
    if (in.down[key::Space]) v.y += sp;
    if (in.down[key::Ctrl]) v.y -= sp;
    pl_.vel += (v - pl_.vel) * std::min(1.f, dt * 8);
    pl_.pos += pl_.vel * dt;
    pl_.onGround = false;
    return;
  }
  float maxSp = pl_.crouch ? 1.6f : sprint ? 6.5f : 3.8f;
  vec3 hv(pl_.vel.x, 0, pl_.vel.z);
  vec3 target = wish * maxSp;
  float accel = pl_.onGround ? 14.f : 2.5f;
  hv += (target - hv) * std::min(1.f, accel * dt);
  pl_.vel.x = hv.x; pl_.vel.z = hv.z;
  // Swimming: buoyant and slow in water.
  uint8_t atChest = sim.world.matI(sim.cellOf(pl_.pos + vec3(0, 0.9f, 0)));
  bool swim = atChest == WATER;
  if (swim) {
    pl_.vel *= (1 - std::min(1.f, dt * 3));
    pl_.vel.y += (in.down[key::Space] ? 4.f : -0.6f) * dt * 3;
  } else {
    pl_.vel.y -= 9.81f * dt;
  }
  if (in.pressed[key::Space] && pl_.onGround) { pl_.vel.y = 3.9f; pl_.onGround = false; }
  if (length(hv) > 0.5f && pl_.onGround) pl_.bob += dt * length(hv) * 2.2f;

  // Axis-separated moves in small steps; step up ledges up to 32 cm.
  float dist = length(pl_.vel) * dt;
  int steps = std::max(1, (int)std::ceil(dist / (sim.vs * 0.8f)));
  float sdt = dt / steps;
  bool grounded = false;
  for (int s = 0; s < steps; s++) {
    for (int axis : {0, 2, 1}) {
      vec3 np = pl_.pos;
      np[axis] += pl_.vel[axis] * sdt;
      if (!playerCollides(np, h)) { pl_.pos = np; continue; }
      if (axis != 1) {
        // Try stepping up.
        bool stepped = false;
        if (pl_.onGround || grounded) {
          for (float up = sim.vs; up <= 0.33f; up += sim.vs) {
            vec3 sp = np + vec3(0, up, 0);
            if (!playerCollides(sp, h)) { pl_.pos = sp; stepped = true; break; }
          }
        }
        if (!stepped) pl_.vel[axis] = 0;
      } else {
        if (pl_.vel.y < 0) grounded = true;
        pl_.vel.y = 0;
      }
    }
  }
  // Stand on and push rigid bodies.
  for (auto& bp : sim.bodies) {
    Body& b = *bp;
    if (b.ghost || length(vec3(b.pos.x - pl_.pos.x, 0, b.pos.z - pl_.pos.z)) > b.radius + 0.4f) continue;
    if (b.pos.y - b.radius > pl_.pos.y + h || b.pos.y + b.radius < pl_.pos.y) continue;
    mat3 R = b.R();
    for (const Sample& smp : b.samples) {
      vec3 p = b.pos + R * smp.local;
      float top = p.y + smp.r;
      if (p.y - smp.r > pl_.pos.y + h || top < pl_.pos.y) continue;
      vec3 d(pl_.pos.x - p.x, 0, pl_.pos.z - p.z);
      float dl = length(d);
      float pen = 0.25f + smp.r - dl;
      if (pen <= 0) continue;
      if (top - pl_.pos.y < 0.3f && pl_.vel.y <= 0.5f) {
        pl_.pos.y = top;
        pl_.vel.y = std::max(0.f, b.vel.y);
        grounded = true;
        // Ride along with moving bodies.
        pl_.pos += vec3(b.pointVel(p).x, 0, b.pointVel(p).z) * dt * 0.25f;
      } else {
        vec3 n = dl > 1e-4f ? d / dl : vec3(1, 0, 0);
        float push = pen * 0.5f;
        if (b.mass < 400 && !b.frozen) {
          b.wake();
          vec3 rel = vec3(pl_.vel.x, 0, pl_.vel.z) - b.pointVel(p);
          float into = -dot(rel, n);
          if (into > 0) b.applyImpulse(n * -into * std::min(8.f, 80.f * 0.02f), p);
          push *= std::min(1.f, b.mass / 80.f);
        }
        pl_.pos += n * push;
        float vn = dot(vec3(pl_.vel.x, 0, pl_.vel.z), n);
        if (vn < 0) { pl_.vel.x -= n.x * vn; pl_.vel.z -= n.z * vn; }
      }
    }
  }
  pl_.onGround = grounded;
  // Keep inside the world.
  float mx = sim.world.nx * sim.vs - 0.3f, mz = sim.world.nz * sim.vs - 0.3f;
  pl_.pos.x = clampf(pl_.pos.x, 0.3f, mx);
  pl_.pos.z = clampf(pl_.pos.z, 0.3f, mz);
  if (pl_.pos.y < -2) { pl_.pos = scene.spawn + vec3(0, 0.5f, 0); pl_.vel = vec3(0); }
}

void Game::updateHover() {
  float range = tool_ == T_SCAN ? 30.f : tool_ == T_BUILD ? 7.f : tool_ == T_HOSE ? 0.f : 4.5f;
  if (tool_ == T_HOSE || tool_ == T_CHARGE) range = 6.f;
  int ignore = -1;
  if (grabId_) { Body* g = sim.bodyById(grabId_); if (g) ignore = sim.bodyIndex(g); }
  hit_ = sim.raycast(cam.pos, cam.forward, range, ignore);
  hover_ = HoverInfo{};
  if (!hit_.hit) { hoverCompCached_ = 0; return; }
  const Volume& v = hit_.body >= 0 ? sim.bodies[hit_.body]->vol : sim.world;
  uint32_t c = v.comp(hit_.cell.x, hit_.cell.y, hit_.cell.z);
  if (!c || comp(c).type == C_TERRAIN) { hoverCompCached_ = 0; hover_.body = hit_.body; return; }
  int bid = hit_.body >= 0 ? sim.bodies[hit_.body]->id : 0;
  static ivec3 lo, hi;
  if (c != hoverCompCached_ || bid != hoverBodyIdCached_ || (hit_.body >= 0 && sim.bodies[hit_.body]->vol.version != (uint32_t)hoverBodyCached_)) {
    // Bounds of the part (flood through the world, scan a body).
    lo = hit_.cell; hi = hit_.cell;
    if (hit_.body >= 0) {
      v.forEach([&](int x, int y, int z, Brick* br, int l) {
        if (br->comp[l] != c) return;
        lo = {std::min(lo.x, x), std::min(lo.y, y), std::min(lo.z, z)};
        hi = {std::max(hi.x, x), std::max(hi.y, y), std::max(hi.z, z)};
      });
      hoverBodyCached_ = (int)sim.bodies[hit_.body]->vol.version;
    } else {
      std::vector<ivec3> st{hit_.cell};
      std::unordered_set<int64_t> seen{packKey(hit_.cell.x, hit_.cell.y, hit_.cell.z)};
      while (!st.empty() && seen.size() < 30000) {
        ivec3 p = st.back();
        st.pop_back();
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        for (auto& d : DIRS6) {
          ivec3 q(p.x + d[0], p.y + d[1], p.z + d[2]);
          if (v.comp(q.x, q.y, q.z) != c || v.matI(q) == AIR) continue;
          if (seen.insert(packKey(q.x, q.y, q.z)).second) st.push_back(q);
        }
      }
    }
    hoverCompCached_ = c;
    hoverBodyIdCached_ = bid;
  }
  hover_.body = hit_.body;
  hover_.comp = c;
  hover_.lo = lo;
  hover_.hi = hi;
}

// ----------------------------------------------------------------------- held tools

int Game::makeViewmodel(int tool) {
  const float vs = sim.vs;
  Volume v((int)std::ceil(0.16f / vs), (int)std::ceil(0.4f / vs), (int)std::ceil(0.24f / vs));
  Shaper s(&v, vs);
  uint32_t c = newComp([&] { Component cc; cc.name = TOOLS[tool].name; cc.object = "Your hands"; return cc; }());
  s.comp = c;
  switch (tool) {
    case T_HAMMER:
      s.mat = OAK; s.box(vec3(0.05f, 0.0f, 0.1f), vec3(0.075f, 0.3f, 0.125f));
      s.mat = RUBBER; s.box(vec3(0.045f, 0.0f, 0.095f), vec3(0.08f, 0.1f, 0.13f));
      s.mat = STEEL; s.box(vec3(0.0f, 0.28f, 0.085f), vec3(0.15f, 0.33f, 0.14f));
      break;
    case T_WRENCH:
      s.mat = STEEL; s.box(vec3(0.05f, 0.0f, 0.1f), vec3(0.075f, 0.26f, 0.125f));
      s.box(vec3(0.025f, 0.26f, 0.1f), vec3(0.1f, 0.33f, 0.125f));
      s.clear(vec3(0.05f, 0.29f, 0.1f), vec3(0.075f, 0.33f, 0.125f));
      break;
    case T_CHISEL:
      s.mat = OAK; s.cylinder(vec3(0.0625f, 0.0f, 0.1125f), vec3(0.0625f, 0.13f, 0.1125f), 0.022f);
      s.mat = STEEL; s.box(vec3(0.05f, 0.13f, 0.1f), vec3(0.075f, 0.28f, 0.125f));
      break;
    case T_TORCH:
      s.mat = PAINT_RED; s.cylinder(vec3(0.0625f, 0.0f, 0.15f), vec3(0.0625f, 0.16f, 0.15f), 0.045f);
      s.mat = BRASS; s.box(vec3(0.05f, 0.16f, 0.125f), vec3(0.075f, 0.2f, 0.175f));
      s.mat = STEEL; s.box(vec3(0.05f, 0.175f, 0.0f), vec3(0.075f, 0.2f, 0.13f));
      break;
    case T_HOSE:
      s.mat = RUBBER; s.cylinder(vec3(0.0625f, 0.0f, 0.2f), vec3(0.0625f, 0.12f, 0.13f), 0.02f);
      s.mat = BRASS; s.cylinder(vec3(0.0625f, 0.12f, 0.15f), vec3(0.0625f, 0.13f, 0.0f), 0.025f);
      break;
    case T_CHARGE:
      s.mat = CHARGE; s.box(vec3(0.05f, 0.05f, 0.1f), vec3(0.1f, 0.25f, 0.15f));
      s.mat = COPPER; s.box(vec3(0.05f, 0.25f, 0.1f), vec3(0.075f, 0.3f, 0.125f));
      break;
    case T_BUILD:
      s.mat = BUILD_PALETTE[buildMat_]; s.box(vec3(0.0f, 0.1f, 0.05f), vec3(0.125f, 0.225f, 0.175f));
      break;
    case T_SCAN:
      s.mat = PAINT_BLUE; s.box(vec3(0.0f, 0.0f, 0.1f), vec3(0.1f, 0.15f, 0.125f));
      s.mat = GLASS; s.box(vec3(0.025f, 0.075f, 0.075f), vec3(0.075f, 0.125f, 0.1f));
      s.mat = COPPER; s.box(vec3(0.075f, 0.15f, 0.1f), vec3(0.1f, 0.225f, 0.125f));
      break;
    default:
      return 0;
  }
  Body* b = spawnBody(sim, std::move(v), vec3(0, -50, 0));
  if (!b) return 0;
  b->ghost = b->viewmodel = b->frozen = b->asleep = true;
  b->hidden = true;
  return b->id;
}

void Game::buildViewmodels() {
  for (int t = 0; t < T_COUNT; t++) viewIds_[t] = makeViewmodel(t);
  viewBuildMat_ = BUILD_PALETTE[buildMat_];
}

void Game::updateViewmodel(float dt) {
  if (viewBuildMat_ != BUILD_PALETTE[buildMat_] && viewIds_[T_BUILD]) {
    if (Body* old = sim.bodyById(viewIds_[T_BUILD])) { int i = sim.bodyIndex(old); if (i >= 0) sim.removeBody(i); }
    viewIds_[T_BUILD] = makeViewmodel(T_BUILD);
    viewBuildMat_ = BUILD_PALETTE[buildMat_];
  }
  useAnim_ = std::max(0.f, useAnim_ - dt * 5);
  quat qc = quat::axisAngle(vec3(0, 1, 0), -cam.yaw) * quat::axisAngle(vec3(1, 0, 0), cam.pitch);
  for (int t = 0; t < T_COUNT; t++) {
    Body* b = viewIds_[t] ? sim.bodyById(viewIds_[t]) : nullptr;
    if (!b) continue;
    bool show = t == tool_ && mode_ == M_PLAY && hud_ && !menu_;
    b->hidden = !show;
    if (!show) continue;
    float raise = std::min(1.f, toolAnim_ * 1.4f);
    vec3 off(0.25f, -0.2f - (1 - raise) * 0.2f + std::sin(pl_.bob * 2) * 0.006f, -0.5f);
    // Tilt: tools lean forward and in; the hammer winds up and swings.
    float pitch = -0.55f, roll = 0.3f;
    if (t == T_HAMMER) { pitch += charge_ * 0.9f - std::sin(swing_ * PI) * 1.3f; off.z += swing_ * 0.08f; }
    if (t == T_CHISEL || t == T_WRENCH) { pitch -= swing_ * 0.6f + useAnim_ * 0.3f; off.z -= workProgress_ > 0 ? std::sin(time_ * 30) * 0.004f : 0; }
    if (t == T_TORCH || t == T_HOSE) { pitch = -1.3f; roll = 0.1f; off.y += 0.04f; off += vec3((rng().f() - 0.5f), (rng().f() - 0.5f), 0) * (useAnim_ * 0.004f); }
    if (t == T_SCAN) { pitch = -0.6f; roll = 0.15f; off = off + vec3(0.04f, 0.0f, -0.05f); }
    quat ql = quat::axisAngle(vec3(0, 0, 1), -roll) * quat::axisAngle(vec3(1, 0, 0), pitch);
    b->rot = qc * ql;
    b->pos = cam.pos + qc.rotate(off);
    b->updateInertiaWorld();
  }
}

void Game::toggleSwitch(int bi, uint32_t c) {
  Volume& v = bi >= 0 ? sim.bodies[bi]->vol : sim.world;
  bool nowOn = false;
  std::vector<ivec3> cells;
  if (bi >= 0) {
    v.forEach([&](int x, int y, int z, Brick* br, int l) { if (br->comp[l] == c && (br->mat[l] == SWITCH_ON || br->mat[l] == SWITCH_OFF)) cells.push_back({x, y, z}); });
  } else {
    for (int64_t k : sim.worldSpecials) {
      ivec3 p = unpackKey(k);
      uint8_t m = v.matI(p);
      if ((m == SWITCH_ON || m == SWITCH_OFF) && v.comp(p.x, p.y, p.z) == c) cells.push_back(p);
    }
  }
  for (auto& p : cells) {
    Cell cc = v.get(p.x, p.y, p.z);
    cc.mat = cc.mat == SWITCH_ON ? SWITCH_OFF : SWITCH_ON;
    nowOn = cc.mat == SWITCH_ON;
    if (bi >= 0) v.put(p.x, p.y, p.z, cc);
    else sim.putWorld(p.x, p.y, p.z, cc);
  }
  if (bi >= 0) sim.bodies[bi]->wake();
  audio.play(SFX_CLICK, hit_.point, 0.8f, nowOn ? 1.1f : 0.9f);
  std::string obj = comp(c).object.empty() ? "Circuit" : comp(c).object;
  toast(obj + (nowOn ? ": switch ON" : ": switch OFF"), nowOn ? ui::theme::good : ui::theme::dim, ui::I_BOLT);
  sim.event(EV_TOGGLE, hit_.point, 0);
}

void Game::detonateCharge(int bi, uint32_t c) {
  Volume& v = bi >= 0 ? sim.bodies[bi]->vol : sim.world;
  if (bi >= 0) {
    Body& b = *sim.bodies[bi];
    v.forEach([&](int x, int y, int z, Brick* br, int l) { if (br->comp[l] == c && br->mat[l] == CHARGE) { br->temp[l] = 900; b.heat.add(v.bindex(x, y, z)); } });
  } else {
    for (int64_t k : sim.worldSpecials) {
      ivec3 p = unpackKey(k);
      if (v.matI(p) == CHARGE && v.comp(p.x, p.y, p.z) == c) { v.temp(p.x, p.y, p.z) = 900; sim.worldHot.add(v.bindex(p.x, p.y, p.z)); }
    }
  }
}

void Game::useKey() {
  if (!hit_.hit || !hover_.comp) return;
  int bi = hit_.body;
  Component& c = comp(hover_.comp);
  switch (c.type) {
    case C_SWITCH: toggleSwitch(bi, hover_.comp); break;
    case C_MOTOR:
      c.reversed = !c.reversed;
      toast(std::string("Motor ") + (c.reversed ? "reversed" : "forward"), ui::theme::accent, ui::I_ROTATE);
      audio.play(SFX_CLICK, hit_.point);
      break;
    case C_BATTERY: {
      char buf[96];
      snprintf(buf, sizeof buf, "Battery %.0f%% (%.1f of %.0f Wh), %s", 100 * c.charge / std::max(1.f, c.capacity), c.charge, c.capacity,
               c.shorted ? "SHORT-CIRCUITED" : c.drawW > 0 ? "supplying power" : "idle");
      toast(buf, c.shorted ? ui::theme::danger : ui::theme::power, ui::I_BOLT);
      break;
    }
    case C_CHARGE:
      if (c.fuse < 0) {
        c.fuse = 3.f;
        if (bi < 0) {  // world-mounted: arm it via the remote list instead
          remoteCharges_.push_back(hover_.comp);
          remoteChargeBodies_.push_back(0);
          toast("Charge armed \xE2\x80\x94 press G to detonate", ui::theme::warm, ui::I_BOMB);
        } else {
          toast("Fuse lit! 3 seconds\xE2\x80\xA6", ui::theme::danger, ui::I_BOMB);
          Body& b = *sim.bodies[bi];
          b.vol.forEach([&](int x, int y, int z, Brick* br, int l) { if (br->comp[l] == hover_.comp) { br->flags[l] |= F_ARMED; b.vol.markDirty(x, y, z); } });
        }
        audio.play(SFX_SIZZLE, hit_.point);
      }
      break;
    case C_ROCKET:
      if (!c.lit && c.fuel > 0) { c.lit = true; toast("Rocket ignited!", ui::theme::warm, ui::I_FLAME); if (bi >= 0) sim.bodies[bi]->wake(); }
      break;
    default:
      toast(c.name + (c.object.empty() ? "" : " (" + c.object + ")") + ": nothing to operate", ui::theme::dim);
  }
}

void Game::chiselAt(const Hit& h, float radius, float energy) {
  Body* B = h.body >= 0 ? sim.bodies[h.body].get() : nullptr;
  Volume& v = B ? B->vol : sim.world;
  int r = (int)std::ceil(radius);
  struct C { ivec3 p; float d; };
  std::vector<C> cand;
  for (int dz = -r; dz <= r; dz++)
    for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++) {
        float d = std::sqrt((float)(dx * dx + dy * dy + dz * dz));
        if (d > radius) continue;
        ivec3 p = h.cell + ivec3(dx, dy, dz);
        uint8_t m = v.matI(p);
        if (m == AIR || m == BEDROCK || !v.inb(p.x, p.y, p.z)) continue;
        cand.push_back({p, d});
      }
  std::sort(cand.begin(), cand.end(), [](const C& a, const C& b) { return a.d < b.d; });
  int removed = 0;
  uint8_t firstMat = v.matI(h.cell);
  for (auto& c : cand) {
    Brick* br = v.brick(c.p.x, c.p.y, c.p.z);
    int l = lidx(c.p.x, c.p.y, c.p.z);
    uint8_t m = br->mat[l];
    if (!m) continue;
    float cost = MATS[m].tough * br->hp[l] / 255.f;
    if (energy < cost) {
      br->hp[l] = (uint8_t)std::max(1, (int)(br->hp[l] - 255 * energy / std::max(1e-6f, MATS[m].tough)));
      v.markDirty(c.p.x, c.p.y, c.p.z);
      break;
    }
    energy -= cost;
    collected[m]++;
    if (B) { B->vol.set(c.p.x, c.p.y, c.p.z, AIR); B->shapeDirty = true; B->splitCheck = true; B->wake(); }
    else sim.removeWorld(c.p.x, c.p.y, c.p.z, false);
    removed++;
  }
  if (removed) {
    for (int i = 0; i < 4; i++) sim.emit(FX_DUST, h.point + h.normal * 0.02f, h.normal * 0.6f + rng().unitVec() * 0.4f, 0.8f, sim.vs * 2.5f, matLinear(firstMat));
    Sfx s = MATS[firstMat].metal > 0.5f ? SFX_METAL : (firstMat == OAK || firstMat == PINE) ? SFX_WOOD : SFX_STONE;
    audio.play(s, h.point, 0.35f, 1.6f);
  } else {
    audio.play(SFX_METAL, h.point, 0.25f, 2.f);
    sim.emit(FX_SPARK, h.point, h.normal * 2 + rng().unitVec(), 0.2f, sim.vs * 0.4f, vec3(3, 2, 1));
  }
}

void Game::extinguish(const Hit& h, float radius) {
  Body* B = h.body >= 0 ? sim.bodies[h.body].get() : nullptr;
  Volume& v = B ? B->vol : sim.world;
  int r = (int)std::ceil(radius);
  for (int dz = -r; dz <= r; dz++)
    for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++) {
        ivec3 p = h.cell + ivec3(dx, dy, dz);
        if (!v.inb(p.x, p.y, p.z)) continue;
        Brick* br = v.brick(p.x, p.y, p.z);
        if (!br) continue;
        int l = lidx(p.x, p.y, p.z);
        if (br->flags[l] & F_BURN) { br->flags[l] &= ~F_BURN; v.markDirty(p.x, p.y, p.z); }
      }
}

void Game::throwCharge() {
  auto t = findPart("Charge")->tpl;
  vec3 p = cam.pos + cam.forward * 0.5f - vec3(0.12f, 0.03f, 0.03f);
  Body* b = spawnTemplate(*t, p);
  if (!b) return;
  b->rot = quat::axisAngle(vec3(0, 1, 0), -pl_.yaw);
  b->vel = cam.forward * 9.f + vec3(0, 2, 0) + pl_.vel;
  b->angVel = cam.right * 6.f;
  for (uint32_t c : b->compIds) if (comp(c).type == C_CHARGE) comp(c).fuse = 3.f;
  b->vol.forEach([&](int x, int y, int z, Brick* br, int l) { if (br->mat[l] == CHARGE) { br->flags[l] |= F_ARMED; b->vol.markDirty(x, y, z); } });
  audio.play(SFX_SWING, cam.pos, 0.6f);
  audio.play(SFX_SIZZLE, cam.pos, 0.4f);
}

void Game::placeCharge() {
  if (!hit_.hit) return;
  auto base = findPart("Charge")->tpl;
  // Orient the stick along the surface.
  auto t = std::abs(hit_.normal.y) > 0.7f ? base : rotated(*base, 1, 0);
  Body* B = hit_.body >= 0 ? sim.bodies[hit_.body].get() : nullptr;
  Volume& v = B ? B->vol : sim.world;
  ivec3 at = hit_.cell + hit_.face;
  ivec3 origin(at.x - t->vol.nx / 2, at.y, at.z - t->vol.nz / 2);
  if (hit_.face.y < 0) origin.y = at.y - t->vol.ny + 1;
  if (hit_.face.x != 0) origin = ivec3(hit_.face.x > 0 ? at.x : at.x - t->vol.nx + 1, at.y - t->vol.ny / 2, at.z - t->vol.nz / 2);
  if (hit_.face.z != 0) origin = ivec3(at.x - t->vol.nx / 2, at.y - t->vol.ny / 2, hit_.face.z > 0 ? at.z : at.z - t->vol.nz + 1);
  Component proto = t->proto;
  proto.name = "Remote charge";
  proto.object = "Demolition";
  uint32_t id = newComp(proto);
  int placed = 0;
  t->vol.forEach([&](int x, int y, int z, Brick* br, int l) {
    ivec3 q = origin + ivec3(x, y, z);
    if (!v.inb(q.x, q.y, q.z) || v.matI(q) != AIR) return;
    Cell c;
    c.mat = br->mat[l]; c.comp = id; c.flags = F_ARMED;
    if (B) v.put(q.x, q.y, q.z, c); else sim.putWorld(q.x, q.y, q.z, c);
    placed++;
  });
  if (!placed) return;
  if (B) { B->shapeDirty = true; B->wake(); }
  remoteCharges_.push_back(id);
  remoteChargeBodies_.push_back(B ? B->id : 0);
  audio.play(SFX_CLICK, hit_.point, 0.8f, 0.7f);
  toast("Charge placed (" + std::to_string(remoteCharges_.size()) + ") \xE2\x80\x94 G to detonate", ui::theme::warm, ui::I_BOMB);
}

void Game::useTools(float dt, plat::Input& in) {
  cooldown_ = std::max(0.f, cooldown_ - dt);
  swing_ = std::max(0.f, swing_ - dt * 4);
  repeat_ = std::max(0.f, repeat_ - dt);
  bool L = in.mdown[0], Lp = in.mpressed[0], Lr = in.mreleased[0];
  bool R = in.mdown[1], Rp = in.mpressed[1];
  // Grabbed body follows a spring to the hold point.
  if (grabId_) {
    Body* b = sim.bodyById(grabId_);
    if (!b || tool_ != T_HAND || !L) {
      if (b) b->held = false;
      grabId_ = 0;
    } else {
      grabDist_ = clampf(grabDist_ + in.wheel * 0.15f, 0.6f, 3.5f);
      vec3 target = cam.pos + cam.forward * grabDist_;
      vec3 p = b->toWorld(grabLocal_, sim.vs);
      vec3 v = b->pointVel(p);
      float w = 14.f;
      float meff = std::min(b->mass, 60.f);
      vec3 F = ((target - p) * (w * w) - v * (2 * w)) * meff + vec3(0, 9.81f, 0) * std::min(b->mass, 60.f);
      float maxF = 650.f;
      if (length(F) > maxF) F = normalize(F) * maxF;
      b->force += F;
      b->torque += cross(p - b->pos, F);
      b->angVel *= (1 - std::min(1.f, dt * 6));
      b->wake();
      b->held = true;
      if (Rp) {
        float e = 160.f;
        float sp = std::min(14.f, std::sqrt(2 * e / std::max(0.5f, b->mass)));
        b->vel += cam.forward * sp;
        b->held = false;
        grabId_ = 0;
        audio.play(SFX_SWING, cam.pos, 0.7f);
      }
      return;
    }
  }
  switch (tool_) {
    case T_HAND:
      if (Lp && hit_.hit) {
        if (hit_.body >= 0 && !sim.bodies[hit_.body]->frozen) {
          Body& b = *sim.bodies[hit_.body];
          grabId_ = b.id;
          grabLocal_ = b.toLocalVoxel(hit_.point, sim.vs);
          grabDist_ = clampf(hit_.t, 0.6f, 3.5f);
          b.wake();
          if (b.mass > 65) toast(comp(hover_.comp).object + " weighs " + std::to_string((int)b.mass) + " kg \xE2\x80\x94 you can only drag it", ui::theme::dim, ui::I_HAND);
        } else if (hover_.comp) {
          toast("Fixed in place (" + std::string(comp(hover_.comp).joint.name) + ") \xE2\x80\x94 hammer it loose or unbolt it with the wrench", ui::theme::dim, ui::I_WRENCH);
        }
      }
      if (Rp && hit_.hit && hit_.body >= 0) {
        Body& b = *sim.bodies[hit_.body];
        b.applyImpulse(cam.forward * 45.f, hit_.point);
        b.wake();
        audio.play(SFX_THUD, hit_.point, 0.5f);
      }
      break;
    case T_HAMMER: {
      if (L && cooldown_ <= 0) charge_ = std::min(1.f, charge_ + dt / 0.75f);
      bool strike = (Lr && charge_ > 0) || (Rp && cooldown_ <= 0);
      if (strike) {
        float e = Rp && !Lr ? 12.f : 15.f + 135.f * charge_ * charge_;
        charge_ = 0;
        cooldown_ = 0.3f;
        swing_ = 1;
        audio.play(SFX_SWING, cam.pos, 0.4f);
        if (hit_.hit && hit_.t < 2.6f) {
          uint8_t m = hit_.body >= 0 ? sim.bodies[hit_.body]->vol.matI(hit_.cell) : sim.world.matI(hit_.cell);
          if (hit_.body >= 0) {
            Body& b = *sim.bodies[hit_.body];
            b.applyImpulse(cam.forward * std::sqrt(2 * 1.5f * e), hit_.point);
            b.wake();
          }
          sim.impact(hit_, e, cam.forward, true);
          Sfx s = MATS[m].metal > 0.5f ? SFX_METAL : (m == OAK || m == PINE || m == BARK) ? SFX_WOOD
                  : (m == GLASS || m == BULBGLASS) ? SFX_GLASS : (MATS[m].tough < 25) ? SFX_SOFT : SFX_STONE;
          audio.play(s, hit_.point, 0.4f + e / 150.f, 0.9f + rng().f() * 0.2f);
          if (MATS[m].metal > 0.5f) for (int i = 0; i < 6; i++) sim.emit(FX_SPARK, hit_.point, hit_.normal * 2.5f + rng().unitVec() * 2.f, 0.35f, sim.vs * 0.4f, vec3(3, 2.2f, 1));
          else for (int i = 0; i < 5; i++) sim.emit(FX_DUST, hit_.point, hit_.normal * 0.8f + rng().unitVec() * 0.6f, 0.9f, sim.vs * 2.f, matLinear(m));
          shake_ = std::max(shake_, e / 150.f);
        }
      }
      if (!L) charge_ = 0;
      break;
    }
    case T_WRENCH: {
      if (L && hover_.comp && hit_.t < 3.f) {
        Component& c = comp(hover_.comp);
        if (workComp_ != hover_.comp) { workComp_ = hover_.comp; workProgress_ = 0; }
        float need = c.joint.energy >= 5000 ? -1.f : 0.25f + c.joint.energy / 260.f;  // seconds
        if (need < 0) {
          if (Lp) toast(c.name + " is " + c.joint.name + " \xE2\x80\x94 cut it with the torch or blast it", ui::theme::dim, ui::I_WRENCH);
          break;
        }
        workProgress_ += dt / need;
        if (repeat_ <= 0) { audio.play(SFX_CLICK, hit_.point, 0.3f, 0.6f + workProgress_); repeat_ = 0.12f; }
        if (workProgress_ >= 1) {
          workProgress_ = 0;
          workComp_ = 0;
          Body* nb = sim.detachComponent(hit_.body, hover_.comp, cam.forward * -0.6f + vec3(0, 0.4f, 0), hit_.cell);
          if (nb) { toast(c.name + " removed", ui::theme::accent, ui::I_WRENCH); audio.play(SFX_POP, hit_.point); }
          hoverCompCached_ = 0;
        }
      } else if (!L) { workProgress_ = 0; workComp_ = 0; }
      if (Rp && hover_.comp && hit_.t < 3.f) {
        Component& c = comp(hover_.comp);
        if (c.joint.energy >= 5000) { toast(c.name + " can't be unbolted (" + c.joint.name + ")", ui::theme::dim); break; }
        Body* nb = sim.detachComponent(hit_.body, hover_.comp, vec3(0), hit_.cell);
        if (nb && nb->voxels < 60000) {
          auto t = captureComponent(nb->vol, hover_.comp);
          if (t) {
            salvage.push_back(t);
            toast("Salvaged " + c.name + " \xE2\x86\x92 workshop (" + std::to_string(salvage.size()) + " parts)", ui::theme::good, ui::I_SAVE);
          }
          int idx = sim.bodyIndex(nb);
          if (idx >= 0) sim.removeBody(idx);
          audio.play(SFX_POP, hit_.point, 1, 1.3f);
          hoverCompCached_ = 0;
        }
      }
      break;
    }
    case T_CHISEL:
      if (hit_.hit && hit_.t < 3.f && ((L && repeat_ <= 0) || Rp)) {
        useAnim_ = 1;
        if (Rp) chiselAt(hit_, 0.5f, 25.f);
        else chiselAt(hit_, 1.8f, 30.f);
        repeat_ = 0.14f;
        swing_ = 0.6f;
      }
      break;
    case T_TORCH: {
      vec3 nozzle = cam.pos + cam.forward * 0.35f + cam.right * 0.12f - cam.up * 0.1f;
      bool reach = hit_.hit && hit_.t < 1.3f;
      if (L) {
        useAnim_ = 1;
        for (int i = 0; i < 3; i++)
          sim.emit(FX_FIRE, nozzle + cam.forward * rng().range(0.0f, 0.1f), cam.forward * rng().range(3, 6) + rng().unitVec() * 0.3f, rng().range(0.08f, 0.2f), sim.vs * 1.4f, vec3(0.6f, 0.9f, 2.2f));
        if (sim.airReady) {
          sim.air.driveTowards(nozzle + cam.forward * 0.25f, cam.forward * 9.f, 0.1f, std::min(1.f, dt * 8));
          sim.air.addHeat(nozzle + cam.forward * 0.3f, 3000.f * dt);
        }
        if (reach) {
          sim.addHeat(hit_, 12000.f, dt, 2.5f);
          if (rng().f() < 0.3f) sim.emit(FX_SPARK, hit_.point, hit_.normal + rng().unitVec(), 0.25f, sim.vs * 0.3f, vec3(3, 2, 1));
          if (hit_.body >= 0) sim.bodies[hit_.body]->wake();
        }
        audio.setLoop(L_TORCH, 0.35f, nozzle);
      } else if (R) {
        for (int i = 0; i < 3; i++) sim.emit(FX_STEAM, nozzle, cam.forward * rng().range(3, 5) + rng().unitVec() * 0.5f, 0.5f, sim.vs * 3, vec3(1));
        if (reach) { sim.addHeat(hit_, -6000.f, dt, 3.f); extinguish(hit_, 3.f); }
        audio.setLoop(L_TORCH, 0.2f, nozzle);
      } else {
        audio.setLoop(L_TORCH, 0, nozzle);
      }
      break;
    }
    case T_HOSE:
      if (L || R) {
        vec3 nozzle = cam.pos + cam.forward * 0.4f + cam.right * 0.12f - cam.up * 0.12f;
        int n = (int)std::ceil((R ? 60.f : 140.f) * dt * 1.f);
        for (int i = 0; i < n; i++) {
          vec3 dir = normalize(cam.forward + rng().unitVec() * (R ? 0.18f : 0.04f));
          Cell c;
          c.mat = WATER; c.temp = 14;
          sim.spawnDebris(nozzle, dir * (R ? 6.f : 11.f) + pl_.vel, c);
          if (!sim.debris.empty()) sim.debris.back().life = 4;
        }
        if (rng().f() < 0.5f) sim.emit(FX_SPRAY, nozzle, cam.forward * 8 + rng().unitVec(), 0.4f, sim.vs * 1.5f);
        audio.setLoop(L_WATER, 0.4f, nozzle);
      } else {
        audio.setLoop(L_WATER, 0, cam.pos);
      }
      break;
    case T_CHARGE:
      if (Lp && cooldown_ <= 0) { throwCharge(); cooldown_ = 0.6f; }
      if (Rp && hit_.hit && hit_.t < 4.f) placeCharge();
      break;
    case T_BUILD:
      if (hit_.hit && hit_.t < 7.f) {
        if (Lp || (L && repeat_ <= 0)) {
          repeat_ = 0.18f;
          if (buildPart_ >= 0 && buildPart_ < (int)salvage.size()) {
            const Template& t = *salvage[buildPart_];
            vec3 p = hit_.point + hit_.normal * (sim.vs * 0.5f);
            vec3 mn = p - vec3(t.vol.nx * 0.5f, 0, t.vol.nz * 0.5f) * sim.vs;
            if (hit_.normal.y < -0.5f) mn.y -= t.vol.ny * sim.vs;
            if (spawnTemplate(t, mn)) {
              audio.play(SFX_THUD, p, 0.5f);
              toast("Placed salvaged " + t.name, ui::theme::accent);
              salvage.erase(salvage.begin() + buildPart_);
              buildPart_ = salvage.empty() ? -1 : std::min(buildPart_, (int)salvage.size() - 1);
            }
          } else {
            uint8_t m = BUILD_PALETTE[buildMat_];
            uint32_t cid = newComp([&] { Component c; c.name = std::string(MATS[m].name) + " block"; c.object = "Placed blocks";
                                         c.joint = MATS[m].metal > 0.5f ? joints::welded : joints::glued; return c; }());
            ivec3 base = hit_.cell + hit_.face;
            int n = buildSize_;
            int off = n > 1 ? -(n / 2) : 0;
            for (int dy = 0; dy < n; dy++)
              for (int dz = 0; dz < n; dz++)
                for (int dx = 0; dx < n; dx++) {
                  ivec3 q = base + ivec3(dx + (hit_.face.x == 0 ? off : 0) * 1, dy + (hit_.face.y == 0 ? off : 0), dz + (hit_.face.z == 0 ? off : 0));
                  if (hit_.face.x < 0) q.x = base.x - dx;
                  if (hit_.face.y < 0) q.y = base.y - dy;
                  if (hit_.face.z < 0) q.z = base.z - dz;
                  if (hit_.body >= 0) {
                    Body& b = *sim.bodies[hit_.body];
                    if (b.vol.inb(q.x, q.y, q.z) && b.vol.matI(q) == AIR) { b.vol.set(q.x, q.y, q.z, m, cid); b.shapeDirty = true; b.wake(); }
                  } else if (sim.world.inb(q.x, q.y, q.z) && sim.world.matI(q) == AIR) {
                    sim.setWorld(q.x, q.y, q.z, m, cid);
                  }
                }
            // Unsupported placements fall: check them.
            if (hit_.body < 0) sim.supportSeeds.push_back(packKey(base.x, base.y, base.z));
            audio.play(SFX_CLICK, hit_.point, 0.4f, 0.8f);
          }
        }
        if (Rp) {
          chiselAt(hit_, buildSize_ > 1 ? buildSize_ * 0.6f : 0.5f, 1e9f);
        }
      }
      break;
    case T_SCAN:
      break;
  }
  if (tool_ != T_TORCH) audio.setLoop(L_TORCH, 0, cam.pos);
  if (tool_ != T_HOSE) audio.setLoop(L_WATER, 0, cam.pos);
}

void Game::handleEvents() {
  float lastExplode = -1;
  for (auto& e : sim.events) {
    switch (e.kind) {
      case EV_EXPLODE: {
        float d = length(e.p - cam.pos);
        flash_ = std::max(flash_, std::min(0.5f, e.energy / 200000.f / (1 + d * d * 0.1f)));
        shake_ = std::max(shake_, std::min(2.f, std::sqrt(e.energy / 40000.f) / (1 + d * 0.3f)));
        audio.play(SFX_BOOM, e.p, std::min(3.f, 0.6f + e.energy / 60000.f), 0.9f + rng().f() * 0.2f);
        if (lastExplode < 0) {
          char buf[64];
          snprintf(buf, sizeof buf, "Explosion \xC2\xB7 %.0f kJ", e.energy / 1000.f);
          toast(buf, ui::theme::warm, ui::I_BOMB);
        }
        lastExplode = e.energy;
        break;
      }
      case EV_IMPACT: {
        if (e.energy < 4) break;
        uint8_t m = e.mat;
        Sfx s = MATS[m].metal > 0.5f ? SFX_METAL : (m == OAK || m == PINE || m == BARK || m == PLASTIC) ? SFX_WOOD
                : (m == GLASS || m == BULBGLASS) ? SFX_GLASS : MATS[m].tough < 25 ? SFX_SOFT : SFX_STONE;
        audio.play(s, e.p, std::min(1.2f, e.energy / 200.f), 0.8f + rng().f() * 0.4f);
        break;
      }
      case EV_SHATTER: audio.play(SFX_GLASS, e.p, std::min(1.f, 0.3f + e.energy / 100.f)); break;
      case EV_DETACH: if (e.energy > 30) audio.play(SFX_POP, e.p, 0.5f, 0.8f); break;
      case EV_SHORT: {
        const Component& c = comp(e.comp);
        toast("\xE2\x9A\xA1 Short circuit! " + (c.object.empty() ? std::string("A battery") : c.object) + " is overheating", ui::theme::danger, ui::I_BOLT);
        break;
      }
      case EV_IGNITE:
        if (e.energy >= 2) toast("Battery thermal runaway!", ui::theme::danger, ui::I_FLAME);
        else if (e.energy >= 1) { toast("Rocket motor lit!", ui::theme::warm, ui::I_FLAME); }
        else toast(std::string(MATS[e.mat].name) + " caught fire", ui::theme::warm, ui::I_FLAME);
        break;
      case EV_MELT: toast(std::string(MATS[e.mat].name) + " is melting", ui::theme::warm, ui::I_THERMO); break;
      default: break;
    }
  }
  sim.events.clear();
  // Ambient loops: fire crackle near the nearest flame, motor hum near powered motors.
  vec3 firePos = cam.pos;
  float best = 1e9f;
  for (size_t i = 0; i < sim.fx.size(); i += 5)
    if (sim.fx[i].kind == FX_FIRE) { float d = length2(sim.fx[i].p - cam.pos); if (d < best) { best = d; firePos = sim.fx[i].p; } }
  audio.setLoop(L_FIRE, best < 1e8f ? std::min(1.f, 0.15f + sim.stats.burning * 0.002f) : 0.f, firePos);
  float hum = 0;
  vec3 humPos = cam.pos;
  for (auto& bp : sim.bodies)
    for (uint32_t c : bp->compIds)
      if (comp(c).type == C_MOTOR && comp(c).powered) { hum = 0.3f; humPos = bp->pos; }
  audio.setLoop(L_MOTOR, hum, humPos);
}

void Game::resetWorld() {
  sim.init(sim.cfg);
  initParts(sim.vs);
  scene = buildScene(sim);
  sim.settleWater();
  sim.initAir();
  for (int i = 0; i < 90; i++) sim.step(1.f / 60.f);
  for (auto& b : sim.bodies) { b->vel = vec3(0); b->angVel = vec3(0); b->asleep = true; }
  sim.events.clear();
  ren.uploadAllWorld();
  workshop->createJig();
  buildViewmodels();
  pl_ = Player{};
  pl_.pos = scene.spawn;
  pl_.yaw = scene.spawnYaw;
  grabId_ = 0;
  remoteCharges_.clear();
  remoteChargeBodies_.clear();
  mode_ = M_PLAY;
  toast("World reset", ui::theme::accent);
}

void Game::saveScreenshot(const std::string& path) {
  int w = plat::width(), h = plat::height();
  std::vector<uint8_t> px((size_t)w * h * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = h - 1; y >= 0; y--) fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
  fclose(f);
  fprintf(stderr, "Saved %s\n", path.c_str());
}

// A tiny command language for automated runs (tests, screenshots):
//   pos x y z yaw pitch | wait frames | tool n | press KEY | hold KEY frames | click L|R | holdclick L|R frames
//   shot file.ppm | workshop | play | blueprint NAME | launch | explode x y z J | log
void Game::runScriptLine(const std::string& line, bool& quit) {
  std::istringstream ss(line);
  std::string cmd;
  ss >> cmd;
  if (cmd.empty() || cmd[0] == '#') return;
  static int holdKey = -1, holdFrames = 0, holdBtn = -1;
  (void)holdKey; (void)holdFrames; (void)holdBtn;
  if (cmd == "pos") { ss >> pl_.pos.x >> pl_.pos.y >> pl_.pos.z >> pl_.yaw >> pl_.pitch; pl_.vel = vec3(0); }
  else if (cmd == "wait") { ss >> scriptWait_; }
  else if (cmd == "tool") { int t; ss >> t; tool_ = std::clamp(t - 1, 0, T_COUNT - 1); }
  else if (cmd == "press") {
    std::string k; ss >> k;
    int code = k.size() == 1 ? (int)toupper(k[0]) : k == "TAB" ? key::Tab : k == "ESC" ? key::Escape : k == "F1" ? key::F1 : k == "F3" ? key::F3 : -1;
    if (code >= 0) { scriptIn_.pressed[code] = true; scriptIn_.down[code] = true; }
    scriptWait_ = 1;
  } else if (cmd == "click") {
    std::string b; ss >> b;
    int i = b == "R" ? 1 : 0;
    scriptIn_.mpressed[i] = scriptIn_.mdown[i] = true;
    scriptWait_ = 1;
  } else if (cmd == "release") {
    std::string b; ss >> b;
    int i = b == "R" ? 1 : 0;
    scriptIn_.mreleased[i] = true;
    scriptWait_ = 1;
  } else if (cmd == "holdclick") {
    // Hold the button for N frames: emulate by injecting 'down' through a queued sequence.
    std::string b; int n; ss >> b >> n;
    int i = b == "R" ? 1 : 0;
    for (int k = 0; k < n; k++) { float dt = 1.f / 60.f; plat::Input in{}; in.mdown[i] = true; if (k == 0) in.mpressed[i] = true; scriptIn_ = in; frame(dt); plat::swap(); }
    scriptIn_ = plat::Input{};
    scriptIn_.mreleased[i] = true;
  } else if (cmd == "mouse") { ss >> scriptIn_.mx >> scriptIn_.my; }
  else if (cmd == "shot") { std::string p; ss >> p; frame(1.f / 60.f); saveScreenshot(p); }
  else if (cmd == "workshop") { mode_ = M_WORKSHOP; workshop->enter(); }
  else if (cmd == "play") { mode_ = M_PLAY; workshop->leave(); }
  else if (cmd == "blueprint") { std::string n; std::getline(ss, n); while (!n.empty() && n[0] == ' ') n.erase(0, 1); workshop->loadBlueprint(n); }
  else if (cmd == "launch") { workshop->launch(); }
  else if (cmd == "explode") { vec3 p; float e; ss >> p.x >> p.y >> p.z >> e; sim.explode(p, e); }
  else if (cmd == "toggle") { updateHover(); useKey(); }
  else if (cmd == "switch") {
    // Flip every switch belonging to the named object.
    std::string name; std::getline(ss, name); while (!name.empty() && name[0] == ' ') name.erase(0, 1);
    for (size_t i = 0; i < sim.bodies.size(); i++)
      for (uint32_t c : sim.bodies[i]->compIds)
        if (comp(c).type == C_SWITCH && (comp(c).object == name || name.empty())) toggleSwitch((int)i, c);
  }
  else if (cmd == "smooth") { int v; ss >> v; ropt.smooth = v != 0; }
  else if (cmd == "log") {
    fprintf(stderr, "[t=%.2f] bodies=%zu awake=%d debris=%zu fx=%zu burning=%d hot=%d powered=%d circuits=%d phys=%.1fms therm=%.1fms air=%.1fms(%d) fluid=%.1fms gpu=%.1fms fps=%.0f\n",
            sim.time, sim.bodies.size(), sim.stats.awake, sim.debris.size(), sim.fx.size(), sim.stats.burning, sim.stats.hot,
            sim.stats.poweredDevices, sim.stats.circuits, sim.stats.physicsMs, sim.stats.thermalMs, sim.air.stepMs, sim.air.activeCells, sim.stats.fluidMs, ren.gpuMs(), fps_);
  } else if (cmd == "probe") {
    vec3 p; ss >> p.x >> p.y >> p.z;
    vec3 v = sim.air.velocity(p);
    fprintf(stderr, "[probe %.2f %.2f %.2f] air T=%.1fC v=(%.2f %.2f %.2f) smoke=%.3f\n", p.x, p.y, p.z, sim.air.temperature(p), v.x, v.y, v.z, sim.air.smoke(p));
  } else if (cmd == "quit") quit = true;
}
