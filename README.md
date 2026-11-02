# cray2sim: a CRAY-2 emulator, assembler and instruction-set reference

An instruction-level emulator of the four-processor **1985 CRAY-2**, written
from Cray Research's *CRAY-2 Computer System Functional Description* (HR-2000,
May 1985). It includes a clock-period timing model, an assembler for CAL
Version 2 syntax, and the instruction set as machine-readable data with
citations.

The CRAY-2 was the fastest computer in the world when it shipped in 1985. As
far as we know there was no runnable emulator of it. Existing Cray emulators
cover the CRAY-1/X-MP/Y-MP family, whose instruction set is quite different.

```
$ make && make test
$ python3 -m cray2.runprog examples/hello.cal --mem 4096=1ff,3,ffffffffffffffff --show 8192:3
EXIT 0 7 663 P=32 A=0,4096,1,64,8192,0,1,0 S=0,ff,a,1,4001c00000000000,4002900000000000,4002f00000000000,0
8192: 4002f00000000000 0000000000000005 0000000000000007
```

## Contents

| Path | Contents |
|---|---|
| `sim/cray2sim.c` | The emulator (below) |
| `sim/cray2fp.c`, `cray2fp.h` | CRAY-2 64-bit floating point (48-bit coefficient, 15-bit exponent, truncated-matrix multiply, reciprocal/rsqrt approximation and iteration, FIX/FLT) |
| `cray2/cal2.py` | Two-pass assembler for the machine-instruction subset of CAL Version 2 syntax (instructions, labels, `name = expr`, `CON`, `BSSZ`, `ALIGN`; no macros or other pseudo-ops). Writes a raw word image, a listing and a symbol table. `python3 -m cray2.cal2 in.cal out.bin` |
| `cray2/client.py` | Python client for the emulator's line protocol |
| `cray2/runprog.py` | Assemble and run a program on CPU 0 |
| `isa/cray2_isa.json` | All 148 instruction forms, each with semantics, functional unit, parcel count and a page/line citation into the 1985 manual (plus the 1989 revision's additions, marked) |
| `isa/ISA_NOTES.md` | Registers, memory organisation, floating point, and the manual's ambiguities and how each was resolved |
| `docs/TIMING.md` | The timing model: every parameter, its source, or an explicit "assumption" |
| `docs/PROTOCOL.md` | The host command protocol |

## What is emulated

- **Four Background Processors.** Each has 8 A (32-bit), 8 S (64-bit) and
  8 V (64 × 64-bit) registers, VL and VM, 16 Kword Local Memory, a real-time
  clock, and 8 × 16-word instruction buffers.
- **Every 1985 instruction** (opcodes 000–177). This includes vector
  gather/scatter, compressed iota, vector population count and leading-zero
  count, double shifts and vector merges.
- **The documented rules**, among them: a vector register used as both source
  and destination reads as zeros; VL = 0 means 64; mask bit 63 is element 0.
- **256 Mwords (2 GiB) of Common Memory** in 128 banks and 4 quadrants, with
  the base/limit field protection.
- **The job semaphore** (test-and-set jumps 004/005, set and clear 006/007).
- **Exits to the Foreground Processor.** The host plays the Foreground
  Processor through a simple protocol. The foreground I/O hardware itself
  (channels, disks) is not emulated.

## Timing

Each processor has its own clock. The model includes:
- in-order issue at one instruction per 2 clock periods (+1 per extra constant
  parcel);
- functional-unit reservation and register write and read reservations;
- **no chaining**: a vector result is usable only once complete;
- vector startup latencies;
- per-element Common Memory references with phased quadrant access and
  57-clock bank busy;
- Local Memory latency, and instruction-buffer misses.

In the default mode, processors execute strictly in order of their next
instruction's true issue time and share the bank timers. One simplification:
a vector memory instruction's element accesses all take effect when the
instruction issues, although their bank timing is modelled element by element.
This matters only for programs that race on data without synchronising.

`mt=1` selects a faster multithreaded mode. Its clocks are slightly
optimistic. In our model runs its results were bit-identical to the default
mode, but that is only guaranteed for programs that synchronise through
scalar memory words and the semaphore (see `docs/TIMING.md`).

Several numbers are not in any surviving document. These are vector
logical/integer startup, unit turnaround and branch cost; each is marked as an
assumption in `docs/TIMING.md` and can be overridden on the command line.

## Fidelity and limits

- **Checked only against the documentation.** No CRAY-2 survives in working
  order to compare against.
- **Unpublished approximation tables.** The reciprocal and reciprocal-sqrt
  approximation tables were never published. The emulator models them as a
  12-bit table plus the documented interpolation step, about 30 bits.
- **Iteration instructions are approximated.** The iteration instructions
  (126/127, 156/157) are modelled as an ordinary truncated multiply followed
  by a subtraction. The real hardware folds the constant into a special
  rounding pattern (HR-2000 figures 2-4 and 2-5), so the last bits of
  reciprocals and square roots may differ from a real CRAY-2.
- **Floating-add rounding.** The manual says the shifted operand is "rounded
  up". This is modelled as round-half-up of the shifted-out bit.
- **Memory effects not modelled:** refresh and quadrant-buffer backup stalls.
- **Not emulated:** the foreground system (Foreground Processor microcode,
  channels, disks), UNICOS, and CFT2/CFT77.

## Used by

[cray2-llm](../cray2-llm) runs an 8-billion-parameter language model on this
emulator at about 4 seconds of CRAY-2 time per token.

## License

MIT, for the code and our own documentation. The CRAY-2 manuals are Cray
Research documents and are not redistributed here. `isa/ISA_NOTES.md` includes
short passages quoted from HR-2000, marked as quotations and attributed, for
reference and commentary. A public scan of HR-2000 is at
<https://www.mrynet.com/cray/HR-2000-CRAY_2_Computer_System_Functional_Description-May_1985.OCR.pdf>.
