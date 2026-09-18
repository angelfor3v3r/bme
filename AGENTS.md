# AGENTS.md - bme

## What this is

**BME** - a bare-metal x86-64 machine-code viewer / mini step-debugger TUI.
Paste raw bytes (e.g. `48ffc0`), run them in BME's sandbox, and watch GPRs,
RFLAGS, XMM, and x87 state change one instruction at a time.

- **Goal** - Differential analysis, and the real reason BME exists, is finding
  where x86 *decoders* and the *actual CPU* diverge. A disassembler only maps bytes to a mnemonic. It
  can't know an instruction's runtime effect, and two decoders can disagree on the same bytes -
  executing on real silicon settles it. Some results exist only at runtime (a segment / privilege
  check whose outcome depends on the live descriptor tables and current privilege level can't be
  resolved statically), and some encodings have generation-specific meanings, so identical bytes can
  represent different instructions on different processors.
- **Platform** - Windows and Linux, x86-64 only. CMake hard-errors otherwise.
- **Sandbox model** - Windows reserves guarded mappings and single-steps a sandbox thread with a Vectored Exception Handler. Linux reserves the same mappings in a traced child and uses `PTRACE_SINGLESTEP`. Both capture GPR, RFLAGS, XMM, and x87 state after each completed instruction. Faults, `int3`, and runaway loops are contained and surfaced in the UI rather than crashing the host. Detected Intel SDE or Pin instrumentation prevents execution because native single-step state cannot be trusted.
- The sandbox contains faults and runaway loops, not hostile code. Executed bytes retain user
  privileges and can issue system calls or modify process state.
- Decode is pluggable (`DisasmBackend`) - **Zydis** (default), **bddisasm**, **Capstone**, or **XED**. TUI via FTXUI, CLI via argparse, formatting via fmt.
- x87 80-bit conversions run on the FPU via three small assembly leaves. Windows uses `src/st80.asm` with the Win64 ABI. Linux uses `src/st80.S` with the SysV x86-64 ABI. Both build as the `bme_asm` OBJECT library.

## Layout

```
src/common.hpp       # compile-time compiler, OS, and x86-64 gates
src/util.hpp         # ASCII case-insensitive string comparison
src/bme_core.hpp     # public library API (namespace bme). Types, enums, parse/compose/engine/history/JSON prototypes
src/bme_core.cpp     # platform-neutral library logic, history rendering, CLI, and TUI
src/cpu.hpp          # CPU fingerprint data model and injectable CPUID query contract
src/cpu.cpp          # CPUID/XGETBV collection, decoding, process cache, and summary formatting
src/trace_json.cpp   # isolated Glaze adapter and streaming versioned JSON writer
src/os.hpp           # private VM, environment, clipboard, and stepping contract
src/os.cpp           # shared scratch layout and platform-run preflight
src/os.windows.cpp   # Windows VM, VEH, clipboard, environment, and stepping implementation
src/os.linux.cpp     # Linux mmap, ptrace, terminal clipboard, environment, and stepping implementation
src/main.cpp         # thin entry. Parses args, then calls run_quick / run_tui
src/st80.asm         # x87 80-bit conversion leaves for Win64 MASM
src/st80.S           # x87 80-bit conversion leaves for SysV GNU assembler
test/                # GoogleTest suite for the headless path (links bme_core), built with -DBME_BUILD_TESTS=ON
CMakeLists.txt       # CPM deps, platform targets, version generation, CPack, warning policy
README.md            # user-facing usage, build, package, and license summary
LICENSE              # BME MIT license
THIRD_PARTY_LICENSES.md   # bundled runtime dependency licenses
cmake/GenerateVersion.cmake + bme_version.hpp.in   # --version git tag/hash/url -> generated header
cmake/XED.cmake      # Intel XED backend - CPM download + mfile.py build via ExternalProject
.githooks/pre-commit # rejects unformatted commits (diffs staged content against clang-format, warns if the local major version is not 22)
.github/workflows/   # CI runs clang-format plus Windows and Debian 12 builds
.clang-format        # the authoritative style (clang-format 22)
third_party/cmake/   # CPM.cmake
```

Build dirs matching `cmake-build*` or `build*` are local/gitignored.

## Build

CMake 3.31 or newer is required.

Windows uses Ninja, MASM, and **clang++ targeting MSVC**. **clang-cl** and **MSVC cl** also build through the MSVC ABI path.

```sh
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

Linux uses Ninja and a C++23 compiler. Debian 12 with Clang 22 is the CI and packaging baseline. GCC 13 or newer is supported.

```sh
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-22 -DCMAKE_CXX_COMPILER=clang++-22
cmake --build build
```

Deps (fmt 12.2.0, argparse 3.2, Glaze 8.3.0, FTXUI 7.0.3, Zydis `a95bb710...`, bddisasm `3.0.1`,
Capstone `5.0.9`) are fetched by CPM. Zydis also builds its pinned Zycore support library. The `URI`
form auto-applies `EXCLUDE_FROM_ALL`/`SYSTEM`, so third-party headers stay out of `-Werror`. Production
Glaze use is private to `bme_serializer`. Tests link it only for generic JSON schema inspection. Intel
XED (`v2026.08.23` plus its mbuild) has no CMake, so CPM downloads it and an ExternalProject builds it
via `mfile.py` (`cmake/XED.cmake`) - needs Python 3.
All slow on first configure.

CI is the authoritative formatting check. Contributors may enable the tracked pre-commit hook explicitly:

```sh
git config core.hooksPath .githooks
```

- C++23. Clang and GCC warn with `-Wall -Wextra -Wshadow -Wpedantic`. clang-cl / MSVC cl use `/W4 /permissive-`. Codegen is pinned to baseline **x86-64** (SSE2, no AVX - `-march=x86-64`, or cl's immutable x64 default) so `bme` runs on any x86-64 CPU.
- `-Werror` / `/WX` only when `CI` env var is set (toolchain pinned there).
  Locally you see warnings but they don't block.
- **The user compiles and runs themselves.** Do NOT kick off full CMake
  builds to "verify" - CPM recompiles everything from scratch. Verify via
  header-grounded API checks and reading the code. Header-level reasoning is
  the expected verification level here.

## Tests

Off by default. Configure with `-DBME_BUILD_TESTS=ON` to fetch GoogleTest (CPM) and build `bme_tests`, then run `ctest`.

```sh
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-22 -DCMAKE_CXX_COMPILER=clang++-22 -DBME_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

- The `bme_core` split exists for this. The static lib (`namespace bme`) holds all logic, so tests link it and call CPU decoding, parsers, seed composition, CLI, environment, instrumentation preflight, history construction, JSON serialization, quick utilities, and focused engine orchestration contracts directly. `bme_tests` links `bme_core` + `GTest::gtest_main`. It is exempt from `-Werror` so GoogleTest macros can't fail the CI build.
- `test/test_helpers.hpp` holds the shared environment-variable scope, CLI argument builder, and stdout/stderr capture used by utility tests.
- Scope. Test BME behavior that we own. Engine tests cover orchestration such as fault-state classification, instrumentation refusal, and concurrent-call isolation. Serializer tests cover schema shape, exact machine-state encoding, provenance, nulls, decoder histories, and output failures. Do not assert decoder correctness, CPU instruction semantics, ASLR-dependent registers, or decoder-specific text.
- Keep engine cases bounded to safe byte sequences and stable architectural outcomes.

## Code style - load-bearing, the user cares a LOT

These were corrected repeatedly across the session. Match them exactly.

- **C-style casts** (`(std::size_t)x`), not `static_cast`.
- **Namespace** - C++ library and OS code lives in `namespace bme`; `src/main.cpp` remains a thin
  global entry. Still qualify `ftxui::`, never `using namespace ftxui`. `using namespace bme` is
  allowed only in test-only code.
- **Immutability** - Do not add plain `const` to local variables. Use `constexpr` for genuine
  compile-time invariants at local, namespace, or header scope. `const` remains appropriate on
  references and member functions. Not on pointer args.
- **Prefer named types when they add information.** Use `auto` when spelling the type would repeat a
  type already clear from the initializer, return expression, or named object returned by the
  function. Accessors returning `std::get<T>(...)` and functions returning a locally declared result
  should use `auto`. Also use `auto` for long, redundant, or unnameable types such as lambda closures.
- **Direct construction** - Put the type on the declaration (`std::array<...> expected{...}`), not
  in a same-type temporary (`auto expected = std::array<...>{...}`).
- **Choose one source of scalar type information.** Use a named type with an unsuffixed direct
  literal, or `auto` with a typed literal. Do not spell both (`constexpr auto leaf = 0u;`, not
  `constexpr std::uint32_t leaf = 0u;`). Keep a suffix when it controls expression semantics before
  assignment. With `constexpr auto`, the suffix pins the intended type
  (`constexpr auto iterations = 2000ull;`).
- Zero-init with **`{}`**, never `= 0`.
- **`emplace_back`** over `push_back`.
- **Bitwise checks must be explicit and semantically correct.** Use `!= 0` /
  `== 0` for plain masks, and `== flag` / `!= flag` only when actually testing
  that exact flag value. `== flag` vs `!= 0` are NOT interchangeable - pick the
  right one per site. No redundant parentheses around bitwise ops.
- **Verbose names** in both source and GUI, single-letter only for trivial loop
  counters (`i`). No cryptic abbreviations (`fcw` -> `fpu_control_word`, etc).
- **`noexcept` only where genuinely justified.** Every callee must be nonthrowing. Never use it around allocation-capable string or vector work, `fmt`, or component insertion.
- GUI register names uppercased accurately (`RAX`, `RFLAGS`).
- **Comment punctuation.** Code-comment prose is plain ASCII. No `;`, no `:`, and no ` - ` as clause separators, and no unicode. MASM's required leading `;` marker is exempt.
  Use periods or commas. Hyphens inside words, code tokens, and `->` are allowed.
- **Comment brevity.** Keep comments concise and load-bearing. Prefer one or two lines, but retain longer explanations when shortening them would hide behavior or a non-obvious invariant.

clang-format config is settled (clang-format 22, tweaked with braced-list breaking,
function-arg vertical compounding, `NumericLiteralCase`
lower/upper/lower/lower, binary-op breaking `NonAssignment`/`OnePerLine`).
Don't reformat by hand. Clang-format 22 and CI are authoritative. The optional pre-commit hook checks staged C++ files.
Never run clang-format on `src/st80.asm` or `src/st80.S`.

## Key internals

- `Reg` enum (RAX..R15, RIP, RFLAGS), `Flag` masks per sandpile.org.
- `Registers` struct - `gpr[16]`, `rip`, `rflags`, `xmm[16]`, `mxcsr`, `st[8]` (80-bit),
  `fpu_control_word`, `fpu_status_word`, `fpu_tag_word_abridged`
  (FXSAVE 1-bit/reg, not the 16-bit x87 tag word). `operator[](Reg)`.
- `CPUFingerprint` stores process-visible CPUID identity, feature masks, XSAVE/XSTATE data, address widths, hypervisor data, and raw queried leaf/subleaf records. `host_cpu_fingerprint()` caches one immutable process snapshot shared by traces. `CPUQuerySource` makes collection deterministic in tests.
- `ExecutionEvent` stores only CPU execution state: RIP, the full register snapshot, and completed/faulted classification. It has no decoder text or instruction-length fields. Fault-time RFLAGS preserves processor exception-delivery changes such as RF being set for fault-class exceptions.
- `Trace` separates the request (`code`, requested seed/backend/syntax, effective step cap, scratch-pointer policy), recorded execution (`seed`, raw `execution_events`), host CPU fingerprint, and outcome. `Outcome::Error` identifies engine or instrumentation failure, while `Outcome::Faulted` identifies a fault raised by supplied bytes. `stop_reason` records why execution halted and labels stop, fault, or not-reached rows where applicable. `stop_address` anchors an `int3`, fault, or selected-row stop.
- `build_history` overlays one decoder's instruction boundaries on immutable trace code and execution events. Static rows stop before every execution-event start, even when a decoder's linear instruction would cross that later entry point. `HistoryRow` maps reached and faulted rows back through `execution_event_index` without copying register snapshots. Multiple backend histories can coexist without mutating `Trace`.
- `write_trace_json` streams compact or two-space-indented schema version 1 JSON through the Glaze-only `bme_serializer` translation unit. The document includes producer and dependency revisions, host and CPU provenance, request state, full execution snapshots, outcome, and all four decoder histories. Integer machine state uses fixed-width hexadecimal strings. Stable unavailable values use `null`. The writer appends one newline and reports serialization or stream failures.
- Platform execution lives behind the private `os.hpp` contract. `os.cpp` handles shared preflight,
  including copying the requested seed and refusing execution when instrumentation is detected.
  Windows uses guarded `VirtualAlloc` mappings and a VEH sandbox thread. Linux uses guarded `mmap`
  mappings and a traced child with `PTRACE_SINGLESTEP`, `PTRACE_O_EXITKILL`, and `PTRACE_GETFPREGS`.
  Both platforms use guarded 64 KiB scratch stack and data regions. The data mapping reserves at
  `SCRATCH_DATA_RESERVE_BASE`, never relocates, and exposes usable bytes one guard page above it.
  RDI and RSI default to that usable address unless seeded. An optional stop offset is resolved against
  each run's code base and checked after every completed step without patching the input. The step cap
  defaults to 50k and clamps to `MAX_STEPS_LIMIT`, but it does not bound wall-clock time. A blocking
  system call can stall a run.
- Linux asks Zydis to identify software-breakpoint instructions only after ptrace reports a breakpoint-class `SIGTRAP`. This avoids handwritten x86 parsing and distinguishes WSL2's ambiguous syscall completion trap. The runtime check does not use the selected display backend.
- `parse_code_text` accepts contiguous or whitespace-separated hex, `\xNN` escapes, and `{ 0xNN, ... }` byte arrays. `--file` reads up to 15,000,000 exact bytes from a raw binary file. It never infers a text encoding. `--bytes` and `--file` are mutually exclusive.
- Decode backends (`DisasmBackend`). `disasm_one(backend, syntax, addr, code, size)` -> `Decoded`
  (`ok`/`text`/`length`), dispatching to **Zydis** (Intel + AT&T), **bddisasm** (Intel only), **Capstone** (Intel + AT&T), or **XED** (Intel + AT&T),
  gated by `backend_supports`, which reads the per-decoder `BACKENDS` capability table. `run_engine` records decoder-neutral execution events and
  `build_history` applies a selected decoder afterward. The goal is differential decoding - run the same bytes through each decoder and watch where
  they diverge in operand or RIP-relative rendering, instruction length, or decode success.
- `GPRSeed` (per-GPR seed text `full`/`dword`/`word`/`byte_high`/`byte_low`) +
  `compose_gpr_seed` - widest non-empty slice is the base, each narrower non-empty
  slice overlays its bits (EAX refines RAX, AL refines AX, ...). Empty slices ignored.
  A malformed slice is skipped (never zeroed over a wider slice) and reported, not
  discarded. `compose_seed` composes a full `Registers` (GPR + RFLAGS + XMM + ST + MXCSR + x87 control word) from
  the seed text and collects every such error, shared by the TUI's Run and `--quick`.
  The TUI stays lenient. A bad field retains its default, its error is appended to `ui.status`, and the
  run proceeds. `--quick` uses the same composer, but `CLI::parse` validates every field first, so a
  composition error there is treated as unreachable and fails loudly.
- XMM/x87 seeding follows the same text-field model. `compose_xmm_seed` (128-bit `{lo, hi}`) and
  `compose_st_seed` (80-bit, 10 bytes) each parse one field (`ui.seed_xmm[i]` / `ui.seed_st[i]`), hex
  or a decimal with a `.` or a non-finite `inf`/`nan` value, and an optional trailing `f`/`F` (single
  precision) or `l`/`L`/none (double), via `parse_decimal_seed`.
  XMM places single in the low 32 bits (f32x4 lane 0) and double in the low 64 bits (f64x2 lane 0).
  x87 rounds the value to 80-bit via `double_to_st80`. `FloatingEnvironmentSeed` carries optional raw
  hexadecimal `MXCSR` and `control_word` text. Both backends reset x87 status and tag state, apply the composed
  control words plus XMM and x87 seeds to the native context, establish `TOP` 0, and mark seeded x87
  slots non-empty. Windows uses `FltSave`; Linux uses `user_fpregs_struct`. MXCSR bits unsupported by
  the live context's mask fail the run rather than being silently cleared.
  The x87 conversion leaves preserve the caller's control word and apply only the captured rounding-control bits.
  `render_xmm` always shows the `f64x2` decimal row when a trace exists, with
  `f32x4` behind the click-to-expand toggle (`ui.xmm_expand`), each lane its own `copy_cell`.
  `render_x87` mirrors this with a per-ST `f32` (Real4) narrowed row behind `ui.st_expand`.
  Both `st80_to_double` and `st80_to_float` use the captured x87 rounding mode.
  The float path converts extended directly to single without double rounding and shows what an `FSTP m32`
  would store. Empty x87 slots expose their inactive raw storage but no derived decimal value.
  Narrowed decimal text is a copy target and its `f` suffix round-trips through the seed field. Its 32-bit
  raw hex is display-only since pasting it back would seed the wrong 80-bit bits.
- Flag seeding uses `ui.seed_flags` (status flags only), copied into the run seed. Both platform
  backends mask it with `RFLAGS_STATUS_MASK` and force IF plus the reserved bit in the reported seed.
  Windows also sets TF in its execution context; Linux single-steps through ptrace. The Flags panel
  (`flags_view`, a `Renderer`+`CatchEvent`) is clickable; a click toggles the corresponding seed bit.
- Render lambdas. `render_registers` (GPRs in `REGISTER_DISPLAY_ORDER` - RIP first, canonical order, RFLAGS last. Every drill-down level RAX -> EAX -> AX -> AH/AL
  has its own editable seed `Input`, `Maybe`-gated by `ui.gpr_depth`, with `reflect()`
  click-boxes toggling the depth), `render_xmm`, and `render_x87` (the last two carry a per-register seed `Input` column too) - all use `ftxui::Table`
  with `SeparatorVertical(LIGHT)` + dim header, changed values render yellow+bold. The x87
  panel shows each `ST(i)` with its physical `x87rN` = `(TOP+i)&7`, tag, 80-bit raw, and double,
  plus `CW`/`SW`/`TW` decoded below via `decode_x87_*`. The SSE panel folds a decoded
  `MXCSR` line under the XMM table (`decode_mxcsr`). `st[i]` is stack-relative as captured
  (`st[i]` == `ST(i)`), so `render_x87` indexes it directly - `TOP` only maps to the physical
  name and the (physical-ordered) tag bit. Every register value cell is click-to-copy
  (`copy_cell`/`copy_hits`, handled in the layout `CatchEvent` alongside the Data-address copy),
  down to each lane in the XMM/x87 float drill-downs.
- Scrolling. `ScrollerBase`/`make_scroller` wrap the register tabs in a viewport driven by an offset getter
  (`ui.register_scroll`, an `array<int32_t, 3>` - one slot per register tab, not one shared value, so
  scrolling GPR doesn't leave a stale offset that clamps XMM/x87 to their own last row the instant you switch
  or defocus into them). Stepped by 1 per wheel notch, same as History's `ui.cursor`. Three concerns, three
  fixes, two helpers shared by all of them (`register_has_real_focus`, `defocus_current_register_tab`, both
  declared once above `register_scroller` and dispatching on `ui.register_tab`).
  - **Which row.** `ftxui::yframe` positions the viewport from `requirement_.focused.box` via
    `Frame::SetBox`, and ftxui's own DOM-level tie-break (`Requirement::Focused::Prefer`) can't be made to
    reliably prefer a wheel-driven marker over real content, since every seed `Input` unconditionally marks
    its own cursor cell focus-enabled (for blink positioning, even when not the real focus) and the active
    register tab is structurally the sole/first-focusable child of its `Container::Tab`
    (`focusPositionRelative`, an unconditional force, was tried and clobbered the cursor while typing).
    Instead, `ScrollerBase` takes a `has_real_focus` callback (`register_has_real_focus`) and decides in
    C++. A `Container::Vertical`'s active child (index 0 by default) permanently wins ftxui's own focus
    chain over the wheel once anything makes it "active", and ftxui has no "unfocus", so `FocusSink` (a
    non-focusable no-op appended last to each of `seed_components`/`xmm_seed_components`/
    `st_seed_components`, `Focusable() == false` so arrow-key `MoveSelector` can never land on it, but
    `TakeFocus()` doesn't check `Focusable()` so it can still be targeted explicitly) gives
    `register_has_real_focus` a cheap, purely local signal to check per tab (`!gpr_focus_sink->Active()`
    etc.). Each seed container is built with an explicit external `int` selector defaulting to the sink's
    index (not 0), so no real `Input` is active at startup. Clicking an `Input` still claims the slot
    normally (`Input::OnEvent` calls `TakeFocus()`), flipping `register_has_real_focus` true.
    `defocus_current_register_tab` reverses that (`TakeFocus()` on the sink for whichever register tab is
    visible), shared by three recovery paths - Escape; each seed `Input`'s own `InputOption::on_enter`
    (`ftxui::Input::HandleReturn` always consumes `Event::Return` for `multiline = false` regardless of
    `on_enter`, so wiring it only adds a side effect, never changes propagation); and a left click on blank
    space inside the panel (see below). Scoped to the visible tab only, since `Container::Tab`'s
    `SetActiveChild` writes straight through to its bound selector (`ui.register_tab`), so calling it on the
    wrong tab's sink would silently switch tabs.
  - **Where it lands.** When `register_has_real_focus` is true, plain `ftxui::yframe` follows the real
    `Input`'s own focus box (centered, cursor stays put while typing). Otherwise `ScrollViewport` (a
    from-scratch `ftxui::Node`, public API only, mirroring `Frame::SetBox`/`Render` in frame.cpp) drives it,
    scrolling `offset` to exactly the viewport's first visible row. Plain `yframe` was tried for this case
    too (`focusPosition(0, offset) | yframe`) but its `Frame::SetBox` always *centers* the target
    (`dy = focused.y_min - external_dimy / 2 + ...`), clamping away roughly half the viewport height at each
    end before the view visibly moves, so the wheel felt like it needed several notches of dead travel at
    the top/bottom before anything happened. ftxui has no public "top align" frame variant, so
    `ScrollViewport` reimplements the box-shift-and-stencil-clip against the public `Node`/`Box`/`AutoReset`
    API instead (`Node` is documented as the extension point; `NodeDecorator` is not public, hence a raw
    `Node` subclass rather than a decorator). Holds `offset` by reference (from the getter, re-fetched each
    `OnRender`) and writes its own clamped `dy` back into it every `SetBox` - the only place the real
    viewport height is known, and the sole clamp now: `ScrollerBase` used to also clamp to
    `[0, content_height - 1]` on every frame regardless of focus state, which was actively harmful, not just
    redundant - it let the wheel handler (see below) silently drive a *typed-into* tab's offset past the
    usable range (nothing visibly scrolls while `register_has_real_focus` is true, since that path ignores
    `offset` entirely), only for it to snap into view later once focus dropped and `ScrollViewport` clamped
    it hard back down.
  - **Blank space.** A left click anywhere inside the Registers box that nothing else already claimed (the
    layout-level `CatchEvent`'s click-to-expand/copy checks run first and fully consume matching clicks, so
    those don't reach here) calls `defocus_current_register_tab` and returns `false`, letting the click fall
    through normally afterward. A real click on a seed `Input`, button, or `register_toggle` reclaims focus
    immediately within the same synchronous dispatch (every ftxui click handler calls `TakeFocus()` on
    `Mouse::Left` + `Motion::Pressed`, confirmed in `input.cpp`'s `HandleMouse`), so this only has a lasting
    effect on genuinely blank space (the separator row, padding, anywhere without a focusable element).
  The wheel is caught by the same whole-box `CatchEvent` (`registers_view`) so the GPR/SSE/x87 toggle can't
  hijack it, and is a no-op (still consumed, so it never reaches the toggle either) while
  `register_has_real_focus` is true - see above. Switching tabs (click or arrow key on `register_toggle`)
  also calls
  `register_scroller->TakeFocus()` via `MenuOption::on_change` - the Toggle's own click claims focus up
  through `registers_pane`, closing `Container::OnEvent`'s `Focused()` gate for the newly selected tab's
  content until something there reclaims it, which otherwise silently breaks arrow-key seed navigation on
  the new tab until a seed field is clicked directly. History has its own drill-down. `history_tabs`
  (`ui.history_tab`) holds a Main menu (`ui.history`, always the active Settings backend) plus one per
  `BACKENDS` decoder. Each tab is built from `Trace::code` with that decoder's own instruction boundaries,
  falling back to Intel syntax if the backend can't render AT&T. The lightweight `HistoryRow` model maps
  each row back to its execution event without duplicating completed register snapshots. An in-range
  fault row is anchored at the CPU's instruction address and exposes exception-time partial state without
  incrementing the executed count. Instruction-fetch fault addresses outside `Trace::code` are not decoded
  as history rows. Switching tabs preserves the selected state or byte address even when decoder boundaries
  differ. Reached rows render normally, faults in red, stop rows emphasized, and static rows dimmed.
  Code and scratch-data addresses use `code+offset` and `data+offset` forms by default. The header shows
  clickable absolute code and data bases, while Settings can switch register and History values back to
  absolute addresses. **Copy row** copies the selected rendered row. The History wheel (`history_view`
  `CatchEvent`) steps `ui.cursor` within the active tab.
- Tabs are GPR / SSE / x87. Buttons are Run / Run to row / Step / Back / Copy row / Reset / Settings /
  About / Quit. **Run to row** reruns from the configured seed and stops before the selected byte offset.
  Keyboard `F5`/`F8`/`F7` runs, steps, and backs. Layout shortcuts are suppressed while a modal is open.
  Flags panel (`render_flags`/`flags_view`) is clickable for seedable status flags. It shows RF as read-only
  exception state only when set on the selected fault row. The SSE and x87 tabs expose `MXCSR` and
  control-word seed inputs. Settings holds the disasm syntax, decode backend, single-step cap
  (`ui.max_steps`), scratch-pointer toggle (`ui.seed_data_pointers`), and normalized-address display.
  About shows version / repo / copyright. `Reset` clears the run but keeps your seeds.

## CLI

```
bme --bytes 48ffc0 --run                 # inc rax, run on launch
bme --bytes "\x48\xFF\xC0"               # C-style escaped bytes
bme --bytes "{ 0x48, 0xFF, 0xC0 }"       # C-style byte array
bme --file code.bin --quick              # raw binary file
bme --bytes 48C7C001000000 --syntax att  # AT&T disasm (default intel)
bme --bytes 48ffc0 --backend bddisasm    # decode with bddisasm instead of Zydis (Intel only)
bme --max-steps 200000                   # raise the single-step cap
bme --version                            # tag/hash/url, clang-format style
bme --bytes 48ffc0 --quick               # headless trace dump to stdout (--track picks register classes)
bme --bytes 48ffc0 --quick --format json # versioned full-state JSON trace
bme --bytes 48ffc0 --quick --format json --pretty # indented full-state JSON trace
bme --bytes 90 --quick --seed mxcsr=5f80,control_word=27f # seed SSE and x87 control state
```

`--quick` runs headless through `run_engine`. The default `--format text` renderer prints the CPU summary, seed, tracked per-event register deltas, static not-reached rows, and outcome. `--format json` sends the complete trace to `write_trace_json`; it always contains full register state and rejects an explicit `--track`. `--pretty` adds indentation and requires `--quick --format json`. Supplied-code faults remain successful traces. Engine or instrumentation errors emit a complete JSON document and return nonzero. Invalid CLI or byte input emits no JSON. `--bytes` accepts formatted text and `--file` reads a raw binary file. `--seed name=value,...` sets initial state. GPRs, flags, `MXCSR`, and `control_word` use hex. XMM and ST also accept finite decimals containing `.`, or `inf`/`nan`, with an optional `f`/`F` single-precision suffix or `l`/`L` double-precision suffix.

argparse gotcha. On `--syntax`, `--backend`, and `--format`, the `.nargs(1)` *after* `.default_value(...)`
is load-bearing. `default_value` resets the nargs min to 0, which would make an invalid value parse as a
stray positional instead of a clean allowed-options error. Keep that ordering.

## Project meta

- Owner - **angelfor3v3r** (Dexxi). Repo `github.com/angelfor3v3r/bme`.
- License - **MIT** © angelfor3v3r (Dexxi). Keep `BME_COPYRIGHT` in `bme_core.cpp`,
  LICENSE, and the About box in sync.
- Versioning - stable SemVer tags use `vMAJOR.MINOR.PATCH`. The public release line starts at `v1.0.0`. `--version` resolves git tag/hash
  via `cmake/GenerateVersion.cmake` -> generated `bme_version.hpp` (build-time regen, recompiles only
  when tag/hash move). Source builds show branch.

## Future - decoder-divergence fuzzer

The real payoff is to brute-force the x86 encoding space and record where things disagree. Where the decoders differ on instruction length, operands, or decode success, and where a live single-step run diverges from the static decode. A dedicated harness - pin to one core, enumerate encodings systematically, diff every backend per encoding, log the divergences - is the right home for that, not the unit tests.
