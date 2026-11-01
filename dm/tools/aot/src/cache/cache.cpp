#include "cache/cache.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>

#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Process.h>

namespace nxaot::cache {

namespace {

// Writes data to path through a unique temporary name in the same directory, then renames it: readers see the whole file or none
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

}  // namespace

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
