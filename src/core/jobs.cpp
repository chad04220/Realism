#include "jobs.h"

void Jobs::start(int threads) {
  stop();
  quit_ = false;
  for (int i = 1; i < threads; i++) workers_.emplace_back([this, i] { worker(i); });
}

void Jobs::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_) t.join();
  workers_.clear();
}

bool Jobs::runChunk(int ti) {
  int b = next_.fetch_add(grain_);
  if (b >= n_) return false;
  int e = b + grain_ < n_ ? b + grain_ : n_;
  (*fn_)(b, e, ti);
  return true;
}

void Jobs::worker(int index) {
  uint64_t seen = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return quit_ || gen_ != seen; });
      if (quit_) return;
      seen = gen_;
      active_++;
    }
    while (runChunk(index)) {}
    {
      std::lock_guard<std::mutex> lk(mu_);
      active_--;
    }
    doneCv_.notify_all();
  }
}

void Jobs::parallelFor(int n, int grain, const std::function<void(int, int, int)>& fn) {
  if (n <= 0) return;
  if (grain < 1) grain = 1;
  if (workers_.empty() || n <= grain) { fn(0, n, 0); return; }
  {
    std::lock_guard<std::mutex> lk(mu_);
    fn_ = &fn;
    n_ = n;
    grain_ = grain;
    next_ = 0;
    gen_++;
  }
  cv_.notify_all();
  while (runChunk(0)) {}
  std::unique_lock<std::mutex> lk(mu_);
  doneCv_.wait(lk, [&] { return active_ == 0 && next_ >= n_; });
  fn_ = nullptr;
}
