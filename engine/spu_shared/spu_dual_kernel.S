/*
 * spu_dual_kernel.S maximum dual issue stress loop
 *
 * ever cycle: even pipe FMA + odd pipe shufb/rotqbi.
 * this is the theoretical maximum power draw for an SPU
 * as both pipelines r active on every single cycle.
 *
 * 6 FMA chains (even, 6 cycle latency) + 4 shufb chains (odd, 4 cycle)
 * interleaved so every instruction pair is even+odd = guaranteed dual issue.
 *
 * 64x unrolled. Each unroll iteration:
 *   6 FMA (even) + 6 odd pipe ops = 12 instructions, 6 cycles
 *
 * OPS accounting:
 *   FMA: 6 chains * 64 unrolls * 4 lanes * 2 ops = 3072 FLOPS
 *   ODD: 6 ops * 64 unrolls * 16 bytes = 6144 byte ops
 *   total = (iterations/64) * (3072 + 6*64*4) = iterations * 72
 *   (we report combined ops/sec)
 *
 * uint32_t spu_stress_dual_kernel(uint32_t iterations);
 */

        .text
        .global spu_stress_dual_kernel
        .type   spu_stress_dual_kernel, @function
        .balign 8

spu_stress_dual_kernel:
        rotmi   $40, $3, -6             /* loop count = iterations / 64 */

        /* FMA constants (even pipe) */
        ilhu    $70, 0x3f7f
        iohl    $70, 0xffff             /* mul ~= 1.0 */
        ilhu    $71, 0x3400             /* add = 2^-23 */

        /* shuffle control patterns (odd pipe) */
        ilhu    $72, 0x0001
        iohl    $72, 0x0203
        ilhu    $73, 0x0405
        iohl    $73, 0x0607
        il      $74, 5                  /* rotate amount */

        /* 6 FMA accumulators: $2-$7 */
        ilhu    $2, 0x3f80              /* 1.0 */
        ilhu    $3, 0x3f8c              /* ~1.1 */
        ilhu    $4, 0x3f99              /* ~1.2 */
        ilhu    $5, 0x3fa6              /* ~1.3 */
        ilhu    $6, 0x3fb3              /* ~1.4 */
        ilhu    $7, 0x3fc0              /* ~1.5 */

        /* 6 odd pipe accumulators: $10-$15 */
        ilhu    $10, 0xDEAD
        iohl    $10, 0xBEEF
        ilhu    $11, 0xCAFE
        iohl    $11, 0xBABE
        ilhu    $12, 0x1234
        iohl    $12, 0x5678
        ilhu    $13, 0xFEED
        iohl    $13, 0xFACE
        ilhu    $14, 0xAAAA
        iohl    $14, 0x5555
        ilhu    $15, 0x5555
        iohl    $15, 0xAAAA

        .balign 128
.Ldual_loop:

.rept 64
        /* cycle 1: FMA chain 0 (even) + shufb chain 0 (odd) */
        fma     $2, $2, $70, $71
        shufb   $10, $10, $11, $72

        /* cycle 2: FMA chain 1 + shufb chain 1 */
        fma     $3, $3, $70, $71
        shufb   $11, $11, $12, $73

        /* cycle 3: FMA chain 2 + rotqbi chain 0 */
        fma     $4, $4, $70, $71
        rotqbi  $12, $12, $74

        /* cycle 4: FMA chain 3 + rotqbi chain 1 */
        fma     $5, $5, $70, $71
        rotqbi  $13, $13, $74

        /* cycle 5: FMA chain 4 + shufb chain 2 */
        fma     $6, $6, $70, $71
        shufb   $14, $14, $15, $72

        /* cycle 6: FMA chain 5 + shlqbi chain 0 */
        fma     $7, $7, $70, $71
        shlqbi  $15, $15, $74
.endr

        ai      $40, $40, -1
        brnz    $40, .Ldual_loop

        /* reduce even pipe results */
        fa      $2, $2, $3
        fa      $4, $4, $5
        fa      $6, $6, $7
        fa      $2, $2, $4
        fa      $3, $2, $6

        /* reduce odd pipe results (prevents gcc dead code fuckery) */
        or      $10, $10, $11
        or      $12, $12, $13
        or      $14, $14, $15
        or      $10, $10, $12
        or      $10, $10, $14

        /* combine both into return value */
        or      $3, $3, $10

        bi      $0
        .size   spu_stress_dual_kernel, .-spu_stress_dual_kernel
