#include "shader.h"
#include <cstdio>

std::string Shader::lastError;

static GLuint compile(GLenum type, const std::string& src, const char* name) {
  GLuint s = glCreateShader(type);
  const char* p = src.c_str();
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    std::string log(len > 1 ? len : 1, '\0');
    glGetShaderInfoLog(s, len, nullptr, log.data());
    Shader::lastError = std::string(name) + (type == GL_VERTEX_SHADER ? " (vertex): " : " (fragment): ") + log;
    fprintf(stderr, "%s\n", Shader::lastError.c_str());
    glDeleteShader(s);
    return 0;
  }
  return s;
}

bool Shader::build(const char* name, const std::string& vsrc, const std::string& fsrc) {
  GLuint v = compile(GL_VERTEX_SHADER, vsrc, name), f = compile(GL_FRAGMENT_SHADER, fsrc, name);
  if (!v || !f) return false;
  prog_ = glCreateProgram();
  glAttachShader(prog_, v);
  glAttachShader(prog_, f);
  glLinkProgram(prog_);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetProgramiv(prog_, GL_INFO_LOG_LENGTH, &len);
    std::string log(len > 1 ? len : 1, '\0');
    glGetProgramInfoLog(prog_, len, nullptr, log.data());
    lastError = std::string(name) + " (link): " + log;
    fprintf(stderr, "%s\n", lastError.c_str());
    return false;
  }
  return true;
}

GLint Shader::loc(const char* n) {
  auto it = locs_.find(n);
  if (it != locs_.end()) return it->second;
  GLint l = glGetUniformLocation(prog_, n);
  locs_[n] = l;
  return l;
}
