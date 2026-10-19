#ifndef _WIN32
// X11 + GLX backend. Mainly for development and headless testing under Xvfb.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/glx.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include "platform.h"

namespace plat {
namespace {
Display* g_dpy = nullptr;
Window g_win = 0;
GLXContext g_ctx = nullptr;
Atom g_wmDelete;
Input g_in;
bool g_captured = false, g_quit = false, g_focus = true;
int g_w = 1280, g_h = 720;
double g_t0 = 0;
Cursor g_blank = 0;

double rawNow() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

int mapKey(KeySym s) {
  if (s >= XK_a && s <= XK_z) return 'A' + (int)(s - XK_a);
  if (s >= XK_A && s <= XK_Z) return (int)s;
  if (s >= XK_0 && s <= XK_9) return (int)s;
  if (s >= XK_F1 && s <= XK_F12) return key::F1 + (int)(s - XK_F1);
  switch (s) {
    case XK_space: return key::Space;
    case XK_Escape: return key::Escape;
    case XK_Return: return key::Enter;
    case XK_Tab: return key::Tab;
    case XK_BackSpace: return key::Backspace;
    case XK_Delete: return key::Delete;
    case XK_Insert: return key::Insert;
    case XK_Left: return key::Left;
    case XK_Right: return key::Right;
    case XK_Up: return key::Up;
    case XK_Down: return key::Down;
    case XK_Prior: return key::PageUp;
    case XK_Next: return key::PageDown;
    case XK_Home: return key::Home;
    case XK_End: return key::End;
    case XK_Shift_L: case XK_Shift_R: return key::Shift;
    case XK_Control_L: case XK_Control_R: return key::Ctrl;
    case XK_Alt_L: case XK_Alt_R: return key::Alt;
    case XK_minus: return key::Minus;
    case XK_equal: return key::Equals;
    case XK_bracketleft: return key::LBracket;
    case XK_bracketright: return key::RBracket;
    case XK_comma: return key::Comma;
    case XK_period: return key::Period;
    case XK_slash: return key::Slash;
    case XK_grave: return key::Grave;
    case XK_semicolon: return key::Semicolon;
    case XK_apostrophe: return key::Quote;
  }
  return -1;
}
typedef GLXContext (*CreateCtx)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
}  // namespace

bool init(const char* title, int w, int h, bool, bool vsync) {
  g_t0 = rawNow();
  g_dpy = XOpenDisplay(nullptr);
  if (!g_dpy) { fprintf(stderr, "Cannot open X display\n"); return false; }
  int fbAttr[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                  GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 24,
                  GLX_DOUBLEBUFFER, True, None};
  int n = 0;
  GLXFBConfig* cfgs = glXChooseFBConfig(g_dpy, DefaultScreen(g_dpy), fbAttr, &n);
  if (!cfgs || n == 0) { fprintf(stderr, "No GLX framebuffer config\n"); return false; }
  GLXFBConfig cfg = cfgs[0];
  XFree(cfgs);
  XVisualInfo* vi = glXGetVisualFromFBConfig(g_dpy, cfg);
  XSetWindowAttributes swa = {};
  swa.colormap = XCreateColormap(g_dpy, RootWindow(g_dpy, vi->screen), vi->visual, AllocNone);
  swa.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                   StructureNotifyMask | FocusChangeMask;
  g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, vi->screen), 0, 0, w, h, 0, vi->depth, InputOutput, vi->visual,
                        CWColormap | CWEventMask, &swa);
  XFree(vi);
  XStoreName(g_dpy, g_win, title);
  g_wmDelete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(g_dpy, g_win, &g_wmDelete, 1);
  XMapWindow(g_dpy, g_win);
  auto create = (CreateCtx)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
  int ctxAttr[] = {0x2091, 4, 0x2092, 3, 0x9126, 0x00000001, None};
  g_ctx = create ? create(g_dpy, cfg, nullptr, True, ctxAttr) : nullptr;
  if (!g_ctx) { fprintf(stderr, "Cannot create OpenGL 4.3 core context\n"); return false; }
  glXMakeCurrent(g_dpy, g_win, g_ctx);
  typedef void (*SwapIntervalEXT)(Display*, GLXDrawable, int);
  if (auto si = (SwapIntervalEXT)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT")) si(g_dpy, g_win, vsync ? 1 : 0);
  g_w = w; g_h = h;
  char data[8] = {};
  Pixmap pm = XCreateBitmapFromData(g_dpy, g_win, data, 8, 8);
  XColor black = {};
  g_blank = XCreatePixmapCursor(g_dpy, pm, pm, &black, &black, 0, 0);
  XFreePixmap(g_dpy, pm);
  XFlush(g_dpy);
  return true;
}

void shutdown() {
  if (!g_dpy) return;
  glXMakeCurrent(g_dpy, None, nullptr);
  if (g_ctx) glXDestroyContext(g_dpy, g_ctx);
  XDestroyWindow(g_dpy, g_win);
  XCloseDisplay(g_dpy);
}

bool pump() {
  for (int k = 0; k < key::Count; k++) g_in.pressed[k] = g_in.released[k] = false;
  for (int b = 0; b < 3; b++) g_in.mpressed[b] = g_in.mreleased[b] = false;
  g_in.dmx = g_in.dmy = 0; g_in.wheel = 0; g_in.text.clear();
  while (XPending(g_dpy)) {
    XEvent e;
    XNextEvent(g_dpy, &e);
    switch (e.type) {
      case ClientMessage: if ((Atom)e.xclient.data.l[0] == g_wmDelete) g_quit = true; break;
      case ConfigureNotify: g_w = e.xconfigure.width; g_h = e.xconfigure.height; break;
      case FocusIn: g_focus = true; break;
      case FocusOut: g_focus = false; break;
      case KeyPress: case KeyRelease: {
        // Skip auto-repeat release/press pairs.
        if (e.type == KeyRelease && XEventsQueued(g_dpy, QueuedAfterReading)) {
          XEvent nx;
          XPeekEvent(g_dpy, &nx);
          if (nx.type == KeyPress && nx.xkey.time == e.xkey.time && nx.xkey.keycode == e.xkey.keycode) {
            XNextEvent(g_dpy, &nx);
            char buf[8];
            KeySym ks;
            int len = XLookupString(&nx.xkey, buf, sizeof buf, &ks, nullptr);
            if (len == 1 && buf[0] >= 32 && buf[0] < 127) g_in.text.push_back(buf[0]);
            break;
          }
        }
        char buf[8];
        KeySym ks;
        int len = XLookupString(&e.xkey, buf, sizeof buf, &ks, nullptr);
        int k = mapKey(XLookupKeysym(&e.xkey, 0));
        if (e.type == KeyPress) {
          if (k >= 0) { if (!g_in.down[k]) g_in.pressed[k] = true; g_in.down[k] = true; }
          if (len == 1 && buf[0] >= 32 && buf[0] < 127) g_in.text.push_back(buf[0]);
        } else if (k >= 0) { g_in.down[k] = false; g_in.released[k] = true; }
        break;
      }
      case ButtonPress: case ButtonRelease: {
        int b = e.xbutton.button;
        bool dn = e.type == ButtonPress;
        if (b == 4 && dn) g_in.wheel += 1;
        else if (b == 5 && dn) g_in.wheel -= 1;
        else if (b >= 1 && b <= 3) {
          int idx = b == 1 ? 0 : b == 3 ? 1 : 2;
          if (dn) { g_in.mdown[idx] = g_in.mpressed[idx] = true; }
          else { g_in.mdown[idx] = false; g_in.mreleased[idx] = true; }
        }
        break;
      }
      case MotionNotify: {
        float x = (float)e.xmotion.x, y = (float)e.xmotion.y;
        if (g_captured) {
          float cx = g_w / 2.f, cy = g_h / 2.f;
          if (x != cx || y != cy) { g_in.dmx += x - cx; g_in.dmy += y - cy; }
        } else {
          g_in.dmx += x - g_in.mx; g_in.dmy += y - g_in.my;
        }
        g_in.mx = x; g_in.my = y;
        break;
      }
    }
  }
  if (g_captured && g_focus) { XWarpPointer(g_dpy, None, g_win, 0, 0, 0, 0, g_w / 2, g_h / 2); g_in.mx = g_w / 2.f; g_in.my = g_h / 2.f; }
  return !g_quit;
}

void swap() { glXSwapBuffers(g_dpy, g_win); }
int width() { return g_w; }
int height() { return g_h; }
bool focused() { return g_focus; }
bool captured() { return g_captured; }
void setCaptured(bool c) {
  if (c == g_captured) return;
  g_captured = c;
  if (c) {
    XGrabPointer(g_dpy, g_win, True, PointerMotionMask | ButtonPressMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, g_win, g_blank, CurrentTime);
  } else {
    XUngrabPointer(g_dpy, CurrentTime);
  }
}
double now() { return rawNow() - g_t0; }
Input& input() { return g_in; }
void* glProc(const char* name) { return (void*)glXGetProcAddressARB((const GLubyte*)name); }
void setTitle(const char* t) { XStoreName(g_dpy, g_win, t); }
void toggleFullscreen() {}
int cpuThreads() { unsigned n = std::thread::hardware_concurrency(); return n ? (int)n : 4; }
void messageBox(const char* title, const char* text) { fprintf(stderr, "[%s] %s\n", title, text); }
std::string userDataDir() {
  const char* home = getenv("HOME");
  std::string d = std::string(home ? home : ".") + "/.realism";
  mkdir(d.c_str(), 0755);
  return d;
}

}  // namespace plat
#endif
