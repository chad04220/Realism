#pragma once
#include <unordered_map>
#include <vector>
#include "../sim/sim.h"
#include "gl.h"
#include "shader.h"

struct Camera {
  vec3 pos;
  float yaw = 0, pitch = 0, fov = 70.f * PI / 180.f;
  float nearZ = 0.03f, farZ = 300.f;
  mat4 view, proj, viewProj, invViewProj;
  vec3 forward, right, up;
  void update(float aspect);
};

struct RenderOptions {
  bool smooth = true;
  bool shadows = true;
  bool bloom = true;
  float renderScale = 1.0f;
  float exposure = 1.0f;
  bool thermal = false;
};

struct HoverInfo {
  int body = -2;              // -2 none, -1 world, else body index
  uint32_t comp = 0;
  ivec3 lo, hi;               // part bounds in that volume's voxel coords
};

class Renderer {
 public:
  bool init(Sim* sim);
  void render(const Camera& cam, const RenderOptions& opt, const HoverInfo& hover, float time, float flash, int winW, int winH);
  void uploadAllWorld();
  float gpuMs() const { return gpuMs_; }
  size_t vramBytes() const { return vram_; }
  int bodiesDrawn = 0;
  float groundY = 1.0f;
  std::vector<std::pair<vec4, vec4>> extraLights;  // (pos+radius, colour) e.g. the player's flashlight

 private:
  struct Tex3D { GLuint data = 0, dens = 0; ivec3 dims; };
  struct BodyGpu {
    Tex3D main;
    std::vector<std::pair<uint32_t, Tex3D>> parts;  // rotating parts
    std::vector<std::pair<uint32_t, vec3>> bulbs;   // powered-bulb light sources (voxel coords)
    uint32_t version = ~0u;
    bool seen = false;
  };
  void ensureTargets(int w, int h);
  void uploadWorldBrick(int b);
  void updateWorld();
  void updateBodies();
  void updateDynOcc();
  void buildBodyTex(Body& b, BodyGpu& g);
  void gatherLights(std::vector<vec4>& pos, std::vector<vec4>& col);
  void setCommon(Shader& s, const Camera& cam, float time);
  void setMaterials(Shader& s);
  void drawDebris(const Camera& cam, float time);
  void drawFx(const Camera& cam, float time);
  void bloom();

  Sim* sim_ = nullptr;
  Shader worldShader_, bodyShader_, debrisShader_, fxShader_, brightShader_, blurShader_, compositeShader_;
  GLuint worldData_ = 0, worldDens_ = 0, worldBricks_ = 0, dynOcc_ = 0;
  std::vector<uint8_t> brickOcc_;
  bool brickOccDirty_ = true;
  std::vector<uint8_t> dynCpu_;
  std::vector<int> dynTouched_;
  ivec3 dynDims_;
  int dynScale_ = 4;
  std::unordered_map<int, BodyGpu> bodies_;
  GLuint vaoEmpty_ = 0, vaoCube_ = 0, vboCube_ = 0, iboCube_ = 0;
  GLuint vaoDebris_ = 0, vboDebrisInst_ = 0;
  GLuint vaoFx_ = 0, vboFxQuad_ = 0, vboFxInst_ = 0;
  // Render targets.
  int w_ = 0, h_ = 0, rw_ = 0, rh_ = 0;
  GLuint fbo_ = 0, colorTex_ = 0, depthTex_ = 0;
  GLuint depthCopyFbo_ = 0, depthCopyTex_ = 0;
  struct Level { GLuint fbo, tex; int w, h; };
  std::vector<Level> bloomLevels_;
  GLuint query_[2] = {0, 0};
  int queryIdx_ = 0;
  float gpuMs_ = 0;
  size_t vram_ = 0;
  RenderOptions opt_;
  Camera cam_;
  std::vector<uint8_t> scratchData_, scratchDens_;
};
