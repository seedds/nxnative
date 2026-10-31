// The minimal native run time of a pack (docs/design/aot-recompiler.md sections 2.4, 3.3, 3.4, 4, 5, 6, 8, 9.4 and 11), enough for the corpus:
// it reserves an arena below 2^39 and maps a module's image at its one-to-one guest address in it (5.1, 5.2; text and read-only data readable,
// never executable), loads the pack's shared object with dlopen and checks its identity against the image (9.4), writes the module's base,
// sets up a state block (SP, TLS, lr = a host-return address, the exclusive monitor clear: 6.5), the guest MXCSR (7.1), and runs guest code
// through the library's `enter` trampoline. It defines what the library imports (include/nxaot/aot_module.h): the SVC path (the corpus's SVCs:
// output, exit, and the thread and synchronization SVCs of section 6.1 mapped onto pthreads), the dispatcher's miss path over the lookup table,
// traps, the JIT fallback (section 11, src/rt/jit.h: dynarmic per thread, from the dispatcher's misses, branches to no entry, continuations with
// no native frame, FPCR away from the specialisation, refused functions), the safepoints of section 6.2 (a pause request stops every guest
// thread at a poll, in an SVC or in the JIT with its whole register file in its state block) and the counter (19.2 MHz guest ticks). A non-local
// transfer that reaches the outermost frame of a thread is dispatched as a new call, or run in the JIT when it has no compiled entry (3.3). A fault
// in guest code is reported at its guest instruction through the line table (3.5); 5.4's dirty tracking is the host's (dm/host) job.
//
// What dm/host must provide instead, later: README.md, "The native run time" and "Threads and safepoints".
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
constexpr unsigned kMaxThreads = 64;                          // guest threads per run, the JIT's exclusive monitor's processors

// Exit statuses of a run (nxaot_host exits with them)
enum Status : int {
    kExited = 0,      // svcExitProcess
    kTrap = 1,        // brk, udf, an undefined encoding
    kUsage = 2,       // usage, a library or image that does not load or does not match
    kJit = 3,         // the JIT fallback of section 11 is needed and refused (JitMode::None)
    kFault = 4,       // a fault in guest code (SIGSEGV, SIGBUS, ...)
    kBadSvc = 5,      // an SVC the corpus run time does not answer, or one with bad arguments
    kReturned = 6,    // guest code returned to the host where it must not (_start, a thread's entry), or a no-return call came back
    kTimeout = 7,     // still running at the time limit (nxaot_funcdiff)
    kSvcStop = 8,     // stopped at an svc on request (nxaot_funcdiff)
    kPauseFailed = 9, // the pause test found a paused thread's state block inconsistent (--pause-test)
};

// Where guest code runs (section 11): compiled entries with the JIT for the rest (the default), everything in the JIT, or no JIT at all
enum class JitMode : uint8_t { Mixed, All, None };

struct Options {
    bool relocate = false;  // the host applies the module's relocations itself, as dm/host's loader does (the corpus's crt0 then skips its own)
    bool quiet = false;     // svcOutputDebugString lines are counted, not printed
    bool capture = false;   // kept in Result::output instead of printed
    std::vector<std::pair<std::string, std::string>> sets;  // symbol=value or symbol=@offset (base + offset), written before the run
    JitMode jit = JitMode::Mixed;
    bool jit_log = true;          // a [RED] line (standard error) for the first JIT entry at each address
    bool jit_one_block = false;   // measurements: the JIT runs one block per Run() and the host checks the next pc (design 11's alternative)
    unsigned pause_test = 0;      // pause every guest thread this many times while more than one runs, check their state blocks, resume
    uint64_t pause_seed = 1;      // the random intervals of the pause test
    unsigned pause_interval_us = 100;  // between two pauses: a random time up to this (at least a tenth of it)
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
    // The JIT fallback (section 11): dm_jit_run calls, the distinct addresses they started at, Run() calls, single steps of stop addresses,
    // native calls the JIT made into compiled entries, and the seconds spent in dm_jit_run
    uint64_t jit_runs = 0, jit_addresses = 0, jit_run_calls = 0, jit_steps = 0, jit_native_calls = 0;
    double jit_seconds = 0;
    uint64_t threads = 0;    // guest threads the run created (svcCreateThread)
    // The pause test (section 6.2): pauses made while more than one guest thread ran, thread stops seen at polls, in SVCs, in the JIT, state
    // blocks checked against the corpus's register invariant, and inconsistencies found
    uint64_t pauses = 0, pauses_threaded = 0, stops_poll = 0, stops_svc = 0, stops_jit = 0, invariant_checks = 0, inconsistent = 0;
    double pause_max_us = 0;  // the longest time from the request until every thread had stopped
};

struct Thread;

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
    const nxaot_lookup_entry* Find(uint64_t offset) const;  // its lookup slot, null when none
    bool IsReturn(uint64_t offset) const;       // the module offset after a call site of compiled code (desc->returns)
    std::string Where(uint64_t native_pc) const;  // a native address as the guest instruction and function the line table and function map say
    std::vector<uint64_t> LookupOffsets() const;  // every module offset the lookup table holds
    size_t LineEntries() const { return lines.size(); }

    void ResetImage();   // the image's bytes again, bss and the stack, TLS and scratch zeroed
    void Relocate();     // the module's relocations at kModuleBase
    bool ApplySets(std::string* error);

    // The library's imports (the extern "C" functions of host.cpp call these)
    int Svc(dm_guest_state* st, uint32_t number);   // 0: the guest goes on; 1: the thread must leave (it exits, or the run stops)
    void* Dispatch(dm_guest_state* st, uint64_t target, const nxaot_ic_desc** slot, uint64_t site);
    [[noreturn]] void Trap(dm_guest_state* st, uint64_t offset, uint32_t reason);
    void Jit(dm_guest_state* st, uint64_t lr, uint64_t sp);  // nxaot_rt_jit
    void Safepoint(dm_guest_state* st);                     // nxaot_rt_safepoint

    // Used by the run time's threads and the JIT's callbacks (host.cpp)
    void RunGuest(Thread& t, uint64_t pc);
    void JitLoop(Thread& t, uint64_t lr, uint64_t sp, bool outermost, const char* why);
    int StopKind(uint64_t pc) const;
    void ThreadBody(Thread& t);

private:
    Host(const module::Image& image, const Options& options) : image(image), options(options) {}
    Result Execute(dm_guest_state* st, uint64_t pc, bool svc_stops, unsigned timeout_ms, bool returning_is_normal);
    void NativeCall(Thread& t, const void* entry);
    void PauseTest();
    bool InModule(uint64_t a) const { return a >= kModuleBase && a - kModuleBase < image.bytes.size(); }

    const module::Image& image;
    Options options;
    void* handle = nullptr;
    const nxaot_module_desc* desc = nullptr;
    uint64_t module_bytes = 0;  // the mapped size: image and bss, page-rounded
    uint64_t pause_head = 0;    // the guest address of the corpus's register-invariant loop head (nxaot_pause_loop_head), 0 when none
    std::vector<std::pair<uint64_t, uint32_t>> lines;           // native address -> module offset, sorted
    std::vector<std::pair<uint64_t, uint64_t>> fn_by_native;    // native body -> module offset, sorted
    std::map<uint64_t, std::unique_ptr<nxaot_ic_desc>> descs;   // run-time inline-cache descriptors by guest address (immutable once published)
};

// The MXCSR guest code runs with for an FPCR (lift::GuestMxcsr computes the same for the lifter's tests): exceptions masked, RMode as the
// rounding control, FZ as FTZ and DAZ
uint32_t GuestMxcsr(uint32_t fpcr);

const char* StatusName(int status);

}  // namespace nxaot::rt
