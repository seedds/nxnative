// The analysis's threads (README.md, "Phase 5c"): discovery's walks, jump tables, summaries, windows and scans (discover.cpp, summary.cpp) and
// the plan's fixed points (src/build/plan.cpp) run their independent items on one pool. For(n, body) runs body(i) for every i in [0, n) on the
// pool's threads and the calling thread, taking indices from an atomic counter, and returns when every one is done. Which thread runs an index
// changes from run to run, never what it computes: every step that uses the pool writes only its own items' results (or per-index slots) and
// merges them in index order afterwards, so the analysis is the same with any number of threads. Header-only: one pool per discovery.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nxaot::analysis {

class Pool {
public:
    // threads: the threads that run items, the calling thread included; 0: one per CPU
    explicit Pool(unsigned threads) {
        if (!threads) {
            threads = std::thread::hardware_concurrency();
        }
        count = std::max(1u, threads);
        for (unsigned i = 1; i < count; ++i) {
            workers.emplace_back([this] { Loop(); });
        }
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        for (auto& t : workers) {
            t.join();
        }
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    unsigned Threads() const { return count; }

    // body(i) for every i in [0, n); fewer than `inline_below` items (or one thread) run on the calling thread alone, where waking the pool
    // would cost more than the items. A thread takes `chunk` neighbouring indices at a time (0: about a sixteenth of a thread's share, so
    // that cheap items do not contend for the counter and neighbouring items' results stay on one thread's cache lines; 1 for items whose
    // costs differ by orders of magnitude)
    void For(size_t n, const std::function<void(size_t)>& body, size_t inline_below = 2, size_t chunk = 0) {
        if (n == 0) {
            return;
        }
        if (count == 1 || n < inline_below) {
            for (size_t i = 0; i < n; ++i) {
                body(i);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            job = &body;
            size = n;
            step = chunk ? chunk : std::max<size_t>(1, n / (16 * count));
            next.store(0, std::memory_order_relaxed);
            busy = static_cast<unsigned>(workers.size());
            ++generation;
        }
        wake.notify_all();
        Work(body, n, step);
        std::unique_lock<std::mutex> lock(mutex);
        done.wait(lock, [this] { return busy == 0; });
        job = nullptr;
    }

private:
    void Work(const std::function<void(size_t)>& body, size_t n, size_t chunk) {
        for (size_t b; (b = next.fetch_add(chunk, std::memory_order_relaxed)) < n;) {
            for (size_t i = b; i < std::min(n, b + chunk); ++i) {
                body(i);
            }
        }
    }
    void Loop() {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(size_t)>* body;
            size_t n, chunk;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stop || generation != seen; });
                if (stop) {
                    return;
                }
                seen = generation;
                body = job;
                n = size;
                chunk = step;
            }
            Work(*body, n, chunk);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (--busy == 0) {
                    done.notify_one();
                }
            }
        }
    }

    unsigned count = 1;
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable wake, done;
    const std::function<void(size_t)>* job = nullptr;
    size_t size = 0, step = 1;
    std::atomic<size_t> next{0};
    unsigned busy = 0;
    uint64_t generation = 0;
    bool stop = false;
};

}  // namespace nxaot::analysis
