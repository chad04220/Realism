#include "settings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

void trim(std::string& s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  s = s.substr(i);
}

bool parseFloat(const std::string& s, float& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  float v = std::strtof(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
  out = v;
  return true;
}

bool parseBool(const std::string& s, bool& out) {
  if (s == "1" || s == "true" || s == "yes" || s == "on") { out = true; return true; }
  if (s == "0" || s == "false" || s == "no" || s == "off") { out = false; return true; }
  return false;
}

float clampF(float v, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : lo; }

}  // namespace

void Settings::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return;
  std::string line;
  while (std::getline(f, line)) {
    // Comments may trail a value: save() writes them, so strip them here too.
    auto hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    trim(k); trim(v);
    if (k.empty() || v.empty()) continue;
    float fv = 0;
    bool bv = false;
    const bool numeric = parseFloat(v, fv);
    const bool boolean = parseBool(v, bv);
    // A malformed value keeps whatever the previous file (or the defaults) set.
    auto number = [&](float& dst) { if (numeric) dst = fv; };
    auto flag = [&](bool& dst) { if (boolean) dst = bv; };
    if (k == "voxel_size") number(voxelSize);
    else if (k == "world_x") number(worldX);
    else if (k == "world_y") number(worldY);
    else if (k == "world_z") number(worldZ);
    else if (k == "thermal_warp") number(thermalWarp);
    else if (k == "threads") { if (numeric) threads = (int)fv; }
    else if (k == "width") { if (numeric) width = (int)fv; }
    else if (k == "height") { if (numeric) height = (int)fv; }
    else if (k == "fullscreen") flag(fullscreen);
    else if (k == "vsync") flag(vsync);
    else if (k == "render_scale") number(renderScale);
    else if (k == "smooth_surfaces") flag(smooth);
    else if (k == "shadows") flag(shadows);
    else if (k == "bloom") flag(bloom);
    else if (k == "volumetrics") flag(volumetrics);
    else if (k == "fov") number(fov);
    else if (k == "exposure") number(exposure);
    else if (k == "ui_scale") number(uiScale);
    else if (k == "mouse_sensitivity") number(sensitivity);
    else if (k == "invert_y") flag(invertY);
    else if (k == "volume") number(volume);
  }
  sanitize();
}

void Settings::sanitize() {
  // These ranges cover everything the README suggests and keep a bad value
  // from turning into a huge allocation or a divide by zero.
  // Content is authored and tested at 2-2.5 cm. Coarser voxels cannot hold
  // the thin features (terminals, motor windings) the parts are built from.
  voxelSize = clampF(voxelSize, 0.02f, 0.025f);
  worldX = clampF(worldX, 4.f, 32.f);
  worldY = clampF(worldY, 2.f, 16.f);
  worldZ = clampF(worldZ, 4.f, 32.f);
  thermalWarp = clampF(thermalWarp, 1.f, 100.f);
  threads = std::clamp(threads, 0, 256);
  width = std::clamp(width, 320, 7680);
  height = std::clamp(height, 240, 4320);
  renderScale = clampF(renderScale, 0.3f, 2.f);
  fov = clampF(fov, 40.f, 120.f);
  exposure = clampF(exposure, 0.25f, 4.f);
  uiScale = clampF(uiScale, 0.5f, 3.f);
  sensitivity = clampF(sensitivity, 0.05f, 10.f);
  volume = clampF(volume, 0.f, 1.f);
}

void Settings::save(const std::string& path) const {
  std::ofstream f(path);
  if (!f) return;
  f << "# Realism settings. Delete this file to restore the defaults.\n";
  f << "# World (takes effect on the next launch)\n";
  f << "voxel_size = " << voxelSize << "      # metres per voxel; smaller = finer detail, more memory and CPU\n";
  f << "world_x = " << worldX << "\nworld_y = " << worldY << "\nworld_z = " << worldZ << "\n";
  f << "thermal_warp = " << thermalWarp << "      # heat runs this many times faster than real time\n";
  f << "threads = " << threads << "            # 0 = use every hardware thread\n";
  f << "# Display\n";
  f << "width = " << width << "\nheight = " << height << "\n";
  f << "fullscreen = " << (fullscreen ? 1 : 0) << "\nvsync = " << (vsync ? 1 : 0) << "\n";
  f << "render_scale = " << renderScale << "     # 1.0 = native; lower for speed\n";
  f << "smooth_surfaces = " << (smooth ? 1 : 0) << "\nshadows = " << (shadows ? 1 : 0) << "\nbloom = " << (bloom ? 1 : 0) << "\nvolumetrics = " << (volumetrics ? 1 : 0) << "\n";
  f << "fov = " << fov << "\nexposure = " << exposure << "\nui_scale = " << uiScale << "\n";
  f << "# Controls and sound\n";
  f << "mouse_sensitivity = " << sensitivity << "\ninvert_y = " << (invertY ? 1 : 0) << "\nvolume = " << volume << "\n";
}
