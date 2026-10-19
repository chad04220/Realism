// In-game HUD, menus and the help sheet.
#include <cstdio>
#include "game.h"
#include "workshop.h"
#include "assemblies.h"

using namespace ui;

namespace {
std::string fmtTemp(float t) {
  char b[32];
  snprintf(b, sizeof b, "%.0f \xC2\xB0""C", t);
  return b;
}
std::string fmtMass(float kg) {
  char b[32];
  if (kg < 1) snprintf(b, sizeof b, "%.0f g", kg * 1000);
  else if (kg < 100) snprintf(b, sizeof b, "%.1f kg", kg);
  else snprintf(b, sizeof b, "%.0f kg", kg);
  return b;
}
Color tempColor(float t) {
  if (t < 0) return Color::hex(0x7cc6ff);
  if (t < 45) return theme::dim;
  if (t < 150) return theme::warm;
  return theme::danger;
}
}  // namespace

void Game::drawHotbar() {
  float s = scale();
  float slot = 58 * s, gap = 8 * s;
  float total = (float)T_COUNT * slot + (float)(T_COUNT - 1) * gap;
  float x0 = width() * 0.5f - total * 0.5f, y0 = height() - slot - 22 * s;
  shadow(x0 - 10 * s, y0 - 10 * s, total + 20 * s, slot + 20 * s, 16 * s, 18 * s, 0.35f);
  rectBorder(x0 - 10 * s, y0 - 10 * s, total + 20 * s, slot + 20 * s, theme::panel, theme::line, 16 * s);
  hotbarAnim_ += ((float)tool_ - hotbarAnim_) * 0.25f;
  float sx = x0 + hotbarAnim_ * (slot + gap);
  rect(sx - 2 * s, y0 - 2 * s, slot + 4 * s, slot + 4 * s, theme::accent.alpha(0.18f), 12 * s);
  rectBorder(sx - 2 * s, y0 - 2 * s, slot + 4 * s, slot + 4 * s, Color(0, 0, 0, 0), theme::accent, 12 * s, 2 * s);
  for (int i = 0; i < T_COUNT; i++) {
    float x = x0 + i * (slot + gap);
    bool sel = i == tool_;
    icon(TOOLS[i].icon, x + slot * 0.5f, y0 + slot * 0.47f, slot * 0.52f, sel ? theme::accent : theme::text.alpha(0.85f));
    text(x + 6 * s, y0 + 3 * s, std::to_string(i + 1), 12 * s, sel ? theme::accent : theme::faint);
  }
  // Tool name and mouse hints above the bar.
  float a = std::min(1.f, toolAnim_ * 1.5f);
  float ty = y0 - 58 * s + (1 - a) * 6 * s;
  text(width() * 0.5f, ty, TOOLS[tool_].name, 20 * s, theme::text.alpha(a), 1, 0.06f);
  std::string hint = std::string("LMB  ") + TOOLS[tool_].lmb + "     RMB  " + TOOLS[tool_].rmb;
  if (tool_ == T_BUILD) {
    if (buildPart_ >= 0 && buildPart_ < (int)salvage.size()) hint = "LMB place salvaged " + salvage[buildPart_]->name + "     X next part";
    else hint += std::string("     Q material \xC2\xB7 ") + MATS[BUILD_PALETTE[buildMat_]].name + "     Z size " + std::to_string(buildSize_);
  }
  if (tool_ == T_HAND && grabId_) hint = "Wheel: distance     RMB throw     release LMB to drop";
  text(width() * 0.5f, ty + 26 * s, hint, 14 * s, theme::dim.alpha(a), 1);
}

void Game::drawTargetCard(float x, float y) {
  float s = scale();
  if (!hit_.hit) return;
  const Volume& v = hit_.body >= 0 ? sim.bodies[hit_.body]->vol : sim.world;
  Cell c = v.get(hit_.cell.x, hit_.cell.y, hit_.cell.z);
  if (c.mat == AIR) return;
  const Material& M = MATS[c.mat];
  const Component* cp = c.comp ? &comp(c.comp) : nullptr;
  bool deep = tool_ == T_SCAN;
  float w = (deep ? 340 : 290) * s;
  std::vector<std::pair<std::string, Color>> lines;
  std::string title = cp ? cp->name : M.name;
  std::string sub = cp && !cp->object.empty() ? cp->object : (hit_.body >= 0 ? "Loose object" : "World");
  // Rows.
  auto row = [&](const std::string& k, const std::string& v_, Color c_ = theme::text) { lines.push_back({k + "\t" + v_, c_}); };
  row("Material", M.name);
  row("Temperature", fmtTemp(c.temp), tempColor(c.temp));
  if (c.flags & F_BURN) row("State", "Burning", theme::warm);
  else if (c.mat == MOLTEN) row("State", std::string("Molten ") + MATS[c.aux].name, theme::danger);
  if (c.hp < 255) {
    char b[32];
    snprintf(b, sizeof b, "%.0f%%", c.hp / 2.55f);
    row("Integrity", b, c.hp < 120 ? theme::danger : theme::warm);
  }
  if (cp && cp->type != C_TERRAIN) {
    char b[64];
    if (cp->joint.energy < 1e20f) {
      snprintf(b, sizeof b, "%s \xC2\xB7 %.0f J", cp->joint.name, cp->joint.energy);
      row("Fixing", b, theme::dim);
    }
    if (cp->type != C_PART) row("Function", compTypeName(cp->type), theme::accent);
    switch (cp->type) {
      case C_BATTERY:
        snprintf(b, sizeof b, "%.0f%% \xC2\xB7 %.0f W out", 100 * cp->charge / std::max(1.f, cp->capacity), cp->drawW);
        row("Charge", b, cp->shorted ? theme::danger : theme::power);
        if (cp->shorted) row("Fault", "SHORT CIRCUIT", theme::danger);
        break;
      case C_BULB: case C_MOTOR: case C_HEATER:
        row("Power", cp->powered ? (cp->powerFactor < 0.99f ? "Dim (low supply)" : "On") : "No circuit", cp->powered ? theme::good : theme::faint);
        if (cp->type == C_MOTOR) row("Direction", cp->reversed ? "Reverse" : "Forward", theme::dim);
        break;
      case C_SOLAR:
        snprintf(b, sizeof b, "%.0f W available", cp->capacity);
        row("Output", b, theme::power);
        break;
      case C_ROCKET:
        snprintf(b, sizeof b, "%s \xC2\xB7 %.1f s fuel", cp->lit ? "Burning" : "Ready", std::max(0.f, cp->fuel));
        row("Motor", b, cp->lit ? theme::warm : theme::dim);
        break;
      case C_CHARGE:
        row("Fuse", cp->fuse >= 0 ? "LIT" : "Safe", cp->fuse >= 0 ? theme::danger : theme::dim);
        break;
      default: break;
    }
  }
  if (hit_.body >= 0) {
    Body& b = *sim.bodies[hit_.body];
    row("Object mass", fmtMass(b.mass), theme::dim);
    if (deep) {
      char bb[64];
      snprintf(bb, sizeof bb, "%.2f m/s", length(b.vel));
      row("Speed", bb, theme::dim);
      snprintf(bb, sizeof bb, "%.0f J", 0.5f * b.mass * length2(b.vel));
      row("Kinetic energy", bb, theme::dim);
      row("Parts", std::to_string(b.compIds.size()), theme::dim);
    }
  }
  if (deep) {
    char b[96];
    snprintf(b, sizeof b, "%.0f kg/m\xC2\xB3 \xC2\xB7 c %.0f J/kgK", M.density, M.cp);
    row("Density \xC2\xB7 heat", b, theme::dim);
    snprintf(b, sizeof b, "k %.2f W/mK", M.k);
    row("Conductivity", b, theme::dim);
    if (M.melt > 0) row("Melts at", fmtTemp(M.melt), theme::dim);
    if (M.ignite > 0) row("Ignites at", fmtTemp(M.ignite), theme::dim);
    if (M.wire) row("Electrical", "Conductor", theme::power);
    snprintf(b, sizeof b, "%.1f m", hit_.t);
    row("Distance", b, theme::dim);
  }
  // Action hint.
  std::string act;
  if (cp) {
    if (cp->type == C_SWITCH) act = "E  flip switch";
    else if (cp->type == C_MOTOR) act = "E  reverse motor";
    else if (cp->type == C_CHARGE) act = "E  light the fuse";
    else if (cp->type == C_ROCKET) act = "E  ignite";
    else if (cp->type == C_BATTERY) act = "E  check charge";
  }
  float rowH = 21 * s;
  float h = 62 * s + lines.size() * rowH + (act.empty() ? 0 : 30 * s) + (deep && M.info[0] ? 50 * s : 0);
  shadow(x, y, w, h, 12 * s, 14 * s, 0.4f);
  rectBorder(x, y, w, h, theme::panel, theme::line, 12 * s);
  rect(x, y, 4 * s, h, (cp && cp->type != C_PART && cp->type != C_TERRAIN) ? theme::accent : Color::hex(0x8b95a5), 2 * s);
  text(x + 16 * s, y + 10 * s, title, 18 * s, theme::text, 0, 0.05f);
  text(x + 16 * s, y + 34 * s, sub, 13 * s, theme::faint);
  float yy = y + 58 * s;
  for (auto& [ln, col] : lines) {
    auto tab = ln.find('\t');
    text(x + 16 * s, yy, ln.substr(0, tab), 14 * s, theme::faint);
    text(x + w - 14 * s, yy, ln.substr(tab + 1), 14 * s, col, 2);
    yy += rowH;
  }
  if (deep && M.info[0]) { yy += 4 * s; yy += textWrapped(x + 16 * s, yy, w - 30 * s, M.info, 13 * s, theme::dim); }
  if (!act.empty()) {
    rect(x + 12 * s, yy + 4 * s, w - 24 * s, 24 * s, theme::accent.alpha(0.12f), 6 * s);
    text(x + w * 0.5f, yy + 7 * s, act, 14 * s, theme::accent, 1);
  }
}

void Game::drawHud(float dt) {
  float s = scale();
  float cx = width() * 0.5f, cy = height() * 0.5f;
  // Crosshair: a dot, with a ring that fills as the hammer charges or the wrench works.
  bool target = hit_.hit;
  circle(cx, cy, 2.5f * s, Color(1, 1, 1, target ? 0.95f : 0.55f));
  ring(cx, cy, 9 * s, 1.5f * s, Color(1, 1, 1, target ? 0.35f : 0.12f));
  if (tool_ == T_HAMMER && charge_ > 0) {
    arc(cx, cy, 16 * s, 3.5f * s, -PI * 0.5f, -PI * 0.5f + 2 * PI * charge_, theme::warm.mix(theme::danger, charge_));
    char b[24];
    snprintf(b, sizeof b, "%.0f J", 15 + 135 * charge_ * charge_);
    text(cx + 26 * s, cy - 9 * s, b, 15 * s, theme::warm);
  }
  if (tool_ == T_WRENCH && workProgress_ > 0) arc(cx, cy, 16 * s, 3.5f * s, -PI * 0.5f, -PI * 0.5f + 2 * PI * workProgress_, theme::accent);
  if (hover_.comp && hit_.hit) {
    // Tiny label under the crosshair.
    text(cx, cy + 18 * s, comp(hover_.comp).name, 14 * s, theme::text.alpha(0.8f), 1);
  }
  if (target && (tool_ == T_SCAN || hover_.comp || hit_.body >= 0)) drawTargetCard(width() - 360 * s, std::max(height() * 0.5f - 60 * s, 270 * s));

  drawHotbar();

  // Status chips, top-left.
  float x = 18 * s, y = 18 * s;
  auto chip = [&](const std::string& t, Color c, int ic) {
    float w = textWidth(t, 14 * s) + 42 * s;
    rectBorder(x, y, w, 30 * s, theme::panel, theme::line, 15 * s);
    icon(ic, x + 16 * s, y + 15 * s, 16 * s, c);
    text(x + 30 * s, y + 7 * s, t, 14 * s, theme::text);
    y += 38 * s;
  };
  if (paused_) chip("Paused  \xC2\xB7  P to resume", theme::warm, I_PLAY);
  if (sim.timeScale < 1) chip("Slow motion \xC3\x97" "0.2", theme::accent, I_ROTATE);
  if (sim.stats.burning > 0) chip(std::to_string(sim.stats.burning) + " voxels burning", theme::warm, I_FLAME);
  if (sim.stats.poweredDevices > 0) chip(std::to_string(sim.stats.poweredDevices) + " powered devices", theme::power, I_BOLT);
  if (!remoteCharges_.empty()) chip(std::to_string(remoteCharges_.size()) + " charges armed \xC2\xB7 G", theme::danger, I_BOMB);
  if (pl_.fly) chip("Fly mode \xC2\xB7 N", theme::dim, I_HAND);
  if (!salvage.empty()) chip(std::to_string(salvage.size()) + " salvaged parts \xC2\xB7 Tab to build", theme::good, I_SAVE);

  // Workshop hint when near the pad.
  vec3 jc = scene.jigOrigin + vec3(1.2f, 0, 1.0f);
  if (length(vec3(pl_.pos.x - jc.x, 0, pl_.pos.z - jc.z)) < 2.5f) {
    std::string t = "Tab  open the workshop";
    float w = textWidth(t, 16 * s) + 30 * s;
    rectBorder(width() * 0.5f - w * 0.5f, height() * 0.18f, w, 34 * s, theme::accent.alpha(0.18f), theme::accent.alpha(0.6f), 17 * s);
    text(width() * 0.5f, height() * 0.18f + 8 * s, t, 16 * s, theme::text, 1);
  }
}

void Game::drawToasts(float dt) {
  float s = scale();
  float y = (mode_ == M_WORKSHOP ? 552.f : 18.f) * s;
  for (auto it = toasts_.begin(); it != toasts_.end();) {
    Toast& t = *it;
    t.age += dt;
    if (t.age > t.life) { it = toasts_.erase(it); continue; }
    float in = std::min(1.f, t.age * 5), out = std::min(1.f, (t.life - t.age) * 2.5f);
    float a = std::min(in, out);
    float w = textWidth(t.text, 15 * s) + (t.icon ? 50 : 30) * s;
    float x = width() - w - 18 * s + (1 - in) * 30 * s;
    shadow(x, y, w, 36 * s, 10 * s, 10 * s, 0.35f * a);
    rectBorder(x, y, w, 36 * s, theme::panel.alpha(a), t.color.alpha(0.35f * a), 10 * s);
    float tx = x + 15 * s;
    if (t.icon) { icon(t.icon, x + 20 * s, y + 18 * s, 18 * s, t.color.alpha(a)); tx = x + 36 * s; }
    text(tx, y + 9 * s, t.text, 15 * s, theme::text.alpha(a));
    y += 44 * s * std::min(1.f, out * 1.5f);
    ++it;
  }
}

void Game::drawStats() {
  float s = scale();
  float x = 18 * s, y = height() - 230 * s, w = 330 * s, h = 190 * s;
  rectBorder(x, y, w, h, theme::panel, theme::line, 10 * s);
  char b[160];
  float yy = y + 10 * s;
  auto ln = [&](const char* t) { text(x + 12 * s, yy, t, 13 * s, theme::dim); yy += 19 * s; };
  snprintf(b, sizeof b, "%.0f fps \xC2\xB7 GPU %.1f ms \xC2\xB7 %d bodies drawn", fps_, ren.gpuMs(), ren.bodiesDrawn); ln(b);
  snprintf(b, sizeof b, "Physics %.1f ms \xC2\xB7 heat %.1f ms \xC2\xB7 fluid %.1f ms", sim.stats.physicsMs, sim.stats.thermalMs, sim.stats.fluidMs); ln(b);
  snprintf(b, sizeof b, "Bodies %zu (%d awake) \xC2\xB7 contacts %d", sim.bodies.size(), sim.stats.awake, sim.stats.contacts); ln(b);
  snprintf(b, sizeof b, "Debris %zu \xC2\xB7 effects %zu \xC2\xB7 fluid cells %zu", sim.debris.size(), sim.fx.size(), sim.fluidActive.size()); ln(b);
  snprintf(b, sizeof b, "Hot voxels %d \xC2\xB7 burning %d \xC2\xB7 circuits %d", sim.stats.hot, sim.stats.burning, sim.stats.circuits); ln(b);
  snprintf(b, sizeof b, "World %zu M voxels \xC2\xB7 %zu MB RAM", sim.world.countSolid() / 1000000, sim.world.memoryBytes() >> 20); ln(b);
  snprintf(b, sizeof b, "Voxel %.1f cm \xC2\xB7 grid %d\xC3\x97%d\xC3\x97%d", sim.vs * 100, sim.world.nx, sim.world.ny, sim.world.nz); ln(b);
  snprintf(b, sizeof b, "Pos %.2f %.2f %.2f", pl_.pos.x, pl_.pos.y, pl_.pos.z); ln(b);
}

void Game::drawHelp() {
  float s = scale();
  rect(0, 0, (float)width(), (float)height(), Color(0, 0, 0, 0.55f));
  float w = 940 * s, h = 640 * s, x = width() * 0.5f - w * 0.5f, y = height() * 0.5f - h * 0.5f;
  shadow(x, y, w, h, 18 * s, 30 * s, 0.6f);
  rectBorder(x, y, w, h, theme::panelHi, theme::line, 18 * s);
  text(x + 32 * s, y + 26 * s, "How to play", 28 * s, theme::text, 0, 0.06f);
  text(x + 32 * s, y + 64 * s, "Every object is assembled from real parts made of real materials. Take things apart, set them on fire, wire them up, build machines.", 15 * s, theme::dim);
  struct KV { const char* k; const char* v; };
  KV left[] = {{"WASD", "Move"}, {"Mouse", "Look"}, {"Space", "Jump / swim up"}, {"Shift", "Sprint"}, {"Ctrl", "Crouch"},
               {"1 \xE2\x80\x93 9  /  wheel", "Choose tool"}, {"E", "Use: switches, motors, fuses, rockets"}, {"F", "Flashlight"},
               {"G", "Detonate remote charges"}, {"N", "Fly mode"}, {"Tab", "Workshop (build machines)"}};
  KV right[] = {{"T", "Slow motion"}, {"P", "Pause the simulation"}, {"V", "Smooth / blocky voxels"}, {"F2", "Hide HUD"},
                {"F3", "Performance stats"}, {"F11", "Fullscreen"}, {"Esc", "Menu"}};
  float yy = y + 110 * s;
  for (auto& kv : left) {
    rectBorder(x + 32 * s, yy - 3 * s, 150 * s, 26 * s, Color(1, 1, 1, 0.06f), theme::line, 6 * s);
    text(x + 107 * s, yy + 1 * s, kv.k, 14 * s, theme::accent, 1);
    text(x + 196 * s, yy + 1 * s, kv.v, 15 * s, theme::text);
    yy += 34 * s;
  }
  yy = y + 110 * s;
  for (auto& kv : right) {
    rectBorder(x + 500 * s, yy - 3 * s, 110 * s, 26 * s, Color(1, 1, 1, 0.06f), theme::line, 6 * s);
    text(x + 555 * s, yy + 1 * s, kv.k, 14 * s, theme::accent, 1);
    text(x + 624 * s, yy + 1 * s, kv.v, 15 * s, theme::text);
    yy += 34 * s;
  }
  yy += 10 * s;
  text(x + 500 * s, yy, "Things to try", 17 * s, theme::warm, 0, 0.05f);
  yy += 28 * s;
  const char* tips[] = {"Flip the rover's switch (E) and watch it drive.",
                        "Hammer a brick out of the shed wall.",
                        "Short a battery with copper and watch the wire glow.",
                        "Roll the powder keg into the campfire.",
                        "Unbolt the door hinges.",
                        "Melt the lamp's aluminium shade with the torch."};
  for (auto t : tips) { yy += textWrapped(x + 500 * s, yy, 410 * s, std::string("\xE2\x80\xA2 ") + t, 14 * s, theme::dim); yy += 4 * s; }
  text(x + w * 0.5f, y + h - 40 * s, "Press F1 or Esc to close", 14 * s, theme::faint, 1);
}

void Game::drawMenu() {
  float s = scale();
  rect(0, 0, (float)width(), (float)height(), Color(0.02f, 0.03f, 0.05f, 0.6f));
  float w = 440 * s, h = menuPage_ == "settings" ? 700 * s : 470 * s;
  float x = width() * 0.5f - w * 0.5f, y = height() * 0.5f - h * 0.5f;
  shadow(x, y, w, h, 18 * s, 30 * s, 0.6f);
  rectBorder(x, y, w, h, theme::panelHi, theme::line, 18 * s);
  float bx = x + 40 * s, bw = w - 80 * s, bh = 46 * s;
  if (menuPage_ == "main") {
    text(x + w * 0.5f, y + 32 * s, "REALISM", 34 * s, theme::text, 1, 0.08f);
    text(x + w * 0.5f, y + 76 * s, "voxel physics sandbox", 14 * s, theme::faint, 1);
    float yy = y + 120 * s;
    if (button("Resume", bx, yy, bw, bh, true)) menu_ = false;
    yy += bh + 12 * s;
    if (button("Settings", bx, yy, bw, bh)) menuPage_ = "settings";
    yy += bh + 12 * s;
    if (button("Controls", bx, yy, bw, bh)) { help_ = true; menu_ = false; }
    yy += bh + 12 * s;
    if (button("Reset world", bx, yy, bw, bh)) { menu_ = false; resetWorld(); }
    yy += bh + 12 * s;
    if (button("Quit", bx, yy, bw, bh)) quit_ = true;
  } else {
    text(x + 40 * s, y + 28 * s, "Settings", 26 * s, theme::text, 0, 0.06f);
    float yy = y + 80 * s;
    if (toggle("Smooth surfaces", bx, yy, bw, ropt.smooth)) settings.smooth = ropt.smooth;
    yy += 40 * s;
    if (toggle("Sun shadows", bx, yy, bw, ropt.shadows)) settings.shadows = ropt.shadows;
    yy += 40 * s;
    if (toggle("Bloom", bx, yy, bw, ropt.bloom)) settings.bloom = ropt.bloom;
    yy += 46 * s;
    if (slider("Render scale", bx, yy, bw, ropt.renderScale, 0.4f, 1.5f, "%.2f\xC3\x97")) settings.renderScale = ropt.renderScale;
    yy += 54 * s;
    float fovDeg = cam.fov * 180 / PI;
    if (slider("Field of view", bx, yy, bw, fovDeg, 55, 100, "%.0f\xC2\xB0")) { cam.fov = fovDeg * PI / 180; settings.fov = fovDeg; }
    yy += 54 * s;
    slider("Mouse sensitivity", bx, yy, bw, settings.sensitivity, 0.2f, 3.f, "%.2f");
    yy += 54 * s;
    if (slider("Thermal time warp", bx, yy, bw, sim.cfg.thermalWarp, 1, 60, "%.0f\xC3\x97")) settings.thermalWarp = sim.cfg.thermalWarp;
    yy += 54 * s;
    if (slider("Volume", bx, yy, bw, settings.volume, 0, 1, "%.2f")) audio.setVolume(settings.volume);
    yy += 54 * s;
    text(bx, yy, "Voxel size and world extent: edit settings.cfg", 13 * s, theme::faint);
    yy += 30 * s;
    if (button("Back", bx, yy, bw, bh, true)) { menuPage_ = "main"; settings.save(plat::userDataDir() + "/settings.cfg"); }
  }
}

bool Game::selftest() {
  // Headless checks of the simulation: run for a while and verify the
  // machines and physics behave.
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
  };
  // Lamp bulb should be powered.
  bool lampLit = false, fanOff = true;
  for (auto& c : comps()) {
    if (c.type == C_BULB && c.object == "Desk lamp" && c.powered) lampLit = true;
    if (c.type == C_MOTOR && c.object == "Desk fan" && c.powered) fanOff = false;
  }
  check(lampLit, "desk lamp bulb is powered through its switch");
  check(fanOff, "desk fan is off (switch open)");
  check(sim.stats.burning > 0, "campfire is burning");
  // Turn on the rover and drive.
  Body* rover = nullptr;
  for (auto& b : sim.bodies) for (uint32_t c : b->compIds) if (comp(c).object == "Rover" && comp(c).type == C_SWITCH) rover = b.get();
  check(rover != nullptr, "rover exists");
  if (rover) {
    vec3 p0 = rover->pos;
    uint32_t sw = 0;
    for (uint32_t c : rover->compIds) if (comp(c).type == C_SWITCH) sw = c;
    toggleSwitch(sim.bodyIndex(rover), sw);
    for (int i = 0; i < 240; i++) {
      sim.step(1.f / 60.f);
      if (i % 60 == 0) {
        Body* r = nullptr;
        for (auto& b : sim.bodies) for (uint32_t c : b->compIds) if (c == sw) r = b.get();
        if (r) {
          fprintf(stderr, "  t=%d asleep=%d vel=%.2f pos=%.2f %.2f %.2f", i, r->asleep, length(r->vel), r->pos.x, r->pos.y, r->pos.z);
          for (uint32_t c : r->compIds) if (comp(c).type == C_MOTOR || comp(c).type == C_BATTERY || comp(c).type == C_WHEEL) fprintf(stderr, " [%s pw=%d df=%.0f]", comp(c).name.c_str(), comp(c).powered, comp(c).driveForce);
          fprintf(stderr, " rot_parts=%zu samples=%zu contacts=%d awake=%d invMass=%.4f", r->rot_parts.size(), r->samples.size(), sim.stats.contacts, sim.stats.awake, r->invMass);
          fprintf(stderr, "\n");
        }
      }
    }
    rover = nullptr;
    for (auto& b : sim.bodies) for (uint32_t c : b->compIds) if (c == sw) rover = b.get();
    float moved = rover ? length(rover->pos - p0) : 0;
    fprintf(stderr, "  rover moved %.2f m\n", moved);
    check(moved > 1.0f, "rover drives when switched on");
  }
  // The campfire heats the air above it, and the breeze carries the plume.
  {
    float tmax = 0, vup = -1e9f;
    for (float y = 1.0f; y <= 2.4f; y += 0.2f)
      for (float x = 7.0f; x <= 9.4f; x += 0.2f)
        for (float z = 8.8f; z <= 10.2f; z += 0.2f) {
          float t = sim.air.temperature(vec3(x, y, z));
          if (t > tmax) { tmax = t; vup = sim.air.velocity(vec3(x, y, z)).y; }
        }
    fprintf(stderr, "  hottest air over the campfire %.0f C (rising at %.2f m/s)\n", tmax, vup);
    check(sim.airReady && tmax > 80 && vup > 0, "campfire plume heats and lifts the air");
  }
  // Knock a brick out of the shed wall with a hammer blow.
  {
    Hit h = sim.raycast(vec3(5.6f, 2.0f, 7.0f), normalize(vec3(0, 0, -1)), 3.f);
    if (h.hit && h.body < 0) {
      size_t nb = sim.bodies.size();
      sim.impact(h, 120.f, vec3(0, 0, -1), true);
      sim.step(1.f / 60.f);
      check(sim.bodies.size() > nb, "hammer blow knocks a brick loose");
    } else check(false, "found the shed wall");
  }
  // Explosion makes debris and bodies.
  {
    size_t before = sim.world.countSolid();
    sim.explode(vec3(14.0f, 1.2f, 13.0f), 80000.f);
    for (int i = 0; i < 30; i++) sim.step(1.f / 60.f);
    check(sim.world.countSolid() < before, "explosion removes material");
  }
  // Heat: torch a steel bar until it glows.
  {
    Volume v(8, 4, 4);
    uint32_t c = newComp(makePart("Test bar", "Test", joints::welded));
    for (int x = 0; x < 8; x++) for (int y = 0; y < 4; y++) for (int z = 0; z < 4; z++) v.set(x, y, z, STEEL, c);
    Body* b = spawnBody(sim, std::move(v), vec3(8, 1.5f, 12));
    Hit h;
    h.hit = true; h.body = sim.bodyIndex(b); h.cell = ivec3(0, 1, 1); h.point = b->pos; h.normal = vec3(-1, 0, 0);
    for (int i = 0; i < 240; i++) { sim.addHeat(h, 12000, 1.f / 60.f, 2.f); sim.step(1.f / 60.f); h.body = sim.bodyIndex(b); if (h.body < 0) break; }
    float tmax = 0;
    if (sim.bodyIndex(b) >= 0) b->vol.forEach([&](int, int, int, Brick* br, int l) { tmax = std::max(tmax, br->temp[l]); });
    fprintf(stderr, "  steel bar peak %.0f C\n", tmax);
    check(tmax > 500, "torch heats steel to a glow");
  }
  // Water levels out in a basin, and squirts from a hole near the bottom.
  {
    Volume& W = sim.world;
    float vs = sim.vs;
    int x0 = (int)(1.0f / vs), z0 = (int)(1.0f / vs), y0 = (int)(1.0f / vs);  // lawn level
    const int N = 24, H = 20;
    uint32_t bc = newComp(makePart("Test basin", "Test", joints::mortar));
    for (int x = -1; x <= N; x++)
      for (int z = -1; z <= N; z++)
        for (int y = -1; y <= H; y++) {
          bool wall = x < 0 || z < 0 || x == N || z == N || y < 0;
          sim.putWorld(x0 + x, y0 + y, z0 + z, Cell{});
          if (wall) { Cell c; c.mat = STONE; c.comp = bc; sim.putWorld(x0 + x, y0 + y, z0 + z, c); }
        }
    // A small tank inside the basin, full to the brim.
    for (int x = 0; x <= 6; x++)
      for (int z = 0; z <= 6; z++)
        for (int y = 0; y < H; y++) {
          Cell c;
          if (x == 6 || z == 6) { c.mat = STONE; c.comp = bc; } else c.mat = WATER;
          sim.putWorld(x0 + x, y0 + y, z0 + z, c);
        }
    sim.settleWater();
    auto fillSum = [&]() {
      double m = 0;
      for (int x = 0; x < N; x++) for (int z = 0; z < N; z++) for (int y = 0; y < H + 2; y++) {
        if (W.mat(x0 + x, y0 + y, z0 + z) != WATER) continue;
        float f = W.brick(x0 + x, y0 + y, z0 + z)->fuel[lidx(x0 + x, y0 + y, z0 + z)];
        m += f > 0 ? f : 1.f;
      }
      for (auto& d : sim.debris) if (d.mat == WATER) m += d.fill;
      return m;
    };
    if (getenv("REALISM_DEBUG_WATER")) fprintf(stderr, "wall check %d %d %d\n", W.mat(x0 + N, y0, z0 + 1), W.mat(x0 + N, y0 + 1, z0 + 1), W.mat(x0 + N, y0 - 1, z0 + 1));
    // Punch a hole 4 cells up the tank wall: the head above it is ~16 cells.
    sim.putWorld(x0 + 6, y0 + 4, z0 + 3, Cell{});
    float vmax = 0;
    int drops = 0;
    for (int i = 0; i < 30; i++) {
      sim.step(1.f / 60.f);
      for (auto& d : sim.debris) if (d.mat == WATER && d.p.x > (x0 + 7) * vs) { vmax = std::max(vmax, d.v.x); drops++; }
    }
    float expect = std::sqrt(2 * 9.81f * 15 * vs);
    fprintf(stderr, "  tank hole: %d droplet-steps, fastest %.2f m/s (Torricelli ~%.2f)\n", drops, vmax, expect);
    check(vmax > expect * 0.6f && vmax < expect * 1.4f, "water jets from a hole at Torricelli speed");
    // Take the tank away: the water should surge out and level across the basin.
    for (int x = 0; x <= 6; x++) for (int z = 0; z <= 6; z++) if (x == 6 || z == 6) for (int y = 0; y < H; y++) sim.putWorld(x0 + x, y0 + y, z0 + z, Cell{});
    if (getenv("REALISM_DEBUG_WATER")) fprintf(stderr, "wall check2 %d %d %d\n", W.mat(x0 + N, y0, z0 + 1), W.mat(x0 + N, y0 + 1, z0 + 1), W.mat(x0 + N, y0 - 1, z0 + 1));
    double m0 = fillSum();
    { extern double g_wlost[10]; for (double& d : g_wlost) d = 0; }
    for (int i = 0; i < (getenv("REALISM_DEBUG_WATER") ? 900 : 300); i++) {
      sim.step(1.f / 60.f);
      if (getenv("REALISM_DEBUG_WATER") && i % 60 == 59) {
        float a = 1e9f, b = -1e9f; double sd = 0; int n = 0;
        for (int x = 0; x < N; x++) for (int z = 0; z < N; z++) {
          float hcol = 0;
          for (int y = 0; y < H; y++) if (W.mat(x0 + x, y0 + y, z0 + z) == WATER) { float f = W.brick(x0 + x, y0 + y, z0 + z)->fuel[lidx(x0 + x, y0 + y, z0 + z)]; hcol = y + std::min(1.f, f > 0 ? f : 1.f); }
          a = std::min(a, hcol); b = std::max(b, hcol); sd += hcol; n++;
        }
        fprintf(stderr, "t=%.0fs surface %.2f..%.2f mean %.2f active %zu\n", (i + 1) / 60.f, a, b, sd / n, sim.fluidActive.size());
      }
      if (getenv("REALISM_DEBUG_WATER") && i % 20 == 0) { extern double g_wlost[10]; int nd = 0; for (auto& d : sim.debris) if (d.mat == WATER) nd++;
        double wt = 0; int outside = 0; W.forEach([&](int x, int y, int z, Brick* br, int l) { if (br->mat[l] == WATER) { float f = br->fuel[l] > 0 ? br->fuel[l] : 1.f; wt += f; if (x >= x0 - 2 && x <= x0 + N + 2 && z >= z0 - 2 && z <= z0 + N + 2 && (x < x0 || x >= x0 + N || z < z0 || z >= z0 + N || y < y0 || y >= y0 + H + 2)) { if (outside++ < 5) fprintf(stderr, "  out %d %d %d\n", x - x0, y - y0, z - z0); } } });
        fprintf(stderr, "world water %.1f, near-box-outside cells %d\n", wt, outside); fprintf(stderr, "i=%d mass %.1f drops %d lost %.2f %.2f %.2f %.2f unplaced %.2f killed %.2f merged %.2f deposited %.2f created %.2f\n", i, fillSum(), nd, g_wlost[0], g_wlost[1], g_wlost[2], g_wlost[3], g_wlost[4], g_wlost[5], g_wlost[6], g_wlost[7], g_wlost[8]); }
      if (getenv("REALISM_DEBUG_WATER") && i == 899) {
        for (int z = 0; z < N; z += 1) {
          for (int x = 0; x < N; x++) {
            float hcol = 0;
            for (int y = 0; y < H; y++) if (W.mat(x0 + x, y0 + y, z0 + z) == WATER) { float f = W.brick(x0 + x, y0 + y, z0 + z)->fuel[lidx(x0 + x, y0 + y, z0 + z)]; hcol = y + std::min(1.f, f > 0 ? f : 1.f); }
            fprintf(stderr, "%4.1f", hcol);
          }
          fprintf(stderr, "\n");
        }
        fprintf(stderr, "--\n");
      }
    }
    double m1 = fillSum();
    // Depth of each column (sum of fills, so compression counts as depth).
    double s1 = 0, s2 = 0;
    for (int x = 0; x < N; x++)
      for (int z = 0; z < N; z++) {
        float hcol = 0;
        for (int y = 0; y < H; y++) {
          if (W.mat(x0 + x, y0 + y, z0 + z) != WATER) continue;
          float f = W.brick(x0 + x, y0 + y, z0 + z)->fuel[lidx(x0 + x, y0 + y, z0 + z)];
          hcol += f > 0 ? f : 1.f;
        }
        s1 += hcol; s2 += hcol * hcol;
      }
    double mean = s1 / (N * N), sd = std::sqrt(std::max(0.0, s2 / (N * N) - mean * mean));
    { extern double g_wlost[10]; fprintf(stderr, "  lost: thin-new %.2f clobbered %.2f deleted %.2f tiny-drops %.2f\n", g_wlost[0], g_wlost[1], g_wlost[2], g_wlost[3]); }
    if (getenv("REALISM_DEBUG_WATER"))
      for (int y = 0; y < 4; y++) {
        double sum = 0, mx = 0; int cnt = 0;
        for (int x = 0; x < N; x++) for (int z = 0; z < N; z++) if (W.mat(x0 + x, y0 + y, z0 + z) == WATER) { float f = W.brick(x0 + x, y0 + y, z0 + z)->fuel[lidx(x0 + x, y0 + y, z0 + z)]; f = f > 0 ? f : 1; sum += f; mx = std::max(mx, (double)f); cnt++; }
        fprintf(stderr, "  layer %d: %d cells, fill %.1f, max %.3f\n", y, cnt, sum, mx);
      }
    fprintf(stderr, "  basin after 5 s: water %.0f -> %.0f, depth %.2f +- %.2f voxels\n", m0, m1, mean, sd);
    check(m1 > m0 * 0.95 && m1 < m0 * 1.05 && sd < 0.25, "water surges out and levels across a basin");
  }
  // Radiation: a glowing block warms a plank 20 cm away across open air.
  {
    Volume hv(8, 8, 8), pv(2, 8, 8);
    uint32_t hc = newComp(makePart("Hot block", "Test", joints::welded));
    uint32_t pc = newComp(makePart("Target plank", "Test", joints::welded));
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) for (int z = 0; z < 8; z++) { hv.set(x, y, z, STEEL, hc); }
    for (int x = 0; x < 2; x++) for (int y = 0; y < 8; y++) for (int z = 0; z < 8; z++) pv.set(x, y, z, PINE, pc);
    hv.forEach([&](int, int, int, Brick* br, int l) { br->temp[l] = 1100; });
    Body* hb = spawnBody(sim, std::move(hv), vec3(3.0f, 4.5f, 3.0f));
    Body* pb = spawnBody(sim, std::move(pv), vec3(3.0f + 0.1f + 0.2f + 0.025f, 4.5f, 3.0f));
    hb->frozen = pb->frozen = true;
    hb->heat.reset(hb->vol.bricks.size()); hb->heat.add(0);
    int pid = pb->id;
    for (int i = 0; i < 120; i++) sim.step(1.f / 60.f);
    float tmax = 0;
    for (auto& b : sim.bodies) if (b->id == pid) b->vol.forEach([&](int, int, int, Brick* br, int l) { tmax = std::max(tmax, br->temp[l]); });
    fprintf(stderr, "  plank facing a 1100 C block reached %.0f C (radiators %zu, %.0f W)\n", tmax, sim.radiators.size(), sim.radiantTotal);
    check(tmax > 40, "radiant heat crosses an air gap");
  }
  fprintf(stderr, "%d failure(s)\n", fails);
  return fails == 0;
}
