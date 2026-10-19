#pragma once
// Minimal OpenGL 4.3 core loader: just the entry points and enums this game uses.
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#define GLAPIENTRY __stdcall
#else
#define GLAPIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NONE 0
#define GL_ZERO 0
#define GL_ONE 1
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_LINES 0x0001
#define GL_POINTS 0x0000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_LESS 0x0201
#define GL_LEQUAL 0x0203
#define GL_ALWAYS 0x0207
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_3D 0x806F
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RED 0x1903
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE0 0x84C0
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_R8 0x8229
#define GL_RG8 0x822B
#define GL_R16F 0x822D
#define GL_R32F 0x822E
#define GL_RG 0x8227
#define GL_RG_INTEGER 0x8228
#define GL_RED_INTEGER 0x8D94
#define GL_RGBA_INTEGER 0x8D99
#define GL_R8UI 0x8232
#define GL_RG8UI 0x8238
#define GL_RGBA8UI 0x8D7C
#define GL_RGBA8 0x8058
#define GL_RGBA16F 0x881A
#ifndef GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT1 0x8CE1
#endif
#define GL_RGB16F 0x881B
#define GL_R11F_G11F_B10F 0x8C3A
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_MAX_3D_TEXTURE_SIZE 0x8073
#define GL_PROGRAM_POINT_SIZE 0x8642
#define GL_TIME_ELAPSED 0x88BF
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_IMAGE_HEIGHT 0x806E
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_IMAGES 0x806D
#define GL_FUNC_ADD 0x8006

#define GL_FUNCS(X) \
  X(void, Clear, GLbitfield) \
  X(void, ClearColor, GLfloat, GLfloat, GLfloat, GLfloat) \
  X(void, ClearDepth, double) \
  X(void, Viewport, GLint, GLint, GLsizei, GLsizei) \
  X(void, Scissor, GLint, GLint, GLsizei, GLsizei) \
  X(void, Enable, GLenum) \
  X(void, Disable, GLenum) \
  X(void, BlendFunc, GLenum, GLenum) \
  X(void, BlendFuncSeparate, GLenum, GLenum, GLenum, GLenum) \
  X(void, DepthFunc, GLenum) \
  X(void, DepthMask, GLboolean) \
  X(void, ColorMask, GLboolean, GLboolean, GLboolean, GLboolean) \
  X(void, CullFace, GLenum) \
  X(GLenum, GetError) \
  X(const GLubyte*, GetString, GLenum) \
  X(void, GetIntegerv, GLenum, GLint*) \
  X(void, PixelStorei, GLenum, GLint) \
  X(void, ReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) \
  X(void, DrawArrays, GLenum, GLint, GLsizei) \
  X(void, DrawElements, GLenum, GLsizei, GLenum, const void*) \
  X(void, GenTextures, GLsizei, GLuint*) \
  X(void, DeleteTextures, GLsizei, const GLuint*) \
  X(void, BindTexture, GLenum, GLuint) \
  X(void, TexParameteri, GLenum, GLenum, GLint) \
  X(void, TexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) \
  X(void, TexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) \
  X(void, Finish) \
  X(void, Flush) \
  X(void, TexImage3D, GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) \
  X(void, TexSubImage3D, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*) \
  X(void, ActiveTexture, GLenum) \
  X(void, GenerateMipmap, GLenum) \
  X(void, GenBuffers, GLsizei, GLuint*) \
  X(void, DeleteBuffers, GLsizei, const GLuint*) \
  X(void, BindBuffer, GLenum, GLuint) \
  X(void, BufferData, GLenum, GLsizeiptr, const void*, GLenum) \
  X(void, BufferSubData, GLenum, GLintptr, GLsizeiptr, const void*) \
  X(void, GenVertexArrays, GLsizei, GLuint*) \
  X(void, DeleteVertexArrays, GLsizei, const GLuint*) \
  X(void, BindVertexArray, GLuint) \
  X(void, VertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) \
  X(void, VertexAttribIPointer, GLuint, GLint, GLenum, GLsizei, const void*) \
  X(void, EnableVertexAttribArray, GLuint) \
  X(void, VertexAttribDivisor, GLuint, GLuint) \
  X(void, DrawArraysInstanced, GLenum, GLint, GLsizei, GLsizei) \
  X(void, DrawElementsInstanced, GLenum, GLsizei, GLenum, const void*, GLsizei) \
  X(GLuint, CreateShader, GLenum) \
  X(void, ShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*) \
  X(void, CompileShader, GLuint) \
  X(void, GetShaderiv, GLuint, GLenum, GLint*) \
  X(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*) \
  X(GLuint, CreateProgram) \
  X(void, AttachShader, GLuint, GLuint) \
  X(void, LinkProgram, GLuint) \
  X(void, GetProgramiv, GLuint, GLenum, GLint*) \
  X(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*) \
  X(void, UseProgram, GLuint) \
  X(void, DeleteShader, GLuint) \
  X(void, DeleteProgram, GLuint) \
  X(GLint, GetUniformLocation, GLuint, const GLchar*) \
  X(void, Uniform1i, GLint, GLint) \
  X(void, Uniform1f, GLint, GLfloat) \
  X(void, Uniform2f, GLint, GLfloat, GLfloat) \
  X(void, Uniform3f, GLint, GLfloat, GLfloat, GLfloat) \
  X(void, Uniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat) \
  X(void, Uniform3i, GLint, GLint, GLint, GLint) \
  X(void, Uniform3fv, GLint, GLsizei, const GLfloat*) \
  X(void, Uniform4fv, GLint, GLsizei, const GLfloat*) \
  X(void, UniformMatrix3fv, GLint, GLsizei, GLboolean, const GLfloat*) \
  X(void, UniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat*) \
  X(void, GenFramebuffers, GLsizei, GLuint*) \
  X(void, DeleteFramebuffers, GLsizei, const GLuint*) \
  X(void, BindFramebuffer, GLenum, GLuint) \
  X(void, FramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint) \
  X(GLenum, CheckFramebufferStatus, GLenum) \
  X(void, DrawBuffers, GLsizei, const GLenum*) \
  X(void, BlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) \
  X(void, GenQueries, GLsizei, GLuint*) \
  X(void, BeginQuery, GLenum, GLuint) \
  X(void, EndQuery, GLenum) \
  X(void, GetQueryObjectiv, GLuint, GLenum, GLint*) \
  X(void, GetQueryObjectui64v, GLuint, GLenum, uint64_t*)

#define GL_DECLARE(ret, name, ...) typedef ret(GLAPIENTRY* PFN_gl##name)(__VA_ARGS__); extern PFN_gl##name gl##name;
GL_FUNCS(GL_DECLARE)
#undef GL_DECLARE

bool glLoad();  // returns false and names the missing function on failure
const char* glMissing();
