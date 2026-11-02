// The memory of the analysis's and the pack stage's tables (README.md, "Memory at scale"): `nxaot discover | plan | build --memory` notes each
// table's bytes at the points where it may be at its largest (the end of each discovery round, of each run of the summaries, of the plan's
// passes, of the pack's stages) and prints every table's peak, where it was reached, and the process's resident set at each stage. The bytes are
// estimated from the containers' sizes and capacities with glibc malloc's chunk overhead (Chunk), not counted by an allocator: cheap enough to
// keep in the tool, off unless --memory asks for it, and close to the resident set (the README compares their sums with it).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "analysis/flat.h"

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace nxaot::memory {

// What malloc takes for a request of n bytes: glibc's chunks, an 8-byte header in 16-byte granules, 32 bytes at least
inline uint64_t Chunk(uint64_t n) {
    return n ? std::max<uint64_t>(32, (n + 8 + 15) & ~uint64_t{15}) : 0;
}
// A node of std::set / std::map (libstdc++'s red-black tree: the colour and three pointers, then the value)
template <class V>
constexpr uint64_t TreeNode() {
    return (std::max<uint64_t>(32, (32 + sizeof(V) + 8 + 15) & ~uint64_t{15}));
}
template <class T, class A>
uint64_t Of(const std::vector<T, A>& v) {
    return Chunk(v.capacity() * sizeof(T));
}
template <class K, class C, class A>
uint64_t Of(const std::set<K, C, A>& s) {
    return s.size() * TreeNode<K>();
}
template <class K, class V, class C, class A>
uint64_t Of(const std::map<K, V, C, A>& m) {
    return m.size() * TreeNode<std::pair<const K, V>>();
}
// libstdc++'s hash tables: a node holds the next pointer and the value (the hash of an integer key is not cached), and the bucket array
template <class K, class V, class H, class E, class A>
uint64_t Of(const std::unordered_map<K, V, H, E, A>& m) {
    return m.size() * Chunk(8 + sizeof(std::pair<const K, V>)) + Chunk(m.bucket_count() * 8);
}
template <class K, class H, class E, class A>
uint64_t Of(const std::unordered_set<K, H, E, A>& s) {
    return s.size() * Chunk(8 + sizeof(K)) + Chunk(s.bucket_count() * 8);
}
// flat.h's sorted vectors: the elements' allocation, and a sparse one's vector besides while it holds one
template <class E, bool S>
uint64_t Of(const analysis::FlatStore<E, S>& st) {
    return Chunk(st.Bytes()) + (st.Held() ? Chunk(sizeof(std::vector<E>)) : 0);
}
template <class T, bool S>
uint64_t Of(const analysis::FlatSet<T, S>& s) {
    return Of(s.Store());
}
template <class K, class V, bool S>
uint64_t Of(const analysis::FlatMap<K, V, S>& m) {
    return Of(m.Store());
}
inline uint64_t Of(const std::string& s) {
    return s.capacity() > 15 ? Chunk(s.capacity() + 1) : 0;  // the short-string buffer is inside the object
}

// The process's resident set now and its peak so far, in bytes (0 where the system does not say)
inline uint64_t RssNow() {
#if defined(__linux__)
    std::ifstream in{"/proc/self/statm"};
    uint64_t pages = 0, resident = 0;
    if (in >> pages >> resident) return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#endif
    return 0;
}
inline uint64_t RssPeak() {
#if defined(__linux__)
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<uint64_t>(ru.ru_maxrss) << 10;
#else
    return 0;
#endif
}

class Registry {
public:
    static Registry& Get() {
        static Registry r;
        return r;
    }
    bool On() const { return on; }
    void Enable() { on = true; }
    // The table's bytes now; its peak and where it was reached are kept
    void Note(const std::string& table, uint64_t bytes, const std::string& at) {
        if (!on) return;
        std::lock_guard<std::mutex> lock(mutex);
        Table& t = tables[table];
        if (bytes >= t.peak) {
            t.peak = bytes;
            t.at = at;
        }
    }
    // The resident set now, printed at once (a run killed for its memory still shows how far it got), and kept for the report
    void Rss(const std::string& at) {
        if (!on) return;
        const uint64_t now = RssNow(), peak = RssPeak(), files = RssFiles();
        std::lock_guard<std::mutex> lock(mutex);
        std::fprintf(stderr, "aot: memory: resident %.1f MiB (files %.1f), peak so far %.1f MiB, at %s\n", Mib(now), Mib(files), Mib(peak),
                     at.c_str());
        std::fflush(stderr);
    }
    // Of the resident set, the pages of mapped files and shared memory (RssFile and RssShmem: the link's inputs and output, which lld maps;
    // on a disk the kernel may drop or write them back, on tmpfs they are memory), in bytes
    static uint64_t RssFiles() {
#if defined(__linux__)
        std::ifstream in{"/proc/self/status"};
        std::string key;
        uint64_t kib = 0, sum = 0;
        std::string unit;
        while (in >> key) {
            if (key == "RssFile:" || key == "RssShmem:") {
                in >> kib >> unit;
                sum += kib << 10;
            } else {
                std::getline(in, unit);
            }
        }
        return sum;
#else
        return 0;
#endif
    }
    // Every table's peak, largest first, and the process's peak resident set
    void Print(std::FILE* f) const {
        if (!on) return;
        std::vector<std::pair<std::string, Table>> rows(tables.begin(), tables.end());
        std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.peak > b.second.peak; });
        for (const auto& [name, t] : rows) {
            std::fprintf(f, "aot: memory: %-44s peak %9.1f MiB at %s\n", name.c_str(), Mib(t.peak), t.at.c_str());
        }
        std::fprintf(f, "aot: memory: the process's peak resident set %.1f MiB\n", Mib(RssPeak()));
    }
    static double Mib(uint64_t bytes) { return static_cast<double>(bytes) / (1 << 20); }

private:
    struct Table {
        uint64_t peak = 0;
        std::string at;
    };
    bool on = false;
    std::mutex mutex;
    std::map<std::string, Table> tables;
};

inline bool On() {
    return Registry::Get().On();
}
inline void Note(const std::string& table, uint64_t bytes, const std::string& at) {
    Registry::Get().Note(table, bytes, at);
}
inline void Rss(const std::string& at) {
    Registry::Get().Rss(at);
}

}  // namespace nxaot::memory
