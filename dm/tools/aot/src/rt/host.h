// The minimal native run time of a pack (docs/design/aot-recompiler.md sections 2.4, 3.3, 4, 5, 6.5, 8 and 9.4), enough for the corpus: it
// reserves an arena below 2^39 and maps a module's image at its one-to-one guest address in it (5.1, 5.2; text and read-only data readable,
// never executable), loads the pack's shared object with dlopen and checks its identity against the image (9.4), writes the module's base,
// sets up a state block (SP, TLS, lr = a host-return address, the exclusive monitor clear: 6.5), the guest MXCSR (7.1), and runs guest code
// through the library's `enter` trampoline. It defines what the library imports (include/nxaot/aot_module.h): the SVC path (svcOutputDebugString
// and svcExitProcess, what corpus/rt calls), the dispatcher's miss path over the lookup table, traps, and the counter (19.2 MHz guest ticks).
// A non-local transfer that reaches the outermost frame is dispatched as a new call (3.3); a target without a compiled entry, like every other
// case of section 11, stops the run with a [RED] line and the JIT-needed status (the JIT fallback is phase 3). A fault in guest code is reported
// at its guest instruction through the line table (3.5); 5.4's dirty tracking is the host's (dm/host) job.
//
// What dm/host must provide instead, later: the arena allocator, dm_guest_call, svc_dispatch behind dm_aot_svc, thread creation with a state
// block and native stack per thread, the JIT fallback (README.md, "The native run time").
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "module/image.h"
#include "nxaot/aot_module.h"
#include "nxaot/guest_state.h"

namespace nxaot::rt {

// The arena (section 5.2): one reservation below 2^39; everything the guest sees lies in it, the state blocks and native stacks outside
constexpr uint64_t kArenaBase = 0x10'0000'0000ull;          // 64 GiB
constexpr uint64_t kArenaSize = 0x2'0000'0000ull;           // 8 GiB reserved (PROT_NONE until used)
constexpr uint64_t kHostReturn = kArenaBase + 0x10;         // the lr the host enters guest code with: an address no code lives at
constexpr uint64_t kStackBase = kArenaBase + 0x0100'0000ull, kStackSize = 0x10'0000;   // the main thread's guest stack, 1 MiB
constexpr uint64_t kTlsBase = kArenaBase + 0x0200'0000ull, kTlsSize = 0x1000;          // its TLS block (tpidr_el0)
constexpr uint64_t kScratchBase = kArenaBase + 0x0300'0000ull, kScratchSize = 0x10'0000;  // data for nxaot_funcdiff
constexpr uint64_t kModuleBase = kArenaBase + 0x1000'0000ull;  // where the module's image is mapped (its guest address of offset 0)

// Exit statuses of a run (nxaot_host exits with them)
enum Status : int {
    kExited = 0,      // svcExitProcess
    kTrap = 1,        // brk, udf, an undefined encoding
    kUsage = 2,       // usage, a library or image that does not load or does not match
    kJit = 3,         // the JIT fallback of section 11 is needed (phase 3): an indirect target with no compiled entry, a non-local transfer with
                      // no frame and no entry, a branch into the middle of a function, FPCR away from the specialisation, a refused function
    kFault = 4,       // a fault in guest code (SIGSEGV, SIGBUS, ...)
    kBadSvc = 5,      // an SVC the corpus run time does not answer, or one with bad arguments
    kReturned = 6,    // guest code returned to the host where it must not (_start), or a call the analysis says does not return came back
    kTimeout = 7,     // still running at the time limit (nxaot_funcdiff)
    kSvcStop = 8,     // stopped at an svc on request (nxaot_funcdiff)
};

struct Options {
    bool relocate = false;  // the host applies the module's relocations itself, as dm/host's loader does (the corpus's crt0 then skips its own)
    bool quiet = false;     // svcOutputDebugString lines are counted, not printed
    bool capture = false;   // kept in Result::output instead of printed
    std::vector<std::pair<std::string, std::string>> sets;  // symbol=value or symbol=@offset (base + offset), written before the run
};

struct Result {
    int status = kReturned;
    std::string message;     // the [RED] line when status is not kExited
    std::string output;      // the lines (Options::capture)
    uint64_t lines = 0;      // svcOutputDebugString calls answered
    uint64_t misses = 0;     // dispatcher misses (inline-cache misses that found an entry)
    double seconds = 0;      // guest time: from the first entry to the end
    uint32_t svc = 0;        // kSvcStop: the svc number
    uint32_t trap = 0;       // kTrap / kJit from a trap: the reason (NXAOT_TRAP_*)
    uint64_t trap_offset = 0;
    int fault_signal = 0;    // kFault
    uint64_t fault_address = 0, fault_pc = 0;
};

class Host {
public:
    static std::unique_ptr<Host> Create(const std::string& library, const module::Image& image, const Options& options, std::string* error);
    ~Host();

    // A fresh copy of the image (bss zeroed), the relocations if asked, the --set values, then _start until svcExitProcess or a failure
    Result RunStart();
    // The standard entry of the function at a module offset with st's registers (nxaot_funcdiff): until it returns to kHostReturn, an svc
    // (svc_stops), a trap, a fault or timeout_ms
    Result Call(uint64_t offset, dm_guest_state* st, bool svc_stops, unsigned timeout_ms);

    const nxaot_module_desc& Desc() const { return *desc; }
    const module::Image& Image() const { return image; }
    const void* Lookup(uint64_t offset) const;  // the native standard entry of the function at a module offset, null when none
    std::string Where(uint64_t native_pc) const;  // a native address as the guest instruction and function the line table and function map say
    std::vector<uint64_t> LookupOffsets() const;  // every module offset the lookup table holds
    size_t LineEntries() const { return lines.size(); }

    void ResetImage();   // the image's bytes again, bss and the stack, TLS and scratch zeroed
    void Relocate();     // the module's relocations at kModuleBase
    bool ApplySets(std::string* error);

    // The library's imports (the extern "C" functions of host.cpp call these)
    void Svc(dm_guest_state* st, uint32_t number);
    void* Dispatch(dm_guest_state* st, uint64_t target, const nxaot_ic_desc** slot, uint64_t site);
    [[noreturn]] void Trap(dm_guest_state* st, uint64_t offset, uint32_t reason);

private:
    Host(const module::Image& image, const Options& options) : image(image), options(options) {}
    Result Execute(dm_guest_state* st, const void* entry, bool svc_stops, unsigned timeout_ms, bool returning_is_normal);

    const module::Image& image;
    Options options;
    void* handle = nullptr;
    const nxaot_module_desc* desc = nullptr;
    uint64_t module_bytes = 0;  // the mapped size: image and bss, page-rounded
    std::vector<std::pair<uint64_t, uint32_t>> lines;           // native address -> module offset, sorted
    std::vector<std::pair<uint64_t, uint64_t>> fn_by_native;    // native body -> module offset, sorted
    std::map<uint64_t, std::unique_ptr<nxaot_ic_desc>> descs;   // run-time inline-cache descriptors by guest address (immutable once published)
};

// The MXCSR guest code runs with for an FPCR (lift::GuestMxcsr computes the same for the lifter's tests): exceptions masked, RMode as the
// rounding control, FZ as FTZ and DAZ
uint32_t GuestMxcsr(uint32_t fpcr);

const char* StatusName(int status);

}  // namespace nxaot::rt
