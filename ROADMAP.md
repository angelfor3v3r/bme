# BME Roadmap

Nothing here blocks the current release. Future improvements are ordered roughly by value to BME's core purpose, with recently completed work retained for context.

## Recently completed

- Floating-point environment seeds for `MXCSR` and the x87 `control_word`, including host-mask validation on Windows and Linux
- Raw binary input through `--file`, with exact byte semantics and no text-format inference
- Formatted `--bytes` input for contiguous or whitespace-separated hex, `\xNN` escapes, and `{ 0xNN, ... }` byte arrays
- Clear History state styling, selected-row clipboard copying, and normalized code, stack, data, end, and guard addresses with visible absolute bases
- Symmetric scratch guard pages, OS-reported memory-fault addresses, and read, write, or execute classification for Windows access violations and Linux x86 page faults
- **Run to row**, which reruns from the configured seed and checks the selected byte offset after every single step without patching the input

## Highest-value features

### Decoder divergence view

The History tabs expose each decoder, but users must compare them manually. Add a summary table keyed by byte offset that highlights differences in:

- Decode success
- Instruction length
- Operand rendering
- Instruction boundaries
- RIP-relative targets

Add **Next divergence** and **Previous divergence** navigation. This directly supports BME's primary purpose.

### Scratch-memory editor and viewer

Registers alone are insufficient for investigating load and store instructions. Useful capabilities include:

- Seed scratch memory as hex or ASCII
- Show the scratch-data region before and after execution
- Highlight modified bytes
- Copy an address or byte range
- Follow an address held in a register
- Inspect the sandbox stack where practical

Do not retain a complete 64 KiB snapshot for every step. At the maximum step count, that could consume gigabytes. Record changed ranges, dirty pages, or only initial and final memory until an efficient per-step design exists.

### Corpus replay and trace comparison

Add a bounded headless corpus runner that accepts explicit byte cases and writes one trace record per case. It should support resumable runs and reuse the normal engine, provenance, decoder histories, and limits rather than creating a second execution path.

Add a trace comparator that checks captures from different CPUs, operating systems, or BME builds and reports the first divergent execution event, register, flag, fault, or decoder boundary. Keep raw traces immutable and make comparisons a separate operation.

This provides immediate multi-machine differential analysis and a reusable execution layer for the eventual fuzzer.

### Windows worker-process execution

Windows currently executes supplied bytes in a thread within BME's process. Moving execution to a worker process would permit:

- Reliable wall-clock timeouts
- Safe cancellation
- Containment of `ExitProcess`
- Better containment of process-global state mutations
- Termination of blocked system calls without terminating the TUI
- Closer Windows and Linux execution parity

The Linux traced-child design already provides much of this isolation. This should precede any stronger claim that the sandbox contains process-level behavior.

## Smaller improvements

### Repeated-history folding

Optionally fold adjacent executions of the same instruction address in the TUI so bounded loops remain navigable. Expansion must recover every event, and JSON must preserve the uncompressed timeline.

## Larger future work

### Offline imports

Treat imported artifacts as immutable capture references. Clearly distinguish the capture CPU, platform, and address space from the current host. Opening an imported artifact must never execute its machine code. Any re-execution remains a separate, explicit action.

#### Trace import and inspection

The versioned export schema is available. Allow BME to load an exported trace for inspection while preserving its raw execution events, outcomes, decoder histories, and provenance.

#### Crash-dump parsing and context extraction

Add an in-tree reader for structured Windows minidumps and Linux ELF core files. Keep it limited to the records BME consumes and expose a platform-neutral data model that can be exercised headlessly. Enumerate captured threads, modules, virtual memory, and exception state. When an exception record exists, identify its faulting thread and preserve the exception code, address, and parameters as captured reference data.

Extract code bytes at the captured instruction pointer and translate supported GPR, RFLAGS, XMM, x87, `MXCSR`, and x87 control-word state from register groups that the captured context marks as present. Keep unsupported state such as a captured stack pointer available for inspection even when BME cannot seed it directly.

Model memory metadata and captured bytes separately. Distinguish an unmapped address, a mapped range whose bytes were not captured, and a truncated read. Never synthesize zeroes for absent dump data. Treat recognized records as optional and independent of directory order. In particular, a Windows `Memory64List` may appear without a `MemoryInfoList`, so captured ranges must be able to establish the memory map themselves. Ignore unrelated records without discarding an otherwise usable dump.

Preserve module names and sparse ranges through bounded random-access reads rather than copying the entire dump into memory. Parsing must be bounds-checked, overflow-checked, resource-capped, and architecture-gated. For Linux parity, translate core-file thread and floating-point notes into the same `Registers` representation used by the existing ptrace path.

#### Crash-dump navigation and replay setup

Build the interactive workflow on the parsed core model rather than coupling parsing to FTXUI. Default to the faulting thread when available, but allow selecting another thread or any mapped x86-64 address. Provide an address-ordered memory map with state, type, permissions, owning module and offset, and captured-byte availability. Support executable-region filtering, address jumps, and short byte previews.

Let the user explicitly load bytes at the selected virtual address into the code buffer and prefill supported seed fields from the selected thread. Reuse the scratch-memory editor to import selected data or stack ranges where they fit BME's controlled mappings. Preserve original virtual addresses for disassembly, add module-plus-offset annotations to history rows, and show the immutable captured exception and context beside any optional rerun result.

Before an explicit sandbox run, report unsupported state and any relocation that prevents faithful replay. This feature must remain focused on loading a real crash into BME's decoder, seed, and sandbox pipeline rather than growing into live process attachment or a general dump viewer.

### YMM, ZMM, and opmask state

Modern SIMD support eventually needs XSAVE-based capture of:

- YMM upper halves
- ZMM registers
- `K0..K7`
- Other enabled XSTATE components

This requires runtime CPUID and XGETBV gating, variable state layouts, OS support checks, and careful baseline code generation. Do not model it with fixed structures that assume AVX-512 is available.

### Decoder-divergence fuzzer

A dedicated differential harness is the long-term payoff:

- Structured encoding generation
- Execution pinned to one core
- Decoder boundary and result comparison
- CPU-result comparison
- Deduplication and minimization
- Reproduction artifact output
- Resumable corpus storage

Build it on the decoder-divergence model, corpus runner, trace comparator, and existing machine-readable trace schema so interactive and automated analysis share the same records.

## Deferred ideas

These currently offer little value relative to their complexity:

- Built-in assembler
- Plugin or scripting framework
- Networked trace sharing
- Persistent project databases
- Arbitrary debugger-style process attachment
- Source-level debugging
- General-purpose memory mapping outside controlled scratch regions

## Suggested order

1. Decoder divergence summary
2. Scratch-memory inspection
3. Corpus replay and trace comparison
4. Trace import and inspection
5. Crash-dump parsing and context extraction
6. Crash-dump navigation and replay setup
7. Windows worker process and wall-clock cancellation
8. XSAVE-based modern SIMD state
9. Dedicated fuzzer
