#include "core/parallel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace eu {

namespace {

// A container is often given fewer CPUs than the host has, and oversubscribing a
// CFS quota throttles the whole process (six threads on a two-CPU quota ran slower
// than two). Prefer the quota; fall back to the core count.
int quota_threads() {
    for (const char* path : {"/sys/fs/cgroup/cpu.max", "/sys/fs/cgroup/cpu/cpu.cfs_quota_us"}) {
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        char a[64] = {0}, b[64] = {0};
        const size_t got = std::fread(a, 1, sizeof a - 1, f);
        (void)got;
        std::fclose(f);
        if (a[0] == 'm') return 0;                    // "max"
        long long quota = std::atoll(a);
        long long period = 100000;
        if (const char* sp = std::strchr(a, ' ')) period = std::atoll(sp + 1);
        else if (std::strstr(path, "quota_us")) {
            FILE* g = std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "rb");
            if (g) {
                if (std::fread(b, 1, sizeof b - 1, g)) period = std::atoll(b);
                std::fclose(g);
            }
        }
        if (quota <= 0 || period <= 0) return 0;      // unlimited
        const long long n = quota / period;
        return n > 0 ? (int)std::min<long long>(n, 64) : 1;
    }
    return 0;
}

int default_threads() {
    if (const char* e = std::getenv("EU_THREADS")) {
        const int v = std::atoi(e);
        if (v > 0) return std::min(v, 64);
    }
    const unsigned hc = std::thread::hardware_concurrency();
    const int hw = hc ? (int)std::min(hc, 16u) : 1;
    const int q = quota_threads();
    return q > 0 ? std::min(hw, q) : hw;
}

}  // namespace

Pool& Pool::get() {
    static Pool p;
    return p;
}

Pool::Pool() {
    const int n = default_threads();
    th_.reserve((size_t)std::max(0, n - 1));
    for (int i = 1; i < n; ++i) th_.emplace_back([this] { worker(); });
}

Pool::~Pool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : th_)
        if (t.joinable()) t.join();
}

void Pool::worker() {
    unsigned seen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
            if (stop_) return;
            seen = gen_;
        }
        // fn_ and the item claim are read under one lock: a late worker must never
        // apply the previous generation's task to the next generation's index.
        for (;;) {
            const std::function<void(int)>* f;
            int i;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (!fn_ || next_ >= n_) break;
                i = next_++;
                f = fn_;
            }
            (*f)(i);
            if (--left_ == 0) {
                std::lock_guard<std::mutex> lk(m_);
                done_.notify_all();
            }
        }
    }
}

void Pool::run(int n, const std::function<void(int)>& fn) {
    std::lock_guard<std::mutex> entry(run_m_);
    {
        std::lock_guard<std::mutex> lk(m_);
        fn_ = &fn;
        n_ = n;
        next_ = 0;
        left_ = n;
        ++gen_;
    }
    cv_.notify_all();

    for (;;) {
        int i;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (next_ >= n_) break;
            i = next_++;
        }
        fn(i);
        if (--left_ == 0) {
            std::lock_guard<std::mutex> lk(m_);
            done_.notify_all();
        }
    }

    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [&] { return left_ == 0; });
    fn_ = nullptr;
}

}  // namespace eu
