#include "cache/cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Process.h>

namespace nxaot::cache {

bool WriteAtomic(const std::string& path, const std::string& data, std::string* error) {
    static std::atomic<uint64_t> serial{0};
    const std::string tmp = path + ".tmp." + std::to_string(llvm::sys::Process::getProcessId()) + "." + std::to_string(serial++);
    {
        std::ofstream out{tmp, std::ios::binary};
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            *error = "cannot write " + tmp;
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (const std::error_code ec = llvm::sys::fs::rename(tmp, path)) {
        *error = "cannot rename " + tmp + " to " + path + ": " + ec.message();
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

KeyBuilder& KeyBuilder::Add(std::string_view name, std::string_view value) {
    const std::string head = std::string(name) + "=" + std::to_string(value.size()) + ":";
    sha.update(llvm::StringRef(head));
    sha.update(llvm::StringRef(value.data(), value.size()));
    sha.update(llvm::StringRef("\n"));
    return *this;
}

KeyBuilder& KeyBuilder::Add(std::string_view name, uint64_t value) {
    return Add(name, std::to_string(value));
}

std::string KeyBuilder::Hex() {
    const std::array<uint8_t, 32> d = sha.final();
    return llvm::toHex(llvm::ArrayRef<uint8_t>(d.data(), d.size()), true);
}

bool Cache::Open(std::string* error) {
    if (const std::error_code ec = llvm::sys::fs::create_directories(dir_)) {
        *error = "cannot create the cache directory " + dir_ + ": " + ec.message();
        return false;
    }
    return true;
}

std::string Cache::ObjectPath(const std::string& key) const {
    return dir_ + "/" + key.substr(0, 2) + "/" + key + ".o";
}

bool Cache::Lookup(const std::string& key, Facts* facts, uint64_t* bytes) const {
    const std::string object = ObjectPath(key);
    std::ifstream in{object.substr(0, object.size() - 2) + ".tsv", std::ios::binary};
    if (!in) {
        return false;  // the facts are stored last: an object without them is an entry still being written
    }
    uint64_t size = 0;
    if (llvm::sys::fs::file_size(object, size)) {
        return false;
    }
    facts->counts.clear();
    facts->notes.clear();
    std::string line;
    while (std::getline(in, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        if (line.compare(0, tab, "note") == 0) {
            facts->notes.push_back(line.substr(tab + 1));
        } else {
            facts->counts[line.substr(0, tab)] = std::strtoull(line.c_str() + tab + 1, nullptr, 10);
        }
    }
    *bytes = size;
    Touch(object.substr(0, object.size() - 2) + ".tsv");  // used now (Prune)
    return true;
}

bool Cache::Store(const std::string& key, const std::string& object, const Facts& facts, std::string* error) const {
    const std::string path = ObjectPath(key);
    if (const std::error_code ec = llvm::sys::fs::create_directories(llvm::sys::path::parent_path(path))) {
        *error = "cannot create " + std::string(llvm::sys::path::parent_path(path)) + ": " + ec.message();
        return false;
    }
    std::string tsv;
    for (const auto& [name, value] : facts.counts) {
        tsv += name + "\t" + std::to_string(value) + "\n";
    }
    for (const auto& n : facts.notes) {
        tsv += "note\t" + n + "\n";  // a note is one line (the pack's notes are)
    }
    return WriteAtomic(path, object, error) && WriteAtomic(path.substr(0, path.size() - 2) + ".tsv", tsv, error);
}

void Touch(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    const auto at = fs::last_write_time(path, ec);
    if (ec || now - at < std::chrono::hours(1)) {
        return;  // used within the hour: a warm pack writes nothing
    }
    fs::last_write_time(path, now, ec);  // a cache on a read-only medium stays as it is
}

static std::string ToolMark(const std::string& dir, const std::string& tool) {
    return dir + "/tools/" + KeyBuilder().Add("tool", tool).Hex().substr(0, 16);
}

bool MarkTool(const std::string& dir, const std::string& tool, std::string* error) {
    namespace fs = std::filesystem;
    const std::string mark = ToolMark(dir, tool);
    std::error_code ec;
    if (fs::exists(mark, ec)) {
        return true;  // its time is the first use: never written again
    }
    if (const std::error_code e = llvm::sys::fs::create_directories(dir + "/tools")) {
        *error = "cannot create " + dir + "/tools: " + e.message();
        return false;
    }
    return WriteAtomic(mark, tool + "\n", error);
}

bool Prune(const std::string& dir, const std::string& tool, uint64_t unused_seconds, PruneStats* stats, std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return true;
    }
    // The tool's first use of this cache: every entry it can reach is younger. A tool without a mark (it never packed into this cache) gets one now
    // and removes nothing
    const std::string mark = ToolMark(dir, tool);
    const auto first_use = fs::last_write_time(mark, ec);
    if (ec) {
        stats->first_use = true;
        return MarkTool(dir, tool, error);
    }
    const auto cutoff = std::min(first_use, fs::file_time_type::clock::now() - std::chrono::seconds(unused_seconds));
    const auto hex = [](const std::string& s, size_t n) {
        return s.size() == n && s.find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    // The files first, then the removals: nothing is removed under a directory iterator
    std::vector<fs::path> facts, objects, others;
    std::set<std::string> have_facts;
    for (fs::directory_iterator top{dir, ec}, end; !ec && top != end; top.increment(ec)) {
        std::error_code e;
        if (!top->is_directory(e)) continue;
        const std::string folder = top->path().filename().string();
        const bool shard = hex(folder, 2), analysis = folder == "analysis";
        if (!shard && !analysis) continue;
        for (fs::directory_iterator it{top->path(), e}; !e && it != end; it.increment(e)) {
            std::error_code f;
            if (!it->is_regular_file(f)) continue;
            const fs::path& p = it->path();
            const std::string name = p.filename().string(), stem = p.stem().string(), ext = p.extension().string();
            if (name.find(".tmp.") != std::string::npos) {
                others.push_back(p);  // an interrupted pack's (WriteAtomic)
            } else if (shard && hex(stem, 64) && ext == ".tsv") {
                facts.push_back(p);
                have_facts.insert(p.string());
            } else if (shard && hex(stem, 64) && ext == ".o") {
                objects.push_back(p);
            } else if (analysis && ext == ".bin") {
                others.push_back(p);
            }
        }
    }
    if (ec) {
        *error = "cannot read the cache directory " + dir + ": " + ec.message();
        return false;
    }
    const auto old = [&](const fs::path& p) {
        std::error_code e;
        const auto at = fs::last_write_time(p, e);
        return !e && at < cutoff;
    };
    const auto drop = [&](const fs::path& p) {
        std::error_code e;
        const uint64_t n = fs::file_size(p, e);
        const uint64_t size = e ? 0 : n;
        if (fs::remove(p, e) && !e) {
            stats->bytes += size;
            return true;
        }
        return false;
    };
    std::set<std::string> gone;
    for (const fs::path& p : facts) {
        if (!old(p)) {
            stats->kept++;
            continue;
        }
        if (drop(p)) {  // the facts first: without them the entry is no entry (Lookup)
            gone.insert(p.string());
            stats->units++;
        }
    }
    for (const fs::path& p : objects) {
        fs::path tsv = p;
        tsv.replace_extension(".tsv");
        if (gone.count(tsv.string())) {
            drop(p);
        } else if (!have_facts.count(tsv.string()) && old(p) && drop(p)) {
            stats->partial++;  // an object whose facts never came
        }
    }
    for (const fs::path& p : others) {
        if (old(p) && drop(p)) {
            (p.extension() == ".bin" ? stats->analyses : stats->partial)++;
        }
    }
    return true;
}

std::string FileSha256(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return "";
    }
    llvm::SHA256 sha;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n > 0) sha.update(llvm::StringRef(buf.data(), static_cast<size_t>(n)));
    }
    const std::array<uint8_t, 32> d = sha.final();
    return llvm::toHex(llvm::ArrayRef<uint8_t>(d.data(), d.size()), true);
}

}  // namespace nxaot::cache
