#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstring>
#include "platform.h"

namespace plat {
namespace {
HWND g_hwnd = nullptr;
HDC g_dc = nullptr;
HGLRC g_rc = nullptr;
HMODULE g_gl = nullptr;
Input g_in;
bool g_captured = false, g_quit = false, g_focus = true, g_fullscreen = false;
int g_w = 1280, g_h = 720;
WINDOWPLACEMENT g_prevPlace = {sizeof(g_prevPlace)};
LARGE_INTEGER g_freq, g_t0;

int mapKey(WPARAM vk, LPARAM lp) {
  if (vk >= 'A' && vk <= 'Z') return (int)vk;
  if (vk >= '0' && vk <= '9') return (int)vk;
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + (int)(vk - VK_NUMPAD0);
  if (vk >= VK_F1 && vk <= VK_F12) return key::F1 + (int)(vk - VK_F1);
  switch (vk) {
    case VK_SPACE: return key::Space;
    case VK_ESCAPE: return key::Escape;
    case VK_RETURN: return key::Enter;
    case VK_TAB: return key::Tab;
    case VK_BACK: return key::Backspace;
    case VK_DELETE: return key::Delete;
    case VK_INSERT: return key::Insert;
    case VK_LEFT: return key::Left;
    case VK_RIGHT: return key::Right;
    case VK_UP: return key::Up;
    case VK_DOWN: return key::Down;
    case VK_PRIOR: return key::PageUp;
    case VK_NEXT: return key::PageDown;
    case VK_HOME: return key::Home;
    case VK_END: return key::End;
    case VK_SHIFT: return key::Shift;
    case VK_CONTROL: return key::Ctrl;
    case VK_MENU: return key::Alt;
    case VK_OEM_MINUS: return key::Minus;
    case VK_OEM_PLUS: return key::Equals;
    case VK_OEM_4: return key::LBracket;
    case VK_OEM_6: return key::RBracket;
    case VK_OEM_COMMA: return key::Comma;
    case VK_OEM_PERIOD: return key::Period;
    case VK_OEM_2: return key::Slash;
    case VK_OEM_3: return key::Grave;
    case VK_OEM_1: return key::Semicolon;
    case VK_OEM_7: return key::Quote;
  }
  (void)lp;
  return -1;
}

void applyClip() {
  if (g_captured && g_focus) {
    RECT r;
    GetClientRect(g_hwnd, &r);
    POINT a = {r.left, r.top}, b = {r.right, r.bottom};
    ClientToScreen(g_hwnd, &a);
    ClientToScreen(g_hwnd, &b);
    RECT c = {a.x, a.y, b.x, b.y};
    ClipCursor(&c);
    while (ShowCursor(FALSE) >= 0) {}
  } else {
    ClipCursor(nullptr);
    while (ShowCursor(TRUE) < 0) {}
  }
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE: g_quit = true; return 0;
    case WM_SIZE: g_w = LOWORD(lp); g_h = HIWORD(lp); if (g_captured) applyClip(); return 0;
    case WM_SETFOCUS: g_focus = true; applyClip(); return 0;
    case WM_KILLFOCUS:
      g_focus = false;
      for (bool& d : g_in.down) d = false;
      for (bool& d : g_in.mdown) d = false;
      applyClip();
      return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
      int k = mapKey(wp, lp);
      if (k >= 0) { if (!g_in.down[k]) g_in.pressed[k] = true; g_in.down[k] = true; }
      if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) g_quit = true;
      return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
      int k = mapKey(wp, lp);
      if (k >= 0) { g_in.down[k] = false; g_in.released[k] = true; }
      return 0;
    }
    case WM_CHAR:
      if (wp >= 32 && wp < 127) g_in.text.push_back((char)wp);
      return 0;
    case WM_MOUSEMOVE: g_in.mx = (float)GET_X_LPARAM(lp); g_in.my = (float)GET_Y_LPARAM(lp); return 0;
    case WM_LBUTTONDOWN: g_in.mdown[0] = g_in.mpressed[0] = true; SetCapture(h); return 0;
    case WM_LBUTTONUP: g_in.mdown[0] = false; g_in.mreleased[0] = true; ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN: g_in.mdown[1] = g_in.mpressed[1] = true; SetCapture(h); return 0;
    case WM_RBUTTONUP: g_in.mdown[1] = false; g_in.mreleased[1] = true; ReleaseCapture(); return 0;
    case WM_MBUTTONDOWN: g_in.mdown[2] = g_in.mpressed[2] = true; return 0;
    case WM_MBUTTONUP: g_in.mdown[2] = false; g_in.mreleased[2] = true; return 0;
    case WM_MOUSEWHEEL: g_in.wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA; return 0;
    case WM_INPUT: {
      RAWINPUT ri;
      UINT sz = sizeof(ri);
      if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
          ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g_in.dmx += (float)ri.data.mouse.lLastX;
        g_in.dmy += (float)ri.data.mouse.lLastY;
      }
      break;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) { SetCursor(g_captured ? nullptr : LoadCursor(nullptr, IDC_ARROW)); return TRUE; }
      break;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

typedef HGLRC(WINAPI* PFNCREATECTX)(HDC, HGLRC, const int*);
typedef BOOL(WINAPI* PFNSWAPINTERVAL)(int);
}  // namespace

bool init(const char* title, int w, int h, bool fullscreen, bool vsync) {
  // Per-monitor DPI awareness so the UI isn't blurry on scaled laptop displays.
  HMODULE user = GetModuleHandleW(L"user32.dll");
  typedef BOOL(WINAPI * SetCtx)(HANDLE);
  if (auto f = (SetCtx)(void*)GetProcAddress(user, "SetProcessDpiAwarenessContext")) f((HANDLE)-4);
  else SetProcessDPIAware();
  // Laptops with Optimus/Enduro: ask for the discrete GPU.
  QueryPerformanceFrequency(&g_freq);
  QueryPerformanceCounter(&g_t0);
  timeBeginPeriod(1);

  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
  wc.lpszClassName = L"RealismWindow";
  RegisterClassExW(&wc);
  RECT r = {0, 0, w, h};
  DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRect(&r, style, FALSE);
  wchar_t wt[256];
  MultiByteToWideChar(CP_UTF8, 0, title, -1, wt, 256);
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, wt, style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) return false;
  g_dc = GetDC(g_hwnd);
  PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1};
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cDepthBits = 24;
  pfd.cStencilBits = 8;
  SetPixelFormat(g_dc, ChoosePixelFormat(g_dc, &pfd), &pfd);
  HGLRC tmp = wglCreateContext(g_dc);
  wglMakeCurrent(g_dc, tmp);
  auto create = (PFNCREATECTX)(void*)wglGetProcAddress("wglCreateContextAttribsARB");
  if (!create) { messageBox("Realism", "OpenGL 4.3 is required (wglCreateContextAttribsARB missing). Update your graphics driver."); return false; }
  const int attrs[] = {0x2091, 4, 0x2092, 3, 0x9126, 0x00000001, 0};  // 4.3 core
  g_rc = create(g_dc, nullptr, attrs);
  wglMakeCurrent(nullptr, nullptr);
  wglDeleteContext(tmp);
  if (!g_rc) { messageBox("Realism", "Could not create an OpenGL 4.3 core context. Update your graphics driver."); return false; }
  wglMakeCurrent(g_dc, g_rc);
  g_gl = LoadLibraryW(L"opengl32.dll");
  if (auto si = (PFNSWAPINTERVAL)(void*)wglGetProcAddress("wglSwapIntervalEXT")) si(vsync ? 1 : 0);

  RAWINPUTDEVICE rid = {0x01, 0x02, 0, g_hwnd};
  RegisterRawInputDevices(&rid, 1, sizeof(rid));
  ShowWindow(g_hwnd, SW_SHOW);
  RECT cr;
  GetClientRect(g_hwnd, &cr);
  g_w = cr.right; g_h = cr.bottom;
  if (fullscreen) toggleFullscreen();
  return true;
}

void shutdown() {
  setCaptured(false);
  if (g_rc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(g_rc); }
  if (g_hwnd) DestroyWindow(g_hwnd);
  timeEndPeriod(1);
}

bool pump() {
  for (int k = 0; k < key::Count; k++) g_in.pressed[k] = g_in.released[k] = false;
  for (int b = 0; b < 3; b++) g_in.mpressed[b] = g_in.mreleased[b] = false;
  g_in.dmx = g_in.dmy = 0; g_in.wheel = 0; g_in.text.clear();
  MSG m;
  while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
  return !g_quit;
}

void swap() { SwapBuffers(g_dc); }
int width() { return g_w; }
int height() { return g_h; }
bool focused() { return g_focus; }
bool captured() { return g_captured; }
void setCaptured(bool c) { if (c == g_captured) return; g_captured = c; applyClip(); }
double now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)(t.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart; }
Input& input() { return g_in; }

void* glProc(const char* name) {
  void* p = (void*)wglGetProcAddress(name);
  if (p == nullptr || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1)
    p = (void*)GetProcAddress(g_gl, name);
  return p;
}

void setTitle(const char* t) {
  wchar_t wt[256];
  MultiByteToWideChar(CP_UTF8, 0, t, -1, wt, 256);
  SetWindowTextW(g_hwnd, wt);
}

void toggleFullscreen() {
  DWORD style = GetWindowLong(g_hwnd, GWL_STYLE);
  if (!g_fullscreen) {
    MONITORINFO mi = {sizeof(mi)};
    if (GetWindowPlacement(g_hwnd, &g_prevPlace) && GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
      SetWindowLong(g_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
      SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
  } else {
    SetWindowLong(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
    SetWindowPlacement(g_hwnd, &g_prevPlace);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  g_fullscreen = !g_fullscreen;
}

int cpuThreads() { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }

void messageBox(const char* title, const char* text) { MessageBoxA(g_hwnd, text, title, MB_OK | MB_ICONINFORMATION); }

std::string userDataDir() {
  char path[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
    std::string d = std::string(path) + "\\Realism";
    CreateDirectoryA(d.c_str(), nullptr);
    return d;
  }
  return ".";
}

}  // namespace plat

// Prefer the discrete GPU on hybrid-graphics laptops.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
