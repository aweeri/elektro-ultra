// Fixed worker pool; parallel_for blocks until every item is done.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace eu {

class Pool {
public:
    static Pool& get();
    int size() const { return (int)th_.size() + 1; }

    // fn(i) for i in [0, n); the caller processes items too.
    void run(int n, const std::function<void(int)>& fn);

private:
    Pool();
    ~Pool();
    void worker();

    std::vector<std::thread> th_;
    // run() owns the pool for the duration, so two threads cannot interleave generations.
    std::mutex run_m_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(int)>* fn_ = nullptr;
    int n_ = 0;
    int next_ = 0;
    std::atomic<int> left_{0};
    unsigned gen_ = 0;
    bool stop_ = false;
};

inline int thread_count() { return Pool::get().size(); }

inline void parallel_for(int n, const std::function<void(int)>& fn) {
    if (n <= 0) return;
    if (n == 1 || thread_count() <= 1) {
        for (int i = 0; i < n; ++i) fn(i);
        return;
    }
    Pool::get().run(n, fn);
}

}  // namespace eu
