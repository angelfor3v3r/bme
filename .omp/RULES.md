# RULES.md - bme

Hard, always-apply rules. Background and full detail live in AGENTS.md.

## Git - never

- NEVER commit, push, stage (`git add`), or create a commit, tag, or branch. Not even when asked to "save", "check in", or "wrap up". The user runs all git themselves.
- Leave the pre-commit hook and `.github/workflows` alone.

## Building and generated files

- Do NOT run a fresh CMake configure (`cmake -B ...`). It forces CPM to refetch and rebuild every dependency from scratch.
- Default to header-grounded reasoning and reading the code. That is the expected level of verification.
- If a compile check is genuinely needed, build incrementally against the build directory that is already configured. Do not create, delete, or reconfigure a build dir. Toolchain is clang / clang++ from `C:\Program Files\LLVM`.
- Codegen stays baseline x86-64 (SSE2, `-march=x86-64`). No AVX, no newer ISA, no `-march=native`. `bme` must run on any x86-64 CPU.
- Never edit generated files. `bme_version.hpp` is generated from `bme_version.hpp.in` by CMake. Edit the template, never the output.

## TUI and --quick

- NEVER break the interactive TUI. Every change must preserve TUI behavior.
- Exercise and verify behavior through headless `--quick` only (e.g. `bme --bytes 48FFC0 --quick --seed rax=64`). Do not drive or depend on the interactive TUI to test.
- Keep feature parity between the TUI and the CLI / `--quick` path within reason. A capability added to one should be reachable from the other.

## Tests

- Unit tests live in `test/` (GoogleTest), gated behind `-DBME_BUILD_TESTS=ON`, run via `ctest`. They link `bme_core`.
- Test our own code. Engine semantics, seeding, parsing, and `--quick` output. Do NOT unit-test decoder correctness because backends legitimately diverge on odd encodings.

## Comments - the rule that keeps getting dropped

Code comments are plain ASCII prose.

- Keep them short. A line or two on the why or a non-obvious gotcha, never a paragraph. Compress ftxui/API backstory to the load-bearing point. Long design rationale belongs in AGENTS.md, not inline.
- No `;`, no `:`, and no ` - ` (spaced dash) as a clause separator.
- No unicode. No em-dashes, no smart quotes.
- Break clauses with periods and commas.
- Allowed: hyphens inside words (`x86-64`, `single-step`, `sub-register`), code tokens (`--backend`, `-march`), and `->` arrows.

## Code style - match exactly

- C-style casts: `(std::size_t)x`. Never `static_cast` / `reinterpret_cast` / etc.
- No `const` on local variables. `const` only on references where it makes sense and on member functions. Never on pointer args.
- Zero-init with `{}`. Never `= 0`.
- Prefer `auto` wherever possible.
- `emplace_back`, never `push_back`.
- `bme_core` (header + cpp) lives in `namespace bme`. Never `using namespace ftxui`; qualify `ftxui::`. `using namespace bme` only in test code.
- Bitwise checks explicit and correct: `!= 0` / `== 0` for plain masks, `== flag` / `!= flag` only when testing that exact flag value. No redundant parentheses around bitwise ops.
- Verbose names. Single-letter only for a trivial loop counter (`i`). No cryptic abbreviations.
- `noexcept` only on genuinely no-throw code. NEVER on anything that calls `fmt` (it can throw `format_error`). The exact allow/deny list is in AGENTS.md.
- Do not reformat by hand. clang-format 22 plus the pre-commit hook own formatting.
