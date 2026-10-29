#include "vec/vectors.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nxaot::vec {

namespace {

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    std::istringstream in{s};
    while (std::getline(in, cur, sep)) {
        parts.push_back(cur);
    }
    if (!s.empty() && s.back() == sep) {
        parts.emplace_back();
    }
    return parts;
}

bool Hex(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 16) {
        return false;
    }
    char* end = nullptr;
    out = std::strtoull(s.c_str(), &end, 16);
    return end && *end == '\0';
}

bool HexBytes(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) {
        return false;
    }
    for (size_t i = 0; i < s.size(); i += 2) {
        uint64_t b = 0;
        if (!Hex(s.substr(i, 2), b)) {
            return false;
        }
        out.push_back(static_cast<uint8_t>(b));
    }
    return true;
}

bool ParseLine(const std::string& line, Vector& v, std::string* error) {
    const auto f = Split(line, '\t');
    if (f.size() != 7) {
        *error = "expected 7 tab-separated fields, found " + std::to_string(f.size());
        return false;
    }
    v.id = f[0];
    v.cls = f[1];
    v.text = f[2];
    for (const auto& w : Split(f[3], ',')) {
        uint64_t word = 0;
        if (!Hex(w, word) || word > 0xFFFFFFFFu) {
            *error = "bad instruction word '" + w + "'";
            return false;
        }
        v.code.push_back(static_cast<uint32_t>(word));
    }
    if (v.code.empty() || v.code.size() > (kImageSize - kCodeOffset) / 4) {
        *error = "no code, or more than fits";
        return false;
    }
    if (!Hex(f[4], v.seed)) {
        *error = "bad seed";
        return false;
    }
    if (f[5] != "-") {
        for (const auto& r : Split(f[5], ',')) {
            const auto eq = r.find('=');
            uint64_t value = 0;
            if (eq == std::string::npos || !Hex(r.substr(eq + 1), value)) {
                *error = "bad register override '" + r + "'";
                return false;
            }
            v.regs.emplace_back(r.substr(0, eq), value);
        }
    }
    if (f[6] != "-") {
        for (const auto& m : Split(f[6], ',')) {
            const auto colon = m.find(':');
            uint64_t offset = 0;
            std::vector<uint8_t> bytes;
            if (colon == std::string::npos || !Hex(m.substr(0, colon), offset) || !HexBytes(m.substr(colon + 1), bytes) ||
                offset + bytes.size() > kImageSize) {
                *error = "bad memory override '" + m + "'";
                return false;
            }
            v.mem.emplace_back(offset, std::move(bytes));
        }
    }
    return true;
}

bool LoadFile(const std::string& path, std::vector<Vector>& out, std::string* error) {
    std::ifstream in{path};
    if (!in) {
        *error = "cannot read " + path;
        return false;
    }
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        Vector v;
        v.where = path + ":" + std::to_string(n);
        if (!ParseLine(line, v, error)) {
            *error = v.where + ": " + *error;
            return false;
        }
        out.push_back(std::move(v));
    }
    return true;
}

// splitmix64: the seed's stream (tests/gen_vectors.py does not need to reproduce it: it overrides what an instruction reads)
struct Stream {
    uint64_t state;
    uint64_t Next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
};

}  // namespace

bool LoadVectors(const std::string& path, std::vector<Vector>& out, std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        std::vector<std::string> files;
        for (const auto& e : fs::directory_iterator(path, ec)) {
            if (e.path().extension() == ".tsv") {
                files.push_back(e.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        if (files.empty()) {
            *error = "no .tsv files in " + path;
            return false;
        }
        for (const auto& f : files) {
            if (!LoadFile(f, out, error)) {
                return false;
            }
        }
        return true;
    }
    return LoadFile(path, out, error);
}

void SeedState(const Vector& v, State& s, std::vector<uint8_t>& image) {
    Stream r{v.seed};
    for (auto& reg : s.x) {
        reg = r.Next();
    }
    s.sp = (kGuestBase + 0x100 + r.Next() % (kImageSize - 0x200)) & ~uint64_t{15};
    s.pc = kCodeBase;
    s.nzcv = static_cast<uint32_t>(r.Next() & 0xF) << 28;
    s.fpcr = 0;
    s.fpsr = 0;
    s.tpidr = r.Next();
    s.tpidrro = r.Next();
    s.cnt = r.Next() >> 8;
    image.assign(kImageSize, 0);
    for (size_t i = 0; i < kImageSize; i += 8) {
        const uint64_t w = r.Next();
        for (size_t k = 0; k < 8; ++k) {
            image[i + k] = static_cast<uint8_t>(w >> (8 * k));
        }
    }
    for (size_t i = 0; i < v.code.size(); ++i) {
        for (size_t k = 0; k < 4; ++k) {
            image[kCodeOffset + 4 * i + k] = static_cast<uint8_t>(v.code[i] >> (8 * k));
        }
    }
    for (const auto& [name, value] : v.regs) {
        if (name.size() > 1 && name[0] == 'x' && std::isdigit(static_cast<unsigned char>(name[1]))) {
            const int n = std::atoi(name.c_str() + 1);
            if (n >= 0 && n < 31) {
                s.x[n] = value;
            }
        } else if (name == "sp") {
            s.sp = value;
        } else if (name == "nzcv") {
            s.nzcv = static_cast<uint32_t>(value);
        } else if (name == "fpcr") {
            s.fpcr = static_cast<uint32_t>(value);
        } else if (name == "fpsr") {
            s.fpsr = static_cast<uint32_t>(value);
        } else if (name == "tpidr") {
            s.tpidr = value;
        } else if (name == "tpidrro") {
            s.tpidrro = value;
        } else if (name == "cnt") {
            s.cnt = value;
        }
    }
    for (const auto& [offset, bytes] : v.mem) {
        std::copy(bytes.begin(), bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

bool InCode(const Vector& v, uint64_t pc) {
    return pc >= kCodeBase && pc < kCodeBase + 4 * v.code.size() && (pc & 3) == 0;
}

}  // namespace nxaot::vec
