# RULES.md - bme

Hard, always-apply rules. Background and implementation detail live in AGENTS.md.

## Git

- NEVER commit, push, stage (`git add`), or create a commit, tag, or branch. The user handles Git state.
- Leave the pre-commit hook and `.github/workflows` alone.

## Build and generated files

- NEVER run a fresh CMake configure. It refetches and rebuilds dependencies.
- Reason from headers and source by default. If compilation is necessary, build incrementally in an existing configured directory. Never create, delete, or reconfigure one.
- Windows uses clang / clang++ from `C:\Program Files\LLVM`. Linux CI uses Debian 12 and Clang 22.
- Keep code generation at baseline x86-64 and SSE2. Never enable AVX, newer ISA extensions, `-march=native`, or equivalent.
- Never edit generated files. Edit `cmake/bme_version.hpp.in`, not generated `bme_version.hpp`.

## Interfaces and verification

- Preserve the interactive TUI.
- Exercise changed runtime behavior through headless `--quick`, never by driving the interactive TUI.
- Keep TUI and CLI / `--quick` capabilities in parity where practical.
- Unit tests live in `test/`, use GoogleTest, link `bme_core`, and run through `ctest` when `BME_BUILD_TESTS` is enabled.
- Test BME-owned parsing, seeding, engine orchestration, and `--quick` behavior. Never assert decoder correctness or CPU instruction semantics.

## Comments

- Use short plain ASCII prose. Explain only why or a non-obvious constraint. Keep longer rationale in AGENTS.md.
- Do not use `;`, `:`, or ` - ` as clause separators.
- Do not use unicode, em dashes, or smart quotes.
- Hyphens inside words, code tokens, and `->` are allowed.

## Code style

- Use C-style casts such as `(std::size_t)x`. Never use C++ named casts.
- Do not add `const` to local variables or pointer arguments. Use it on references and member functions where meaningful.
- Zero-initialize with `{}`, never `= 0`.
- Prefer `auto`.
- Use `emplace_back`, never `push_back`.
- C++ library and OS code lives in `namespace bme`. Keep `main` thin. Never use `using namespace ftxui`; qualify `ftxui::`. `using namespace bme` is test-only.
- Test plain bitmasks with `!= 0` or `== 0`. Compare with a flag value only when testing that exact flag. Avoid redundant parentheses around bitwise expressions.
- Use descriptive names. Single-letter names are only for trivial loop counters.
- Use `noexcept` only when the implementation and every callee genuinely cannot throw. Never use it around `fmt`, which can throw `format_error`.
- Let clang-format 22 and the pre-commit hook own source formatting. Never run clang-format on `src/st80.asm` or `src/st80.S`.
