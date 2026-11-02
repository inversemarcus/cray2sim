# cray2sim timing model

Emulated Cray-2 time is counted in clock periods (CP) of 4.1 ns (1985 Cray-2
brochure; HR-02000 1989 Appendix B). Host wall time is never reported as
Cray time. The model follows documented mechanisms where they exist. Where the
documents give no number, it uses an explicit assumed value. Every value can be
overridden on the `cray2sim` command line (`name=value`) for sensitivity runs.

## Documented mechanisms implemented

| Mechanism | Source |
|---|---|
| 4 Background Processors, each with its own clock; real-time clock readable by 115 | HR-2000 §2.1.2 |
| In-order issue; an instruction waits in the issue position until its registers and functional unit are free | HR-2000 §2.1.1 |
| No chaining: a vector result register is reserved until the whole result is written | HR-2000 §2.2.3; Simmons & Wasserman 1988 §2.1 |
| One functional unit of each type serves vector instructions and is reserved for the whole stream | HR-2000 §2.4 |
| One result per CP per functional unit | HR-2000 §2.4 |
| A vector register that is both destination and source reads as zeros | HR-2000 §2.2.3 (implemented functionally) |
| 128 banks, low 2 address bits = quadrant, next 5 bits = bank | HR-2000 §4.1 |
| A processor reaches a quadrant only in its own phase (every 4th CP) | HR-2000 §4.2 |
| Bank Busy flag; a busy bank rejects the reference and it is retried | HR-2000 §4.3 |
| Gather (072) forced to 4 references per 16 CP | HR-2000 §4 note |
| 8 instruction-buffer fields of 16 words, oldest field replaced on a miss | HR-2000 §2.1.1 |
| One semaphore flag per job; 004/005/006/007 test, set and clear it | HR-2000 §2.1.3 and §3 |

## Numeric parameters

| Parameter | Value (CP) | Source / status |
|---|---|---|
| `issue` | 2 | At most one instruction every 2 CP (Dec-1982 design document; Simmons 1988: scalar instructions every other CP) |
| `extra_parcel` | 1 | Each constant parcel beyond the first adds 1 CP (Dec-1982 design) |
| `cm_bank_busy` | 57 | Original DRAM bank cycle (Simmons 1988 Table II, SN2003; "Cray-2 Vector Overheads.txt") |
| `cm_load_lat` | 59 | Scalar Common Memory access (Simmons 1988 §4.1; Vector Overheads.txt) |
| `cm_vec_tail` | 8 | The next vector memory instruction issues VL+8 CP later (Simmons 1988 eq. 1) |
| `v_fadd`, `v_fmul` | 22 | Vector add/multiply startup overhead (Vector Overheads.txt). Scalar FP is 19 (Simmons Table I) |
| `v_log`, `v_int` | 22 | **Assumption.** No published value. Set equal to floating-point startup; sensitivity range 4–30 |
| `s_fp` | 19 | Scalar FP add/multiply 76 ns (Simmons Table I) |
| `lm_scalar`, `lm_vector` | 4 | Local Memory access 4 CP (1985 brochure; Vector Overheads.txt "LM to V 4 clocks") |
| `unit_gap` | 2 | **Assumption.** CP between the last operand of one vector stream and the next stream on the same unit |
| `branch_taken` | 4 | **Assumption.** |
| `ibuf_miss` | 75 | **Assumption.** 59 CP access plus 16 words |
| `a_add`, `a_mul`, `s_int`, `s_shift`, `s_log` | 4, 8, 4, 4, 2 | **Assumptions.** Scalar latencies are minor in this workload |
| `sema` | 4 | Semaphore flags rotate among processors on a 4 CP window (Dec-1982 design) |

## Not modelled

- Memory refresh. Original DRAM refresh steals bank cycles; a rough estimate
  is single-digit milliseconds per second of streaming vector work.
- Quadrant-buffer backup stalls (§4.4). Bank conflicts are modelled, but the
  2-entry buffer limit is not.
- Foreground Processor I/O time. Data the host passes in or out while the
  processors are stopped at an exit is not counted.
- Exact operand-arrival timing inside a functional unit pipeline. Only startup
  plus one element per CP is modelled.

## Fast multithreaded mode

`cray2sim mt=1 [qwin=N]` runs each Background Processor on its own host thread,
synchronised every N clock periods (default 2,000). Within a window a
processor sees other processors' bank-busy times as of the window start, so
cross-processor bank conflicts inside a window are missed. Functional results
are identical to the single-threaded mode, provided programs synchronise
through the semaphore and Common Memory words, as real CRAY-2 programs must.
Clock counts read slightly low (about 0.8% on a bank-heavy workload) and vary
a little from run to run. Use the default single-threaded mode for timing.

