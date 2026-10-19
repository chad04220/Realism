#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include "core/jobs.h"
#include "game/game.h"
#include "platform/platform.h"

#ifdef _WIN32
#include <windows.h>
int main(int argc, char** argv);
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif

int main(int argc, char** argv) {
  Game::Args args;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--shot" && i + 1 < argc) args.screenshot = argv[++i];
    else if (a == "--frames" && i + 1 < argc) args.frames = std::atoi(argv[++i]);
    else if (a == "--size" && i + 2 < argc) { args.w = std::atoi(argv[++i]); args.h = std::atoi(argv[++i]); }
    else if (a == "--script" && i + 1 < argc) args.script = argv[++i];
    else if (a == "--selftest") args.selftest = true;
    else if (a == "--voxel" && i + 1 < argc) args.voxelSize = (float)std::atof(argv[++i]);
  }
  Game game;
  return game.run(args);
}
