/*
 * spu_recip_kernel.S reciprocal + rsqrt chain stress loop
 * Probably the worst one to write
 * 6 independent reciprocal chains: frest -> fi -> fnms -> fma
 * 6 independent rsqrt chains: frsqest -> and -> fi -> fm -> fnms -> fm -> fma
 *
 * 6 chains perfectly hides all inter stage latencies with zero stalls:
 *   frest(4cyc):   6 insns gap before fi use     = 2 spare cycles
 *   fi(6cyc):      exactly 6 insns before fnms   = zero stall
 *   fnms(6cyc):    exactly 6 insns before fma    = zero stall
 *   frsqest(4cyc): 12 insns before fi use        = plenty of slack
 *   and(2cyc):     provides extra slack for fi
 *   fi(6cyc):      exactly 6 insns before fm     = zero stall
 *   fm(6cyc):      exactly 6 insns before fnms   = zero stall
 *   fnms(6cyc):    exactly 6 insns before fm     = zero stall
 *   fm(6cyc):      exactly 6 insns before fma    = zero stall
 *
 * OPS accounting: all even pipe instructions * 4 SIMD lanes
 *   Recip: 4 insns/chain * 6 chains * 4 lanes = 96 ops/block
 *   Rsqrt: 7 insns/chain * 6 chains * 4 lanes = 168 ops/block
 *   Total per .rept block: 264 ops
 *   Total = (iterations/8) * 264 * 8 = iterations * 264
 *
 * uint32_t spu_stress_recip_kernel(uint32_t iterations);
 */

        .text
        .global spu_stress_recip_kernel
        .type   spu_stress_recip_kernel, @function
        .balign 8

spu_stress_recip_kernel:
        rotmi   $50, $3, -3             /* loop count = iterations / 8 */

        /* constants */
        ilhu    $70, 0x3f80             /* 1.0f */
        ilhu    $71, 0x3f00             /* 0.5f */
        ilhu    $72, 0x7fff
        iohl    $72, 0xffff             /* 0x7fffffff = abs mask */

        /* init 6 reciprocal input chains with distinct positive values */
        ilhu    $2, 0x4000              /* 2.0f */
        ilhu    $3, 0x4040              /* 3.0f */
        ilhu    $4, 0x4080              /* 4.0f */
        ilhu    $5, 0x40a0              /* 5.0f */
        ilhu    $6, 0x40c0              /* 6.0f */
        ilhu    $7, 0x40e0              /* 7.0f */

        /* init 6 rsqrt input chains with distinct positive values */
        ilhu    $8,  0x4100             /* 8.0f */
        ilhu    $9,  0x4110             /* 9.0f-ish */
        ilhu    $10, 0x4120             /* 10.0f */
        ilhu    $11, 0x4130             /* 11.0f */
        ilhu    $12, 0x4140             /* 12.0f */
        ilhu    $13, 0x4150             /* 13.0f */

        .balign 128
.Lrecip_loop:

.rept 8
        /* !!!!! 6x reciprocal chains (frest -> fi -> fnms -> fma) !!!!!
         * $2-$7:   inputs/outputs (reciprocal of reciprocal oscillates)
         * $20-$25: frest estimates
         * $26-$31: fnms Newton Raphson temps because thank sony for this retarded ISA with no div instruction
         */

        /* step 1: estimate 1/x (frest, 4-cycle latency) */
        frest   $20, $2
        lnop
        frest   $21, $3
        lnop
        frest   $22, $4
        lnop
        frest   $23, $5
        lnop
        frest   $24, $6
        lnop
        frest   $25, $7
        lnop

        /* step 2: interpolate (fi, 6-cycle latency)
         * frest results ready: issued 6 cycles ago = no stall */
        fi      $20, $2,  $20
        lnop
        fi      $21, $3,  $21
        lnop
        fi      $22, $4,  $22
        lnop
        fi      $23, $5,  $23
        lnop
        fi      $24, $6,  $24
        lnop
        fi      $25, $7,  $25
        lnop

        /* step 3: NR: t = -(x*y - 1.0) (fnms, 6-cycle latency)
         * fi results exactly available - no stall */
        fnms    $26, $2,  $20, $70
        lnop
        fnms    $27, $3,  $21, $70
        lnop
        fnms    $28, $4,  $22, $70
        lnop
        fnms    $29, $5,  $23, $70
        lnop
        fnms    $30, $6,  $24, $70
        lnop
        fnms    $31, $7,  $25, $70
        lnop

        /* step 4: refine: y = t*y + y (fma, 6-cycle latency)
         * fnms results exactly available - no stall */
        fma     $2,  $26, $20, $20
        lnop
        fma     $3,  $27, $21, $21
        lnop
        fma     $4,  $28, $22, $22
        lnop
        fma     $5,  $29, $23, $23
        lnop
        fma     $6,  $30, $24, $24
        lnop
        fma     $7,  $31, $25, $25
        lnop

        /* ---- 6x rsqrt chains (frsqest -> and -> fi -> fm -> fnms -> fm -> fma) ----
         * $8-$13:  inputs/outputs
         * $32-$37: frsqest estimates
         * $38-$43: abs(x) values, reused as fm intermediates
         * $44-$49: fnms and fm correction temps
         */

        /* step 1: rsqrt estimate (frsqest, 4-cycle latency) */
        frsqest $32, $8
        lnop
        frsqest $33, $9
        lnop
        frsqest $34, $10
        lnop
        frsqest $35, $11
        lnop
        frsqest $36, $12
        lnop
        frsqest $37, $13
        lnop

        /* step 1.5: abs(x) for fi (and, 2-cycle latency)
         * independent of frsqest - provides 12-cycle gap before fi uses frsqest */
        /* and is even pipe; pair with lnop (odd) to dual issue */
        and     $38, $8,  $72
        lnop
        and     $39, $9,  $72
        lnop
        and     $40, $10, $72
        lnop
        and     $41, $11, $72
        lnop
        and     $42, $12, $72
        lnop
        and     $43, $13, $72
        lnop

        /* step 2: interpolate (fi, 6-cycle latency)
         * frsqest was 12 insns ago (plenty), and $38 was 6 insns ago (exact) */
        fi      $32, $38, $32
        lnop
        fi      $33, $39, $33
        lnop
        fi      $34, $40, $34
        lnop
        fi      $35, $41, $35
        lnop
        fi      $36, $42, $36
        lnop
        fi      $37, $43, $37
        lnop

        /* step 3: abs(x) * estimate (fm, 6-cycle latency)
         * fi results exactly available - no stall */
        fm      $38, $38, $32
        lnop
        fm      $39, $39, $33
        lnop
        fm      $40, $40, $34
        lnop
        fm      $41, $41, $35
        lnop
        fm      $42, $42, $36
        lnop
        fm      $43, $43, $37
        lnop

        /* step 4: NR error: -(t1*y - 1.0) (fnms, 6-cycle latency)
         * fm results exactly available - no stall */
        fnms    $44, $38, $32, $70
        lnop
        fnms    $45, $39, $33, $70
        lnop
        fnms    $46, $40, $34, $70
        lnop
        fnms    $47, $41, $35, $70
        lnop
        fnms    $48, $42, $36, $70
        lnop
        fnms    $49, $43, $37, $70
        lnop

        /* step 5: half correction (fm, 6-cycle latency)
         * fnms results exactly available - no stall */
        fm      $44, $44, $71
        lnop
        fm      $45, $45, $71
        lnop
        fm      $46, $46, $71
        lnop
        fm      $47, $47, $71
        lnop
        fm      $48, $48, $71
        lnop
        fm      $49, $49, $71
        lnop

        /* step 6: refine: y2 = (t/2)*y + y (fma, 6-cycle latency)
         * fm results exactly available - no stall */
        fma     $8,  $44, $32, $32
        lnop
        fma     $9,  $45, $33, $33
        lnop
        fma     $10, $46, $34, $34
        lnop
        fma     $11, $47, $35, $35
        lnop
        fma     $12, $48, $36, $36
        lnop
        fma     $13, $49, $37, $37
        lnop
.endr

        ai      $50, $50, -1
        brnz    $50, .Lrecip_loop

        /* reduce recip results */
        fa      $2, $2, $3
        fa      $4, $4, $5
        fa      $6, $6, $7
        fa      $2, $2, $4
        fa      $2, $2, $6

        /* reduce rsqrt results */
        fa      $8,  $8,  $9
        fa      $10, $10, $11
        fa      $12, $12, $13
        fa      $8,  $8,  $10
        fa      $8,  $8,  $12

        /* combine into return value */
        fa      $3, $2, $8

        bi      $0
        .size   spu_stress_recip_kernel, .-spu_stress_recip_kernel
