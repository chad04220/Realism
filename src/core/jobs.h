#pragma once
// A small fork-join thread pool. parallelFor splits [0,n) into chunks that the
// worker threads and the calling thread pull from until all are done.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class Jobs {
 public:
  static Jobs& get() { static Jobs j; return j; }
  ~Jobs() { stop(); }
  void start(int threads);
  void stop();
  int threads() const { return (int)workers_.size() + 1; }
  // fn(begin, end, threadIndex)
  void parallelFor(int n, int grain, const std::function<void(int, int, int)>& fn);

 private:
  void worker(int index);
  bool runChunk(int threadIndex);
  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::condition_variable cv_, doneCv_;
  const std::function<void(int, int, int)>* fn_ = nullptr;
  std::atomic<int> next_{0};
  int n_ = 0, grain_ = 1;
  std::atomic<int> active_{0};
  uint64_t gen_ = 0;
  bool quit_ = false;
};
