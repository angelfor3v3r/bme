# BME

**Bare-metal machine code viewer.** Paste x86-64 bytes, single-step them in a
sandbox, and watch the registers, flags, and FPU/SSE state change one
instruction at a time.

## Goal

Original idea by [kmx00](https://github.com/kmx00).

BME is built for differential analysis - finding where x86 *decoders* and the *actual CPU* diverge.
A disassembler only maps bytes to a mnemonic. It can't know an instruction's runtime effect, and two
decoders can disagree on the same bytes. Executing on real silicon (and swapping decoders) settles it.

- Some effects exist only at runtime - a segment / privilege check whose result depends on the live descriptor tables and the current privilege level, which no decoder can resolve statically.
- Some encodings sit in reserved NOP space that newer CPUs repurpose, so the same bytes can be a real instruction on one processor and a multi-byte NOP on another.

## Features

- Single-step execution with full GPR, RFLAGS, SSE (XMM), and x87 state after every completed instruction
- Faulting instructions retain and display their exception-time partial register state without counting as executed
- GPR sub-register drill-down (RAX -> EAX -> AX -> AH/AL), seed any level before a run
- x87 FPU and MXCSR shown in detail - each `ST(i)` with its physical `x87rN`, tag, 80-bit raw and value, plus decoded control/status words and MXCSR fields
- XMM shows a decoded `f64x2` view by default, expandable to `f32x4`. x87 has a matching narrowed `Real4` view, showing what an `FSTP m32` would store
- Intel or AT&T disassembly, switchable on the fly
- Reports where and why a run stopped (fault, `int3`, or step cap), and marks skipped code not-reached
- Seed status flags too - click any flag in the Flags panel (CF/PF/AF/ZF/SF/OF/DF)
- Seed XMM and x87 `ST(i)` registers too, as raw hex or a decimal value (`1.5`, optional `f` for single precision or `l` for double precision), from the SSE / x87 panels or `--seed`
- Scratch data buffer one guard page above a fixed reservation. The TUI shows its usable address, which RDI and RSI receive by default (toggle in Settings), or paste it into any seedable register
- Click any register value (GPR, XMM, MXCSR, x87, or an individual float in a drill-down) to copy it to the clipboard
- Keyboard shortcuts. **F5** run, **F8** step, **F7** back
- Headless `--quick` output as a human-readable trace or versioned JSON with full machine state, CPU provenance, dependency revisions, and all decoder histories
- Detects Intel SDE and Pin instrumentation from environment markers. Windows also checks parent processes and loaded modules. Execution is refused because instrumented single-step state cannot be trusted
- History panel has a `Main` tab plus one tab per decoder (`zydis`, `bddisasm`, `capstone`, `xed`). Each backend applies its own instruction boundaries to the original bytes without re-running the code

## Usage

BME is a terminal app - run it with no arguments to open the TUI, then edit the
bytes, **Run** (or **F5**), and **Step** / **Back** (**F8** / **F7**) through the trace. Seed GPR, XMM, and
x87 registers in the left panels and status flags by clicking the Flags panel. **Settings** holds the
disasm syntax, step cap, and whether RDI/RSI point at the scratch data. **About** shows the build.

The sandbox contains common faults and instruction-count runaways, but it is not a security boundary. Windows executes bytes in a host-process thread. Linux uses a traced child process. Executed bytes retain user privileges and may invoke system calls or modify process state. Run only trusted machine code. The step cap is not a wall-clock deadline, so a blocking system call can stall a run. BME refuses execution when it detects Intel SDE or Pin instrumentation.

```sh
bme                                      # open the TUI empty
```

Most options pre-fill TUI state. `--run`, `--quick`, `--format`, `--track`, and `--version` instead control launch or output behavior.

```sh
bme --bytes 48FFC0                       # preload "inc rax", ready to run
bme --bytes 48FFC0 --run                 # preload and run on launch
bme --bytes 48C7C001000000 --syntax att  # preload "mov rax, 1", AT&T syntax
bme --bytes 48FFC0 --backend bddisasm    # decode with bddisasm instead of Zydis
bme --version                            # print build version information
```

- `--bytes <hex>` - x86-64 machine code as hex (whitespace allowed)
- `--run` - run immediately when `--bytes` is supplied. Otherwise open the TUI normally
- `--version` - print the build tag, commit hash, and repository URL
- `--syntax intel|att` - disassembly syntax (default `intel`)
- `--backend zydis|bddisasm|capstone|xed` - x86 decoder backend (default `zydis`, bddisasm is Intel-only)
- `--max-steps N` - instruction cap before a run aborts (default `50000`, maximum `1000000`)
- `--quick` - write the trace to stdout and exit instead of opening the TUI (needs `--bytes`)
- `--format text|json` - `--quick` output format (default `text`). JSON always contains full state, so `--track` cannot be combined with `--format json`
- `--pretty` - use two-space indentation for human-readable JSON. Requires `--quick --format json`
- `--track <classes>` - register classes whose text `--quick` deltas include (`gpr,rip,rflags,xmm,x87` / `all` / `none`, default `gpr,rip,rflags`)
- `--seed <name=value,...>` - initial register state. GPR slices and flags use hexadecimal; zero clears a flag and any nonzero value sets it.
  `XMM0..15` and `ST0..7` accept raw hexadecimal or a decimal containing `.` or spelled `inf`/`nan`, with `f` for single precision and `l` or no
  suffix for double precision. Any GPR slice is supported except `RSP` and its slices. Narrower GPR slices overlay wider ones

```sh
bme --bytes 48FFC0 --quick                                   # dump "inc rax" trace to stdout
bme --bytes 48FFC0 --quick --track all                       # track deltas from every register class
bme --bytes 48F7F3 --quick --seed rax=64,rbx=9               # div rbx with seeded operands (100 / 9)
bme --bytes D8C1 --quick --track x87 --seed st0=2.0,st1=3.0  # fadd st0, st1 -> ST0 = 5
bme --bytes 48FFC0 --quick --format json                     # export schema-versioned JSON to stdout
bme --bytes 48FFC0 --quick --format json --pretty            # export indented JSON for human inspection
```

### JSON trace export

`--format json` writes exactly one RFC 8259 document followed by a newline. Output is compact by default. Add `--pretty` for two-space-indented, human-readable JSON. Schema version 1 records:

- BME version, commit, repository, and compiled dependency versions and revisions
- Capture OS, architecture, and a process-visible CPU fingerprint with raw CPUID records
- The requested bytes, seed, backend, syntax, step cap, and scratch-pointer policy
- The actual seed, every completed or faulted execution event, outcome, message, and stop location
- Native disassembly histories for all four compiled decoder backends, including each effective syntax

Register values, addresses, feature masks, XMM lanes, and x87 values use fixed-width hexadecimal strings so ordinary JSON tooling cannot lose integer precision. Unavailable values use `null`. A fault raised by the supplied bytes is a successful recorded trace and returns exit code 0. An engine or instrumentation failure still emits a complete JSON document with outcome `error`, then returns nonzero. Invalid CLI arguments or byte input emit no JSON, write a diagnostic to stderr, and return nonzero. Serialization or stdout write failures also report to stderr and may leave a partial document on stdout.

```sh
bme --bytes 48FFC0 --quick --format json > trace.json
bme --bytes 48FFC0 --quick --format json --pretty
```

## Build

BME supports native x86-64 Windows and Linux. CMake 3.31 or newer is required. Python 3 is needed to build the bundled XED backend.

### Windows

Use Ninja, MASM, and Clang targeting the MSVC ABI.

```sh
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

### Linux

Use Ninja and a C++23 compiler. CI builds a Debian 12 package with Clang 22. GCC 13 or newer also works.

```sh
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-22 -DCMAKE_CXX_COMPILER=clang++-22
cmake --build build
```

### Formatting

CI is the authoritative formatting check. Contributors can optionally enable the tracked pre-commit hook:

```sh
git config core.hooksPath .githooks
```

Prebuilt binaries and packages are on the [Releases](https://github.com/angelfor3v3r/bme/releases) page.

## Packages

Release builds provide a versioned Windows ZIP containing `bme.exe`, `LICENSE`, and `THIRD_PARTY_LICENSES.md`.

CPack generates a Debian package plus a `.tar.gz` archive on Linux.

```sh
cpack --config build/CPackConfig.cmake
```

Both package formats include BME's license and the bundled third-party license texts under `share/doc/bme`.

## Tests

Unit tests cover BME-owned behavior including parsing, seed composition, CLI handling, CPU fingerprinting, environment handling, instrumentation refusal, bounded engine execution, fault-state capture, versioned JSON serialization, and `--quick` behavior. Decoder correctness remains outside the unit-test contract. Off by default.

```sh
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBME_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## License

MIT © angelfor3v3r (Dexxi) - see [LICENSE](LICENSE).

## Acknowledgments

The distributed `bme` binary includes these open-source libraries:

| Library                                                 | Role                  | License               |
|---------------------------------------------------------|-----------------------|-----------------------|
| [Zydis](https://github.com/zyantific/zydis)             | disassembler backend  | MIT                   |
| [Zycore](https://github.com/zyantific/zycore-c)         | Zydis support library | MIT                   |
| [bddisasm](https://github.com/bitdefender/bddisasm)     | disassembler backend  | Apache-2.0            |
| [Capstone](https://github.com/capstone-engine/capstone) | disassembler backend  | BSD-3-Clause and NCSA |
| [XED](https://github.com/intelxed/xed)                  | disassembler backend  | Apache-2.0            |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI)        | terminal UI           | MIT                   |
| [fmt](https://github.com/fmtlib/fmt)                    | formatting            | MIT                   |
| [Glaze](https://github.com/stephenberry/glaze)           | JSON serialization    | MIT                   |
| [argparse](https://github.com/p-ranav/argparse)         | CLI parsing           | MIT                   |

Full license texts for the bundled libraries are in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

The following build and test tools are not part of the distributed binary.

- [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) 0.42.3 (MIT) is vendored package-management tooling.
- [mbuild](https://github.com/intelxed/mbuild) v2026.08.23 (Apache-2.0) is fetched to build XED through `mfile.py`.
- [GoogleTest](https://github.com/google/googletest) v1.18.0 (BSD-3-Clause) is fetched only when tests are enabled.

Building XED requires Python 3.
