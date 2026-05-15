/*
 * spu_dp_fma_kernel.S double precision FMA stress loop
 *
 * 13 chains to hide the 13 cycle DP FMA latency (6-cycle stall + 7-cycle).
 * 16x unrolled = 208 DFMA per iteration.
 * 2-way SIMD: each DFMA operates on 2 x float64.
 *
 * flops accounting:
 *   each DFMA = 2 lanes * 2 ops (mul+add) = 4 FLOPS
 *   per inner loop: 208 * 4 = 832 FLOPS
 *   total = (iterations/16) * 832 = iterations * 52
 *
 * uint32_t spu_stress_dp_fma_kernel(uint32_t iterations);
 */

        .text
        .global spu_stress_dp_fma_kernel
        .type   spu_stress_dp_fma_kernel, @function
        .balign 8

spu_stress_dp_fma_kernel:
        rotmi   $40, $3, -4             /* loop count = iterations / 16 */

        /* init 13 accumulator registers with distinct starting values.
         * use ilhu to set upper halfword of preferred slot.
         * for DP, we want slot 0 and slot 1 (two doubles per register).
         * init with small integer values via convert path. */
        il      $2, 1
        il      $3, 2
        il      $4, 3
        il      $5, 4
        il      $6, 5
        il      $7, 6
        il      $8, 7
        il      $9, 8
        il      $10, 9
        il      $11, 10
        il      $12, 11
        il      $13, 12
        il      $14, 13

        /* convert int -> float -> double for proper DP init */
        csflt   $2, $2, 0
        csflt   $3, $3, 0
        csflt   $4, $4, 0
        csflt   $5, $5, 0
        csflt   $6, $6, 0
        csflt   $7, $7, 0
        csflt   $8, $8, 0
        csflt   $9, $9, 0
        csflt   $10, $10, 0
        csflt   $11, $11, 0
        csflt   $12, $12, 0
        csflt   $13, $13, 0
        csflt   $14, $14, 0

        fesd    $2, $2
        fesd    $3, $3
        fesd    $4, $4
        fesd    $5, $5
        fesd    $6, $6
        fesd    $7, $7
        fesd    $8, $8
        fesd    $9, $9
        fesd    $10, $10
        fesd    $11, $11
        fesd    $12, $12
        fesd    $13, $13
        fesd    $14, $14

        /* mul constant: 1.0 in double via int->float->double */
        il      $70, 1
        csflt   $70, $70, 0
        fesd    $70, $70                /* $70 = 1.0 double */

        /* add constant: small double (2^-10 ~= 0.001) via scale */
        il      $71, 1
        csflt   $71, $71, 10            /* 1.0 * 2^-10 */
        fesd    $71, $71                /* $71 = ~0.001 double */

        .balign 128
.Ldp_loop:

.rept 16
        dfma    $2, $70, $71
        lnop
        dfma    $3, $70, $71
        lnop
        dfma    $4, $70, $71
        lnop
        dfma    $5, $70, $71
        lnop
        dfma    $6, $70, $71
        lnop
        dfma    $7, $70, $71
        lnop
        dfma    $8, $70, $71
        lnop
        dfma    $9, $70, $71
        lnop
        dfma    $10, $70, $71
        lnop
        dfma    $11, $70, $71
        lnop
        dfma    $12, $70, $71
        lnop
        dfma    $13, $70, $71
        lnop
        dfma    $14, $70, $71
        lnop
.endr

        ai      $40, $40, -1
        brnz    $40, .Ldp_loop

        /* reduce to single result so gcc destroy this */
        dfa     $2, $2, $3
        dfa     $4, $4, $5
        dfa     $6, $6, $7
        dfa     $8, $8, $9
        dfa     $10, $10, $11
        dfa     $12, $12, $13
        dfa     $2, $2, $4
        dfa     $6, $6, $8
        dfa     $10, $10, $12
        dfa     $2, $2, $6
        dfa     $3, $2, $10
        dfa     $3, $3, $14

        bi      $0
        .size   spu_stress_dp_fma_kernel, .-spu_stress_dp_fma_kernel
