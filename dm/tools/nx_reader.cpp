// Read-only pipe adapter for the vendored NX reader; no conversion engine.
#include "../third_party/nxformat/nx_disk_source.hpp"
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <iostream>
#include <sstream>
#include <stdexcept>

static std::string hex(const uint8_t* p, size_t n) {
    const char* digits = "0123456789abcdef"; std::string out;
    for (size_t i=0;i<n;++i) { out+=digits[p[i]>>4]; out+=digits[p[i]&15]; }
    return out;
}
template<class T> static auto checked(T result) {
    if (!result) throw std::runtime_error(result.error.message);
    return std::move(*result.value);
}
static void checked(nx::Status result) {
    if (!result) throw std::runtime_error(result.error.message);
}
static bool decode(void*, const uint8_t* p, size_t n, uint8_t* out, size_t cap, size_t* written, size_t* consumed) {
    ZSTD_frameHeader h{};
    if (n<5 || p[0]!=0x28 || p[1]!=0xb5 || p[2]!=0x2f || p[3]!=0xfd ||
        (p[4]&0x1b) || ZSTD_getFrameHeader(&h,p,n)!=0 || h.dictID ||
        h.frameContentSize!=cap || h.windowSize>(1u<<20) || ZSTD_findFrameCompressedSize(p,n)!=n) return false;
    auto size=ZSTD_decompress(out,cap,p,n);
    if (ZSTD_isError(size) || size!=cap) return false;
    *written=size; *consumed=n; return true;
}
int main(int argc, char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("usage: nx_reader game.nx");
        auto source=checked(nx::DiskSource::open(argv[1]));
        nx::OpenOptions options; options.zstd=decode; options.max_cached_block_bytes=64u<<20;
        auto archive=checked(nx::Archive::open(*source,options)); checked(archive.mount());
        for (uint32_t a=0;a<archive.applications().size();++a) {
            const auto& app=archive.applications()[a];
            std::cout<<"A "<<a<<' '<<app.id<<' '<<unsigned(app.platform)<<' '<<app.icon_index<<' '<<app.metadata_snapshot<<'\n';
            for (uint32_t i=app.first_snapshot;i<app.first_snapshot+app.snapshot_count;++i) {
                const auto& s=archive.snapshots()[i];
                uint64_t pid=s.program_index==0xffffffffu?0:archive.programs()[s.program_index].id;
                std::cout<<"S "<<a<<' '<<i<<' '<<unsigned(s.type)<<' '<<unsigned(s.flags)<<' '<<s.numeric_version<<' '<<pid<<' '<<s.addon_title_id().value_or(0)<<' '<<s.nacp_index<<' '<<hex(s.identity.data(),32)<<' '<<hex(s.content_id.data(),16)<<' '<<s.icon_index<<' '<<(s.program_index==0xffffffffu?2u:unsigned(archive.programs()[s.program_index].kind))<<'\n';
                checked(archive.dependencies(i,[&](const nx::Dependency& d) {
                    std::cout<<"R "<<i<<' '<<unsigned(d.kind)<<' '<<hex(d.target_identity.data(),32)<<'\n'; return true;
                }));
                std::vector<std::pair<nx::DirectoryHandle,std::string>> todo{{checked(archive.root_directory(i)),""}};
                while (!todo.empty()) {
                    auto item=std::move(todo.back()); todo.pop_back();
                    auto dir=item.first; auto path=std::move(item.second);
                    checked(archive.enumerate(dir,[&](const nx::DirectoryEntryView& e) {
                        auto name=path+std::string(e.name); auto encoded=hex(reinterpret_cast<const uint8_t*>(name.data()),name.size());
                        if (e.type==nx::EntryType::directory) { std::cout<<"D "<<i<<' '<<encoded<<'\n'; todo.emplace_back(e.directory,name+"/"); }
                        else { auto sha=checked(archive.file_digest(e.file)); std::cout<<"F "<<i<<' '<<encoded<<' '<<e.file.value<<' '<<e.size<<' '<<hex(sha.data(),32)<<'\n'; }
                        return true;
                    }));
                }
            }
        }
        std::cout<<"END\n"<<std::flush;
        // One request, one length line followed by exactly that many binary bytes.
        char command; uint64_t id,offset,length;
        while (std::cin>>command>>id) {
            std::vector<uint8_t> data;
            if (command=='M') data=checked(archive.read_media(static_cast<uint32_t>(id)));
            else if (command=='F' && std::cin>>offset>>length) {
                if (length>(4u<<20)) throw std::runtime_error("read exceeds 4 MiB");
                data.resize(length); checked(archive.read_file(nx::FileId{static_cast<uint32_t>(id)},offset,length,data.data(),data.size()));
            } else throw std::runtime_error("invalid read request");
            std::cout<<data.size()<<'\n'; std::cout.write(reinterpret_cast<const char*>(data.data()),data.size()); std::cout.flush();
        }
    } catch (const std::exception& e) { std::cerr<<"NX: "<<e.what()<<'\n'; return 1; }
}
