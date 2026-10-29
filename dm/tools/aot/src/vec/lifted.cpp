#include "vec/lifted.h"

#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <optional>

#include <immintrin.h>
#include <sys/mman.h>
#include <unistd.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "lift/jit.h"
#include "lift/lifter.h"

namespace nxaot::vec {

namespace {

constexpr uint64_t kGuard = 1 << 20;  // inaccessible bytes on each side of the image: a near miss faults instead of corrupting the harness
constexpr unsigned kMaxContinuations = 64;  // functions one run may chain through (Run): more is a loop the reference's tick budget also ends

sigjmp_buf g_jump;
volatile sig_atomic_t g_in_lifted = 0;
void* volatile g_fault_addr = nullptr;
volatile sig_atomic_t g_fault_signal = 0;

constexpr unsigned kRunSeconds = 5;  // a lifted run that has not returned by then (a loop in the code) ends with SIGALRM

void OnFault(int sig, siginfo_t* info, void*) {
    if (!g_in_lifted) {
        if (sig == SIGALRM) {
            return;
        }
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_fault_signal = sig;
    g_fault_addr = info->si_addr;
    siglongjmp(g_jump, 1);
}

}  // namespace

struct Lifted::Impl {
    std::unique_ptr<lift::Jit> jit;
    lift::Target target;
    lift::NaNMode nan_mode = lift::NaNMode::Chains;
    bool fpsr_flags = true;
    uint8_t* image = nullptr;
    size_t counter = 0;
    std::map<std::tuple<std::vector<uint32_t>, uint64_t, uint32_t>, std::pair<void*, std::vector<int>>> continuations;
};

Lifted::Lifted() : impl(std::make_unique<Impl>()) {}
Lifted::~Lifted() = default;

std::unique_ptr<Lifted> Lifted::Create(const std::string& cpu, lift::NaNMode nan_mode, bool fpsr_flags, std::string* error) {
    std::unique_ptr<Lifted> self{new Lifted()};
    void* want = reinterpret_cast<void*>(kGuestBase - kGuard);
    void* got = mmap(want, kGuard * 2 + kImageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (got != want) {
        *error = "cannot map the guest image at " + std::to_string(kGuestBase) + ": " + std::strerror(errno);
        return nullptr;
    }
    self->impl->image = reinterpret_cast<uint8_t*>(kGuestBase);
    if (mprotect(self->impl->image, kImageSize, PROT_READ | PROT_WRITE) != 0) {
        *error = std::string("mprotect: ") + std::strerror(errno);
        return nullptr;
    }
    struct sigaction sa {};
    sa.sa_sigaction = OnFault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGALRM}) {
        sigaction(sig, &sa, nullptr);
    }
    self->impl->jit = lift::Jit::Create(cpu, error);
    if (!self->impl->jit) {
        return nullptr;
    }
    self->impl->target = self->impl->jit->TargetFeatures();
    self->impl->nan_mode = nan_mode;
    self->impl->fpsr_flags = fpsr_flags;
    return self;
}

std::string Lifted::Cpu() const {
    return impl->jit->Cpu() + " (" + impl->jit->Features() + ")";
}

lift::Target Lifted::Target() const {
    return impl->target;
}

size_t Lifted::Continuations() const {
    return impl->continuations.size();
}

std::vector<Prepared> Lifted::Prepare(const std::vector<const Vector*>& batch, bool dump, std::string* ir) {
    std::vector<Prepared> out(batch.size());
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = impl->jit->NewModule(*context, "nxaot_vec_" + std::to_string(impl->counter));
    std::vector<std::string> names(batch.size());
    std::map<std::pair<std::vector<uint32_t>, uint32_t>, size_t> same;  // (code, FPCR) -> the first vector of the batch with them
    for (size_t i = 0; i < batch.size(); ++i) {
        const Vector& v = *batch[i];
        const auto key = std::make_pair(v.code, v.fpcr & lift::kFpcrModeMask);
        const auto first = same.find(key);
        if (first != same.end()) {
            out[i] = out[first->second];
            names[i] = names[first->second];
            continue;
        }
        same.emplace(key, i);
        lift::Region region;
        region.entry = kCodeBase;
        region.lo = kCodeBase;
        region.hi = kCodeBase + 4 * v.code.size();
        const decode::CodeReader read = [&v](uint64_t vaddr) -> std::optional<uint32_t> {
            if (!InCode(v, vaddr)) {
                return std::nullopt;
            }
            return v.code[(vaddr - kCodeBase) / 4];
        };
        lift::LiftOptions options;
        options.fpcr = v.fpcr;
        options.fpsr_flags = impl->fpsr_flags;
        options.nan_mode = impl->nan_mode;
        options.target = impl->target;
        names[i] = "v" + std::to_string(impl->counter++);
        lift::Lifted l = lift::LiftRegion(*module, names[i], region, read, options);
        out[i].unsupported = std::move(l.unsupported);
        out[i].error = std::move(l.error);
        out[i].opcodes = std::move(l.opcodes);
        out[i].dynarmic_issues = std::move(l.dynarmic_issues);
        if (!l.function) {
            names[i].clear();
        }
    }
    impl->jit->KeepIr(dump);
    std::string error;
    if (!impl->jit->Add(std::move(module), std::move(context), &error)) {
        for (auto& p : out) {
            if (p.unsupported.empty() && p.error.empty()) {
                p.error = "jit: " + error;
            }
        }
        return out;
    }
    if (dump && ir) {
        *ir = impl->jit->LastIr();
    }
    for (size_t i = 0; i < batch.size(); ++i) {
        if (names[i].empty()) {
            continue;
        }
        void* fn = impl->jit->Lookup(names[i], &error);
        if (!fn) {
            out[i].error = "jit lookup: " + error;
            continue;
        }
        out[i].fn = reinterpret_cast<void (*)(dm_guest_state*)>(fn);
    }
    return out;
}

void* Lifted::Continuation(const Vector& v, uint64_t pc, uint32_t fpcr, std::vector<int>* issues, std::string* error) {
    const auto key = std::make_tuple(v.code, pc, fpcr & lift::kFpcrModeMask);
    const auto it = impl->continuations.find(key);
    if (it != impl->continuations.end()) {
        issues->insert(issues->end(), it->second.second.begin(), it->second.second.end());
        return it->second.first;
    }
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = impl->jit->NewModule(*context, "nxaot_cont_" + std::to_string(impl->counter));
    lift::Region region;
    region.entry = pc;
    region.lo = kCodeBase;
    region.hi = kCodeBase + 4 * v.code.size();
    const decode::CodeReader read = [&v](uint64_t vaddr) -> std::optional<uint32_t> {
        if (!InCode(v, vaddr)) {
            return std::nullopt;
        }
        return v.code[(vaddr - kCodeBase) / 4];
    };
    lift::LiftOptions options;
    options.fpcr = fpcr;
    options.fpsr_flags = impl->fpsr_flags;
    options.nan_mode = impl->nan_mode;
    options.target = impl->target;
    const std::string name = "c" + std::to_string(impl->counter++);
    lift::Lifted l = lift::LiftRegion(*module, name, region, read, options);
    if (!l.function) {
        *error = l.error.empty() ? "continuation refused" : l.error;
        return nullptr;
    }
    if (!impl->jit->Add(std::move(module), std::move(context), error)) {
        return nullptr;
    }
    void* fn = impl->jit->Lookup(name, error);
    impl->continuations.emplace(key, std::make_pair(fn, l.dynarmic_issues));
    issues->insert(issues->end(), l.dynarmic_issues.begin(), l.dynarmic_issues.end());
    return fn;
}

Outcome Lifted::Run(const Vector& v, const Prepared& p, const State& init, const std::vector<uint8_t>& image) {
    alignas(64) static dm_guest_state st;
    std::memset(&st, 0, sizeof st);
    for (size_t i = 0; i < 31; ++i) {
        st.x[i] = init.x[i];
    }
    st.sp = init.sp;
    st.pc = init.pc;
    st.nzcv = init.nzcv & 0xF0000000u;
    st.fpcr = init.fpcr & lift::kFpcrMask;
    st.fpsr = init.fpsr & lift::kFpsrMask;
    for (size_t i = 0; i < 32; ++i) {
        std::memcpy(st.v[i], init.v[i].data(), 16);
    }
    st.tpidr_el0 = init.tpidr;
    st.tpidrro_el0 = init.tpidrro;
    st.cnt = init.cnt;
    std::memcpy(impl->image, image.data(), kImageSize);

    Outcome o;
    g_fault_signal = 0;
    const unsigned host_mxcsr = _mm_getcsr();
    void (*fn)(dm_guest_state*) = p.fn;
    unsigned hops = 0;
    if (sigsetjmp(g_jump, 1) == 0) {
        g_in_lifted = 1;
        alarm(kRunSeconds);
        while (true) {
            // What the host does at a gate into guest code: MXCSR from FPCR, and afterwards its flags into FPSR (section 7.1, 7.4)
            _mm_setcsr(lift::GuestMxcsr(st.fpcr));
            fn(&st);
            const unsigned m = _mm_getcsr();
            _mm_setcsr(host_mxcsr);
            st.fpsr |= lift::FpsrFromMxcsr(m);
            if (st.exit_reason != DM_EXIT_NONE || !InCode(v, st.pc) || ++hops > kMaxContinuations) {
                break;
            }
            std::string error;
            void* next = Continuation(v, st.pc, st.fpcr, &o.issues, &error);
            if (!next) {
                o.failure = "lifted code: no continuation at " + std::to_string(st.pc) + ": " + error;
                break;
            }
            fn = reinterpret_cast<void (*)(dm_guest_state*)>(next);
        }
        alarm(0);
        g_in_lifted = 0;
    } else {
        _mm_setcsr(host_mxcsr);
        alarm(0);
        g_in_lifted = 0;
        char buf[96];
        if (g_fault_signal == SIGALRM) {
            std::snprintf(buf, sizeof buf, "lifted code: still running after %u s", kRunSeconds);
        } else {
            std::snprintf(buf, sizeof buf, "lifted code: signal %d at address %p", static_cast<int>(g_fault_signal), g_fault_addr);
        }
        o.failure = buf;
    }
    for (size_t i = 0; i < 31; ++i) {
        o.s.x[i] = st.x[i];
    }
    o.s.sp = st.sp;
    o.s.pc = st.pc;
    o.s.nzcv = st.nzcv;
    o.s.fpcr = st.fpcr;
    o.s.fpsr = st.fpsr;
    for (size_t i = 0; i < 32; ++i) {
        std::memcpy(o.s.v[i].data(), st.v[i], 16);
    }
    o.s.tpidr = st.tpidr_el0;
    o.s.tpidrro = st.tpidrro_el0;
    o.s.cnt = st.cnt;
    o.reason = st.exit_reason;
    o.arg = st.exit_arg;
    o.image.assign(impl->image, impl->image + kImageSize);
    return o;
}

}  // namespace nxaot::vec
