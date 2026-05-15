/*
 * spu_int_kernel.S integer multiply add stress loop
 *
 * Uses mpya (multiply and add, 16-bit * 16-bit + 32-bit, 4-way SIMD).
 * 7 chains to hide the 7 cycle latency. 32x unrolled = 224 mpya per iter.
 *
 * OPS accounting:
 *   each mpya = 4 lanes * 2 ops (mul+add) = 8 OPS
 *   per inner loop: 224 * 8 = 1792 OPS
 *   total = (iterations/32) * 1792 = iterations * 56
 *
 * uint32_t spu_stress_int_kernel(uint32_t iterations);
 */

        .text
        .global spu_stress_int_kernel
        .type   spu_stress_int_kernel, @function
        .balign 8

spu_stress_int_kernel:
        rotmi   $9, $3, -5              /* loop count = iterations / 32 */

        /* mul constant: small value to keep results bounded
         * use 3 so products stay in 16-bit range longer */
        il      $70, 3

        /* add constant */
        il      $71, 7

        /* init 7 accumulator chains with distinct values */
        il      $2, 100
        il      $3, 200
        il      $4, 300
        il      $5, 400
        il      $6, 500
        il      $7, 600
        il      $8, 700

        .balign 128
.Lint_loop:

.rept 32
        mpya    $2, $2, $70, $71
        lnop
        mpya    $3, $3, $70, $71
        lnop
        mpya    $4, $4, $70, $71
        lnop
        mpya    $5, $5, $70, $71
        lnop
        mpya    $6, $6, $70, $71
        lnop
        mpya    $7, $7, $70, $71
        lnop
        mpya    $8, $8, $70, $71
        lnop
.endr

        ai      $9, $9, -1
        brnz    $9, .Lint_loop

        /* reduce */
        a       $2, $2, $3
        a       $4, $4, $5
        a       $6, $6, $7
        a       $2, $2, $4
        a       $3, $2, $6
        a       $3, $3, $8

        bi      $0
        .size   spu_stress_int_kernel, .-spu_stress_int_kernel
