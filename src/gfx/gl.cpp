#include "gl.h"
#include "../platform/platform.h"

#define GL_DEFINE(ret, name, ...) PFN_gl##name gl##name = nullptr;
GL_FUNCS(GL_DEFINE)
#undef GL_DEFINE

static const char* g_missing = nullptr;

bool glLoad() {
#define GL_LOAD(ret, name, ...) \
  gl##name = (PFN_gl##name)plat::glProc("gl" #name); \
  if (!gl##name && !g_missing) g_missing = "gl" #name;
  GL_FUNCS(GL_LOAD)
#undef GL_LOAD
  return g_missing == nullptr;
}

const char* glMissing() { return g_missing ? g_missing : ""; }
