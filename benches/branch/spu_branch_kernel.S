.text
.global spu_stress_branch_hinted_kernel
.type   spu_stress_branch_hinted_kernel, @function
.balign 16

spu_stress_branch_hinted_kernel:
        ori     $40, $3, 0          // counter = iterations

        .balign 16
.Lbh_loop:
        hbrr    .Lbh_branch, .Lbh_loop
        lnop
        lnop
        lnop
        lnop
        lnop
        ai      $40, $40, -1
        lnop
        lnop
        lnop
        lnop
        lnop
.Lbh_branch:
        brnz    $40, .Lbh_loop

        ori     $3, $40, 0
        bi      $0
        .size   spu_stress_branch_hinted_kernel, .-spu_stress_branch_hinted_kernel


        .global spu_stress_branch_unhinted_kernel
        .type   spu_stress_branch_unhinted_kernel, @function
        .balign 16

spu_stress_branch_unhinted_kernel:
        ori     $40, $3, 0

        .balign 16
.Lbu_loop:
        // same body as hinted version but NO hbrr, every taken backward branch is a mispredict against the SPU's default predict not taken
        lnop
        lnop
        lnop
        lnop
        lnop
        ai      $40, $40, -1
        lnop
        lnop
        lnop
        lnop
        lnop
.Lbu_branch:
        brnz    $40, .Lbu_loop

        ori     $3, $40, 0
        bi      $0
        .size   spu_stress_branch_unhinted_kernel, .-spu_stress_branch_unhinted_kernel