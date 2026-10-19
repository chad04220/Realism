#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

constexpr float PI = 3.14159265358979f;

struct vec2 {
  float x = 0, y = 0;
  vec2() = default;
  constexpr vec2(float x_, float y_) : x(x_), y(y_) {}
  vec2 operator+(vec2 b) const { return {x + b.x, y + b.y}; }
  vec2 operator-(vec2 b) const { return {x - b.x, y - b.y}; }
  vec2 operator*(float s) const { return {x * s, y * s}; }
};

struct vec3 {
  float x = 0, y = 0, z = 0;
  vec3() = default;
  constexpr vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  explicit constexpr vec3(float s) : x(s), y(s), z(s) {}
  vec3 operator+(const vec3& b) const { return {x + b.x, y + b.y, z + b.z}; }
  vec3 operator-(const vec3& b) const { return {x - b.x, y - b.y, z - b.z}; }
  vec3 operator*(const vec3& b) const { return {x * b.x, y * b.y, z * b.z}; }
  vec3 operator/(const vec3& b) const { return {x / b.x, y / b.y, z / b.z}; }
  vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  vec3 operator-() const { return {-x, -y, -z}; }
  vec3& operator+=(const vec3& b) { x += b.x; y += b.y; z += b.z; return *this; }
  vec3& operator-=(const vec3& b) { x -= b.x; y -= b.y; z -= b.z; return *this; }
  vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
  float& operator[](int i) { return (&x)[i]; }
  float operator[](int i) const { return (&x)[i]; }
};
inline vec3 operator*(float s, const vec3& v) { return v * s; }
inline float dot(const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline vec3 cross(const vec3& a, const vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(const vec3& v) { return std::sqrt(dot(v, v)); }
inline float length2(const vec3& v) { return dot(v, v); }
inline vec3 normalize(const vec3& v) { float l = length(v); return l > 1e-12f ? v / l : vec3(0, 1, 0); }
inline vec3 vmin(const vec3& a, const vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline vec3 vmax(const vec3& a, const vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline vec3 lerp(const vec3& a, const vec3& b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstep(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }

struct vec4 {
  float x = 0, y = 0, z = 0, w = 0;
  vec4() = default;
  constexpr vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  vec4(const vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
  vec3 xyz() const { return {x, y, z}; }
};

struct ivec3 {
  int x = 0, y = 0, z = 0;
  ivec3() = default;
  constexpr ivec3(int x_, int y_, int z_) : x(x_), y(y_), z(z_) {}
  ivec3 operator+(const ivec3& b) const { return {x + b.x, y + b.y, z + b.z}; }
  ivec3 operator-(const ivec3& b) const { return {x - b.x, y - b.y, z - b.z}; }
  bool operator==(const ivec3& b) const { return x == b.x && y == b.y && z == b.z; }
  bool operator!=(const ivec3& b) const { return !(*this == b); }
  int& operator[](int i) { return (&x)[i]; }
  int operator[](int i) const { return (&x)[i]; }
};
inline vec3 toF(const ivec3& v) { return {(float)v.x, (float)v.y, (float)v.z}; }
inline ivec3 floorI(const vec3& v) { return {(int)std::floor(v.x), (int)std::floor(v.y), (int)std::floor(v.z)}; }

struct mat3 {
  float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major: m[r*3+c]
  float& operator()(int r, int c) { return m[r * 3 + c]; }
  float operator()(int r, int c) const { return m[r * 3 + c]; }
  vec3 operator*(const vec3& v) const {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z};
  }
  mat3 operator*(const mat3& b) const {
    mat3 r;
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++) r(i, j) = (*this)(i, 0) * b(0, j) + (*this)(i, 1) * b(1, j) + (*this)(i, 2) * b(2, j);
    return r;
  }
  mat3 transposed() const { mat3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r(i, j) = (*this)(j, i); return r; }
  static mat3 zero() { mat3 r; for (float& f : r.m) f = 0; return r; }
  mat3 inverse() const {
    const float* a = m;
    float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    mat3 r;
    if (std::fabs(det) < 1e-20f) return mat3::zero();
    float id = 1.f / det;
    r.m[0] = (a[4] * a[8] - a[5] * a[7]) * id; r.m[1] = (a[2] * a[7] - a[1] * a[8]) * id; r.m[2] = (a[1] * a[5] - a[2] * a[4]) * id;
    r.m[3] = (a[5] * a[6] - a[3] * a[8]) * id; r.m[4] = (a[0] * a[8] - a[2] * a[6]) * id; r.m[5] = (a[2] * a[3] - a[0] * a[5]) * id;
    r.m[6] = (a[3] * a[7] - a[4] * a[6]) * id; r.m[7] = (a[1] * a[6] - a[0] * a[7]) * id; r.m[8] = (a[0] * a[4] - a[1] * a[3]) * id;
    return r;
  }
};

struct quat {
  float x = 0, y = 0, z = 0, w = 1;
  quat() = default;
  constexpr quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  static quat axisAngle(const vec3& axis, float a) {
    vec3 n = normalize(axis);
    float s = std::sin(a * 0.5f);
    return {n.x * s, n.y * s, n.z * s, std::cos(a * 0.5f)};
  }
  quat operator*(const quat& b) const {
    return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
            w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
  }
  quat conj() const { return {-x, -y, -z, w}; }
  quat normalized() const { float l = std::sqrt(x * x + y * y + z * z + w * w); return l > 0 ? quat{x / l, y / l, z / l, w / l} : quat{}; }
  vec3 rotate(const vec3& v) const {
    vec3 u(x, y, z);
    vec3 t = cross(u, v) * 2.f;
    return v + t * w + cross(u, t);
  }
  mat3 toMat3() const {
    mat3 r;
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    r.m[0] = 1 - 2 * (yy + zz); r.m[1] = 2 * (xy - wz); r.m[2] = 2 * (xz + wy);
    r.m[3] = 2 * (xy + wz); r.m[4] = 1 - 2 * (xx + zz); r.m[5] = 2 * (yz - wx);
    r.m[6] = 2 * (xz - wy); r.m[7] = 2 * (yz + wx); r.m[8] = 1 - 2 * (xx + yy);
    return r;
  }
  // Integrate angular velocity w over dt.
  quat integrated(const vec3& av, float dt) const {
    quat q = quat{av.x, av.y, av.z, 0} * (*this);
    return quat{x + q.x * 0.5f * dt, y + q.y * 0.5f * dt, z + q.z * 0.5f * dt, w + q.w * 0.5f * dt}.normalized();
  }
};

struct mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // column-major (OpenGL)
  float& at(int r, int c) { return m[c * 4 + r]; }
  float at(int r, int c) const { return m[c * 4 + r]; }
  mat4 operator*(const mat4& b) const {
    mat4 r;
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++) {
        float s = 0;
        for (int k = 0; k < 4; k++) s += at(i, k) * b.at(k, j);
        r.at(i, j) = s;
      }
    return r;
  }
  vec4 operator*(const vec4& v) const {
    return {at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z + at(0, 3) * v.w,
            at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z + at(1, 3) * v.w,
            at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z + at(2, 3) * v.w,
            at(3, 0) * v.x + at(3, 1) * v.y + at(3, 2) * v.z + at(3, 3) * v.w};
  }
  static mat4 perspective(float fovy, float aspect, float n, float f) {
    mat4 r;
    float t = 1.f / std::tan(fovy * 0.5f);
    for (float& v : r.m) v = 0;
    r.at(0, 0) = t / aspect; r.at(1, 1) = t;
    r.at(2, 2) = (f + n) / (n - f); r.at(2, 3) = 2 * f * n / (n - f); r.at(3, 2) = -1;
    return r;
  }
  static mat4 ortho(float l, float rr, float b, float t, float n, float f) {
    mat4 r;
    r.at(0, 0) = 2 / (rr - l); r.at(1, 1) = 2 / (t - b); r.at(2, 2) = -2 / (f - n);
    r.at(0, 3) = -(rr + l) / (rr - l); r.at(1, 3) = -(t + b) / (t - b); r.at(2, 3) = -(f + n) / (f - n);
    return r;
  }
  static mat4 lookAt(const vec3& eye, const vec3& target, const vec3& up) {
    vec3 f = normalize(target - eye), s = normalize(cross(f, up)), u = cross(s, f);
    mat4 r;
    r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z; r.at(0, 3) = -dot(s, eye);
    r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z; r.at(1, 3) = -dot(u, eye);
    r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z; r.at(2, 3) = dot(f, eye);
    return r;
  }
  static mat4 trs(const vec3& t, const quat& q, const vec3& s) {
    mat3 R = q.toMat3();
    mat4 r;
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++) r.at(i, j) = R(i, j) * s[j];
    r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
    return r;
  }
  mat4 inverse() const;
};

inline mat4 mat4::inverse() const {
  const float* a = m;
  float inv[16];
  inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
  float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  mat4 r;
  if (std::fabs(det) < 1e-30f) return r;
  det = 1.f / det;
  for (int i = 0; i < 16; i++) r.m[i] = inv[i] * det;
  return r;
}

// Small fast PRNG.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : s(seed ? seed : 1) {}
  uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
  float f() { return (next() & 0xFFFFFF) / 16777216.f; }
  float range(float a, float b) { return a + (b - a) * f(); }
  int irange(int a, int b) { return a + (int)(next() % (uint32_t)(b - a + 1)); }
  vec3 unitVec() { for (;;) { vec3 v(range(-1, 1), range(-1, 1), range(-1, 1)); float l = length2(v); if (l > 1e-4f && l <= 1) return v / std::sqrt(l); } }
};
inline Rng& rng() { static thread_local Rng r(0x1234567ull + (uint64_t)(uintptr_t)&r); return r; }

inline uint32_t hash3(int x, int y, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
