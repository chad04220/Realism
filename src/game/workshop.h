#pragma once
#include <memory>
#include <string>
#include <vector>
#include "../gfx/renderer.h"
#include "../platform/platform.h"
#include "parts.h"

class Game;

// The assembly workshop: a build jig on the concrete pad. Place materials
// voxel by voxel or drop in parts, watch circuits come alive while it is
// still clamped in the jig, then launch it into the world.
class Workshop {
 public:
  explicit Workshop(Game& g);
  void createJig();
  void enter();
  void leave();
  void update(float dt, plat::Input& in, Camera& cam);
  void drawUI(float dt);
  Body* jig();
  bool loadBlueprint(const std::string& name);
  void launch();
  bool typing() const { return typingName_; }
  std::vector<std::string> blueprintNames() const;

 private:
  enum EditTool { E_PLACE, E_ERASE, E_PAINT, E_PICK };
  enum Tab { TAB_MATS, TAB_PARTS, TAB_SALVAGE, TAB_BLUEPRINTS };
  struct Edit { ivec3 p; Cell before, after; };
  struct Snapshot { std::vector<Edit> edits; };

  void applyEdit(std::vector<Edit>& edits, const ivec3& p, const Cell& c);
  void commit(std::vector<Edit>&& edits);
  void undo();
  void redo();
  void place(const ivec3& cell, bool erase);
  void paint(const ivec3& cell);
  void pick(const ivec3& cell);
  void clearJig();
  void saveBlueprint(const std::string& name);
  void refreshAnalysis();
  std::shared_ptr<Template> currentTemplate();
  void updateGhost(const ivec3& at, bool valid);
  bool cursorCell(const vec3& ro, const vec3& rd, ivec3& cell, ivec3& hitCell, bool& onFloor);
  void afterEdit();

  Game& g_;
  int jigId_ = 0;
  int ghostId_ = 0;
  vec3 target_;
  float yaw_ = 0.6f, pitch_ = -0.45f, dist_ = 2.6f;
  float tYaw_ = 0.6f, tPitch_ = -0.45f, tDist_ = 2.6f;
  vec3 tTarget_;
  EditTool tool_ = E_PLACE;
  Tab tab_ = TAB_PARTS;
  int mat_ = 0, part_ = 0, salv_ = 0;
  int rotYaw_ = 0, rotPitch_ = 0;
  bool mirror_ = false;
  int brush_ = 1;
  float paletteScroll_ = 0;
  std::vector<Snapshot> undo_, redo_;
  bool dragging_ = false, orbiting_ = false, panning_ = false;
  float downX_ = 0, downY_ = 0;
  bool rightDown_ = false;
  ivec3 lastPlaced_{-999, -999, -999};
  ivec3 hoverCell_{-1, -1, -1};
  bool hoverValid_ = false;
  std::string status_;
  float statusT_ = 0;
  // Live analysis.
  struct Analysis {
    float mass = 0;
    size_t voxels = 0;
    int batteries = 0, loads = 0, poweredLoads = 0, motors = 0, wheels = 0, props = 0, rockets = 0, charges = 0;
    bool shorted = false;
    float drawW = 0, chargeWh = 0;
    int pieces = 1;
    std::vector<std::string> notes;
  } an_;
  float anTimer_ = 0;
  std::string saveName_ = "My build";
  bool typingName_ = false;
};
