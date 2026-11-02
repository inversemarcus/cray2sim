# CRAY-2 Background Processor ISA notes

Companion to `cray2_isa.json`, which has one entry per instruction form (148 entries: 140 from the May 1985 manual, 4 hardware forms found only in the 1989 manual, and 4 assembler aliases that the 1989 manual documents). Every opcode from 000 through 177 octal has at least one entry.

## Sources and how they are cited

- **HR-2000 (May 1985)**: *CRAY-2 Computer System Functional Description*. This is the primary source, from the public scan at https://www.mrynet.com/cray/HR-2000-CRAY_2_Computer_System_Functional_Description-May_1985.OCR.pdf. Citations use line numbers in a plain-text extraction of that scan (`pdftotext -layout`) and the printed page numbers. Printed page 3-n is PDF page 38+n, 2-n is PDF 16+n, and 4-1 is PDF 123. In the OCR text each page's footer comes after that page's content.
- **HR-02000-0D (June 1989)**: the revised manual (in the Cray-2-Reboot project's documentation, https://github.com/DarkwaveTechnologies/Cray-2-Reboot). It was used only to settle ambiguities and to record differences. Citations use its printed page numbers.
- **CRAY-2 Description, Dec 1982**: a pre-production design document (image PDF, OCR'd with tesseract). **Its instruction set is different from the 1985 one** (for example 125 is the reciprocal iteration, 126 the reciprocal approximation, 132 FIX, Local Memory is 1024 words, and there are 16 semaphores selected by Ak). Use it only for design and timing background. Citations give the PDF page.
- **"Cray-2 Vector Overheads.txt"** (Cray-2-Reboot documentation): an undated survey paragraph. It mentions 512-Mword memory, the CRAY-2S and DD-40 disks, so it was written about 1988-90.
- **Other sources**: the 1985 CRAY-2 brochure (Computer History Museum, https://s3data.computerhistory.org/brochures/cray.cray2.1985.102646185.pdf) and Simmons & Wasserman 1988 (https://www.osti.gov/servlets/purl/6728393).

OCR caveat for HR-2000: the digit 6 is often read as 5 ("54-bit" means 64-bit, "54-element" means 64-element), and I/l/1 get confused. Every place where this mattered was checked against the PDF: pp. 2-14, 2-15, 2-18, 2-20, 3-4, 3-5, 3-6, 3-14, 3-15, 3-53, 3-57, 3-58, 3-83, 4-1, 4-2 and 4-3.

## Registers (per Background Processor; there are four processors)

| Register | Size | Notes / source |
|---|---|---|
| A0-A7 | 32 bits | Addresses and 32-bit integer arithmetic (2.2.1, lines 840-847) |
| S0-S7 | 64 bits | Scalar values; one S can supply an operand to a vector instruction (2.2.2, lines 849-862) |
| V0-V7 | 64 elements x 64 bits | Vector registers (2.2.3, lines 866-903) |
| VL | 6 bits | Values 0-63; 0 means 64. Captured when a vector instruction issues (2.3.1, lines 919-934, p.2-7) |
| VM | 64 bits | Bit 2^63 is element 0 and bit 2^0 is element 63 (2.3.2, lines 937-966; 1.2 conventions, lines 494-512) |
| P | 32 bits | **Parcel** address of the instruction in the issue position. It advances by 1 for each parcel (2.1.1, lines 603-614) |
| BA, LA | 32 bits each | Field base and limit. The Foreground Processor loads them while the processor is idle (2.1.4, lines 768-808) |
| RTC | 64 bits | Counts every clock period. The clocks in all processors are synchronized at deadstart (2.1.2, lines 734-741) |
| Local Memory | 16,384 x 64-bit words | Private to each processor. Not relative to BA, and no protection is described (2.4.10, lines 1121-1140) |
| Semaphore flag | 1 bit | The flag assigned to the job, selected through the Background Port Status (BPS) register (2.1.3) |
| Shared registers (1989, S/N 2025 only) | 8 x 32 bits | Accessed with 134/136/137 (HR-02000-0D 2.2.1, p.2-5; p.3-73) |

Bits are numbered right to left, so 2^63 is the MSB of an S or V value and 2^31 is the MSB of an A value (1.2, lines 494-512).

There are no B or T registers; Local Memory takes their place (brochure lines 213-224). The manual does not list an exchange package or any user-visible interrupt instruction.

### Special designator rules

- **No special meaning for A0, S0 or k=0** is given anywhere in Section 3 (1985 or 1989). Unlike CRAY-1, A0 is not an implicit branch operand, because branches test Ak or Sj explicitly.
- Two exceptions appear only in Appendix A and look like leftovers from the CRAY-1 manual. Treat them as unresolved:
  - A.6.4 (lines 5827-5845, p.A-8): "The VM register is zeroed if the j designator is 0." This would mean `034x0x` clears VM.
  - A.6.5 (lines 5850-5864): "enters the low-order 7 bits" into VL. This conflicts with the 6-bit VL. The same paragraph mentions "Si Vj,Ak and Vi,Ak Sj" single-element transfers, which have no CRAY-2 opcodes.
- **Vector source equal to destination**: "In the case of a Destination register that is the same as a Source register, the V register writing function takes priority over reading. When this occurs, the reading vector delivers all zero words to the functional unit" (2.2.3, lines 897-903, p.2-6).
  - The 1989 manual builds aliases on this rule: `143iii` "Vi 0", `144iji` "Vi Sj" (broadcast), `163iik` "Vi -Vk", and `173iik` "Vi -FVk".
  - **An emulator must implement it**: any V source register that equals i reads as 0.
  - The Dec-82 design said writing takes priority "but with a delay equal to the transit time", which gives a different recurrence. The 1985 wording overrides it.
- The i/j/k usage varies by instruction and needs care:
  - The data register is in **j** for 055, 047, 014-017 and 034.
  - The data register is in **i** for 057, 061, 063, 065, 067, 071, 073 and 075 (stores).
  - Scalar 132/133 take the operand in **j**, but vector 166/167 take it in **k**.
  - 110/111 use Si as both source and destination, with jk as the shift count.
  - 112 is `Si Si,Sj<Ak` and 113 is `Si Sj,Si>Ak`.
- Instructions that read Sj or Ak alongside a vector stream capture the value at issue and hold it in the functional unit.

## Instruction format and constants

- A parcel is 16 bits: f (7 bits, octal 000-177), then i, j, k (3 bits each). Four parcels are packed per 64-bit word, and parcels are addressed as if memory held 4x as many 16-bit locations (3.2, lines 1667-1683, p.3-2).
- Constant parcels follow the instruction. A single parcel (m1) is normally a Local Memory address or a 16-bit constant. Two parcels (m1 m2) are a 32-bit address or constant. Four parcels (m1-m4) are a 64-bit S constant.
- "The high-order data parcel is read first" (lines 1704-1710), so the value is (m1<<16)|m2 or (m1<<48)|(m2<<32)|(m3<<16)|m4.
- P is advanced past the constant parcels.
- This was checked against 1989 examples:
  - `042100 00004172107` = a1 1111111 (decimal 1111111 = octal 4172107).
  - `052100 10000300000` = s1 1.0, whose high 32 bits are 0x40018000.
- Instruction lengths in parcels:
  - **5 parcels**: 053.
  - **3 parcels**: 003, 004, 005, 010-017, 042, 043, 050-052, 064-067.
  - **2 parcels**: 040, 041, 044, 045, 054, 055.
  - **1 parcel**: everything else.
- The order of parcels inside a word is not stated. Assume the CRAY-1 convention (from the CRAY-1 hardware manual, P register): parcel 0 (the low 2 bits of P = 0) is bits 63-48 of the word, and the word address is P>>2.

## Jumps, calls, returns, exits

- **All branch targets are parcel addresses** relative to BA. The absolute instruction word is BA + (P>>2).
- **003 `j exp`** is an unconditional jump to (m1<<16)|m2.
- **010-013** test the 32-bit Ak (zero, nonzero, positive, negative). **014-017** test the 64-bit Sj. The target comes from m1 m2, and if the branch is not taken execution continues at P+3.
  - "Positive" is presumably the sign bit = 0, so 0 counts as positive. The manual does not say this.
- **002**, register jump. This is the only subroutine mechanism (there is no stack and no separate return-jump opcode):
  - `r,Ai Ak` (002ixk): P <- (Ak) and Ai <- (old P + 1), the parcel address of the next instruction.
  - `j Ak` (002kxk, i==k): jump through Ak; the return address overwrites Ak ("value in ak erased").
  - Call idiom: `r,A7 A6`. Return idiom: `j A7`.
  - Source: lines 1760-1793, p.3-5 (checked visually).
- **Exits (000/001)**: "stop the current program sequence, place the Background Processor in idle mode, and set the Exit Mode and Idle Mode flags in the Background Port Status register. The 6-bit jk value is entered into the Background Port Status register" (lines 1725-1758, p.3-4).
  - `err` = 000000. `exit` = 000001. `exit n` = 000x(n).
  - In 1985, 001xjk executes as 000. In 1989, 001xxx is **CMR, "Hold issue on memory busy"** (HR-02000-0D p.3-4, checked visually).
- **Halt and interrupt**:
  - 035 sets or clears "halt on memory field range error" (k=0 or 1, `dri`/`eri`) and "halt on floating-point error" (k=2 or 3, `dfi`/`efi`). These are BPS bits 21 and 22, but the manual does not say which bit is which (lines 2296-2333, p.3-19).
  - Memory range error (lines 810-820, p.2-5): an absolute address < BA or >= LA signals an error; a read returns 0 and a write is aborted.
  - Floating-point range errors also signal BPS if enabled (2.5.2).
  - What a "halt" looks like to the program (presumably the processor goes idle and the Foreground Processor is notified) is not described. Section 5 says the Foreground Processor "is transparent to the user". There is no user-level interrupt or exchange instruction.
  - For reference only, the Dec-82 design (PDF pp.39-43) had a 16-bit status register with enable, interrupt and status bits (semaphore error, field error, FP error, error exit, normal exit, idle), and FP-to-BP functions 00-07 (read status, read P, interrupt and idle, enter BA, enter LA, enter status, enter P and start). The 1985 bit numbering (21/22) shows the register changed, so do not reuse the Dec-82 layout.

## Semaphores (004-007)

From 2.1.3 (lines 744-766, pp.2-3/2-4) and Section 3 (lines 1834-1940):

- There are eight semaphore flags in the background system, one per active job. A processor uses the flag assigned to its job, selected through the BPS register.
- **004 `jcs exp`** jumps if the flag is clear. **005 `jss exp`** jumps if the flag is set. Both set the flag if it was not already set, so each is an atomic test-and-set plus branch. The BPS semaphore bit is set when the flag goes 0->1.
- **006 `ssm`** sets the flag unconditionally. The OS uses it to restore state on restart.
- **007 `csm`** clears the flag. The instruction issues immediately, but "the following instruction does not issue until the Common Memory quadrant buffers are clear", which gives release ordering for earlier writes.
- In 1989, S/N 2025 adds Shared registers: `137` "read and increment" is a fetch-and-add style primitive.

## Common Memory (Section 4, verbatim from PDF pp.4-1..4-3, checked visually)

> Common Memory contains 256 million words of dynamic memory. The dynamic memory consists of 128 banks with 2 million words in each bank. Each 72-bit word consists of 64-data bits and 8 error correction bits.
>
> Common Memory is organized into quadrants with 32 banks in each quadrant. Each memory quadrant has a data path to each of four Common Memory ports. A Background Processor and a foreground communication channel are connected to each Common Memory port. Total memory bandwidth is 64 gigabits per second. Total memory capacity is 17 gigabits.

(The OCR text reads "12-bit" and "11 gigabits"; the PDF says 72-bit and 17 gigabits.)

> 4.1 MEMORY ADDRESSING — A word in memory is addressed by 32 bits. The low-order 2 bits select the quadrants and the next 5 bits select the bank. [Figure 4-1: bits 2^31..2^7 Bank Address, 2^6..2^2 Bank Select, 2^1..2^0 Quad Select]
>
> 4.2 MEMORY ACCESS — The Background Processors are locked into a phased access time scheme with the memory quadrants through the Common Memory ports. Through its Common Memory port, a Background Processor can access any given quadrant but only in the processor's own phase time, that is, every fourth clock period (CP). If a Background Processor requests a quadrant out of its phase time, the request is delayed until the correct time.
>
> For example, assume the Background Processors are A through D, and the quadrants are 0 through 3. Also assume processor A is locked into quadrant 0 at phase time 0. If processor A references quadrant 0 at phase time 1, it must wait until the next phase time 0 (CP 4) to have access to memory in that quadrant.
>
> Memory banks in a quadrant share a data path to each Common Memory port. Because of the phased access time between the quadrants and the Common Memory ports, however, only one bank accesses the path in a given 4-CP time slot. Because two banks never compete for the same data path in the same time slot, each bank functionally has an independent path to each of the four Common Memory ports.
>
> 4.3 MEMORY CONFLICTS — To prevent memory conflicts, each memory bank has a Bank Busy flag. If the bank is busy, the quadrant sends a rejected signal to the requesting memory port. The requesting port retries the data.
>
> 4.4 MEMORY BACKUP — Memory backup occurs when too many memory references arrive at a single memory quadrant. Each Common Memory port has four quadrant buffers, one for each quadrant, each buffer can hold two memory references for its memory quadrant. Therefore, references can continue to the memory port when the reference is not in the proper phase time. When a quadrant buffer in a memory port is filled, and another reference to that quadrant is made, the memory port begins a backup procedure.
>
> The memory port backup procedure stops instruction issue for the associated Background Processor if that processor is making a memory reference. Vector streams initiated in the Background Processor and associated with a Common Memory reference are held.
>
> After all references have been submitted for retry, a stop issue is released allowing additional references to issue. A conflict during the retry process causes the backup procedure to begin again at the point the conflict occurred; which could be the original backup references or additional new references filling buffer positions that became empty during retry.
>
> NOTE — A special timing problem exists for execution of Background Processor instruction 072 (the gather instruction). This instruction allows addresses in any sequence with respect to the low-order 2 bits, quadrant select. Without special treatment of this instruction, the data could arrive at the Vector Destination register out of order. Therefore, the hardware forces a maximum memory reference pattern of four references and 12 null references which averages to one reference every 4 clock periods.

Sources: lines 4920-5036, pp.4-1..4-3. SECDED is described in 4.5 (lines 5041-5126).

Port priority (5.2.1, lines 5234-5257), highest first: refresh, then Background Processor operand references, then Background Processor instruction references, then foreground channel transfers.

How the 1989 revision changes this (HR-02000-0D pp.4-1..4-2):

- Configurations of 256 or 512 Mwords of DRAM, or 64 or 128 Mwords of SRAM; 64 or 128 banks; the 64-Mword system uses 4 bank-select bits.
- **Pseudo-banking**: each DRAM bank has two Bank Busy flags (long and short count) and is split into 2 or 4 pseudobanks. SRAM uses one busy flag per bank.
- The capacity is restated as 34 Gbits.

## Floating point (2.5.2, lines 1173-1615, pp.2-11..2-21)

**Format**

- Bit 63 is the coefficient sign (sign-magnitude, so negative values are not complemented).
- Bits 62-48 are a 15-bit exponent with a bias of 40000(8).
- Bits 47-0 are a 48-bit fraction with the binary point to the left of bit 47.
- Value = (-1)^s x 0.f x 2^(e - 40000(8)). The exponent range is 2^-20000(8) through 2^+17777(8), and precision is about 15 decimal digits.
- Examples in octal (sign, exponent, coefficient): 0 = 0 00000 0000000000000000, +1 = 0 40001 4000000000000000, -1 = 1 40001 4000000000000000.
- A number is normalized when coefficient bit 47 = 1; the normalized zero is all zeros.
- To float a 48-bit integer: insert exponent 40060(8), then add 0 to normalize.

**Range**

- An exponent >= 60000(8) is overflow and is detected by hardware.
- An exponent < 20000(8) is underflow. It is treated as zero and not reported.
- On overflow the result exponent is forced to a signature value, whether or not range errors are enabled:

| Operation | Forced exponent |
|---|---|
| add/subtract | 60000(8) |
| multiply | 60001(8) |
| reciprocal approximation | 60002(8) |
| square root approximation | 60004(8) |

**Add (120, 170, 171) and subtract (121, 172, 173)**

- The result is always normalized.
- The coefficient shifted for exponent alignment is "rounded up".
- A zero result coefficient gives an all-zero word.
- An operand with an overflow exponent gives an error (if enabled) and exponent 60000(8).
- Dec-82 design detail: a 49-bit adder with a round bit of 1 in the 49th position for the larger operand and 0 for the smaller (PDF p.33).

**FIX (122, 174)**

- Works by adding a constant integer. The result is at most 48 bits and is sign-extended to 64 bits (p.2-14, checked visually).
- A value larger than a 48-bit integer is an error (if enabled) and gives 0.
- The Dec-82 design gives the constant as 0 40060 0000000000000000, with no round bit, so FIX truncates.

**FLT (123, 175)**: add to a constant and normalize. Only 48 bits of the operand are used; higher bits are discarded with no error.

**Multiply (124, 154, 155)**

- Product of 48x48 bits, truncated 8 bits below the result LSB.
- Rounding constant at 2^-49..2^-56 = `0 1 1 0 1 0 0 1` (fig 2-3, p.2-15).
- Special cases:
  1. An overflow operand, or 2. an overflow exponent sum: error and exponent 60001(8).
  3. An underflow exponent sum: result 0.
  4. Both exponents zero: integer-style product with no normalize and exponent 0. The 1989 manual adds "and sign bit".
- Integer multiply is done with the floating multiply (2.5.1).

**Reciprocal approximation (132, 166)**

- Table lookup on the high-order 12 coefficient bits, then interpolation R = 2A - A*A*B, where the table gives 2A and -A*A (p.2-16).
- An overflow or underflow operand exponent gives an error (if enabled) and exponent 60002(8).
- Dec-82: a 4096 x 26-bit ROM giving a 25-bit approximation. **The table is not published, so a bit-exact emulation is impossible from these documents.**

**Reciprocal iteration (126, 156)**

- Computes 2 - a*b. Use it only once, and only on the output of 132/166; the result has 46 coefficient bits.
- Rounding: bits 2^-1..2^-48 = 1, and 2^-49..2^-56 = `1 0 1 1 0 0 1 0` (fig 2-4, p.2-18).
- Division (p.2-17): S3 = /HS2; S4 = S2*IS3; S5 = S3*FS4; S6 = S1*FS5.

**Reciprocal square-root approximation (133, 167)**

- Interpolation R = 3A/2 - A^3*B/2.
- Special cases: an overflow exponent, an exponent of 0..3, or a negative operand. All three force exponent 60004(8); overflow and negative also signal an error.

**Reciprocal square-root iteration (127, 157)**

- Computes (3 - a*b)/2, with 46-bit accuracy.
- Rounding: 2^-49..2^-56 = `0 1 0 0 0 1 1 0` (fig 2-5, p.2-20).
- Square root sequence:
  - 1985 (p.2-21): S2 = *QS1; S3 = S1*FS2; S4 = S2*QS3; S5 = S3*FS4.
  - 1989 (p.2-19) adds a step "S3 = 1; S4 = S1 ! S3 — force X odd before doing the iteration". The operator is garbled in the OCR.

## Functional units (2.4, lines 972-1140)

| Unit | Opcodes (1985) |
|---|---|
| Address Add | 020, 021 |
| Address Multiply | 022, 023 |
| Scalar Integer | 104-107 |
| Scalar Shift | 110-113 |
| Scalar Logical | 100-103 |
| Vector Integer | 150-153, 160-165, 176 |
| Vector Logical | 140-147 and 030-033 |
| FP Add | 120-123, 170-175 |
| FP Multiply | 124-127, 132, 133, 154-157, 166, 167 |
| Local Memory | 044-047, 054-057, 074, 075 |
| Common Memory port | 060-073 |

- Each unit delivers one result per CP. Non-vector units accept operands as fast as instructions issue. There is one unit of each type, and a vector operation reserves its unit for the whole stream.
- **There is no chaining** (Simmons 1988, p.4).
- In 1989, systems with vector tailgating (S/N 2025, 2027 and later) add a Vector Shift unit for 150-153, 164 and 165, and move 030-033, 146 and 147 to the Vector Integer unit. Tailgating lets a later vector instruction use, as its destination, a register that an unfinished instruction is still reading. It is not available for Common Memory or compressed-iota instructions (HR-02000-0D pp.2-8..2-10).

## Timing numbers found (clock periods, CP)

| Value | Context | Source |
|---|---|---|
| 4.1 ns clock | production CRAY-2 | brochure lines 32, 52; HR-02000-0D Appendix B spec sheets (p.B-3 etc.); Vector Overheads.txt line 8 |
| 4 ns clock period, 250 MHz oscillator | 1982 design | Dec82 PDF p.4 |
| 1 result per CP per functional unit; "transit time delay" not quantified | all units | HR-2000 2.4 (lines 972-975, 1068-1072, 1083-1084, 1097-1100, 1114-1117) |
| Quadrant access only in own phase, every 4th CP; gather 072 forced to 4 refs + 12 nulls (average 1 ref per 4 CP) | Common Memory | HR-2000 4.2, 4.4 NOTE |
| 2 references per quadrant buffer, 4 buffers per port | Common Memory | HR-2000 4.4 |
| RTC increments every CP | — | HR-2000 2.1.2 |
| Scalar 132/133 place a 4 CP reservation on FP Multiply | 1989 | HR-02000-0D 2.4.10 p.2-11 |
| Shared registers: 8-bit path, 4-packet / 4-CP transfer | 1989 S/N 2025 | HR-02000-0D 2.2.1 p.2-5 |
| Local Memory access time 4 CP | 1985 | brochure lines 220-222 |
| Scalar FP add 76 ns (19 CP), multiply 76 ns (19 CP), divide 88 ns (22 CP) | Simmons & Wasserman 1988, Table I | simmons-page4.png |
| Memory cycle 234 ns (57 CP) on early DRAM CRAY-2; pseudobanking reduces it to 33 CP | Simmons 1988 p.4 | simmons-page4.png |
| Next vector memory instruction issues VL+8 CP later; T(min) = (VL+8)/VL CP per word (72/64 = 1.125 for VL=64, 217 Mword/s) | Simmons 1988 eq.1 | simmons-page4.png |
| Bank busy 57 clocks; scalar memory access 59 clocks; Local Memory to V registers 4 clocks; LM vector references unit-stride only | survey, about 1988-90 | Vector Overheads.txt lines 8-12 |
| Vector overheads: 63 cycles load, 22 multiply, 22 add, 63 store | same survey | Vector Overheads.txt lines 14-18 |
| Memory chip access 100 ns, cycle 160 ns; bank-to-port packet 3 CP x 24 bits, port-to-bank packet 4 CP | 1982 design | Dec82 PDF p.5 |
| V-register chips 2-CP cycle (4 banks of 16 elements) | 1982 design | Dec82 PDF p.24 |
| Local Memory chips 3-CP cycle; scalar LM reservation 3 CP; vector LM reservation 4 + VL CP; vector LM needs 1 extra CP setup | 1982 design (1024-word LM) | Dec82 PDF p.27 |
| Maximum issue rate 1 instruction per 2 CP; each constant parcel beyond the first delays the next issue by 1 CP; source operands are read in the 2 CP after issue | 1982 design | Dec82 PDF p.29 |
| Instruction stack fetch of 32 words; refetch when within 64 parcels of the end | 1982 design | Dec82 PDF p.28 |
| Semaphore flags rotate among processors, giving exclusive access in a 4-CP unit | 1982 design | Dec82 PDF p.38 |
| Instruction buffer: 8 fields x 16 words = 128 words (1989: 16 or 32 words = 128 or 256) | — | HR-2000 2.1.1 lines 706-717; HR-02000-0D p.2-3 |

The issue interval of 2 CP and the absence of chaining come from Simmons & Wasserman 1988, section 2.1.

## 1985 vs 1989 differences (HR-02000-0D, June 1989)

1. 001xxx becomes `CMR`, "hold issue on memory busy". In 1985 it executes as 000xjk.
2. 134/136/137 become Shared-register instructions (`SRj Ak`, `Ai SRj`, `Ai SRj+`) on S/N 2025 only. 135 stays a pass. The printed Result and Operand columns look swapped.
3. Assembler special forms are documented: `Vi 0` = 143iii, `Vi Sj` = 144iji, `Vi -Vk` = 163iik, `Vi -FVk` = 173iik.
4. Vector tailgating and the Vector Shift unit; 030-033, 146 and 147 move to Vector Integer on tailgating systems.
5. 025: the 1985 text says the "high-order 24 bits" are 0 (an error in the 1985 text, checked visually); 1989 says 26.
6. 036: 1989 adds "A value of 0 in the VL register is interpreted as 64."
7. 071: 1989 adds that strides may be negative or zero.
8. 110/111: CAL maps a count of 0 to 111i00 and 64 to 110i00.
9. 064/065: CAL converts a zero offset to 062/063.
10. Multiply case 4: the sign bit is also 0.
11. The square root algorithm adds a "force X odd" step.
12. Memory: pseudobanking, SRAM options and 512-Mword configurations; the instruction buffer can be 256 words.
13. 2.4.10: the 4-CP reservation for scalar reciprocal and square-root approximations.

## Uncertainties and ambiguities that remain open

1. **001**: 1985 says it executes as an exit; 1989 says it is the CMR barrier. Pick one per target machine.
2. **034 with j=0 and the 7-bit VL** (Appendix A.6.4/A.6.5) conflict with the main text. These are probably CRAY-1 leftovers. Recommendation: no special S0 behaviour; VL = Ak & 077.
3. **Parcel order within a word** is not stated (assume parcel 0 = bits 63-48), and neither is whether BA is a word address (assumed).
4. **"Positive" tests** (012, 016, 032): the manual does not say whether zero counts as positive (assume the sign bit is tested, so it does).
5. **035**: which of BPS bits 21 and 22 belongs to range halt and which to FP halt; k=4..7 are undefined. The full BPS layout is not documented.
6. **What "halt" means** after a range or FP error with halt enabled, the value of P after an exit, and restart semantics.
7. **Local Memory addresses >= 16384** (wrap?), and whether LM has any protection.
8. **Common Memory address arithmetic width**: Aj+Ak, Ak+exp, the stride products and gather offsets are assumed mod 2^32 before BA is added and the range check is applied. Duplicate-address order in scatter 073 is undocumented.
9. **176 compressed iota**: interaction with VL, element width or sign extension of the iota values, the contents of Vi beyond popcount(Sj), and whether 177 honours its ijk fields ("177xxx executes same as 176ijk").
10. **124** text says operands come from "Si and Sk" (both editions); the box says Sj and Sk. Assumed Sj.
11. **Floating-point bit-exactness**: the reciprocal and rsqrt ROM tables are not published; add rounding ("rounded up") is not specified bit-exactly; the exact FIX/FLT constants come only from the 1982 design. For bit-exact results these need another source, such as the CRAY-2 simulator or hardware reference manuals.
12. **Vector dest==source zero-read rule**: documented in 1985 (and relied on by the 1989 aliases). The Dec-82 design describes a delayed recurrence instead. Follow 1985.
13. **152/153**: the text says shift counts "greater than 128" give zero, while 112/113 say ">= 128". A count of exactly 128 yields 0 arithmetically either way.
14. **Shared registers (1989)**: the text says eight 32-bit registers, but the instructions only allow j=0 or 1. Whether 137 returns the value before or after the increment is not stated.
15. **025 after VL=64**: VL is a 6-bit register, so presumably it reads back as 0.
16. **Instructions not documented at all**: 106 and 164 with k=2..7, and 035 with k=4..7. They are marked as reserved entries in the JSON.
