/*
 * spu_fma_kernel.S high thruput SPU FMA stress loop
 *
 * 64x unrolled, 6 chains = 384 FMA per iteration.
 * 3080 bytes loop body = 24 LS fetch lines, prefetched inline.
 * branch back penalty: 15 cycles / ~385 total = 3.9% overhead.
 * expected: ~148 GFLOPS at stock 3.2 GHz.
 *
 * uint32_t spu_stress_fma_kernel(uint32_t iterations);
 */

        .text
        .global spu_stress_fma_kernel
        .type   spu_stress_fma_kernel, @function
        .balign 8

spu_stress_fma_kernel:
        rotmi   $10, $3, -6            /* loop count = iterations / 64 */

        ilhu    $8, 0x3f7f
        iohl    $8, 0xffff             /* mul ~= 1.0 */
        ilhu    $9, 0x3400             /* add = 2^-23 */

        ilhu    $2, 0x3f80             /* acc0 = 1.0 */
        ilhu    $3, 0x3f8c             /* acc1 ~= 1.1 */
        ilhu    $4, 0x3f99             /* acc2 ~= 1.2 */
        ilhu    $5, 0x3fa6             /* acc3 ~= 1.3 */
        ilhu    $6, 0x3fb3             /* acc4 ~= 1.4 */
        ilhu    $7, 0x3fc0             /* acc5 ~= 1.5 */

        /* no hbrr. displacement too large for 64x unroll.
         * the 15 cycle branch back penalty is already budgeted
         * at 3.9% overhead. the ILB cannot cache a 3 KB loop
         * anyway, so the hint would not help... */

        .balign 128
.Lloop:

.rept 64
        fma     $2, $2, $8, $9
        lnop
        fma     $3, $3, $8, $9
        lnop
        fma     $4, $4, $8, $9
        lnop
        fma     $5, $5, $8, $9
        lnop
        fma     $6, $6, $8, $9
        lnop
        fma     $7, $7, $8, $9
        lnop
.endr

        ai      $10, $10, -1
        brnz    $10, .Lloop

        fa      $2, $2, $3
        fa      $4, $4, $5
        fa      $6, $6, $7
        fa      $2, $2, $4
        fa      $3, $2, $6

        bi      $0
        .size   spu_stress_fma_kernel, .-spu_stress_fma_kernel