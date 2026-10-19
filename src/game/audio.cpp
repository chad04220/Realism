#include "audio.h"
#include <cmath>
#include <cstring>
#include <functional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {
constexpr int RATE = 44100;

struct Noise {
  uint32_t s = 12345;
  float operator()() { s = s * 1664525u + 1013904223u; return ((s >> 9) & 0x7FFF) / 16384.f - 1.f; }
};

std::vector<float> make(float seconds, const std::function<float(float, int)>& f) {
  int n = (int)(seconds * RATE);
  std::vector<float> v(n);
  for (int i = 0; i < n; i++) v[i] = f((float)i / RATE, i);
  return v;
}
}  // namespace

void Audio::synth() {
  Noise nz;
  // Metal: inharmonic ringing partials.
  bank_[SFX_METAL] = make(1.2f, [&](float t, int) {
    float e = std::exp(-t * 5.f);
    return e * (0.5f * std::sin(2 * PI * 610 * t) + 0.3f * std::sin(2 * PI * 1580 * t) * std::exp(-t * 4) +
                0.2f * std::sin(2 * PI * 2770 * t) * std::exp(-t * 9)) + nz() * std::exp(-t * 60) * 0.4f;
  });
  // Wood: knock with a hollow body resonance.
  float lp = 0;
  bank_[SFX_WOOD] = make(0.35f, [&](float t, int) {
    lp += (nz() - lp) * 0.25f;
    return std::exp(-t * 22) * (0.7f * std::sin(2 * PI * 190 * t) + 0.3f * std::sin(2 * PI * 420 * t)) + lp * std::exp(-t * 40) * 0.8f;
  });
  // Stone/brick: sharp crack and gritty tail.
  lp = 0;
  bank_[SFX_STONE] = make(0.5f, [&](float t, int) {
    lp += (nz() - lp) * 0.5f;
    return lp * (std::exp(-t * 30) * 0.9f + std::exp(-t * 7) * 0.15f) + std::sin(2 * PI * 140 * t) * std::exp(-t * 25) * 0.4f;
  });
  // Glass: bright noise burst plus scattered tinkles.
  bank_[SFX_GLASS] = make(1.4f, [&](float t, int i) {
    float s = nz() * std::exp(-t * 18) * 0.6f;
    for (int k = 0; k < 7; k++) {
      float t0 = 0.03f + k * 0.11f + (k * 37 % 11) * 0.01f;
      if (t > t0) s += std::sin(2 * PI * (2400 + k * 530) * (t - t0)) * std::exp(-(t - t0) * 25) * 0.25f;
    }
    (void)i;
    return s;
  });
  lp = 0;
  bank_[SFX_SOFT] = make(0.25f, [&](float t, int) { lp += (nz() - lp) * 0.08f; return lp * std::exp(-t * 25) * 2.f; });
  // Explosion: low rumbling boom.
  float l1 = 0, l2 = 0;
  bank_[SFX_BOOM] = make(3.0f, [&](float t, int) {
    l1 += (nz() - l1) * 0.04f;
    l2 += (l1 - l2) * 0.05f;
    float env = t < 0.01f ? t / 0.01f : std::exp(-t * 1.6f);
    return (l2 * 6.f + std::sin(2 * PI * 48 * t) * std::exp(-t * 3) * 0.6f + nz() * std::exp(-t * 14) * 0.3f) * env;
  });
  lp = 0;
  bank_[SFX_SWING] = make(0.3f, [&](float t, int) { lp += (nz() - lp) * 0.15f; return lp * std::sin(PI * t / 0.3f) * 0.5f; });
  bank_[SFX_CLICK] = make(0.08f, [&](float t, int) { return (std::sin(2 * PI * 2200 * t) * 0.5f + nz() * 0.3f) * std::exp(-t * 90); });
  bank_[SFX_POP] = make(0.25f, [&](float t, int) {
    return std::sin(2 * PI * (300 - 600 * t) * t) * std::exp(-t * 18) * 0.8f + nz() * std::exp(-t * 50) * 0.3f;
  });
  lp = 0;
  bank_[SFX_SIZZLE] = make(1.0f, [&](float t, int) { float n = nz(); lp += (n - lp) * 0.6f; return (n - lp) * std::exp(-t * 3) * 0.5f; });
  bank_[SFX_UI] = make(0.06f, [&](float t, int) { return std::sin(2 * PI * 1300 * t) * std::exp(-t * 70) * 0.35f; });
  lp = 0;
  bank_[SFX_THUD] = make(0.4f, [&](float t, int) { lp += (nz() - lp) * 0.05f; return (lp * 3 + std::sin(2 * PI * 70 * t) * 0.5f) * std::exp(-t * 12); });

  // Loops (2 s, seamless enough with noise).
  lp = 0;
  loops_[L_FIRE] = make(2.0f, [&](float t, int) {
    lp += (nz() - lp) * 0.12f;
    float crack = (nz() > 0.995f) ? nz() * 1.5f : 0;
    return lp * 0.35f + crack;
  });
  loops_[L_MOTOR] = make(1.0f, [&](float t, int) {
    float ph = std::fmod(t * 120.f, 1.f);
    return (ph * 2 - 1) * 0.15f + std::sin(2 * PI * 240 * t) * 0.15f + nz() * 0.03f;
  });
  float hp = 0;
  loops_[L_WATER] = make(2.0f, [&](float t, int) { float n = nz(); hp += (n - hp) * 0.3f; return (n - hp) * 0.4f; });
  lp = 0;
  loops_[L_TORCH] = make(2.0f, [&](float t, int) { lp += (nz() - lp) * 0.3f; return lp * 0.6f; });
}

void Audio::pan(const vec3& p, float gain, float& l, float& r) const {
  vec3 d = p - lpos_;
  float dist = length(d);
  float att = gain / (1.f + dist * dist * 0.08f);
  float side = dist > 1e-3f ? dot(d / dist, lright_) : 0;
  l = att * std::sqrt(clampf(0.5f - side * 0.45f, 0, 1));
  r = att * std::sqrt(clampf(0.5f + side * 0.45f, 0, 1));
}

void Audio::play(Sfx s, const vec3& p, float gain, float pitch) {
  if (!running_) return;
  float l, r;
  pan(p, gain, l, r);
  if (l + r < 0.002f) return;
  std::lock_guard<std::mutex> lk(mu_);
  if (voices_.size() > 48) voices_.erase(voices_.begin());
  voices_.push_back({s, 0, pitch, l, r});
}

void Audio::playUi(Sfx s, float gain, float pitch) {
  if (!running_) return;
  std::lock_guard<std::mutex> lk(mu_);
  voices_.push_back({s, 0, pitch, gain * 0.7f, gain * 0.7f});
}

void Audio::setLoop(Loop lp, float gain, const vec3& p) {
  float l, r;
  pan(p, gain, l, r);
  loopGainL_[lp] = l;
  loopGainR_[lp] = r;
}

void Audio::mix(int16_t* out, int frames) {
  std::vector<float> buf(frames * 2, 0.f);
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < voices_.size();) {
      Voice& v = voices_[i];
      const auto& b = bank_[v.sfx];
      for (int f = 0; f < frames; f++) {
        int idx = (int)v.pos;
        if (idx + 1 >= (int)b.size()) { v.pos = 1e9f; break; }
        float fr = v.pos - idx;
        float s = b[idx] * (1 - fr) + b[idx + 1] * fr;
        buf[f * 2] += s * v.gl;
        buf[f * 2 + 1] += s * v.gr;
        v.pos += v.rate;
      }
      if (v.pos >= b.size() - 1) { voices_[i] = voices_.back(); voices_.pop_back(); } else i++;
    }
  }
  for (int l = 0; l < L_COUNT; l++) {
    const auto& b = loops_[l];
    if (b.empty()) continue;
    float tl = loopGainL_[l], tr = loopGainR_[l];
    for (int f = 0; f < frames; f++) {
      loopCurL_[l] += (tl - loopCurL_[l]) * 0.0005f;
      loopCurR_[l] += (tr - loopCurR_[l]) * 0.0005f;
      if (loopCurL_[l] + loopCurR_[l] < 1e-4f) { loopPos_[l] += 1; continue; }
      float s = b[(size_t)loopPos_[l] % b.size()];
      buf[f * 2] += s * loopCurL_[l];
      buf[f * 2 + 1] += s * loopCurR_[l];
      loopPos_[l] += 1;
    }
    loopPos_[l] = std::fmod(loopPos_[l], (float)b.size());
  }
  float vol = volume_;
  for (int i = 0; i < frames * 2; i++) {
    float s = std::tanh(buf[i] * vol);  // soft limiter
    out[i] = (int16_t)(s * 32000);
  }
}

#ifdef _WIN32
bool Audio::init(float volume) {
  volume_ = volume;
  for (int l = 0; l < L_COUNT; l++) { loopGainL_[l] = 0; loopGainR_[l] = 0; }
  synth();
  WAVEFORMATEX wf = {};
  wf.wFormatTag = WAVE_FORMAT_PCM;
  wf.nChannels = 2;
  wf.nSamplesPerSec = RATE;
  wf.wBitsPerSample = 16;
  wf.nBlockAlign = 4;
  wf.nAvgBytesPerSec = RATE * 4;
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HWAVEOUT h = nullptr;
  if (waveOutOpen(&h, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) return false;
  device_ = h;
  running_ = true;
  thread_ = std::thread([this, h, ev] {
    const int N = 4, FR = 1024;
    std::vector<std::vector<int16_t>> bufs(N, std::vector<int16_t>(FR * 2));
    std::vector<WAVEHDR> hdr(N);
    for (int i = 0; i < N; i++) {
      hdr[i] = {};
      hdr[i].lpData = (LPSTR)bufs[i].data();
      hdr[i].dwBufferLength = FR * 4;
      waveOutPrepareHeader(h, &hdr[i], sizeof(WAVEHDR));
      mix(bufs[i].data(), FR);
      waveOutWrite(h, &hdr[i], sizeof(WAVEHDR));
    }
    while (running_) {
      WaitForSingleObject(ev, 50);
      for (int i = 0; i < N; i++) {
        if (hdr[i].dwFlags & WHDR_DONE) {
          mix(bufs[i].data(), FR);
          hdr[i].dwFlags &= ~WHDR_DONE;
          waveOutWrite(h, &hdr[i], sizeof(WAVEHDR));
        }
      }
    }
    waveOutReset(h);
    for (int i = 0; i < N; i++) waveOutUnprepareHeader(h, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(h);
    CloseHandle(ev);
  });
  return true;
}
void Audio::shutdown() {
  if (!running_) return;
  running_ = false;
  if (thread_.joinable()) thread_.join();
}
#else
bool Audio::init(float volume) {
  volume_ = volume;
  for (int l = 0; l < L_COUNT; l++) { loopGainL_[l] = 0; loopGainR_[l] = 0; }
  return false;  // silent on this platform
}
void Audio::shutdown() {}
#endif
