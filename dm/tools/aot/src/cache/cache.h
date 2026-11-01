// The per-function cache of the pack stage (docs/design/aot-recompiler.md section 13): a directory of compiled objects, one per compile unit
// (a group of neighbouring guest functions; at tiers H and H+ a shard of them; a part of a split function), each under the SHA-256 of
// everything its code depends on: its functions' keys (src/build/pack.cpp, UnitKey, lists those inputs). The module's analysis is kept beside
// them (src/build/analysis_cache.h). A pack takes a unit's object from here instead of lifting and compiling it when its key is there; a key that is not there
// is compiled and stored. Entries are written to a temporary name and renamed, the object before its facts, so a pack that stops half-way leaves
// no entry that looks whole, and two packs may share one cache. For a game the directory is <NSP folder>/<id>-work/aot-cache/ (AGENTS.md: work
// files stay in the NSP folder).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/Support/SHA256.h>

namespace nxaot::cache {

// A key under construction: fields appended with their names and lengths, so that no two different field lists hash alike
class KeyBuilder {
public:
    KeyBuilder& Add(std::string_view name, std::string_view value);
    KeyBuilder& Add(std::string_view name, uint64_t value);
    std::string Hex();  // the SHA-256 of what was added, 64 hex digits (ends the key)

private:
    llvm::SHA256 sha;
};

// What a cached unit carries beside its object: the counts a pack prints for it (blocks, sites, text bytes, ...) and its notes ([RED] lines)
struct Facts {
    std::map<std::string, uint64_t> counts;
    std::vector<std::string> notes;
};

class Cache {
public:
    explicit Cache(std::string dir) : dir_(std::move(dir)) {}
    const std::string& Dir() const { return dir_; }
    // Creates the directory (and its parents). False with *error set when it cannot
    bool Open(std::string* error);
    // The object file of a key, whether or not it exists
    std::string ObjectPath(const std::string& key) const;
    // True when the key has a whole entry: *facts then holds its facts and *bytes the object's size
    bool Lookup(const std::string& key, Facts* facts, uint64_t* bytes) const;
    // Stores an entry (the object first, then its facts, each through a temporary name). False with *error set on failure
    bool Store(const std::string& key, const std::string& object, const Facts& facts, std::string* error) const;

private:
    std::string dir_;
};

// The SHA-256 of a file's contents as 64 hex digits; empty when it cannot be read (the tool's own executable, part of every key)
std::string FileSha256(const std::string& path);

// Writes data to path through a unique temporary name in the same directory, then renames it: readers see the whole file or none. False with
// *error set on failure
bool WriteAtomic(const std::string& path, const std::string& data, std::string* error);

}  // namespace nxaot::cache
