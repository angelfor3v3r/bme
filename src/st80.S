.text

.globl st80_to_double
.type st80_to_double, @function
st80_to_double:
    subq $24, %rsp
    fnstcw (%rsp)
    movw %si, 2(%rsp)
    fldcw 2(%rsp)
    fldt (%rdi)
    fstpl 8(%rsp)
    fldcw (%rsp)
    movsd 8(%rsp), %xmm0
    addq $24, %rsp
    ret
.size st80_to_double, .-st80_to_double

.globl double_to_st80
.type double_to_st80, @function
double_to_st80:
    movsd %xmm0, -8(%rsp)
    fldl -8(%rsp)
    fstpt (%rdi)
    ret
.size double_to_st80, .-double_to_st80

.globl st80_to_float
.type st80_to_float, @function
st80_to_float:
    subq $24, %rsp
    fnstcw (%rsp)
    movw %si, 2(%rsp)
    fldcw 2(%rsp)
    fldt (%rdi)
    fstps 8(%rsp)
    fldcw (%rsp)
    movss 8(%rsp), %xmm0
    addq $24, %rsp
    ret
.size st80_to_float, .-st80_to_float


.section .note.GNU-stack,"",@progbits