#include "build/analysis_cache.h"

#include <cstring>
#include <fstream>
#include <iterator>

#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SHA256.h>

#include "cache/cache.h"

namespace nxaot::build {

namespace {

// The entry's format: changed whenever what it holds or how it is written changes
constexpr const char* kFormat = "nxaot-analysis 1\n";

class Writer {
public:
    void U(uint64_t v) {
        do {
            uint8_t b = v & 0x7F;
            v >>= 7;
            out.push_back(static_cast<char>(b | (v ? 0x80 : 0)));
        } while (v);
    }
    void S(int64_t v) { U((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
    void Str(const std::string& s) {
        U(s.size());
        out += s;
    }
    void R(const lift::Regs& r) {
        U(r.x);
        U(r.v);
        U((r.sp ? 1u : 0u) | (r.nzcv ? 2u : 0u));
    }
    std::string out;
};

class Reader {
public:
    Reader(const char* p, const char* e) : p(p), e(e) {}
    uint64_t U() {
        uint64_t v = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (p == e) {
                ok = false;
                return 0;
            }
            const uint8_t b = static_cast<uint8_t>(*p++);
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    int64_t S() {
        const uint64_t u = U();
        return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
    }
    std::string Str() {
        const uint64_t n = U();
        if (!ok || n > static_cast<uint64_t>(e - p)) {
            ok = false;
            return {};
        }
        std::string s(p, p + n);
        p += n;
        return s;
    }
    lift::Regs R() {
        lift::Regs r;
        r.x = static_cast<uint32_t>(U());
        r.v = static_cast<uint32_t>(U());
        const uint64_t f = U();
        r.sp = f & 1;
        r.nzcv = f & 2;
        return r;
    }
    // A count of items that each take at least one byte: more than the bytes left means a corrupt file
    uint64_t Count() {
        const uint64_t n = U();
        if (n > static_cast<uint64_t>(e - p)) ok = false;
        return ok ? n : 0;
    }
    bool ok = true;
    bool AtEnd() const { return p == e; }

private:
    const char* p;
    const char* e;
};

}  // namespace

std::string AnalysisKey(const module::Image& image, const lift::Convention& conv, bool all_entries, uint64_t split_above, const std::string& tool) {
    cache::KeyBuilder k;
    k.Add("format", kFormat).Add("tool", tool).Add("module", image.name).Add("image", image.Sha256Hex());
    // A behaviour image's manifest (its layout, data, observation and spin rows): the file as it is
    k.Add("manifest", image.nro ? std::string("none") : cache::FileSha256(image.path));
    k.Add("cc", lift::CallConvName(conv.cc)).Add("all_entries", all_entries).Add("split_above", split_above);
    return k.Hex();
}

std::string AnalysisPath(const std::string& cache_dir, const std::string& key) {
    return cache_dir + "/analysis/" + key + ".bin";
}

bool StoreAnalysis(const std::string& path, const std::string& key, const analysis::Discovery& d, const Plan& plan, bool reads_fpsr,
                   uint64_t split_above, std::string* error) {
    Writer w;
    w.out = kFormat + key;
    w.U(reads_fpsr);
    w.U(d.notes.size());
    for (const auto& n : d.notes) w.Str(n);
    w.R(plan.indirect_pass);
    w.R(plan.indirect_writes);
    w.U(plan.indirect_rv);
    w.U(plan.live_after_rounds);
    w.U(plan.fns.size());
    uint64_t last = 0;
    for (const auto& [entry, ff] : plan.fns) {
        const analysis::Function& f = d.functions.at(entry);
        const auto rel = [entry = entry](uint64_t a) { return static_cast<int64_t>(a - entry); };
        w.U(entry - last);
        last = entry;
        w.S(rel(ff.limit));
        w.S(rel(f.end));
        w.U(ff.size);
        w.U(ff.words);
        w.Str(ff.name);
        w.R(ff.pass);
        w.R(ff.reads);
        w.R(ff.writes);
        w.R(ff.raw);
        w.R(ff.live_after);
        w.U(ff.rv);
        w.U((ff.returns ? 1u : 0u) | (ff.host_callable ? 2u : 0u));
        w.S(ff.component);
        w.U(ff.continuations.size());
        for (const uint64_t c : ff.continuations) w.S(rel(c));
        w.U(ff.pads.size());
        for (const auto& [call, pad] : ff.pads) {
            w.S(rel(call));
            w.S(rel(pad));
        }
        w.U(ff.tables.size());
        for (const auto& [br, targets] : ff.tables) {
            w.S(rel(br));
            w.U(targets.size());
            for (const uint64_t t : targets) w.S(rel(t));
        }
        w.U(ff.roots.size());
        for (const uint64_t r : ff.roots) w.S(rel(r));
        w.U(ff.extra.size());
        for (const auto& [lo, hi] : ff.extra) {
            w.S(rel(lo));
            w.S(rel(hi));
        }
        // Discovery's view: the blocks' bounds, for a function the pack may split their successors and its windows too; the calls' sites
        const bool edges = ff.words > split_above;
        w.U(edges);
        w.U(f.blocks.size());
        for (const auto& [start, b] : f.blocks) {
            w.S(rel(b.start));
            w.U(b.end - b.start);
            if (edges) {
                w.U(static_cast<uint64_t>(b.term));
                w.S(rel(b.target));
                w.U(b.succ.size());
                for (const auto& [t, kind] : b.succ) {
                    w.S(rel(t));
                    w.U(static_cast<uint64_t>(kind));
                }
            }
        }
        if (edges) {
            w.U(f.windows.size());
            for (const auto& win : f.windows) {
                w.S(rel(win.lo));
                w.S(rel(win.hi));
                w.Str(win.kind);
            }
        }
        w.U(f.calls.size());
        for (const auto& [pc, target] : f.calls) {
            w.S(rel(pc));
            w.U(target);
        }
    }
    const std::array<uint8_t, 32> sum = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(w.out.data()), w.out.size()));
    w.out.append(reinterpret_cast<const char*>(sum.data()), sum.size());
    if (const std::error_code ec = llvm::sys::fs::create_directories(llvm::sys::path::parent_path(path))) {
        *error = "cannot create " + std::string(llvm::sys::path::parent_path(path)) + ": " + ec.message();
        return false;
    }
    return cache::WriteAtomic(path, w.out, error);
}

bool LoadAnalysis(const std::string& path, const std::string& key, CachedAnalysis* out, std::string* why) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        *why = "none";
        return false;
    }
    const std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const std::string head = kFormat + key;
    if (data.size() < head.size() + 32 || data.compare(0, head.size(), head) != 0) {
        *why = "another format or key";
        return false;
    }
    const size_t body = data.size() - 32;
    const std::array<uint8_t, 32> sum = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), body));
    if (std::memcmp(sum.data(), data.data() + body, 32) != 0) {
        *why = "its checksum fails";
        return false;
    }
    Reader r{data.data() + head.size(), data.data() + body};
    CachedAnalysis& a = *out;
    a = CachedAnalysis{};
    a.reads_fpsr = r.U();
    for (uint64_t n = r.Count(); n-- && r.ok;) a.d.notes.push_back(r.Str());
    a.plan.indirect_pass = r.R();
    a.plan.indirect_writes = r.R();
    a.plan.indirect_rv = static_cast<uint32_t>(r.U());
    a.plan.live_after_rounds = r.U();
    uint64_t entry = 0;
    for (uint64_t n = r.Count(); n-- && r.ok;) {
        entry += r.U();
        const auto abs = [&, e = entry]() { return e + static_cast<uint64_t>(r.S()); };
        FnFacts ff;
        analysis::Function f;
        ff.entry = f.entry = entry;
        ff.limit = f.limit = abs();
        f.end = abs();
        ff.size = r.U();
        ff.words = f.words = r.U();
        ff.name = f.name = r.Str();
        ff.pass = r.R();
        ff.reads = r.R();
        ff.writes = r.R();
        ff.raw = r.R();
        ff.live_after = r.R();
        ff.rv = static_cast<uint32_t>(r.U());
        const uint64_t flags = r.U();
        ff.returns = flags & 1;
        ff.host_callable = flags & 2;
        ff.component = static_cast<int>(r.S());
        for (uint64_t k = r.Count(); k-- && r.ok;) ff.continuations.push_back(abs());
        for (uint64_t k = r.Count(); k-- && r.ok;) {
            const uint64_t call = abs();
            ff.pads[call] = abs();
        }
        for (uint64_t k = r.Count(); k-- && r.ok;) {
            const uint64_t br = abs();
            std::vector<uint64_t>& targets = ff.tables[br];
            for (uint64_t t = r.Count(); t-- && r.ok;) targets.push_back(abs());
        }
        for (uint64_t k = r.Count(); k-- && r.ok;) ff.roots.push_back(abs());
        for (uint64_t k = r.Count(); k-- && r.ok;) {
            const uint64_t lo = abs();
            ff.extra.emplace_back(lo, abs());
        }
        const bool edges = r.U() != 0;
        for (uint64_t k = r.Count(); k-- && r.ok;) {
            analysis::Block b;
            b.start = abs();
            b.end = b.start + r.U();
            if (edges) {
                b.term = static_cast<analysis::Term>(r.U());
                b.target = abs();
                for (uint64_t s = r.Count(); s-- && r.ok;) {
                    const uint64_t t = abs();
                    b.succ.emplace_back(t, static_cast<analysis::Edge>(r.U()));
                }
            }
            f.blocks.emplace(b.start, std::move(b));
        }
        if (edges) {
            for (uint64_t k = r.Count(); k-- && r.ok;) {
                analysis::Window win;
                win.lo = abs();
                win.hi = abs();
                win.kind = r.Str();
                f.windows.push_back(std::move(win));
            }
        }
        for (uint64_t k = r.Count(); k-- && r.ok;) {
            const uint64_t pc = abs();
            f.calls.emplace_back(pc, r.U());
        }
        a.plan.fns.emplace(entry, std::move(ff));
        a.d.functions.emplace(entry, std::move(f));
    }
    if (!r.ok || !r.AtEnd()) {
        *why = "it does not parse";
        return false;
    }
    return true;
}

}  // namespace nxaot::build
