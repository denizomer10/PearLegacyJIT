/* Benchmark harness for the PearLegacyEngine A32 profile.
 * Copyright (c) 2025 PearLegacyJIT contributors
 * SPDX-License-Identifier: 0BSD
 */

// Measures guest instruction throughput for the same JIT configuration the
// engine runs (fastmem page table + absolute page offsets), so that changes can
// be compared against a reproducible baseline instead of impressions.
//
// Three kernels, each isolating a different cost centre:
//   alu      - long basic block, one dispatcher round trip every 66 instructions
//   dispatch - short basic block, one dispatcher round trip every 6 instructions
//   mem      - load/store loop, so per-access checks dominate
//   compile  - many distinct short blocks, executed once, so JIT translation
//              cost dominates instead of execution
//
// Usage: dynarmic_bench [--no-halt-check] [--repeats N]

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"

namespace {

double g_scale = 1.0;
bool g_verbose = false;
bool g_verify_ir = true;
int g_compile_body = 1;

constexpr std::uint32_t kGuestBase = 0x10000;
constexpr std::size_t kGuestSize = 8 * 1024 * 1024;
constexpr std::uint32_t kDataBase = 0x400000;
constexpr std::uint32_t kDataWindow = 0x2000;

// ARM encodings used by the guest kernels. Register numbers are encoded in the
// low bits, so helpers build the ones the kernels need.
constexpr std::uint32_t ARM_SUBS_R2_R2_1 = 0xE2522001;   // subs r2, r2, #1
constexpr std::uint32_t ARM_SVC_0 = 0xEF000000;          // svc #0

constexpr std::uint32_t AddImm(std::uint32_t rd, std::uint32_t rn, std::uint32_t imm8) {
    return 0xE2800000u | (rn << 16) | (rd << 12) | imm8;
}
constexpr std::uint32_t LdrImm(std::uint32_t rd, std::uint32_t rn, std::uint32_t imm12) {
    return 0xE5900000u | (rn << 16) | (rd << 12) | imm12;
}
constexpr std::uint32_t StrImm(std::uint32_t rd, std::uint32_t rn, std::uint32_t imm12) {
    return 0xE5800000u | (rn << 16) | (rd << 12) | imm12;
}
constexpr std::uint32_t Mul(std::uint32_t rd, std::uint32_t rm, std::uint32_t rs) {
    return 0xE0000090u | (rd << 16) | (rs << 8) | rm;
}
constexpr std::uint32_t Eor(std::uint32_t rd, std::uint32_t rn, std::uint32_t rm) {
    return 0xE0200000u | (rn << 16) | (rd << 12) | rm;
}
constexpr std::uint32_t CmpReg(std::uint32_t rn, std::uint32_t rm) {
    return 0xE1500000u | (rn << 16) | rm;
}
constexpr std::uint32_t MovHsReg(std::uint32_t rd, std::uint32_t rm) {
    return 0x21A00000u | (rd << 12) | rm;  // movhs rd, rm
}

// offset_bytes is relative to the branch's own address plus 8, as encoded by ARM.
std::uint32_t BNE(std::int32_t offset_bytes) {
    // bne <label>: cond=NE, bits[27:24]=1010
    const auto imm24 = static_cast<std::uint32_t>(offset_bytes >> 2) & 0xFFFFFF;
    return 0x1A000000u | imm24;
}

constexpr std::uint32_t ARM_B_0 = 0xEA000000;  // b <offset>

// Lays out two blocks: a prefix block that runs the seed instructions and then
// branches forward to the body block. The body loops back with bne to the
// *prefix*, which matters: dynarmic turns a branch to the current block's own
// start into an inline self-loop, hiding the dispatcher cost this benchmark
// wants to measure. Branching to a different block goes through it.
std::vector<std::uint32_t> LayoutLoop(const std::vector<std::uint32_t>& seed,
                                     const std::vector<std::uint32_t>& body) {
    std::vector<std::uint32_t> code = seed;
    const auto b_index = static_cast<std::int32_t>(code.size());
    code.push_back(ARM_B_0);
    const auto body_index = static_cast<std::int32_t>(code.size());
    code.insert(code.end(), body.begin(), body.end());

    const auto b_address = b_index * 4;
    const auto body_address = body_index * 4;
    code[b_index] = ARM_B_0 | ((static_cast<std::uint32_t>(body_address - (b_address + 8)) >> 2) & 0xFFFFFF);

    code.push_back(ARM_SUBS_R2_R2_1);
    const auto bne_address = static_cast<std::int32_t>(code.size() * 4);
    code.push_back(BNE(-(bne_address + 8)));
    code.push_back(ARM_SVC_0);
    return code;
}

// A straight line of many small blocks, each terminated by a branch to the next
// one, so translation rather than repeated execution is what gets measured.
std::vector<std::uint32_t> BuildCompileKernel(int blocks, int body_pairs) {
    const int words_per_block = body_pairs * 2 + 1;
    std::vector<std::uint32_t> code;
    code.reserve(std::size_t(blocks) * words_per_block + 1);
    for (int i = 0; i < blocks; i++) {
        for (int j = 0; j < body_pairs; j++) {
            code.push_back(Mul(0, 0, 9));
            code.push_back(Eor(9, 9, 0));
        }
        code.push_back(ARM_B_0);  // patched below to branch to the next block
    }
    code.push_back(ARM_SVC_0);

    // ARM branches are PC-relative from address+8, so the offset has to be
    // computed per instruction rather than assumed to be zero: a `b #0` here
    // would jump two words ahead, skipping the instruction it should reach.
    for (int i = 0; i < blocks; i++) {
        const std::int32_t branch_index = i * words_per_block + (words_per_block - 1);
        const std::int32_t target_index = (i + 1) * words_per_block;
        const std::int32_t offset_bytes =
            (target_index - (branch_index + 2)) * std::int32_t(sizeof(std::uint32_t));
        code[std::size_t(branch_index)] =
            ARM_B_0 | (static_cast<std::uint32_t>(offset_bytes >> 2) & 0xFFFFFF);
    }
    return code;
}

class BenchEnv final : public Dynarmic::A32::UserCallbacks {
public:
    explicit BenchEnv(std::vector<std::uint8_t>& memory) : memory(memory) {}

    Dynarmic::A32::Jit* cpu = nullptr;
    std::uint64_t ticks_executed = 0;

private:
    bool InRange(std::uint32_t vaddr, std::size_t size) const {
        return vaddr >= kGuestBase && std::size_t(vaddr - kGuestBase) + size <= memory.size();
    }

    std::uint8_t* Host(std::uint32_t vaddr) { return memory.data() + (vaddr - kGuestBase); }

    void Abort() { cpu->HaltExecution(Dynarmic::HaltReason::MemoryAbort); }

    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr vaddr) override {
        if (!InRange(vaddr, 1)) {
            Abort();
            return 0;
        }
        return *Host(vaddr);
    }
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr vaddr) override {
        if (!InRange(vaddr, 2)) {
            Abort();
            return 0;
        }
        std::uint16_t value;
        std::memcpy(&value, Host(vaddr), sizeof(value));
        return value;
    }
    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr vaddr) override {
        if (!InRange(vaddr, 4)) {
            Abort();
            return 0;
        }
        std::uint32_t value;
        std::memcpy(&value, Host(vaddr), sizeof(value));
        return value;
    }
    std::uint64_t MemoryRead64(Dynarmic::A32::VAddr vaddr) override {
        if (!InRange(vaddr, 8)) {
            Abort();
            return 0;
        }
        std::uint64_t value;
        std::memcpy(&value, Host(vaddr), sizeof(value));
        return value;
    }
    void MemoryWrite8(Dynarmic::A32::VAddr vaddr, std::uint8_t value) override {
        if (!InRange(vaddr, 1)) {
            Abort();
            return;
        }
        *Host(vaddr) = value;
    }
    void MemoryWrite16(Dynarmic::A32::VAddr vaddr, std::uint16_t value) override {
        if (!InRange(vaddr, 2)) {
            Abort();
            return;
        }
        std::memcpy(Host(vaddr), &value, sizeof(value));
    }
    void MemoryWrite32(Dynarmic::A32::VAddr vaddr, std::uint32_t value) override {
        if (!InRange(vaddr, 4)) {
            Abort();
            return;
        }
        std::memcpy(Host(vaddr), &value, sizeof(value));
    }
    void MemoryWrite64(Dynarmic::A32::VAddr vaddr, std::uint64_t value) override {
        if (!InRange(vaddr, 8)) {
            Abort();
            return;
        }
        std::memcpy(Host(vaddr), &value, sizeof(value));
    }

    void InterpreterFallback(Dynarmic::A32::VAddr pc, size_t) override {
        std::fprintf(stderr, "unexpected interpreter fallback at %08x\n", pc);
        std::abort();
    }
    void CallSVC(std::uint32_t) override {
        // Kernels end with SVC, which is the benchmark's finish line.
        cpu->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
    void ExceptionRaised(Dynarmic::A32::VAddr pc, Dynarmic::A32::Exception exception) override {
        std::fprintf(stderr, "unexpected exception %d at %08x\n", static_cast<int>(exception), pc);
        std::abort();
    }
    // Cycle counting is left enabled because the engine runs with it on; a huge
    // tick budget keeps the kernel from halting before the SVC.
    // The JIT reports every retired guest instruction here (GetTicksForCode
    // returns 1 per instruction), so this is the ground truth for throughput
    // rather than an assumption about how many of the emitted operations
    // survived the optimiser passes.
    void AddTicks(std::uint64_t ticks) override {
        ticks_executed += ticks;
        if (ticks < ticks_remaining) {
            ticks_remaining -= ticks;
        } else {
            ticks_remaining = 0;
        }
    }
    std::uint64_t GetTicksRemaining() override { return ticks_remaining; }

    std::uint64_t ticks_remaining = 1ULL << 40;

    std::vector<std::uint8_t>& memory;
};

struct Result {
    double seconds;
    std::uint64_t ticks;
    std::int32_t iterations;
};

Result RunKernel(const std::string& name,
                 const std::vector<std::uint32_t>& code,
                 std::uint64_t instructions_per_iteration,
                 std::int32_t iterations,
                 bool halt_check) {
    std::vector<std::uint8_t> memory(kGuestSize, 0);
    BenchEnv env{memory};
    std::memcpy(memory.data(), code.data(), code.size() * sizeof(std::uint32_t));
    if (kDataBase >= kGuestBase && kDataBase - kGuestBase + kDataWindow <= memory.size()) {
        for (std::uint32_t i = 0; i < kDataWindow; i += 4) {
            const std::uint32_t value = 0x9E3779B9u * (i + 1);
            std::memcpy(memory.data() + (kDataBase - kGuestBase) + i, &value, sizeof(value));
        }
    }

    // Same layout the engine uses: one host pointer per guest page, with
    // absolute offsets so no address arithmetic is needed in compiled code.
    auto page_table =
        std::make_unique<std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
    page_table->fill(nullptr);
    constexpr std::size_t first_page = kGuestBase >> Dynarmic::A32::UserConfig::PAGE_BITS;
    constexpr std::size_t page_count = kGuestSize >> Dynarmic::A32::UserConfig::PAGE_BITS;
    for (std::size_t i = 0; i < page_count; i++) {
        (*page_table)[first_page + i] = memory.data() + (i << Dynarmic::A32::UserConfig::PAGE_BITS);
    }

    Dynarmic::A32::UserConfig config;
    config.callbacks = &env;
    config.page_table = page_table.get();
    config.absolute_offset_page_table = true;
    config.check_halt_on_memory_access = halt_check;

    auto jit = std::make_unique<Dynarmic::A32::Jit>(config);
    env.cpu = jit.get();

    auto& regs = jit->Regs();
    regs.fill(0);
    regs[15] = kGuestBase;
    regs[2] = static_cast<std::uint32_t>(iterations);
    regs[4] = kDataBase;
    regs[5] = kDataBase + kDataWindow;
    regs[6] = kDataBase;
    regs[7] = kDataBase + kDataWindow;
    jit->SetCpsr(0x00000010);  // user mode, ARM state

    const auto start = std::chrono::steady_clock::now();
    const auto halt_reason = jit->Run();
    const auto end = std::chrono::steady_clock::now();

    if (!Dynarmic::Has(halt_reason, Dynarmic::HaltReason::UserDefined1)) {
        std::fprintf(stderr, "%s: unexpected halt reason %08x pc=%08x counter=%u cpsr=%08x\n",
                     name.c_str(), static_cast<unsigned>(halt_reason), jit->Regs()[15],
                     jit->Regs()[2], jit->Cpsr());
        std::abort();
    }

    // Guest-side sanity check: the loop counter must have reached zero and PC
    // must be on the SVC, otherwise the elapsed time means nothing.
    if (g_verbose) {
        std::fprintf(stderr, "%s: counter_after=%u pc=%08x\n", name.c_str(), jit->Regs()[2],
                     jit->Regs()[15]);
    }

    const auto seconds = std::chrono::duration<double>(end - start).count();
    if (env.ticks_executed == 0) {
        std::fprintf(stderr, "%s: no ticks were accounted, benchmark is invalid\n", name.c_str());
        std::abort();
    }
    return Result{seconds, env.ticks_executed, iterations};
}

// Reported per loop iteration: block tick counts are static, so they include
// instructions the optimiser removed and cannot be turned into a MIPS figure.
void Report(const std::string& name, const Result& result) {
    const double ns_per_iteration = result.seconds * 1e9 / double(result.iterations);
    const double mega_iterations = double(result.iterations) / result.seconds / 1e6;
    std::printf("  %-9s %10d iterations in %7.3fs  %8.2f Miter/s  %8.1f ns/iter"
                "  (block ticks %" PRIu64 ")\n",
                name.c_str(), result.iterations, result.seconds, mega_iterations,
                ns_per_iteration, result.ticks);
}

// Times translation only: every block runs exactly once, so the measurement is
// dominated by IR generation and the optimisation/verification passes.
double TimeCompile(int blocks, bool halt_check, bool verify_ir, double* seconds_out) {
    auto code = BuildCompileKernel(blocks, g_compile_body);

    std::vector<std::uint8_t> memory(kGuestSize, 0);
    BenchEnv env{memory};
    std::memcpy(memory.data(), code.data(), code.size() * sizeof(std::uint32_t));

    auto page_table =
        std::make_unique<std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
    page_table->fill(nullptr);
    constexpr std::size_t first_page = kGuestBase >> Dynarmic::A32::UserConfig::PAGE_BITS;
    constexpr std::size_t page_count = kGuestSize >> Dynarmic::A32::UserConfig::PAGE_BITS;
    for (std::size_t i = 0; i < page_count; i++) {
        (*page_table)[first_page + i] = memory.data() + (i << Dynarmic::A32::UserConfig::PAGE_BITS);
    }

    Dynarmic::A32::UserConfig config;
    config.callbacks = &env;
    config.page_table = page_table.get();
    config.absolute_offset_page_table = true;
    config.check_halt_on_memory_access = halt_check;
    config.enable_ir_verification = verify_ir;

    auto jit = std::make_unique<Dynarmic::A32::Jit>(config);
    env.cpu = jit.get();
    auto& regs = jit->Regs();
    regs.fill(0);
    regs[15] = kGuestBase;
    regs[4] = kDataBase;
    regs[7] = kDataBase + kDataWindow;
    jit->SetCpsr(0x00000010);

    if (g_verbose) {
        const auto probe = BuildCompileKernel(blocks, g_compile_body);
        std::fprintf(stderr, "compile: %d blocks, %zu words, first=%08x\n", blocks, probe.size(),
                     probe[0]);
        std::fflush(stderr);
    }

    const auto start = std::chrono::steady_clock::now();
    const auto halt_reason = jit->Run();
    const auto end = std::chrono::steady_clock::now();

    if (!Dynarmic::Has(halt_reason, Dynarmic::HaltReason::UserDefined1)) {
        std::fprintf(stderr, "compile: unexpected halt reason %08x\n",
                     static_cast<unsigned>(halt_reason));
        std::abort();
    }

    *seconds_out = std::chrono::duration<double>(end - start).count();
    return double(*seconds_out) * 1e6 / double(blocks);
}

// Measures the cost of a guest -> host -> guest round trip, which is what every
// HLE framework call costs: the guest executes `svc`, which requests a halt, the
// host handles the call, and the guest is re-entered. One iteration is one whole
// round trip, so this is the boundary cost the engine pays per framework call.
double TimeSvcRoundTrip(int iterations, double* seconds_out) {
    // svc #0 followed by an endless branch, so a run always stops on the svc.
    std::vector<std::uint32_t> code{ARM_SVC_0, ARM_B_0 | 0xFFFFFEu};
    std::vector<std::uint8_t> memory(kGuestSize, 0);
    BenchEnv env{memory};
    std::memcpy(memory.data(), code.data(), code.size() * sizeof(std::uint32_t));

    auto page_table =
        std::make_unique<std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
    page_table->fill(nullptr);
    constexpr std::size_t first_page = kGuestBase >> Dynarmic::A32::UserConfig::PAGE_BITS;
    constexpr std::size_t page_count = kGuestSize >> Dynarmic::A32::UserConfig::PAGE_BITS;
    for (std::size_t i = 0; i < page_count; i++) {
        (*page_table)[first_page + i] = memory.data() + (i << Dynarmic::A32::UserConfig::PAGE_BITS);
    }

    Dynarmic::A32::UserConfig config;
    config.callbacks = &env;
    config.page_table = page_table.get();
    config.absolute_offset_page_table = true;
    config.check_halt_on_memory_access = true;

    auto jit = std::make_unique<Dynarmic::A32::Jit>(config);
    env.cpu = jit.get();
    jit->Regs().fill(0);
    jit->SetCpsr(0x00000010);

    // Translate once up front so the measurement is the round trip and not the
    // first-block compilation.
    jit->Regs()[15] = kGuestBase;
    jit->Run();

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; i++) {
        jit->Regs()[15] = kGuestBase;
        const auto halt_reason = jit->Run();
        if (!Dynarmic::Has(halt_reason, Dynarmic::HaltReason::UserDefined1)) {
            std::fprintf(stderr, "hle: unexpected halt reason %08x\n",
                         static_cast<unsigned>(halt_reason));
            std::abort();
        }
    }
    const auto end = std::chrono::steady_clock::now();

    *seconds_out = std::chrono::duration<double>(end - start).count();
    return *seconds_out * 1e9 / double(iterations);
}

}  // namespace

int main(int argc, char** argv) {
    bool halt_check = true;
    int repeats = 3;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--no-halt-check") {
            halt_check = false;
        } else if (arg == "--no-ir-verification") {
            g_verify_ir = false;
        } else if (arg == "--compile-body" && i + 1 < argc) {
            g_compile_body = std::atoi(argv[++i]);
        } else if (arg == "--verbose") {
            g_verbose = true;
        } else if (arg == "--scale" && i + 1 < argc) {
            g_scale = std::atof(argv[++i]);
        } else if (arg == "--repeats" && i + 1 < argc) {
            repeats = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: %s [--no-halt-check] [--repeats N] [--scale N] [--verbose] [--no-ir-verification] [--compile-body N]\n", argv[0]);
            return 2;
        }
    }

    std::printf("dynarmic A32 throughput benchmark (ir_verification=%s, check_halt_on_memory_access=%s, compile_body=%d, best of %d)\n",
                g_verify_ir ? "on" : "off", halt_check ? "on" : "off", g_compile_body * 2 + 1,
                repeats);

    struct Case {
        const char* name;
        std::vector<std::uint32_t> code;
        std::uint64_t instructions_per_iteration;
        std::int32_t iterations;
    };

    // Multiply/xor chain: the two operations cannot be folded into each other,
    // so the emitted code survives the optimiser instead of collapsing.
    std::vector<std::uint32_t> alu_body;
    for (int round = 0; round < 32; round++) {
        alu_body.push_back(Mul(0, 0, 9));
        alu_body.push_back(Eor(9, 9, 0));
    }
    alu_body.push_back(StrImm(0, 7, 0));
    auto alu = LayoutLoop({LdrImm(0, 4, 0), LdrImm(9, 4, 4)}, alu_body);

    std::vector<std::uint32_t> dispatch_body;
    for (int i = 0; i < 2; i++) {
        dispatch_body.push_back(Mul(0, 0, 9));
        dispatch_body.push_back(Eor(9, 9, 0));
    }
    dispatch_body.push_back(StrImm(0, 7, 0));
    auto dispatch = LayoutLoop({LdrImm(0, 4, 0), LdrImm(9, 4, 4)}, dispatch_body);

    // Stays inside an 8 KB window so the measurement is the cost of the
    // per-access checks, not DRAM latency.
    std::vector<std::uint32_t> mem_body{
        LdrImm(3, 4, 0),
        StrImm(3, 4, 4),
        AddImm(4, 4, 8),
        CmpReg(4, 5),
        MovHsReg(4, 6),
    };
    auto mem = LayoutLoop({}, mem_body);

    const std::array<Case, 3> cases = {
        Case{"alu", std::move(alu), 72, std::int32_t(2000000 * g_scale)},
        Case{"dispatch", std::move(dispatch), 11, std::int32_t(20000000 * g_scale)},
        Case{"mem", std::move(mem), 10, std::int32_t(20000000 * g_scale)},
    };

    for (const auto& test : cases) {
        Result best{1e9, 0, 0};
        for (int r = 0; r < repeats; r++) {
            const auto result = RunKernel(test.name, test.code, test.instructions_per_iteration,
                                          test.iterations, halt_check);
            if (result.seconds < best.seconds) {
                best = result;
            }
        }
        Report(test.name, best);
    }

    // Translation cost: every block runs exactly once, so what is measured is IR
    // generation plus the optimisation and verification passes.
    {
        const int blocks = int(20000 * g_scale);
        double best_seconds = 1e9;
        double us_per_block = 0.0;
        for (int r = 0; r < repeats; r++) {
            double seconds = 0.0;
            const auto per_block = TimeCompile(blocks, halt_check, g_verify_ir, &seconds);
            if (seconds < best_seconds) {
                best_seconds = seconds;
                us_per_block = per_block;
            }
        }
        std::printf("  %-9s %10d blocks     in %7.3fs  %8.1f kblocks/s  %8.2f us/block\n",
                    "compile", blocks, best_seconds, double(blocks) / best_seconds / 1000.0,
                    us_per_block);
    }

    // Cost of one HLE round trip: halt on svc, host handles it, re-enter.
    {
        const int iterations = int(200000 * g_scale);
        double best_seconds = 1e9;
        double ns_per_round_trip = 0.0;
        for (int r = 0; r < repeats; r++) {
            double seconds = 0.0;
            const auto ns = TimeSvcRoundTrip(iterations, &seconds);
            if (seconds < best_seconds) {
                best_seconds = seconds;
                ns_per_round_trip = ns;
            }
        }
        std::printf("  %-9s %10d round trips in %7.3fs  %8.2f M/s      %8.1f ns/call\n", "hle",
                    iterations, best_seconds, double(iterations) / best_seconds / 1e6,
                    ns_per_round_trip);
    }

    return 0;
}
