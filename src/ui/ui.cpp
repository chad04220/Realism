#include "ui.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include "../gfx/gl.h"
#include "../gfx/shader.h"
#include "font_data.h"

namespace ui {
namespace {

struct Vtx { float x, y, lx, ly, hx, hy, r, g, b, a, r2, g2, b2, a2, type, rad, border, soft; };
struct Cmd { size_t start, count; int cx, cy, cw, ch; bool clip; };

std::vector<Vtx> g_v;
std::vector<Cmd> g_cmds;
std::vector<float> g_clipStack;  // x,y,w,h quads
Shader g_shader;
GLuint g_vao = 0, g_vbo = 0, g_font = 0, g_backdrop = 0;
int g_w = 0, g_h = 0;
float g_scale = 1;
Mouse g_mouse;
bool g_overUI = false, g_overUIPrev = false;
std::string g_tip;
int g_glyphIndex[0x2800];

const char* kVS = R"GLSL(
#version 430 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aLocal;
layout(location=2) in vec2 aHalf;
layout(location=3) in vec4 aCol;
layout(location=4) in vec4 aCol2;
layout(location=5) in vec4 aP;
uniform vec2 uScreen;
out vec2 vLocal; out vec2 vHalf; out vec4 vCol; out vec4 vCol2; out vec4 vP;
void main() {
  vLocal = aLocal; vHalf = aHalf; vCol = aCol; vCol2 = aCol2; vP = aP;
  gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
}
)GLSL";

const char* kFS = R"GLSL(
#version 430 core
in vec2 vLocal; in vec2 vHalf; in vec4 vCol; in vec4 vCol2; in vec4 vP;
uniform sampler2D uFont;
uniform sampler2D uBackdrop;
uniform vec2 uScreen;
out vec4 oColor;
float sdRound(vec2 p, vec2 b, float r) { vec2 q = abs(p) - b + r; return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }
void main() {
  int type = int(vP.x + 0.5);
  vec4 c;
  if (type == 1) {
    float d = texture(uFont, vLocal).r;
    float w = max(fwidth(d) * 0.7, 0.01);
    float edge = 0.5 - vP.z;
    float a = smoothstep(edge - w, edge + w, d);
    c = vec4(vCol.rgb, vCol.a * a);
  } else if (type == 6) {
    // Horizontal gradient, square corners: a soft shade across the screen.
    vec4 base = mix(vCol, vCol2, clamp(vLocal.x / max(vHalf.x, 1.0) * 0.5 + 0.5, 0.0, 1.0));
    float t = clamp(vLocal.x / max(vHalf.x, 1.0) * 0.5 + 0.5, 0.0, 1.0);
    base.a = mix(vCol.a, vCol2.a, t * t * (3.0 - 2.0 * t));
    c = base;
  } else if (type == 0 || type == 3) {
    float d = sdRound(vLocal, vHalf, vP.y);
    float aa = max(fwidth(d), 0.6);
    float fill = 1.0 - smoothstep(-aa, aa, d);
    vec4 base = vCol;
    if (type == 3) base = mix(vCol, vCol2, clamp(vLocal.y / max(vHalf.y, 1.0) * 0.5 + 0.5, 0.0, 1.0));
    if (vP.z > 0.0 && type == 0) {
      float inner = smoothstep(-vP.z - aa, -vP.z + aa, d);
      base = mix(base, vCol2, inner);
    }
    c = vec4(base.rgb, base.a * fill);
  } else if (type == 5) {
    // Frosted glass: the blurred frame behind, darkened and tinted, with a
    // faint sheen at the top and a hairline border.
    float d = sdRound(vLocal, vHalf, vP.y);
    float aa = max(fwidth(d), 0.6);
    float fill = 1.0 - smoothstep(-aa, aa, d);
    vec3 back = texture(uBackdrop, gl_FragCoord.xy / uScreen).rgb;
    float l = dot(back, vec3(0.299, 0.587, 0.114));
    back = mix(vec3(l), back, 0.75);
    vec3 base = mix(back * 0.55, vCol.rgb, vCol.a * 0.72);
    float top = clamp(vLocal.y / max(vHalf.y, 1.0) * 0.5 + 0.5, 0.0, 1.0);
    base += vec3(0.045) * (1.0 - smoothstep(0.0, 0.35, top));
    if (vP.z > 0.0) {
      float inner = smoothstep(-vP.z - aa, -vP.z + aa, d);
      base = mix(base, vCol2.rgb, inner * vCol2.a);
    }
    c = vec4(base, fill * min(1.0, vCol.a + 0.12));
  } else if (type == 2) {
    float d = sdRound(vLocal, vHalf, vP.y);
    float a = 1.0 - smoothstep(-vP.w, vP.w, d);
    c = vec4(0.0, 0.0, 0.0, a * vCol.a);
  } else {
    // Ring / arc: radius vP.y, thickness vP.z, angles in vCol2.xy.
    float r = length(vLocal);
    float d = abs(r - vP.y) - vP.z * 0.5;
    float aa = max(fwidth(d), 0.6);
    float a = 1.0 - smoothstep(-aa, aa, d);
    if (vCol2.x < vCol2.y) {
      float ang = atan(vLocal.y, vLocal.x);
      if (ang < vCol2.x) ang += 6.2831853;
      a *= step(vCol2.x, ang) * step(ang, vCol2.y);
    }
    c = vec4(vCol.rgb, vCol.a * a);
  }
  if (c.a <= 0.001) discard;
  oColor = c;
}
)GLSL";

void quad(float x, float y, float w, float h, const Vtx& proto, float lx0, float ly0, float lx1, float ly1) {
  Vtx v[4];
  for (int i = 0; i < 4; i++) v[i] = proto;
  v[0].x = x; v[0].y = y; v[0].lx = lx0; v[0].ly = ly0;
  v[1].x = x + w; v[1].y = y; v[1].lx = lx1; v[1].ly = ly0;
  v[2].x = x + w; v[2].y = y + h; v[2].lx = lx1; v[2].ly = ly1;
  v[3].x = x; v[3].y = y + h; v[3].lx = lx0; v[3].ly = ly1;
  g_v.push_back(v[0]); g_v.push_back(v[1]); g_v.push_back(v[2]);
  g_v.push_back(v[0]); g_v.push_back(v[2]); g_v.push_back(v[3]);
}

Vtx proto(Color c, Color c2, float type, float rad, float border, float soft, float hx, float hy) {
  Vtx v{};
  v.r = c.r; v.g = c.g; v.b = c.b; v.a = c.a;
  v.r2 = c2.r; v.g2 = c2.g; v.b2 = c2.b; v.a2 = c2.a;
  v.type = type; v.rad = rad; v.border = border; v.soft = soft; v.hx = hx; v.hy = hy;
  return v;
}

void newCmd() {
  Cmd c;
  c.start = g_v.size(); c.count = 0;
  c.clip = !g_clipStack.empty();
  if (c.clip) {
    size_t n = g_clipStack.size();
    c.cx = (int)g_clipStack[n - 4]; c.cy = (int)g_clipStack[n - 3]; c.cw = (int)g_clipStack[n - 2]; c.ch = (int)g_clipStack[n - 1];
  } else { c.cx = c.cy = c.cw = c.ch = 0; }
  g_cmds.push_back(c);
}

uint32_t nextCode(const std::string& s, size_t& i) {
  unsigned char c = (unsigned char)s[i++];
  if (c < 0x80) return c;
  int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
  uint32_t cp = c & (0x3F >> n);
  for (int k = 0; k < n && i < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
  return cp;
}

int glyphFor(uint32_t cp) {
  if (cp < 0x2800) { int g = g_glyphIndex[cp]; if (g >= 0) return g; }
  if (cp == 0x26A1) for (int i = 0; i < font::COUNT; i++) if (font::CODES[i] == 0x26A1) return i;
  return g_glyphIndex['?'];
}

}  // namespace

bool init() {
  if (!g_shader.build("ui", kVS, kFS)) return false;
  std::vector<uint8_t> atlas((size_t)font::W * font::H);
  size_t o = 0;
  for (int i = 0; i + 1 < font::RLE_LEN; i += 2)
    for (int k = 0; k < font::RLE[i] && o < atlas.size(); k++) atlas[o++] = font::RLE[i + 1];
  glGenTextures(1, &g_font);
  glBindTexture(GL_TEXTURE_2D, g_font);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, font::W, font::H, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  for (int& g : g_glyphIndex) g = -1;
  for (int i = 0; i < font::COUNT; i++) if (font::CODES[i] < 0x2800) g_glyphIndex[font::CODES[i]] = i;
  glGenVertexArrays(1, &g_vao);
  glBindVertexArray(g_vao);
  glGenBuffers(1, &g_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
  const int stride = sizeof(Vtx);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
  glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)8);
  glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)16);
  glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)24);
  glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)40);
  glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void*)56);
  glBindVertexArray(0);
  return true;
}

void begin(int w, int h, float s) {
  g_w = w; g_h = h; g_scale = s;
  g_v.clear();
  g_cmds.clear();
  g_clipStack.clear();
  newCmd();
  g_overUIPrev = g_overUI;
  g_overUI = false;
  g_tip.clear();
}

float scale() { return g_scale; }
int width() { return g_w; }
int height() { return g_h; }
Mouse& mouse() { return g_mouse; }
bool mouseOverUI() { return g_overUIPrev || g_overUI; }
void claimMouse() { g_overUI = true; }

void end() {
  if (!g_tip.empty()) {
    float s = g_scale;
    float tw = textWidth(g_tip, 15 * s) + 20 * s;
    float x = std::min(g_mouse.x + 16 * s, g_w - tw - 4 * s), y = g_mouse.y + 20 * s;
    shadow(x, y, tw, 28 * s, 6 * s, 10 * s, 0.5f);
    rectBorder(x, y, tw, 28 * s, theme::panelHi, theme::line, 6 * s);
    text(x + 10 * s, y + 6 * s, g_tip, 15 * s, theme::text);
  }
  if (!g_cmds.empty()) g_cmds.back().count = g_v.size() - g_cmds.back().start;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, g_w, g_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  g_shader.use();
  g_shader.set("uScreen", vec2((float)g_w, (float)g_h));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_font);
  g_shader.set("uFont", 0);
  glActiveTexture(GL_TEXTURE0 + 1);
  glBindTexture(GL_TEXTURE_2D, g_backdrop);
  g_shader.set("uBackdrop", 1);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(g_vao);
  glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
  glBufferData(GL_ARRAY_BUFFER, g_v.size() * sizeof(Vtx), g_v.data(), GL_STREAM_DRAW);
  for (auto& c : g_cmds) {
    if (!c.count) continue;
    if (c.clip) { glEnable(GL_SCISSOR_TEST); glScissor(c.cx, g_h - c.cy - c.ch, std::max(0, c.cw), std::max(0, c.ch)); }
    else glDisable(GL_SCISSOR_TEST);
    glDrawArrays(GL_TRIANGLES, (GLint)c.start, (GLsizei)c.count);
  }
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(0);
}

void pushClip(float x, float y, float w, float h) {
  g_cmds.back().count = g_v.size() - g_cmds.back().start;
  g_clipStack.insert(g_clipStack.end(), {x, y, w, h});
  newCmd();
}
void popClip() {
  g_cmds.back().count = g_v.size() - g_cmds.back().start;
  if (!g_clipStack.empty()) g_clipStack.resize(g_clipStack.size() - 4);
  newCmd();
}

void rect(float x, float y, float w, float h, Color c, float radius) {
  float pad = 1;
  Vtx p = proto(c, c, 0, std::min(radius, std::min(w, h) * 0.5f), 0, 0, w * 0.5f, h * 0.5f);
  quad(x - pad, y - pad, w + 2 * pad, h + 2 * pad, p, -w * 0.5f - pad, -h * 0.5f - pad, w * 0.5f + pad, h * 0.5f + pad);
}

void setBackdrop(unsigned tex) { g_backdrop = tex; }

void glass(float x, float y, float w, float h, Color tint, Color border, float radius, float bw) {
  float pad = 1;
  Vtx p = proto(tint, border, g_backdrop ? 5.f : 0.f, std::min(radius, std::min(w, h) * 0.5f), bw, 0, w * 0.5f, h * 0.5f);
  quad(x - pad, y - pad, w + 2 * pad, h + 2 * pad, p, -w * 0.5f - pad, -h * 0.5f - pad, w * 0.5f + pad, h * 0.5f + pad);
}

void rectBorder(float x, float y, float w, float h, Color fill, Color border, float radius, float bw) {
  // Dark, nearly opaque panels are drawn as frosted glass.
  if (g_backdrop && fill.a >= 0.75f && std::max(fill.r, std::max(fill.g, fill.b)) < 0.2f) { glass(x, y, w, h, fill, border, radius, bw); return; }
  float pad = 1;
  // The shader mixes from the fill (col) to the border (col2) in the edge band.
  Vtx p = proto(fill, border, 0, std::min(radius, std::min(w, h) * 0.5f), bw, 0, w * 0.5f, h * 0.5f);
  quad(x - pad, y - pad, w + 2 * pad, h + 2 * pad, p, -w * 0.5f - pad, -h * 0.5f - pad, w * 0.5f + pad, h * 0.5f + pad);
}

void gradient(float x, float y, float w, float h, Color top, Color bottom, float radius) {
  float pad = 1;
  Vtx p = proto(top, bottom, 3, std::min(radius, std::min(w, h) * 0.5f), 0, 0, w * 0.5f, h * 0.5f);
  quad(x - pad, y - pad, w + 2 * pad, h + 2 * pad, p, -w * 0.5f - pad, -h * 0.5f - pad, w * 0.5f + pad, h * 0.5f + pad);
}

void gradientH(float x, float y, float w, float h, Color left, Color right) {
  Vtx p = proto(left, right, 6, 0, 0, 0, w * 0.5f, h * 0.5f);
  quad(x, y, w, h, p, -w * 0.5f, -h * 0.5f, w * 0.5f, h * 0.5f);
}

void shadow(float x, float y, float w, float h, float radius, float spread, float alpha) {
  Vtx p = proto(Color(0, 0, 0, alpha), Color(), 2, radius, 0, spread, w * 0.5f, h * 0.5f);
  float e = spread * 2;
  quad(x - e, y - e + spread * 0.4f, w + 2 * e, h + 2 * e, p, -w * 0.5f - e, -h * 0.5f - e, w * 0.5f + e, h * 0.5f + e);
}

void circle(float cx, float cy, float r, Color c) { rect(cx - r, cy - r, 2 * r, 2 * r, c, r); }

void ring(float cx, float cy, float r, float thick, Color c) { arc(cx, cy, r, thick, 0, 0, c); }

void arc(float cx, float cy, float r, float thick, float a0, float a1, Color c) {
  float e = r + thick + 2;
  Vtx p = proto(c, Color(a0, a1, 0, 0), 4, r, thick, 0, e, e);
  quad(cx - e, cy - e, 2 * e, 2 * e, p, -e, -e, e, e);
}

void line(float x0, float y0, float x1, float y1, float thick, Color c) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-3f) { circle(x0, y0, thick * 0.5f, c); return; }
  float ux = dx / len, uy = dy / len;
  float hl = len * 0.5f + thick * 0.5f + 1, hw = thick * 0.5f + 1;
  float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
  Vtx p = proto(c, c, 0, thick * 0.5f, 0, 0, len * 0.5f + thick * 0.5f, thick * 0.5f);
  // Rotated quad: corners in local space (±hl, ±hw).
  float lx[4] = {-hl, hl, hl, -hl}, ly[4] = {-hw, -hw, hw, hw};
  Vtx v[4];
  for (int i = 0; i < 4; i++) {
    v[i] = p;
    v[i].x = cx + lx[i] * ux - ly[i] * uy;
    v[i].y = cy + lx[i] * uy + ly[i] * ux;
    v[i].lx = lx[i]; v[i].ly = ly[i];
  }
  g_v.push_back(v[0]); g_v.push_back(v[1]); g_v.push_back(v[2]);
  g_v.push_back(v[0]); g_v.push_back(v[2]); g_v.push_back(v[3]);
}

float textWidth(const std::string& s, float size) {
  float sc = size / font::EM, w = 0;
  for (size_t i = 0; i < s.size();) w += font::ADV[glyphFor(nextCode(s, i))] * sc;
  return w;
}

float text(float x, float y, const std::string& s, float size, Color c, int align, float weight) {
  float w = textWidth(s, size);
  if (align == 1) x -= w * 0.5f;
  else if (align == 2) x -= w;
  float sc = size / font::EM;
  float pen = x;
  // Snap the baseline to whole pixels so text stays crisp.
  y = std::floor(y + 0.5f);
  for (size_t i = 0; i < s.size();) {
    int g = glyphFor(nextCode(s, i));
    int col = g % font::COLS, row = g / font::COLS;
    float u0 = (float)(col * font::CELLW) / font::W, v0 = (float)(row * font::CELLH) / font::H;
    float u1 = (float)((col + 1) * font::CELLW) / font::W, v1 = (float)((row + 1) * font::CELLH) / font::H;
    float gx = pen - font::PAD * sc, gy = y - font::PAD * sc;
    if (font::CODES[g] != ' ') {
      Vtx p = proto(c, c, 1, 0, weight, 0, 0, 0);
      quad(gx, gy, font::CELLW * sc, font::CELLH * sc, p, u0, v0, u1, v1);
    }
    pen += font::ADV[g] * sc;
  }
  return w;
}

float textWrapped(float x, float y, float w, const std::string& s, float size, Color c, float gap) {
  float lineH = size * gap;
  std::string line, word;
  float yy = y;
  auto flush = [&]() { if (!line.empty()) { text(x, yy, line, size, c); yy += lineH; line.clear(); } };
  for (size_t i = 0; i <= s.size(); i++) {
    char ch = i < s.size() ? s[i] : ' ';
    if (ch == '\n') { std::string t = line.empty() ? word : line + " " + word; if (textWidth(t, size) > w) { flush(); line = word; } else line = t; word.clear(); flush(); continue; }
    if (ch == ' ') {
      if (word.empty()) continue;
      std::string t = line.empty() ? word : line + " " + word;
      if (textWidth(t, size) > w && !line.empty()) { flush(); line = word; }
      else line = t;
      word.clear();
    } else word.push_back(ch);
  }
  flush();
  return yy - y;
}

bool hover(float x, float y, float w, float h) {
  return g_mouse.active && g_mouse.x >= x && g_mouse.x < x + w && g_mouse.y >= y && g_mouse.y < y + h;
}

bool button(const char* label, float x, float y, float w, float h, bool primary, bool enabled) {
  float s = g_scale;
  bool hv = enabled && hover(x, y, w, h);
  if (hv) g_overUI = true;
  Color fill = primary ? theme::accent.alpha(hv ? 1.f : 0.88f) : (hv ? Color(1, 1, 1, 0.12f) : Color(1, 1, 1, 0.06f));
  if (!enabled) fill = Color(1, 1, 1, 0.03f);
  if (hv && g_mouse.down) fill = primary ? theme::accent.mix(Color(1, 1, 1), 0.2f) : Color(1, 1, 1, 0.18f);
  if (primary || !enabled) rectBorder(x, y, w, h, fill, primary ? Color(0, 0, 0, 0) : theme::line, 8 * s);
  else glass(x, y, w, h, hv ? Color::hex(g_mouse.down ? 0x3a4454 : 0x2a3240, 0.86f) : theme::panel, hv ? theme::accent.alpha(0.5f) : theme::line, 8 * s);
  Color tc = primary ? Color::hex(0x0b1a19) : (enabled ? theme::text : theme::faint);
  text(x + w * 0.5f, y + h * 0.5f - 9 * s, label, 16 * s, tc, 1, primary ? 0.05f : 0);
  return hv && g_mouse.released && enabled;
}

bool iconButton(int ic, float x, float y, float sz, bool selected, const char* tip) {
  float s = g_scale;
  bool hv = hover(x, y, sz, sz);
  if (hv) { g_overUI = true; if (tip) tooltip(tip); }
  Color fill = selected ? theme::accent.alpha(0.22f) : (hv ? Color(1, 1, 1, 0.1f) : Color(1, 1, 1, 0.04f));
  rectBorder(x, y, sz, sz, fill, selected ? theme::accent.alpha(0.9f) : theme::line, 8 * s, selected ? 1.5f * s : 1);
  icon(ic, x + sz * 0.5f, y + sz * 0.5f, sz * 0.55f, selected ? theme::accent : theme::text);
  return hv && g_mouse.released;
}

bool toggle(const char* label, float x, float y, float w, bool& v) {
  float s = g_scale;
  float h = 30 * s;
  bool hv = hover(x, y, w, h);
  if (hv) g_overUI = true;
  text(x, y + 6 * s, label, 15 * s, theme::text);
  float tw = 40 * s, th = 22 * s, tx = x + w - tw, ty = y + 4 * s;
  rect(tx, ty, tw, th, v ? theme::accent : Color(1, 1, 1, 0.15f), th * 0.5f);
  circle(v ? tx + tw - th * 0.5f : tx + th * 0.5f, ty + th * 0.5f, th * 0.5f - 3 * s, Color(1, 1, 1));
  if (hv && g_mouse.released) { v = !v; return true; }
  return false;
}

bool slider(const char* label, float x, float y, float w, float& v, float lo, float hi, const char* fmt) {
  float s = g_scale;
  float h = 44 * s;
  bool hv = hover(x, y, w, h);
  if (hv) g_overUI = true;
  char buf[64];
  snprintf(buf, sizeof buf, fmt, v);
  text(x, y + 2 * s, label, 15 * s, theme::text);
  text(x + w, y + 2 * s, buf, 15 * s, theme::dim, 2);
  float ty = y + 30 * s;
  float t = clampf((v - lo) / (hi - lo), 0, 1);
  rect(x, ty - 2 * s, w, 4 * s, Color(1, 1, 1, 0.12f), 2 * s);
  rect(x, ty - 2 * s, w * t, 4 * s, theme::accent, 2 * s);
  circle(x + w * t, ty, (hv ? 8.f : 7.f) * s, Color(1, 1, 1));
  bool changed = false;
  if (hv && g_mouse.down) {
    float nt = clampf((g_mouse.x - x) / w, 0, 1);
    float nv = lo + (hi - lo) * nt;
    if (nv != v) { v = nv; changed = true; }
  }
  return changed;
}

void tooltip(const std::string& s) { g_tip = s; }

void icon(int id, float cx, float cy, float s, Color c) {
  float t = std::max(1.5f, s * 0.11f);
  switch (id) {
    case I_HAND:
      rect(cx - s * 0.3f, cy - s * 0.05f, s * 0.6f, s * 0.5f, c, s * 0.15f);
      for (int i = 0; i < 4; i++) line(cx - s * 0.22f + i * s * 0.15f, cy, cx - s * 0.22f + i * s * 0.15f, cy - s * 0.42f + (i == 0 || i == 3 ? s * 0.1f : 0), s * 0.12f, c);
      line(cx - s * 0.3f, cy + s * 0.1f, cx - s * 0.48f, cy - s * 0.12f, s * 0.12f, c);
      break;
    case I_HAMMER:
      line(cx - s * 0.35f, cy + s * 0.4f, cx + s * 0.15f, cy - s * 0.1f, t * 1.4f, c);
      line(cx - s * 0.05f, cy - s * 0.42f, cx + s * 0.42f, cy + s * 0.05f, s * 0.24f, c);
      break;
    case I_WRENCH:
      line(cx - s * 0.32f, cy + s * 0.32f, cx + s * 0.12f, cy - s * 0.12f, t * 1.5f, c);
      arc(cx + s * 0.2f, cy - s * 0.2f, s * 0.18f, t * 1.4f, -0.8f, 3.6f, c);
      break;
    case I_CHISEL:
      line(cx - s * 0.35f, cy + s * 0.35f, cx + s * 0.05f, cy - s * 0.05f, t * 2.2f, c);
      line(cx + s * 0.05f, cy - s * 0.05f, cx + s * 0.38f, cy - s * 0.38f, t * 0.9f, c);
      break;
    case I_TORCH: case I_FLAME:
      circle(cx, cy + s * 0.15f, s * 0.25f, c);
      line(cx, cy + s * 0.1f, cx + s * 0.05f, cy - s * 0.42f, s * 0.22f, c);
      circle(cx, cy + s * 0.18f, s * 0.11f, Color(0, 0, 0, 0.45f));
      break;
    case I_HOSE: case I_DROP:
      circle(cx, cy + s * 0.12f, s * 0.26f, c);
      line(cx, cy - s * 0.42f, cx - s * 0.2f, cy + s * 0.02f, t * 1.6f, c);
      line(cx, cy - s * 0.42f, cx + s * 0.2f, cy + s * 0.02f, t * 1.6f, c);
      break;
    case I_BOMB:
      circle(cx - s * 0.05f, cy + s * 0.08f, s * 0.32f, c);
      line(cx + s * 0.16f, cy - s * 0.16f, cx + s * 0.32f, cy - s * 0.38f, t, c);
      circle(cx + s * 0.36f, cy - s * 0.42f, s * 0.07f, theme::warm);
      break;
    case I_BUILD:
      rect(cx - s * 0.36f, cy - s * 0.05f, s * 0.34f, s * 0.34f, c, s * 0.05f);
      rect(cx + s * 0.02f, cy - s * 0.05f, s * 0.34f, s * 0.34f, c.alpha(0.7f), s * 0.05f);
      rect(cx - s * 0.17f, cy - s * 0.42f, s * 0.34f, s * 0.34f, c.alpha(0.85f), s * 0.05f);
      break;
    case I_SCAN: case I_THERMO:
      ring(cx, cy, s * 0.3f, t, c);
      line(cx - s * 0.45f, cy, cx - s * 0.2f, cy, t, c);
      line(cx + s * 0.2f, cy, cx + s * 0.45f, cy, t, c);
      line(cx, cy - s * 0.45f, cx, cy - s * 0.2f, t, c);
      line(cx, cy + s * 0.2f, cx, cy + s * 0.45f, t, c);
      break;
    case I_PLAY:
      line(cx - s * 0.2f, cy - s * 0.32f, cx - s * 0.2f, cy + s * 0.32f, t * 1.6f, c);
      line(cx - s * 0.2f, cy - s * 0.32f, cx + s * 0.32f, cy, t * 1.6f, c);
      line(cx - s * 0.2f, cy + s * 0.32f, cx + s * 0.32f, cy, t * 1.6f, c);
      break;
    case I_UNDO: case I_REDO: {
      float sg = id == I_UNDO ? 1.f : -1.f;
      arc(cx, cy + s * 0.05f, s * 0.28f, t * 1.2f, id == I_UNDO ? -3.14f : -1.2f, id == I_UNDO ? 1.2f : 3.14f, c);
      line(cx - sg * s * 0.28f, cy + s * 0.05f, cx - sg * s * 0.42f, cy - s * 0.12f, t * 1.2f, c);
      line(cx - sg * s * 0.28f, cy + s * 0.05f, cx - sg * s * 0.1f, cy - s * 0.08f, t * 1.2f, c);
      break;
    }
    case I_ROTATE:
      arc(cx, cy, s * 0.3f, t * 1.2f, -2.6f, 2.0f, c);
      line(cx + s * 0.13f, cy + s * 0.27f, cx + s * 0.32f, cy + s * 0.4f, t * 1.2f, c);
      break;
    case I_MIRROR:
      line(cx, cy - s * 0.42f, cx, cy + s * 0.42f, t * 0.8f, c.alpha(0.6f));
      line(cx - s * 0.12f, cy - s * 0.25f, cx - s * 0.4f, cy + s * 0.25f, t * 1.3f, c);
      line(cx + s * 0.12f, cy - s * 0.25f, cx + s * 0.4f, cy + s * 0.25f, t * 1.3f, c);
      break;
    case I_TRASH:
      rect(cx - s * 0.25f, cy - s * 0.2f, s * 0.5f, s * 0.6f, c, s * 0.06f);
      rect(cx - s * 0.36f, cy - s * 0.34f, s * 0.72f, s * 0.1f, c, s * 0.04f);
      break;
    case I_SAVE:
      rect(cx - s * 0.36f, cy - s * 0.36f, s * 0.72f, s * 0.72f, c, s * 0.08f);
      rect(cx - s * 0.2f, cy - s * 0.36f, s * 0.4f, s * 0.24f, Color(0, 0, 0, 0.5f));
      break;
    case I_EYEDROP:
      line(cx - s * 0.35f, cy + s * 0.35f, cx + s * 0.15f, cy - s * 0.15f, t * 1.3f, c);
      circle(cx + s * 0.22f, cy - s * 0.22f, s * 0.15f, c);
      break;
    case I_ERASE:
      line(cx - s * 0.3f, cy + s * 0.15f, cx + s * 0.15f, cy - s * 0.3f, s * 0.3f, c);
      line(cx - s * 0.42f, cy + s * 0.38f, cx + s * 0.4f, cy + s * 0.38f, t, c.alpha(0.6f));
      break;
    case I_PAINT:
      rect(cx - s * 0.3f, cy - s * 0.4f, s * 0.6f, s * 0.3f, c, s * 0.06f);
      line(cx, cy - s * 0.1f, cx, cy + s * 0.4f, t * 1.4f, c);
      break;
    case I_GEAR:
      ring(cx, cy, s * 0.24f, t * 1.6f, c);
      for (int i = 0; i < 8; i++) {
        float a = i * PI / 4;
        line(cx + std::cos(a) * s * 0.3f, cy + std::sin(a) * s * 0.3f, cx + std::cos(a) * s * 0.42f, cy + std::sin(a) * s * 0.42f, t * 1.6f, c);
      }
      break;
    case I_CLOSE:
      line(cx - s * 0.3f, cy - s * 0.3f, cx + s * 0.3f, cy + s * 0.3f, t * 1.3f, c);
      line(cx + s * 0.3f, cy - s * 0.3f, cx - s * 0.3f, cy + s * 0.3f, t * 1.3f, c);
      break;
    case I_BOLT:
      line(cx + s * 0.12f, cy - s * 0.45f, cx - s * 0.15f, cy + s * 0.05f, t * 1.5f, c);
      line(cx - s * 0.15f, cy + s * 0.05f, cx + s * 0.15f, cy - s * 0.05f, t * 1.5f, c);
      line(cx + s * 0.15f, cy - s * 0.05f, cx - s * 0.12f, cy + s * 0.45f, t * 1.5f, c);
      break;
    default: {
      // Part icons 1..20: a coloured chip with a glyph.
      rect(cx - s * 0.4f, cy - s * 0.4f, s * 0.8f, s * 0.8f, c.alpha(0.25f), s * 0.15f);
      static const char* glyphs[] = {"?", "B", "M", "W", "P", "S", "L", "H", "PV", "C", "R", "#", "W", "W", "W", "W"};
      const char* g = id >= 0 && id < 16 ? glyphs[id] : "?";
      text(cx, cy - s * 0.33f, g, s * 0.62f, c, 1, 0.05f);
    }
  }
}

}  // namespace ui
