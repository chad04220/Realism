// Headless platform backend: no window, no OpenGL, no input. Built when
// REALISM_HEADLESS is on (Linux CI, servers) so the simulation and its
// self-tests can run anywhere. The game only uses it for --selftest.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include "platform.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace plat {
namespace {

Input g_in;
int g_w = 1600, g_h = 900;

double clockNow() {
  using clk = std::chrono::steady_clock;
  static const clk::time_point t0 = clk::now();
  return std::chrono::duration<double>(clk::now() - t0).count();
}

std::string envOr(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return v && *v ? std::string(v) : fallback;
}

}  // namespace

bool init(const char*, int w, int h, bool, bool) {
  g_w = w > 0 ? w : 1600;
  g_h = h > 0 ? h : 900;
  return true;
}

void shutdown() {}
bool pump() { return false; }
void swap() {}
int width() { return g_w; }
int height() { return g_h; }
void setCaptured(bool) {}
bool captured() { return false; }
bool focused() { return false; }
double now() { return clockNow(); }
Input& input() { return g_in; }
void* glProc(const char*) { return nullptr; }
void setTitle(const char*) {}
void toggleFullscreen() {}

int cpuThreads() {
  unsigned n = std::thread::hardware_concurrency();
  return n ? (int)n : 1;
}

void messageBox(const char* title, const char* text) { fprintf(stderr, "[%s] %s\n", title, text); }

std::string userDataDir() {
  // Tests should never touch the player's real settings, so allow an override.
  std::string d = envOr("REALISM_USER_DIR", ".");
#ifdef _WIN32
  _mkdir(d.c_str());
#else
  mkdir(d.c_str(), 0755);
#endif
  return d;
}

}  // namespace plat
