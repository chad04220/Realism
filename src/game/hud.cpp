// In-game HUD, menus and the help sheet.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <iterator>
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "game.h"
#include "../sim/save.h"
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
        row("Fuse", cp->fuse >= 0 ? "LIT" : cp->lit ? "Armed \xC2\xB7 G" : "Safe",
            cp->fuse >= 0 ? theme::danger : cp->lit ? theme::warm : theme::dim);
        break;
      case C_VALVE:
        row("Valve", cp->lit ? "Open" : "Closed", cp->lit ? theme::accent : theme::dim);
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
  std::string act;  // shown as a prompt under the crosshair instead
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
  // What the player feels: air temperature, wind and radiant heat at head height.
  if ((envTick_++ % 6) == 0 && sim.airReady) {
    airHere_ = sim.air.temperature(cam.pos);
    windHere_ = sim.air.velocity(cam.pos);
    radiant_ = sim.radiantFlux(cam.pos);
  }
  float want = clampf((radiant_ - 300.f) / 3000.f, 0.f, 1.f) + clampf((airHere_ - 38.f) / 90.f, 0.f, 1.f);
  warmth_ += (std::min(1.f, want) - warmth_) * std::min(1.f, dt * 3.f);
  drawHeatVignette();
  drawEnvironment(width() - 252 * s, 18 * s);
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
  drawPrompt(cx, cy + 42 * s);
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
  float y = (mode_ == M_WORKSHOP ? 552.f : (hud_ ? (radiant_ > 150 ? 112.f : 88.f) : 18.f)) * s;
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
                {"F3", "Performance stats"}, {"F5 / F9", "Quicksave / quickload"}, {"F11", "Fullscreen"},
                {"Esc", "Menu"}};
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
  float W = (float)width(), H = (float)height();
  if (menuPage_ == "saves") { drawSavesPage(); return; }
  if (menuPage_ == "main") {
    // Pause: the same left-hand column as the title screen.
    rect(0, 0, W, H, Color(0.02f, 0.03f, 0.05f, 0.25f));
    gradientH(0, 0, W * 0.55f, H, Color(0.02f, 0.03f, 0.05f, 0.62f), Color(0.02f, 0.03f, 0.05f, 0.f));
    float x = std::max(48 * s, W * 0.07f), y = H * 0.26f;
    text(x + 2 * s, y, "PAUSED", 13 * s, theme::accent, 0, 0.12f);
    text(x - 2 * s, y + 20 * s, "REALISM", 56 * s, theme::text, 0, 0.1f);
    float yy = y + 110 * s, bw = 300 * s, bh = 48 * s;
    if (button("Resume", x, yy, bw, bh, true)) menu_ = false;
    yy += bh + 10 * s;
    if (button("Settings", x, yy, bw, bh)) menuPage_ = "settings";
    yy += bh + 10 * s;
    if (button("Controls", x, yy, bw, bh)) { help_ = true; menu_ = false; }
    yy += bh + 10 * s;
    if (button("Save / load game", x, yy, bw, bh)) { menuPage_ = "saves"; saveListTime_ = -1; confirm_.clear(); }
    yy += bh + 10 * s;
    if (button("Reset world", x, yy, bw, bh)) { menu_ = false; resetWorld(); }
    yy += bh + 10 * s;
    if (button("Title screen", x, yy, bw, bh)) { menu_ = false; mode_ = M_PLAY; title_ = true; }
    yy += bh + 10 * s;
    if (button("Quit", x, yy, bw, bh)) quit_ = true;
    return;
  }
  // Settings: a glass sheet with tabs.
  rect(0, 0, W, H, Color(0.02f, 0.03f, 0.05f, 0.45f));
  float w = 700 * s, h = 600 * s, x = W * 0.5f - w * 0.5f, y = H * 0.5f - h * 0.5f;
  shadow(x, y, w, h, 20 * s, 34 * s, 0.55f);
  glass(x, y, w, h, theme::panelHi, theme::line, 20 * s);
  text(x + 36 * s, y + 28 * s, "Settings", 28 * s, theme::text, 0, 0.06f);
  static const char* tabs[] = {"Graphics", "Simulation", "Controls", "Audio"};
  float tx = x + 36 * s, ty = y + 80 * s;
  for (int i = 0; i < 4; i++) {
    float tw = textWidth(tabs[i], 15 * s) + 32 * s;
    bool sel = settingsTab_ == i;
    bool hv = hover(tx, ty, tw, 34 * s);
    if (hv) claimMouse();
    rect(tx, ty, tw, 34 * s, sel ? theme::accent.alpha(0.16f) : (hv ? Color(1, 1, 1, 0.07f) : Color(0, 0, 0, 0)), 17 * s);
    text(tx + tw * 0.5f, ty + 8 * s, tabs[i], 15 * s, sel ? theme::accent : theme::dim, 1, sel ? 0.05f : 0);
    if (hv && mouse().released) settingsTab_ = i;
    tx += tw + 6 * s;
  }
  line(x + 36 * s, ty + 46 * s, x + w - 36 * s, ty + 46 * s, 1, theme::line);
  float bx = x + 36 * s, bw = w - 72 * s, yy = ty + 66 * s;
  auto note = [&](const char* t) { text(bx, yy, t, 13 * s, theme::faint); yy += 26 * s; };
  if (settingsTab_ == 0) {
    if (toggle("Smooth surfaces", bx, yy, bw, ropt.smooth)) settings.smooth = ropt.smooth;
    yy += 38 * s;
    if (toggle("Sun shadows", bx, yy, bw, ropt.shadows)) settings.shadows = ropt.shadows;
    yy += 38 * s;
    if (toggle("Bloom", bx, yy, bw, ropt.bloom)) settings.bloom = ropt.bloom;
    yy += 38 * s;
    if (toggle("Volumetric smoke, steam and heat haze", bx, yy, bw, ropt.volumetrics)) settings.volumetrics = ropt.volumetrics;
    yy += 46 * s;
    if (slider("Render scale", bx, yy, bw, ropt.renderScale, 0.4f, 1.5f, "%.2f\xC3\x97")) settings.renderScale = ropt.renderScale;
    yy += 52 * s;
    if (slider("Exposure", bx, yy, bw, ropt.exposure, 0.5f, 2.f, "%.2f")) settings.exposure = ropt.exposure;
    yy += 52 * s;
    float fovDeg = cam.fov * 180 / PI;
    if (slider("Field of view", bx, yy, bw, fovDeg, 55, 100, "%.0f\xC2\xB0")) { cam.fov = fovDeg * PI / 180; settings.fov = fovDeg; }
    yy += 56 * s;
    note("Lower the render scale for more frame rate (0.75 suits most laptops).");
  } else if (settingsTab_ == 1) {
    if (slider("Thermal time warp", bx, yy, bw, sim.cfg.thermalWarp, 1, 60, "%.0f\xC3\x97")) settings.thermalWarp = sim.cfg.thermalWarp;
    yy += 52 * s;
    float wind = length(sim.air.wind);
    if (slider("Breeze", bx, yy, bw, wind, 0, 6, "%.1f m/s")) {
      vec3 dir = length(sim.air.wind) > 1e-3f ? normalize(sim.air.wind) : vec3(0.93f, 0, 0.36f);
      sim.air.wind = dir * wind;
    }
    yy += 56 * s;
    note("Heat runs faster than real time so fires and furnaces play out in seconds;");
    note("everything else, including air and water, runs in real time.");
    yy += 8 * s;
    note("Voxel size and world extent are set in settings.cfg (restart to apply).");
  } else if (settingsTab_ == 2) {
    slider("Mouse sensitivity", bx, yy, bw, settings.sensitivity, 0.2f, 3.f, "%.2f");
    yy += 52 * s;
    toggle("Invert mouse Y", bx, yy, bw, settings.invertY);
    yy += 46 * s;
    if (slider("Interface scale", bx, yy, bw, settings.uiScale, 0.75f, 1.5f, "%.2f\xC3\x97")) {}
    yy += 56 * s;
    note("F1 shows every control.");
  } else {
    if (slider("Master volume", bx, yy, bw, settings.volume, 0, 1, "%.2f")) audio.setVolume(settings.volume);
    yy += 56 * s;
  }
  float bh = 46 * s;
  if (button(title_ ? "Back to title" : "Back", x + w - 36 * s - 200 * s, y + h - 36 * s - bh, 200 * s, bh, true)) {
    menuPage_ = "main";
    if (title_) menu_ = false;
    settings.save(plat::userDataDir() + "/settings.cfg");
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
  // Settings round-trip: save() writes inline comments, so load() must strip them.
  {
    std::string p = plat::userDataDir() + "/selftest-settings.cfg";
    Settings s;
    s.voxelSize = 0.025f; s.worldX = 12; s.worldY = 6; s.worldZ = 14; s.thermalWarp = 7;
    s.threads = 3; s.width = 1280; s.height = 720; s.fullscreen = true; s.vsync = false;
    s.renderScale = 0.75f; s.smooth = false; s.shadows = true; s.bloom = false; s.volumetrics = true;
    s.fov = 61; s.exposure = 1.5f; s.uiScale = 1.25f; s.sensitivity = 2; s.invertY = true; s.volume = 0.5f;
    s.save(p);
    Settings t;
    t.load(p);
    std::remove(p.c_str());
    bool same = std::fabs(t.voxelSize - s.voxelSize) < 1e-5f && std::fabs(t.worldX - s.worldX) < 1e-5f &&
                std::fabs(t.worldY - s.worldY) < 1e-5f && std::fabs(t.worldZ - s.worldZ) < 1e-5f &&
                std::fabs(t.thermalWarp - s.thermalWarp) < 1e-5f && t.threads == s.threads &&
                t.width == s.width && t.height == s.height && t.fullscreen == s.fullscreen && t.vsync == s.vsync &&
                std::fabs(t.renderScale - s.renderScale) < 1e-5f && t.smooth == s.smooth && t.shadows == s.shadows &&
                t.bloom == s.bloom && t.volumetrics == s.volumetrics && std::fabs(t.fov - s.fov) < 1e-5f &&
                std::fabs(t.exposure - s.exposure) < 1e-5f && std::fabs(t.uiScale - s.uiScale) < 1e-5f &&
                std::fabs(t.sensitivity - s.sensitivity) < 1e-5f && t.invertY == s.invertY &&
                std::fabs(t.volume - s.volume) < 1e-5f;
    if (!same) fprintf(stderr, "  settings did not survive the round-trip\n");
    check(same, "settings save and load round-trip");
  }
  {
    std::string p = plat::userDataDir() + "/selftest-settings.cfg";
    {
      std::ofstream f(p);
      f << "thermal_warp = 5   # faster than real time\nfullscreen = yes\nvsync = off\nvoxel_size = 0.02\n";
    }
    Settings u;
    u.load(p);
    std::remove(p.c_str());
    check(std::fabs(u.thermalWarp - 5) < 1e-5f && u.fullscreen && !u.vsync && std::fabs(u.voxelSize - 0.02f) < 1e-5f,
          "settings accept inline comments and true/false words");
  }
  {
    Settings b;
    b.voxelSize = 0; b.worldX = 0; b.worldY = 1e9f; b.worldZ = -4; b.thermalWarp = -5;
    b.threads = -3; b.width = 0; b.height = 99999; b.renderScale = 100; b.fov = 0;
    b.exposure = 50; b.sensitivity = 0; b.volume = 5; b.uiScale = -1;
    b.sanitize();
    check(b.voxelSize >= 0.02f && b.voxelSize <= 0.025f && b.worldX >= 4 && b.worldY <= 16 && b.worldZ >= 4 &&
              b.thermalWarp >= 1 && b.threads >= 0 && b.width >= 320 && b.height <= 4320 && b.renderScale <= 2 &&
              b.fov >= 40 && b.exposure <= 4 && b.uiScale >= 0.5f && b.sensitivity > 0 && b.volume <= 1,
          "settings sanitize clamps out-of-range values");
  }
  // Every voxel of the shipped scene is held up by its neighbours: building it
  // must not overload a single bond (loose specks with no neighbours at all
  // may drop, but nothing may break).
  fprintf(stderr, "  scene structure: %ld bonds overloaded, %d collapses at build\n", sim.stats.bondsFailed, sim.stats.collapses);
  check(sim.stats.bondsFailed == 0, "the shipped scene holds itself up, bond by bond");
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
  // The wood stove draws: warm air rises up its flue.
  {
    float vmax = -1e9f, tmax = 0;
    for (float y = 2.6f; y <= 3.6f; y += 0.1f)
      for (float dx = -0.1f; dx <= 0.1f; dx += 0.1f)
        for (float dz = -0.1f; dz <= 0.1f; dz += 0.1f) {
          vec3 p(5.8f + dx, y, 4.7f + dz);
          vmax = std::max(vmax, sim.air.velocity(p).y);
          tmax = std::max(tmax, sim.air.temperature(p));
        }
    fprintf(stderr, "  stove flue: air rising at %.2f m/s, %.0f C\n", vmax, tmax);
    check(vmax > 0.3f && tmax > AMBIENT + 5, "wood stove draws air up its flue");
  }
  // Opening the water tower's valve sends out a jet with the tower's head behind it.
  {
    uint32_t gate = 0;
    for (uint32_t i = 0; i < comps().size(); i++) if (comps()[i].type == C_VALVE && comps()[i].object == "Water tower") gate = i;
    ivec3 near(0, 0, 0);
    if (gate) sim.world.forEach([&](int x, int y, int z, Brick* br, int l) { if (br->comp[l] == gate && br->aux[l] == 1) near = ivec3(x, y, z); });
    float vmax = 0;
    if (gate) {
      toggleValve(gate, -1, near);
      for (int i = 0; i < 60; i++) {
        sim.step(1.f / 60.f);
        for (auto& d : sim.debris) if (d.mat == WATER) vmax = std::max(vmax, length(vec3(d.v.x, 0, d.v.z)));
      }
      toggleValve(gate, -1, near);
    }
    fprintf(stderr, "  water tower: valve %s, jet %.2f m/s\n", gate ? "found" : "missing", vmax);
    check(gate && vmax > 3.f, "water tower valve jets water");
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
    // Water voxels in the basin, standing or in flight: whole voxels, so the
    // count must not change at all.
    const vec3 bLo = vec3((float)x0, (float)y0, (float)z0) * vs, bHi = vec3((float)(x0 + N), (float)(y0 + H + 2), (float)(z0 + N)) * vs;
    auto fillSum = [&]() {
      long m = 0;
      for (int x = 0; x < N; x++) for (int z = 0; z < N; z++) for (int y = 0; y < H + 2; y++)
        if (W.mat(x0 + x, y0 + y, z0 + z) == WATER) m++;
      for (auto& d : sim.debris)
        if (d.mat == WATER && d.p.x >= bLo.x && d.p.y >= bLo.y && d.p.z >= bLo.z && d.p.x < bHi.x && d.p.y < bHi.y && d.p.z < bHi.z) m++;
      return m;
    };
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
    long m0 = fillSum();
    for (int i = 0; i < 480; i++) sim.step(1.f / 60.f);
    long m1 = fillSum();
    // Depth of each column.
    double s1 = 0, s2 = 0;
    for (int x = 0; x < N; x++)
      for (int z = 0; z < N; z++) {
        float hcol = 0;
        for (int y = 0; y < H; y++) if (W.mat(x0 + x, y0 + y, z0 + z) == WATER) hcol += 1;
        s1 += hcol; s2 += hcol * hcol;
      }
    double mean = s1 / (N * N), sd = std::sqrt(std::max(0.0, s2 / (N * N) - mean * mean));
    fprintf(stderr, "  basin after 8 s: %ld -> %ld water voxels, depth %.2f +- %.2f voxels\n", m0, m1, mean, sd);
    check(m1 == m0 && sd < 0.5, "water surges out and levels across a basin, voxel for voxel");
    // Connected containers: two shafts joined at the bottom. Water poured into
    // one rises in the other until both stand at the same height.
    for (int x = -1; x <= N; x++) for (int z = -1; z <= N; z++) for (int y = 0; y <= H; y++) sim.putWorld(x0 + x, y0 + y, z0 + z, Cell{});
    for (int x = -1; x <= 12; x++)
      for (int z = -1; z <= 3; z++)
        for (int y = -1; y <= H + 1; y++) {
          bool shaftA = x >= 0 && x <= 2 && z >= 0 && z <= 2 && y >= 0;
          bool shaftB = x >= 9 && x <= 11 && z >= 0 && z <= 2 && y >= 0;
          bool channel = x >= 0 && x <= 11 && z >= 0 && z <= 2 && y >= 0 && y <= 1;
          Cell c;
          if (!(shaftA || shaftB || channel)) { c.mat = STONE; c.comp = bc; }
          sim.putWorld(x0 + x, y0 + y, z0 + z, c);
        }
    for (int x = 0; x <= 2; x++) for (int z = 0; z <= 2; z++) for (int y = 2; y < 18; y++) {
      Cell c; c.mat = WATER; sim.putWorld(x0 + x, y0 + y, z0 + z, c);
    }
    auto level = [&](int xa) {
      int top = -1;
      for (int y = 0; y <= H; y++) if (W.mat(x0 + xa, y0 + y, z0 + 1) == WATER) top = y;
      return top;
    };
    auto shaftWater = [&]() {
      long n = 0;
      for (int x = 0; x <= 11; x++) for (int z = 0; z <= 2; z++) for (int y = 0; y <= H; y++) if (W.mat(x0 + x, y0 + y, z0 + z) == WATER) n++;
      for (auto& d : sim.debris)
        if (d.mat == WATER && d.p.x >= x0 * vs && d.p.x < (x0 + 12) * vs && d.p.z >= z0 * vs && d.p.z < (z0 + 3) * vs && d.p.y >= y0 * vs) n++;
      return n;
    };
    const long u0 = shaftWater();
    for (int i = 0; i < 360; i++) sim.step(1.f / 60.f);
    int la = level(1), lb = level(10);
    const long u1 = shaftWater();
    fprintf(stderr, "  connected shafts after 6 s: levels %d and %d voxels, water %ld -> %ld\n", la, lb, u0, u1);
    check(u1 == u0 && la >= 2 && lb >= 2 && std::abs(la - lb) <= 1, "water finds one level in connected containers");
    for (int x = -1; x <= 12; x++) for (int z = -1; z <= 3; z++) for (int y = -1; y <= H + 1; y++) sim.putWorld(x0 + x, y0 + y, z0 + z, Cell{});
  }
  // Structure: nothing holds a voxel up but its neighbours.
  {
    Volume& W = sim.world;
    const float vs = sim.vs;
    const int gy = (int)std::lround(1.0f / vs);  // first cell above the lawn
    const int X0 = (int)(13.2f / vs), Z0 = (int)(0.6f / vs);
    auto clearArea = [&]() {
      for (int x = X0 - 2; x < X0 + (int)(2.4f / vs); x++)
        for (int z = Z0 - 2; z < Z0 + (int)(1.6f / vs); z++)
          for (int y = gy; y < gy + (int)(1.2f / vs); y++) if (W.mat(x, y, z) != AIR) sim.removeWorld(x, y, z, false);
      for (int i = 0; i < 12; i++) sim.step(1.f / 60.f);
      // Anything the clearing knocked loose has been carried away.
      for (size_t i = 0; i < sim.bodies.size();) {
        Body& b = *sim.bodies[i];
        vec3 c = b.pos / vs;
        if (!b.frozen && c.x > X0 - 8 && c.x < X0 + 2.4f / vs + 8 && c.z > Z0 - 8 && c.z < Z0 + 1.6f / vs + 8) sim.removeBody((int)i);
        else i++;
      }
      sim.debris.clear();
    };
    auto put = [&](int x, int y, int z, uint8_t m, uint32_t c) { Cell cell; cell.mat = m; cell.comp = c; sim.putWorld(x, y, z, cell); };
    auto standing = [&](int x, int y, int z) { return W.mat(x, y, z) != AIR; };
    // Check the edits now (as a player's own edit would be), cascade a few
    // times, and give what broke a moment to fall clear.
    auto settle = [&]() {
      for (int i = 0; i < 4; i++) { sim.flushStructure(); sim.step(1.f / 60.f); }
      for (int i = 0; i < 30; i++) sim.step(1.f / 60.f);
    };
    // A stone pillar to build out from: anchored, column after column, on the lawn.
    const int H = (int)std::lround(0.5f / vs), P = 4;
    auto pillar = [&]() {
      uint32_t pc = newComp(makePart("Test pillar", "Test", joints::cast));
      for (int x = X0; x < X0 + P; x++) for (int z = Z0; z < Z0 + P; z++) for (int y = gy; y < gy + H; y++) put(x, y, z, STONE, pc);
    };
    // A cantilever of material m sticking out of the pillar's side near its
    // top: depth d and length L in metres, joined to the pillar as one part.
    auto cantilever = [&](uint8_t m, float d, float L) {
      clearArea();
      pillar();
      settle();
      uint32_t bc = newComp(makePart("Test beam", "Test", joints::cast));
      const int nd = std::max(1, (int)std::lround(d / vs)), nl = std::max(1, (int)std::lround(L / vs));
      for (int x = X0 + P; x < X0 + P + nl; x++)
        for (int z = Z0; z < Z0 + P; z++)
          for (int y = gy + H - nd; y < gy + H; y++) put(x, y, z, m, bc);
      settle();
      settle();
      return standing(X0 + P + nl - 1, gy + H - 1, Z0 + 1);
    };
    // Soil is cohesive but weak: beam theory gives L = sqrt(sigma d / (3 rho g))
    // ~ 0.18 m for a 10 cm deep soil cantilever.
    const float Lsoil = std::sqrt(MATS[DIRT].tensile * 0.1f / (3 * MATS[DIRT].density * 9.81f));
    bool shortSoil = cantilever(DIRT, 0.1f, Lsoil * 0.75f);
    bool longSoil = cantilever(DIRT, 0.1f, Lsoil * 1.35f);
    bool longSteel = cantilever(STEEL, 0.1f, 0.35f);
    fprintf(stderr, "  cantilevers (predicted soil limit %.2f m): soil %.2f m %s, soil %.2f m %s, steel 0.35 m %s\n", Lsoil,
            Lsoil * 0.75f, shortSoil ? "stands" : "falls", Lsoil * 1.35f, longSoil ? "stands" : "falls", longSteel ? "stands" : "falls");
    check(shortSoil && !longSoil && longSteel, "a cantilever stands or breaks as beam theory says");
    // Resting is not attached: a brick merely leaning on a wall's side falls;
    // the same brick glued there stays.
    auto sideBrick = [&](const Joint& j) {
      clearArea();
      pillar();
      settle();
      uint32_t bc = newComp(makePart("Test brick", "Test", j));
      for (int x = X0 + P; x < X0 + P + 4; x++) for (int z = Z0; z < Z0 + 2; z++) for (int y = gy + H - 3; y < gy + H; y++) put(x, y, z, BRICK, bc);
      settle();
      return standing(X0 + P + 1, gy + H - 2, Z0);
    };
    bool resting = sideBrick(joints::resting), glued = sideBrick(joints::glued);
    fprintf(stderr, "  brick against a wall: resting %s, glued %s\n", resting ? "stays" : "falls", glued ? "stays" : "falls");
    check(!resting && glued, "only what is attached is held: a resting brick falls, a glued one stays");
    // Tension: a steel block hung from a beam by a single voxel.
    auto hung = [&](uint8_t link) {
      clearArea();
      pillar();
      settle();
      uint32_t arm = newComp(makePart("Test arm", "Test", joints::cast));
      for (int x = X0 + P; x < X0 + P + 12; x++) for (int z = Z0; z < Z0 + P; z++) for (int y = gy + H - 4; y < gy + H; y++) put(x, y, z, STEEL, arm);
      settle();
      uint32_t lc = newComp(makePart("Test link", "Test", joints::cast));
      put(X0 + P + 9, gy + H - 5, Z0 + 1, link, lc);
      uint32_t blk = newComp(makePart("Test weight", "Test", joints::cast));
      for (int x = X0 + P + 8; x < X0 + P + 12; x++) for (int z = Z0; z < Z0 + 4; z++) for (int y = gy + H - 9; y < gy + H - 5; y++) put(x, y, z, STEEL, blk);
      // The weight and its link are one part with the link's material only where the link is.
      settle();
      settle();
      return standing(X0 + P + 9, gy + H - 7, Z0 + 1);
    };
    bool soilLink = hung(DIRT), steelLink = hung(STEEL);
    fprintf(stderr, "  steel block hung by one voxel: soil link %s, steel link %s\n", soilLink ? "holds" : "snaps", steelLink ? "holds" : "snaps");
    check(!soilLink && steelLink, "a hanging weight snaps a weak link and not a strong one");
    clearArea();
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
  // External forces are accumulated once per frame and must be integrated once
  // per frame, not once per physics substep. A 10 N/kg push for 1/60 s should
  // add 0.167 m/s regardless of how many substeps the solver takes.
  {
    vec3 savedGravity = sim.gravity;
    sim.gravity = vec3(0);
    Volume v(2, 2, 2);
    uint32_t c = newComp(makePart("Test cube", "Test", joints::welded));
    for (int x = 0; x < 2; x++) for (int y = 0; y < 2; y++) for (int z = 0; z < 2; z++) v.set(x, y, z, STEEL, c);
    Body* b = spawnBody(sim, std::move(v), vec3(2.0f, 6.5f, 2.0f));
    if (b) {
      b->wake();
      b->force = vec3(10.f * b->mass, 0, 0);
      const float v0 = b->vel.x;
      sim.step(1.f / 60.f);
      const float dv = b->vel.x - v0;
      fprintf(stderr, "  unit force: dv = %.4f m/s (expect %.4f)\n", dv, 10.f / 60.f);
      check(std::fabs(dv - 10.f / 60.f) < 0.02f, "external forces are applied once per frame");
      int bi = sim.bodyIndex(b);
      if (bi >= 0) sim.removeBody(bi);
    } else {
      check(false, "spawned a body for the force test");
    }
    sim.gravity = savedGravity;
  }
  // Saved games. The world at this point has been burned, flooded, blasted and
  // driven by the checks above, so it exercises every section of the format.
  {
    auto fileBytes = [](const std::string& p) {
      std::ifstream f(p, std::ios::binary);
      return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    auto writeBytes = [](const std::string& p, const std::vector<char>& b) {
      std::ofstream f(p, std::ios::binary | std::ios::trunc);
      f.write(b.data(), (std::streamsize)b.size());
    };
    // Never touch the player's own saves (the autosave check deletes files).
    Game::saveDir = plat::userDataDir() + "/selftest-saves-" + std::to_string((long long)(plat::now() * 1e6) % 1000000007);
    for (int i = 0; i < 30; i++) sim.step(1.f / 60.f);
    std::vector<std::pair<std::string, uint64_t>> parts0, parts1;
    const uint64_t h0 = save::stateHash(sim, &parts0);
    const size_t bodies0 = sim.bodies.size(), debris0 = sim.debris.size(), voxels0 = sim.world.countSolid();
    const float airT0 = *std::max_element(sim.air.T.begin(), sim.air.T.end());
    pl_.pos = vec3(3.25f, 1.5f, 7.75f);
    pl_.yaw = 1.25f;
    tool_ = T_TORCH;
    double t0 = plat::now();
    std::string err;
    bool saved = saveGame("selftest", &err);
    double tSave = plat::now() - t0;
    const std::string path = savePath("selftest");
    const std::vector<char> good = fileBytes(path);
    fprintf(stderr, "  save: %.1f MB in %.2f s (%zu bodies, %zu debris, %zu M voxels)%s%s\n", good.size() / 1048576.0, tSave,
            bodies0, debris0, voxels0 / 1000000, saved ? "" : " FAILED: ", err.c_str());
    check(saved && !good.empty(), "game saves to disk");

    // Change everything the load must undo.
    for (int i = 0; i < 20; i++) sim.step(1.f / 60.f);
    sim.removeWorld(sim.world.nx / 2, 10, sim.world.nz / 2, false);
    pl_.pos = vec3(1, 1, 1);
    tool_ = T_HAND;
    const uint64_t hChanged = save::stateHash(sim);
    t0 = plat::now();
    bool loaded = loadGame("selftest", &err);
    double tLoad = plat::now() - t0;
    const uint64_t h1 = save::stateHash(sim, &parts1);
    for (size_t i = 0; i < parts0.size() && i < parts1.size(); i++)
      if (parts0[i] != parts1[i]) { fprintf(stderr, "  first difference after load: %s\n", parts0[i].first.c_str()); break; }
    const float airT1 = *std::max_element(sim.air.T.begin(), sim.air.T.end());
    fprintf(stderr, "  load: %.2f s, state hash %016llx (changed %016llx) -> %016llx, hottest air %.2f -> %.2f C%s%s\n",
            tLoad, (unsigned long long)h0, (unsigned long long)hChanged, (unsigned long long)h1, airT0, airT1,
            loaded ? "" : " FAILED: ", err.c_str());
    check(loaded && hChanged != h0 && h0 == h1 && sim.bodies.size() == bodies0 && sim.debris.size() == debris0 &&
              sim.world.countSolid() == voxels0,
          "save/load round-trip restores the exact world, bodies and parts");
    check(loaded && airT1 == airT0 &&
              length(pl_.pos - vec3(3.25f, 1.5f, 7.75f)) < 1e-5f && std::fabs(pl_.yaw - 1.25f) < 1e-5f && tool_ == T_TORCH,
          "save/load restores the air, the player and the tool");
    // The loaded world must keep simulating.
    for (int i = 0; i < 30; i++) sim.step(1.f / 60.f);
    bool sane = true;
    for (auto& b : sim.bodies)
      if (!std::isfinite(b->pos.x + b->pos.y + b->pos.z) || b->pos.y < -1) {
        sane = false;
        fprintf(stderr, "  body %d: %zu voxels, pos %.2f %.2f %.2f, vel %.2f %.2f %.2f, asleep %d\n", b->id, b->voxels,
                b->pos.x, b->pos.y, b->pos.z, b->vel.x, b->vel.y, b->vel.z, (int)b->asleep);
      }
    check(sane, "a loaded world keeps simulating");

    // Damaged files are refused before the live world is touched.
    const uint64_t hLive = save::stateHash(sim);
    std::vector<char> bad = good;
    bad[bad.size() / 2] ^= 0x5A;
    writeBytes(path, bad);
    bool corruptRejected = !loadGame("selftest", &err) && save::stateHash(sim) == hLive;
    fprintf(stderr, "  corrupt byte: %s\n", err.c_str());
    bad.assign(good.begin(), good.begin() + good.size() * 2 / 3);
    writeBytes(path, bad);
    bool truncRejected = !loadGame("selftest", &err) && save::stateHash(sim) == hLive;
    fprintf(stderr, "  truncated: %s\n", err.c_str());
    bad = good;
    bad[4] = 99;  // format version from the future
    writeBytes(path, bad);
    bool futureRejected = !loadGame("selftest", &err) && save::stateHash(sim) == hLive;
    fprintf(stderr, "  newer format: %s\n", err.c_str());
    bool missingRejected = !loadGame("selftest-missing", &err) && !loadGame("../escape", &err);
    check(corruptRejected && truncRejected && futureRejected && missingRejected,
          "corrupt, truncated, future and missing saves are refused without touching the world");

#ifndef _WIN32
    // Killing the game mid-save (kill -9, a crash, a power button) must never
    // cost the player the save they already had: the old file stays in place
    // until the new one is complete. A child process writes the slot over and
    // over and is killed at a different moment each time.
    {
      saveGame("killtest", &err);
      std::string header;
      std::vector<uint8_t> payload;
      bool readable = save::readFile(savePath("killtest"), header, payload, err);
      int survived = 0, tries = 12, midWrite = 0;
      for (int k = 0; k < tries && readable; k++) {
        fflush(stderr);
        pid_t pid = fork();
        if (pid == 0) {
          std::string e;
          for (;;) save::writeFile(savePath("killtest"), header, payload, e);
        }
        std::error_code tec;
        const std::string tmp = savePath("killtest") + ".tmp";
        if (k % 2 == 0) {
          // Kill the moment the new file exists: always before it has been
          // renamed over the old one, however fast the disk is.
          double until = plat::now() + 2;
          while (!std::filesystem::exists(tmp, tec) && plat::now() < until) {}
        } else {
          usleep(3000 + k * 4500);  // and at assorted other moments
        }
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        if (std::filesystem::exists(tmp, tec)) midWrite++;  // died before the rename
        std::filesystem::remove(tmp, tec);
        std::string h2;
        std::vector<uint8_t> p2;
        if (save::readFile(savePath("killtest"), h2, p2, err) && p2 == payload) survived++;
        else fprintf(stderr, "  killed writer %d: %s\n", k, err.c_str());
      }
      fprintf(stderr, "  killed the writer %d times (%d before its rename): %d left a loadable save\n", tries, midWrite,
              survived);
      check(readable && survived == tries && midWrite > 0, "killing the game mid-save never damages the existing save");
    }
#endif
    // Autosave keeps the previous file; a corrupt newest autosave falls back to it.
    std::error_code ec;
    std::filesystem::remove(savePath("autosave"), ec);
    std::filesystem::remove(savePath("autosave.1"), ec);
    double tA = plat::now();
    bool a1 = autosave(true);  // the in-game path: encode now, write on a thread
    double tBlock = plat::now() - tA;
    finishPendingSave();
    a1 = a1 && saveExists("autosave");
    fprintf(stderr, "  background autosave blocked the game for %.0f ms\n", tBlock * 1000);
    const uint64_t hA = save::stateHash(sim);
    for (int i = 0; i < 10; i++) sim.step(1.f / 60.f);
    bool a2 = autosave();
    std::vector<char> newest = fileBytes(savePath("autosave"));
    if (!newest.empty()) newest.resize(newest.size() / 2);
    writeBytes(savePath("autosave"), newest);
    err.clear();
    bool fellBack = loadNewestAutosave(&err) && save::stateHash(sim) == hA;
    if (!fellBack) fprintf(stderr, "  autosave fallback: %s (hash %016llx, wanted %016llx)\n", err.c_str(),
                           (unsigned long long)save::stateHash(sim), (unsigned long long)hA);
    check(a1 && a2 && fellBack, "a corrupt autosave falls back to the previous one");
    std::filesystem::remove_all(Game::saveDir, ec);
    Game::saveDir.clear();
    toasts_.clear();
  }
  // Re-initialising a Sim is how "new world" works: no clock, body, effect,
  // radiator, flow field or statistic may survive it.
  {
    sim.time = 12.f;
    sim.stats.burning = 7;
    sim.radiators.push_back(Radiator{});
    sim.events.push_back(SimEvent{});
    sim.waterFlow.push_back(vec3(1, 2, 3));
    sim.init(sim.cfg);
    check(sim.time == 0 && sim.bodies.empty() && sim.debris.empty() && sim.fx.empty() && sim.events.empty() &&
              sim.radiators.empty() && sim.waterFlow.empty() && sim.stats.burning == 0 && !sim.airReady &&
              sim.world.countSolid() == 0,
          "Sim::init clears every per-run system");
  }
  fprintf(stderr, "%d failure(s)\n", fails);
  return fails == 0;
}

// A key cap and what it does, under the crosshair, for things you can use.
void Game::drawPrompt(float cx, float cy) {
  if (!hit_.hit || hit_.t > 4.f || !hover_.comp) return;
  const Component& cp = comp(hover_.comp);
  const char* act = nullptr;
  switch (cp.type) {
    case C_SWITCH: act = "Flip switch"; break;
    case C_MOTOR: act = "Reverse motor"; break;
    case C_CHARGE: act = cp.fuse >= 0 || cp.lit ? nullptr : "Light the fuse"; break;
    case C_ROCKET: act = cp.lit ? nullptr : "Ignite"; break;
    case C_BATTERY: act = "Check charge"; break;
    case C_VALVE: act = cp.lit ? "Close valve" : "Open valve"; break;
    default: break;
  }
  if (!act) return;
  float s = scale();
  float tw = textWidth(act, 15 * s);
  float k = 26 * s, w = k + 10 * s + tw + 18 * s, x = cx - w * 0.5f;
  glass(x, cy, w, 34 * s, theme::panel, theme::line, 17 * s);
  rectBorder(x + 5 * s, cy + 4 * s, k, k, Color(1, 1, 1, 0.92f), Color(1, 1, 1, 0.92f), 7 * s);
  text(x + 5 * s + k * 0.5f, cy + 8 * s, "E", 15 * s, Color::hex(0x10141b), 1, 0.12f);
  text(x + k + 14 * s, cy + 8 * s, act, 15 * s, theme::text);
}

// Top right: air temperature, a wind compass relative to where you look, and
// radiant heat when something hot is shining on you.
void Game::drawEnvironment(float x, float y) {
  float s = scale();
  float w = 234 * s, h = (radiant_ > 150 ? 86.f : 62.f) * s;
  shadow(x, y, w, h, 14 * s, 14 * s, 0.3f);
  glass(x, y, w, h, theme::panel, theme::line, 14 * s);
  // Temperature.
  icon(I_THERMO, x + 22 * s, y + 31 * s, 22 * s, tempColor(airHere_));
  char b[48];
  snprintf(b, sizeof b, "%.0f\xC2\xB0", airHere_);
  text(x + 40 * s, y + 10 * s, b, 26 * s, theme::text, 0, 0.05f);
  text(x + 41 * s, y + 41 * s, "AIR", 11 * s, theme::faint, 0, 0.08f);
  // Wind compass.
  float ccx = x + 140 * s, ccy = y + 31 * s, r = 19 * s;
  ring(ccx, ccy, r, 1.5f * s, Color(1, 1, 1, 0.18f));
  for (int i = 0; i < 4; i++) {
    float aa = i * PI * 0.5f;
    line(ccx + std::sin(aa) * (r - 4 * s), ccy - std::cos(aa) * (r - 4 * s), ccx + std::sin(aa) * r, ccy - std::cos(aa) * r, 1.5f * s, Color(1, 1, 1, 0.25f));
  }
  vec3 fw = normalize(vec3(cam.forward.x, 0, cam.forward.z) + vec3(1e-5f, 0, 0)), rt = normalize(vec3(cam.right.x, 0, cam.right.z) + vec3(1e-5f, 0, 0));
  float wx = dot(windHere_, rt), wz = dot(windHere_, fw);
  float spd = std::sqrt(wx * wx + wz * wz);
  if (spd > 0.05f) {
    // The arrow points where the wind is going: up = away from you.
    float ux = wx / spd, uy = -wz / spd;
    float L = r - 5 * s;
    vec2 tip(ccx + ux * L, ccy + uy * L), tail(ccx - ux * L * 0.7f, ccy - uy * L * 0.7f);
    line(tail.x, tail.y, tip.x, tip.y, 2.5f * s, theme::accent);
    float hx = -uy, hy = ux;
    line(tip.x, tip.y, tip.x - ux * 7 * s + hx * 5 * s, tip.y - uy * 7 * s + hy * 5 * s, 2.5f * s, theme::accent);
    line(tip.x, tip.y, tip.x - ux * 7 * s - hx * 5 * s, tip.y - uy * 7 * s - hy * 5 * s, 2.5f * s, theme::accent);
  } else circle(ccx, ccy, 3 * s, theme::accent);
  snprintf(b, sizeof b, "%.1f", spd);
  text(x + 170 * s, y + 13 * s, b, 18 * s, theme::text, 0, 0.04f);
  text(x + 170 * s, y + 36 * s, "WIND M/S", 11 * s, theme::faint, 0, 0.08f);
  if (radiant_ > 150) {
    line(x + 14 * s, y + 62 * s, x + w - 14 * s, y + 62 * s, 1, theme::line);
    if (radiant_ >= 1000) snprintf(b, sizeof b, "Radiant heat  %.1f kW/m\xC2\xB2", radiant_ / 1000.f);
    else snprintf(b, sizeof b, "Radiant heat  %.0f W/m\xC2\xB2", radiant_);
    icon(I_FLAME, x + 22 * s, y + 74 * s, 14 * s, theme::warm);
    text(x + 36 * s, y + 66 * s, b, 13 * s, radiant_ > 4000 ? theme::danger : theme::warm);
  }
}

// The screen edges glow and pulse when radiant heat or hot air is on you.
void Game::drawHeatVignette() {
  if (warmth_ < 0.01f) return;
  float s = scale();
  float k = warmth_ * (0.85f + 0.15f * std::sin(time_ * 6.f));
  Color hot = Color::hex(0xff6a1a, 0.42f * k), none = Color::hex(0xff6a1a, 0.f);
  float W = (float)width(), H = (float)height(), e = std::min(W, H) * 0.22f;
  gradient(0, 0, W, e, hot, none);
  gradient(0, H - e, W, e, none, hot);
  gradientH(0, 0, e, H, hot, none);
  gradientH(W - e, 0, e, H, none, hot);
  (void)s;
}

// The title screen: the live yard behind, the name and a column of choices.
void Game::drawTitle() {
  float s = scale();
  float W = (float)width(), H = (float)height();
  // Darken the left side so the column reads over the scene.
  gradientH(0, 0, W * 0.62f, H, Color(0.02f, 0.03f, 0.05f, 0.66f), Color(0.02f, 0.03f, 0.05f, 0.f));
  gradient(0, H * 0.7f, W, H * 0.3f, Color(0, 0, 0, 0), Color(0.02f, 0.03f, 0.05f, 0.55f));
  float x = std::max(48 * s, W * 0.07f), y = H * 0.24f;
  text(x + 2 * s, y, "A VOXEL PHYSICS SANDBOX", 13 * s, theme::accent, 0, 0.12f);
  text(x - 4 * s, y + 22 * s, "REALISM", 96 * s, theme::text, 0, 0.1f);
  rect(x, y + 140 * s, 64 * s, 3 * s, theme::accent, 1.5f * s);
  textWrapped(x, y + 160 * s, 440 * s,
              "Every object is built from the parts that make it work, and every part from a real material. "
              "Heat, air, water, current and motion move between them.",
              16 * s, theme::dim, 1.4f);
  float by = y + 250 * s, bw = 300 * s, bh = 50 * s;
  if (saveExists("autosave") || saveExists("autosave.1")) {
    if (button("Continue", x, by, bw, bh, true)) {
      std::string err;
      if (loadNewestAutosave(&err)) { toast("Continuing from your last session", theme::accent, I_SAVE); audio.playUi(SFX_UI); }
      else toast("Couldn't load the autosave: " + err, theme::danger, I_SAVE);
    }
    by += bh + 12 * s;
  }
  if (button("Play", x, by, bw, bh, !saveExists("autosave") && !saveExists("autosave.1"))) {
    title_ = false;
    toast("Welcome to Realism \xE2\x80\x94 press F1 for controls", theme::accent, I_GEAR);
    toast("Everything here is built from real parts. Take it apart.", theme::text);
    audio.playUi(SFX_UI);
  }
  by += bh + 12 * s;
  if (button("Assembly workshop", x, by, bw, bh)) { title_ = false; mode_ = M_WORKSHOP; workshop->enter(); audio.playUi(SFX_UI); }
  by += bh + 12 * s;
  if (!listSaves().empty()) {
    if (button("Load game", x, by, bw, bh)) { menu_ = true; menuPage_ = "saves"; saveListTime_ = -1; confirm_.clear(); }
    by += bh + 12 * s;
  }
  if (button("Settings", x, by, bw, bh)) { menu_ = true; menuPage_ = "settings"; }
  by += bh + 12 * s;
  if (button("Quit", x, by, bw, bh)) quit_ = true;
  // Live readout of the world behind.
  char b[160];
  snprintf(b, sizeof b, "%.1f cm voxels  \xC2\xB7  %zu M voxels  \xC2\xB7  %d burning  \xC2\xB7  breeze %.1f m/s", sim.vs * 100,
           sim.world.countSolid() / 1000000, sim.stats.burning, length(sim.air.wind) * (1 + sim.air.gust));
  text(x, H - 46 * s, b, 13 * s, theme::faint);
  text(W - 28 * s, H - 46 * s, "F11 fullscreen", 13 * s, theme::faint, 2);
}

// ------------------------------------------------------------- saved games

void Game::typeSaveName(plat::Input& in) {
  if (!typingSave_) return;
  for (char c : in.text) {
    std::string t = saveName_ + c;
    if (saveName_.size() < 40 && (unsigned char)c >= 0x20 && (validSlot(t) || c == ' ' || c == '.')) saveName_ = t;
  }
  in.text.clear();
  if (in.pressed[key::Backspace] && !saveName_.empty()) {
    saveName_.pop_back();
    while (!saveName_.empty() && ((unsigned char)saveName_.back() & 0xC0) == 0x80) saveName_.pop_back();  // whole UTF-8 char
  }
  if (in.pressed[key::Enter]) typingSave_ = false;
}

void Game::drawSavesPage() {
  float s = scale();
  float W = (float)width(), H = (float)height();
  if (saveListTime_ < 0 || plat::now() - saveListTime_ > 1.0) { saveList_ = listSaves(); saveListTime_ = plat::now(); }
  rect(0, 0, W, H, Color(0.02f, 0.03f, 0.05f, 0.45f));
  float w = 720 * s, h = std::min(H - 60 * s, 640 * s), x = W * 0.5f - w * 0.5f, y = H * 0.5f - h * 0.5f;
  shadow(x, y, w, h, 20 * s, 34 * s, 0.55f);
  glass(x, y, w, h, theme::panelHi, theme::line, 20 * s);
  text(x + 36 * s, y + 28 * s, "Saved games", 28 * s, theme::text, 0, 0.06f);
  const float bx = x + 36 * s, bw = w - 72 * s;
  float yy = y + 82 * s;
  auto fmtWhen = [](long long t) {
    if (t <= 0) return std::string("unknown date");
    std::time_t tt = (std::time_t)t;
    char b[64];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", std::localtime(&tt));
    return std::string(b);
  };
  // Save the current world under a name (not from the title screen: there is
  // no game in progress to save there).
  if (!title_) {
    float fw = bw - 140 * s;
    bool hvName = hover(bx, yy, fw, 44 * s);
    if (hvName) claimMouse();
    if (hvName && mouse().released) typingSave_ = true;
    else if (mouse().released && !hvName) typingSave_ = false;
    rectBorder(bx, yy, fw, 44 * s, Color(0, 0, 0, 0.25f), typingSave_ ? theme::accent : theme::line, 8 * s);
    std::string nm = saveName_ + (typingSave_ && std::fmod(plat::now(), 1.0) < 0.5 ? "|" : "");
    text(bx + 14 * s, yy + 12 * s, nm.empty() ? std::string("Name this save") : nm, 15 * s, typingSave_ ? theme::text : theme::dim);
    bool exists = false;
    for (auto& si : saveList_) if (si.slot == saveName_) exists = true;
    bool armed = confirm_ == "over:" + saveName_;
    const char* label = exists ? (armed ? "Overwrite" : "Save") : "Save";
    if (button(label, bx + fw + 12 * s, yy, 128 * s, 44 * s, true, validSlot(saveName_))) {
      if (exists && !armed) confirm_ = "over:" + saveName_;
      else {
        std::string err;
        typingSave_ = false;
        confirm_.clear();
        if (saveGame(saveName_, &err)) { toast("Saved \"" + saveName_ + "\"", theme::good, I_SAVE); menu_ = false; }
        else toast("Save failed: " + err, theme::danger, I_SAVE);
        saveListTime_ = -1;
      }
    }
    yy += 52 * s;
    if (exists) text(bx, yy, armed ? "A save with this name exists \xE2\x80\x94 click Overwrite to replace it." : "A save with this name exists.", 13 * s, armed ? theme::warm : theme::faint);
    else if (!validSlot(saveName_) && !saveName_.empty()) text(bx, yy, "Names can't start or end with a space or a dot.", 13 * s, theme::faint);
    yy += 30 * s;
  }
  line(bx, yy, bx + bw, yy, 1, theme::line);
  yy += 12 * s;
  // The list.
  const float rowH = 62 * s, listTop = yy, listH = y + h - 80 * s - listTop;
  if (saveList_.empty()) text(bx, yy + 10 * s, "No saves yet. F5 quicksaves at any time.", 15 * s, theme::dim);
  float contentH = saveList_.size() * rowH;
  if (hover(bx, listTop, bw, listH)) saveScroll_ -= mouse().wheel * rowH;
  saveScroll_ = std::clamp(saveScroll_, 0.f, std::max(0.f, contentH - listH));
  pushClip(bx, listTop, bw, listH);
  std::string loadSlot, delSlot;
  for (size_t i = 0; i < saveList_.size(); i++) {
    const SaveInfo& si = saveList_[i];
    float ry = listTop + i * rowH - saveScroll_;
    if (ry + rowH < listTop || ry > listTop + listH) continue;
    bool rowHover = hover(bx, ry, bw, rowH - 6 * s) && mouse().y >= listTop && mouse().y <= listTop + listH;
    rect(bx, ry, bw, rowH - 6 * s, rowHover ? Color(1, 1, 1, 0.06f) : Color(0, 0, 0, 0.12f), 10 * s);
    std::string title = si.slot == "quicksave" ? "Quicksave" : si.slot == "autosave" ? "Autosave" :
                        si.slot == "autosave.1" ? "Previous autosave" : si.slot;
    text(bx + 16 * s, ry + 9 * s, title, 16 * s, theme::text);
    char sub[160];
    int mins = (int)(si.simTime / 60), secs = (int)si.simTime % 60;
    snprintf(sub, sizeof sub, "%s  \xC2\xB7  %d:%02d in the world  \xC2\xB7  %.1f MB", fmtWhen(si.savedUnix).c_str(), mins, secs,
             si.bytes / 1048576.0);
    text(bx + 16 * s, ry + 33 * s, sub, 12.5f * s, theme::faint);
    bool armed = confirm_ == "del:" + si.slot;
    if (button(armed ? "Delete?" : "Delete", bx + bw - 236 * s, ry + 9 * s, 104 * s, 38 * s)) {
      if (armed) delSlot = si.slot; else confirm_ = "del:" + si.slot;
    }
    if (button("Load", bx + bw - 120 * s, ry + 9 * s, 104 * s, 38 * s, true)) loadSlot = si.slot;
  }
  popClip();
  if (!delSlot.empty()) { deleteSave(delSlot); confirm_.clear(); saveListTime_ = -1; }
  if (!loadSlot.empty()) {
    std::string err;
    confirm_.clear();
    typingSave_ = false;
    if (loadGame(loadSlot, &err)) toast("Loaded \"" + loadSlot + "\"", theme::accent, I_SAVE);
    else toast("Couldn't load \"" + loadSlot + "\": " + err, theme::danger, I_SAVE);
  }
  if (button("Back", bx, y + h - 64 * s, 140 * s, 42 * s)) {
    typingSave_ = false;
    confirm_.clear();
    if (title_) menu_ = false; else menuPage_ = "main";
  }
  text(bx + bw, y + h - 52 * s, "F5 quicksave  \xC2\xB7  F9 quickload", 13 * s, theme::faint, 2);
}
