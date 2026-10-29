#include "vec/vectors.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <unistd.h>
#include <zlib.h>

namespace nxaot::vec {

namespace {

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t at = s.find(sep, start);
        if (at == std::string::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, at - start));
        start = at + 1;
    }
}

bool Hex(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 16) {
        return false;
    }
    char* end = nullptr;
    out = std::strtoull(s.c_str(), &end, 16);
    return end && *end == '\0';
}

bool Hex128(const std::string& s, V128& out) {
    if (s.empty() || s.size() > 32) {
        return false;
    }
    const size_t split = s.size() > 16 ? s.size() - 16 : 0;
    uint64_t hi = 0, lo = 0;
    if (split && !Hex(s.substr(0, split), hi)) {
        return false;
    }
    if (!Hex(s.substr(split), lo)) {
        return false;
    }
    out = {lo, hi};
    return true;
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

bool ParseKind(const std::string& s, FillKind& k) {
    static const std::pair<const char*, FillKind> kinds[] = {{"rnd", FillKind::Random}, {"f16", FillKind::F16}, {"f32", FillKind::F32},
                                                             {"f64", FillKind::F64},    {"i8", FillKind::I8},   {"i16", FillKind::I16},
                                                             {"i32", FillKind::I32},    {"i64", FillKind::I64}};
    for (const auto& [name, kind] : kinds) {
        if (s == name) {
            k = kind;
            return true;
        }
    }
    return false;
}

bool ParseLine(const std::string& line, Vector& v, std::string* error) {
    const auto f = Split(line, '\t');
    if (f.size() != 8) {
        *error = "expected 8 tab-separated fields, found " + std::to_string(f.size());
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
    uint64_t fpcr = 0;
    if (!Hex(f[5], fpcr) || fpcr > 0xFFFFFFFFu) {
        *error = "bad fpcr";
        return false;
    }
    v.fpcr = static_cast<uint32_t>(fpcr);
    if (f[6] != "-") {
        for (const auto& r : Split(f[6], ',')) {
            const auto eq = r.find('=');
            if (eq == std::string::npos) {
                *error = "bad register override '" + r + "'";
                return false;
            }
            const std::string name = r.substr(0, eq), val = r.substr(eq + 1);
            if (name == "vk") {
                if (!ParseKind(val, v.fill)) {
                    *error = "bad lane kind '" + val + "'";
                    return false;
                }
                continue;
            }
            if (name.size() > 1 && name[0] == 'v' && std::isdigit(static_cast<unsigned char>(name[1]))) {
                const int n = std::atoi(name.c_str() + 1);
                V128 q{};
                if (n < 0 || n > 31 || !Hex128(val, q)) {
                    *error = "bad vector register override '" + r + "'";
                    return false;
                }
                v.vregs.emplace_back(static_cast<unsigned>(n), q);
                continue;
            }
            uint64_t value = 0;
            if (!Hex(val, value)) {
                *error = "bad register override '" + r + "'";
                return false;
            }
            v.regs.emplace_back(name, value);
        }
    }
    if (f[7] != "-") {
        for (const auto& m : Split(f[7], ',')) {
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

// splitmix64: the seed's stream (tests/gen_vectors.py does not need to reproduce it: it overrides what an instruction reads)
struct Stream {
    uint64_t state;
    uint64_t Next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint64_t Below(uint64_t n) { return Next() % n; }
};

// ---- Lane values of the fill kinds ----

const uint16_t kF16[] = {
    0x0000, 0x8000, 0x7C00, 0xFC00, 0x7E00, 0xFE00, 0x7E01, 0x7FFF, 0xFFFF, 0x7C01, 0x7DFF, 0xFC01, 0xFD00, 0x0001, 0x8001, 0x03FF, 0x83FF, 0x0200,
    0x0400, 0x8400, 0x0401, 0x7BFF, 0xFBFF, 0x3C00, 0xBC00, 0x3800, 0xB800, 0x4000, 0x3E00, 0x4100, 0xC100, 0x4200, 0x3BFF, 0x3C01, 0x6400, 0x63FF,
    0x6800, 0x7800, 0x77FF, 0x1400, 0x37FF, 0x3801,
};
const uint32_t kF32[] = {
    0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FC00001, 0x7FFFFFFF, 0xFFFFFFFF, 0x7F800001, 0x7FBFFFFF, 0xFF800001,
    0xFFA00000, 0x7FA5A5A5, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00400000, 0x00800000, 0x80800000, 0x00800001, 0x00FFFFFF, 0x7F7FFFFF,
    0xFF7FFFFF, 0x7F000000, 0x3F800000, 0xBF800000, 0x3F000000, 0xBF000000, 0x40000000, 0xC0000000, 0x3FC00000, 0xBFC00000, 0x40200000, 0xC0200000,
    0x40400000, 0x3F7FFFFF, 0x3F800001, 0x3EFFFFFF, 0x3F000001, 0x4B000000, 0x4B7FFFFF, 0x4B800000, 0x4AFFFFFF, 0x34000000, 0x4EFFFFFF, 0x4F000000,
    0xCF000000, 0xCF000001, 0x4F7FFFFF, 0x4F800000, 0x5EFFFFFF, 0x5F000000, 0xDF000000, 0xDF000001, 0x5F7FFFFF, 0x5F800000, 0x46FFFE00, 0x47000000,
    0xC7000000, 0xC7000100, 0x477FFF00, 0x47800000, 0x477FE000, 0x477FF000, 0x477FEFFF, 0x477FF001, 0x33800000, 0x33000000, 0x33000001, 0x387FC000,
    0x38800000, 0x387FE000, 0x7E800000, 0x3F3504F3, 0x40490FDB, 0x2F800000, 0x5F7FFFFE,
};
const uint64_t kF64[] = {
    0x0000000000000000, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000000000001,
    0x7FFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0x7FF0000000000001, 0x7FF7FFFFFFFFFFFF, 0xFFF0000000000001, 0xFFF4000000000000, 0x7FF4A5A5A5A5A5A5,
    0x0000000000000001, 0x8000000000000001, 0x000FFFFFFFFFFFFF, 0x800FFFFFFFFFFFFF, 0x0008000000000000, 0x0010000000000000, 0x8010000000000000,
    0x0010000000000001, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF, 0x3FF0000000000000, 0xBFF0000000000000, 0x3FE0000000000000, 0xBFE0000000000000,
    0x4000000000000000, 0xC000000000000000, 0x3FF8000000000000, 0xBFF8000000000000, 0x4004000000000000, 0xC004000000000000, 0x3FEFFFFFFFFFFFFF,
    0x3FF0000000000001, 0x3FDFFFFFFFFFFFFF, 0x4330000000000000, 0x433FFFFFFFFFFFFF, 0x4340000000000000, 0x41DFFFFFFFC00000, 0x41DFFFFFFFE00000,
    0x41E0000000000000, 0xC1E0000000000000, 0xC1E0000000200000, 0xC1E0000000100000, 0x41EFFFFFFFE00000, 0x41EFFFFFFFF00000, 0x41F0000000000000,
    0x43DFFFFFFFFFFFFF, 0x43E0000000000000, 0xC3E0000000000000, 0xC3E0000000000001, 0x43EFFFFFFFFFFFFF, 0x43F0000000000000, 0x40DFFFC000000000,
    0x40E0000000000000, 0xC0E0000000000000, 0x40EFFFE000000000, 0x47EFFFFFE0000000, 0x47EFFFFFF0000000, 0x47EFFFFFEFFFFFFF, 0x36A0000000000000,
    0x3690000000000000, 0x3690000000000001, 0x380FFFFFC0000000, 0x3810000000000000, 0x380FFFFFE0000000, 0x40EFFC0000000000, 0x40EFFE0000000000,
    0x3E70000000000000, 0x3E60000000000000, 0x3CB0000000000000, 0x400921FB54442D18, 0x7FE0000000000000,
};
const uint8_t kI8[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0x81, 0xFF, 0xFE, 0x40, 0x3F, 0xC0, 0x10, 0x08, 0x07, 0xF8, 0xF9, 0x55, 0xAA};
const uint16_t kI16[] = {0x0000, 0x0001, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0xFFFE, 0x00FF, 0xFF00, 0x0080, 0xFF80, 0x007F, 0x4000, 0xC000, 0x0100, 0x0010, 0xFFF0, 0x5555};
const uint32_t kI32[] = {0x00000000, 0x00000001, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 0x0000FFFF, 0xFFFF0000, 0x00008000,
                         0xFFFF8000, 0x00007FFF, 0x40000000, 0xC0000000, 0x00010000, 0x000000FF, 0xFFFFFF80, 0x3FFFFFFF, 0x55555555, 0x00000020};
const uint64_t kI64[] = {0x0000000000000000, 0x0000000000000001, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0x8000000000000001, 0xFFFFFFFFFFFFFFFF,
                         0xFFFFFFFFFFFFFFFE, 0x00000000FFFFFFFF, 0xFFFFFFFF00000000, 0x0000000080000000, 0xFFFFFFFF80000000, 0x000000007FFFFFFF,
                         0x4000000000000000, 0xC000000000000000, 0x0000000100000000, 0x3FFFFFFFFFFFFFFF, 0x5555555555555555, 0x0000000000000040};

template<typename T, size_t N>
uint64_t Pick(Stream& r, const T (&table)[N]) {
    return table[r.Below(N)];
}

// A floating-point lane: a special value, a value of moderate magnitude, a small integer or half-integer, or random bits
uint64_t FloatLane(Stream& r, unsigned bits) {
    const unsigned mant = bits == 16 ? 10 : bits == 32 ? 23 : 52;
    const unsigned ebits = bits - 1 - mant;
    const uint64_t bias = (1ull << (ebits - 1)) - 1;
    const uint64_t k = r.Below(16);
    if (k < 7) {
        return bits == 16 ? Pick(r, kF16) : bits == 32 ? Pick(r, kF32) : Pick(r, kF64);
    }
    if (k < 11) {
        // sign, an exponent within 2^-12 .. 2^+12 of 1 (or wider for 64 bits), a random mantissa
        const uint64_t span = bits == 16 ? 12 : 30;
        const uint64_t e = bias - span + r.Below(2 * span + 1);
        const uint64_t m = r.Next() & ((1ull << mant) - 1);
        return (r.Below(2) << (bits - 1)) | (e << mant) | m;
    }
    if (k < 13) {
        // n or n + 0.5 for a small n: rounding ties and exact integers
        const int64_t n = static_cast<int64_t>(r.Below(65)) - 32;
        const double d = static_cast<double>(n) + (r.Below(2) ? 0.5 : 0.0);
        if (bits == 64) {
            uint64_t u;
            std::memcpy(&u, &d, 8);
            return u;
        }
        const float f = static_cast<float>(d);
        uint32_t u;
        std::memcpy(&u, &f, 4);
        if (bits == 32) {
            return u;
        }
        // to half: exact for these small values
        const uint32_t sign = u >> 31, exp = (u >> 23) & 0xFF, man = u & 0x7FFFFF;
        if (exp == 0) {
            return sign << 15;
        }
        return (sign << 15) | ((exp - 127 + 15) << 10) | (man >> 13);
    }
    if (k == 13) {
        // a denormal
        return (r.Below(2) << (bits - 1)) | (r.Next() & ((1ull << mant) - 1));
    }
    return r.Next() & (bits == 64 ? ~0ull : (1ull << bits) - 1);
}

uint64_t IntLane(Stream& r, unsigned bits) {
    const uint64_t k = r.Below(8);
    const uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    if (k < 3) {
        return bits == 8 ? Pick(r, kI8) : bits == 16 ? Pick(r, kI16) : bits == 32 ? Pick(r, kI32) : Pick(r, kI64);
    }
    if (k < 5) {
        // a small signed value: the shift amounts of the register shifts (sshl, ushl, srshl, sqshl, ...) read the low byte of each lane
        return static_cast<uint64_t>(static_cast<int64_t>(r.Below(141)) - 70) & mask;
    }
    if (k == 5) {
        return (1ull << r.Below(bits)) & mask;
    }
    return r.Next() & mask;
}

V128 FillV(Stream& r, FillKind kind) {
    unsigned bits = 64;
    bool fp = false;
    switch (kind) {
    case FillKind::Random:
        return {r.Next(), r.Next()};
    case FillKind::F16:
        bits = 16, fp = true;
        break;
    case FillKind::F32:
        bits = 32, fp = true;
        break;
    case FillKind::F64:
        bits = 64, fp = true;
        break;
    case FillKind::I8:
        bits = 8;
        break;
    case FillKind::I16:
        bits = 16;
        break;
    case FillKind::I32:
        bits = 32;
        break;
    case FillKind::I64:
        bits = 64;
        break;
    }
    V128 q{0, 0};
    for (unsigned lane = 0; lane < 128 / bits; ++lane) {
        const uint64_t e = fp ? FloatLane(r, bits) : IntLane(r, bits);
        const unsigned at = lane * bits;
        q[at / 64] |= e << (at % 64);
    }
    return q;
}

}  // namespace

VectorReader::VectorReader(std::vector<std::string> paths) {
    namespace fs = std::filesystem;
    for (const auto& p : paths) {
        std::error_code ec;
        if (p != "-" && fs::is_directory(p, ec)) {
            std::vector<std::string> in_dir;
            for (const auto& e : fs::directory_iterator(p, ec)) {
                const std::string name = e.path().filename().string();
            if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".tsv") == 0 || (name.size() > 7 && name.compare(name.size() - 7, 7, ".tsv.gz") == 0))) {
                    in_dir.push_back(e.path().string());
                }
            }
            std::sort(in_dir.begin(), in_dir.end());
            if (in_dir.empty()) {
                files.push_back(p + "/*.tsv");  // reported as unreadable by OpenNext
            }
            files.insert(files.end(), in_dir.begin(), in_dir.end());
        } else {
            files.push_back(p);
        }
    }
}

VectorReader::~VectorReader() {
    if (in) {
        gzclose(static_cast<gzFile>(in));
    }
}

bool VectorReader::OpenNext(std::string* error) {
    // zlib reads gzip files and plain ones alike (.tsv.gz, .tsv, a pipe)
    if (in) {
        gzclose(static_cast<gzFile>(in));
    }
    in = nullptr;
    if (next_file >= files.size()) {
        return false;
    }
    current = files[next_file++];
    line = 0;
    if (current == "-") {
        in = gzdopen(dup(STDIN_FILENO), "rb");
        current = "<stdin>";
    } else {
        in = gzopen(current.c_str(), "rb");
    }
    if (!in) {
        *error = "cannot read " + current;
        return false;
    }
    gzbuffer(static_cast<gzFile>(in), 1 << 20);
    return true;
}

bool VectorReader::Next(Vector& out, std::string* error) {
    error->clear();
    std::string text;
    while (true) {
        if (!in && !OpenNext(error)) {
            return false;
        }
        text.clear();
        int c = 0;
        while ((c = gzgetc(static_cast<gzFile>(in))) != -1 && c != '\n') {
            if (c != '\r') {
                text.push_back(static_cast<char>(c));
            }
        }
        if (c == -1 && text.empty()) {
            gzclose(static_cast<gzFile>(in));
            in = nullptr;
            continue;
        }
        ++line;
        if (text.empty() || text[0] == '#') {
            continue;
        }
        out = Vector{};
        out.where = current + ":" + std::to_string(line);
        if (!ParseLine(text, out, error)) {
            *error = out.where + ": " + *error;
            return false;
        }
        return true;
    }
}

void SeedState(const Vector& v, State& s, std::vector<uint8_t>& image) {
    Stream r{v.seed};
    for (auto& reg : s.x) {
        reg = r.Next();
    }
    s.sp = (kGuestBase + 0x100 + r.Next() % (kImageSize - 0x200)) & ~uint64_t{15};
    s.pc = kCodeBase;
    s.nzcv = static_cast<uint32_t>(r.Next() & 0xF) << 28;
    s.fpcr = v.fpcr;
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
    // The vector registers from a stream of their own, so that the values above stay what they were before the registers were seeded
    Stream rv{v.seed ^ 0x5EED5EED5EED5EEDull};
    for (auto& q : s.v) {
        q = FillV(rv, v.fill);
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
    for (const auto& [n, q] : v.vregs) {
        s.v[n] = q;
    }
    for (const auto& [offset, bytes] : v.mem) {
        std::copy(bytes.begin(), bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

bool InCode(const Vector& v, uint64_t pc) {
    return pc >= kCodeBase && pc < kCodeBase + 4 * v.code.size() && (pc & 3) == 0;
}

}  // namespace nxaot::vec
