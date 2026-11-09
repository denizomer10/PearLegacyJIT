# PearLegacyJIT

A Dynarmic-based dynamic recompiler, specialized for **PearLegacyEngine** on
Apple Silicon.

PearLegacyJIT is a fork of [Dynarmic](https://github.com/MerryMage/dynarmic),
the ARM dynamic recompiler written by **merryhime** (`MerryMage`,
<git@mary.rs>) and contributors. All of the groundwork — the A32 frontend, the
IR and its optimization passes, the AArch64 backend, the block cache, the
fastmem machinery and the test suite — is Dynarmic's work, licensed under the
0BSD-style terms reproduced in [LICENSE.txt](LICENSE.txt). Changes in this fork
are limited to the build profile, tuning and integration described below. It is
not a rewrite, and it does not claim authorship of the underlying translator.

```
Original project:  https://github.com/MerryMage/dynarmic   (merryhime)
Direct base:       https://github.com/touchHLE/dynarmic     (touchHLE)
```

## Why this fork exists

PearLegacyEngine runs 32-bit iOS applications (Miku Flick, Miku Flick/02,
Asphalt 6, …) on Apple Silicon Macs running macOS 26 or later. Their ARMv7 and
Thumb-2 guest code is executed by Dynarmic, so every microsecond of translation
overhead, every guest memory access and every guest-to-host transition shows up
directly as frame-time jitter in those games.

The mission of this fork is narrow and measurable:

> Optimize Dynarmic for ARMv7/Thumb-2 guest code running on Apple Silicon under
> PearLegacyEngine, without changing guest-visible behaviour.

Correctness first, performance second, maintainability third. Guest CPU
semantics, exception behaviour, memory ordering and floating-point behaviour are
never traded for benchmark scores.

## What this fork is built from

| Item | Value |
| --- | --- |
| Upstream project | MerryMage/dynarmic, version 6.7.0 |
| Direct base | touchHLE/dynarmic master (`e0f6bd9d`) |
| Guest support needed | ARMv7-A and Thumb-2 (A32 frontend), VFP/NEON |
| Host architecture | AArch64 (Apple Silicon) only |
| Minimum OS | macOS 26.0 |
| Guest 64-bit support | not built |

The direct base is touchHLE/dynarmic rather than MerryMage/dynarmic master
because touchHLE/dynarmic is a few commits ahead (bundled `fmt` 12.0.0, which
modern Apple Clang requires, and an `mcl::list_sequence` fix for clang 20).
Those commits are already part of what PearLegacyEngine links against, so the
fork tracks them instead of diverging.

## Usage

This repository is not used on its own. It is consumed as the `dynarmic` source
tree of PearLegacyEngine:

```sh
# In PearLegacyEngine:
git clone https://github.com/denizomer10/PearLegacyJIT vendor/dynarmic
cargo build --release
```

`src/cpu/dynarmic_wrapper/build.rs` configures this tree with CMake, passing
`DYNARMIC_FRONTENDS=A32`, and links it as a static library because Dynarmic
cannot be dynamically linked. Set `PEAR_DYNARMIC_ROOT` to build against a
checkout somewhere other than `vendor/dynarmic`.

### Build profile: `pearlegacy-apple-silicon`

`CMakePresets.json` contains the profile PearLegacyEngine builds:

```sh
cmake --preset pearlegacy-apple-silicon \
    -DBoost_INCLUDE_DIR="$(brew --prefix boost)/include"
cmake --build --preset pearlegacy-apple-silicon
```

The non-negotiable parts of the profile:

* `CMAKE_OSX_ARCHITECTURES=arm64` — no x86_64, no fat binaries.
* `CMAKE_OSX_DEPLOYMENT_TARGET=26.0` — macOS 26 or later.
* `DYNARMIC_FRONTENDS=A32` — the A64 guest frontend is not compiled at all.
* `DYNARMIC_TESTS=OFF` — the tests have their own preset.
* `DYNARMIC_USE_BUNDLED_EXTERNALS=ON` — self-contained, reproducible build.

A matching `pearlegacy-apple-silicon-tests` preset enables the upstream
correctness tests, for differential testing after an optimization.

Verified output of the profile:

```
$ lipo -archs build-pearlegacy-apple-silicon/src/dynarmic/libdynarmic.a
arm64
$ otool -l … | grep minos
    minos 26.0
```

### macOS fast memory access

On macOS, Dynarmic's fastmem support depends on `mach/mach_exc.defs` being
found at configure time. If it is not found, CMake silently falls back to the
generic exception handler and guest memory access becomes dramatically slower.
The profile is only correct if configuration prints:

```
-- mach/mach_exc.defs location: …/Kernel.framework/Headers
```

The `mig`-generated Mach exception stubs are written into
`src/dynarmic/backend/*/mig/` by CMake, so a fresh clone has none until it has
been configured once. They are build output, not source.

## Execution architecture

Dynarmic translates guest ARMv7/Thumb-2 code to AArch64 in blocks, ahead of
execution, and caches the result:

```
guest code ──► A32 frontend ──► IR ──► optimization passes ──► AArch64 backend
   │                                                                   │
   │                                                       block cache (JIT arena)
   │                                                                   │
   └── page table + fastmem ────────────────────────────────► compiled block
```

The parts that matter most to PearLegacyEngine:

* **Guest memory access.** With fastmem active, guest loads and stores use host
  memory directly through a page table; a fault is caught by the macOS Mach
  exception handler and resolved through `Memory::Read`/`Write` callbacks.
  Without fastmem, *every* access is a callback.
* **Block cache.** Blocks are looked up by guest address in a hash map. Too
  small an arena means recompilation; too large an arena means worse code
  locality.
* **HLE transitions.** PearLegacyEngine's iOS frameworks are host functions
  called from translated guest code, so the cost of saving and restoring guest
  registers at that boundary is paid on every framework call — and games call
  frameworks constantly, every frame, touch and buffer swap.
* **`MAP_JIT` and instruction-cache coherency.** The JIT arena is written and
  then executed, which on Apple Silicon requires `MAP_JIT`, correct
  `pthread_jit_write_protect_np` transitions and instruction-cache
  invalidation.

## Benchmarking and validation rules

No claim in this repository is valid without a measured number and a way to
reproduce it. Every optimization follows the same loop:

1. Reproduce and measure the current behaviour.
2. State a hypothesis naming the specific cost.
3. Make the smallest change that tests the hypothesis.
4. Build with `pearlegacy-apple-silicon`.
5. Run `dynarmic_tests` — a performance win that breaks a test is reverted.
6. Measure again with the same methodology.
7. Keep the change only if it improves; record what was measured.
8. Check in-game behaviour in Miku Flick/02: frame pacing, audio sync, input.

Correctness gate for every change:

```sh
cmake --preset pearlegacy-apple-silicon-tests \
    -DBoost_INCLUDE_DIR="$(brew --prefix boost)/include"
cmake --build --preset pearlegacy-apple-silicon-tests
./build-pearlegacy-apple-silicon-tests/tests/dynarmic_tests
```

Current baseline of that gate on Apple Silicon:

```
All tests passed (200561 assertions in 47 test cases)
```

PearLegacyEngine's own runtime checks are the only valid evidence for in-game
results:

* `PEAR_PRESENT_STATS=1` — present-time average, worst case, frames over 33 ms.
* `PEAR_CONTENT_SELFTEST=1` — verifies the frame is really drawn, not black.
* `PEAR_TOUCH_SCRIPT=<file>` — replays scripted input so runs are repeatable.

### Running the benchmark

```sh
cmake --preset pearlegacy-apple-silicon-bench \
    -DBoost_INCLUDE_DIR="$(brew --prefix boost)/include"
cmake --build --preset pearlegacy-apple-silicon-bench
./build-pearlegacy-apple-silicon-bench/benchmarks/dynarmic_bench
```

Useful flags: `--repeats N` (default 3, best run wins), `--scale X` (multiplies
iteration and block counts), `--compile-body N` (mul/eor pairs per compiled
block, so translation can be measured on realistically sized blocks),
`--no-ir-verification`, `--no-halt-check`, `--verbose`.

### Baseline to compare against

The reference for every comparison is the same source tree configured with
upstream-compatible settings, on the same machine with the same guest workload,
so that only the optimization under test differs. Values are added to the table
when an actual measurement exists; an empty cell means "not measured yet", not
"no improvement".

| Measure | Baseline | Optimized | Status |
| --- | --- | --- | --- |
| A32 execution, ALU chain (72 guest insns/iter) | 8.5 ns/iter | — | measured |
| A32 execution, block dispatch (11 guest insns/iter) | 1.3 ns/iter | — | measured (blocked linked, not optimized) |
| A32 execution, guest memory loop (10 guest insns/iter) | 1.3 ns/iter | — | measured |
| Block compilation latency, 3-instruction block | 4.59 µs/block | 4.42 µs/block | 3.7% with IR verification off |
| Block compilation latency, 33-instruction block | 28.26 µs/block | 26.70 µs/block | 5.5% with IR verification off |
| `check_halt_on_memory_access` cost | 1.3 ns/iter | 1.5 ns/iter | no benefit when off — see below |
| HLE round trip (guest `svc` -> host -> re-enter) | 12.7 ns/call | — | measured |
| Block cache hit rate | — | — | not measured yet |
| Miku Flick/02 present time | 5905 slow-path frames, up to 332 ms each | avg 0.8 ms, max 1.3 ms | measured in PearLegacyEngine (graphics, not JIT) |

Methodology for the measured rows:

* Machine: Apple A18 Pro, macOS 27.2, arm64, `pearlegacy-apple-silicon` release.
* Harness: `dynarmic_bench`, which links `libdynarmic.a` with the engine's exact
  `UserConfig` (absolute-offset page table, cycle counting on).
* Each row is the best of three runs; `--scale` changes iteration counts and was
  checked to scale linearly so the timings are not first-block artefacts.
* Guest MIPS is *not* reported: `AddTicks` receives the static tick count of the
  translated block, including instructions the optimiser removed, so any MIPS
  figure derived from it would overstate throughput.

Two hypotheses were tested and rejected by measurement, which is why the table
has no "improvement" in those rows:

* **Per-access halt checks.** `check_halt_on_memory_access` sounds like a
  per-access branch, but `EmitA32CheckMemoryAbort` returns immediately when the
  flag is off, and when the flag is on the check is emitted *only* on the
  memory-callback slow path — an in-range access pays nothing. Turning the flag
  off measured 1.5 ns/iter against 1.3 ns/iter on; the difference is noise, so
  the engine keeps the safer setting.
* **HLE callback boundary cost.** A framework call costs the guest a `svc`, a
  halt, host-side handling, and a re-entry. That whole round trip measures
  12.7 ns, so the JIT side of the boundary is not where framework-call time
  goes; any remaining cost is in the handler itself, not in crossing the
  boundary.
* **Block dispatch overhead.** A call plus hash lookup per block exit would be
  the largest architectural cost, but `AddressSpace::block_references` already
  links blocks directly: a branch whose target is already compiled is patched
  into a direct jump. 1.3 ns/iter for a two-block transition confirms the
  dispatcher is not on the hot path. Note that dynarmic also compiles a branch
  to the current block's own start into an inline self-loop, so a naive
  self-loop benchmark measures neither dispatch nor linking.

## Scope: what is intentionally not built

Dynarmic supports far more than PearLegacyEngine needs. The profile leaves out
what a 32-bit iOS app on Apple Silicon cannot reach:

* the A64 guest frontend (`DYNARMIC_FRONTENDS=A32`),
* the x86-64 and RISC-V host backends (not compiled for an arm64 build),
* the unicorn-based fuzzing harness (`DYNARMIC_TESTS_USE_UNICORN=OFF`),
* LLVM-based x86 JIT disassembly (`DYNARMIC_USE_LLVM=OFF`).

Nothing is deleted from the source tree: keeping the upstream layout intact is
what makes it possible to pull upstream fixes and to run the full test suite
when a change looks suspicious.

## Known limits

* The block cache is not designed around one long-running game with a fixed
  working set; arena sizing and recompilation remain open questions.
* macOS fastmem depends on configure-time discovery of `mach_exc.defs`, and the
  fallback is silent. Always check the configure log.
* An adversarial guest can detect that it runs under Dynarmic. That is an
  upstream property, and this fork does not try to hide it.

## Credits

* Dynarmic and its AArch64 backend: **merryhime** and the Dynarmic
  contributors — <https://github.com/MerryMage/dynarmic>.
* The touchHLE/dynarmic commits this fork builds on: the touchHLE project —
  <https://github.com/touchHLE/dynarmic>.
* PearLegacyEngine integration and the `pearlegacy-apple-silicon` profile:
  PearLegacyEngine contributors.

Licensed under the same terms as Dynarmic; see [LICENSE.txt](LICENSE.txt).
