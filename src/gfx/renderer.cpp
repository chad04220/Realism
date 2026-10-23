#include "renderer.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include "shaders.h"

void Camera::update(float aspect) {
  forward = vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw));
  right = normalize(cross(forward, vec3(0, 1, 0)));
  up = cross(right, forward);
  view = mat4::lookAt(pos, pos + forward, vec3(0, 1, 0));
  proj = mat4::perspective(fov, aspect, nearZ, farZ);
  viewProj = proj * view;
  invViewProj = viewProj.inverse();
}

namespace {
std::string matDefines() {
  char buf[512];
  snprintf(buf, sizeof buf,
           "#define MAT_WATER %du\n#define MAT_FILAMENT %du\n#define MAT_MOLTEN %du\n#define MAT_BEDROCK %du\n#define MAT_STEAM %du\n",
           (int)WATER, (int)FILAMENT, (int)MOLTEN, (int)BEDROCK, (int)STEAM);
  return buf;
}

GLuint makeTex3D(GLenum internal, int w, int h, int d, GLenum format, GLenum type, bool linear) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_3D, t);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
  glTexImage3D(GL_TEXTURE_3D, 0, internal, w, h, d, 0, format, type, nullptr);
  return t;
}

GLuint makeTex2D(GLenum internal, int w, int h, GLenum format, GLenum type, bool linear) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
  return t;
}

inline uint8_t opaqueOf(uint8_t m) {
  if (m == AIR) return 0;
  const Material& M = MATS[m];
  return M.transparent || M.phase == GAS ? 0 : 1;
}
inline uint8_t partHash(uint32_t c) { return c ? (uint8_t)(hash3((int)c, 17, 91) & 255) : 0; }
}  // namespace

bool Renderer::init(Sim* sim) {
  sim_ = sim;
  std::string defs = matDefines();
  std::string common = shaders::kCommon;
  bool ok = true;
  const std::string world = std::string(shaders::kWorldUniforms) + shaders::kWorldTrace;
  ok &= worldShader_.build("world", shaders::kFullscreenVS, common + defs + "#define WORLD\n" + world + shaders::kVoxelFS);
  ok &= bodyShader_.build("body", common + shaders::kBoxVS, common + defs + world + shaders::kVoxelFS);
  ok &= debrisShader_.build("debris", common + shaders::kDebrisVS, common + shaders::kDebrisFS);
  ok &= brightShader_.build("bright", shaders::kFullscreenVS, common + shaders::kBrightFS);
  ok &= blurShader_.build("blur", shaders::kFullscreenVS, common + shaders::kBlurFS);
  ok &= compositeShader_.build("composite", shaders::kFullscreenVS, common + shaders::kCompositeFS);
  if (!ok) return false;
  // Precomputed sun visibility is an optimisation: without compute support
  // (or with REALISM_NO_SUNVIS) shadows fall back to a ray per pixel.
  sunVisOk_ = !getenv("REALISM_NO_SUNVIS") && sunVisShader_.buildCompute("sunvis", common + defs + world + shaders::kSunVisCS);

  const Volume& w = sim_->world;
  worldData_ = makeTex3D(GL_RGBA8UI, w.nx, w.ny, w.nz, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, false);
  worldBricks_ = makeTex3D(GL_R8UI, w.bx, w.by, w.bz, GL_RED_INTEGER, GL_UNSIGNED_BYTE, false);
  worldCells_ = makeTex3D(GL_R8UI, w.bx * 4, w.by * 4, w.bz * 4, GL_RED_INTEGER, GL_UNSIGNED_BYTE, false);
  vram_ += (size_t)w.bx * w.by * w.bz * 64;
  if (sunVisOk_) {
    sunVis_ = makeTex3D(GL_R8, w.nx, w.ny, w.nz, GL_RED, GL_UNSIGNED_BYTE, true);
    vram_ += (size_t)w.nx * w.ny * w.nz;
    // Start fully lit, a 16-voxel slab at a time (no 100 MB staging buffer).
    std::vector<uint8_t> lit((size_t)w.nx * w.ny * 16, 255);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_3D, sunVis_);
    for (int z = 0; z < w.nz; z += 16) glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, z, w.nx, w.ny, std::min(16, w.nz - z), GL_RED, GL_UNSIGNED_BYTE, lit.data());
    sunDirty_.assign(w.bricks.size(), 0);
    sunOccHash_.assign(w.bricks.size(), 0);
  }
  vram_ += (size_t)w.nx * w.ny * w.nz * 4;
  dynDims_ = {w.nx / dynScale_, w.ny / dynScale_, w.nz / dynScale_};
  dynOcc_ = makeTex3D(GL_R8, dynDims_.x, dynDims_.y, dynDims_.z, GL_RED, GL_UNSIGNED_BYTE, true);
  dynCpu_.assign((size_t)dynDims_.x * dynDims_.y * dynDims_.z, 0);
  brickOcc_.assign(w.bricks.size(), 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glBindTexture(GL_TEXTURE_3D, dynOcc_);
  glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, dynDims_.x, dynDims_.y, dynDims_.z, GL_RED, GL_UNSIGNED_BYTE, dynCpu_.data());

  glGenVertexArrays(1, &vaoEmpty_);
  // Unit cube (positions only) for body boxes and debris.
  const float cube[] = {
      // pos, normal (24 verts)
      0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1,
      1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, -1, 0, 1, 0, 0, 0, -1, 1, 1, 0, 0, 0, -1,
      1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0,
      0, 0, 0, -1, 0, 0, 0, 0, 1, -1, 0, 0, 0, 1, 1, -1, 0, 0, 0, 1, 0, -1, 0, 0,
      0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0,
      0, 0, 0, 0, -1, 0, 1, 0, 0, 0, -1, 0, 1, 0, 1, 0, -1, 0, 0, 0, 1, 0, -1, 0,
  };
  uint16_t idx[36];
  for (int f = 0; f < 6; f++) {
    uint16_t b = (uint16_t)(f * 4);
    uint16_t q[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
    memcpy(idx + f * 6, q, sizeof q);
  }
  glGenVertexArrays(1, &vaoCube_);
  glBindVertexArray(vaoCube_);
  glGenBuffers(1, &vboCube_);
  glBindBuffer(GL_ARRAY_BUFFER, vboCube_);
  glBufferData(GL_ARRAY_BUFFER, sizeof cube, cube, GL_STATIC_DRAW);
  glGenBuffers(1, &iboCube_);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, iboCube_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)12);

  // Debris: same cube + per-instance data.
  glGenVertexArrays(1, &vaoDebris_);
  glBindVertexArray(vaoDebris_);
  glBindBuffer(GL_ARRAY_BUFFER, vboCube_);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, iboCube_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)12);
  glGenBuffers(1, &vboDebrisInst_);
  glBindBuffer(GL_ARRAY_BUFFER, vboDebrisInst_);
  for (int a = 0; a < 2; a++) {
    glEnableVertexAttribArray(2 + a);
    glVertexAttribPointer(2 + a, 4, GL_FLOAT, GL_FALSE, 32, (void*)(intptr_t)(a * 16));
    glVertexAttribDivisor(2 + a, 1);
  }
  glBindVertexArray(0);
  syncProfile_ = getenv("REALISM_PROFILE_SYNC") != nullptr;
  glGenQueries(P_COUNT + 1, stamps_[0]);
  glGenQueries(P_COUNT + 1, stamps_[1]);
  return true;
}

double Renderer::meanSceneRed() {
  std::vector<float> px((size_t)rw_ * rh_ * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
  glReadPixels(0, 0, rw_, rh_, GL_RGBA, GL_FLOAT, px.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  double s = 0;
  for (size_t i = 0; i < px.size(); i += 4) s += px[i];
  return s / std::max<size_t>(1, px.size() / 4);
}

const char* Renderer::passName(int p) {
  static const char* n[P_COUNT] = {"upload", "world", "bodies", "debris", "post"};
  return p >= 0 && p < P_COUNT ? n[p] : "?";
}

void Renderer::stamp(int i) {
  if (syncProfile_) {
    glFinish();
    syncStamp_[i] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (i == P_COUNT) {
      float total = 0;
      for (int p = 0; p < P_COUNT; p++) {
        passMs_[p] = passMs_[p] * 0.9f + (float)(syncStamp_[p + 1] - syncStamp_[p]) * 0.1f;
        total += passMs_[p];
      }
      gpuMs_ = total;
    }
    return;
  }
  glQueryCounter(stamps_[stampIdx_][i], GL_TIMESTAMP);
}

void Renderer::readStamps() {
  // The other set was issued last frame; it is normally done by now.
  if (syncProfile_ || stampFrames_ < 2) return;
  const int prev = stampIdx_ ^ 1;
  GLint avail = 0;
  glGetQueryObjectiv(stamps_[prev][P_COUNT], GL_QUERY_RESULT_AVAILABLE, &avail);
  if (!avail) return;
  uint64_t t[P_COUNT + 1];
  for (int i = 0; i <= P_COUNT; i++) glGetQueryObjectui64v(stamps_[prev][i], GL_QUERY_RESULT, &t[i]);
  float total = 0;
  for (int i = 0; i < P_COUNT; i++) {
    float ms = t[i + 1] > t[i] ? (t[i + 1] - t[i]) / 1e6f : 0.f;
    passMs_[i] = passMs_[i] * 0.9f + ms * 0.1f;
    total += passMs_[i];
  }
  gpuMs_ = total;
}

void Renderer::ensureTargets(int w, int h) {
  if (w == rw_ && h == rh_) return;
  rw_ = w; rh_ = h;
  if (fbo_) {
    glDeleteFramebuffers(1, &fbo_); glDeleteTextures(1, &colorTex_); glDeleteTextures(1, &depthTex_);
    for (auto& l : bloomLevels_) { glDeleteFramebuffers(1, &l.fbo); glDeleteTextures(1, &l.tex); }
    bloomLevels_.clear();
  }
  colorTex_ = makeTex2D(GL_RGBA16F, w, h, GL_RGBA, GL_FLOAT, true);
  depthTex_ = makeTex2D(GL_DEPTH_COMPONENT32F, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, false);
  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTex_, 0);
  int bw = w / 2, bh = h / 2;
  for (int i = 0; i < 6 && bw >= 4 && bh >= 4; i++) {
    Level l;
    l.w = bw; l.h = bh;
    l.tex = makeTex2D(GL_R11F_G11F_B10F, bw, bh, GL_RGB, GL_FLOAT, true);
    glGenFramebuffers(1, &l.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, l.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l.tex, 0);
    bloomLevels_.push_back(l);
    bw /= 2; bh /= 2;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "HDR framebuffer incomplete: 0x%x\n", st);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::uploadWorldBrick(int b) {
  const Volume& w = sim_->world;
  int bx = b % w.bx, bz = (b / w.bx) % w.bz, by = b / (w.bx * w.bz);
  scratchData_.resize(BS3 * 4);
  scratchOpaque_.resize(BS3);
  scratchCells_.assign(64, 0);
  const Brick* br = w.brickAt(b);
  const GasBrick* gb = sim_->gas.bricks.empty() ? nullptr : sim_->gas.bricks[b].get();
  if (gb && !gb->count) gb = nullptr;
  if ((!br || br->count == 0) && !gb) {
    std::fill(scratchData_.begin(), scratchData_.end(), 0);
    std::fill(scratchOpaque_.begin(), scratchOpaque_.end(), 0);
    if (brickOcc_[b]) { brickOcc_[b] = 0; brickOccDirty_ = true; }
  } else {
    for (int z = 0; z < 16; z++)
      for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
          int l = x | (z << 4) | (y << 8);
          int o = x + 16 * (y + 16 * z);
          uint8_t m = br ? br->mat[l] : 0;
          if (!m && gb && gb->mat[l]) {
            // A gas voxel: drawn like any other voxel, glowing by its temperature.
            m = gb->mat[l];
            scratchData_[o * 4 + 0] = m;
            scratchData_[o * 4 + 1] = glowByte(gb->temp[l], 0);
            scratchData_[o * 4 + 2] = 255;
            scratchData_[o * 4 + 3] = 0;
            scratchOpaque_[o] = 0;
            scratchCells_[(x >> 2) + 4 * ((y >> 2) + 4 * (z >> 2))] = 1;
            continue;
          }
          scratchData_[o * 4 + 0] = m;
          scratchData_[o * 4 + 1] = m ? glowByte(br->temp[l], br->flags[l]) : 0;
          scratchData_[o * 4 + 2] = br ? br->hp[l] : 255;
          scratchData_[o * 4 + 3] = br ? partHash(br->comp[l]) : 0;
          scratchOpaque_[o] = opaqueOf(m);
          if (m) scratchCells_[(x >> 2) + 4 * ((y >> 2) + 4 * (z >> 2))] = 1;
        }
    if (!brickOcc_[b]) { brickOcc_[b] = 1; brickOccDirty_ = true; }
  }
  glBindTexture(GL_TEXTURE_3D, worldData_);
  glTexSubImage3D(GL_TEXTURE_3D, 0, bx * 16, by * 16, bz * 16, 16, 16, 16, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, scratchData_.data());
  glBindTexture(GL_TEXTURE_3D, worldCells_);
  glTexSubImage3D(GL_TEXTURE_3D, 0, bx * 4, by * 4, bz * 4, 4, 4, 4, GL_RED_INTEGER, GL_UNSIGNED_BYTE, scratchCells_.data());
  // Most uploads are glow (a fire's temperatures); shadows only change when
  // what blocks light does. Compare a hash of the opaque voxels.
  if (sunVisOk_) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < BS3; i++) { h ^= scratchOpaque_[i]; h *= 0x100000001b3ull; }
    if (sunOccHash_[b] != h) { sunOccHash_[b] = h; markSunShadow(b); }
  }
}

// A changed brick can change the sunlight on itself and on everything it
// shades: the bricks down-sun of it, out to the ground.
void Renderer::markSunShadow(int b) {
  if (!sunVisOk_) return;
  const Volume& w = sim_->world;
  auto mark = [&](int x, int y, int z) {
    if (x < 0 || y < 0 || z < 0 || x >= w.bx || y >= w.by || z >= w.bz) return;
    int i = x + w.bx * (z + w.bz * y);
    if (!sunDirty_[i]) { sunDirty_[i] = 1; sunQueue_.push_back(i); }
  };
  const vec3 sun = sim_->sunDir;
  vec3 c(b % w.bx * 16 + 8.f, b / (w.bx * w.bz) * 16 + 8.f, (b / w.bx) % w.bz * 16 + 8.f);
  for (float t = 0; ; t += 8) {
    vec3 p = c - sun * t;
    if (p.y < -16 || p.x < -32 || p.z < -32) break;
    int x = (int)std::floor(p.x / 16), y = (int)std::floor(p.y / 16), z = (int)std::floor(p.z / 16);
    for (int dz = -1; dz <= 1; dz++)
      for (int dx = -1; dx <= 1; dx++) mark(x + dx, y, z + dz);
    mark(x, y + 1, z);
  }
}

// Everything that can be sampled: occupied bricks and their neighbours.
void Renderer::markAllSun() {
  if (!sunVisOk_) return;
  const Volume& w = sim_->world;
  sunQueue_.clear();
  std::fill(sunDirty_.begin(), sunDirty_.end(), 0);
  for (int y = 0; y < w.by; y++)
    for (int z = 0; z < w.bz; z++)
      for (int x = 0; x < w.bx; x++) {
        bool near = false;
        for (int dy = -1; dy <= 1 && !near; dy++)
          for (int dz = -1; dz <= 1 && !near; dz++)
            for (int dx = -1; dx <= 1 && !near; dx++) {
              int xx = x + dx, yy = y + dy, zz = z + dz;
              if (xx < 0 || yy < 0 || zz < 0 || xx >= w.bx || yy >= w.by || zz >= w.bz) continue;
              const Brick* br = w.bricks[xx + w.bx * (zz + w.bz * yy)].get();
              near = br && br->count > 0;
            }
        if (!near) continue;
        int i = x + w.bx * (z + w.bz * y);
        sunDirty_[i] = 1;
        sunQueue_.push_back(i);
      }
}

// Recompute up to maxBricks queued bricks of sun visibility on the GPU.
void Renderer::processSun(size_t maxBricks) {
  if (!sunVisOk_) return;
  const Volume& w = sim_->world;
  if (length2(sunDirUsed_ - sim_->sunDir) > 1e-12f) {
    // The sun moved: all of it.
    sunDirUsed_ = sim_->sunDir;
    markAllSun();
  }
  if (sunQueue_.empty()) return;
  sunVisShader_.use();
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_3D, worldData_); sunVisShader_.set("uWorldData", 0);
  glActiveTexture(GL_TEXTURE0 + 2); glBindTexture(GL_TEXTURE_3D, worldBricks_); sunVisShader_.set("uWorldBricks", 2);
  glActiveTexture(GL_TEXTURE0 + 6); glBindTexture(GL_TEXTURE_3D, worldCells_); sunVisShader_.set("uWorldCells", 6);
  glActiveTexture(GL_TEXTURE0);
  sunVisShader_.set("uWorldDims", ivec3(w.nx, w.ny, w.nz));
  sunVisShader_.set("uSunDir", sim_->sunDir);
  sunVisShader_.set("uCellSkip", getenv("REALISM_NO_CELLS") ? 0 : 1);
  setMaterials(sunVisShader_);
  glBindImageTexture(0, sunVis_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R8);
  size_t n = std::min(maxBricks, sunQueue_.size());
  size_t from = sunQueue_.size() - n;  // newest first: what just changed is what the player is looking at
  GLint coords[64 * 4];
  for (size_t i = from; i < sunQueue_.size();) {
    int k = 0;
    for (; k < 64 && i < sunQueue_.size(); k++, i++) {
      int b = sunQueue_[i];
      sunDirty_[b] = 0;
      coords[k * 4 + 0] = b % w.bx;
      coords[k * 4 + 1] = b / (w.bx * w.bz);
      coords[k * 4 + 2] = (b / w.bx) % w.bz;
      coords[k * 4 + 3] = 0;
    }
    glUniform4iv(sunVisShader_.loc("uBricks"), k, coords);
    glDispatchCompute(2, 2, 2 * k);
  }
  sunQueue_.resize(from);
  glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
}

void Renderer::uploadAllWorld() {
  Volume& w = sim_->world;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (size_t b = 0; b < w.bricks.size(); b++) uploadWorldBrick((int)b);
  for (int b : w.dirtyList) w.dirty[b] = 0;
  w.dirtyList.clear();
  // The whole world's sunlight, before the first frame.
  if (brickOccDirty_) updateWorld();
  markAllSun();
  sunDirUsed_ = sim_->sunDir;
  processSun(sunQueue_.size());
}

void Renderer::updateWorld() {
  Volume& w = sim_->world;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  // A time budget, not a count: a blast that dirties thousands of bricks
  // streams in over a few frames instead of stalling one.
  auto t0 = std::chrono::steady_clock::now();
  size_t n = 0;
  while (n < w.dirtyList.size()) {
    int b = w.dirtyList[n++];
    w.dirty[b] = 0;
    uploadWorldBrick(b);
    if ((n & 63) == 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 3.0) break;
  }
  w.dirtyList.erase(w.dirtyList.begin(), w.dirtyList.begin() + n);
  if (brickOccDirty_) {
    brickOccDirty_ = false;
    // Volume bricks are ordered (x, z, y); GL wants (x, y, z).
    std::vector<uint8_t> gl((size_t)w.bx * w.by * w.bz);
    for (int y = 0; y < w.by; y++)
      for (int z = 0; z < w.bz; z++)
        for (int x = 0; x < w.bx; x++) gl[x + (size_t)w.bx * (y + (size_t)w.by * z)] = brickOcc_[x + w.bx * (z + w.bz * y)];
    glBindTexture(GL_TEXTURE_3D, worldBricks_);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, w.bx, w.by, w.bz, GL_RED_INTEGER, GL_UNSIGNED_BYTE, gl.data());
  }
  // Sunlight where the world changed: a few hundred bricks a frame at most
  // (a big blast catches up over a few frames).
  processSun(256);
}

void Renderer::buildBodyTex(Body& b, BodyGpu& g) {
  Volume& v = b.vol;
  ivec3 td(v.nx + 2, v.ny + 2, v.nz + 2);
  size_t n = (size_t)td.x * td.y * td.z;
  auto alloc = [&](Tex3D& t) {
    if (t.data && t.dims == td) return;
    if (t.data) glDeleteTextures(1, &t.data);
    t.data = makeTex3D(GL_RGBA8UI, td.x, td.y, td.z, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, false);
    t.dims = td;
  };
  // Which components spin separately?
  std::vector<uint32_t> rotIds;
  for (auto& rp : b.rot_parts) rotIds.push_back(rp.comp);
  // Free textures of parts that no longer exist.
  for (size_t i = 0; i < g.parts.size();) {
    if (std::find(rotIds.begin(), rotIds.end(), g.parts[i].first) == rotIds.end()) {
      glDeleteTextures(1, &g.parts[i].second.data);
      g.parts.erase(g.parts.begin() + i);
    } else i++;
  }
  std::vector<std::vector<uint8_t>> data(1 + rotIds.size(), std::vector<uint8_t>(n * 4, 0));
  std::vector<std::pair<uint32_t, vec3>> bulbSum;
  std::vector<int> bulbCount;
  v.forEach([&](int x, int y, int z, Brick* br, int l) {
    uint32_t c = br->comp[l];
    size_t slot = 0;
    for (size_t k = 0; k < rotIds.size(); k++) if (rotIds[k] == c) { slot = k + 1; break; }
    size_t o = (size_t)(x + 1) + (size_t)td.x * ((size_t)(y + 1) + (size_t)td.y * (size_t)(z + 1));
    uint8_t m = br->mat[l];
    data[slot][o * 4 + 0] = m;
    data[slot][o * 4 + 1] = glowByte(br->temp[l], br->flags[l]);
    data[slot][o * 4 + 2] = br->hp[l];
    data[slot][o * 4 + 3] = partHash(c);
    if (m == FILAMENT && c) {
      size_t k = 0;
      for (; k < bulbSum.size(); k++) if (bulbSum[k].first == c) break;
      if (k == bulbSum.size()) { bulbSum.push_back({c, vec3(0)}); bulbCount.push_back(0); }
      bulbSum[k].second += vec3(x + 0.5f, y + 0.5f, z + 0.5f);
      bulbCount[k]++;
    }
  });
  g.bulbs.clear();
  for (size_t k = 0; k < bulbSum.size(); k++) g.bulbs.push_back({bulbSum[k].first, bulbSum[k].second / (float)bulbCount[k]});
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  auto upload = [&](Tex3D& t, size_t slot) {
    alloc(t);
    glBindTexture(GL_TEXTURE_3D, t.data);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, td.x, td.y, td.z, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, data[slot].data());
  };
  upload(g.main, 0);
  for (size_t k = 0; k < rotIds.size(); k++) {
    auto it = std::find_if(g.parts.begin(), g.parts.end(), [&](auto& p) { return p.first == rotIds[k]; });
    if (it == g.parts.end()) { g.parts.push_back({rotIds[k], Tex3D{}}); it = g.parts.end() - 1; }
    upload(it->second, k + 1);
  }
  for (int bi : v.dirtyList) v.dirty[bi] = 0;
  v.dirtyList.clear();
  g.version = v.version;
}

void Renderer::updateBodies() {
  for (auto& [id, g] : bodies_) g.seen = false;
  int budget = 24;
  for (auto& bp : sim_->bodies) {
    Body& b = *bp;
    BodyGpu& g = bodies_[b.id];
    g.seen = true;
    bool need = g.version != b.vol.version || !b.vol.dirtyList.empty() || !g.main.data;
    if (need && (budget-- > 0 || !g.main.data)) buildBodyTex(b, g);
  }
  for (auto it = bodies_.begin(); it != bodies_.end();) {
    if (!it->second.seen) {
      auto& g = it->second;
      if (g.main.data) glDeleteTextures(1, &g.main.data);
      for (auto& p : g.parts) glDeleteTextures(1, &p.second.data);
      it = bodies_.erase(it);
    } else ++it;
  }
}

void Renderer::updateDynOcc() {
  for (int i : dynTouched_) dynCpu_[i] = 0;
  dynTouched_.clear();
  const float cell = sim_->vs * dynScale_;
  for (auto& bp : sim_->bodies) {
    Body& b = *bp;
    if (b.ghost) continue;
    mat3 R = b.R();
    for (const Sample& s : b.samples) {
      vec3 p = b.pos + R * s.local;
      ivec3 c = floorI(p / cell);
      if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= dynDims_.x || c.y >= dynDims_.y || c.z >= dynDims_.z) continue;
      int i = c.x + dynDims_.x * (c.y + dynDims_.y * c.z);
      if (!dynCpu_[i]) dynTouched_.push_back(i);
      dynCpu_[i] = 255;
    }
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glBindTexture(GL_TEXTURE_3D, dynOcc_);
  glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, dynDims_.x, dynDims_.y, dynDims_.z, GL_RED, GL_UNSIGNED_BYTE, dynCpu_.data());
}

void Renderer::gatherLights(std::vector<vec4>& pos, std::vector<vec4>& col) {
  auto add = [&](const vec3& p, float r, const vec3& c) {
    if (pos.size() >= 16) return;
    pos.push_back(vec4(p, r));
    col.push_back(vec4(c, 0));
  };
  for (auto& [p, c] : extraLights) add(p.xyz(), p.w, c.xyz());
  for (auto& bp : sim_->bodies) {
    auto it = bodies_.find(bp->id);
    if (it == bodies_.end()) continue;
    for (auto& [c, local] : it->second.bulbs)
      if (comp(c).powered) add(bp->toWorld(local, sim_->vs), 6.f, vec3(5.5f, 4.4f, 2.8f) * comp(c).powerFactor);
  }
  // Blast flashes (light, not matter).
  for (auto& f : sim_->flashes) add(f.p, 14.f, vec3(60, 40, 18) * (f.life / f.maxLife));
  // Fires: burning surfaces and glowing gas, a few representative spots spaced apart.
  std::vector<vec3> picked;
  auto fire = [&](const vec3& p) {
    if (picked.size() >= 6) return;
    for (auto& q : picked) if (length2(q - p) < 0.8f) return;
    picked.push_back(p);
    float flick = 0.8f + 0.4f * rng().f();
    add(p, 4.f, vec3(4.0f, 1.9f, 0.6f) * flick);
  };
  for (size_t i = 0; i < sim_->fireSpots.size(); i += 3) fire(sim_->fireSpots[i]);
  for (size_t i = 0; i < sim_->gas.hot.size(); i += 2) fire(sim_->gas.hot[i]);
}

void Renderer::setCommon(Shader& s, const Camera& cam, float time) {
  s.set("uViewProj", cam.viewProj);
  s.set("uInvViewProj", cam.invViewProj);
  s.set("uCamPos", cam.pos);
  s.set("uSunDir", sim_->sunDir);
  s.set("uSunColor", vec3(2.6f, 2.42f, 2.15f));
  s.set("uSkyZenith", vec3(0.22f, 0.42f, 0.85f));
  s.set("uSkyHorizon", vec3(0.72f, 0.8f, 0.9f));
  s.set("uTime", time);
  s.set("uVS", sim_->vs);
}

void Renderer::setMaterials(Shader& s) {
  static float a[64 * 4], b[64 * 4];
  static bool built = false;
  if (!built) {
    for (int i = 0; i < 64; i++) {
      if (i < MAT_COUNT) {
        vec3 c = matLinear((uint8_t)i);
        a[i * 4 + 0] = c.x; a[i * 4 + 1] = c.y; a[i * 4 + 2] = c.z; a[i * 4 + 3] = MATS[i].roughness;
        b[i * 4 + 0] = MATS[i].metal; b[i * 4 + 1] = (float)MATS[i].pattern;
        b[i * 4 + 2] = MATS[i].transparent ? 1.f : 0.f; b[i * 4 + 3] = MATS[i].phase == GAS && i != AIR ? 1.f : 0.f;
      }
    }
    built = true;
  }
  glUniform4fv(s.loc("uMatA"), 64, a);
  glUniform4fv(s.loc("uMatB"), 64, b);
}

// The centre of the world voxel p is in: loose voxels and effects are drawn
// on the grid, exactly where a voxel there would be.
static vec3 snapToVoxel(const vec3& p, float vs) {
  return vec3((std::floor(p.x / vs) + 0.5f) * vs, (std::floor(p.y / vs) + 0.5f) * vs, (std::floor(p.z / vs) + 0.5f) * vs);
}

void Renderer::drawDebris(const Camera& cam, float time) {
  auto& deb = sim_->debris;
  if (deb.empty()) return;
  const float vs = sim_->vs;
  std::vector<float> inst;
  inst.reserve(deb.size() * 8);
  for (const Debris& d : deb) {
    vec3 c = matLinear(d.mat);
    if (d.mat == MOLTEN) c = vec3(1.0f, 0.35f, 0.05f);
    float glow = d.temp > 430 ? std::min(63.f, (d.temp - 430) / 25.f + 1) : 0;
    vec3 p = snapToVoxel(d.p, vs);
    inst.insert(inst.end(), {p.x, p.y, p.z, vs, c.x, c.y, c.z, glow});
  }
  debrisShader_.use();
  setCommon(debrisShader_, cam, time);
  glBindVertexArray(vaoDebris_);
  glBindBuffer(GL_ARRAY_BUFFER, vboDebrisInst_);
  glBufferData(GL_ARRAY_BUFFER, inst.size() * sizeof(float), inst.data(), GL_STREAM_DRAW);
  glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr, (GLsizei)deb.size());
}

// The final image is composited off screen, shown with a blit, and a
// blurred quarter-resolution copy is kept for the UI's frosted panels.
void Renderer::ensureOutput(int w, int h) {
  if (w == ow_ && h == oh_) return;
  ow_ = w; oh_ = h;
  if (ldrFbo_) {
    glDeleteFramebuffers(1, &ldrFbo_); glDeleteTextures(1, &ldrTex_);
    glDeleteFramebuffers(3, glassFbo_); glDeleteTextures(3, glassTex_);
  }
  ldrTex_ = makeTex2D(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, true);
  glGenFramebuffers(1, &ldrFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, ldrFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ldrTex_, 0);
  // Levels: 1/2, 1/4, then a blurred 1/4 that the UI samples.
  int sw[3] = {std::max(4, w / 2), std::max(4, w / 4), std::max(4, w / 4)}, sh[3] = {std::max(4, h / 2), std::max(4, h / 4), std::max(4, h / 4)};
  for (int i = 0; i < 3; i++) {
    gw_[i] = sw[i]; gh_[i] = sh[i];
    glassTex_[i] = makeTex2D(GL_RGBA8, sw[i], sh[i], GL_RGBA, GL_UNSIGNED_BYTE, true);
    glGenFramebuffers(1, &glassFbo_[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, glassFbo_[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glassTex_[i], 0);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::buildBackdrop() {
  glDisable(GL_BLEND);
  glBindVertexArray(vaoEmpty_);
  blurShader_.use();
  blurShader_.set("uSrc", 0);
  glActiveTexture(GL_TEXTURE0);
  GLuint src = ldrTex_;
  int srcW = ow_, srcH = oh_;
  for (int i = 0; i < 3; i++) {
    glBindFramebuffer(GL_FRAMEBUFFER, glassFbo_[i]);
    glViewport(0, 0, gw_[i], gh_[i]);
    glBindTexture(GL_TEXTURE_2D, src);
    blurShader_.set("uTexel", vec2(1.f / srcW, 1.f / srcH) * (i == 2 ? 1.5f : 1.f));
    blurShader_.set("uMode", i == 2 ? 1.f : 0.f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    src = glassTex_[i]; srcW = gw_[i]; srcH = gh_[i];
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::bloom() {
  if (bloomLevels_.empty()) return;
  glDisable(GL_DEPTH_TEST);
  glBindVertexArray(vaoEmpty_);
  brightShader_.use();
  glBindFramebuffer(GL_FRAMEBUFFER, bloomLevels_[0].fbo);
  glViewport(0, 0, bloomLevels_[0].w, bloomLevels_[0].h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, colorTex_);
  brightShader_.set("uSrc", 0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  blurShader_.use();
  blurShader_.set("uSrc", 0);
  for (size_t i = 1; i < bloomLevels_.size(); i++) {
    auto& src = bloomLevels_[i - 1];
    auto& dst = bloomLevels_[i];
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    glBindTexture(GL_TEXTURE_2D, src.tex);
    blurShader_.set("uTexel", vec2(1.f / src.w, 1.f / src.h));
    blurShader_.set("uMode", 0.f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  for (int i = (int)bloomLevels_.size() - 1; i > 0; i--) {
    auto& src = bloomLevels_[i];
    auto& dst = bloomLevels_[i - 1];
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    glBindTexture(GL_TEXTURE_2D, src.tex);
    blurShader_.set("uTexel", vec2(1.f / src.w, 1.f / src.h));
    blurShader_.set("uMode", 1.f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glDisable(GL_BLEND);
}

void Renderer::render(const Camera& cam, const RenderOptions& opt, const HoverInfo& hover, float time, float flash, int winW, int winH) {
  opt_ = opt;
  cam_ = cam;
  w_ = winW; h_ = winH;
  int rw = std::max(64, (int)(winW * opt.renderScale)), rh = std::max(64, (int)(winH * opt.renderScale));
  ensureTargets(rw, rh);
  readStamps();
  stamp(P_UPLOAD);
  {
    auto c0 = std::chrono::steady_clock::now();
    updateWorld();
    updateBodies();
    updateDynOcc();
    uploadCpuMs_ = uploadCpuMs_ * 0.9f + std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - c0).count() * 0.1f;
  }
  std::vector<vec4> lpos, lcol;
  gatherLights(lpos, lcol);

  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glViewport(0, 0, rw, rh);
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glClearDepth(1.0);
  glClear(GL_DEPTH_BUFFER_BIT);

  auto bindWorld = [&](Shader& s) {
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_3D, worldData_); s.set("uWorldData", 0);
    glActiveTexture(GL_TEXTURE0 + 2); glBindTexture(GL_TEXTURE_3D, worldBricks_); s.set("uWorldBricks", 2);
    glActiveTexture(GL_TEXTURE0 + 3); glBindTexture(GL_TEXTURE_3D, dynOcc_); s.set("uDynOcc", 3);
    glActiveTexture(GL_TEXTURE0 + 6); glBindTexture(GL_TEXTURE_3D, worldCells_); s.set("uWorldCells", 6);
    static const int cellSkip = getenv("REALISM_NO_CELLS") ? 0 : 1;
    s.set("uCellSkip", cellSkip);
    glActiveTexture(GL_TEXTURE0 + 7); glBindTexture(GL_TEXTURE_3D, sunVis_); s.set("uSunVis", 7);
    s.set("uSunVisOn", sunVisOk_ ? 1 : 0);
    s.set("uWorldDims", ivec3(sim_->world.nx, sim_->world.ny, sim_->world.nz));
    s.set("uDynScale", (float)dynScale_);
    s.set("uShadows", opt.shadows ? 1 : 0);
    s.set("uNumLights", (int)lpos.size());
    if (!lpos.empty()) {
      glUniform4fv(s.loc("uLightPos"), (GLsizei)lpos.size(), &lpos[0].x);
      glUniform4fv(s.loc("uLightCol"), (GLsizei)lcol.size(), &lcol[0].x);
    }
    s.set("uHoverPulse", 0.5f + 0.5f * std::sin(time * 5.f));
    s.set("uGhost", 0);
    s.set("uDepthScale", 1.f);
    s.set("uGroundY", groundY);
    s.set("uDebug", getenv("REALISM_DEBUG_PASS") ? atoi(getenv("REALISM_DEBUG_PASS")) : 0);
    s.set("uObjData", 4);
  };
  auto setHover = [&](Shader& s, int which) {
    if (hover.body == which && hover.comp) {
      s.set("uHover", vec4(0, 0, 0, (float)partHash(hover.comp)));
      s.set("uHoverMin", toF(hover.lo));
      s.set("uHoverMax", toF(hover.hi));
    } else {
      s.set("uHover", vec4(0, 0, 0, -1));
    }
  };

  // ---- world (full screen; writes depth for every pixel)
  stamp(P_WORLD);
  worldShader_.use();
  setCommon(worldShader_, cam, time);
  setMaterials(worldShader_);
  bindWorld(worldShader_);
  setHover(worldShader_, -1);
  glDepthFunc(GL_ALWAYS);
  glBindVertexArray(vaoEmpty_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDepthFunc(GL_LESS);

  // ---- bodies (boxes ray-marched in local space)
  stamp(P_BODIES);
  bodyShader_.use();
  setCommon(bodyShader_, cam, time);
  setMaterials(bodyShader_);
  bindWorld(bodyShader_);
  glBindVertexArray(vaoCube_);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_FRONT);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  bodiesDrawn = 0;
  const float vs = sim_->vs;
  for (size_t bi = 0; bi < sim_->bodies.size(); bi++) {
    Body& b = *sim_->bodies[bi];
    auto it = bodies_.find(b.id);
    if (it == bodies_.end() || !it->second.main.data || b.hidden) continue;
    BodyGpu& g = it->second;
    // Frustum-ish cull: behind the camera and far away.
    vec3 toB = b.pos - cam.pos;
    float dist = length(toB);
    if (dist > b.radius && dot(toB, cam.forward) < -b.radius) continue;
    mat3 R = b.R();
    mat4 rotM = mat4::trs(vec3(0), b.rot, vec3(1));
    mat4 model = mat4::trs(b.pos, b.rot, vec3(vs)) * mat4::trs(b.com * -1.f, quat(), vec3(1));
    auto drawTex = [&](const Tex3D& t, const mat4& M, const mat3& Rm) {
      glActiveTexture(GL_TEXTURE0 + 4); glBindTexture(GL_TEXTURE_3D, t.data);
      bodyShader_.set("uObjDims", ivec3(b.vol.nx, b.vol.ny, b.vol.nz));
      bodyShader_.set("uModel", M);
      bodyShader_.set("uModelInv", M.inverse());
      float rm[9];
      for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) rm[c * 3 + r] = Rm(r, c);
      glUniformMatrix3fv(bodyShader_.loc("uObjRot"), 1, GL_FALSE, rm);
      bodyShader_.set("uBoxMin", vec3(-0.01f));
      bodyShader_.set("uBoxMax", vec3((float)b.vol.nx, (float)b.vol.ny, (float)b.vol.nz) + vec3(0.01f));
      glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
    };
    (void)rotM;
    setHover(bodyShader_, (int)bi);
    bodyShader_.set("uGhost", b.ghost && !b.viewmodel ? 1 : 0);
    bodyShader_.set("uDepthScale", b.viewmodel ? 0.04f : 1.f);
    glDepthMask(b.ghost && !b.viewmodel ? GL_FALSE : GL_TRUE);
    drawTex(g.main, model, R);
    for (auto& [cid, tex] : g.parts) {
      const RotPart* rp = nullptr;
      for (auto& r : b.rot_parts) if (r.comp == cid) rp = &r;
      if (!rp) continue;
      quat spin = quat::axisAngle(rp->axis, comp(cid).spin);
      mat4 M = model * mat4::trs(rp->pivot, spin, vec3(1)) * mat4::trs(rp->pivot * -1.f, quat(), vec3(1));
      drawTex(tex, M, (b.rot * spin).toMat3());
    }
    bodiesDrawn++;
  }
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);

  // ---- loose voxels in flight
  stamp(P_PARTICLES);
  drawDebris(cam, time);

  // ---- bloom + composite
  stamp(P_POST);
  if (opt.bloom) bloom();
  ensureOutput(winW, winH);
  glBindFramebuffer(GL_FRAMEBUFFER, ldrFbo_);
  glViewport(0, 0, winW, winH);
  glDisable(GL_DEPTH_TEST);
  compositeShader_.use();
  setCommon(compositeShader_, cam, time);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, colorTex_); compositeShader_.set("uScene", 0);
  glActiveTexture(GL_TEXTURE0 + 1);
  glBindTexture(GL_TEXTURE_2D, bloomLevels_.empty() ? colorTex_ : bloomLevels_[0].tex);
  compositeShader_.set("uBloom", 1);
  compositeShader_.set("uBloomStrength", opt.bloom ? 0.55f : 0.f);
  compositeShader_.set("uExposure", opt.exposure);
  compositeShader_.set("uFlash", flash);
  glBindVertexArray(vaoEmpty_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  // Show it, then blur a copy for the UI.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, ldrFbo_);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, winW, winH, 0, 0, winW, winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  buildBackdrop();
  glViewport(0, 0, winW, winH);
  stamp(P_COUNT);
  stampIdx_ ^= 1;
  stampFrames_++;
  if (getenv("REALISM_DEBUG_GL")) { GLenum e; while ((e = glGetError()) != 0) fprintf(stderr, "GL error 0x%x\n", e); }
  glActiveTexture(GL_TEXTURE0);
}

bool Renderer::vramInfo(int& totalMB, int& freeMB) const {
  // Mesa answers these queries too, with numbers that mean nothing.
  const char* vendor = (const char*)glGetString(GL_VENDOR);
  if (!vendor || !strstr(vendor, "NVIDIA")) return false;
  GLint total = 0, avail = 0;
  while (glGetError() != GL_NO_ERROR) {}
  glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total);
  glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &avail);
  if (glGetError() != GL_NO_ERROR || total <= 0) return false;
  totalMB = total / 1024; freeMB = avail / 1024;  // the queries report KB
  return true;
}
