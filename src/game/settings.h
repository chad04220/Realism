#pragma once
#include <string>

struct Settings {
  // World
  float voxelSize = 0.025f;    // metres. 0.025 suits a 6-core CPU with 8 GB VRAM; try 0.02 on faster machines
  float worldX = 16, worldY = 8, worldZ = 16;
  float thermalWarp = 20;
  int threads = 0;             // 0 = all hardware threads
  // Display
  int width = 1600, height = 900;
  bool fullscreen = false, vsync = true;
  float renderScale = 1.0f;
  bool dynamicRes = true;      // lower the render scale while the GPU can't hold targetFps
  float targetFps = 60;
  bool shadows = true, bloom = true;
  float fov = 72, exposure = 1.0f;
  float uiScale = 1.0f;
  // Controls
  float sensitivity = 1.0f;
  bool invertY = false;
  float volume = 0.8f;

  void load(const std::string& path);
  void save(const std::string& path) const;
  // Clamp every value to a range the simulation and renderer can handle, so a
  // typo in settings.cfg can't hang or exhaust memory at start-up.
  void sanitize();
};
