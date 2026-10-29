#include "module/manifest.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace nxaot::module {

namespace {

std::vector<std::string> Split(const std::string& line, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t p = line.find(sep, start);
        out.push_back(line.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) {
            return out;
        }
        start = p + 1;
    }
}

bool Hex(const std::string& s, uint64_t* v) {
    std::string t = s;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        t = t.substr(2);
    }
    if (t.empty() || t.size() > 16) {
        return false;
    }
    uint64_t r = 0;
    for (const char c : t) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        r = r << 4 | static_cast<uint64_t>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::tolower(c) - 'a' + 10));
    }
    *v = r;
    return true;
}

bool Dec(const std::string& s, uint64_t* v) {
    if (s.empty() || s.size() > 19) {
        return false;
    }
    uint64_t r = 0;
    for (const char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        r = r * 10 + static_cast<uint64_t>(c - '0');
    }
    *v = r;
    return true;
}

bool HexBytes(const std::string& s) {
    if (s.empty() || s.size() % 2) {
        return false;
    }
    for (const char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

bool HexList(const std::string& s, std::vector<uint64_t>* out) {
    for (const std::string& part : Split(s, ',')) {
        uint64_t v = 0;
        if (!Hex(part, &v)) {
            return false;
        }
        out->push_back(v);
    }
    return true;
}

std::string X(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%#llx", static_cast<unsigned long long>(v));
    return v ? buf : "0x0";
}

}  // namespace

std::optional<Manifest> ParseManifest(const std::string& text, std::string* error) {
    Manifest m;
    std::istringstream in{text};
    std::string line;
    size_t n = 0;
    bool header = false;
    auto fail = [&](const std::string& why) -> std::optional<Manifest> {
        *error = "line " + std::to_string(n) + ": " + why;
        return std::nullopt;
    };
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto f = Split(line, '\t');
        if (!header) {
            uint64_t version = 0;
            if (f.size() != 4 || f[0].rfind("# nxaot-manifest ", 0) != 0 || !Dec(f[0].substr(17), &version)) {
                return fail("the first line must be '# nxaot-manifest <version>\\t<module>\\t<build ID>\\t<SHA-256>'");
            }
            if (version != 1) {
                return fail("manifest version " + std::to_string(version) + ", this nxaot reads version 1");
            }
            uint64_t dummy = 0;
            if (f[1].empty() || f[2].empty() || f[3].size() != 64 || !Hex(f[3].substr(0, 16), &dummy) || !HexBytes(f[3])) {
                return fail("bad module name, build ID or SHA-256 in the header");
            }
            m.version = static_cast<int>(version);
            m.module = f[1];
            m.build_id = f[2];
            m.sha256 = f[3];
            header = true;
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        const std::string& kind = f[0];
        if (kind == "layout") {
            Manifest::Layout l;
            if (f.size() != 11 || f[1] != "text_end" || f[3] != "ro_off" || f[5] != "data_off" || f[7] != "bss" || f[9] != "image_size" ||
                !Hex(f[2], &l.text_end) || !Hex(f[4], &l.ro_off) || !Hex(f[6], &l.data_off) || !Hex(f[8], &l.bss) || !Hex(f[10], &l.image_size)) {
                return fail("layout: text_end <hex> ro_off <hex> data_off <hex> bss <hex> image_size <hex>");
            }
            if (m.layout) {
                return fail("a second layout row");
            }
            if (!(l.text_end <= l.ro_off && l.ro_off <= l.data_off && l.data_off <= l.image_size)) {
                return fail("layout: text_end <= ro_off <= data_off <= image_size does not hold");
            }
            m.layout = l;
        } else if (kind == "aux") {
            Manifest::Aux a;
            if (f.size() != 5 || f[3] != "fps_state" || !Hex(f[1], &a.offset) || !Hex(f[2], &a.length) || !Hex(f[4], &a.fps_state)) {
                return fail("aux: <offset> <length> fps_state <offset>");
            }
            if (m.aux) {
                return fail("a second aux row");
            }
            m.aux = a;
        } else if (kind == "data" || kind == "oracle-W") {
            Manifest::Range r;
            if (f.size() != 3 || !Hex(f[1], &r.lo) || !Hex(f[2], &r.hi) || r.hi < r.lo) {
                return fail(kind + ": <start> <end> with start <= end");
            }
            (kind == "data" ? m.data : m.oracle_w).push_back(r);
        } else if (kind == "obs") {
            Manifest::Obs o;
            uint64_t number = 0, word = 0;
            if (f.size() != 5 || !Hex(f[1], &o.offset) || !Dec(f[2], &number) || number > 0xFFFFFFFFu || !Hex(f[3], &o.parameter) ||
                !Hex(f[4], &word) || word > 0xFFFFFFFFu) {
                return fail("obs: <offset> <number (decimal)> <parameter> <original instruction word>");
            }
            o.number = static_cast<uint32_t>(number);
            o.word = static_cast<uint32_t>(word);
            m.obs.push_back(o);
        } else if (kind == "spin") {
            Manifest::Spin s;
            if (f.size() != 3 || !Hex(f[1], &s.head) || !HexList(f[2], &s.back_edges)) {
                return fail("spin: <head> <back-edge>[,<back-edge>...]");
            }
            m.spin.push_back(s);
        } else if (kind == "cut") {
            Manifest::Cut c;
            if (f.size() < 5 || !Hex(f[1], &c.offset) || !HexBytes(f[2]) || !HexBytes(f[3]) || f[2].size() != f[3].size()) {
                return fail("cut: <offset> <old hex> <new hex> <reason>, the byte strings of one length");
            }
            c.old_hex = f[2];
            c.new_hex = f[3];
            for (size_t i = 4; i < f.size(); ++i) {
                c.reason += (i > 4 ? "\t" : "") + f[i];
            }
            m.cuts.push_back(c);
        } else if (kind == "oracle-T") {
            Manifest::OracleT t;
            if (f.size() != 3 || !Hex(f[1], &t.br) || !HexList(f[2], &t.targets)) {
                return fail("oracle-T: <br offset> <target>[,<target>...]");
            }
            m.oracle_t.push_back(t);
        } else if (kind == "oracle-J") {
            Manifest::OracleJ j;
            uint64_t width = 0;
            if (f.size() != 4 || !Hex(f[1], &j.br) || !Hex(f[2], &j.base) || !Dec(f[3], &width) || (width != 1 && width != 2)) {
                return fail("oracle-J: <br offset> <base> <width: 1 or 2>");
            }
            j.width = static_cast<uint32_t>(width);
            m.oracle_j.push_back(j);
        } else {
            return fail("unknown row kind '" + kind + "'");
        }
    }
    if (!header) {
        *error = "empty manifest";
        return std::nullopt;
    }
    return m;
}

std::string WriteManifest(const Manifest& m) {
    std::string out = "# nxaot-manifest " + std::to_string(m.version) + "\t" + m.module + "\t" + m.build_id + "\t" + m.sha256 + "\n";
    if (m.layout) {
        const auto& l = *m.layout;
        out += "layout\ttext_end\t" + X(l.text_end) + "\tro_off\t" + X(l.ro_off) + "\tdata_off\t" + X(l.data_off) + "\tbss\t" + X(l.bss) +
               "\timage_size\t" + X(l.image_size) + "\n";
    }
    if (m.aux) {
        out += "aux\t" + X(m.aux->offset) + "\t" + X(m.aux->length) + "\tfps_state\t" + X(m.aux->fps_state) + "\n";
    }
    for (const auto& r : m.data) {
        out += "data\t" + X(r.lo) + "\t" + X(r.hi) + "\n";
    }
    for (const auto& o : m.obs) {
        out += "obs\t" + X(o.offset) + "\t" + std::to_string(o.number) + "\t" + X(o.parameter) + "\t" + X(o.word) + "\n";
    }
    auto list = [](const std::vector<uint64_t>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) {
            s += (i ? "," : "") + X(v[i]);
        }
        return s;
    };
    for (const auto& s : m.spin) {
        out += "spin\t" + X(s.head) + "\t" + list(s.back_edges) + "\n";
    }
    for (const auto& c : m.cuts) {
        out += "cut\t" + X(c.offset) + "\t" + c.old_hex + "\t" + c.new_hex + "\t" + c.reason + "\n";
    }
    for (const auto& t : m.oracle_t) {
        out += "oracle-T\t" + X(t.br) + "\t" + list(t.targets) + "\n";
    }
    for (const auto& j : m.oracle_j) {
        out += "oracle-J\t" + X(j.br) + "\t" + X(j.base) + "\t" + std::to_string(j.width) + "\n";
    }
    for (const auto& w : m.oracle_w) {
        out += "oracle-W\t" + X(w.lo) + "\t" + X(w.hi) + "\n";
    }
    return out;
}

}  // namespace nxaot::module
