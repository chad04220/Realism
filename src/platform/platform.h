#pragma once
// Thin OS layer: one window, an OpenGL 4.3 core context, keyboard/mouse input
// and a clock. Win32 + WGL on Windows, X11 + GLX on Linux (used for headless
// testing under Xvfb).
#include <string>

namespace key {
// Letters and digits use their ASCII upper-case codes ('A', '1', ...).
enum : int {
  Space = 32, Escape = 256, Enter, Tab, Backspace, Delete, Insert, Left, Right, Up, Down,
  PageUp, PageDown, Home, End, Shift, Ctrl, Alt,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
  Minus, Equals, LBracket, RBracket, Comma, Period, Slash, Grave, Semicolon, Quote,
  Count = 320
};
}

namespace plat {

struct Input {
  bool down[key::Count] = {};
  bool pressed[key::Count] = {};
  bool released[key::Count] = {};
  bool mdown[3] = {}, mpressed[3] = {}, mreleased[3] = {};
  float mx = 0, my = 0;      // cursor position, pixels
  float dmx = 0, dmy = 0;    // raw mouse motion this frame (works while captured)
  float wheel = 0;           // notches this frame
  std::string text;          // typed characters this frame
};

bool init(const char* title, int w, int h, bool fullscreen, bool vsync);
void shutdown();
bool pump();                 // process OS events; false once the window is closed
void swap();
int width();
int height();
void setCaptured(bool captured);  // hide + lock cursor for mouse-look
bool captured();
bool focused();
double now();                // seconds
Input& input();
void* glProc(const char* name);
void setTitle(const char* title);
void toggleFullscreen();
int cpuThreads();
void messageBox(const char* title, const char* text);
std::string userDataDir();   // %APPDATA%\Realism on Windows, ~/.realism elsewhere

}  // namespace plat
