# AGENTS.md - bme

## What this is

**BME** - a bare-metal x86-64 machine-code viewer / mini step-debugger TUI.
Paste raw bytes (e.g. `48ffc0`), run them in an in-process sandbox, and watch
GPRs, RFLAGS, XMM, and x87 state change one instruction at a time.

- **Goal** - Differential analysis, and the real reason BME exists, is finding
  where x86 *decoders* and the *actual CPU* diverge. A disassembler only maps bytes to a mnemonic. It
  can't know an instruction's runtime effect, and two decoders can disagree on the same bytes -
  executing on real silicon settles it. Some results exist only at runtime (a segment / privilege
  check whose outcome depends on the live descriptor tables and current privilege level can't be
  resolved statically), and some encodings have generation-specific meanings, so identical bytes can
  represent different instructions on different processors.
- **Platform** - Windows only, x86-64 only. CMake hard-errors otherwise.
- **Sandbox model** - `VirtualAlloc` a buffer, write the bytes, mark RX, then
  single-step via a Vectored Exception Handler (trap flag). Each step records
  register state *after* the instruction. Faults / `int3` / runaway loops are
  contained and surfaced in the UI rather than crashing the process.
- Decode is pluggable (`DisasmBackend`) - **Zydis** (default), **bddisasm**, **Capstone**, or **XED**. TUI via FTXUI, CLI via argparse, formatting via fmt.
- x87 80-bit conversions run on the FPU via three tiny MASM leaves in `src/st80.asm`
  (`st80_to_double`, `double_to_st80`, and `st80_to_float`, Win64 ABI, built as the `bme_asm` OBJECT library).

## Layout

```
src/bme_core.hpp     # public library API (namespace bme). Types, enums, parse/compose/engine/decode prototypes
src/bme_core.cpp     # library implementation (namespace bme). All logic except main
src/main.cpp         # thin entry. Parses args, calls bme::init then run_quick / run_tui
src/st80.asm         # x87 80-bit <-> double and -> float, on-FPU (MASM, win64)
test/                # GoogleTest suite for the headless path (links bme_core), built with -DBME_BUILD_TESTS=ON
CMakeLists.txt       # CPM deps, version-gen, warning policy
cmake/GenerateVersion.cmake + bme_version.hpp.in   # --version git tag/hash/url -> generated header
cmake/XED.cmake      # Intel XED backend - CPM download + mfile.py build via ExternalProject
.githooks/pre-commit # rejects unformatted commits (diffs staged content against clang-format, warns if the local major version is not 22)
.github/workflows/   # CI runs clang-format check + Windows clang->MSVC build
.clang-format        # the authoritative style (clang-format 22)
third_party/cmake/   # CPM.cmake
```

Build dirs (`cmake-build-*`, `build-clang`) are local/gitignored.

## Build

Windows, Ninja + MASM. Primary/CI toolchain is **clang++ targeting MSVC** (GNU driver). **clang-cl** and **MSVC cl** also build (they ride the MSVC flag path).

```sh
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

Deps (fmt 12.2.0, argparse 3.2, FTXUI 7.0.3, Zydis `a95bb710...`, bddisasm `3.0.1`, Capstone
`5.0.9`) are fetched by CPM - the `URI` form auto-applies `EXCLUDE_FROM_ALL`/`SYSTEM`, so third-party
headers stay out of `-Werror`. Intel XED (`v2026.08.23` plus its mbuild) has no CMake, so CPM
downloads it and an ExternalProject builds it via `mfile.py` (`cmake/XED.cmake`) - needs Python 3.
All slow on first configure.

- C++23. clang++ warns with `-Wall -Wextra -Wshadow -Wpedantic`. clang-cl / MSVC cl use `/W4 /permissive-`. Codegen is pinned to baseline **x86-64** (SSE2, no AVX - `-march=x86-64`, or cl's immutable x64 default) so `bme` runs on any x86-64 CPU.
- `-Werror` / `/WX` only when `CI` env var is set (toolchain pinned there).
  Locally you see warnings but they don't block.
- **The user compiles and runs themselves.** Do NOT kick off full CMake
  builds to "verify" - CPM recompiles everything from scratch. Verify via
  header-grounded API checks and reading the code. Header-level reasoning is
  the expected verification level here.

## Tests

Off by default. Configure with `-DBME_BUILD_TESTS=ON` to fetch GoogleTest (CPM) and build `bme_tests`, then run `ctest`.

```sh
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBME_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

- The `bme_core` split exists for this. The static lib (`namespace bme`) holds all logic, so tests link it and call parser, seed-composition, CLI, environment, quick error-path utilities, and focused engine orchestration contracts directly. `bme_tests` links `bme_core` + `GTest::gtest` with a custom `main` (`test/main.cpp`) that calls `bme::init`. It is exempt from `-Werror` so GoogleTest macros can't fail the CI build.
- `test/test_helpers.hpp` holds only the shared CLI argument builder and stdout/stderr capture used by utility tests.
- Scope. Test BME behavior that we own. Engine tests cover orchestration such as fault-state classification and concurrent-call isolation. Do not assert decoder correctness, CPU instruction semantics, ASLR-dependent registers, or decoder-specific text.
- Keep engine cases bounded to safe byte sequences and stable architectural outcomes.

## Code style - load-bearing, the user cares a LOT

These were corrected repeatedly across the session. Match them exactly.

- **C-style casts** (`(std::size_t)x`), not `static_cast`.
- **Namespace** - the `bme_core` library (header + cpp) is in `namespace bme`. Still qualify `ftxui::`, never `using namespace ftxui`. `using namespace bme` is allowed only in test-only code.
- **No `const` on local variables** inside functions (useless). `const` only on
  references where it makes sense and on member functions. Not on pointer args.
- **Prefer `auto`** wherever possible.
- Zero-init with **`{}`**, never `= 0`.
- **`emplace_back`** over `push_back`.
- **Bitwise checks must be explicit and semantically correct.** Use `!= 0` /
  `== 0` for plain masks, and `== flag` / `!= flag` only when actually testing
  that exact flag value. `== flag` vs `!= 0` are NOT interchangeable - pick the
  right one per site. No redundant parentheses around bitwise ops.
- **Verbose names** in both source and GUI, single-letter only for trivial loop
  counters (`i`). No cryptic abbreviations (`fcw` -> `fpu_control_word`, etc).
- **`noexcept` only where genuinely justified.** Fine on genuinely no-throw code
  and OOM-only paths whose *sole* failure is `bad_alloc` (`std::string`/`std::vector`
  ops - `disasm_one`, `decode_history_steps`, `apply_history_steps`, and `redisasm`). NOT on anything
  that calls `fmt::format`/`fmt::println` - `fmt` can throw `format_error`, so
  formatting functions (`format_hex64_string`, `parse_hex`, `parse_seed`, `CLI::parse`,
  `compose_gpr_seed`, `compose_seed`, `compose_xmm_seed`, `compose_st_seed`, `run_engine`,
  `main`, `build_history_lines`, `rebuild_history`, the render
  lambdas) are deliberately non-`noexcept`.
- GUI register names uppercased accurately (`RAX`, `RFLAGS`).
- **Comment punctuation.** Code-comment prose is plain ASCII. No `;`, no `:`, and no ` - ` as clause separators, and no unicode. MASM's required leading `;` marker is exempt.
  Use periods or commas. Hyphens inside words, code tokens, and `->` are allowed.
- **Comment brevity.** Keep comments concise and load-bearing. Prefer one or two lines, but retain longer explanations when shortening them would hide behavior or a non-obvious invariant.

clang-format config is settled (clang-format 22, tweaked with braced-list breaking,
function-arg vertical compounding, `NumericLiteralCase`
lower/upper/lower/lower, binary-op breaking `NonAssignment`/`OnePerLine`).
Don't reformat by hand - the pre-commit hook + CI enforce it.
Never run clang-format on `src/st80.asm`. clang-format does not support MASM.

## Key internals (src/bme_core.cpp)

- `Reg` enum (RAX..R15, RIP, RFLAGS), `Flag` masks per sandpile.org.
- `Registers` struct - `gpr[16]`, `rip`, `rflags`, `xmm[16]`, `mxcsr`, `st[8]` (80-bit),
  `fpu_control_word`, `fpu_status_word`, `fpu_tag_word_abridged`
  (FXSAVE 1-bit/reg, not the 16-bit x87 tag word). `operator[](Reg)`.
- `Step` - rip, bytes, disasm `text`, and `registers`. `reached` marks a completed instruction,
  `faulted` marks a faulting instruction whose registers are exception-time partial state, and neither
  marks static disassembly that never executed. `data` identifies raw "(data)" bytes.
- `Trace` - seed, steps, `Outcome` (Idle/Finished/Faulted/AbortedCap/Stopped), `message` (status
  summary), `stop_reason` + `stop_address` (labels the int3/fault stop instruction).
- `Engine` - VirtualAlloc/VEH sandbox. Pre-reserves step vector so the trap path
  is allocation-free (a wild `mov [addr],reg` can corrupt the heap - never
  realloc inside the VEH). 64 KiB scratch stack + a 64 KiB scratch data region (`SCRATCH_DATA_BYTES`, guard pages both sides) reserved at the fixed `SCRATCH_DATA_BASE` (shown in the header) so any seed or `[mem]` can target it. RDI/RSI default there unless seeded (they're x86's string-op source/dest pointers, so `movs`/`stos`/`rep` snippets just work). Step cap `ui.max_steps` (default 50k, via `--max-steps`/Settings, clamped to `MAX_STEPS_LIMIT`).
- `parse_hex` (`std::from_chars`, `Result`/`std::expected`-based),
  `parse_seed`, `fault_name`.
- Decode backends (`DisasmBackend`). `disasm_one(backend, syntax, addr, code, size)` -> `Decoded`
  (`ok`/`text`/`length`), dispatching to **Zydis** (Intel + AT&T), **bddisasm** (Intel only), **Capstone** (Intel + AT&T), or **XED** (Intel + AT&T),
  gated by `backend_supports`, which reads the per-decoder `BACKENDS` capability table. `run_engine`/`redisasm` thread the backend through. The goal is
  differential decoding - run the same bytes through each decoder and watch where they diverge -
  operand / RIP-relative rendering, instruction length, or outright decode disagreement - to
  surface decoder assumptions and bugs (e.g. whether a decoder follows RIP correctly).
- `disasm_one` returns each decoder's exact text - whitespace-trimmed only, never normalized (that native formatting is what we diff between backends).
- `GPRSeed` (per-GPR seed text `full`/`dword`/`word`/`byte_high`/`byte_low`) +
  `compose_gpr_seed` - widest non-empty slice is the base, each narrower non-empty
  slice overlays its bits (EAX refines RAX, AL refines AX, ...). Empty slices ignored.
  A malformed slice is skipped (never zeroed over a wider slice) and reported, not
  discarded. `compose_seed` composes a full `Registers` (GPR + RFLAGS + XMM + ST) from
  the seed text and collects every such error, shared by the TUI's Run and `--quick`.
  The TUI stays lenient (a bad field just gets left unseeded, reported in `ui.status`,
  never blocking the run), matching `--seed`'s already-lenient TUI seed fields, while
  `run_quick` treats a composition error as unreachable (`CLI::parse` already validated
  every field strictly) and fails loudly if it ever happens.
- XMM/x87 seeding follows the same text-field model. `compose_xmm_seed` (128-bit `{lo, hi}`) and
  `compose_st_seed` (80-bit, 10 bytes) each parse one field (`ui.seed_xmm[i]` / `ui.seed_st[i]`), hex
  or a decimal with a `.` or a non-finite `inf`/`nan` value, and an optional trailing `f`/`F` (single
  precision) or `l`/`L`/none (double, `long double` == `double` on this ABI), via `parse_decimal_seed`.
  XMM places single in the low 32 bits (f32x4 lane 0) and double in the low 64 bits (f64x2 lane 0); x87 rounds the value
  to 80-bit via `double_to_st80`. `run_engine` writes XMM into `FltSave.XmmRegisters` and seeded ST
  slots into `FloatRegisters` with `TOP` 0 and their tag-word bits set. `render_xmm` always shows the
  `f64x2` decimal row when a trace exists, with `f32x4` behind the click-to-expand toggle (`ui.xmm_expand`),
  each lane its own `copy_cell`. `render_x87` mirrors this with a per-ST `f32` (Real4) narrowed row behind
  `ui.st_expand`. Both `st80_to_double` and `st80_to_float` use the captured x87 rounding mode.
  The float path converts extended directly to single without double rounding and shows what an `FSTP m32`
  would store. Empty x87 slots expose their inactive raw storage but no derived decimal value.
  Narrowed decimal text is a copy target and its `f` suffix round-trips through the seed field. Its 32-bit
  raw hex is display-only since pasting it back would seed the wrong 80-bit bits.
- Flag seeding uses `ui.seed_flags` (status flags only), copied into the run seed. `run_engine`
  masks it with `RFLAGS_STATUS_MASK` and forces TF/IF/reserved. The Flags panel (`flags_view`,
  a `Renderer`+`CatchEvent`) is clickable - a click toggles that flag's seed bit (`FlagHit` boxes).
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
    etc). Each seed container is built with an explicit external `int` selector defaulting to the sink's
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
  falling back to Intel syntax if the backend can't render AT&T. The lightweight `HistoryStep` model maps
  each row back to its execution position without duplicating completed register snapshots. An in-range
  fault row is anchored at the CPU's instruction address and exposes exception-time partial state without
  incrementing the executed count. Instruction-fetch fault addresses outside `Trace::code` are not decoded
  as history rows. Switching tabs preserves the selected state or byte address even when decoder boundaries
  differ. The History wheel (`history_view` `CatchEvent`) steps `ui.cursor` within the active tab.
- Tabs are GPR / SSE / x87. Buttons are Run / Step / Back / Reset / Settings /
  About / Quit - keyboard `F5`/`F8`/`F7` run/step/back via a layout `CatchEvent`
  (suppressed while a modal is open). Flags panel (`render_flags`/`flags_view`) is
  clickable to seed status flags. Settings modal holds the disasm syntax, decode backend, single-step cap
  (`ui.max_steps`), and an RDI/RSI-to-scratch toggle (`ui.seed_data_pointers`). About modal shows version / repo / copyright. `Reset` clears the run but keeps your seeds.

## CLI

```
bme --bytes 48FFC0 --run                 # inc rax, run on launch
bme --bytes 48C7C001000000 --syntax att  # AT&T disasm (default intel)
bme --bytes 48FFC0 --backend bddisasm    # decode with bddisasm instead of Zydis (Intel only)
bme --max-steps 200000                   # raise the single-step cap
bme --version                            # tag/hash/url, clang-format style
bme --bytes 48FFC0 --quick               # headless trace dump to stdout (--track picks register classes)
bme --bytes 48F7F3 --quick --seed rax=64,rbx=9 # seed GPRs/XMM/ST/flags before the run (hex or a decimal)
```

`--quick` runs headless via `run_quick` - same `run_engine`, but prints the seed then per-step register deltas (`--track` selects classes), the not-reached rows, and the outcome instead of opening the TUI. `--seed name=value,...` sets initial state. Values can be hex, a finite decimal containing `.`, or `inf`/`nan`, with an optional `f`/`F` single-precision suffix or `l`/`L` double-precision suffix. Names are any GPR slice (`RAX`/`EAX`/`AX`/`AH`/`AL`, not `RSP`), `XMM0..15`, `ST0..7`, or a status flag (`CF/PF/AF/ZF/SF/DF/OF`). `CLI::parse` validates and stores the raw text (`seed_gpr`/`seed_flags`/`seed_xmm`/`seed_st`), feeding both the TUI seed inputs and `--quick`, which compose it (`compose_gpr_seed`/`compose_xmm_seed`/`compose_st_seed`) so narrower GPR slices overlay wider ones.

argparse gotcha. On `--syntax` and `--backend`, the `.nargs(1)` *after* `.default_value(...)`
is load-bearing - `default_value` resets the nargs min to 0, which would make an
invalid `--syntax` value parse as a stray positional instead of a clean "allowed
options" error. Keep that ordering.

## Project meta

- Owner - **angelfor3v3r** (Dexxi). Repo `github.com/angelfor3v3r/bme`.
- License - **MIT** © angelfor3v3r (Dexxi). Keep `BME_COPYRIGHT` in `bme_core.cpp`,
  LICENSE, and the About box in sync.
- Versioning - SemVer tags. The public release line starts at `v1.0.0`. `--version` resolves git tag/hash
  via `cmake/GenerateVersion.cmake` -> generated `bme_version.hpp` (build-time regen, recompiles only
  when tag/hash move). Source builds show branch.

## Future - decoder-divergence fuzzer

The real payoff is to brute-force the x86 encoding space and record where things disagree. Where the decoders differ on instruction length, operands, or decode success, and where a live single-step run diverges from the static decode. A dedicated harness - pin to one core, enumerate encodings systematically, diff every backend per encoding, log the divergences - is the right home for that, not the unit tests.
