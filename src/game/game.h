#pragma once
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../gfx/renderer.h"
#include "../platform/platform.h"
#include "../sim/sim.h"
#include "../ui/ui.h"
#include "audio.h"
#include "parts.h"
#include "scene.h"
#include "settings.h"

class Workshop;

enum Tool { T_HAND, T_HAMMER, T_WRENCH, T_CHISEL, T_TORCH, T_HOSE, T_CHARGE, T_BUILD, T_SCAN, T_COUNT };

struct ToolInfo { const char* name; const char* lmb; const char* rmb; int icon; };
extern const ToolInfo TOOLS[T_COUNT];

struct Toast {
  std::string text;
  ui::Color color;
  float age = 0, life = 4;
  int icon = 0;
};

class Game {
 public:
  struct Args {
    std::string screenshot, script;
    int frames = 0, w = 0, h = 0;
    bool selftest = false;
    float voxelSize = 0;
  };
  Game();
  ~Game();
  int run(const Args& args);

  // Shared with the workshop.
  Sim sim;
  Renderer ren;
  SceneInfo scene;
  Settings settings;
  Camera cam;
  RenderOptions ropt;
  Audio audio;
  std::unique_ptr<Workshop> workshop;
  std::vector<std::shared_ptr<Template>> salvage;     // parts taken from the world
  std::map<uint8_t, int> collected;                   // material voxels chiselled out
  void toast(const std::string& s, ui::Color c = ui::theme::text, int icon = 0);
  float uiScale() const;

 private:
  enum Mode { M_PLAY, M_WORKSHOP };
  struct Player {
    vec3 pos;                 // feet
    vec3 vel;
    float yaw = 0, pitch = 0;
    bool onGround = false, fly = false, crouch = false;
    float eye = 1.62f, bob = 0;
  };

  bool init(const Args& args);
  void frame(float dt);
  void updatePlay(float dt, plat::Input& in);
  void movePlayer(float dt, plat::Input& in);
  bool playerCollides(const vec3& feet, float h) const;
  void updateHover();
  void useTools(float dt, plat::Input& in);
  void useKey();
  void handleEvents();
  void drawHud(float dt);
  void drawMenu();
  void drawHelp();
  void drawTargetCard(float x, float y);
  void drawHotbar();
  void drawToasts(float dt);
  void drawStats();
  void resetWorld();
  void saveScreenshot(const std::string& path);
  void runScriptLine(const std::string& line, bool& quit);
  bool selftest();
  void toggleSwitch(int bodyIndex, uint32_t comp);
  void detonateCharge(int bodyIndex, uint32_t comp);
  void throwCharge();
  void placeCharge();
  void chiselAt(const Hit& h, float radius, float energy);
  void extinguish(const Hit& h, float radius);
  Body* spawnTemplate(const Template& t, const vec3& worldMin, const quat& rot = quat());
  void buildViewmodels();
  int makeViewmodel(int tool);
  void updateViewmodel(float dt);

  Mode mode_ = M_PLAY;
  Player pl_;
  int tool_ = T_HAND;
  float toolAnim_ = 0, hotbarAnim_ = 0;
  bool menu_ = false, help_ = false, hud_ = true, stats_ = false, flashlight_ = false;
  bool paused_ = false;
  // Hammer.
  float charge_ = 0, cooldown_ = 0, swing_ = 0;
  // Grab.
  int grabId_ = 0;
  vec3 grabLocal_;
  float grabDist_ = 1.5f;
  // Wrench/chisel.
  float workProgress_ = 0;
  uint32_t workComp_ = 0;
  float repeat_ = 0;
  // Build.
  int buildMat_ = 0;           // index into PALETTE
  int buildSize_ = 1;
  int buildPart_ = -1;         // index into salvage (>= 0 places a part)
  // Hover state.
  Hit hit_;
  HoverInfo hover_;
  uint32_t hoverCompCached_ = 0;
  int hoverBodyCached_ = -3;
  int hoverBodyIdCached_ = 0;
  std::vector<uint32_t> remoteCharges_;
  std::vector<int> remoteChargeBodies_;
  std::deque<Toast> toasts_;
  float flash_ = 0, shake_ = 0;
  double lastTime_ = 0;
  float fps_ = 60, frameMs_ = 16;
  float time_ = 0;
  bool quit_ = false;
  bool scripted_ = false;
  plat::Input scriptIn_;
  std::vector<std::string> script_;
  size_t scriptPos_ = 0;
  int scriptWait_ = 0;
  std::string menuPage_ = "main";
  int viewIds_[T_COUNT] = {};
  uint8_t viewBuildMat_ = 0;
  float useAnim_ = 0;
  friend class Workshop;
};

extern const uint8_t BUILD_PALETTE[];
extern const int BUILD_PALETTE_N;
