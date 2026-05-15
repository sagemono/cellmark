# SPU tuning on PS3: one bug, one limit, four kernels at the ceiling

This is the writeup of auditing six SPU compute kernels that were running at 65–89% of theoretical peak. Same disassembly-driven approach as the PPE writeup. The result is shorter: the SPU kernels are mostly already at their practical ceiling, one had a real bug worth fixing, and one possible improvement (branch hints) is unreachable due to ISA encoding limits.

If you only read one section, read [§3 what the disassembly told us](#3-what-the-disassembly-told-us).

---

## 1. The starting point

Test platform:

| | |
|---|---|
| Console     | PS3 (CECHA00, original 60 GB fat) |
| Firmware    | Evilnat 4.92 CFW |
| Compiler | spu-lv2-gcc 4.1.1 |
| Compiler flags | `-W -Wall -O2 -funroll-loops` |

Initial cellmark Cell-page numbers, aggregated across all 6 SPEs at stock 3.2 GHz:

```
SP FMA Stress    134.8 / 153.6 GFLOPS    (87% of peak)
DP FMA Stress      8.7 /  10.97 GFLOPS   (79% of peak)
Int Multiply     130.1 / 153.6 GIOPS     (85% of peak)
Recip/Rsqrt       52.8 /  76.8 GOPS      (69% of peak)
Shuffle Storm     68.4 /  76.8 GOPS      (89% of peak)
Dual-Issue Max   202.1 / 230.4 GOPS      (88% of peak)
```

The four kernels at 85%+ of peak are doing roughly what hand-tuned SPU code is documented to do. IBM's own Cell Programming Tutorial cites 80–95% for well-written SPU kernels. Two stand out as below that band: **Recip/Rsqrt at 69%** and **DP FMA at 79%**.

---

## 2. The hypothesis space

The SPU has a simpler perf story than the PPE because there's no SMT and no hypervisor between userland and the silicon. The candidate causes for sub-peak SPU performance are well-defined:

1. **Pipe-pairing mistakes.** SPU is dual-issue: one even-pipe instruction + one odd-pipe instruction per cycle, in 8-byte aligned pairs. If two consecutive instructions are *both* even-pipe (or both odd-pipe), they serialise. The kernels use `lnop` (odd-pipe NOP) as filler to keep pairs balanced. Getting one of those wrong (`nop` is the *even-pipe* NOP, distinct from `lnop`) silently halves throughput in that block.
2. **Insufficient chain depth.** Each SPU operation has a documented latency (FMA: 6 cyc, DFMA: 13 cyc, shufb: 4 cyc, etc.). Dependent ops on the same chain must be N issue cycles apart where N >= latency, or they stall.
3. **Missing branch hints.** SPU has no dynamic branch predictor. A taken branch costs ~18 cycles unless preceded by an `hbrr`/`hbra`/`hbr` hint instruction issued at least 11 cycles before the branch.
4. **Per-batch DMA writeback overhead.** The SPU stress harness DMAs results back to the PPE every batch; the MFC queue work is small but non-zero.

---

## 3. What the disassembly told us

`spu-lv2-objdump -d` on the SPU ELF, kernel by kernel.

### 3.1 SP FMA / Shuffle / Dual at 87–89% - clean

The disasm of these kernels is exactly what the source asks for: tight `(even, odd)` pairs, all dual-issue, dependency chains spaced at the documented latency, no spills, no microcoded ops. The 11–13% gap from peak comes from:

- ~18-cycle taken-branch miss per outer iter (un-hintable, see §4)
- Per-batch DMA writeback to PPE
- Sysutil callbacks crossing the SPE→PPE boundary every batch
- General measurement noise

These are at their practical SPU ceiling.

### 3.2 Int Multiply at 85% - clean

Same picture. `mpya` (4-way SIMD multiply-add, 7-cycle latency) chained with 7 chains and 32x unrolled. Disasm matches source. ~15% gap is the same set of platform overheads.

### 3.3 DP FMA at 79% - disasm clean, gap unexplained

```
8280:  6b 91 e3 02   dfma  $2,$70,$71      <- even pipe, 13-cyc latency
8284:  00 20 00 00   lnop                   <- odd pipe, dual-issues
8288:  6b 91 e3 03   dfma  $3,$70,$71
828c:  00 20 00 00   lnop
...   (16x .rept of 13 dfmas + 13 lnops)
```

Pipe-pairing is correct. The 13-chain unroll covers DFMA's 13-cycle latency at the documented 1-per-7-cycle issue rate (DFMA stalls the even pipe for 6 cycles after issue). At peak that's `4 FLOPs / 7 cycles * 6 SPEs * 3.2 GHz = 10.97 GFLOPS`.

We hit 8.7. The kernel is doing what the source says, the source matches the documented DP throughput, and yet there's a ~21% gap unaccounted for. Possibilities (none verified):

- **The actual DFMA even-pipe stall on PS3-era Cell silicon may be longer than the documented 6 cycles.** Later PowerXCell 8i revisions improved DP throughput substantially and PS3 ships the early-revision Cell.
- **Per-batch overhead amortises poorly here** because DP FMA's intrinsic ops/cycle is much lower than SP FMA, so the same overhead is a bigger fraction of total time.

Without a per-cycle SPU trace tool it's hard to localise further. Marked as "what we don't know" rather than fixed.

### 3.4 Recip/Rsqrt at 69% - found a bug

The `and` block in the rsqrt section, lines 149–160 of the source, looked like:

```asm
        and     $38, $8,  $72       /* even pipe */
        nop                         /* even pipe!! */
        and     $39, $9,  $72
        nop
        ...
```

`and` is documented even-pipe (logical ops). `nop` (with no `l`) is the even-pipe NOP, distinct from `lnop` (odd-pipe NOP). The pair `and; nop` is even+even. The SPU dispatcher cannot dual-issue two same-pipe instructions, so each pair takes 2 cycles instead of 1.

Disasm at 0x92f0 confirmed:
```
92f0:  18 32 04 26   and  $38,$8,$72       <- even pipe
92f4:  40 20 00 00   nop  $0               <- even pipe (opcode 0x40)
```

Compare with the surrounding (correct) lnops, which use opcode `0x00 20 00 00`; different opcode, odd pipe.

Six instances. Each costs one extra cycle. Over a `.rept` block of 72 cycles, that's 6 cycles wasted = **8% of recip-kernel time**.

---

## 4. The branch-hint thing that almost worked

SPU has no dynamic branch predictor. Without a hint, a taken backward branch costs ~18 cycles to flush and refetch. All six kernels have that miss every outer iteration. For the smaller-bodied kernels it's a meaningful overhead (~4–5% on FMA, Shuffle, Dual).

The fix is `hbrr branch_label, target_label`. A hint instruction that updates the branch target buffer. Place it >=11 instructions before the branch and the miss disappears.

I tried adding hbrr at the top of each loop body, but the assembler refused:

```
spu/spu_fma_kernel.S:38: Error: Relocation doesn't fit. (relocation value = 0xc08)
```

The reason is in the SPU ISA encoding: **`hbrr` encodes the branch offset in a 9-bit signed field**, capping the hint→branch distance at ±256 instructions (±1024 bytes). Our loop bodies range 416–1152 instructions. Placing`hbrr` before `.rept` leaves the `brnz` out of range from any of them.

Two ways to make it work:

1. **Split each `.rept` into pre-hint and post-hint halves.** The hbrr sits between them, <=256 insns from the brnz. Adds ~30 lines of assembly per kernel. Estimated gain: ~3% per kernel.
2. **Replace one strategically-placed `lnop` inside the loop with `hbrr`.** Cleaner but requires the same `.rept` split (since `.rept` repeats identically).

Both are mechanical. Neither was written, the gain isn't worth the assembly complexity, and the un-hinted branch miss is a small fraction of an already-near-peak kernel for everything except recip (where the bug fix was the bigger win).

Documented in source comments for posterity; future work if someone wants the last few percent.

---

## 5. The fix that mattered

`spu_recip_kernel.S` lines 149–160: replace six `nop` (even-pipe NOP) with `lnop` (odd-pipe NOP):

```diff
+        /* and is even pipe; pair with lnop (odd) to dual issue */
         and     $38, $8,  $72
-        nop
+        lnop
         and     $39, $9,  $72
-        nop
+        lnop
         and     $40, $10, $72
-        nop
+        lnop
         and     $41, $11, $72
-        nop
+        lnop
         and     $42, $12, $72
-        nop
+        lnop
         and     $43, $13, $72
-        nop
+        lnop
```

Six lines! That's the entire kernel-side change.

---

## 6. Headline numbers

| Kernel        | Before        | After        | Δ        | % of stock peak |
|---------------|---------------|--------------|----------|-----------------|
| SP FMA        | 134.8 GFLOPS  | 134.0        | -0.6%    | 87.2%           |
| DP FMA        | 8.70 GFLOPS   | 8.6          | -1%      | 78.4%           |
| INT MUL       | 130.1 GIOPS   | 130          | flat     | 84.6%           |
| **Recip/Rsqrt** | **52.77 GOPS** | **56.43**  | **+6.94%** | **73.5%**     |
| Shuffle       | 68.43 GOPS    | 68.43        | flat     | 89.1%           |
| Dual-Issue    | 202.13 GOPS   | 202          | flat     | 87.7%           |

The +6.94% on recip lands within the predicted 7–8% range from disasm-counting. Five kernels unchanged within measurement noise, exactly as expected. The only the one with the bug had room to move.

---

## 7. What we don't know

- **Why DP FMA caps at 79% instead of close to peak.** The kernel is structurally correct (verified by disasm). Possible causes: longer DFMA stall on early Cell silicon vs. documented 6 cycles, harness overhead amortising poorly at low ops/cycle workloads, or something else. Marking this for a future investigation that has access to per-cycle SPU tracing.
- **Whether splitting `.rept` to enable `hbrr` would actually deliver the predicted ~3% per kernel** or whether there's a second-order penalty (alignment, ILBT prefetch overflow) that eats the gain. Worth a one-kernel experiment if someone has the appetite.
- **What the 11–15% gap on the four "near-ceiling" kernels actually consists of.** Best estimate: branch miss + DMA writeback + sysutil callbacks. Hasn't been broken down.

---

## 8. Sources

- Cell Broadband Engine SPU Instruction Set Architecture v1.2 - The authoritative reference for instruction pipe assignment, latency, throughput, and `hbrr`/`hbra`/`hbr` encoding constraints.
- SPE Programming Tutorial - Section on SPU pipeline and dual-issue rules.
- [Maximizing the power of the Cell Broadband Engine: 25 tips to optimal application performance](https://arcb.csc.ncsu.edu/~mueller/cluster/ps3/25tips.pdf) - Tips 17–20 cover SPU-specific dual-issue and branch handling.

---

*Investigation by sagemono, 2026. cellmark v2.0.0.*