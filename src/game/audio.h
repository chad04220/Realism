#pragma once
// Procedurally synthesised sound: impacts voiced by material, shattering
// glass, explosions, fire, motors, water and tools, positioned in stereo.
// Windows uses waveOut on a mixer thread; other platforms are silent.
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include "../core/math.h"

enum Sfx {
  SFX_METAL, SFX_WOOD, SFX_STONE, SFX_GLASS, SFX_SOFT, SFX_BOOM, SFX_SWING, SFX_CLICK, SFX_POP, SFX_SIZZLE,
  SFX_UI, SFX_THUD, SFX_COUNT
};
enum Loop { L_FIRE, L_MOTOR, L_WATER, L_TORCH, L_COUNT };

class Audio {
 public:
  bool init(float volume);
  void shutdown();
  void play(Sfx s, const vec3& pos, float gain = 1, float pitch = 1);
  void playUi(Sfx s, float gain = 1, float pitch = 1);
  void setLoop(Loop l, float gain, const vec3& pos);
  void setListener(const vec3& pos, const vec3& right) { lpos_ = pos; lright_ = right; }
  void setVolume(float v) { volume_ = v; }
  void mix(int16_t* out, int frames);   // called from the audio thread

 private:
  struct Voice { int sfx; float pos; float rate; float gl, gr; };
  std::vector<float> bank_[SFX_COUNT];
  std::vector<float> loops_[L_COUNT];
  float loopPos_[L_COUNT] = {};
  std::atomic<float> loopGainL_[L_COUNT], loopGainR_[L_COUNT];
  float loopCurL_[L_COUNT] = {}, loopCurR_[L_COUNT] = {};
  std::vector<Voice> voices_;
  std::mutex mu_;
  vec3 lpos_, lright_{1, 0, 0};
  std::atomic<float> volume_{0.8f};
  std::thread thread_;
  std::atomic<bool> running_{false};
  void* device_ = nullptr;
  void synth();
  void pan(const vec3& p, float gain, float& l, float& r) const;
};
