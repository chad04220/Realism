#pragma once
// Immediate-mode UI drawn on top of the frame: SDF text, rounded panels with
// soft shadows, gradients, lines and rings, plus a few widgets. Everything
// is batched into one vertex buffer per frame.
#include <string>
#include "../core/math.h"
#include "../platform/platform.h"

namespace ui {

struct Color {
  float r = 1, g = 1, b = 1, a = 1;
  constexpr Color() = default;
  constexpr Color(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
  Color alpha(float m) const { return {r, g, b, a * m}; }
  static Color hex(uint32_t h, float a = 1) { return {((h >> 16) & 255) / 255.f, ((h >> 8) & 255) / 255.f, (h & 255) / 255.f, a}; }
  Color mix(const Color& o, float t) const { return {r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t}; }
};

namespace theme {
inline const Color panel = Color::hex(0x12151c, 0.84f);
inline const Color panelHi = Color::hex(0x1b2029, 0.92f);
inline const Color line = Color(1, 1, 1, 0.08f);
inline const Color text = Color::hex(0xe8ecf1);
inline const Color dim = Color::hex(0x9aa4b2);
inline const Color faint = Color::hex(0x6b7480);
inline const Color accent = Color::hex(0x4fd1c5);
inline const Color warm = Color::hex(0xffb547);
inline const Color danger = Color::hex(0xff5d5d);
inline const Color good = Color::hex(0x5ee08a);
inline const Color power = Color::hex(0xffd84d);
}  // namespace theme

bool init();
void begin(int w, int h, float scale);
void end();
float scale();
int width();
int height();

// Primitives (pixel coordinates, top-left origin).
void rect(float x, float y, float w, float h, Color c, float radius = 0);
void rectBorder(float x, float y, float w, float h, Color fill, Color border, float radius, float borderW = 1);
void gradient(float x, float y, float w, float h, Color top, Color bottom, float radius = 0);
void shadow(float x, float y, float w, float h, float radius, float spread, float alpha);
void circle(float cx, float cy, float r, Color c);
void ring(float cx, float cy, float r, float thick, Color c);
void arc(float cx, float cy, float r, float thick, float a0, float a1, Color c);  // ring segment, radians
void line(float x0, float y0, float x1, float y1, float thick, Color c);
// Text: returns advance width. align 0 left, 1 centre, 2 right. weight > 0 thickens.
float text(float x, float y, const std::string& s, float size, Color c, int align = 0, float weight = 0);
float textWidth(const std::string& s, float size);
// Word-wrapped text; returns height used.
float textWrapped(float x, float y, float w, const std::string& s, float size, Color c, float lineGap = 1.25f);
void pushClip(float x, float y, float w, float h);
void popClip();

// Widgets.
struct Mouse { float x = 0, y = 0; bool down = false, pressed = false, released = false, rpressed = false; float wheel = 0; bool active = true; };
Mouse& mouse();
bool hover(float x, float y, float w, float h);
bool button(const char* label, float x, float y, float w, float h, bool primary = false, bool enabled = true);
bool iconButton(int icon, float x, float y, float s, bool selected, const char* tip = nullptr);
bool toggle(const char* label, float x, float y, float w, bool& v);
bool slider(const char* label, float x, float y, float w, float& v, float lo, float hi, const char* fmt = "%.2f");
void tooltip(const std::string& s);
bool mouseOverUI();       // true if the pointer is over something drawn interactively this frame
void claimMouse();        // mark the pointer as over UI

// Icons drawn from primitives (tools, parts, categories).
void icon(int id, float cx, float cy, float s, Color c);
enum Icon {
  I_HAND = 100, I_HAMMER, I_WRENCH, I_CHISEL, I_TORCH, I_HOSE, I_BOMB, I_BUILD, I_SCAN,
  I_PLAY, I_UNDO, I_REDO, I_ROTATE, I_MIRROR, I_TRASH, I_SAVE, I_EYEDROP, I_ERASE, I_PAINT, I_GEAR, I_CLOSE,
  I_BOLT, I_FLAME, I_THERMO, I_DROP
};

}  // namespace ui
