# BME Roadmap

Nothing here blocks the current release. These are possible future improvements, ordered roughly by value to BME's core purpose.

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

### Seed the floating-point environment

Allow explicit seeds for:

- `MXCSR`
- x87 control word
- Possibly x87 status and TOP where safe and meaningful

Rounding mode and exception masks materially affect results. Without these seeds, users need setup instructions such as `LDMXCSR` or `FLDCW` before the instruction under investigation.

### Raw-file input

Add binary-file input for longer sequences and generated corpora:

```text
bme --file code.bin
```

Keep `--bytes` for short interactive cases. Text formats such as C arrays and `\x48\xff` strings are lower priority.

### Run to selected row

Add a **Run to cursor** action that continues until:

- RIP reaches the selected address
- Execution finishes
- A fault occurs
- The step cap is reached
- A wall-clock timeout is reached

Check RIP after each step rather than patching the code with `int3`. Decoder boundaries may disagree, so the target should be an address or offset rather than a decoder-specific row identity.

### Normalized address display

Offer an offset-oriented display alongside absolute addresses:

```text
code+0000
code+0003
data+0010
stack-0028
```

Normalized offsets make traces stable across ASLR and easier to compare between platforms and runs.

### Clearer static-history state

Keep scrolling into not-reached rows, but distinguish execution and static browsing visually:

- Reached rows shown normally
- Fault rows shown in red
- Stop rows emphasized
- Not-reached rows dimmed
- A `Static` or `No execution state` indicator while a not-reached row is selected

This preserves useful disassembly browsing while clarifying that no register snapshot exists for the selected row.

## Larger future work

### Trace import and offline inspection

The versioned export schema is available. Allow BME to load an exported trace for inspection without executing its machine code. Preserve the capture CPU and platform metadata, raw execution events, outcomes, and decoder histories. Clearly distinguish the capture host from the current host.

Import must not imply replay. Re-executing imported bytes on the current machine should remain a separate, explicit operation.

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

Build the divergence comparison on the existing machine-readable trace schema so the fuzzer consumes the same stable execution and decoder records.

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
3. Windows worker process and wall-clock cancellation
4. Floating-point environment seeds
5. XSAVE-based modern SIMD state
6. Dedicated fuzzer
