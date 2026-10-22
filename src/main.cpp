#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "core/jobs.h"
#include "game/game.h"
#include "platform/platform.h"

#ifndef REALISM_VERSION
#define REALISM_VERSION "dev"
#endif

#ifdef _WIN32
#include <windows.h>
int main(int argc, char** argv);
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif

namespace {

void usage(FILE* out) {
  fprintf(out,
          "Realism %s - a voxel physics sandbox\n"
          "\n"
          "Usage: Realism [options]\n"
          "\n"
          "  --selftest          Run the simulation self-test and exit (no window needed)\n"
          "  --headless          Run without a window; only with --selftest\n"
          "  --script <file>     Run a command script (see README)\n"
          "  --shot <file>       Save a screenshot (PPM) and exit\n"
          "  --frames <n>        Frames to render before the screenshot (default 1)\n"
          "  --size <w> <h>      Window size in pixels\n"
          "  --voxel <metres>    Voxel size, 0.02 or 0.025 (overrides settings.cfg)\n"
          "  -h, --help          Show this help and exit\n"
          "  --version           Print the version and exit\n",
          REALISM_VERSION);
}

bool parseLong(const char* s, long lo, long hi, long& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < lo || v > hi) return false;
  out = v;
  return true;
}

bool parseFloat(const char* s, float lo, float hi, float& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  float v = std::strtof(s, &end);
  if (end == s || *end != '\0' || !(v >= lo && v <= hi)) return false;
  out = v;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Game::Args args;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto value = [&](const char* option) -> const char* {
      if (i + 1 >= argc) {
        fprintf(stderr, "realism: %s needs a value (try --help)\n", option);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") { usage(stdout); return 0; }
    else if (a == "--version") { printf("Realism %s\n", REALISM_VERSION); return 0; }
    else if (a == "--headless") args.headless = true;
    else if (a == "--selftest") args.selftest = true;
    else if (a == "--shot") args.screenshot = value("--shot");
    else if (a == "--script") args.script = value("--script");
    else if (a == "--frames") {
      long n = 0;
      if (!parseLong(value("--frames"), 1, 1000000, n)) { fprintf(stderr, "realism: --frames needs a positive frame count\n"); return 2; }
      args.frames = (int)n;
    } else if (a == "--size") {
      long w = 0, h = 0;
      const char* ws = value("--size");
      const char* hs = value("--size");
      if (!parseLong(ws, 320, 7680, w) || !parseLong(hs, 240, 4320, h)) {
        fprintf(stderr, "realism: --size needs width 320-7680 and height 240-4320\n");
        return 2;
      }
      args.w = (int)w; args.h = (int)h;
    } else if (a == "--voxel") {
      float v = 0;
      if (!parseFloat(value("--voxel"), 0.02f, 0.025f, v)) { fprintf(stderr, "realism: --voxel needs a size between 0.02 and 0.025 m\n"); return 2; }
      args.voxelSize = v;
    } else {
      fprintf(stderr, "realism: unknown option '%s' (try --help)\n", a.c_str());
      return 2;
    }
  }
  if (args.headless && !args.selftest) {
    fprintf(stderr, "realism: --headless is only supported with --selftest\n");
    return 2;
  }
  if (!args.screenshot.empty() && args.frames == 0) args.frames = 1;
  Game game;
  return game.run(args);
}
