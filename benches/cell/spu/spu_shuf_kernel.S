/*
 * spu_shuf_kernel.S  odd pipeline shuffle/permute stress loop
 *
 * shufb is odd pipeline, 4-cycle latency, operates on all 16 bytes.
 * 5 chains to hide the 4-cycle latency with zero stalls. 64x unrolled.
 * Interleaved with nop (even pipe stays idle - this tests odd pipe only).
 *
 * The 4-chain version had a 1-cycle stall: chain 4 read chain 0's output
 * only 3 cycles after it was written (needed 4). The 5th chain inserts
 * exactly 4 cycles between write and re-read, eliminating the stall
 *
 * Additional odd pipe instructions mixed in:
 *   rotqbi (rotate quadword by bits, odd pipe, 4-cycle)
 *   shlqbi (shift left quadword by bits, odd pipe, 4-cycle)
 *
 * OPS accounting: odd-pipe instructions * 4 word-lanes
 *   per .rept: 5 shufb + 2 rotqbi + 2 shlqbi = 9 insns * 4 = 36 ops
 *   total = (iterations/64) * 9 * 64 * 4 = iterations * 36
 *
 * uint32_t spu_stress_shuf_kernel(uint32_t iterations);
 */

        .text
        .global spu_stress_shuf_kernel
        .type   spu_stress_shuf_kernel, @function
        .balign 8

spu_stress_shuf_kernel:
        rotmi   $40, $3, -6             /* loop count = iterations / 64 */

        /* shuffle control patterns - each produces a diff permutation */
        ilhu    $70, 0x0001
        iohl    $70, 0x0203           /* identity-ish low bytes */
        ilhu    $71, 0x0405
        iohl    $71, 0x0607
        ilhu    $72, 0x0809
        iohl    $72, 0x0A0B
        ilhu    $73, 0x0C0D
        iohl    $73, 0x0E0F
        ilhu    $74, 0x0F0E             /* 5th pattern: reverse low 4 bytes */
        iohl    $74, 0x0D0C

        /* build full shuffle mask in $60: byte reversal pattern */
        ilhu    $60, 0x0F0E
        iohl    $60, 0x0D0C
        ilhu    $61, 0x0B0A
        iohl    $61, 0x0908
        /* combine into $60 */
        shufb   $60, $60, $61, $70     /* partial mask build */

        /* rotation amount */
        il      $62, 3                  /* rotate by 3 bits */

        /* init 5 shuffle accumulator chains with diff data */
        ilhu    $2, 0xDEAD
        iohl    $2, 0xBEEF
        ilhu    $3, 0xCAFE
        iohl    $3, 0xBABE
        ilhu    $4, 0x1234
        iohl    $4, 0x5678
        ilhu    $5, 0xFEED
        iohl    $5, 0xFACE
        ilhu    $6, 0xBADB
        iohl    $6, 0xAD01

        /* init 4 rotate/shift chains */
        ilhu    $10, 0xAAAA
        iohl    $10, 0x5555
        ilhu    $11, 0x5555
        iohl    $11, 0xAAAA
        ilhu    $12, 0xFF00
        iohl    $12, 0x00FF
        ilhu    $13, 0x0F0F
        iohl    $13, 0xF0F0

        .balign 128
.Lshuf_loop:

.rept 64
        /* 5x shuffle chains (odd pipe)
         * shufb $6 reads $2 exactly 4 cycles after $2 was written = no stall */
        nop
        shufb   $2, $2, $3, $70
        nop
        shufb   $3, $3, $4, $71
        nop
        shufb   $4, $4, $5, $72
        nop
        shufb   $5, $5, $6, $73
        nop
        shufb   $6, $6, $2, $74

        /* 4x rotate chains (odd pipe) */
        nop
        rotqbi  $10, $10, $62
        nop
        rotqbi  $11, $11, $62

        /* 4x shift chains (odd pipe) */
        nop
        shlqbi  $12, $12, $62
        nop
        shlqbi  $13, $13, $62
.endr

        ai      $40, $40, -1
        brnz    $40, .Lshuf_loop

        /* reduce */
        or      $2, $2, $3
        or      $4, $4, $5
        or      $2, $2, $4
        or      $2, $2, $6
        or      $10, $10, $11
        or      $12, $12, $13
        or      $10, $10, $12
        or      $3, $2, $10

        bi      $0
        .size   spu_stress_shuf_kernel, .-spu_stress_shuf_kernel
