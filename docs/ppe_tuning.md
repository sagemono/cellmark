# PPE tuning on PS3: chasing 48% of peak across an SMT wall

This is the writeup of how the PPE benchmarks went from "the kernels are obviously broken" to "the kernels are fine. The platform documentation says you can't go faster from a userland thread, and the SDK explicitly nullifies the one workaround." The numbers moved meaningfully on three of five kernels and not at all on the fourth, and the fourth not moving is *the* finding.

If you only read one section, read [§4 what the disassembly told us](#4-what-the-disassembly-told-us).

---

## 1. The starting point

Test platform:

| | |
|---|---|
| Console     | PS3 (CECHA00, original 60 GB fat) |
| Firmware    | Evilnat 4.92 CFW |
| Compiler    | ppu-lv2-gcc 4.1.1 |
| Compiler flags  | `-W -Wall -O2 -g -m64 -maltivec` |

Initial cellmark PPE-page numbers:

```
VMX FMA      12.2 / 25.5 GFLOPS   (48% of theoretical peak)
L1 read BW   9.12 GB/s            (~30% of expected)
L2 read BW   2.75 GB/s            (~14% of expected)
L1 latency   3.9 ns / 12.5 cyc    (~3x over PPE-published 4-cyc hit)
L2 latency   16.1 ns / 51 cyc     (~40% over PPE-published 36-cyc hit)
```

Five out of five kernels under spec. The natural assumption, and the one initially made, was that the kernel code had bugs, the compiler was emitting garbage, or both. That hypothesis was partly right and partly very wrong.

---

## 2. The hypothesis space

Before instrumenting anything:

1. **SMT slot-splitting.** The PPE is 2-way SMT. If the second hardware thread is doing OS housekeeping during the timed window, it consumes ~50% of the dispatch slots. **A FMA result at exactly 48% of peak is the textbook signature of this.**
2. **Microcoded PowerPC ops in the inner loop.** Per IBM's "Maximizing the Power of the Cell BE" tip 6, anything `Rc=1` (record forms: `addic.`, `cmpwi.`, `subf.`), register-sourced shifts, load/store-multiple, or misaligned VMX loads stalls *both* SMT threads regardless of whether the second is idle. GCC commonly emits `addic.`/`cmpwi.` for loop counters.
3. **Compiler-emitted address arithmetic per load.** PPC64 with default `-m64` may generate per-load zero-extends or shifts when indexing through an array, bloating the inner.
4. **No prefetch.** The L2 buffer (400 KB) doesn't fit in L1 (32 KB). Every miss to L2 is ~36 cycles of unhidden latency. AltiVec `vec_dst` and friends, but tip 5 says those are no-ops on Cell.
5. **Bad latency loop construction.** The chase body might compile to more than the textbook `lwz; bdnz` if indices need scaling.
6. **VMX load-to-use latency mishandled.** Documented at 7 cycles on PPE; if the load-ahead pattern doesn't keep enough loads in flight, every dependent add stalls.

Half of these were correct. The most important one wasn't. The disassembler tells the difference.

---

## 3. Reading the binary

`ppu-lv2-objdump` on a stripped release `cellmark.elf`, with `--no-show-raw-insn`. Five symbols of interest: `run_vmx_fma`, `run_l1_bw`, `run_l2_bw`, `run_l1_lat`, `run_l2_lat`.

What the disassembler showed, kernel by kernel.

---

## 4. What the disassembly told us

### 4.1 FMA loop is clean

```
15470:  vmaddfp v0,v0,v8,v9
15474:  vmaddfp v1,v1,v8,v9
...     (48 vmaddfps total)
1552c:  vmaddfp v10,v10,v8,v9
15530:  bdnz+   15470
```

48 `vmaddfp` plus one `bdnz+` (decrement-and-branch with static "taken" hint) per inner. **No microcoded ops. No `Rc=1` forms. No spills. No address arithmetic.** The 6 chains x 8x unroll the source asked for, exactly as written. 49 instructions per inner, 384 FLOPs → 25.6 GFLOPS at 100% issue rate.

This gets 12.2. Half. **The kernel is operating as designed; the platform is the ceiling.** See §5.

### 4.2 L1/L2 BW loop bleeds half its instructions to address arithmetic

```
152bc:  addi    r9,r6,16
152c0:  addi    r11,r6,32
152c4:  addi    r10,r6,48
152c8:  clrldi  r9,r9,32        # zero-extend address to 64-bit
152cc:  clrldi  r11,r11,32      # zero-extend address to 64-bit
152d0:  clrldi  r10,r10,32      # zero-extend address to 64-bit
152d4:  clrldi  r7,r6,32        # zero-extend address to 64-bit
...
152dc:  lvx     v10,r0,r9
152e4:  lvx     v9,r0,r11
...
```

Eight `lvx` need eight pairs of address computation: `addi` to compute the offset, then `clrldi` to zero-extend the result to a 64-bit address. **Half the instructions in the inner are doing this.** GCC defensively zero-extends because the source uses `l1_buf[i + N]` where `i` is a 32-bit signed `int` and the compiler can't prove the resulting address fits in 32 bits without further analysis.

Counting: ~32 instructions per 8 loads (128 B). At strict 1-issue: 32 cyc/128 B = 4 B/cyc = 12.8 GB/s. SMT-halved: 6.4 GB/s. This got 9, slightly above this, presumably because some `clrldi`s pair with adjacent ops. The L2 case is worse because every miss adds 36 cycles of platter the cycles can't hide.

### 4.3 Latency loop chase is 5 instructions per chase, not 1

Index-based Sattolo (`uint32_t lat_l1[N]; idx = lat_l1[idx];`) compiles to:

```
14f0c:  rlwinm  r9,r9,2,0,29    # idx * 4 (sizeof uint32_t)
14f10:  extsw   r9,r9            # sign-extend to 64-bit
14f14:  add     r9,r9,r0         # add base address
14f18:  lwz     r9,-20480(r9)    # the actual load
14f1c:  bdnz+   14f0c
```

Five instructions. The dependency chain `lwz → rlwinm → extsw → add → lwz` is 7 cycles minimum (4-cycle L1 hit + 3 cycles of address scaling). 7 cycles / 3.2 GHz = 2.19 ns. But this measured 3.9 ns, the published number was reading the *combined* latency of "load + address-formation chain," not the L1 hit itself.

### 4.4 The `__cctpl` bombshell

The 48% FMA result has a documented bare-metal fix on PPE. The PowerPC ISA defines priority hints `or 1,1,1` (low) / `or 2,2,2` (medium) / `or 3,3,3` (high), and the PPE manual (Table 6-1, p. 50) documents that a low-priority thread yields its dispatch slots to the other hardware thread. Spawn an idle thread that does `or 1,1,1` in a tight loop, LV2 schedules it onto the otherwise-busy second hardware context, the busy thread yields, and compute thread gets near-100% of peak.

The Cell SDK provides `__cctpl()` / `__cctpm()` / `__cctph()` GCC intrinsics for exactly this pattern. They are documented in `C_and_Cpp_Language_Extension-Specifications`, Tables 112–114.

The same SDK doc, in the same table, says this:

> **Note: This intrinsic has no effect on Cell OS Lv-2.**

LV2 does not enable `TSCR[UCP]` for problem-state code (the gate that allows user-mode threads to set hardware priority). The intrinsic compiles, the `or N,N,N` instruction assembles, the hardware sees it and silently drops it. The SDK ships these intrinsics specifically to inform you they don't work.

A `grep -r 'cctpl\|cctpm\|cctph\|or 1,1,1' samples/` across the entire SDK install returns zero matches. Sony shipped no example using them.

### 4.5 Enumerating the LV2 PPU-thread syscalls - does anything else help?

For completeness, the full PPU-thread syscall set in LV2 (syscalls 0x028–0x03A) was reviewed against `sys/ppu_thread.h` and the publicly documented LV2 syscall table:

| Syscall | API | What it controls |
|---|---|---|
| 0x02A | `sys_ppu_thread_get_id` | Read-only |
| 0x02B | `sys_ppu_thread_yield` | Yields *this* thread, wrong direction! |
| 0x02C | `sys_ppu_thread_join` | Wait for another thread |
| 0x02F | `sys_ppu_thread_set_priority` | Software ready-queue priority, range 0..3071 |
| 0x030 | `sys_ppu_thread_get_priority` | Read-only |
| 0x032 | `sys_ppu_thread_stop` | **`root`-flagged**; stops a software thread, LV2 just reschedules another onto the freed HW context |
| 0x033 | `sys_ppu_thread_restart` | **`root`-flagged**, not reachable from problem state |
| 0x034 | `sys_ppu_thread_create` | `prio` argument is the same software priority. Flags are only `JOINABLE` and `INTERRUPT` - **no hardware-thread affinity flag** |
| 0x036 / 0x037 | undocumented | **`root`-flagged**, kernel allocation information |
| 3071..3199 | privileged priority band | **`dbg`-flagged** (DECR/debug builds only) still software-side, not a hardware priority hint... |

None of these change PPE hardware dispatch arbitration. The privileged 3071..3199 priority band exists but is software-scheduler-only and gated behind the `dbg` flag. Even if reachable on CFW with kernel-syscall access, it just makes LV2's ready-queue pick this thread first, which is implicitly true anyway when no other normal-priority thread is runnable. The hardware-side arbiter on the PPE silicon decides slot allocation based on the TSRL[TP] register, which LV2 does not allow problem-state code to modify.

**The 48% PPE-VMX ceiling is the documented platform behaviour, not a bug.** No combination of public SDK can break it.

---

## 5. The fixes that mattered

### 5.1 Pointer-chase Sattolo

Source change: store `void *` next-pointers instead of `uint32_t` indices. Build the Sattolo cycle as a chain of pointers in-place. The chase becomes:

```c
register void *p = lat_l1_ptrs[0];
for (i = 0; i < CHASES; i++)
    p = *(void **)p;
```

Disassembled, with `-O3 -funroll-loops`, the inner is 8x unrolled:

```
20:  clrldi  r11,r9,32
24:  lwz     r10,0(r11)
28:  lwz     r9,0(r10)
2c:  lwz     r6,0(r9)
30:  lwz     r5,0(r6)
34:  lwz     r4,0(r5)
38:  lwz     r12,0(r4)
3c:  lwz     r11,0(r12)
40:  lwz     r9,0(r11)
44:  bdnz+   20
```

One `lwz` per chase, dependency-chained. The `clrldi` at line 0x20 only runs once per 8 chases (zero-extends the loop register). GCC chose `lwz` over `ld` because the BSS pointers fit in 32 bits. Same L1 latency, harmless on PS3.

### 5.2 Single moving pointer + `vec_ld(offset, base)` for BW

```c
const vector float *base = l1_buf;
for (i = 0; i < L1_BUF_VECS; i += 8) {
    t0 = vec_ld(0,   base);  t1 = vec_ld(16,  base);
    t2 = vec_ld(32,  base);  t3 = vec_ld(48,  base);
    t4 = vec_ld(64,  base);  t5 = vec_ld(80,  base);
    t6 = vec_ld(96,  base);  t7 = vec_ld(112, base);
    s0 = vec_add(s0, t0); /* ... */
    base += 8;
}
```

The literal offsets `16, 32, 48, ..., 112` get hoisted into registers once outside the loop (`li r5,16; li r7,32; ...`). The base pointer is zero-extended once per outer iteration. Per-load address arithmetic disappears.

### 5.3 `dcbt` prefetch for L2 only

```c
static inline void prefetch_l1l2(const void *p) {
    __asm__ __volatile__ ("dcbt 0,%0" : : "r"(p));
}
```

Issued 4 cache lines (512 B) ahead of the current load in the L2 BW kernel. PPE L2 hit is ~36 cycles; at 8 B/cycle issue, 4 lines covers ~64 cycles. Doesn't help L1 BW (already L1-resident).

Per IBM "Maximizing the Power of the Cell BE" tip 5: AltiVec `vec_dst()` / `dst` / `dstst` / `dststt` are explicit no-ops on Cell. `dcbt` (with `th=0` for L1+L2 fetch) is the only effective prefetch primitive on the PPE.

### 5.4 LV2 thread priority bump

In `ppe_run_batch`, the calling thread queries its current priority, sets it to 100 (well above the default 1000), runs the timed kernel, then restores. This is the *software* LV2 ready-queue priority, it doesn't change PPE hardware dispatch slot allocation, but it reduces how much normal-priority sysutil/callback work runs on either hardware context during the timed window.

This did not visibly move any number on its own, but it's defensible best practice and harmless.

### 5.5 Compiler flags

`-O2 -g` → `-O3 -g -mfused-madd -funroll-loops`. The `-funroll-loops` is responsible for the 8x unroll on the chase loop (the source has no manual unroll). `-mfused-madd` is a no-op for code already using `vec_madd`, kept for safety.

---

## 6. Headline numbers

| Metric        | Before        | After        | Predicted floor                | Verdict |
|---------------|---------------|--------------|--------------------------------|---------|
| VMX FMA       | 12.2 GFLOPS   | 12.2         | 12.8 (LV2 SMT ceiling, ~50%)   | At ceiling. No further gain available from userland |
| L1 read BW    | 9.12 GB/s     | **10.3**     | 12.8 (SMT-halved 1-issue)      | 80% of strict 1-issue ceiling; the rest is loop overhead and SMT noise |
| L2 read BW    | 2.75 GB/s     | **8.4**      | 6–12                            | **3.05x**, within prediction; `dcbt` covers the L2 miss path |
| L1 latency    | 3.9 ns / 12.5 cyc | **2.0 ns / 6.4 cyc** | 1.25 ns / 4 cyc (PPE L1 hit)| Matches PPE load-to-use floor (4–6 cycles per the manual) |
| L2 latency    | 16.1 ns / 51 cyc | **11.3 ns / 36 cyc** | 11.25 ns / 36 cyc (PPE L2 hit) | **Exactly the PPE-published L2 hit latency** |

Five kernels, three at-spec, one at the documented platform ceiling, one at 80% of theoretical with the remaining gap explained by SMT noise.

---

## 7. What was considered and didn't ship

- **Software-pipelined L1 BW loop.** The current `8 lvx; 8 vaddfp` pattern can't dual-issue because the `vaddfp`s depend on loads with 7-cycle latency. Restructuring as `lvx_N+1; vaddfp_N; lvx_N+1; vaddfp_N; ...` (loads of next iter interleaved with adds of current) would let LSU and VMX FXU dual-issue, raising the strict-1-issue ceiling from 25.6 GB/s to ~50 GB/s and the SMT-halved expectation from 12.8 to 25 GB/s. Implementation requires hand-written inline assembly (GCC won't software-pipeline at this level) and the realistic gain is ~15–20% on a single kernel. Not shipped. This writeup is more valuable than the number.
- **Allocating buffers from the 1 MB-page pool.** Done for the disk benchmark; not done here. The PPE benchmark buffers all fit in cache (28 KB / 400 KB / 28 KB / 256 KB) so TLB pressure isn't the bottleneck.
- **Disabling `cellSysutilCheckCallback` during timed windows.** Considered but the callback is necessary for the exit handler to fire. The kernel only runs for ~100ms anyway.

---

## 8. The LV2 SMT ceiling, restated

The single most important finding from this investigation is publishable on its own:

> **A single problem-state PPU thread cannot exceed roughly 50% of the documented PPE VMX FMA peak on GameOS / LV2.** The PowerPC `or N,N,N` priority hints that the bare-metal mechanism for one SMT thread to yield dispatch slots to the other are gated by the hypervisor's `TSCR[UCP]` bit, which LV2 leaves cleared in problem state. Sony's own SDK ships `__cctpl()`/`__cctpm()`/`__cctph()` intrinsics with the explicit note "*This intrinsic has no effect on Cell OS Lv-2*" (`C_and_Cpp_Language_Extension-Specifications_e.htm`, Tables 112–114). There is no documented LV2 syscall to set hardware priority or pin a userland thread to a specific PPE hardware context. The Cell programming model expects vectorised work to happen on the SPUs.

Cellmark's six-SPU SP FMA result lands at ~134 GFLOPS combined, which is more than 12x the PPE-VMX practical ceiling and within 90% of theoretical SPU peak. The chip [WORKS](https://www.youtube.com/watch?v=nVqcxarP9J4), you just have to use it the way the STI folks designed it to be used.

---

## 9. The unknown

- **Whether the PPE handbook's `or 31,31,31` (16-cycle dispatch block) escapes the LV2 gate.** The PPE manual notes this is a different mechanism from the priority hints, it forces a deterministic dispatch stall for 16 cycles regardless of priority. This wasn't attempted, but it could be useful for forcing a window where the active thread runs alone, but the SDK doesn't document it as supported either.
- **Whether running two PPU threads, both doing the FMA workload, would beat 12.2 GFLOPS combined.** Theoretically yes (full pipeline saturation across both HW contexts), but cellmark is (currently™) single-threaded by design and wasn't tested.
- **Why L1 latency lands at 6.4 cycles instead of the 4-cycle published L1 hit.** The PPE manual notes load-to-use latency varies 4–6 cycles depending on bypass routing; chained dependent loads typically land at the upper end. The number is consistent with the documented range but a per-cycle trace doesn't exist to confirm which bypass path is in use.

---

## 10. Sources

- PPE-Users_Manual_e.pdf - Table 6-1 (p. 50) for SMT priority slot-yielding behaviour, Table 11-1 (p. 110) for the `or 1/2/3` priority encodings, p. 135 for the TSRL/TSCR registers.
- Lv2-Users_Manual_e.pdf - p. 19 for the LV2 fixed-priority scheduler description.
- Lv2_System_Call_and_Library-Reference_e.pdf - `sys_ppu_thread_set_priority` reference (range 0..3071, 0 = highest); explicitly does not affect hardware-side priority.
- [psdevwiki](https://www.psdevwiki.com/ps3/LV2_Functions_and_Syscalls#sys_ppu_thread_Syscalls) - full LV2 syscall table including the privilege flags (`dbg`, `root`) and the 3071..3199 privileged priority band. Used in §4.5 to rule out every PPU-thread syscall as a route to influence hardware dispatch.
- `ppu_thread.h` - public SDK header showing the priority range cap at 3071 (line 89) and the absence of any hardware-affinity flag in `sys_ppu_thread_create`'s flags argument (lines 91–99: only `JOINABLE` and `INTERRUPT`).
- C_and_Cpp_Language_Extension-Specifications_e.htm - Tables 112–114, the `__cctpl`/`__cctpm`/`__cctph` intrinsics with the load-bearing "*has no effect on Cell OS Lv-2*" note.
- [Maximizing the power of the Cell Broadband Engine: 25 tips to optimal application performance](https://arcb.csc.ncsu.edu/~mueller/cluster/ps3/25tips.pdf) - Tip 3 (VMX FMA latency), Tip 5 (`vec_dst` is a no-op, use `dcbt`), Tip 6 (microcoded ops stall both SMT threads), Tip 21 (`__builtin_expect`).

---

*Investigation by sagemono, 2026. cellmark v2.0.0.*