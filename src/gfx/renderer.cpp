#include "renderer.h"
#include <algorithm>
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
           "#define MAT_WATER %du\n#define MAT_FILAMENT %du\n#define MAT_MOLTEN %du\n#define MAT_BEDROCK %du\n",
           (int)WATER, (int)FILAMENT, (int)MOLTEN, (int)BEDROCK);
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

inline uint8_t densOf(uint8_t m) {
  if (m == AIR) return 0;
  const Material& M = MATS[m];
  if (M.transparent || M.phase == GAS) return 0;
  return 255;
}
inline uint8_t partHash(uint32_t c) { return c ? (uint8_t)(hash3((int)c, 17, 91) & 255) : 0; }
}  // namespace

bool Renderer::init(Sim* sim) {
  sim_ = sim;
  std::string defs = matDefines();
  std::string common = shaders::kCommon;
  bool ok = true;
  ok &= worldShader_.build("world", shaders::kFullscreenVS, common + defs + "#define WORLD\n" + shaders::kVoxelFS);
  ok &= bodyShader_.build("body", common + shaders::kBoxVS, common + defs + shaders::kVoxelFS);
  ok &= debrisShader_.build("debris", common + shaders::kDebrisVS, common + shaders::kDebrisFS);
  ok &= fxShader_.build("fx", common + shaders::kFxVS, common + shaders::kFxFS);
  ok &= brightShader_.build("bright", shaders::kFullscreenVS, common + shaders::kBrightFS);
  ok &= blurShader_.build("blur", shaders::kFullscreenVS, common + shaders::kBlurFS);
  ok &= compositeShader_.build("composite", shaders::kFullscreenVS, common + shaders::kCompositeFS);
  if (!ok) return false;

  const Volume& w = sim_->world;
  worldData_ = makeTex3D(GL_RGBA8UI, w.nx, w.ny, w.nz, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, false);
  worldDens_ = makeTex3D(GL_R8, w.nx, w.ny, w.nz, GL_RED, GL_UNSIGNED_BYTE, true);
  worldBricks_ = makeTex3D(GL_R8UI, w.bx, w.by, w.bz, GL_RED_INTEGER, GL_UNSIGNED_BYTE, false);
  vram_ += (size_t)w.nx * w.ny * w.nz * 5;
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
  for (int a = 0; a < 3; a++) {
    glEnableVertexAttribArray(2 + a);
    glVertexAttribPointer(2 + a, 4, GL_FLOAT, GL_FALSE, 48, (void*)(intptr_t)(a * 16));
    glVertexAttribDivisor(2 + a, 1);
  }

  // FX sprites.
  const float quad[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
  glGenVertexArrays(1, &vaoFx_);
  glBindVertexArray(vaoFx_);
  glGenBuffers(1, &vboFxQuad_);
  glBindBuffer(GL_ARRAY_BUFFER, vboFxQuad_);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
  glGenBuffers(1, &vboFxInst_);
  glBindBuffer(GL_ARRAY_BUFFER, vboFxInst_);
  for (int a = 0; a < 3; a++) {
    glEnableVertexAttribArray(1 + a);
    glVertexAttribPointer(1 + a, 4, GL_FLOAT, GL_FALSE, 48, (void*)(intptr_t)(a * 16));
    glVertexAttribDivisor(1 + a, 1);
  }
  glBindVertexArray(0);
  glGenQueries(2, query_);
  return true;
}

void Renderer::ensureTargets(int w, int h) {
  if (w == rw_ && h == rh_) return;
  rw_ = w; rh_ = h;
  if (fbo_) {
    glDeleteFramebuffers(1, &fbo_); glDeleteTextures(1, &colorTex_); glDeleteTextures(1, &depthTex_);
    glDeleteFramebuffers(1, &depthCopyFbo_); glDeleteTextures(1, &depthCopyTex_);
    for (auto& l : bloomLevels_) { glDeleteFramebuffers(1, &l.fbo); glDeleteTextures(1, &l.tex); }
    bloomLevels_.clear();
  }
  colorTex_ = makeTex2D(GL_RGBA16F, w, h, GL_RGBA, GL_FLOAT, true);
  depthTex_ = makeTex2D(GL_DEPTH_COMPONENT32F, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, false);
  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTex_, 0);
  depthCopyTex_ = makeTex2D(GL_DEPTH_COMPONENT32F, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, false);
  glGenFramebuffers(1, &depthCopyFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, depthCopyFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthCopyTex_, 0);
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
  scratchDens_.resize(BS3);
  const Brick* br = w.brickAt(b);
  if (!br || br->count == 0) {
    std::fill(scratchData_.begin(), scratchData_.end(), 0);
    std::fill(scratchDens_.begin(), scratchDens_.end(), 0);
    if (brickOcc_[b]) { brickOcc_[b] = 0; brickOccDirty_ = true; }
  } else {
    for (int z = 0; z < 16; z++)
      for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
          int l = x | (z << 4) | (y << 8);
          int o = x + 16 * (y + 16 * z);
          uint8_t m = br->mat[l];
          scratchData_[o * 4 + 0] = m;
          scratchData_[o * 4 + 1] = m == WATER ? (uint8_t)(std::min(1.f, br->fuel[l] > 0 ? br->fuel[l] : 1.f) * 255.f)
                                               : m ? glowByte(br->temp[l], br->flags[l]) : 0;
          scratchData_[o * 4 + 2] = br->hp[l];
          scratchData_[o * 4 + 3] = partHash(br->comp[l]);
          scratchDens_[o] = densOf(m);
        }
    if (!brickOcc_[b]) { brickOcc_[b] = 1; brickOccDirty_ = true; }
  }
  glBindTexture(GL_TEXTURE_3D, worldData_);
  glTexSubImage3D(GL_TEXTURE_3D, 0, bx * 16, by * 16, bz * 16, 16, 16, 16, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, scratchData_.data());
  glBindTexture(GL_TEXTURE_3D, worldDens_);
  glTexSubImage3D(GL_TEXTURE_3D, 0, bx * 16, by * 16, bz * 16, 16, 16, 16, GL_RED, GL_UNSIGNED_BYTE, scratchDens_.data());
}

void Renderer::uploadAllWorld() {
  Volume& w = sim_->world;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (size_t b = 0; b < w.bricks.size(); b++) uploadWorldBrick((int)b);
  for (int b : w.dirtyList) w.dirty[b] = 0;
  w.dirtyList.clear();
}

void Renderer::updateWorld() {
  Volume& w = sim_->world;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  size_t n = std::min<size_t>(w.dirtyList.size(), 2500);
  for (size_t i = 0; i < n; i++) {
    int b = w.dirtyList[i];
    w.dirty[b] = 0;
    uploadWorldBrick(b);
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
}

void Renderer::buildBodyTex(Body& b, BodyGpu& g) {
  Volume& v = b.vol;
  ivec3 td(v.nx + 2, v.ny + 2, v.nz + 2);
  size_t n = (size_t)td.x * td.y * td.z;
  auto alloc = [&](Tex3D& t) {
    if (t.data && t.dims == td) return;
    if (t.data) { glDeleteTextures(1, &t.data); glDeleteTextures(1, &t.dens); }
    t.data = makeTex3D(GL_RGBA8UI, td.x, td.y, td.z, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, false);
    t.dens = makeTex3D(GL_R8, td.x, td.y, td.z, GL_RED, GL_UNSIGNED_BYTE, true);
    t.dims = td;
  };
  // Which components spin separately?
  std::vector<uint32_t> rotIds;
  for (auto& rp : b.rot_parts) rotIds.push_back(rp.comp);
  // Free textures of parts that no longer exist.
  for (size_t i = 0; i < g.parts.size();) {
    if (std::find(rotIds.begin(), rotIds.end(), g.parts[i].first) == rotIds.end()) {
      glDeleteTextures(1, &g.parts[i].second.data); glDeleteTextures(1, &g.parts[i].second.dens);
      g.parts.erase(g.parts.begin() + i);
    } else i++;
  }
  std::vector<std::vector<uint8_t>> data(1 + rotIds.size(), std::vector<uint8_t>(n * 4, 0));
  std::vector<std::vector<uint8_t>> dens(1 + rotIds.size(), std::vector<uint8_t>(n, 0));
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
    dens[slot][o] = densOf(m);
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
    glBindTexture(GL_TEXTURE_3D, t.dens);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, td.x, td.y, td.z, GL_RED, GL_UNSIGNED_BYTE, dens[slot].data());
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
      if (g.main.data) { glDeleteTextures(1, &g.main.data); glDeleteTextures(1, &g.main.dens); }
      for (auto& p : g.parts) { glDeleteTextures(1, &p.second.data); glDeleteTextures(1, &p.second.dens); }
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
  // Explosions.
  for (auto& f : sim_->fx)
    if (f.kind == FX_FLASH) add(f.p, 14.f, vec3(60, 40, 18) * (f.life / f.maxLife));
  // Fires: a few representative flames spaced apart.
  std::vector<vec3> picked;
  for (size_t i = 0; i < sim_->fx.size() && picked.size() < 6; i += 7) {
    const Fx& f = sim_->fx[i];
    if (f.kind != FX_FIRE) continue;
    bool near = false;
    for (auto& p : picked) if (length2(p - f.p) < 0.8f) { near = true; break; }
    if (near) continue;
    picked.push_back(f.p);
    float flick = 0.8f + 0.4f * rng().f();
    add(f.p, 4.f, vec3(4.0f, 1.9f, 0.6f) * flick);
  }
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
        b[i * 4 + 2] = MATS[i].transparent ? 1.f : 0.f; b[i * 4 + 3] = 0;
      }
    }
    built = true;
  }
  glUniform4fv(s.loc("uMatA"), 64, a);
  glUniform4fv(s.loc("uMatB"), 64, b);
}

void Renderer::drawDebris(const Camera& cam, float time) {
  auto& deb = sim_->debris;
  if (deb.empty()) return;
  std::vector<float> inst;
  inst.reserve(deb.size() * 12);
  for (size_t i = 0; i < deb.size(); i++) {
    const Debris& d = deb[i];
    vec3 c = matLinear(d.mat);
    if (d.mat == MOLTEN) c = vec3(1.0f, 0.35f, 0.05f);
    float glow = d.temp > 430 ? std::min(63.f, (d.temp - 430) / 25.f + 1) : 0;
    uint32_t h = hash3((int)i, 3, 7);
    vec3 ax = normalize(vec3((h & 255) / 255.f - .5f, ((h >> 8) & 255) / 255.f - .5f, ((h >> 16) & 255) / 255.f - .5f));
    quat q = quat::axisAngle(ax, time * (3 + (h % 7)) * (d.bounces < 3 ? 1.f : 0.f) + (h & 63));
    float size = sim_->vs * (0.75f + 0.25f * ((h >> 24) & 15) / 15.f);
    inst.insert(inst.end(), {d.p.x, d.p.y, d.p.z, size, c.x, c.y, c.z, glow, q.x, q.y, q.z, q.w});
  }
  debrisShader_.use();
  setCommon(debrisShader_, cam, time);
  glBindVertexArray(vaoDebris_);
  glBindBuffer(GL_ARRAY_BUFFER, vboDebrisInst_);
  glBufferData(GL_ARRAY_BUFFER, inst.size() * sizeof(float), inst.data(), GL_STREAM_DRAW);
  glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr, (GLsizei)deb.size());
}

void Renderer::drawFx(const Camera& cam, float time) {
  auto& fx = sim_->fx;
  if (fx.empty()) return;
  std::vector<float> inst;
  inst.reserve(fx.size() * 12);
  for (size_t i = 0; i < fx.size(); i++) {
    const Fx& f = fx[i];
    float age = 1.f - f.life / f.maxLife;
    float stretch = (f.kind == FX_SPARK) ? std::min(3.f, length(f.v) * 0.15f) : 0.f;
    inst.insert(inst.end(), {f.p.x, f.p.y, f.p.z, f.size, f.color.x, f.color.y, f.color.z, 1.f,
                             (float)f.kind, age, (float)(i % 97) / 97.f, stretch});
  }
  fxShader_.use();
  setCommon(fxShader_, cam, time);
  fxShader_.set("uCamRight", cam.right);
  fxShader_.set("uCamUp", cam.up);
  fxShader_.set("uViewport", vec2((float)rw_, (float)rh_));
  fxShader_.set("uNear", cam.nearZ);
  fxShader_.set("uFar", cam.farZ);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, depthCopyTex_);
  fxShader_.set("uDepth", 0);
  glBindVertexArray(vaoFx_);
  glBindBuffer(GL_ARRAY_BUFFER, vboFxInst_);
  glBufferData(GL_ARRAY_BUFFER, inst.size() * sizeof(float), inst.data(), GL_STREAM_DRAW);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);
  glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)fx.size());
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
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
  // GPU timing (previous frame's result).
  static int frames = 0;
  if (frames++ > 1) {
    GLint avail = 0;
    glGetQueryObjectiv(query_[queryIdx_ ^ 1], GL_QUERY_RESULT_AVAILABLE, &avail);
    if (avail) { uint64_t ns = 0; glGetQueryObjectui64v(query_[queryIdx_ ^ 1], GL_QUERY_RESULT, &ns); gpuMs_ = gpuMs_ * 0.9f + (ns / 1e6f) * 0.1f; }
  }
  glBeginQuery(GL_TIME_ELAPSED, query_[queryIdx_]);
  updateWorld();
  updateBodies();
  updateDynOcc();
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
    glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_3D, worldDens_); s.set("uWorldDensity", 1);
    glActiveTexture(GL_TEXTURE0 + 2); glBindTexture(GL_TEXTURE_3D, worldBricks_); s.set("uWorldBricks", 2);
    glActiveTexture(GL_TEXTURE0 + 3); glBindTexture(GL_TEXTURE_3D, dynOcc_); s.set("uDynOcc", 3);
    s.set("uWorldDims", ivec3(sim_->world.nx, sim_->world.ny, sim_->world.nz));
    s.set("uDynScale", (float)dynScale_);
    s.set("uSmooth", opt.smooth ? 1 : 0);
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
    s.set("uObjDensity", 5);
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
      glActiveTexture(GL_TEXTURE0 + 5); glBindTexture(GL_TEXTURE_3D, t.dens);
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

  // ---- debris and effects
  drawDebris(cam, time);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, depthCopyFbo_);
  glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  drawFx(cam, time);

  // ---- bloom + composite
  if (opt.bloom) bloom();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
  compositeShader_.set("uTexel", vec2(1.f / rw, 1.f / rh));
  compositeShader_.set("uFlash", flash);
  glBindVertexArray(vaoEmpty_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glEndQuery(GL_TIME_ELAPSED);
  queryIdx_ ^= 1;
  if (getenv("REALISM_DEBUG_GL")) { GLenum e; while ((e = glGetError()) != 0) fprintf(stderr, "GL error 0x%x\n", e); }
  glActiveTexture(GL_TEXTURE0);
}
