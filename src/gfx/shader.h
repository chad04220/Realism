#pragma once
#include <string>
#include <unordered_map>
#include "../core/math.h"
#include "gl.h"

class Shader {
 public:
  bool build(const char* name, const std::string& vs, const std::string& fs);
  bool buildCompute(const char* name, const std::string& cs);
  void use() const { glUseProgram(prog_); }
  GLint loc(const char* n);
  void set(const char* n, int v) { glUniform1i(loc(n), v); }
  void set(const char* n, float v) { glUniform1f(loc(n), v); }
  void set(const char* n, const vec2& v) { glUniform2f(loc(n), v.x, v.y); }
  void set(const char* n, const vec3& v) { glUniform3f(loc(n), v.x, v.y, v.z); }
  void set(const char* n, const vec4& v) { glUniform4f(loc(n), v.x, v.y, v.z, v.w); }
  void set(const char* n, const ivec3& v) { glUniform3i(loc(n), v.x, v.y, v.z); }
  void set(const char* n, const mat4& m) { glUniformMatrix4fv(loc(n), 1, GL_FALSE, m.m); }
  GLuint id() const { return prog_; }
  static std::string lastError;

 private:
  GLuint prog_ = 0;
  std::unordered_map<std::string, GLint> locs_;
};
