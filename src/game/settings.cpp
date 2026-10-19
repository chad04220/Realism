#include "settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

void Settings::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    auto trim = [](std::string& s) {
      while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
      size_t i = 0;
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
      s = s.substr(i);
    };
    trim(k); trim(v);
    float fv = (float)std::atof(v.c_str());
    bool bv = v == "1" || v == "true" || v == "yes" || v == "on";
    if (k == "voxel_size") voxelSize = fv;
    else if (k == "world_x") worldX = fv;
    else if (k == "world_y") worldY = fv;
    else if (k == "world_z") worldZ = fv;
    else if (k == "thermal_warp") thermalWarp = fv;
    else if (k == "threads") threads = (int)fv;
    else if (k == "width") width = (int)fv;
    else if (k == "height") height = (int)fv;
    else if (k == "fullscreen") fullscreen = bv;
    else if (k == "vsync") vsync = bv;
    else if (k == "render_scale") renderScale = fv;
    else if (k == "smooth_surfaces") smooth = bv;
    else if (k == "shadows") shadows = bv;
    else if (k == "bloom") bloom = bv;
    else if (k == "volumetrics") volumetrics = bv;
    else if (k == "fov") fov = fv;
    else if (k == "exposure") exposure = fv;
    else if (k == "ui_scale") uiScale = fv;
    else if (k == "mouse_sensitivity") sensitivity = fv;
    else if (k == "invert_y") invertY = bv;
    else if (k == "volume") volume = fv;
  }
  if (voxelSize < 0.01f) voxelSize = 0.01f;
  if (voxelSize > 0.1f) voxelSize = 0.1f;
  if (renderScale < 0.3f) renderScale = 0.3f;
  if (renderScale > 2.f) renderScale = 2.f;
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
