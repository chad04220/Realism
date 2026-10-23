#pragma once
// GLSL sources. The voxel shader is compiled twice: once for the world (a
// full-screen pass with brick skipping) and once for rigid bodies (drawn as
// boxes, ray-marched in their own local voxel space).
#include <string>

namespace shaders {

inline const char* kCommon = R"GLSL(
#version 430 core
uniform mat4 uViewProj;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uSkyZenith;
uniform vec3 uSkyHorizon;
uniform float uTime;
uniform float uVS;

float hash13(vec3 p) {
  p = fract(p * 0.1031);
  p += dot(p, p.zyx + 31.32);
  return fract((p.x + p.y) * p.z);
}
float noise3(vec3 p) {
  vec3 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = hash13(i), b = hash13(i + vec3(1, 0, 0)), c = hash13(i + vec3(0, 1, 0)), d = hash13(i + vec3(1, 1, 0));
  float e = hash13(i + vec3(0, 0, 1)), g = hash13(i + vec3(1, 0, 1)), h = hash13(i + vec3(0, 1, 1)), k = hash13(i + vec3(1, 1, 1));
  return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), mix(mix(e, g, f.x), mix(h, k, f.x), f.y), f.z);
}
float pow5(float x) { x = clamp(x, 0.0, 1.0); float x2 = x * x; return x2 * x2 * x; }
float fbm(vec3 p) { float s = 0.0, a = 0.5; for (int i = 0; i < 4; i++) { s += a * noise3(p); p *= 2.03; a *= 0.5; } return s; }

vec3 skyColor(vec3 rd) {
  float h = clamp(rd.y, -0.2, 1.0);
  vec3 col = mix(uSkyHorizon, uSkyZenith, pow(max(h, 1e-4), 0.55));
  if (rd.y < 0.0) col = mix(uSkyHorizon, uSkyHorizon * vec3(0.55, 0.6, 0.55), clamp(-rd.y * 4.0, 0.0, 1.0));
  float sd = max(dot(rd, uSunDir), 0.0);
  col += uSunColor * (pow(sd, 900.0) * 18.0 + pow(sd, 12.0) * 0.18 + pow(sd, 3.0) * 0.06);
  // Soft drifting clouds on a high plane.
  if (rd.y > 0.02) {
    vec2 uv = rd.xz / (rd.y + 0.12) * 1.4 + vec2(uTime * 0.008, uTime * 0.003);
    float c = fbm(vec3(uv, uTime * 0.01));
    c = smoothstep(0.48, 0.78, c) * smoothstep(0.02, 0.25, rd.y);
    vec3 cloud = mix(vec3(0.95, 0.93, 0.9), uSunColor * 0.9, pow(sd, 6.0) * 0.5) * 1.1;
    col = mix(col, cloud, c * 0.85);
  }
  return col;
}

vec3 blackbody(float level) {  // level 0..63 from 430 C upwards in 25 C steps
  float t = level / 63.0;
  vec3 c = vec3(1.0, 0.18 + 0.75 * t, 0.02 + 0.9 * t * t);
  return c * (0.25 + 6.0 * t * t + 1.2 * t);
}
)GLSL";

inline const char* kFullscreenVS = R"GLSL(
#version 430 core
out vec2 vUV;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  vUV = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

inline const char* kBoxVS = R"GLSL(
uniform mat4 uModel;     // voxel space -> world
uniform vec3 uBoxMin, uBoxMax;
layout(location = 0) in vec3 aPos;  // unit cube corner
out vec3 vWorld;
void main() {
  vec3 v = mix(uBoxMin, uBoxMax, aPos);
  vec4 w = uModel * vec4(v, 1.0);
  vWorld = w.xyz;
  gl_Position = uViewProj * w;
}
)GLSL";

// World data and tracing shared by the voxel shader and the sun-visibility
// compute shader, so both see exactly the same world.
inline const char* kWorldUniforms = R"GLSL(
uniform usampler3D uWorldData;    // r=material g=glow b=integrity a=part hash
uniform usampler3D uWorldBricks;  // 1 if a 16^3 brick has anything in it
uniform usampler3D uWorldCells;   // 1 if a 4^3 cell has anything in it (finer empty-space skipping)
uniform int uCellSkip;            // 0: bricks only (REALISM_NO_CELLS, for comparisons)
uniform ivec3 uWorldDims;
uniform vec4 uMatA[64];           // linear rgb, roughness
uniform vec4 uMatB[64];           // metal, pattern, transparent, wet
)GLSL";

inline const char* kWorldTrace = R"GLSL(
bool isTrans(uint m) { return uMatB[m].z > 0.5; }
int gShadowSteps = 0;  // shadow-march iterations, for the cost view (REALISM_DEBUG_PASS 12)

vec2 boxClip(vec3 ro, vec3 rd, vec3 lo, vec3 hi) {
  vec3 inv = 1.0 / rd;
  vec3 a = (lo - ro) * inv, b = (hi - ro) * inv;
  vec3 mn = min(a, b), mx = max(a, b);
  return vec2(max(max(mn.x, mn.y), mn.z), min(min(mx.x, mx.y), mx.z));
}

// Where a ray leaves an axis-aligned box, and the face of the voxel it then
// enters (so a hit straight after a skip still knows which face it crossed).
float exitBox(vec3 ro, vec3 rd, vec3 lo, vec3 hi, vec3 stp, out vec3 face) {
  vec3 inv = 1.0 / rd;
  vec3 a = (lo - ro) * inv, b = (hi - ro) * inv;
  vec3 mx = max(a, b);
  face = (mx.x <= mx.y && mx.x <= mx.z) ? vec3(-stp.x, 0, 0) : (mx.y <= mx.z ? vec3(0, -stp.y, 0) : vec3(0, 0, -stp.z));
  return min(min(mx.x, mx.y), mx.z);
}

// Empty-space skipping through the world: whole 16^3 bricks, then 4^3 cells
// inside occupied bricks. Returns the size of the empty block v sits in (0 if
// v's cell holds something). Caches the last brick and cell looked up.
int emptyBlock(ivec3 v, inout ivec3 lastBrick, inout bool brickFull, inout ivec3 lastCell, inout bool cellFull) {
  if (v.y < 0 || any(lessThan(v, ivec3(0))) || any(greaterThanEqual(v, uWorldDims))) return 0;
  ivec3 b = v >> 4;
  if (b != lastBrick) { lastBrick = b; brickFull = texelFetch(uWorldBricks, b, 0).r != 0u; }
  if (!brickFull) return 16;
  if (uCellSkip == 0) return 0;
  ivec3 c = v >> 2;
  if (c != lastCell) { lastCell = c; cellFull = texelFetch(uWorldCells, c, 0).r != 0u; }
  return cellFull ? 0 : 4;
}

// Hard sun shadow through the world grid.
float shadowWorld(vec3 pv, vec3 dir) {
  vec3 ro = pv;
  vec3 rd = dir;
  vec2 tb = boxClip(ro, rd, vec3(0.0), vec3(uWorldDims));
  if (tb.y < 0.0) return 1.0;
  float t = max(tb.x, 0.0) + 1e-3;
  float tEnd = min(tb.y, 320.0);
  vec3 stp = sign(rd), tDelta = abs(1.0 / rd);
  vec3 p0 = ro + rd * t;
  ivec3 v = ivec3(floor(p0));
  vec3 side = (stp * (vec3(v) - p0) + stp * 0.5 + 0.5) * tDelta + t;
  ivec3 lastBrick = ivec3(-999), lastCell = ivec3(-999);
  bool brickFull = false, cellFull = false;
  vec3 face;
  for (int i = 0; i < 700; i++) {
    if (t > tEnd) break;
    gShadowSteps++;
    int skip = emptyBlock(v, lastBrick, brickFull, lastCell, cellFull);
    if (skip > 0) {
      vec3 base = vec3((v / skip) * skip);
      t = exitBox(ro, rd, base, base + float(skip), stp, face) + 1e-3;
      p0 = ro + rd * t;
      v = ivec3(floor(p0));
      side = (stp * (vec3(v) - p0) + stp * 0.5 + 0.5) * tDelta + t;
      continue;
    }
    if (all(greaterThanEqual(v, ivec3(0))) && all(lessThan(v, uWorldDims))) {
      uint m = texelFetch(uWorldData, v, 0).r;
      if (m != 0u && !isTrans(m)) return 0.0;
    }
    if (side.x < side.y && side.x < side.z) { t = side.x; side.x += tDelta.x; v.x += int(stp.x); }
    else if (side.y < side.z) { t = side.y; side.y += tDelta.y; v.y += int(stp.y); }
    else { t = side.z; side.z += tDelta.z; v.z += int(stp.z); }
  }
  return 1.0;
}

)GLSL";

// Sun visibility, one value per world voxel: the sun never moves, so whether
// a point is lit can be worked out once (and again only where the world
// changes) instead of marching a shadow ray for every pixel every frame.
// Each work group of 8^3 covers an eighth of one 16^3 brick from uBricks.
inline const char* kSunVisCS = R"GLSL(
layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;
layout(r8, binding = 0) uniform writeonly image3D uSunVis;
uniform ivec4 uBricks[64];   // brick coordinates (x, y, z) to compute
void main() {
  ivec3 g = ivec3(gl_WorkGroupID);
  ivec3 brick = uBricks[g.z >> 1].xyz;
  ivec3 v = brick * 16 + ivec3(g.x, g.y, g.z & 1) * 8 + ivec3(gl_LocalInvocationID);
  if (any(greaterThanEqual(v, uWorldDims))) return;
  // Leave this voxel first: a lit surface's own voxel counts as lit, so
  // filtered lookups just outside surfaces aren't darkened by it.
  float vis = shadowWorld(vec3(v) + 0.5 + uSunDir * 0.75, uSunDir);
  imageStore(uSunVis, v, vec4(vis));
}
)GLSL";

// The ray marcher. #define WORLD selects the world variant.
inline const char* kVoxelFS = R"GLSL(
layout(location = 0) out vec4 oColor;

uniform sampler3D uDynOcc;        // bodies, 4x coarser, for soft shadows and AO
uniform sampler3D uSunVis;        // precomputed sun visibility per world voxel (kSunVisCS)
uniform int uSunVisOn;            // 0: march a shadow ray per pixel instead
uniform float uDynScale;          // voxels per dyn-occ cell

uniform usampler3D uObjData;
uniform ivec3 uObjDims;           // voxel dims (texture is dims + 2 with a 1-voxel border)
uniform mat4 uModel, uModelInv;
uniform mat3 uObjRot;

uniform int uNumLights;
uniform vec4 uLightPos[16];       // world xyz, radius
uniform vec4 uLightCol[16];
uniform int uShadows;
uniform vec4 uHover;              // xyz unused, w = part hash to highlight (-1 none)
uniform vec3 uHoverMin, uHoverMax;
uniform float uHoverPulse;
uniform int uGhost;               // placement preview
uniform int uDebug;
uniform float uDepthScale;        // < 1 pulls a body in front of everything (held tool)
uniform float uGroundY;          // the lawn continues past the simulated area

#ifdef WORLD
in vec2 vUV;
#else
in vec3 vWorld;
#endif


// ---------------------------------------------------------- volume access
#ifdef WORLD
uvec4 vox(ivec3 v) {
  if (v.y < 0) return uvec4(1u, 0u, 255u, 0u);
  if (any(lessThan(v, ivec3(0))) || any(greaterThanEqual(v, uWorldDims))) return uvec4(0u);
  return texelFetch(uWorldData, v, 0);
}
ivec3 DIMS() { return uWorldDims; }
#else
uvec4 vox(ivec3 v) {
  if (any(lessThan(v, ivec3(0))) || any(greaterThanEqual(v, uObjDims))) return uvec4(0u);
  return texelFetch(uObjData, v + 1, 0);
}
ivec3 DIMS() { return uObjDims; }
#endif
float dynOcc(vec3 pw) { return texture(uDynOcc, pw / uVS / uDynScale / vec3(uWorldDims / int(uDynScale))).r; }


// ---------------------------------------------------------- marching
// Work counters for the cost views (REALISM_DEBUG_PASS 11/12): loop
// iterations of the primary march and of the sun-shadow march.
int gMarchSteps = 0;
struct Hit {
  float t;
  vec3 p;        // volume voxel space
  vec3 n;        // volume space normal
  uvec4 d;
  ivec3 cell;
};

// March the volume. Returns opaque hit; accumulates transmission through glass/water.
bool march(vec3 ro, vec3 rd, float tLimit, out Hit h, inout vec3 trans, inout vec3 refl, inout float firstT) {
  vec2 tb = boxClip(ro, rd, vec3(0.0), vec3(DIMS()));
#ifdef WORLD
  tb = boxClip(ro, rd, vec3(0.0, -1.0, 0.0), vec3(DIMS()));
#endif
  if (tb.x > tb.y || tb.y < 0.0) return false;
  float tMin = max(tb.x, 0.0);
  float t = tMin + 1e-3;
  float tEnd = min(tb.y, tLimit);
  vec3 stp = sign(rd);
  vec3 tDelta = abs(1.0 / rd);
  ivec3 v;
  vec3 side;
  vec3 face = vec3(0.0);
  // Face of entry for the first cell.
  {
    vec3 inv = 1.0 / rd;
    vec3 a = (vec3(0.0) - ro) * inv, b = (vec3(DIMS()) - ro) * inv;
    vec3 mn = min(a, b);
    if (tb.x > 0.0) face = (mn.x >= mn.y && mn.x >= mn.z) ? vec3(-stp.x, 0, 0) : (mn.y >= mn.z ? vec3(0, -stp.y, 0) : vec3(0, 0, -stp.z));
  }
  vec3 p0 = ro + rd * t;
  v = ivec3(floor(p0));
  side = (stp * (vec3(v) - p0) + stp * 0.5 + 0.5) * tDelta + t;
  bool inTrans = false;
  uint transMat = 0u;
#ifdef WORLD
  ivec3 lastBrick = ivec3(-999), lastCell = ivec3(-999);
  bool brickFull = false, cellFull = false;
#endif
  for (int i = 0; i < 1400; i++) {
    if (t > tEnd) break;
    gMarchSteps++;
#ifdef WORLD
    if (!inTrans) {
      int skip = emptyBlock(v, lastBrick, brickFull, lastCell, cellFull);
      if (skip > 0) {
        // Jump to the far side of the empty brick or cell.
        vec3 base = vec3((v / skip) * skip);
        t = exitBox(ro, rd, base, base + float(skip), stp, face) + 1e-3;
        p0 = ro + rd * t;
        v = ivec3(floor(p0));
        side = (stp * (vec3(v) - p0) + stp * 0.5 + 0.5) * tDelta + t;
        continue;
      }
    }
#endif
    uvec4 d = vox(v);
    uint m = d.r;
    if (m != 0u) {
      if (isTrans(m) && uMatB[m].w > 0.5) {
        // Gas (smoke, steam): a semi-transparent voxel. It absorbs and
        // scatters over exactly the length of ray inside it, and glows by its
        // temperature: a flame is hot smoke.
        float seg = (min(min(side.x, side.y), side.z) - t) * uVS;
        float a = 1.0 - exp(-(m == MAT_STEAM ? 34.0 : 44.0) * seg);
        float sunW = 1.0;
#ifdef WORLD
        if (uSunVisOn != 0) sunW = texelFetch(uSunVis, v, 0).r;
#endif
        vec3 amb = mix(uSkyHorizon, uSkyZenith, 0.6) * 0.55;
        vec3 L = uMatA[m].rgb * (uSunColor * sunW * 0.55 + amb);
        float lvl = float(d.g & 63u);
        if (lvl > 0.0) L += blackbody(lvl) * 1.6;
        refl += trans * a * L;
        trans *= 1.0 - a;
        inTrans = true;
        transMat = m;
        if (max(trans.r, max(trans.g, trans.b)) < 0.02) return false;
      } else if (isTrans(m)) {
        if (!inTrans || m != transMat) {
          // Entering glass or water: Fresnel reflection of the sky.
          // The face of the voxel it entered: water is drawn as the voxels it
          // is, like glass, with no surface of its own.
          vec3 n = length(face) > 0.0 ? face : -rd;
#ifndef WORLD
          vec3 nw = normalize(uObjRot * n);
          vec3 rdw = normalize(uObjRot * rd);
#else
          vec3 nw = n;
          vec3 rdw = rd;
#endif
          float c = clamp(dot(-rdw, nw), 0.0, 1.0);
          float F = (m == MAT_WATER ? 0.06 : 0.04) + 0.94 * pow5(1.0 - c);
          vec3 rr = reflect(rdw, nw);
          vec3 glint = uSunColor * pow(max(dot(rr, uSunDir), 0.0), 220.0) * 6.0;
          refl += trans * (F * skyColor(rr) * (m == MAT_WATER ? 1.0 : 0.7) + glint);
          trans *= (1.0 - F);
          if (firstT < 0.0) firstT = t;
        }
        inTrans = true;
        transMat = m;
        // Absorption and in-scattering per voxel crossed (scaled to voxel size).
        float len = uVS / 0.025;
        if (m == MAT_WATER) {
          vec3 absorb = exp(-vec3(0.45, 0.11, 0.07) * len * 0.9);
          vec3 scatter = vec3(0.03, 0.11, 0.13) * (uSunColor * 0.35 + uSkyZenith * 0.6);
          refl += trans * (1.0 - absorb) * scatter;
          trans *= absorb;
        } else {
          vec3 tint = uMatA[m].rgb;
          trans *= mix(vec3(1.0), tint, clamp(0.08 * len, 0.0, 0.5));
        }
        if (max(trans.r, max(trans.g, trans.b)) < 0.02) return false;
      } else {
        // Every surface is the face of the voxel the ray entered.
        h.t = t;
        h.p = ro + rd * t;
        h.n = length(face) > 0.0 ? face : -rd;
        h.d = d;
        h.cell = v;
        return true;
      }
    } else {
      inTrans = false;
    }
    // DDA step.
    if (side.x < side.y && side.x < side.z) { t = side.x; side.x += tDelta.x; v.x += int(stp.x); face = vec3(-stp.x, 0, 0); }
    else if (side.y < side.z) { t = side.y; side.y += tDelta.y; v.y += int(stp.y); face = vec3(0, -stp.y, 0); }
    else { t = side.z; side.z += tDelta.z; v.z += int(stp.z); face = vec3(0, 0, -stp.z); }
  }
  return false;
}

// Sunlight reaching a point (voxel coords) past the world's matter: one
// filtered lookup in the precomputed visibility, or a shadow ray as fallback.
float sunLight(vec3 pv) {
  if (uSunVisOn == 0) return shadowWorld(pv, uSunDir);
  if (pv.y < 0.0) return 0.0;
  if (any(lessThan(pv, vec3(0.0))) || any(greaterThanEqual(pv, vec3(uWorldDims)))) return 1.0;
  return texture(uSunVis, pv / vec3(uWorldDims)).r;
}

// Soft shadow from bodies (coarse occupancy).
float shadowDyn(vec3 pw, vec3 dir) {
  float vis = 1.0;
  float step = uVS * uDynScale * 0.6;
  float t = step * 1.6;
  for (int i = 0; i < 40; i++) {
    vec3 q = pw + dir * t;
    float o = dynOcc(q);
    vis *= 1.0 - clamp(o * 0.9, 0.0, 1.0);
    if (vis < 0.03) break;
    t += step;
  }
  return vis;
}

bool solidAt(ivec3 c) { uint m = vox(c).r; return m != 0u && !isTrans(m); }

// Voxel ambient occlusion: the eight voxels around the face, one layer out,
// darken its corners (side, side, corner), blended across the face.
float voxelAO(Hit h) {
  ivec3 n = ivec3(round(h.n));
  if (abs(n.x) + abs(n.y) + abs(n.z) != 1) return 1.0;
  ivec3 t1 = n.x != 0 ? ivec3(0, 1, 0) : ivec3(1, 0, 0);
  ivec3 t2 = n.z != 0 ? ivec3(0, 1, 0) : ivec3(0, 0, 1);
  ivec3 b = h.cell + n;
  float a1 = solidAt(b - t1) ? 1.0 : 0.0, b1 = solidAt(b + t1) ? 1.0 : 0.0;
  float a2 = solidAt(b - t2) ? 1.0 : 0.0, b2 = solidAt(b + t2) ? 1.0 : 0.0;
  float c00 = solidAt(b - t1 - t2) ? 1.0 : 0.0, c10 = solidAt(b + t1 - t2) ? 1.0 : 0.0;
  float c01 = solidAt(b - t1 + t2) ? 1.0 : 0.0, c11 = solidAt(b + t1 + t2) ? 1.0 : 0.0;
  float k00 = a1 * a2 > 0.5 ? 0.0 : 3.0 - a1 - a2 - c00;
  float k10 = b1 * a2 > 0.5 ? 0.0 : 3.0 - b1 - a2 - c10;
  float k01 = a1 * b2 > 0.5 ? 0.0 : 3.0 - a1 - b2 - c01;
  float k11 = b1 * b2 > 0.5 ? 0.0 : 3.0 - b1 - b2 - c11;
  vec2 f = clamp(vec2(dot(h.p, vec3(t1)), dot(h.p, vec3(t2))) - vec2(dot(vec3(h.cell), vec3(t1)), dot(vec3(h.cell), vec3(t2))), 0.0, 1.0);
  float k = mix(mix(k00, k10, f.x), mix(k01, k11, f.x), f.y) / 3.0;
  return 0.3 + 0.7 * k;
}

// ---------------------------------------------------------- surface look
vec3 albedoFor(uvec4 d, vec3 pw, vec3 nw, vec3 pv) {
  uint m = d.r;
  vec3 c = uMatA[m].rgb;
  int pat = int(uMatB[m].y + 0.5);
  float h = hash13(floor(pv) + 0.37);
  float partTint = (float(d.a) / 255.0 - 0.5);
  if (pat == 1) {        // wood grain
    float g = sin((pw.x * 9.0 + pw.z * 3.0 + pw.y * 1.5) * 6.0 + fbm(pw * 6.0) * 6.0);
    c *= 0.86 + 0.12 * g + 0.08 * partTint;
  } else if (pat == 2) { // brick: each brick its own shade
    c *= 0.85 + 0.25 * partTint + 0.08 * (h - 0.5);
    c = mix(c, c * vec3(1.05, 0.95, 0.9), fbm(pw * 18.0) * 0.4);
  } else if (pat == 3) {
    c *= 0.92 + 0.14 * h + 0.06 * partTint;
  } else if (pat == 4) { // grass: green on top, soil on the sides
    vec3 dirt = vec3(0.16, 0.085, 0.04);
    float top = smoothstep(0.35, 0.8, nw.y);
    vec3 g = mix(c, c * vec3(1.2, 1.12, 0.6), smoothstep(0.35, 0.85, fbm(pw * 1.6))) * (0.78 + 0.3 * h) * (0.85 + 0.3 * fbm(pw * 9.0));
    c = mix(dirt * (0.85 + 0.3 * h), g, top);
  } else if (pat == 5) { // metal
    c *= 0.95 + 0.06 * sin(pw.x * 260.0 + pw.z * 40.0) * 0.5 + 0.05 * partTint;
  } else if (pat == 6) { // stone
    c *= 0.82 + 0.25 * fbm(pw * 14.0) + 0.1 * (h - 0.5);
  } else if (pat == 7) { // leaves
    c *= 0.7 + 0.5 * h;
    c = mix(c, c * vec3(1.2, 1.1, 0.5), step(0.92, h));
  } else if (pat == 8) { // cells
    vec3 f = fract(pv * 0.5);
    float grid = step(0.92, max(f.x, max(f.y, f.z)));
    c *= 1.0 - grid * 0.35;
  }
  // Damage darkens and cracks.
  float integ = float(d.b) / 255.0;
  if (integ < 0.99) c *= mix(0.45, 1.0, integ) * (0.85 + 0.3 * step(0.3, noise3(pv * 3.0)));
  return max(c, vec3(0.0));
}

vec3 shadePoint(vec3 pw, vec3 nw, vec3 rdw, uvec4 d, vec3 pv, float sunVis, float ao, bool highlight) {
  uint m = d.r;
  vec3 alb = albedoFor(d, pw, nw, pv);
  float rough = uMatA[m].a;
  float metal = uMatB[m].x;
  uint glow = d.g;
  bool burning = (glow & 64u) != 0u;
  if (burning) alb *= 0.25;
  // Sun.
  float ndl = max(dot(nw, uSunDir), 0.0);
  vec3 diffuseAlb = alb * (1.0 - metal);
  vec3 col = diffuseAlb * uSunColor * ndl * sunVis;
  // Sky and ground bounce.
  vec3 sky = mix(uSkyHorizon, uSkyZenith, 0.5) * (0.55 + 0.45 * nw.y);
  vec3 bounce = vec3(0.32, 0.27, 0.2) * uSunColor * 0.12 * max(-nw.y * 0.5 + 0.5, 0.0);
  col += diffuseAlb * (sky * 0.55 + bounce) * ao;
  // Specular (GGX-ish).
  vec3 hv = normalize(uSunDir - rdw);
  float a = max(rough * rough, 0.02);
  float ndh = max(dot(nw, hv), 0.0);
  float dd = ndh * ndh * (a * a - 1.0) + 1.0;
  float D = a * a / (3.14159 * dd * dd);
  vec3 F0 = mix(vec3(0.04), alb, metal);
  float vdh = max(dot(-rdw, hv), 0.0);
  vec3 F = F0 + (1.0 - F0) * pow5(1.0 - vdh);
  col += F * D * ndl * sunVis * uSunColor * 0.25;
  // Environment reflection for metals/glossy surfaces.
  vec3 env = skyColor(reflect(rdw, nw));
  float fres = pow5(1.0 - max(dot(nw, -rdw), 0.0));
  col += env * mix(F0, vec3(1.0), fres) * (1.0 - rough) * 0.35 * ao;
  // Point lights (bulbs, fires, blasts).
  for (int i = 0; i < uNumLights; i++) {
    vec3 L = uLightPos[i].xyz - pw;
    float dist = length(L);
    float r = uLightPos[i].w;
    if (dist > r) continue;
    L /= dist;
    float fall = clamp(1.0 - dist / r, 0.0, 1.0); float att = fall * fall / (1.0 + dist * dist * 4.0);
    col += diffuseAlb * uLightCol[i].rgb * max(dot(nw, L), 0.0) * att;
  }
  // Incandescence and fire.
  uint lvl = glow & 63u;
  if (lvl > 0u) col += blackbody(float(lvl)) * (m == MAT_MOLTEN ? 1.4 : 1.0);
  if (burning) {
    float fl = 0.7 + 0.3 * noise3(pv * 0.6 + vec3(0.0, uTime * 4.0, 0.0));
    col += vec3(2.6, 0.9, 0.18) * fl;
  }
  if ((glow & 128u) != 0u) {
    if (m == MAT_FILAMENT) col += vec3(9.0, 7.6, 4.8);       // filament
    else col += vec3(3.0, 0.3, 0.1) * (0.6 + 0.4 * sin(uTime * 12.0));
  }
  if (highlight) {
    float rim = clamp(1.0 - max(dot(nw, -rdw), 0.0), 0.0, 1.0); rim *= rim;
    col = mix(col, col * 1.25 + vec3(0.05, 0.12, 0.16), 0.6) + vec3(0.25, 0.75, 1.0) * rim * (0.35 + 0.25 * uHoverPulse);
  }
  return col;
}

vec3 thermalColor(float tC) {
  float x = clamp((tC - 0.0) / 1200.0, 0.0, 1.0);
  vec3 c = mix(vec3(0.05, 0.05, 0.25), vec3(0.6, 0.0, 0.7), smoothstep(0.0, 0.08, x));
  c = mix(c, vec3(1.0, 0.25, 0.0), smoothstep(0.06, 0.25, x));
  c = mix(c, vec3(1.0, 0.9, 0.2), smoothstep(0.2, 0.6, x));
  c = mix(c, vec3(1.0), smoothstep(0.6, 1.0, x));
  return c;
}

void main() {
  vec3 rdw;
#ifdef WORLD
  vec4 a = uInvViewProj * vec4(vUV * 2.0 - 1.0, -1.0, 1.0);
  vec4 b = uInvViewProj * vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
  vec3 pa = a.xyz / a.w, pb = b.xyz / b.w;
  rdw = normalize(pb - pa);
  vec3 ro = uCamPos / uVS;
  vec3 rd = rdw;
#else
  rdw = normalize(vWorld - uCamPos);
  vec3 ro = (uModelInv * vec4(uCamPos, 1.0)).xyz;
  vec3 rd = normalize((uModelInv * vec4(uCamPos + rdw, 1.0)).xyz - ro);
#endif
  if (uDebug == 1) { oColor = vec4(skyColor(rdw), 1.0); gl_FragDepth = 1.0; return; }
  Hit h;
  vec3 trans = vec3(1.0), refl = vec3(0.0);
  float firstT = -1.0;
  bool got = march(ro, rd, 1e6, h, trans, refl, firstT);
  if (uDebug == 2) { oColor = vec4(got ? h.n * 0.5 + 0.5 : vec3(0.0, 0.0, 0.3), 1.0); gl_FragDepth = 0.5; return; }
  if (uDebug == 11) { oColor = vec4(vec3(float(gMarchSteps) / 1024.0), 1.0); gl_FragDepth = 0.5; return; }  // march iterations
  if (uDebug == 12 && !got) { oColor = vec4(0.0, 0.0, 0.0, 1.0); gl_FragDepth = 1.0; return; }
  vec3 col;
  float depth = 1.0;
  if (got) {
#ifdef WORLD
    vec3 pw = h.p * uVS;
    vec3 nw = h.n;
#else
    vec3 pw = (uModel * vec4(h.p, 1.0)).xyz;
    vec3 nw = normalize(uObjRot * h.n);
#endif
    vec3 pvw = pw / uVS;
    float sunVis = 1.0;
    if (uShadows != 0 && dot(nw, uSunDir) > 0.0) {
#ifdef WORLD
      // The sunlight of the air voxel this face looks into: shadows fall in
      // whole voxels, like everything else.
      ivec3 a = h.cell + ivec3(round(h.n));
      if (uSunVisOn != 0 && all(greaterThanEqual(a, ivec3(0))) && all(lessThan(a, uWorldDims))) sunVis = texelFetch(uSunVis, a, 0).r;
      else sunVis = sunLight(pvw + nw * 1.1 + uSunDir * 0.5);
#else
      sunVis = sunLight(pvw + nw * 1.1 + uSunDir * 0.5);
#endif
      if (sunVis > 0.0) sunVis *= shadowDyn(pw + nw * uVS * 1.5, uSunDir);
    }
    float ao = clamp(voxelAO(h) * (1.0 - clamp(dynOcc(pw + nw * uVS * uDynScale * 0.9) * 0.45, 0.0, 0.6)), 0.12, 1.0);
    if (uDebug == 12) {  // shadow-march iterations / 1024 (read back as a work measure)
      oColor = vec4(vec3(float(gShadowSteps) / 1024.0), 1.0);
      gl_FragDepth = 0.5;
      return;
    }
    if (uDebug >= 4) {
      vec3 v = uDebug == 4 ? albedoFor(h.d, pw, nw, h.p) : uDebug == 5 ? vec3(ao) : uDebug == 6 ? vec3(sunVis) : uDebug == 8 ? h.p * 0.001 : nw;
      oColor = vec4(any(isnan(v)) || any(isinf(v)) ? vec3(1, 0, 1) : v, 1.0);
      gl_FragDepth = 0.5;
      return;
    }
    bool hl = false;
    if (uHover.w >= 0.0 && float(h.d.a) == uHover.w) {
      vec3 c = vec3(h.cell) + 0.5;
      hl = all(greaterThanEqual(c, uHoverMin - 0.01)) && all(lessThanEqual(c, uHoverMax + 0.01));
    }
    col = shadePoint(pw, nw, rdw, h.d, h.p, sunVis, ao, hl);
    float dist = length(pw - uCamPos);
    col = mix(col, uSkyHorizon * 0.95, 1.0 - exp(-dist * 0.012));
    col = col * trans + refl;
    vec4 clip = uViewProj * vec4(pw, 1.0);
    depth = clip.z / clip.w * 0.5 + 0.5;
  } else {
#ifdef WORLD
    col = skyColor(rdw);
    if (rdw.y < -1e-4) {
      // Endless lawn outside the simulated block.
      float tg = (uGroundY - uCamPos.y) / rdw.y;
      vec3 pg = uCamPos + rdw * tg;
      vec3 wmax = vec3(uWorldDims) * uVS;
      if (tg > 0.0 && (pg.x < 0.0 || pg.z < 0.0 || pg.x > wmax.x || pg.z > wmax.z)) {
        vec3 g = vec3(0.068, 0.25, 0.03) * (0.8 + 0.35 * fbm(vec3(pg.xz * 0.7, 0.0)));
        g = mix(g, g * vec3(1.25, 1.1, 0.6), smoothstep(0.45, 0.8, fbm(vec3(pg.xz * 0.15, 3.0))));
        vec3 sky = mix(uSkyHorizon, uSkyZenith, 0.5);
        vec3 lit = g * (uSunColor * max(uSunDir.y, 0.0) + sky * 0.55);
        col = mix(lit, uSkyHorizon * 0.95, 1.0 - exp(-tg * 0.012));
        vec4 clip = uViewProj * vec4(pg, 1.0);
        depth = clip.z / clip.w * 0.5 + 0.5;
      }
    }
    col = col * trans + refl;
    if (firstT >= 0.0) {
      vec3 pw = (ro + rd * firstT) * uVS;
      vec4 clip = uViewProj * vec4(pw, 1.0);
      depth = clip.z / clip.w * 0.5 + 0.5;
    }
#else
    if (firstT < 0.0) discard;
    // Only glass hit (no opaque behind within this object): blend over what is behind.
    vec3 pw = (uModel * vec4(ro + rd * firstT, 1.0)).xyz;
    vec4 clip = uViewProj * vec4(pw, 1.0);
    oColor = vec4(refl, 1.0 - dot(trans, vec3(0.3333)));
    gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
    return;
#endif
  }
  if (any(isnan(col)) || any(isinf(col))) col = vec3(0.0);
  col = min(col, vec3(200.0));
  if (uDebug == 3) { oColor = vec4(any(isnan(col)) || any(isinf(col)) ? vec3(1, 0, 1) : (any(lessThan(col, vec3(0.0))) ? vec3(0, 1, 0) : col / (1.0 + col)), 1.0); gl_FragDepth = depth; return; }
  depth *= uDepthScale;
  if (uGhost != 0) {
    float pulse = 0.5 + 0.5 * sin(uTime * 4.0);
    col = mix(col, vec3(0.3, 1.1, 1.25), 0.55 + 0.15 * pulse);
    oColor = vec4(col * 0.75, 0.75);
  } else {
    oColor = vec4(col, 1.0);
  }
  gl_FragDepth = depth;
}
)GLSL";

// Instanced voxels: loose debris and effect voxels (flames, sparks, embers,
// spray). Every one is a single world-aligned voxel at a grid position.
inline const char* kDebrisVS = R"GLSL(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 iPosSize;   // centre, edge length
layout(location = 3) in vec4 iColorGlow; // rgb, glow level (debris) or alpha (effects)
out vec3 vN;
out vec3 vColor;
out float vGlow;
out vec3 vW;
void main() {
  vN = aNormal;
  vColor = iColorGlow.rgb;
  vGlow = iColorGlow.a;
  vW = iPosSize.xyz + (aPos - 0.5) * iPosSize.w;
  gl_Position = uViewProj * vec4(vW, 1.0);
}
)GLSL";

inline const char* kDebrisFS = R"GLSL(
in vec3 vN;
in vec3 vColor;
in float vGlow;
in vec3 vW;
layout(location = 0) out vec4 oColor;
void main() {
  vec3 n = normalize(vN);
  float ndl = max(dot(n, uSunDir), 0.0);
  vec3 sky = mix(uSkyHorizon, uSkyZenith, 0.5) * (0.55 + 0.45 * n.y);
  vec3 c = vColor * (uSunColor * ndl * 0.9 + sky * 0.5);
  c += blackbody(vGlow) * step(0.5, vGlow);
  float dist = length(vW - uCamPos);
  c = mix(c, uSkyHorizon * 0.95, 1.0 - exp(-dist * 0.012));
  oColor = vec4(c, 1.0);
}
)GLSL";

// Bloom and final composite.
inline const char* kBrightFS = R"GLSL(
in vec2 vUV;
uniform sampler2D uSrc;
layout(location = 0) out vec4 oColor;
void main() {
  vec3 c = texture(uSrc, vUV).rgb;
  if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0);
  c = min(c, vec3(60.0));
  float l = max(c.r, max(c.g, c.b));
  float k = max(l - 1.0, 0.0);
  k = k * k / (k + 0.6);
  oColor = vec4(c * (k / max(l, 1e-4)), 1.0);
}
)GLSL";

inline const char* kBlurFS = R"GLSL(
in vec2 vUV;
uniform sampler2D uSrc;
uniform vec2 uTexel;
uniform float uMode;   // 0 = downsample, 1 = upsample-add
layout(location = 0) out vec4 oColor;
void main() {
  vec2 o = uTexel;
  vec3 c;
  if (uMode < 0.5) {
    c = texture(uSrc, vUV).rgb * 4.0;
    c += texture(uSrc, vUV + vec2(-o.x, -o.y)).rgb + texture(uSrc, vUV + vec2(o.x, -o.y)).rgb;
    c += texture(uSrc, vUV + vec2(-o.x, o.y)).rgb + texture(uSrc, vUV + vec2(o.x, o.y)).rgb;
    c /= 8.0;
  } else {
    c = texture(uSrc, vUV + vec2(-o.x * 2.0, 0)).rgb + texture(uSrc, vUV + vec2(o.x * 2.0, 0)).rgb;
    c += texture(uSrc, vUV + vec2(0, -o.y * 2.0)).rgb + texture(uSrc, vUV + vec2(0, o.y * 2.0)).rgb;
    c += (texture(uSrc, vUV + vec2(-o.x, -o.y)).rgb + texture(uSrc, vUV + vec2(o.x, -o.y)).rgb +
          texture(uSrc, vUV + vec2(-o.x, o.y)).rgb + texture(uSrc, vUV + vec2(o.x, o.y)).rgb) * 2.0;
    c /= 12.0;
  }
  oColor = vec4(c, 1.0);
}
)GLSL";

inline const char* kCompositeFS = R"GLSL(
in vec2 vUV;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uExposure;
uniform float uBloomStrength;
uniform float uFlash;
layout(location = 0) out vec4 oColor;
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
  // No edge smoothing: voxel edges stay as crisp as the voxels.
  vec3 c = texture(uScene, vUV).rgb;
  c += texture(uBloom, vUV).rgb * uBloomStrength;
  c *= uExposure * (1.0 + uFlash);
  c = aces(c);
  // Gentle warm grade and vignette.
  c = pow(c, vec3(1.0 / 2.2));
  vec2 q = vUV - 0.5;
  c *= 1.0 - dot(q, q) * 0.45;
  c += (hash13(vec3(gl_FragCoord.xy, uTime)) - 0.5) / 255.0;
  oColor = vec4(c, 1.0);
}
)GLSL";

}  // namespace shaders
