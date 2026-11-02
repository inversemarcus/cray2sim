# cray2sim host protocol

`cray2sim` reads one command per line on stdin and answers on stdout. The host
plays the CRAY-2 Foreground Processor. Addresses are absolute Common Memory
word addresses. Words are hexadecimal in `w`/`r` and raw little-endian 64-bit
in files.

| Command | Reply | Meaning |
|---|---|---|
| `load FILE ADDR` | `OK n` | Copy a raw word file into Common Memory at ADDR |
| `w ADDR v1 v2 ...` | `OK n` | Write words (hex) |
| `r ADDR N` | `DATA v1 v2 ...` | Read N words (hex) |
| `dump ADDR N FILE` | `OK` | Write N words to a raw file |
| `start CPU PARCEL` | `OK` | Deadstart a Background Processor at a parcel address. Registers are cleared, and A7 = S7 = processor number |
| `resume CPU` | `OK` | Continue a processor after it exited |
| `go` | `EXIT cpu code clock P=.. A=.. S=..` lines, optional `IDLE clock`, then `END` | Run until a processor executes an exit (000/001) or all are idle |
| `regs CPU` | `REGS ...` | VL, VM and the first two elements of each vector register |
| `stats` | 4 `STAT` lines | Per-processor clock, instruction, element and functional-unit-busy counts |
| `timing NAME=VALUE` | `OK` | Change a timing parameter (see TIMING.md) |
| `quit` | | Exit |

Command-line arguments: timing overrides `NAME=VALUE`, `mt=1` for fast
multithreaded mode, `qwin=N` for its window size.

Exit codes are the low six bits of the exit instruction (`EXIT n` in CAL).
`EXIT 0` (`ERR`) is an error exit. A processor stopped at an exit keeps its
state and continues with `resume`. The other processors stop where they are
and continue on the next `go`.
